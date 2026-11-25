// SPDX-License-Identifier: ISC
// Startup for an kaiHEN plugin (include/kaihen/plugin.h).
//
// The plugin registers the temporary service mig:usr. The bootstrap connects
// to it and sends AdoptHandles with copies of its SM session and FS root. The
// reply has the runtime version (PluginRuntimeVersion) and three handles: a
// release event, a ready event and the client end of the plugin's control
// session, or INVALID_HANDLE for a plugin without a service. Once the
// bootstrap has the reply it signals the release event, and the plugin
// unregisters mig:usr. Then the plugin installs the SM session and FS root as
// its services.
#include <switch.h>

#include "../include/kaihen/plugin.h"
#include "plugin_startup.h"
#include "cmif_server.hpp"

extern "C" {

// A plugin that initializes FS starts with one session. Its own strong
// definition can select a larger pool when it needs concurrent FS calls.
__attribute__((weak)) u32 __nx_fs_num_sessions = 1;

}

namespace {

using ounce::closeHandle;

constexpr u32 MigrationServiceSpace = 1;
constexpr u64 ReleaseTimeoutNs = 5000000000ull;

/// The handles that the plugin keeps from the bootstrap's AdoptHandles.
struct ReceivedHandles {
    Handle sm_session = INVALID_HANDLE;
    Handle fs_root = INVALID_HANDLE;
    Handle ready_event = INVALID_HANDLE;     ///< Writable end; the plugin signals it.
    Handle control_session = INVALID_HANDLE; ///< Server end, when the plugin publishes a service.
    Handle supervisor_control_client = INVALID_HANDLE; // Retained local client copy for next HEN generation.
    KaihenSupervisorEscrow escrow = {INVALID_HANDLE, INVALID_HANDLE,
                                     INVALID_HANDLE, INVALID_HANDLE,
                                     INVALID_HANDLE, INVALID_HANDLE,
                                     INVALID_HANDLE};
    KaihenSupervisorLaunchGeometry launch_geometry = {};
    KaihenSupervisorLaunchHandles launch_handles = {};
    bool geometry_received = false;
    bool gate_handles0_received = false;
    bool gate_handles_received = false;
};

/// The bootstrap's ends of the objects that the AdoptHandles reply creates.
/// The reply copies them, so the plugin closes these after sending it.
struct AdoptReply {
    Handle release_event = INVALID_HANDLE;  ///< Writable end.
    Handle ready_event = INVALID_HANDLE;    ///< Readable end.
    Handle control_client = INVALID_HANDLE; ///< When the plugin publishes a service.

    void close() {
        closeHandle(&release_event);
        closeHandle(&ready_event);
        closeHandle(&control_client);
    }
};

ReceivedHandles g_received;
bool g_publish_service = false;
bool g_supervisor_mode = false;
bool g_initialized = false;

bool matchesWirePayloadSize(const ounce::CmifRequest& request, size_t expected) {
    if (!request.valid)
        return false;

    const auto* words = reinterpret_cast<const u8*>(request.hipc.data.data_words);
    const auto* aligned = static_cast<const u8*>(
        cmifGetAlignedDataStart(request.hipc.data.data_words, armGetTls()));

    const size_t leading_padding = static_cast<size_t>(aligned - words);
    if (leading_padding > 15)
        return false;

    // libounce reserves 16 bytes for CMIF alignment. Its unused
    // portion is included in HIPC num_data_words.
    const size_t trailing_padding = 16 - leading_padding;
    return request.data_size == expected + trailing_padding;
}

void closeReceivedHandles() {
    closeHandle(&g_received.supervisor_control_client);
    if (g_received.gate_handles_received) {
        closeHandle(&g_received.launch_handles.ro_session);
        closeHandle(&g_received.launch_handles.sm_master);
        closeHandle(&g_received.launch_handles.event_write);
        closeHandle(&g_received.launch_handles.event_read);
        closeHandle(&g_received.launch_handles.server_copy);
    }
    if (g_received.gate_handles0_received) {
        for (Handle& worker : g_received.launch_handles.workers) closeHandle(&worker);
        closeHandle(&g_received.launch_handles.storage);
    }
    g_received.gate_handles0_received = false;
    g_received.gate_handles_received = false;
    g_received.geometry_received = false;
    closeHandle(&g_received.escrow.hen_control);
    closeHandle(&g_received.escrow.fs_notifier);
    closeHandle(&g_received.escrow.ro_reader);
    closeHandle(&g_received.escrow.fs_process);
    closeHandle(&g_received.escrow.loader_root);
    closeHandle(&g_received.escrow.monitor_session);
    closeHandle(&g_received.escrow.shell_session);
    closeHandle(&g_received.control_session);
    closeHandle(&g_received.ready_event);
    closeHandle(&g_received.fs_root);
    closeHandle(&g_received.sm_session);
}

bool isAdoptHandles(const ounce::CmifRequest& request) {
    const HipcMetadata& meta = request.hipc.meta;
    return request.valid && meta.type == CmifCommandType_Request &&
        !meta.num_move_handles &&
        ((request.command_id == PluginStartupCommand_AdoptHandles &&
          meta.num_copy_handles == 2 && !g_supervisor_mode) ||
         (request.command_id == PluginStartupCommand_AdoptSupervisorHandles &&
          meta.num_copy_handles == 8 && g_supervisor_mode));
}

/// Creates the events and the control session that the AdoptHandles reply
/// returns. The plugin keeps one end of each, and `release_event` receives
/// the readable end of the release event.
Result createAdoptObjects(AdoptReply* reply, Handle* release_event) {
    Result rc = svcCreateEvent(&reply->release_event, release_event);
    if (R_FAILED(rc))
        return rc;

    rc = svcCreateEvent(&g_received.ready_event, &reply->ready_event);
    if (R_FAILED(rc))
        goto fail_release_event;

    if (g_publish_service) {
        rc = svcCreateSession(&g_received.control_session, &reply->control_client, false, 0);
        if (R_FAILED(rc))
            goto fail_ready_event;
    }
    return 0;

fail_ready_event:
    closeHandle(&g_received.ready_event);
    closeHandle(&reply->ready_event);
fail_release_event:
    closeHandle(release_event);
    closeHandle(&reply->release_event);
    return rc;
}

/// Adopts the SM session and FS root of an AdoptHandles request and writes
/// the reply. On success, `reply` holds the bootstrap's ends to close after
/// sending it.
Result adoptHandles(void* message, const ounce::CmifRequest& request,
                    AdoptReply* reply, Handle* release_event) {
    Result rc = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    if (g_received.sm_session == INVALID_HANDLE)
        rc = createAdoptObjects(reply, release_event);

    if (R_FAILED(rc)) {
        ounce::closeRequestHandles(request);
        ounce::writeReply(message, rc, nullptr, 0);
        return rc;
    }

    g_received.sm_session = request.hipc.data.copy_handles[0];
    g_received.fs_root = request.hipc.data.copy_handles[1];
    if (g_supervisor_mode) {
        const Handle* h = request.hipc.data.copy_handles;
        g_received.escrow = {h[2], h[3], h[4], h[5], h[6], h[7], INVALID_HANDLE};
    }

    // The kernel passes an INVALID_HANDLE in the copy list through unchanged,
    // so a plugin without a service sends one in the third place.
    const u32 runtime_version = PluginRuntimeVersion;
    const Handle handles[] = {reply->release_event, reply->ready_event, reply->control_client};
    ounce::writeReply(message, 0, &runtime_version, sizeof(runtime_version), handles, 3);
    return 0;
}

/// Serves one request on the mig:usr session. `adopted` is set when it was
/// AdoptHandles and the plugin now has the bootstrap's handles.
Result serveBootstrapRequest(Handle session, Handle* release_event,
                             bool* adopted, bool* completed) {
    void* message = armGetTls();
    const ounce::CmifRequest request = ounce::parseRequest(message);
    AdoptReply reply;
    Result rc;
    *adopted = false;
    *completed = false;

    if (ounce::isQueryPointerBufferSize(request)) {
        ounce::writePointerBufferSizeReply(message);
        rc = 0;
    } else if (isAdoptHandles(request) &&
               g_received.sm_session == INVALID_HANDLE) {
        rc = adoptHandles(message, request, &reply, release_event);
        *adopted = R_SUCCEEDED(rc);
        if (*adopted && !g_supervisor_mode) *completed = true;
    } else if (g_supervisor_mode &&
        request.valid && request.hipc.meta.type == CmifCommandType_Request &&
        !request.hipc.meta.num_move_handles &&
        !request.hipc.meta.num_send_buffers &&
        !request.hipc.meta.num_recv_buffers &&
        !request.hipc.meta.num_exch_buffers &&
        request.command_id == PluginStartupCommand_AdoptSupervisorGeometry &&
        request.hipc.meta.num_copy_handles == 0 &&
        g_received.sm_session != INVALID_HANDLE &&
        !g_received.geometry_received &&
        matchesWirePayloadSize(request, sizeof(KaihenSupervisorLaunchGeometry))) {
        KaihenSupervisorLaunchGeometry geometry = {};
        __builtin_memcpy(&geometry, request.data, sizeof(geometry));
        rc = geometry.version == KaihenSupervisorLaunchGeometryVersion ? 0
             : MAKERESULT(Module_Libnx, LibnxError_BadInput);
        if (R_SUCCEEDED(rc)) {
            g_received.launch_geometry = geometry;
            g_received.geometry_received = true;
        }
        ounce::writeReply(message, rc, nullptr, 0);
    } else if (g_supervisor_mode &&
        request.valid && request.hipc.meta.type == CmifCommandType_Request &&
        !request.hipc.meta.num_move_handles &&
        !request.hipc.meta.num_send_buffers &&
        !request.hipc.meta.num_recv_buffers &&
        !request.hipc.meta.num_exch_buffers &&
        request.command_id == PluginStartupCommand_AdoptSupervisorGateHandles0 &&
        request.hipc.meta.num_copy_handles == 8 &&
        matchesWirePayloadSize(request, 0) &&
        g_received.sm_session != INVALID_HANDLE &&
        g_received.geometry_received &&
        !g_received.gate_handles0_received) {
        const Handle* h = request.hipc.data.copy_handles;
        g_received.launch_handles.storage = h[0];
        for (unsigned i = 0; i < 7; ++i)
            g_received.launch_handles.workers[i] = h[i + 1];
        g_received.gate_handles0_received = true;
        rc = 0;
        ounce::writeReply(message, 0, nullptr, 0);
    } else if (g_supervisor_mode &&
        request.valid && request.hipc.meta.type == CmifCommandType_Request &&
        !request.hipc.meta.num_move_handles &&
        !request.hipc.meta.num_send_buffers &&
        !request.hipc.meta.num_recv_buffers &&
        !request.hipc.meta.num_exch_buffers &&
        request.command_id == PluginStartupCommand_AdoptSupervisorGateHandles1 &&
        request.hipc.meta.num_copy_handles == 6 &&
        matchesWirePayloadSize(request, 0) &&
        g_received.gate_handles0_received &&
        !g_received.gate_handles_received) {
        const Handle* h = request.hipc.data.copy_handles;
        g_received.launch_handles.server_copy = h[0];
        g_received.launch_handles.event_read = h[1];
        g_received.launch_handles.event_write = h[2];
        g_received.launch_handles.sm_master = h[3];
        g_received.launch_handles.ro_session = h[4];
        g_received.escrow.hen_control = h[5];
        g_received.gate_handles_received = true;
        *completed = true;
        rc = 0;
        ounce::writeReply(message, 0, nullptr, 0);
    } else {
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        ounce::closeRequestHandles(request);
        ounce::writeReply(message, rc, nullptr, 0);
    }

    const Result send_rc = ounce::sendReply(session);
    if (R_SUCCEEDED(send_rc) && g_supervisor_mode && *adopted &&
        reply.control_client != INVALID_HANDLE) {
        // HIPC sent a COPY. Keep the original client handle open even if the
        // initial HEN exits, so the next HEN can adopt it without recreating
        // the plugin server or losing live homebrew sessions.
        g_received.supervisor_control_client = reply.control_client;
        reply.control_client = INVALID_HANDLE;
    }
    reply.close();
    return R_FAILED(send_rc) ? send_rc : rc;
}

/// Accepts the bootstrap's connection to mig:usr and serves it until the
/// bootstrap has sent AdoptHandles. The session stays open, so that the
/// bootstrap can receive the reply; the caller closes it.
Result receiveBootstrapHandles(Handle port, Handle* out_session, Handle* release_event) {
    Handle session = INVALID_HANDLE;
    bool primary_received = false;
    for (;;) {
        const Handle handles[] = {port, session};
        const s32 count = session == INVALID_HANDLE ? 1 : 2;
        s32 index = -1;
        const u64 timeout = primary_received && g_supervisor_mode
            ? ReleaseTimeoutNs : UINT64_MAX;
        Result rc = svcReplyAndReceive(&index, handles, count, INVALID_HANDLE, timeout);
        if (R_FAILED(rc))
            return rc;

        if (index == 0) {
            rc = svcAcceptSession(&session, port);
            if (R_FAILED(rc))
                return rc;
            continue;
        }

        bool adopted = false;
        bool completed = false;
        rc = serveBootstrapRequest(session, release_event, &adopted, &completed);
        if (adopted) primary_received = true;
        if (R_FAILED(rc) || completed) {
            *out_session = session;
            return rc;
        }
    }
}

/// Registers mig:usr, receives the bootstrap's handles, and unregisters
/// mig:usr once the bootstrap signals that it has the reply.
Result receiveThroughMigrationService() {
    Handle port = INVALID_HANDLE;
    Result rc = smRegisterServiceInSpace(&port, MigrationServiceSpace,
                                         smEncodeName("mig:usr"), false, 1);
    if (R_FAILED(rc))
        return rc;

    Handle session = INVALID_HANDLE;
    Handle release_event = INVALID_HANDLE;
    rc = receiveBootstrapHandles(port, &session, &release_event);
    if (R_SUCCEEDED(rc))
        rc = svcWaitSynchronizationSingle(release_event, ReleaseTimeoutNs);
    closeHandle(&session);
    closeHandle(&release_event);

    if (R_SUCCEEDED(rc))
        rc = smUnregisterServiceInSpace(MigrationServiceSpace, smEncodeName("mig:usr"));
    svcCloseHandle(port);
    return rc;
}

/// Installs the bootstrap's SM session and FS root for later service use.
Result installReceivedServices() {
    smAddOverrideHandle(smEncodeName("sm:"), g_received.sm_session);
    smAddOverrideHandle(smEncodeName("fsp-srv"), g_received.fs_root);
    return smInitialize();
}

} // namespace

Result pluginRuntimeInitialize(Handle* out_ready_event) {
    Result rc = smInitialize();
    if (R_FAILED(rc))
        return rc;

    rc = receiveThroughMigrationService();
    // mig:usr was registered with the plugin's own SM session. Close it
    // before the bootstrap's session replaces it.
    smExit();

    if (R_SUCCEEDED(rc))
        rc = installReceivedServices();
    if (R_FAILED(rc)) {
        closeReceivedHandles();
        return rc;
    }

    g_initialized = true;
    *out_ready_event = g_received.ready_event;
    return 0;
}

Result pluginRuntimeInitializeWithService(Handle* out_ready_event,
                                          Handle* out_control_session) {
    g_publish_service = true;
    Result rc = pluginRuntimeInitialize(out_ready_event);
    if (R_SUCCEEDED(rc))
        *out_control_session = g_received.control_session;
    else
        g_publish_service = false;
    return rc;
}

Result pluginRuntimeInitializeSupervisor(Handle* out_ready_event,
                                         Handle* out_control_session,
                                         KaihenSupervisorEscrow* out_escrow) {
    if (!out_ready_event || !out_control_session || !out_escrow)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    g_supervisor_mode = true;
    const Result rc = pluginRuntimeInitializeWithService(out_ready_event, out_control_session);
    if (R_SUCCEEDED(rc)) *out_escrow = g_received.escrow; // borrowed, NOT transferred
    else g_supervisor_mode = false;
    return rc;
}

Result pluginRuntimeGetSupervisorLaunch(KaihenSupervisorLaunchGeometry* geometry,
                                        KaihenSupervisorLaunchHandles* handles) {
    if (!g_initialized || !g_supervisor_mode ||
        !g_received.geometry_received || !g_received.gate_handles_received ||
        !geometry || !handles) {
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }
    *geometry = g_received.launch_geometry;
    *handles = g_received.launch_handles;
    return 0;
}

Result pluginRuntimeGetSupervisorControlClient(Handle* client, Handle* fs_root) {
    if (!g_initialized || !g_supervisor_mode || !client || !fs_root ||
        g_received.supervisor_control_client == INVALID_HANDLE ||
        g_received.fs_root == INVALID_HANDLE)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    *client = g_received.supervisor_control_client;
    *fs_root = g_received.fs_root;
    return 0;
}

void pluginRuntimeExit(void) {
    if (g_initialized) {
        smExit();
        g_initialized = false;
    }
    closeReceivedHandles();
    g_publish_service = false;
    g_supervisor_mode = false;
}
