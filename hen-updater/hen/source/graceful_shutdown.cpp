// SPDX-License-Identifier: GPL-2.0-only
#include "graceful_shutdown.hpp"

namespace {
constexpr Result Busy = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
constexpr Result Invalid = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr u32 MaxGraceMs = 10000;
}

GracefulShutdown::~GracefulShutdown() {
    waitForWorker();
}

void GracefulShutdown::waitForWorker() {
    // HEN normally persists for the rest of the boot. Never free a worker's
    // stack or its copied session while kernel IPC is still outstanding.
    while (worker_created_ && !finished_.load(std::memory_order_acquire))
        svcSleepThread(1000000000ll);
    reapCompleted();
}

void GracefulShutdown::reapCompleted() {
    if (worker_created_ && finished_.load(std::memory_order_acquire)) {
        threadWaitForExit(&worker_);
        threadClose(&worker_);
        worker_created_ = false;
        if (control_copy_ != INVALID_HANDLE) {
            svcCloseHandle(control_copy_);
            control_copy_ = INVALID_HANDLE;
        }
    }
    if (occupied_ && stop_dispatched_ && !worker_created_) {
        // PM dispatch has its own exact-PID result/exit tracking. Once this
        // IPC has unwound, a failed forced stop must not monopolize the only
        // graceful worker for the remainder of the boot.
        occupied_ = false;
        stop_dispatched_ = false;
        pid_ = 0;
    }
}

Result GracefulShutdown::request(u64 pid, Handle control, u32 reason, u32 grace_ms) {
    reapCompleted();
    if (occupied_) return Busy;
    if (!pid || control == INVALID_HANDLE ||
        (reason != KaihenPluginShutdown_UserStop &&
         reason != KaihenPluginShutdown_Restart &&
         reason != KaihenPluginShutdown_Update)) return Invalid;
    if (grace_ms > MaxGraceMs) grace_ms = MaxGraceMs;
    if (!grace_ms) grace_ms = 2500;
    // Horizon has no svcDuplicateHandle. Consume HEN's private control
    // endpoint after successful startup, retaining it through blocked IPC.
    control_copy_ = control;
    pid_ = pid;
    started_tick_ = armGetSystemTick();
    req_ = {KAIHEN_PLUGIN_LIFECYCLE_VERSION, sizeof(KaihenPluginShutdownRequest),
            reason, grace_ms, ++sequence_};
    stop_dispatched_ = false;
    finished_.store(false, std::memory_order_release);
    result_.store(0, std::memory_order_release);
    Result rc = threadCreate(&worker_, worker, this, nullptr, 0x4000, 0x2c, -2);
    if (R_SUCCEEDED(rc)) {
        worker_created_ = true;
        rc = threadStart(&worker_);
        if (R_FAILED(rc)) {
            threadClose(&worker_);
            worker_created_ = false;
        }
    }
    if (R_FAILED(rc)) {
        finished_.store(true, std::memory_order_release);
        control_copy_ = INVALID_HANDLE;
        pid_ = 0;
        return rc;
    }
    occupied_ = true;
    return 0;
}

void GracefulShutdown::worker(void* ptr) {
    auto* self = static_cast<GracefulShutdown*>(ptr);
    Service service = {};
    serviceCreate(&service, self->control_copy_); // this worker owns the endpoint
    // This dispatch may block indefinitely in a buggy old plugin; deadline is
    // enforced by HEN's main thread independently, never by joining here.
    const Result rc = serviceDispatchIn(&service,
        KAIHEN_PLUGIN_CONTROL_REQUEST_SHUTDOWN, self->req_);
    self->result_.store(rc, std::memory_order_release);
    self->finished_.store(true, std::memory_order_release);
}

bool GracefulShutdown::shouldForce() const {
    if (!occupied_ || stop_dispatched_) return false;
    if (finished_.load(std::memory_order_acquire) &&
        R_FAILED(result_.load(std::memory_order_acquire))) return true;
    const u64 elapsed_ns = armTicksToNs(armGetSystemTick() - started_tick_);
    return elapsed_ns >= static_cast<u64>(req_.grace_period_ms) * 1000000ull;
}

void GracefulShutdown::finishIfExited(u64 exited_pid) {
    if (!occupied_ || exited_pid != pid_) return;
    // May be called when the plugin has exited but kernel IPC hasn't unwound.
    // Leave the worker memory and session alive until it actually finishes.
    if (!finished_.load(std::memory_order_acquire)) return;
    reapCompleted();
    occupied_ = false;
    stop_dispatched_ = false;
    pid_ = 0;
}
