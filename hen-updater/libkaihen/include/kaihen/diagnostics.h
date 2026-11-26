// SPDX-License-Identifier: ISC
// Cached observations only: this API never refreshes PM, FS, or SM state.
#pragma once
#include "protocol.h"

#define HEN_MANAGER_DIAGNOSTICS_VERSION 1
#define HEN_MANAGER_DIAGNOSTICS_BUILD_ID 2026112505u

enum {
    HenManagerFlag_ShellAvailable = 1u,
    HenManagerFlag_GateOwned = 2u,
    HenManagerFlag_GateRestored = 4u,
    HenManagerFlag_Active = 8u,
    HenManagerFlag_Quarantined = 16u,
    HenManagerFlag_Pending = 32u,
    HenManagerFlag_PmDispatched = 64u,
    HenManagerFlag_PmFinished = 128u,
    HenManagerFlag_ProcessStarted = 256u,
    HenManagerFlag_TerminationActive = 512u,
    HenManagerFlag_GameHousekeepingAllowed = 1024u,
};

enum {
    // Bootstrap skipped boot plugin enumeration to preserve HEN's pending
    // restoration receipt. Skipped plugins are not launched automatically later.
    HenBootstrapFlag_AutoloadSkippedPendingRestore = 1u,
};

// Fields observed across threads are sampled, not one atomic transaction.
// All ticks/counters are local to this HEN instance, not wall-clock time.
// Started/completed PM counts cover manager liveness/identity queries on the
// HEN server thread, not all upstream GameArm or worker PM calls.
typedef struct {
    u32 version;
    u32 size;
    u32 build_id;
    u32 flags;
    u64 sampled_tick;
    u64 loop_iterations;
    u64 game_step_calls;
    u64 game_steps_suppressed;
    u64 pm_queries_started;
    u64 pm_queries_completed;
    HenBackgroundPluginStatus background;
    u64 game_pid;
    u32 game_phase;
    Result game_result;
    u32 game_serial;
    u32 game_mounts;
    Result shell_adoption_result;
    Result game_start_result;
    u32 game_start_attempts;
    u32 bootstrap_flags;
    Result restore_result;
    Result launch_result;
    Result last_pm_result;
    u32 trace_sequence;
    u32 trace_point;
    Result trace_result;
    u32 trace_value;
    u32 trace_requests;
    u32 trace_steps;
    u32 launch_failure_step; // Formerly reserved; zero means no recorded gate failure.
} HenManagerDiagnosticsV1;

#ifdef __cplusplus
static_assert(sizeof(HenManagerDiagnosticsV1) == 176, "manager diagnostics v1 layout");
#else
_Static_assert(sizeof(HenManagerDiagnosticsV1) == 176, "manager diagnostics v1 layout");
#endif
