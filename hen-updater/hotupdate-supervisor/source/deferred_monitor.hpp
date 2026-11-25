// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <switch.h>
#include "deferred_protocol.hpp"
#include "restart_controller.hpp"

namespace hotupdate {
// One-shot deferred monitor. The only live replacement entry point is reached
// after host exit, two verified safe snapshots and a second staged-file hash.
class DeferredMonitor {
public:
    DeferredMonitor(Controller& controller, Handle monitor_session)
        : controller_(controller), monitor_session_(monitor_session) {}
    Result start();
    void stop();
    Result queueDryRun(const DeferredRequest& request, DeferredReport* out);
    Result queueHen(const DeferredRequest& request, DeferredReport* out);
    Result queueHenPackage(const DeferredHenPackageRequest& request, DeferredReport* out);
    Result status(DeferredReport* out);
    Result lastRestart(RestartReport* out);
    Result preflightHen(DeferredHenPreflight* out);
    Result cancel(DeferredReport* out);
    Result querySafety(u32* flags);
    Result capabilities(DeferredCapabilities* out);
private:
    Result preflight(DeferredHenPreflight* out);
    Result queue(const DeferredRequest& request, DeferredKind kind,
                 DeferredReport* out, const DeferredHenPackageRequest* expected = nullptr);
    static void threadEntry(void* opaque);
    void run();
    void poll();
    u32 elapsedMs() const;
    Controller& controller_;
    Handle monitor_session_;
    Mutex lock_ = {};
    Mutex pm_ipc_lock_ = {};
    Mutex hen_ipc_lock_ = {};
    Mutex fs_ipc_lock_ = {};
    Thread thread_ = {};
    DeferredReport report_ = {};
    RestartReport restart_report_ = {};
    u8 staged_digest_[32] = {};
    u64 staged_size_ = 0;
    u64 start_ticks_ = 0;
    u32 consecutive_safe_ = 0;
    bool stop_requested_ = false;
    bool started_ = false;
    // Fail-closed: only a successful operation with an independently healthy
    // HEN can clear the latch for another request. Failure stays locked.
    bool live_attempted_ = false;
    u32 live_attempts_ = 0;
    u32 successful_live_ = 0;
    static constexpr u32 MaxLive = 3;
};
} // namespace hotupdate
