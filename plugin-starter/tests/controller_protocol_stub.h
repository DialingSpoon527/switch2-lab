// Existing public ABI types used by the controller flow mock. The new
// diagnostics structure itself is included unchanged from the real overlay.
#pragma once
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t Result;
#define HEN_PLUGIN_NAME_SIZE 64
typedef struct { char name[HEN_PLUGIN_NAME_SIZE]; } HenPluginName;
typedef struct { char name[HEN_PLUGIN_NAME_SIZE]; u8 sha256[32]; } HenBackgroundPluginRequest;
typedef enum {
    HenBackgroundPluginPhase_Idle,
    HenBackgroundPluginPhase_Queued,
    HenBackgroundPluginPhase_Verifying,
    HenBackgroundPluginPhase_Packaging,
    HenBackgroundPluginPhase_Parking,
    HenBackgroundPluginPhase_Launching,
    HenBackgroundPluginPhase_Exchanging,
    HenBackgroundPluginPhase_Restoring,
    HenBackgroundPluginPhase_Started,
    HenBackgroundPluginPhase_WaitingMigration,
    HenBackgroundPluginPhase_Provisioning,
    HenBackgroundPluginPhase_WaitingReady,
    HenBackgroundPluginPhase_RunningUnpublished,
    HenBackgroundPluginPhase_Ready,
    HenBackgroundPluginPhase_Published,
    HenBackgroundPluginPhase_PublishFailed,
    HenBackgroundPluginPhase_Failed,
    HenBackgroundPluginPhase_Quarantined,
    HenBackgroundPluginPhase_Cancelled,
    HenBackgroundPluginPhase_Exited,
} HenBackgroundPluginPhase;
typedef struct {
    u64 serial;
    u64 pid;
    HenBackgroundPluginPhase phase;
    Result result;
    u32 gate_step;
    u32 mounts;
} HenBackgroundPluginStatus;
typedef enum {
    HenManagedState_Running = 1,
    HenManagedState_Stopping,
    HenManagedState_Stopped,
    HenManagedState_StopFailed,
    HenManagedState_RestartQueued,
} HenManagedState;
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
    u32 origin;
    Result last_result;
    u32 shutdown_flags;
} HenManagedPluginInfo;
