/* SPDX-License-Identifier: ISC
 * kaiHEN optional plugin lifecycle ABI v1.
 * Plain C data contract shared by bootstrap, HEN, and plugin programs.
 * ABI is additive; a plugin built against the older runtime still starts.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KAIHEN_PLUGIN_LIFECYCLE_VERSION 1u
/* Sent ONLY on the plugin's private control session, not a public plugin client. */
#define KAIHEN_PLUGIN_CONTROL_REQUEST_SHUTDOWN 1u
#define KAIHEN_PLUGIN_FIRST_START_THIS_HEN_INSTANCE (1u << 0)

typedef enum KaihenPluginStartSource {
    KaihenPluginStart_Unknown = 0,
    KaihenPluginStart_Bootstrap = 1, /* started during initial kaiHEN activation */
    KaihenPluginStart_OnDemand = 2,  /* started after initial activation */
} KaihenPluginStartSource;

typedef enum KaihenPluginStartReason {
    KaihenPluginReason_Unknown = 0,
    KaihenPluginReason_BootPlugin = 1,
    KaihenPluginReason_ManualLaunch = 2,
    KaihenPluginReason_ManagerRestart = 3,
} KaihenPluginStartReason;

typedef enum KaihenPluginSessionPhase {
    KaihenPluginSession_Unknown = 0,
    /* Part of initial kaiHEN bootstrap, often reached through the 4/4 acquisition.
       This does NOT claim the exploit completed 4/4 at the instant of callback. */
    KaihenPluginSession_InitialActivation = 1,
    KaihenPluginSession_Live = 2,
} KaihenPluginSessionPhase;

typedef struct KaihenPluginLaunchContext {
    uint32_t version;
    uint32_t size;
    uint32_t start_source;      /* KaihenPluginStartSource */
    uint32_t start_reason;      /* KaihenPluginStartReason */
    uint32_t session_phase;     /* KaihenPluginSessionPhase */
    uint32_t flags;
    uint32_t launch_number;     /* 1 means first invocation of name tracked by this HEN instance */
    uint32_t reserved;
    uint64_t sequence;          /* manager launch operation serial, or 0 at bootstrap */
    uint64_t process_id;
} KaihenPluginLaunchContext;

typedef enum KaihenPluginShutdownReason {
    KaihenPluginShutdown_Unknown = 0,
    KaihenPluginShutdown_UserStop = 1,
    KaihenPluginShutdown_Restart = 2,
    KaihenPluginShutdown_Update = 3,
} KaihenPluginShutdownReason;

typedef struct KaihenPluginShutdownRequest {
    uint32_t version;
    uint32_t size;
    uint32_t reason;           /* KaihenPluginShutdownReason */
    uint32_t grace_period_ms;  /* advisory cleanup time before manager force-stop */
    uint64_t sequence;         /* request identifier, not a console-global boot ID */
} KaihenPluginShutdownRequest;

#ifdef __cplusplus
static_assert(sizeof(KaihenPluginLaunchContext) == 48, "launch ABI changed");
static_assert(sizeof(KaihenPluginShutdownRequest) == 24, "shutdown ABI changed");
static_assert(offsetof(KaihenPluginLaunchContext, sequence) == 32, "launch sequence offset");
static_assert(offsetof(KaihenPluginLaunchContext, process_id) == 40, "launch PID offset");
static_assert(offsetof(KaihenPluginShutdownRequest, sequence) == 16, "shutdown sequence offset");
#else
_Static_assert(sizeof(KaihenPluginLaunchContext) == 48, "launch ABI changed");
_Static_assert(sizeof(KaihenPluginShutdownRequest) == 24, "shutdown ABI changed");
_Static_assert(offsetof(KaihenPluginLaunchContext, sequence) == 32, "launch sequence offset");
_Static_assert(offsetof(KaihenPluginLaunchContext, process_id) == 40, "launch PID offset");
_Static_assert(offsetof(KaihenPluginShutdownRequest, sequence) == 16, "shutdown sequence offset");
#endif

#ifdef __cplusplus
}
#endif
