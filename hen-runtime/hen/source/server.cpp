// SPDX-License-Identifier: GPL-2.0-only
#include <cstring>
#include "server.hpp"
#include "../../libkaihen/source/inline_payload.hpp"

namespace {

/// The service space that HEN registers mig:usr in.
constexpr u32 MigrationServiceSpace = 1;
/// How long the loop sleeps when no request is waiting.
constexpr u64 IdleSleepNs = 1000000;

bool isRequest(u32 type) {
    return type == CmifCommandType_Request || type == CmifCommandType_RequestWithContext;
}

bool hasBuffers(const HipcMetadata& meta) {
    return meta.num_send_statics || meta.num_recv_statics || meta.num_send_buffers ||
        meta.num_recv_buffers || meta.num_exch_buffers;
}

} // namespace

HenServer::HenServer() = default;

HenServer::~HenServer() {
    // Drain workers before this destructor closes borrowed FS/SM roots.
    graceful_.waitForWorker();
    background_.waitForWorkers();
    ounce::closeHandle(&supervisor_session_);
    ounce::closeHandle(&fs_root_);
    ounce::closeHandle(&sm_session_);
    if (settings_ready_) {
        setsysExit();
    }
    for (Handle& session : sessions_) {
        ounce::closeHandle(&session);
    }
    for (Handle& session : homebrew_sessions_) {
        ounce::closeHandle(&session);
    }
    for (Handle& process : homebrew_processes_) {
        ounce::closeHandle(&process);
    }
    ounce::closeHandle(&port_);
    for (PluginRecord& plugin : plugins_) {
        ounce::closeHandle(&plugin.control_session);
    }
}

Result HenServer::initialize() {
    status_.protocol_version = HEN_PROTOCOL_VERSION;
    Result rc = svcGetProcessId(&status_.process_id, CUR_PROCESS_HANDLE);
    if (R_SUCCEEDED(rc)) {
        rc = setsysInitialize();
        settings_ready_ = R_SUCCEEDED(rc);
    }
    if (R_SUCCEEDED(rc)) {
        rc = smRegisterServiceInSpace(&port_, MigrationServiceSpace, smEncodeName("mig:usr"),
                                      false, MaxSessions);
    }
    status_.initialization_result = rc;
    return rc;
}

//-----------------------------------------------------------------------------
// The loop
//-----------------------------------------------------------------------------

Result HenServer::run() {
    for (;;) {
        WaitList waits;
        collectWaits(waits);

        std::memset(armGetTls(), 0, ounce::MessageSize);
        s32 index = -1;
        const Result rc = svcReplyAndReceive(&index, waits.handles, waits.count,
                                             INVALID_HANDLE, 0);
        if (index < 0 || index >= waits.count) {
            if (rc == KERNELRESULT(TimedOut)) {
                stepBetweenRequests();
                continue;
            }
            if (R_FAILED(rc)) {
                return rc;
            }
            continue;
        }
        if (R_FAILED(rc) && !(index != waits.port_index && rc == KERNELRESULT(ConnectionClosed))) {
            return rc;
        }

        if (index == waits.port_index) {
            acceptSession();
            continue;
        }
        const SessionRef session = waits.sessions[index];
        if (rc == KERNELRESULT(ConnectionClosed)) {
            closeSession(session);
            continue;
        }
        const Result serve_result = serveSession(session);
        if (R_FAILED(serve_result)) {
            return serve_result;
        }
        // Reply was sent before shutdown. No additional game-arm step occurs.
        if (stop_requested_) return 0;
    }
}

void HenServer::collectWaits(WaitList& waits) const {
    waits.count = 0;
    waits.port_index = -1;
    if (port_ != INVALID_HANDLE) {
        waits.port_index = waits.count;
        waits.handles[waits.count++] = port_;
    }
    for (u32 index = 0; index < MaxSessions; index++) {
        if (sessions_[index] != INVALID_HANDLE) {
            waits.sessions[waits.count] = {SessionKind::Control, index};
            waits.handles[waits.count++] = sessions_[index];
        }
    }
    if (supervisor_session_ != INVALID_HANDLE) {
        waits.sessions[waits.count] = {SessionKind::Supervisor, 0};
        waits.handles[waits.count++] = supervisor_session_;
    }
    for (u32 index = 0; index < MaxSessions; index++) {
        if (homebrew_sessions_[index] != INVALID_HANDLE) {
            waits.sessions[waits.count] = {SessionKind::Homebrew, index};
            waits.handles[waits.count++] = homebrew_sessions_[index];
        }
    }
}

/// Advances game arming between incoming requests.
void HenServer::stepBetweenRequests() {
    ++manager_loop_iterations_;
    background_.pollTermination();
    u64 terminated_pid = 0;
    Result termination_result = 0;
    if (background_.takeTerminationResult(&terminated_pid, &termination_result) &&
        R_FAILED(termination_result)) {
        for (u32 i = 0; i < managed_.count(); ++i) {
            const auto* entry = managed_.at(i);
            if (!entry || entry->pid != terminated_pid) continue;
            managed_.stopFailed(entry->name, terminated_pid, termination_result);
            if (restart_pending_ && !std::strcmp(restart_request_.name, entry->name))
                restart_pending_ = false;
            break;
        }
    }
    if (!background_.gateOwned()) {
        ++manager_game_steps_;
        game_.step();
    } else {
        ++manager_game_suppressed_;
    }
    graceful_.reapCompleted();
    recordStartedPlugin();

    // No optional PM housekeeping during initial startup, Loader recovery,
    // a launch transaction, or a still-running termination IPC. Merely
    // having boot plugins installed never enables recurring manager polls.
    const bool work = managerNeedsPoll();
    const bool allowed = game_.managerHousekeepingAllowed() &&
        !background_.gateOwned() && !background_.active() && !background_.terminationActive();
    const u64 now = armGetSystemTick();
    if (!work) manager_poll_started_ = false;
    if (work && allowed && (!manager_poll_started_ ||
        armTicksToNs(now - manager_poll_tick_) >= 250000000ull)) {
        manager_poll_started_ = true;
        manager_poll_tick_ = now;
        removeExitedPlugins();
        tickGracefulShutdown();
        if (background_.pending()) {
            BackgroundPluginContext context;
            if (game_.backgroundContext(&context)) background_.startPending(context);
        }
        // takePublication can only succeed after the launch worker releases
        // ownership. Keep the original identity/liveness check for publication.
        HenPluginName name = {};
        Handle control = INVALID_HANDLE;
        u64 pid = 0;
        if (background_.takePublication(&name, &pid, &control)) {
            const HenPluginRecord record = {name, pid};
            bool exists = true;
            Result rc = managerQueryProcessPresence(pid, &exists);
            if (R_SUCCEEDED(rc)) rc = exists ? adoptPlugin(record, control)
                : MAKERESULT(Module_Libnx, LibnxError_NotFound);
            if (R_FAILED(rc)) svcCloseHandle(control);
            background_.publicationResult(rc);
        }
        // A restart never treats a PM error, timeout, or accepted stop as exit.
        if (restart_pending_ && !background_.active() && !background_.pending() &&
            !background_.terminationActive() && !background_.quarantined()) {
            const auto* old = managed_.find(restart_request_.name);
            if (!old || old->state == pluginmanager::Stopped) {
                const Result queued = queueManagedBackground(restart_request_, restart_host_pid_, true);
                if (queued != MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized)) {
                    if (R_FAILED(queued) && old) {
                        auto* modifiable = managed_.find(old->name);
                        if (modifiable) modifiable->last_result = queued;
                    }
                    restart_pending_ = false;
                }
            }
        }
    }
    expireManagedStops();
    svcSleepThread(IdleSleepNs);
}

void HenServer::acceptSession() {
    Handle session = INVALID_HANDLE;
    if (R_FAILED(svcAcceptSession(&session, port_))) {
        return;
    }
    for (Handle& available : sessions_) {
        if (available == INVALID_HANDLE) {
            available = session;
            return;
        }
    }
    svcCloseHandle(session);
}

/// Answers the request that the session sent.
Result HenServer::serveSession(SessionRef session) {
    const ReceivedRequest request = receiveRequest();
    if (request.type == CmifCommandType_Close) {
        closeSession(session);
        return 0;
    }

    CommandReply reply = serveRequest(request, session);
    if (!reply.keeps_request_handles) {
        for (u32 index = 0; index < request.num_copy_handles; index++) {
            svcCloseHandle(request.copy_handles[index]);
        }
    }

    void* message = armGetTls();
    const u32 num_copy_handles = reply.copy_handle != INVALID_HANDLE ? 1 : 0;
    ounce::writeReply(message, reply.result, reply.data, reply.data_size,
                      &reply.copy_handle, num_copy_handles);
    const Result rc = ounce::sendReply(sessionHandle(session));
    if (reply.close_copy_after_send) {
        ounce::closeHandle(&reply.copy_handle);
    }
    if (rc == KERNELRESULT(ConnectionClosed)) {
        closeSession(session);
        return 0;
    }
    return rc;
}

Handle& HenServer::sessionHandle(SessionRef session) {
    if (session.kind == SessionKind::Supervisor) return supervisor_session_;
    return session.kind == SessionKind::Homebrew ? homebrew_sessions_[session.index]
                                                 : sessions_[session.index];
}

void HenServer::closeSession(SessionRef session) {
    // If the supervisor disconnects unexpectedly, restore normal input
    // interception immediately. No Loader write is involved in this hold.
    if (session.kind == SessionKind::Supervisor) game_.resumeHold();
    ounce::closeHandle(&sessionHandle(session));
    if (session.kind == SessionKind::Homebrew) {
        ounce::closeHandle(&homebrew_processes_[session.index]);
    }
}

//-----------------------------------------------------------------------------
// Requests
//-----------------------------------------------------------------------------

/// Copies the request out of the message buffer. A request that is not
/// accepted still has its handles, so that they can be closed.
HenServer::ReceivedRequest HenServer::receiveRequest() const {
    void* message = armGetTls();
    const ounce::CmifRequest parsed = ounce::parseRequest(message);
    const HipcMetadata& meta = parsed.hipc.meta;

    ReceivedRequest request = {};
    request.type = meta.type;
    request.num_copy_handles = meta.num_copy_handles < MaxCopyHandles ? meta.num_copy_handles
                                                                      : MaxCopyHandles;
    for (u32 index = 0; index < request.num_copy_handles; index++) {
        request.copy_handles[index] = parsed.hipc.data.copy_handles[index];
    }
    // HEN takes no moved handles; close them now.
    for (u32 index = 0; index < meta.num_move_handles; index++) {
        svcCloseHandle(parsed.hipc.data.move_handles[index]);
    }

    request.accepted = parsed.valid && !hasBuffers(meta) && !meta.num_move_handles;
    if (request.accepted) {
        request.command_id = parsed.command_id;
        size_t payload_size = parsed.data_size;
        // libounce reserves a full 16-byte CMIF alignment area. The generic
        // parser reports its unused tail as data; normalize it before exact
        // ABI-size checks. Control messages keep their upstream handling.
        if (isRequest(meta.type) && !ounce::inlinePayloadSize(parsed, &payload_size)) {
            request.accepted = false;
            return request;
        }
        if (payload_size > sizeof(request.data)) {
            request.accepted = false;
            return request;
        }
        request.data_size = payload_size;
        std::memcpy(request.data, parsed.data, request.data_size);
    }
    return request;
}

/// Answers a request. The status commands answer on both kinds of session;
/// the others on one kind only. Anything else is BadInput.
HenServer::CommandReply HenServer::serveRequest(const ReceivedRequest& request,
                                                SessionRef session) {
    CommandReply reply;
    if (!request.accepted) {
        return reply;
    }
    if (request.type == CmifCommandType_Control &&
        request.command_id == ounce::CmifControl_QueryPointerBufferSize) {
        // HEN has no pointer buffer.
        const u16 pointer_buffer_size = 0;
        reply.result = 0;
        reply.setData(pointer_buffer_size);
        return reply;
    }
    if (!isRequest(request.type)) {
        return reply;
    }

    switch (request.command_id) {
    case HenControlCommand_GetStatus:
        if (!request.num_copy_handles) {
            reply.result = 0;
            reply.setData(status_);
        }
        return reply;
    case HenControlCommand_GetDebugSnapshot:
        if (!request.num_copy_handles && session.kind != SessionKind::Homebrew) {
            reply.result = 0;
            reply.setData(game_.debugSnapshot());
        }
        return reply;
    case HenControlCommand_GetGameStatus:
        if (!request.num_copy_handles) {
            reply.result = 0;
            reply.setData(game_.status());
        }
        return reply;
    default:
        break;
    }

    if (session.kind == SessionKind::Control) {
        serveControlCommand(request, reply);
    } else if (session.kind == SessionKind::Supervisor) {
        serveSupervisorCommand(request, reply);
    } else {
        serveHomebrewCommand(request, session.index, reply);
    }
    return reply;
}

/// The commands of the bootstrap's control session.
void HenServer::serveControlCommand(const ReceivedRequest& request, CommandReply& reply) {
    const u32 copies = request.num_copy_handles;
    const Handle* handles = request.copy_handles;

    switch (request.command_id) {
    case HenControlCommand_ConfigureGame:
        if (!copies && request.data_size >= sizeof(HenGameConfig)) {
            HenGameConfig configuration;
            std::memcpy(&configuration, request.data, sizeof(configuration));
            reply.result = game_.configure(configuration);
        }
        break;
    case HenControlCommand_AdoptGameHandles0:
        if (copies == 8) {
            reply.result = game_.adoptHandles0(handles);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptGameHandles1:
        if (copies == 7) {
            reply.result = game_.adoptHandles1(handles);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptLoaderRoot:
        if (copies == 1) {
            reply.result = game_.adoptLoaderRoot(handles[0]);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_StartGameArm:
        ++manager_game_start_attempts_;
        if (copies == 1 && fs_root_ != INVALID_HANDLE) {
            reply.result = startGameArm(handles[0]);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        manager_game_start_result_ = reply.result;
        break;
    case HenControlCommand_StartGameArmPaused:
        if (copies == 1 && fs_root_ != INVALID_HANDLE) {
            Handle root = INVALID_HANDLE;
            reply.result = cmifCloneCurrentObject(fs_root_, &root);
            if (R_SUCCEEDED(reply.result)) {
                reply.result = game_.start(root, handles[0], true);
                if (R_FAILED(reply.result)) svcCloseHandle(root);
            }
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_CreateHomebrewSession:
        if (copies == 1) {
            reply.result = createHomebrewSession(handles[0], &reply.copy_handle);
            reply.close_copy_after_send = R_SUCCEEDED(reply.result);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptFilesystemRoot:
        if (copies == 1) {
            reply.result = adoptFilesystemRoot(handles[0]);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptServiceManager:
        if (copies == 1) {
            reply.result = adoptServiceManager(handles[0]);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_CreateSupervisorSession:
        if (!copies && port_ != INVALID_HANDLE &&
            supervisor_session_ == INVALID_HANDLE) {
            reply.result = createSupervisorSession(&reply.copy_handle);
            reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_ReleaseControlPort:
        if (!copies) {
            reply.result = releaseControlPort();
        }
        break;
    case HenControlCommand_CreateServiceManagerSession:
        // A plugin's SM session, on the proxy as a homebrew program's is.
        if (!copies) {
            reply.result = sm_proxy_.openSession(&reply.copy_handle);
            reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptBackgroundShell: {
        if (copies == 1 && request.data_size == sizeof(BackgroundPluginCapability)) {
            BackgroundPluginCapability cap = {};
            std::memcpy(&cap, request.data, sizeof(cap));
            manager_bootstrap_flags_ = cap.reserved[0];
            reply.result = background_.adoptShell(handles[0], cap);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        manager_shell_result_ = reply.result;
        break;
    }
    case HenControlCommand_RegisterPluginProcess: {
        if (!copies && request.data_size == sizeof(HenPluginRecord)) {
            HenPluginRecord record = {};
            std::memcpy(&record, request.data, sizeof(record));
            reply.result = managed_.track(record.name.name, record.process_id, 0,
                                          pluginmanager::BootPlugin) ? 0 :
                MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        }
        break;
    }
    case HenControlCommand_CloneFilesystemRoot:
        // A plugin's FS root: a session of its own, not HEN's root.
        if (!copies && fs_root_ != INVALID_HANDLE) {
            reply.result = cmifCloneCurrentObject(fs_root_, &reply.copy_handle);
            reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptPlugin:
        if (copies == 1 && request.data_size >= sizeof(HenPluginRecord)) {
            HenPluginRecord plugin;
            std::memcpy(&plugin, request.data, sizeof(plugin));
            reply.result = adoptPlugin(plugin, handles[0]);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    default:
        break;
    }
}

// These commands are NOT reachable over a homebrew or bootstrap session.
// The supervisor's kernel session was minted before bootstrap port release.
Result HenServer::createSupervisorSession(Handle* out_client) {
    Handle server = INVALID_HANDLE;
    Handle client = INVALID_HANDLE;
    Result rc = svcCreateSession(&server, &client, 0, 0);
    if (R_SUCCEEDED(rc)) {
        supervisor_session_ = server;
        *out_client = client;
    }
    return rc;
}

bool HenServer::pluginWorkIdleForReplacement() const {
    if (!background_.canQueue() || background_.gateOwned() ||
        graceful_.occupied() || restart_pending_) return false;
    // Plugin control sessions and registry entries belong to this HEN.
    // Until migration of those entries is implemented and tested, require
    // non-supervisor plugins to be stopped before replacing HEN.
    for (u32 i = 0; i < managed_.count(); ++i) {
        const auto* entry = managed_.at(i);
        if (entry && entry->state != pluginmanager::Stopped &&
            std::strcmp(entry->name, "hotupdate-supervisor")) return false;
    }
    return true;
}

void HenServer::serveSupervisorCommand(const ReceivedRequest& request,
                                       CommandReply& reply) {
    if (request.command_id == HenControlCommand_ReplaceRoReader &&
        request.num_copy_handles == 1 && !request.data_size) {
        reply.result = game_.replaceRoReader(request.copy_handles[0]);
        reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        return;
    }
    if (request.num_copy_handles || request.data_size) return;
    switch (request.command_id) {
    case HenControlCommand_QuerySupervisorQuiescence: {
        const u32 bits = (game_.canHold() && pluginWorkIdleForReplacement() ? 1u : 0u) |
                         (game_.isHeld() ? 2u : 0u);
        reply.result = 0;
        reply.setData(bits);
        break;
    }
    case HenControlCommand_HoldGameArm:
        if (pluginWorkIdleForReplacement()) reply.result = game_.holdAtSafePoint();
        break;
    case HenControlCommand_ResumeGameArm:
        // Old generation's ordinary hold/resume, never start an unconfigured one.
        if (game_.safeToExitHeld()) {
            game_.resumeHold();
            reply.result = 0;
        }
        break;
    case HenControlCommand_WarmupPaused:
        reply.result = game_.warmupPaused();
        break;
    case HenControlCommand_ActivatePaused:
        if (game_.safeToExitHeld()) {
            game_.resumeHold();
            reply.result = 0;
        }
        break;
    case HenControlCommand_CreateRoControlSession: {
        Handle server = INVALID_HANDLE, client = INVALID_HANDLE;
        for (Handle& available : sessions_) {
            if (available != INVALID_HANDLE) continue;
            reply.result = svcCreateSession(&server, &client, false, 0);
            if (R_SUCCEEDED(reply.result)) {
                available = server;
                reply.copy_handle = client;
                reply.close_copy_after_send = true;
            }
            break;
        }
        break;
    }
    case HenControlCommand_StopIfHeld:
        // No async kill: only the HEN loop may exit after replying.
        if (game_.safeToExitHeld() && pluginWorkIdleForReplacement()) {
            stop_requested_ = true;
            reply.result = 0;
        }
        break;
    default: break;
    }
}

/// The commands of a homebrew process's session.
void HenServer::serveHomebrewCommand(const ReceivedRequest& request, u32 index,
                                     CommandReply& reply) {
    if (request.num_copy_handles) {
        return;
    }

    switch (request.command_id) {
    case HenHomebrewCommand_GetFilesystemRoot:
        if (fs_root_ == INVALID_HANDLE) {
            reply.result = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
            break;
        }
        reply.result = cmifCloneCurrentObject(fs_root_, &reply.copy_handle);
        reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        break;
    case HenHomebrewCommand_GetSettingsSession:
        reply.result = cmifCloneCurrentObject(setsysGetServiceSession()->session,
                                              &reply.copy_handle);
        reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        break;
    case HenHomebrewCommand_GetProcessHandle:
        // The process that RO initialized this session for. HEN keeps it.
        reply.result = 0;
        reply.copy_handle = homebrew_processes_[index];
        break;
    case HenHomebrewCommand_GetServiceManager:
        // A session of the program's own on the SM proxy, never the master:
        // SM closes a session after a malformed request, and the master
        // serves every program.
        reply.result = sm_proxy_.openSession(&reply.copy_handle);
        reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        break;
    case HenHomebrewCommand_LaunchBackgroundPlugin: {
        if (request.data_size != sizeof(HenBackgroundPluginRequest) ||
            fs_root_ == INVALID_HANDLE || !background_.canQueue()) break;
        HenBackgroundPluginRequest submitted = {};
        std::memcpy(&submitted, request.data, sizeof(submitted));
        u64 requester_pid = 0;
        Result caller = svcGetProcessId(&requester_pid, homebrew_processes_[index]);
        reply.result = R_FAILED(caller) ? caller : restart_pending_ ?
            MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized) :
            queueManagedBackground(submitted, requester_pid);
        break;
    }
    case HenHomebrewCommand_GetManagerDiagnostics: {
        if (request.data_size) break;
        reply.result = 0;
        reply.setData(managerDiagnostics());
        break;
    }
    case HenHomebrewCommand_GetBackgroundPluginStatus: {
        if (request.data_size) break;
        reply.result = 0;
        reply.setData(background_.status()); // Cached; does not prove current liveness.
        break;
    }
    case HenHomebrewCommand_CancelBackgroundPlugin: {
        if (request.data_size) break;
        if (restart_pending_) {
            restart_pending_ = false;
            reply.result = 0;
        } else {
            reply.result = background_.cancelPending();
        }
        break;
    }
    case HenHomebrewCommand_GetManagedPluginCount: {
        if (request.data_size) break;
        // Cached; explicit stop/restart and paced active operations refresh liveness.
        const u32 count = managed_.count();
        reply.result = 0;
        reply.setData(count);
        break;
    }
    case HenHomebrewCommand_GetManagedPluginInfo: {
        if (request.data_size != sizeof(u32)) break;
        u32 item = 0;
        std::memcpy(&item, request.data, sizeof(item));
        const auto* entry = managed_.at(item);
        if (!entry) { reply.result = MAKERESULT(Module_Libnx, LibnxError_NotFound); break; }
        HenManagedPluginInfo info = {};
        std::memcpy(info.name.name, entry->name, sizeof(info.name.name));
        info.process_id = entry->pid;
        info.serial = entry->serial;
        info.state = restart_pending_ &&
            std::strcmp(restart_request_.name, entry->name) == 0 ?
            HenManagedState_RestartQueued : static_cast<HenManagedState>(entry->state);
        info.origin = entry->origin;
        info.last_result = entry->last_result;
        info.shutdown_flags = entry->shutdown_flags;
        reply.result = 0;
        reply.setData(info);
        break;
    }
    case HenHomebrewCommand_StopManagedPlugin: {
        if (request.data_size != sizeof(HenPluginName)) break;
        HenPluginName name = {};
        std::memcpy(&name, request.data, sizeof(name));
        if (!pluginmanager::Registry::validName(name.name)) break;
        // An explicit stop supersedes any earlier scheduled restart.
        if (restart_pending_ && std::strcmp(restart_request_.name, name.name) == 0)
            restart_pending_ = false;
        reply.result = stopManaged(name);
        break;
    }
    case HenHomebrewCommand_RestartManagedPlugin: {
        if (request.data_size != sizeof(HenBackgroundPluginRequest) || restart_pending_) break;
        HenBackgroundPluginRequest next = {};
        std::memcpy(&next, request.data, sizeof(next));
        if (!pluginmanager::Registry::validName(next.name)) break;
        auto* entry = managed_.find(next.name);
        if (!entry || background_.active() || background_.pending() ||
            background_.terminationActive() || background_.quarantined()) {
            reply.result = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
            break;
        }
        // Verify and retain the requester's identity BEFORE terminating the
        // old plugin. A malformed caller must not stop a healthy process.
        u64 caller_pid = 0;
        Result caller = svcGetProcessId(&caller_pid, homebrew_processes_[index]);
        if (R_FAILED(caller) || !caller_pid) {
            reply.result = R_FAILED(caller) ? caller :
                MAKERESULT(Module_Libnx, LibnxError_BadInput);
            break;
        }
        if (entry->state != pluginmanager::Stopped) {
            HenPluginName name = {};
            std::memcpy(name.name, next.name, sizeof(name.name));
            reply.result = stopManaged(name, KaihenPluginShutdown_Restart);
            if (R_FAILED(reply.result)) break;
        }
        restart_request_ = next;
        restart_host_pid_ = caller_pid;
        restart_pending_ = true;
        reply.result = 0;
        break;
    }
    case HenHomebrewCommand_GetLoaderRoot:
        if (game_.loaderRoot() == INVALID_HANDLE) {
            reply.result = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
            break;
        }
        reply.result = cmifCloneCurrentObject(game_.loaderRoot(), &reply.copy_handle);
        reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        break;
    case HenHomebrewCommand_GetPluginCount:
        removeExitedPlugins();
        reply.result = 0;
        reply.setData(plugin_count_);
        break;
    case HenHomebrewCommand_GetPluginName: {
        u32 plugin = 0;
        if (request.data_size < sizeof(plugin)) {
            break;
        }
        std::memcpy(&plugin, request.data, sizeof(plugin));
        if (plugin >= plugin_count_) {
            reply.result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
            break;
        }
        reply.result = 0;
        reply.setData(plugins_[plugin].name);
        break;
    }
    case HenHomebrewCommand_OpenPlugin: {
        HenPluginName name;
        if (request.data_size < sizeof(name)) {
            break;
        }
        std::memcpy(&name, request.data, sizeof(name));
        removeExitedPlugins();
        reply.result = openPlugin(name, &reply.copy_handle);
        reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        break;
    }
    default:
        break;
    }
}

//-----------------------------------------------------------------------------
// Control commands
//-----------------------------------------------------------------------------

/// Starts game arming on a clone of HEN's FS root. The game arm closes the
/// bootstrap's process handle once it has started.
Result HenServer::startGameArm(Handle bootstrap_process) {
    Handle root = INVALID_HANDLE;
    Result rc = cmifCloneCurrentObject(fs_root_, &root);
    if (R_FAILED(rc)) {
        return rc;
    }
    rc = game_.start(root, bootstrap_process);
    if (R_FAILED(rc)) {
        svcCloseHandle(root);
    }
    return rc;
}

/// Creates a session for a process that RO initialized, and keeps the
/// process handle for GetProcessHandle.
Result HenServer::createHomebrewSession(Handle copied_process, Handle* out_client) {
    for (u32 index = 0; index < MaxSessions; index++) {
        if (homebrew_sessions_[index] != INVALID_HANDLE) {
            continue;
        }
        Handle server = INVALID_HANDLE;
        Handle client = INVALID_HANDLE;
        Result rc = svcCreateSession(&server, &client, 0, 0);
        if (R_SUCCEEDED(rc)) {
            homebrew_sessions_[index] = server;
            homebrew_processes_[index] = copied_process;
            *out_client = client;
        }
        return rc;
    }
    return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
}

Result HenServer::adoptFilesystemRoot(Handle copied_root) {
    if (fs_root_ != INVALID_HANDLE) {
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    }
    fs_root_ = copied_root;
    return 0;
}

Result HenServer::adoptServiceManager(Handle copied_session) {
    if (sm_session_ != INVALID_HANDLE) {
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    }
    sm_session_ = copied_session;
    sm_proxy_.setMaster(sm_session_);
    return 0;
}

Result HenServer::releaseControlPort() {
    if (port_ == INVALID_HANDLE) {
        return 0;
    }
    const Result rc = smUnregisterServiceInSpace(MigrationServiceSpace, smEncodeName("mig:usr"));
    if (R_FAILED(rc)) {
        return rc;
    }
    ounce::closeHandle(&port_);
    return 0;
}

Result HenServer::adoptPlugin(const HenPluginRecord& plugin, Handle control_session) {
    const HenPluginName& name = plugin.name;
    if (!name.name[0] || !std::memchr(name.name, '\0', sizeof(name.name))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    for (u32 index = 0; index < plugin_count_; index++) {
        if (!std::strcmp(plugins_[index].name.name, name.name)) {
            return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
        }
    }
    if (plugin_count_ == HEN_MAX_PLUGINS) {
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    // HEN accepts published plugin processes only; no arbitrary client may
    // register a PID without using the privileged bootstrap control channel.
    const auto launching = background_.status();
    const auto origin = launching.pid == plugin.process_id &&
        background_.sameName(name.name) ? pluginmanager::OnDemand : pluginmanager::BootPlugin;
    if (!managed_.track(name.name, plugin.process_id, launching.serial, origin))
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    plugins_[plugin_count_++] = {name, plugin.process_id, control_session};
    return 0;
}

// Stop and launch entry points share the same HEN plugin manager. Never take
// a raw user-supplied PID or acquire new system process permissions.
Result HenServer::queueManagedBackground(const HenBackgroundPluginRequest& submitted,
                                         u64 host_pid, bool from_restart) {
    if (!pluginmanager::Registry::validName(submitted.name) || fs_root_ == INVALID_HANDLE || !host_pid)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!std::strcmp(submitted.name, "hotupdate-supervisor"))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (game_.isHeld() || !game_.managerHousekeepingAllowed() || !background_.canQueue())
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    removeExitedPlugins();
    if (plugin_count_ >= HEN_MAX_PLUGINS)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    if (managed_.count() >= pluginmanager::RegistryCapacity &&
        !managed_.find(submitted.name))
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    // Refuse duplicate names, including plugins without a published service.
    if (const auto* existing = managed_.find(submitted.name)) {
        if (existing->state != pluginmanager::Stopped)
            return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    }
    for (u32 i = 0; i < plugin_count_; ++i)
        if (!std::strncmp(plugins_[i].name.name, submitted.name, HEN_PLUGIN_NAME_SIZE))
            return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    Handle proxy = INVALID_HANDLE, root = INVALID_HANDLE;
    Result rc = sm_proxy_.openSession(&proxy);
    if (R_SUCCEEDED(rc)) rc = cmifCloneCurrentObject(fs_root_, &root);
    KaihenPluginLaunchContext startup = {};
    startup.version = KAIHEN_PLUGIN_LIFECYCLE_VERSION;
    startup.size = sizeof(startup);
    startup.start_source = KaihenPluginStart_OnDemand;
    startup.session_phase = KaihenPluginSession_Live;
    startup.start_reason = from_restart ? KaihenPluginReason_ManagerRestart :
                           KaihenPluginReason_ManualLaunch;
    const auto* previous = managed_.find(submitted.name);
    startup.launch_number = previous ? previous->launches_this_hen + 1 : 1;
    startup.flags = startup.launch_number == 1 ?
                    KAIHEN_PLUGIN_FIRST_START_THIS_HEN_INSTANCE : 0;
    if (R_SUCCEEDED(rc)) rc = background_.queue(submitted, proxy, root, startup);
    if (R_SUCCEEDED(rc)) request_host_pid_ = host_pid;
    if (R_FAILED(rc)) {
        if (proxy != INVALID_HANDLE) svcCloseHandle(proxy);
        if (root != INVALID_HANDLE) svcCloseHandle(root);
    }
    return rc;
}

// Tick on HEN's main loop. A failed or timed-out cooperative request must
// not freeze HEN, and force-stop still requires the original exact PID and
// signed migration-title identity. Never kill an arbitrary supplied PID.
void HenServer::tickGracefulShutdown() {
    // Called only by the paced manager tick after its single liveness sweep.
    if (!graceful_.occupied() || background_.gateOwned() || background_.active() ||
        !game_.managerHousekeepingAllowed() || background_.terminationActive()) return;
    if (!graceful_.shouldForce()) return;
    const u64 pid = graceful_.pid();
    const pluginmanager::Entry* entry = nullptr;
    for (u32 i = 0; i < managed_.count(); ++i) {
        const auto* candidate = managed_.at(i);
        if (candidate && candidate->pid == pid && candidate->state == pluginmanager::Stopping) {
            entry = candidate;
            break;
        }
    }
    if (!entry) { graceful_.noteForceDispatched(); return; }
    u64 actual_program = 0;
    Result rc = managerQueryProgramId(&actual_program, pid);
    if (R_SUCCEEDED(rc) && !background_.recognizesProgramId(actual_program))
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_SUCCEEDED(rc)) rc = background_.terminateExactPid(pid);
    if (R_FAILED(rc)) {
        managed_.stopFailed(entry->name, pid, rc);
        if (restart_pending_ && !std::strcmp(restart_request_.name, entry->name)) restart_pending_ = false;
    } else managed_.noteShutdown(entry->name, pid, pluginmanager::ForceRequested);
    graceful_.noteForceDispatched();
}

// Forced stop is used for generic plugins because the plugin ABI does not
// specify a universal shutdown IPC command. Only a previously recorded
// kernel PID, verified as the original signed migration program, is eligible.
Result HenServer::stopManaged(const HenPluginName& name, u32 reason) {
    if (!pluginmanager::Registry::validName(name.name))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!std::strcmp(name.name, "hotupdate-supervisor"))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (game_.isHeld() || !game_.managerHousekeepingAllowed() || background_.active() || background_.gateOwned() || background_.pending() ||
        background_.quarantined() || background_.terminationActive())
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    removeExitedPlugins();
    auto* entry = managed_.find(name.name);
    if (!entry) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    if (entry->state == pluginmanager::Stopped) return 0;
    if (entry->state == pluginmanager::Stopping) return 0;
    const u64 pid = entry->pid;
    u64 actual_program = 0;
    Result rc = managerQueryProgramId(&actual_program, pid);
    if (R_FAILED(rc)) {
        // A plugin may exit naturally between registry polling and this PM
        // query. Confirm absence before treating the stop as a failure.
        bool exists = true;
        const Result presence = managerQueryProcessPresence(pid, &exists);
        if (R_SUCCEEDED(presence) && !exists) {
            managed_.exited(name.name, pid);
            return 0;
        }
        return rc;
    }
    if (!background_.recognizesProgramId(actual_program))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    u64 selected = 0;
    if (!managed_.beginStop(name.name, &selected, armGetSystemTick()) || selected != pid)
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    // Only the exact managed name/PID pair is eligible. The private
    // plugin control session is never supplied to arbitrary homebrew.
    for (u32 i = 0; i < plugin_count_; ++i) {
        if (plugins_[i].process_id != pid ||
            std::strncmp(plugins_[i].name.name, name.name, HEN_PLUGIN_NAME_SIZE)) continue;
        if (!graceful_.occupied()) {
            const Result requested = graceful_.request(pid, plugins_[i].control_session,
                                                        reason, 2500);
            if (R_SUCCEEDED(requested)) {
                // The shutdown worker owns this endpoint until it returns.
                // HEN must not issue CreateSession concurrently on it.
                plugins_[i].control_session = INVALID_HANDLE;
                managed_.noteShutdown(name.name, pid, pluginmanager::GraceRequested);
                return 0; // async best effort, not process-exit confirmation
            }
        }
        break;
    }
    // Old runtime/no IPC, or no graceful worker: original force-stop fallback.
    rc = background_.terminateExactPid(pid);
    if (R_FAILED(rc)) managed_.stopFailed(name.name, pid, rc);
    else managed_.noteShutdown(name.name, pid, pluginmanager::ForceRequested);
    // A zero result only means the termination worker accepted the request; HEN will
    // poll PM before marking the plugin stopped or permitting a restart.
    return rc;
}

//-----------------------------------------------------------------------------
// Homebrew commands
//-----------------------------------------------------------------------------

/// Asks the named plugin for a new client session.
Result HenServer::openPlugin(const HenPluginName& name, Handle* out_session) {
    if (!std::memchr(name.name, '\0', sizeof(name.name))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    for (u32 index = 0; index < plugin_count_; index++) {
        if (std::strcmp(plugins_[index].name.name, name.name)) {
            continue;
        }
        // Cooperative stop transfers this private session to its
        // worker. Do not issue a concurrent request or use an invalid handle.
        if (plugins_[index].control_session == INVALID_HANDLE)
            return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
        Service control = {};
        serviceCreate(&control, plugins_[index].control_session);
        control.own_handle = false;
        const Result rc = serviceDispatch(&control, HEN_PLUGIN_CONTROL_CREATE_SESSION,
            .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
            .out_handles = out_session,
        );
        // The plugin closed its control session, so it no longer serves.
        if (rc == KERNELRESULT(ConnectionClosed)) {
            removePlugin(index);
            return MAKERESULT(Module_Libnx, LibnxError_NotFound);
        }
        return rc;
    }
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

void HenServer::removePlugin(u32 index) {
    if (plugins_[index].control_session != INVALID_HANDLE)
        svcCloseHandle(plugins_[index].control_session);
    plugin_count_--;
    for (u32 next = index; next < plugin_count_; next++) {
        plugins_[next] = plugins_[next + 1];
    }
    plugins_[plugin_count_] = {};
}

/// Refreshes liveness for explicit mutations, legacy plugin discovery,
/// or a paced pending management operation. Cached diagnostics/list never call it.
void HenServer::removeExitedPlugins() {
    if (!game_.managerHousekeepingAllowed() || background_.gateOwned() ||
        background_.active() || background_.terminationActive()) return;
    // Deduplicate published and managed records in this pass. A failed query
    // is cached too; never turn unknown liveness into an observed exit.
    struct Presence { u64 pid; Result result; bool exists; };
    Presence observed[pluginmanager::RegistryCapacity + HEN_MAX_PLUGINS] = {};
    u32 observed_count = 0;
    const auto query = [&](u64 pid, bool* exists) -> Result {
        for (u32 i = 0; i < observed_count; ++i) {
            if (observed[i].pid != pid) continue;
            *exists = observed[i].exists;
            return observed[i].result;
        }
        bool present = true;
        const Result rc = managerQueryProcessPresence(pid, &present);
        if (observed_count < pluginmanager::RegistryCapacity + HEN_MAX_PLUGINS)
            observed[observed_count++] = {pid, rc, present};
        *exists = present;
        return rc;
    };
    for (u32 n = 0; n < managed_.count(); ++n) {
        const auto* entry = managed_.at(n);
        if (!entry || entry->state == pluginmanager::Stopped) continue;
        bool exists = true;
        if (R_SUCCEEDED(query(entry->pid, &exists)) && !exists) {
            const u64 pid = entry->pid;
            managed_.exited(entry->name, pid);
            background_.noteExit(pid);
            graceful_.finishIfExited(pid);
        }
    }
    for (u32 i = plugin_count_; i > 0; --i) {
        bool exists = true;
        if (R_SUCCEEDED(query(plugins_[i - 1].process_id, &exists)) && !exists)
            removePlugin(i - 1);
    }
}

// No service queries here. This remains useful while the background worker
// is quarantined, provided HEN's own IPC server thread can still answer.
HenManagerDiagnosticsV1 HenServer::managerDiagnostics() const {
    HenManagerDiagnosticsV1 out = {};
    out.version = HEN_MANAGER_DIAGNOSTICS_VERSION;
    out.size = sizeof(out);
    out.build_id = HEN_MANAGER_DIAGNOSTICS_BUILD_ID;
    out.flags = background_.diagnosticFlags();
    if (game_.managerHousekeepingAllowed()) out.flags |= HenManagerFlag_GameHousekeepingAllowed;
    out.sampled_tick = armGetSystemTick();
    out.loop_iterations = manager_loop_iterations_;
    out.game_step_calls = manager_game_steps_;
    out.game_steps_suppressed = manager_game_suppressed_;
    out.pm_queries_started = manager_pm_started_;
    out.pm_queries_completed = manager_pm_completed_;
    out.background = background_.status();
    const auto game = game_.status();
    out.game_pid = game.process_id;
    out.game_phase = game.phase;
    out.game_result = game.result;
    out.game_serial = game.serial;
    out.game_mounts = game.mounts;
    out.shell_adoption_result = manager_shell_result_;
    out.game_start_result = manager_game_start_result_;
    out.game_start_attempts = manager_game_start_attempts_;
    out.bootstrap_flags = manager_bootstrap_flags_;
    out.restore_result = background_.lastRestoreResult();
    out.launch_result = background_.launchResult();
    out.last_pm_result = manager_last_pm_result_;
    const auto trace = game_.traceSnapshot();
    out.trace_sequence = trace.sequence;
    out.trace_point = trace.point;
    out.trace_result = trace.result;
    out.trace_value = trace.value;
    out.trace_requests = trace.requests;
    out.trace_steps = trace.steps;
    out.launch_failure_step = background_.launchFailureStep();
    return out;
}

Result HenServer::managerQueryProcessPresence(u64 pid, bool* exists) {
    ++manager_pm_started_;
    const Result rc = game_.queryProcessPresence(pid, exists);
    manager_last_pm_result_ = rc;
    ++manager_pm_completed_;
    return rc;
}

Result HenServer::managerQueryProgramId(u64* program, u64 pid) {
    ++manager_pm_started_;
    const Result rc = pmdmntGetProgramId(program, pid);
    manager_last_pm_result_ = rc;
    ++manager_pm_completed_;
    return rc;
}

// Register a confirmed StartProcess once, without repeatedly querying PM or
// resurrecting a registry entry already observed exited by a later sweep.
void HenServer::recordStartedPlugin() {
    if (!background_.hasStartedProcess()) return;
    const auto latest = background_.status();
    if (!latest.pid || !latest.serial || latest.serial == manager_tracked_serial_) return;
    const auto name = background_.currentName();
    if (managed_.track(name.name, latest.pid, latest.serial, pluginmanager::OnDemand))
        manager_tracked_serial_ = latest.serial;
}

bool HenServer::managerNeedsPoll() const {
    if (background_.pending() || restart_pending_ || background_.publicationPending()) return true;
    for (u32 i = 0; i < managed_.count(); ++i) {
        const auto* entry = managed_.at(i);
        if (entry && entry->state == pluginmanager::Stopping) return true;
    }
    return false;
}

// The stop watchdog is bookkeeping only; even a blocked PM worker cannot
// turn deadline expiration into false process-exit confirmation.
void HenServer::expireManagedStops() {
    for (u32 i = 0; i < managed_.count(); ++i) {
        const auto* entry = managed_.at(i);
        if (!entry || entry->state != pluginmanager::Stopping || !entry->stop_started_tick ||
            armTicksToNs(armGetSystemTick() - entry->stop_started_tick) < 15000000000ull) continue;
        managed_.stopFailed(entry->name, entry->pid, KERNELRESULT(TimedOut));
        managed_.noteShutdown(entry->name, entry->pid, pluginmanager::ExitDeadlineExpired);
        if (restart_pending_ && !std::strcmp(restart_request_.name, entry->name))
            restart_pending_ = false;
    }
}
