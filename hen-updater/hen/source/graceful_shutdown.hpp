// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <atomic>
#include <switch.h>
#include "../../libkaihen/include/kaihen/lifecycle.h"

// One bounded, non-blocking HEN->plugin lifecycle request. HEN's main thread
// never waits synchronously for arbitrary plugin code. The per-request IPC
// worker can outlive its deadline and must keep its owned session alive.
class GracefulShutdown {
public:
    ~GracefulShutdown();
    void waitForWorker();
    bool occupied() const { return occupied_; }
    bool pending() const { return occupied_ && !stop_dispatched_; }
    bool shouldForce() const;
    u64 pid() const { return pid_; }
    // Takes ownership of plugin_control on success ONLY. The caller clears
    // its published control slot then; failed startup leaves it untouched.
    Result request(u64 pid, Handle plugin_control, u32 reason, u32 grace_ms);
    void noteForceDispatched() { stop_dispatched_ = true; }
    void finishIfExited(u64 exited_pid);
    void reapCompleted();
private:
    static void worker(void* ptr);
    Thread worker_ = {};
    bool worker_created_ = false;
    bool occupied_ = false;
    bool stop_dispatched_ = false;
    u64 pid_ = 0;
    u64 started_tick_ = 0;
    u64 sequence_ = 0;
    Handle control_copy_ = INVALID_HANDLE;
    KaihenPluginShutdownRequest req_ = {};
    std::atomic<bool> finished_{true};
    std::atomic<Result> result_{0};
};
