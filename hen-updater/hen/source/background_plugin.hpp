// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <atomic>
#include <switch.h>
#include "background_recovery.hpp"
#include "../../common/retained_memory.hpp"
#include "../../common/loader_gate.hpp"
#include "../../common/domain_mount.hpp"
#include "../../common/background_plugin.h"
#include "../../libkaihen/include/kaihen/protocol.h"
#include "../../libkaihen/include/kaihen/lifecycle.h"

struct BackgroundPluginContext {
    ounce::RetainedMemoryConfig memory;
    ounce::LoaderGateConfig gate;
    ounce::DomainMountConfig mount;
    Handle reader = INVALID_HANDLE;
    Handle fs_process = INVALID_HANDLE;
    Handle loader_root = INVALID_HANDLE;
    Handle fs_root = INVALID_HANDLE;
    Handle monitor = INVALID_HANDLE;
};

// HEN's IPC thread owns the queue. A dedicated worker owns PM/Loader during
// launch and is the only thread that talks to the plugin's mig:usr startup port.
class BackgroundPluginManager {
public:
    BackgroundPluginManager() = default;
    ~BackgroundPluginManager();
    void waitForWorkers(); // Preserve GameArm/FS/SM dependencies until safe teardown.
    Result adoptShell(Handle copied, const BackgroundPluginCapability& capability);
    bool canQueue() const {
        return pluginmanager::canQueueBackground(
            pending_, active_.load(std::memory_order_acquire),
            publish_pending_.load(std::memory_order_acquire), terminationActive(),
            quarantined_.load(std::memory_order_acquire));
    }
    Result queue(const HenBackgroundPluginRequest& request, Handle sm_proxy, Handle fs_root,
                 const KaihenPluginLaunchContext& startup);
    Result cancelPending(); // Main HEN IPC thread only.
    bool pending() const { return pending_; } // Main HEN IPC thread only.
    bool publicationPending() const { return publish_pending_.load(std::memory_order_acquire); }
    bool sameName(const char* name) const;
    HenPluginName currentName() const;
    Result terminateExactPid(u64 pid);
    // A successful termination call means queued, never confirmed exit.
    void pollTermination(); // Main loop; a stuck worker quarantines the manager.
    bool takeTerminationResult(u64* pid, Result* result);
    bool terminationActive() const {
        return termination_active_.load(std::memory_order_acquire) ||
               termination_result_pending_.load(std::memory_order_acquire);
    }
    bool hasStartedProcess() const { return process_started_.load(std::memory_order_acquire); }
    bool recognizesProgramId(u64 id) const { return shell_ != INVALID_HANDLE && id == policy_.program_id; }
    Result startPending(const BackgroundPluginContext& context);
    bool gateOwned() const { return gate_owned_.load(std::memory_order_acquire); }
    bool active() const { return active_.load(std::memory_order_acquire); }
    bool quarantined() const { return quarantined_.load(std::memory_order_acquire); }
    // Called from HEN's IPC thread. Flags are an observational snapshot: the
    // worker may advance between individual atomic reads.
    u32 diagnosticFlags() const;
    Result lastRestoreResult() const { return last_restore_result_.load(std::memory_order_acquire); }
    Result launchResult() const { return launch_result_.load(std::memory_order_acquire); }
    u32 launchFailureStep() const { return launch_failure_step_.load(std::memory_order_acquire); }
    u64 phaseStartedTick() const { return phase_started_tick_.load(std::memory_order_acquire); }
    HenBackgroundPluginStatus status() const;
    // Called by HEN's own main loop, not from the native worker.
    bool takePublication(HenPluginName* name, u64* pid, Handle* control);
    void publicationResult(Result result);
    void noteExit(u64 pid); // Called by HEN main thread after PM confirms exit.
private:
    static void worker(void* arg);
    static void pmWorker(void* arg);
    static void lookupWorker(void* arg);
    static void adoptionWorker(void* arg);
    static void terminationWorker(void* arg);
    void adoptHandles();
    void execute();
    Result verifyAndStage(char out[FS_MAX_PATH]);
    Result launchPackage(const char* executable, u64* out_pid);
    Result handOffPlugin(u64 pid);
    void report(HenBackgroundPluginPhase phase, Result rc = 0, u32 gate_step = 0);
    void quarantine(Result rc);
    void reapThreads();

    mutable Mutex lock_ = {};
    Thread worker_ = {};
    Thread pm_thread_ = {};
    Thread lookup_thread_ = {};
    Thread adoption_thread_ = {};
    Thread termination_thread_ = {};
    bool worker_created_ = false;
    bool pm_created_ = false;
    bool lookup_created_ = false;
    bool adoption_created_ = false;
    bool termination_created_ = false;
    std::atomic<bool> worker_finished_{true};
    std::atomic<bool> pm_finished_{true};
    std::atomic<bool> lookup_finished_{true};
    std::atomic<bool> adoption_finished_{true};
    std::atomic<bool> termination_finished_{true};
    std::atomic<bool> termination_active_{false};
    std::atomic<bool> termination_result_pending_{false};
    std::atomic<bool> process_started_{false};
    std::atomic<bool> gate_owned_{false};
    std::atomic<bool> gate_restored_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> quarantined_{false};
    bool pending_ = false;
    std::atomic<bool> publish_pending_{false};
    std::atomic<Result> last_restore_result_{0};
    std::atomic<Result> launch_result_{0};
    std::atomic<u32> launch_failure_step_{0};
    std::atomic<u64> phase_started_tick_{0};
    u64 serial_ = 0;
    u64 instance_tick_ = 0;
    HenBackgroundPluginRequest request_ = {};
    HenBackgroundPluginStatus status_ = {};
    BackgroundPluginContext ctx_ = {};
    BackgroundPluginCapability policy_ = {};
    KaihenPluginLaunchContext launch_context_ = {};
    Handle shell_ = INVALID_HANDLE;
    Handle sm_proxy_ = INVALID_HANDLE;
    Handle fs_root_ = INVALID_HANDLE;
    Handle plugin_control_ = INVALID_HANDLE;
    Handle plugin_migration_ = INVALID_HANDLE;
    Handle lookup_sm_ = INVALID_HANDLE;
    std::atomic<Result> pm_rc_{0};
    std::atomic<u64> pm_pid_{0};
    std::atomic<bool> pm_dispatched_{false};
    Result lookup_rc_ = 0;
    Result adoption_rc_ = 0;
    Handle adoption_release_ = INVALID_HANDLE;
    Handle adoption_ready_ = INVALID_HANDLE;
    Handle adoption_control_ = INVALID_HANDLE;
    u64 termination_pid_ = 0;
    u64 termination_started_tick_ = 0;
    std::atomic<Result> termination_rc_{0};
    u32 mount_count_ = 0;
    u32 gate_step_ = 0;
};
