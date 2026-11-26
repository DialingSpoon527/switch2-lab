// SPDX-License-Identifier: GPL-2.0-only
#include "deferred_monitor.hpp"
#include <cstring>
#include <cstdio>
#include <switch/arm/counter.h>
#include <switch/services/pm.h>
#include "../../common/control.h"

namespace hotupdate {
namespace {
constexpr Result Invalid = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr Result Busy = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
constexpr Result ProcessNotFound = MAKERESULT(15, 1); // Launcher convention.
constexpr u32 TimeoutMs = 180000;
constexpr u64 PollNs = 500000000ull;

bool pending(DeferredPhase p) {
    return p == DeferredPhase::WaitingForHostExit ||
           p == DeferredPhase::WaitingForHenIdle;
}
bool executing(DeferredPhase p) {
    return p == DeferredPhase::HenExecuting;
}
} // namespace

Result DeferredMonitor::start() {
    if (started_ || monitor_session_ == INVALID_HANDLE) return Invalid;
    mutexInit(&lock_);
    mutexInit(&pm_ipc_lock_);
    mutexInit(&hen_ipc_lock_);
    mutexInit(&fs_ipc_lock_);
    // The same worker now runs the native candidate/rollback transaction.
    // Give that nested call chain a larger stack than the read-only V1 poller.
    Result rc = threadCreate(&thread_, threadEntry, this, nullptr,
                             0x10000, 0x34, -2);
    if (R_FAILED(rc)) return rc;
    rc = threadStart(&thread_);
    if (R_FAILED(rc)) { threadClose(&thread_); return rc; }
    started_ = true;
    return 0;
}

void DeferredMonitor::stop() {
    if (!started_) return;
    mutexLock(&lock_);
    stop_requested_ = true;
    mutexUnlock(&lock_);
    threadWaitForExit(&thread_);
    threadClose(&thread_);
    started_ = false;
}

Result DeferredMonitor::queueDryRun(const DeferredRequest& req, DeferredReport* out) {
    if (req.confirmation != DeferredDryRunConfirmation) return Invalid;
    return queue(req, DeferredKind::DryRun, out);
}

Result DeferredMonitor::queueHen(const DeferredRequest& req, DeferredReport* out) {
    if (req.confirmation != DeferredHenConfirmation) return Invalid;
    return queue(req, DeferredKind::ReplaceHen, out);
}
Result DeferredMonitor::queueHenPackage(const DeferredHenPackageRequest& req,
                                       DeferredReport* out) {
    if (req.confirmation != DeferredHenConfirmation || !req.main_size) return Invalid;
    return queue({req.confirmation, req.host_pid}, DeferredKind::ReplaceHen, out, &req);
}
Result DeferredMonitor::queue(const DeferredRequest& req, DeferredKind kind,
                              DeferredReport* out, const DeferredHenPackageRequest* expected) {
    if (!out || !started_ || !req.host_pid ||
        (kind != DeferredKind::DryRun && kind != DeferredKind::ReplaceHen))
        return Invalid;
    mutexLock(&lock_);
    const auto phase = static_cast<DeferredPhase>(report_.phase);
    if (pending(phase) || executing(phase) ||
        (kind != DeferredKind::DryRun && (live_attempted_ || live_attempts_ >= MaxLive))) {
        mutexUnlock(&lock_);
        return Busy;
    }
    mutexUnlock(&lock_);

    // The initiating app must currently exist. We inspect the ACTUAL host
    // process, not an NRO file's presence. The magic word is NOT an ACL.
    mutexLock(&pm_ipc_lock_);
    Service* pm = pmdmntGetServiceSession();
    serviceCreate(pm, monitor_session_); // borrowed, never pmdmntExit()
    u64 program = 0;
    Result rc = pmdmntGetProgramId(&program, req.host_pid);
    mutexUnlock(&pm_ipc_lock_);
    if (R_FAILED(rc)) return rc;

    u8 digest[32] = {};
    u64 size = 0;
    if (kind == DeferredKind::ReplaceHen) {
        mutexLock(&fs_ipc_lock_);
        rc = controller_.preflightHen(digest, &size);
        mutexUnlock(&fs_ipc_lock_);
        if (R_FAILED(rc)) return rc;
        if (expected && (size != expected->main_size ||
            std::memcmp(digest, expected->sha256, sizeof(digest)) != 0)) return Invalid;
    }

    mutexLock(&lock_);
    const auto latest_phase = static_cast<DeferredPhase>(report_.phase);
    if (pending(latest_phase) || executing(latest_phase) ||
        (kind != DeferredKind::DryRun && (live_attempted_ || live_attempts_ >= MaxLive))) {
        mutexUnlock(&lock_);
        return Busy;
    }
    const u32 previous = report_.ticket;
    report_ = {};
    report_.ticket = previous + 1u ? previous + 1u : 1u;
    report_.kind = static_cast<u32>(kind);
    report_.phase = static_cast<u32>(DeferredPhase::WaitingForHostExit);
    report_.host_pid = req.host_pid;
    std::memcpy(staged_digest_, digest, sizeof(digest));
    staged_size_ = size;
    consecutive_safe_ = 0;
    start_ticks_ = armGetSystemTick();
    *out = report_;
    mutexUnlock(&lock_);
    return 0;
}

Result DeferredMonitor::status(DeferredReport* out) {
    if (!out) return Invalid;
    mutexLock(&lock_);
    *out = report_;
    if (executing(static_cast<DeferredPhase>(report_.phase)))
        out->elapsed_ms = elapsedMs();
    mutexUnlock(&lock_);
    return 0;
}

Result DeferredMonitor::lastRestart(RestartReport* out) {
    if (!out) return Invalid;
    mutexLock(&lock_);
    *out = restart_report_;
    mutexUnlock(&lock_);
    return 0;
}

Result DeferredMonitor::preflightHen(DeferredHenPreflight* out) {
    return preflight(out);
}
Result DeferredMonitor::preflight(DeferredHenPreflight* out) {
    if (!out) return Invalid;
    *out = {};
    mutexLock(&lock_);
    const bool busy = executing(static_cast<DeferredPhase>(report_.phase));
    mutexUnlock(&lock_);
    if (busy) { out->result = Busy; return 0; }
    if (!mutexTryLock(&fs_ipc_lock_)) {
        out->result = Busy;
        return 0;
    }
    mutexLock(&lock_);
    const bool now_busy = executing(static_cast<DeferredPhase>(report_.phase));
    mutexUnlock(&lock_);
    if (now_busy) out->result = Busy;
    else out->result = controller_.preflightHen(out->sha256, &out->main_size);
    mutexUnlock(&fs_ipc_lock_);
    return 0;
}

Result DeferredMonitor::capabilities(DeferredCapabilities* out) {
    if (!out) return Invalid;
    mutexLock(&lock_);
    const auto phase = static_cast<DeferredPhase>(report_.phase);
    *out = {};
    out->live_attempts = live_attempts_;
    out->successful_live = successful_live_;
    out->max_live = MaxLive;
    out->may_queue_live = started_ && !live_attempted_ &&
        live_attempts_ < MaxLive && !pending(phase) && !executing(phase);
    mutexUnlock(&lock_);
    return 0;
}

Result DeferredMonitor::querySafety(u32* flags) {
    if (!flags) return Invalid;
    mutexLock(&lock_);
    const bool busy = executing(static_cast<DeferredPhase>(report_.phase));
    mutexUnlock(&lock_);
    if (busy) return Busy; // no shared HEN IPC during hot replacement
    if (!mutexTryLock(&hen_ipc_lock_)) return Busy;
    Result rc = controller_.queryActiveHenSafety(flags);
    mutexUnlock(&hen_ipc_lock_);
    return rc;
}

Result DeferredMonitor::cancel(DeferredReport* out) {
    if (!out) return Invalid;
    mutexLock(&lock_);
    if (pending(static_cast<DeferredPhase>(report_.phase)))
        report_.phase = static_cast<u32>(DeferredPhase::Cancelled);
    if (executing(static_cast<DeferredPhase>(report_.phase))) {
        *out = report_;
        mutexUnlock(&lock_);
        return Busy; // once started, cancellation cannot safely undo it
    }
    *out = report_;
    mutexUnlock(&lock_);
    return 0;
}

u32 DeferredMonitor::elapsedMs() const {
    const u64 elapsed_ns = armTicksToNs(armGetSystemTick() - start_ticks_);
    const u64 ms = elapsed_ns / 1000000ull;
    return ms > UINT32_MAX ? UINT32_MAX : static_cast<u32>(ms);
}

void DeferredMonitor::threadEntry(void* opaque) {
    static_cast<DeferredMonitor*>(opaque)->run();
}

void DeferredMonitor::run() {
    for (;;) {
        mutexLock(&lock_);
        const bool stopping = stop_requested_;
        mutexUnlock(&lock_);
        if (stopping) return;
        poll();
        svcSleepThread(PollNs);
    }
}

void DeferredMonitor::poll() {
    mutexLock(&lock_);
    const DeferredReport current = report_;
    const auto phase = static_cast<DeferredPhase>(current.phase);
    if (!pending(phase)) { mutexUnlock(&lock_); return; }
    const u32 elapsed = elapsedMs();
    if (elapsed >= TimeoutMs) {
        report_.elapsed_ms = elapsed;
        report_.phase = static_cast<u32>(DeferredPhase::TimedOut);
        mutexUnlock(&lock_);
        return;
    }
    mutexUnlock(&lock_);

    // Every IPC executes OUTSIDE the report lock, so status/cancel remain
    // responsive. We recheck ticket + phase before committing an outcome.
    u32 next_phase = current.phase;
    Result rc = 0;
    u32 flags = 0;
    HenGameStatus game = {};
    bool host_exited = false;
    if (phase == DeferredPhase::WaitingForHostExit) {
        mutexLock(&pm_ipc_lock_);
        Service* pm = pmdmntGetServiceSession();
        serviceCreate(pm, monitor_session_); // borrowed
        u64 program = 0;
        rc = pmdmntGetProgramId(&program, current.host_pid);
        mutexUnlock(&pm_ipc_lock_);
        if (rc == ProcessNotFound) {
            rc = 0;
            host_exited = true;
            next_phase = static_cast<u32>(DeferredPhase::WaitingForHenIdle);
        } else if (R_FAILED(rc)) {
            next_phase = static_cast<u32>(DeferredPhase::Failed);
        }
    } else {
        mutexLock(&hen_ipc_lock_);
        rc = controller_.queryActiveHenGameStatus(&game);
        if (R_SUCCEEDED(rc)) rc = controller_.queryActiveHenSafety(&flags);
        mutexUnlock(&hen_ipc_lock_);
        if (R_FAILED(rc)) next_phase = static_cast<u32>(DeferredPhase::Failed);
    }

    bool execute_live = false;
    u8 expected_digest[32] = {};
    u64 expected_size = 0;
    mutexLock(&lock_);
    if (report_.ticket != current.ticket || report_.phase != current.phase) {
        mutexUnlock(&lock_);
        return; // cancelled or replaced during IPC
    }
    report_.checks++;
    report_.elapsed_ms = elapsedMs();
    if (phase == DeferredPhase::WaitingForHostExit) {
        if (host_exited) report_.host_exited = 1;
    } else if (R_SUCCEEDED(rc)) {
        report_.hen_flags = flags;
        report_.hen_phase = static_cast<u32>(game.phase);
        report_.hen_result = game.result;
        report_.hen_mounts = game.mounts;
        report_.hen_process_id = game.process_id;
        const bool safe = (flags & 1u) && !(flags & 2u) &&
            game.phase == HenGamePhase_WaitingForGame && game.result == 0 &&
            game.mounts == 0 && game.process_id == 0;
        consecutive_safe_ = safe ? consecutive_safe_ + 1u : 0u;
        if (consecutive_safe_ >= 2) {
            if (report_.kind == static_cast<u32>(DeferredKind::ReplaceHen)) {
                // Latch this attempt. Only a verified healthy replacement
                // clears it, and the boot still has a three-attempt limit.
                live_attempted_ = true;
                ++live_attempts_;
                execute_live = true;
                std::memcpy(expected_digest, staged_digest_, 32);
                expected_size = staged_size_;
                next_phase = static_cast<u32>(DeferredPhase::HenExecuting);
            } else {
                next_phase = static_cast<u32>(DeferredPhase::DryRunPassed);
            }
        }
    }
    if (R_FAILED(rc)) report_.result = rc;
    report_.phase = next_phase;
    mutexUnlock(&lock_);

    if (!execute_live) return;

    // One durable HEN-only BEGIN/END pair, never written during Loader interception.
    RestartReport restart = {};
    u8 actual_digest[32] = {};
    u64 actual_size = 0;
    char checkpoint[384] = {};
    const int begin_length = std::snprintf(checkpoint, sizeof(checkpoint),
        "KAIHEN_HEN_ONLY_V1 ticket=%u host=%llu phase=BEGIN staged_size=%llu\n",
        current.ticket, static_cast<unsigned long long>(current.host_pid),
        static_cast<unsigned long long>(expected_size));
    mutexLock(&fs_ipc_lock_);
    Result replace_rc = Invalid;
    if (begin_length > 0 && static_cast<size_t>(begin_length) < sizeof(checkpoint))
        replace_rc = controller_.writeHenTicketCheckpoint(
            current.ticket, checkpoint, static_cast<size_t>(begin_length));
    if (R_SUCCEEDED(replace_rc))
        replace_rc = controller_.preflightHen(actual_digest, &actual_size);
    if (R_SUCCEEDED(replace_rc) && (actual_size != expected_size ||
        std::memcmp(actual_digest, expected_digest, 32) != 0))
        replace_rc = Invalid;
    if (R_SUCCEEDED(replace_rc)) {
        mutexLock(&hen_ipc_lock_);
        replace_rc = controller_.replaceHen(&restart, expected_digest, expected_size);
        mutexUnlock(&hen_ipc_lock_);
    }
    if (R_SUCCEEDED(replace_rc) &&
        restart.phase != RestartPhase::RunningReplacement)
        replace_rc = Invalid;
    const int end_length = std::snprintf(checkpoint, sizeof(checkpoint),
        "KAIHEN_HEN_ONLY_V1 ticket=%u host=%llu phase=END result=%08x "
        "restart_phase=%u recovery=%08x old_pid=%llu "
        "new_pid=%llu generation=%llu old_stopped=%u\n",
        current.ticket, static_cast<unsigned long long>(current.host_pid),
        replace_rc, static_cast<unsigned>(restart.phase), restart.recovery,
        static_cast<unsigned long long>(restart.previous_pid),
        static_cast<unsigned long long>(restart.new_pid),
        static_cast<unsigned long long>(restart.generation), restart.old_hen_stopped);
    if (restart.phase != RestartPhase::Unresolved && end_length > 0 &&
        static_cast<size_t>(end_length) < sizeof(checkpoint)) {
        (void)controller_.writeHenTicketCheckpoint(
            current.ticket, checkpoint, static_cast<size_t>(end_length));
    }
    mutexUnlock(&fs_ipc_lock_);
    if (R_FAILED(replace_rc) && R_SUCCEEDED(restart.result))
        restart.result = replace_rc;

    // Only allow a further update if the NEW HEN answers through the
    // supervisor session and reports the idle safe state. An unresponsive
    // handoff permanently locks further native changes for this boot.
    bool repeat_ready = false;
    if (R_SUCCEEDED(replace_rc) &&
        restart.phase == RestartPhase::RunningReplacement) {
        for (u32 attempt = 0; attempt < 12; ++attempt) {
            u32 latest_flags = 0;
            mutexLock(&hen_ipc_lock_);
            const Result health = controller_.queryActiveHenSafety(&latest_flags);
            mutexUnlock(&hen_ipc_lock_);
            if (R_SUCCEEDED(health) && (latest_flags & 1u) &&
                !(latest_flags & 2u)) {
                repeat_ready = true;
                break;
            }
            svcSleepThread(250000000ull);
        }
    }

    mutexLock(&lock_);
    restart_report_ = restart;
    report_.result = replace_rc;
    report_.elapsed_ms = elapsedMs();
    if (R_SUCCEEDED(replace_rc) &&
        restart.phase == RestartPhase::RunningReplacement) {
        report_.phase = static_cast<u32>(DeferredPhase::HenReplaced);
        ++successful_live_;
        if (repeat_ready) live_attempted_ = false;
    }
    else if (restart.phase == RestartPhase::Unresolved)
        report_.phase = static_cast<u32>(DeferredPhase::HenUnresolved);
    else
        report_.phase = static_cast<u32>(DeferredPhase::HenFailed);
    mutexUnlock(&lock_);
}
} // namespace hotupdate
