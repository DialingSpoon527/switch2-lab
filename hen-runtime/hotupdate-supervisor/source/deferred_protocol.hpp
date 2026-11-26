// SPDX-License-Identifier: GPL-2.0-only
// HEN-only updater. Protocol 5 adds a digest-bound queue; reply layouts stay fixed.
#pragma once
#include <switch.h>
#include <kaihen/plugin.h>

namespace hotupdate {
constexpr u32 DeferredProtocolVersion = 5;
constexpr u64 DeferredDryRunConfirmation = 0x4b48534452594e31ull; // KHSDRYN1
constexpr u64 DeferredHenConfirmation = 0x4b48454e51455631ull; // KHENQEV1
constexpr u64 DeferredRoConfirmation = 0x4b48524f51455631ull; // KHROQEV1
constexpr u32 DeferredQueueCommand = 20;
constexpr u32 DeferredStatusCommand = 21;
constexpr u32 DeferredCancelCommand = 22;
constexpr u32 DeferredQueueHenCommand = 23;
constexpr u32 DeferredRestartStatusCommand = 24;
constexpr u32 DeferredPreflightHenCommand = 25;
constexpr u32 DeferredQueueRoCommand = 26; // legacy RESERVED: always rejected
constexpr u32 DeferredPreflightRoCommand = 27; // legacy RESERVED: always rejected
constexpr u32 DeferredCapabilitiesCommand = 28;
constexpr u32 DeferredQueueHenPackageCommand = 29;
enum class DeferredKind : u32 { None = 0, DryRun = 1, ReplaceHen = 2, ReplaceRo = 3 /* legacy RESERVED: rejected */ };

enum class DeferredPhase : u32 {
    Idle = 0,
    WaitingForHostExit = 1,
    WaitingForHenIdle = 2,
    DryRunPassed = 3,
    TimedOut = 4,
    Cancelled = 5,
    Failed = 6,
    HenExecuting = 7,
    HenReplaced = 8,
    HenFailed = 9,
    HenUnresolved = 10,
    RoExecuting = 11,
    RoReplaced = 12,
    RoFailed = 13,
    RoUnresolved = 14,
};

struct DeferredRequest {
    u64 confirmation; // Exact token per command; public opt-in, not a security secret.
    u64 host_pid; // Host application PID, NOT just NRO filename.
};
static_assert(sizeof(DeferredRequest) == 16);

struct DeferredHenPackageRequest {
    u64 confirmation;
    u64 host_pid;
    u64 main_size;
    u8 sha256[32];
};
static_assert(sizeof(DeferredHenPackageRequest) == 56);
static_assert(sizeof(DeferredHenPackageRequest) <= KAIHEN_PLUGIN_MESSAGE_SIZE);

// Exactly one plugin scalar reply; zero-filled for unavailable fields.
// Result fields are the LAST observed non-success status, when applicable.
struct DeferredReport {
    u32 protocol = DeferredProtocolVersion;
    u32 phase = static_cast<u32>(DeferredPhase::Idle);
    Result result = 0;
    u32 ticket = 0;
    u64 host_pid = 0;
    u32 hen_flags = 0;
    u32 host_exited = 0;
    u32 hen_phase = 0;
    u32 kind = 0; // DeferredKind; replaces v1 hen_serial in protocol v2.
    Result hen_result = 0;
    u32 hen_mounts = 0;
    u64 hen_process_id = 0;
    u32 elapsed_ms = 0;
    u32 checks = 0;
};
static_assert(sizeof(DeferredReport) == 64);
static_assert(sizeof(DeferredReport) <= KAIHEN_PLUGIN_MESSAGE_SIZE);

// Read-only stage check before requesting a live update. The launcher itself
// generates the staged exefs.nsp from staged/{hen,ro}/main and signed policy.
struct DeferredHenPreflight {
    u32 protocol = DeferredProtocolVersion;
    Result result = 0;
    u64 main_size = 0;
    u8 sha256[32] = {};
};
static_assert(sizeof(DeferredHenPreflight) == 48);
static_assert(sizeof(DeferredHenPreflight) <= KAIHEN_PLUGIN_MESSAGE_SIZE);

struct DeferredCapabilities {
    u32 protocol = DeferredProtocolVersion;
    u32 live_attempts = 0;
    u32 successful_live = 0;
    u32 max_live = 3;   // Test envelope, not an unlimited restart guarantee.
    u32 may_queue_live = 0;
};
static_assert(sizeof(DeferredCapabilities) <= KAIHEN_PLUGIN_MESSAGE_SIZE);
} // namespace hotupdate
