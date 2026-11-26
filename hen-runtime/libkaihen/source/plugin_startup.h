// SPDX-License-Identifier: ISC
// The bootstrap's side of the plugin handoff (plugin_runtime.cpp has
// the plugin's side).
#pragma once

#include <switch.h>
#include "../include/kaihen/supervisor_launch.h"
#include "../include/kaihen/lifecycle.h"

enum PluginStartupCommand : u32 {
    PluginStartupCommand_AdoptHandles = 1,
    PluginStartupCommand_AdoptSupervisorHandles = 2,
    PluginStartupCommand_AdoptSupervisorGeometry = 3,
    PluginStartupCommand_AdoptSupervisorGateHandles0 = 4,
    PluginStartupCommand_AdoptSupervisorGateHandles1 = 5,
};

/// The version of the plugin runtime's AdoptHandles reply. A later version
/// may add data or handles after the current ones, but keeps these.
/// Versions 0 and 1 did not exist as such: the first runtime replied with a
/// "publishes a service" flag in this word, and sent the third handle only
/// when the flag was 1.
constexpr u32 PluginRuntimeVersion = 2;

/// Sends the plugin copies of the SM session and the FS root. The reply has
/// the release event (writable), the ready event (readable) and the client
/// end of the plugin's control session, which is INVALID_HANDLE when the
/// plugin does not publish a service. The caller owns the returned handles.
NX_INLINE Result pluginAdoptHandles(Service* plugin, Handle sm, Handle fs_root,
                                    Handle* release_event, Handle* ready_event,
                                    Handle* control_session) {
    Handle handles[3] = {INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE};
    u32 runtime_version = 0;
    Result rc = serviceDispatchOut(plugin, PluginStartupCommand_AdoptHandles, runtime_version,
        .in_num_handles = 2,
        .in_handles = {sm, fs_root},
        .out_handle_attrs = {SfOutHandleAttr_HipcCopy, SfOutHandleAttr_HipcCopy, SfOutHandleAttr_HipcCopy},
        .out_handles = handles,
    );
    if (R_FAILED(rc))
        return rc;

    // The first runtime's reply has two handles when the flag is 0, so the
    // third value read is not a handle.
    if (runtime_version == 0)
        handles[2] = INVALID_HANDLE;

    *release_event = handles[0];
    *ready_event = handles[1];
    *control_session = handles[2];
    return 0;
}

// Supervisor-only variant: six additional COPY handles. Every one is still
// owned by the bootstrap; neither endpoint may close the other's copy.
NX_INLINE Result pluginAdoptSupervisorHandles(Service* plugin, Handle sm,
        Handle fs_root, Handle shell, Handle monitor, Handle loader_root,
        Handle fs_process, Handle ro_reader, Handle fs_notifier,
        Handle* release_event, Handle* ready_event, Handle* control_session) {
    Handle handles[3] = {INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE};
    u32 runtime_version = 0;
    Result rc = serviceDispatchOut(plugin, PluginStartupCommand_AdoptSupervisorHandles,
        runtime_version,
        .in_num_handles = 8,
        .in_handles = {sm, fs_root, shell, monitor, loader_root,
                       fs_process, ro_reader, fs_notifier},
        .out_handle_attrs = {SfOutHandleAttr_HipcCopy, SfOutHandleAttr_HipcCopy,
                             SfOutHandleAttr_HipcCopy},
        .out_handles = handles,
    );
    if (R_FAILED(rc)) return rc;
    if (runtime_version != PluginRuntimeVersion) {
        for (Handle h : handles) if (h != INVALID_HANDLE) svcCloseHandle(h);
        return MAKERESULT(0x1EE, 0x140);
    }
    *release_event=handles[0];
    *ready_event=handles[1];
    *control_session=handles[2];
    return 0;
}

// The supervisor gets geometry and 14 extra COPY handles in THREE further
// requests (one scalar and two <=8-handle IPC messages) BEFORE bootstrap
// signals the release event. Keeping these separate
// avoids overflow of the 0x100-byte Horizon IPC message buffer.
NX_INLINE Result pluginAdoptSupervisorGeometry(Service* plugin,
        const KaihenSupervisorLaunchGeometry* geometry) {
    return serviceDispatchIn(plugin, PluginStartupCommand_AdoptSupervisorGeometry, *geometry);
}

NX_INLINE Result pluginAdoptSupervisorGateHandles0(Service* plugin,
        const KaihenSupervisorLaunchHandles* handles) {
    return serviceDispatch(plugin, PluginStartupCommand_AdoptSupervisorGateHandles0,
        .in_num_handles = 8,
        .in_handles = {handles->storage, handles->workers[0],
                       handles->workers[1], handles->workers[2],
                       handles->workers[3], handles->workers[4],
                       handles->workers[5], handles->workers[6]});
}

NX_INLINE Result pluginAdoptSupervisorGateHandles1(Service* plugin,
        const KaihenSupervisorLaunchHandles* handles, Handle hen_control) {
    return serviceDispatch(plugin, PluginStartupCommand_AdoptSupervisorGateHandles1,
        .in_num_handles = 6,
        .in_handles = {handles->server_copy, handles->event_read,
                       handles->event_write, handles->sm_master,
                       handles->ro_session, hen_control});
}
// Extended input only: the original runtime version and three output handles
// are unchanged. Old plugin runtimes that ignore inline input still work.
NX_INLINE Result pluginAdoptHandlesWithLifecycle(Service* plugin, Handle sm, Handle fs_root,
        const KaihenPluginLaunchContext* startup, Handle* release_event,
        Handle* ready_event, Handle* control_session) {
    if (!startup) return pluginAdoptHandles(plugin, sm, fs_root,
                                           release_event, ready_event, control_session);
    Handle handles[3] = {INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE};
    u32 runtime_version = 0;
    Result rc = serviceDispatchInOut(plugin, PluginStartupCommand_AdoptHandles,
        *startup, runtime_version,
        .in_num_handles = 2,
        .in_handles = {sm, fs_root},
        .out_handle_attrs = {SfOutHandleAttr_HipcCopy, SfOutHandleAttr_HipcCopy,
                             SfOutHandleAttr_HipcCopy},
        .out_handles = handles);
    if (R_FAILED(rc)) return rc;
    if (runtime_version == 0) handles[2] = INVALID_HANDLE;
    *release_event = handles[0];
    *ready_event = handles[1];
    *control_session = handles[2];
    return 0;
}
