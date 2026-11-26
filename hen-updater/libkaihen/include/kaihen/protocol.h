/* SPDX-License-Identifier: ISC
 * Copyright 2026 kaiHEN contributors
 *
 * The kaiHEN commands that Switch 2 homebrew can use, and their data.
 * kaihen.h has the client functions. This file needs only the libnx
 * types, so the RO and HEN servers and their host tests can include it.
 */
#pragma once

#include <switch/types.h>

/// The version of HEN's protocol, in HenStatus::protocol_version.
#define HEN_PROTOCOL_VERSION 7

#define HEN_PLUGIN_NAME_SIZE 64
#define HEN_MAX_PLUGINS 16

/// Private request on the plugin's control session to create a client session.
#define HEN_PLUGIN_CONTROL_CREATE_SESSION 0

typedef struct {
    char name[HEN_PLUGIN_NAME_SIZE];
} HenPluginName;

/// RO's private commands on the ldr:ro session. hbloader passes this session
/// to each NRO as the "ldr:ro" service override. It is initialized for the
/// NRO's process. Each command sends the caller's PID.
typedef enum {
    /// Returns a new HEN homebrew session by COPY. The caller owns it.
    KaihenRoCommand_GetHomebrewSession = 65004,
    /// Maps code for libounce's Jit API: input destination, source and size.
    KaihenRoCommand_MapJitCode = 65010,
    /// Unmaps code that KaihenRoCommand_MapJitCode mapped: input
    /// destination, source and size.
    KaihenRoCommand_UnmapJitCode = 65011,
    /// Unmaps all JIT code of the session. hbloader sends it when an NRO
    /// returns. The reply value is the number of mappings unmapped.
    KaihenRoCommand_ReleaseJitCode = 65012,
    /// Keeps a writable alias in RO of an already mapped executable JIT
    /// range. Input: destination and size. The reply value is RO's alias.
    KaihenRoCommand_MapJitAlias = 65013,
    /// Copies one input buffer into the retained alias and cleans its data
    /// cache. Input: destination, offset, size; one send buffer of size bytes.
    KaihenRoCommand_PublishJitAlias = 65014,
    /// Publishes several ranges in one request. Input: destination and range
    /// count; send buffers contain KaihenJitRange[] and concatenated bytes.
    KaihenRoCommand_PublishJitRanges = 65015,
    /// Unloads all NROs of the session: the program and the shared libraries
    /// that the loader or the program loaded for it. hbloader sends it when
    /// a program returns. The reply value is the number of NROs unloaded.
    KaihenRoCommand_ReleaseNros = 65016,
} KaihenRoCommand;

/// A byte range within a JIT mapping. Payload bytes follow range order.
typedef struct {
    u64 offset;
    u64 size;
} KaihenJitRange;

/// RO's local view of a persistent JIT alias, returned by MapJitAlias.
typedef struct {
    u64 address;
    u32 memory_type;
    u32 permission;
    u32 attribute;
    u32 reserved;
} KaihenJitAliasInfo;

#ifdef __cplusplus
static_assert(sizeof(KaihenJitAliasInfo) == 24, "JIT alias info layout");
#else
_Static_assert(sizeof(KaihenJitAliasInfo) == 24, "JIT alias info layout");
#endif

/// HEN's commands on a homebrew session.
typedef enum {
    /// Returns HenStatus.
    HenHomebrewCommand_GetStatus = 0,
    /// Returns a new clone of HEN's FS proxy root session by COPY.
    HenHomebrewCommand_GetFilesystemRoot = 16,
    /// Returns a new clone of HEN's set:sys session by COPY.
    HenHomebrewCommand_GetSettingsSession = 17,
    /// Returns a COPY of the calling program's process handle.
    HenHomebrewCommand_GetProcessHandle = 18,
    /// Returns a new session on HEN's SM proxy by COPY. The caller owns it.
    HenHomebrewCommand_GetServiceManager = 19,
    /// Returns the number of plugins that opted into IPC publication.
    HenHomebrewCommand_GetPluginCount = 20,
    /// Takes a u32 index and returns HenPluginName.
    HenHomebrewCommand_GetPluginName = 21,
    /// Takes HenPluginName and returns a new plugin session by COPY.
    HenHomebrewCommand_OpenPlugin = 22,
    /// Returns a new clone of HEN's fsp-ldr root by COPY. The loader passes
    /// it to Switch 2 programs as their fsp-ldr override. Every clone shares
    /// the root's object: a client must not send SetCurrentProcess on it.
    HenHomebrewCommand_GetLoaderRoot = 23,
    HenHomebrewCommand_LaunchBackgroundPlugin = 24,
    HenHomebrewCommand_GetBackgroundPluginStatus = 25,
    HenHomebrewCommand_CancelBackgroundPlugin = 26,
    HenHomebrewCommand_GetManagedPluginCount = 27,
    HenHomebrewCommand_GetManagedPluginInfo = 28,
    HenHomebrewCommand_StopManagedPlugin = 29,
    HenHomebrewCommand_RestartManagedPlugin = 30,
    HenHomebrewCommand_GetManagerDiagnostics = 31,
} HenHomebrewCommand;

// On-demand native plugin at /kaihen/plugins/<name>.nso.
// The plugin must use the ordinary libkaihen plugin runtime's mig:usr handshake.
typedef struct {
    char name[HEN_PLUGIN_NAME_SIZE];
    u8 sha256[32];
} HenBackgroundPluginRequest;

typedef enum {
    HenBackgroundPluginPhase_Idle = 0,
    HenBackgroundPluginPhase_Queued = 1,
    HenBackgroundPluginPhase_Verifying = 2,
    HenBackgroundPluginPhase_Packaging = 3,
    HenBackgroundPluginPhase_Parking = 4,
    HenBackgroundPluginPhase_Launching = 5,
    HenBackgroundPluginPhase_Exchanging = 6,
    HenBackgroundPluginPhase_Restoring = 7,
    HenBackgroundPluginPhase_Started = 8,
    HenBackgroundPluginPhase_WaitingMigration = 9,
    HenBackgroundPluginPhase_Provisioning = 10,
    HenBackgroundPluginPhase_WaitingReady = 11,
    HenBackgroundPluginPhase_RunningUnpublished = 12,
    HenBackgroundPluginPhase_Ready = 13,
    HenBackgroundPluginPhase_Published = 14,
    HenBackgroundPluginPhase_PublishFailed = 15,
    HenBackgroundPluginPhase_Failed = 16,
    HenBackgroundPluginPhase_Quarantined = 17,
    HenBackgroundPluginPhase_Cancelled = 18,
    HenBackgroundPluginPhase_Exited = 19,
} HenBackgroundPluginPhase;

typedef struct {
    u64 serial;
    u64 pid;
    HenBackgroundPluginPhase phase;
    Result result;
    u32 gate_step;
    u32 mounts;
} HenBackgroundPluginStatus;

// Entries track only plugins HEN launched or received through the bootstrap
// (not arbitrary OS processes). Stop is a PM termination, not a cooperative
// plugin-protocol command. Restart accepts a new digest for file replacement.
typedef enum {
    HenManagedState_Running = 1,
    HenManagedState_Stopping = 2,
    HenManagedState_Stopped = 3,
    HenManagedState_StopFailed = 4,
    HenManagedState_RestartQueued = 5,
} HenManagedState;
// Observation flags. GraceRequested means IPC was queued; it does not prove
// the plugin ran its cleanup callback. ExitConfirmed comes only from PM.
enum {
    HenManagedShutdown_GraceRequested = 1u,
    HenManagedShutdown_ForceRequested = 2u,
    HenManagedShutdown_ExitConfirmed = 4u,
    HenManagedShutdown_ExitDeadlineExpired = 8u,
};
typedef struct {
    HenPluginName name;
    u64 process_id;
    u64 serial;
    HenManagedState state;
    u32 origin; // 1 = boot-time, 2 = on-demand
    Result last_result;
    u32 shutdown_flags; // Previously reserved; unchanged structure size.
} HenManagedPluginInfo;

/// The reply of HenHomebrewCommand_GetStatus.
typedef struct {
    u64 process_id;               ///< HEN's process ID.
    u32 protocol_version;         ///< HEN_PROTOCOL_VERSION.
    Result initialization_result; ///< The result of HEN's startup.
} HenStatus;

#ifdef __cplusplus
static_assert(sizeof(HenStatus) == 16, "HenStatus layout");
#else
_Static_assert(sizeof(HenStatus) == 16, "HenStatus layout");
#endif

#include "diagnostics.h"
