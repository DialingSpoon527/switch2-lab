// SPDX-License-Identifier: GPL-2.0-only
#include "background_plugin.hpp"
#include "private_sm_lookup.hpp"
#include "../../common/policy_package.hpp"
#include "../../common/package.hpp"
#include "../../common/sd_writer.hpp"
#include "../../common/paced_loader_step.hpp"
#include "../../libkaihen/source/plugin_startup.h"
#include <switch/crypto/sha256.h>
#include <switch/services/pm.h>
#include <switch/services/sm.h>
#include <cstdio>
#include <cstring>

namespace {
using Reader = ounce::RetainedMemory<ounce::MemoryWriter::RoProcess>;
using Gate = ounce::LoaderGate<ounce::MemoryWriter::RoProcess>;
using Mount = ounce::DomainMount<ounce::MemoryWriter::RoProcess>;
using Package = ounce::NativePackage<ounce::MemoryWriter::RoProcess>;
constexpr Result Invalid = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr Result Busy = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
constexpr u64 MaxNso = 64ull * 1024 * 1024;
constexpr u64 PollNs = 1000000;
constexpr u64 LookupTimeoutNs = 5000000000ull;
constexpr u64 AdoptionTimeoutNs = 5000000000ull;
constexpr u64 TerminationTimeoutNs = 5000000000ull;
constexpr u64 ReadyTimeoutNs = 5000000000ull;
constexpr u64 LaunchTimeoutNs = 30000000000ull;

bool pluginNameValid(const char name[HEN_PLUGIN_NAME_SIZE]) {
    bool nonempty = false;
    for (size_t i = 0; i < HEN_PLUGIN_NAME_SIZE; ++i) {
        const char c = name[i];
        if (c == 0) return nonempty;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
        nonempty = true;
    }
    return false; // Missing terminating NUL.
}

void closeOwned(Handle& handle) {
    if (handle != INVALID_HANDLE) svcCloseHandle(handle);
    handle = INVALID_HANDLE;
}
}

void BackgroundPluginManager::waitForWorkers() {
    // HEN normally lives for the boot. Never free the stack used by a blocked
    // migration lookup or a Loader worker that is still running.
    while ((worker_created_ && !worker_finished_.load(std::memory_order_acquire)) ||
           (pm_created_ && !pm_finished_.load(std::memory_order_acquire)) ||
           (lookup_created_ && !lookup_finished_.load(std::memory_order_acquire)) ||
           (adoption_created_ && !adoption_finished_.load(std::memory_order_acquire)) ||
           (termination_created_ && !termination_finished_.load(std::memory_order_acquire)))
        svcSleepThread(1000000000ll);
    reapThreads();
}

BackgroundPluginManager::~BackgroundPluginManager() {
    waitForWorkers();
    closeOwned(shell_);
    closeOwned(sm_proxy_);
    closeOwned(fs_root_);
    closeOwned(plugin_control_);
    closeOwned(plugin_migration_);
    closeOwned(lookup_sm_);
    closeOwned(adoption_release_);
    closeOwned(adoption_ready_);
    closeOwned(adoption_control_);
}

Result BackgroundPluginManager::adoptShell(Handle copied,
                                             const BackgroundPluginCapability& capability) {
    if (shell_ != INVALID_HANDLE) return Busy;
    if (copied == INVALID_HANDLE || !capability.program_id ||
        capability.storage_id != NcmStorageId_BuiltInSystem) return Invalid;
    shell_ = copied;
    policy_ = capability;
    instance_tick_ = armGetSystemTick();
    return 0;
}

void BackgroundPluginManager::report(HenBackgroundPluginPhase phase, Result rc, u32 step) {
    mutexLock(&lock_);
    if (status_.phase != phase)
        phase_started_tick_.store(armGetSystemTick(), std::memory_order_release);
    status_.phase = phase;
    status_.result = rc;
    if (step) status_.gate_step = step;
    status_.mounts = mount_count_;
    if (pm_pid_) status_.pid = pm_pid_;
    mutexUnlock(&lock_);
}

HenBackgroundPluginStatus BackgroundPluginManager::status() const {
    mutexLock(&lock_);
    auto out = status_;
    mutexUnlock(&lock_);
    return out;
}

u32 BackgroundPluginManager::diagnosticFlags() const {
    u32 flags = 0;
    if (shell_ != INVALID_HANDLE) flags |= HenManagerFlag_ShellAvailable;
    if (gate_owned_.load(std::memory_order_acquire)) flags |= HenManagerFlag_GateOwned;
    if (gate_restored_.load(std::memory_order_acquire)) flags |= HenManagerFlag_GateRestored;
    if (active_.load(std::memory_order_acquire)) flags |= HenManagerFlag_Active;
    if (quarantined_.load(std::memory_order_acquire)) flags |= HenManagerFlag_Quarantined;
    if (pending_) flags |= HenManagerFlag_Pending;
    if (pm_dispatched_.load(std::memory_order_acquire)) flags |= HenManagerFlag_PmDispatched;
    if (pm_finished_.load(std::memory_order_acquire)) flags |= HenManagerFlag_PmFinished;
    if (process_started_.load(std::memory_order_acquire)) flags |= HenManagerFlag_ProcessStarted;
    if (terminationActive()) flags |= HenManagerFlag_TerminationActive;
    return flags;
}

void BackgroundPluginManager::quarantine(Result rc) {
    quarantined_.store(true, std::memory_order_release);
    report(HenBackgroundPluginPhase_Quarantined, rc, gate_step_);
}

void BackgroundPluginManager::reapThreads() {
    if (worker_created_ && worker_finished_.load(std::memory_order_acquire)) {
        threadWaitForExit(&worker_);
        threadClose(&worker_);
        worker_created_ = false;
    }
    if (pm_created_ && pm_finished_.load(std::memory_order_acquire)) {
        threadWaitForExit(&pm_thread_);
        threadClose(&pm_thread_);
        pm_created_ = false;
    }
    if (lookup_created_ && lookup_finished_.load(std::memory_order_acquire)) {
        threadWaitForExit(&lookup_thread_);
        threadClose(&lookup_thread_);
        lookup_created_ = false;
    }
    if (adoption_created_ && adoption_finished_.load(std::memory_order_acquire)) {
        threadWaitForExit(&adoption_thread_);
        threadClose(&adoption_thread_);
        adoption_created_ = false;
    }
    if (termination_created_ && termination_finished_.load(std::memory_order_acquire)) {
        threadWaitForExit(&termination_thread_);
        threadClose(&termination_thread_);
        termination_created_ = false;
    }
}

bool BackgroundPluginManager::sameName(const char* name) const {
    return name && std::strncmp(request_.name, name, HEN_PLUGIN_NAME_SIZE) == 0;
}

HenPluginName BackgroundPluginManager::currentName() const {
    HenPluginName result = {};
    mutexLock(&lock_);
    std::memcpy(result.name, request_.name, sizeof(result.name));
    mutexUnlock(&lock_);
    return result;
}

// Invoked only after HEN has validated the exact tracked PID and confirmed
// no PM/Loader launch worker is active. This is an abrupt PM termination:
// no assumption that any arbitrary plugin supports cooperative shutdown.
Result BackgroundPluginManager::terminateExactPid(u64 pid) {
    if (!pid || shell_ == INVALID_HANDLE || active_.load() || pending_ ||
        gate_owned_.load() || quarantined_.load() || terminationActive()) return Busy;
    reapThreads();
    termination_pid_ = pid;
    termination_started_tick_ = armGetSystemTick();
    termination_rc_.store(0, std::memory_order_relaxed);
    termination_finished_.store(false, std::memory_order_release);
    termination_active_.store(true, std::memory_order_release);
    Result rc = threadCreate(&termination_thread_, terminationWorker, this, nullptr,
                             0x4000, 0x2c, -2);
    if (R_SUCCEEDED(rc)) {
        termination_created_ = true;
        rc = threadStart(&termination_thread_);
        if (R_FAILED(rc)) {
            threadClose(&termination_thread_);
            termination_created_ = false;
        }
    }
    if (R_FAILED(rc)) {
        termination_finished_.store(true, std::memory_order_release);
        termination_active_.store(false, std::memory_order_release);
    }
    return rc;
}

void BackgroundPluginManager::terminationWorker(void* ptr) {
    auto* self = static_cast<BackgroundPluginManager*>(ptr);
    Service* shell = pmshellGetServiceSession();
    serviceCreate(shell, self->shell_); // Exclusive while terminationActive().
    const Result rc = pmshellTerminateProcess(self->termination_pid_);
    self->termination_rc_.store(rc, std::memory_order_relaxed);
    self->termination_finished_.store(true, std::memory_order_release);
}

void BackgroundPluginManager::pollTermination() {
    if (!termination_active_.load(std::memory_order_acquire)) return;
    if (termination_finished_.load(std::memory_order_acquire)) {
        // Publish the result before releasing shell ownership. Until the
        // main loop consumes it, no second stop or launch can overwrite it.
        termination_result_pending_.store(true, std::memory_order_release);
        termination_active_.store(false, std::memory_order_release);
        reapThreads();
        return;
    }
    if (!quarantined_.load(std::memory_order_acquire) &&
        armTicksToNs(armGetSystemTick() - termination_started_tick_) >= TerminationTimeoutNs)
        quarantine(KERNELRESULT(TimedOut));
    // An ambiguous PM request retains its thread and shell for the boot.
    // GameArm never shared this worker, so its loop remains available.
}

bool BackgroundPluginManager::takeTerminationResult(u64* pid, Result* result) {
    if (!pid || !result || !termination_result_pending_.load(std::memory_order_acquire))
        return false;
    *pid = termination_pid_;
    *result = termination_rc_.load(std::memory_order_acquire);
    termination_result_pending_.store(false, std::memory_order_release);
    return true;
}

Result BackgroundPluginManager::queue(const HenBackgroundPluginRequest& request,
                                        Handle sm_proxy, Handle fs_root,
                                        const KaihenPluginLaunchContext& startup) {
    if (!canQueue()) return Busy;
    if (!pluginNameValid(request.name) || shell_ == INVALID_HANDLE ||
        sm_proxy == INVALID_HANDLE || fs_root == INVALID_HANDLE) return Invalid;
    bool nonzero = false;
    for (u8 byte : request.sha256) nonzero |= byte != 0;
    if (!nonzero) return Invalid;
    reapThreads();
    request_ = request;
    launch_context_ = startup;
    sm_proxy_ = sm_proxy; // Ownership transferred on success only.
    fs_root_ = fs_root;
    ++serial_;
    pm_pid_ = 0;
    pm_rc_ = 0;
    gate_restored_.store(false, std::memory_order_release);
    last_restore_result_.store(0, std::memory_order_release);
    launch_result_.store(0, std::memory_order_release);
    launch_failure_step_.store(0, std::memory_order_release);
    process_started_.store(false, std::memory_order_release);
    pm_dispatched_.store(false, std::memory_order_release);
    lookup_rc_ = 0;
    mount_count_ = 0;
    gate_step_ = 0;
    mutexLock(&lock_);
    status_ = {};
    status_.serial = serial_;
    status_.phase = HenBackgroundPluginPhase_Queued;
    phase_started_tick_.store(armGetSystemTick(), std::memory_order_release);
    mutexUnlock(&lock_);
    pending_ = true;
    return 0;
}

Result BackgroundPluginManager::cancelPending() {
    if (!pending_ || active_.load() || quarantined_.load() || terminationActive()) return Busy;
    pending_ = false;
    closeOwned(sm_proxy_);
    closeOwned(fs_root_);
    report(HenBackgroundPluginPhase_Cancelled);
    return 0;
}

Result BackgroundPluginManager::startPending(const BackgroundPluginContext& context) {
    if (!pending_ || active_.load() || quarantined_.load() || terminationActive()) return Busy;
    if (context.reader == INVALID_HANDLE || context.fs_process == INVALID_HANDLE ||
        context.loader_root == INVALID_HANDLE || context.fs_root == INVALID_HANDLE ||
        context.monitor == INVALID_HANDLE) return Invalid;
    ctx_ = context;
    pending_ = false;
    worker_finished_.store(false, std::memory_order_release);
    active_.store(true, std::memory_order_release);
    gate_restored_.store(false, std::memory_order_release);
    gate_owned_.store(true, std::memory_order_release); // Before starting worker.
    Result rc = threadCreate(&worker_, worker, this, nullptr, 0x20000, 0x2c, -2);
    if (R_SUCCEEDED(rc)) {
        worker_created_ = true;
        rc = threadStart(&worker_);
        if (R_FAILED(rc)) {
            threadClose(&worker_);
            worker_created_ = false;
        }
    }
    if (R_FAILED(rc)) {
        launch_result_.store(rc, std::memory_order_release);
        report(HenBackgroundPluginPhase_Failed, rc);
        worker_finished_.store(true, std::memory_order_release);
        active_.store(false, std::memory_order_release);
        gate_owned_.store(false, std::memory_order_release);
        closeOwned(sm_proxy_);
        closeOwned(fs_root_);
    }
    return rc;
}

void BackgroundPluginManager::worker(void* ctx) {
    static_cast<BackgroundPluginManager*>(ctx)->execute();
}

void BackgroundPluginManager::pmWorker(void* ctx) {
    auto* self = static_cast<BackgroundPluginManager*>(ctx);
    Service* shell = pmshellGetServiceSession();
    serviceCreate(shell, self->shell_); // Borrow, never close.
    NcmProgramLocation location = {};
    location.program_id = self->policy_.program_id;
    location.storageID = self->policy_.storage_id;
    u64 pid = 0;
    const Result rc = pmshellLaunchProgram(self->policy_.flags, &location, &pid);
    self->pm_pid_ = pid;
    self->pm_rc_ = rc;
    self->pm_finished_.store(true, std::memory_order_release);
}

Result BackgroundPluginManager::verifyAndStage(char out[FS_MAX_PATH]) {
    char input[FS_MAX_PATH];
    std::snprintf(input, sizeof(input), "/kaihen/plugins/%s.nso", request_.name);
    std::snprintf(out, FS_MAX_PATH, "/kaihen/plugin-packages/on-demand-%016llx-%016llx.nso",
                  static_cast<unsigned long long>(instance_tick_),
                  static_cast<unsigned long long>(serial_));
    SdReader source;
    u64 size = 0;
    R_TRY(source.begin(ctx_.fs_root, input, &size));
    if (size < 0x100 || size > MaxNso) return Invalid;
    u8 header[0x100];
    u64 received = 0;
    R_TRY(source.read(header, sizeof(header), &received));
    if (received != sizeof(header) || std::memcmp(header, "NSO0", 4)) return Invalid;
    u32 flags = 0;
    std::memcpy(&flags, header + 12, 4);
    u64 previous_end = sizeof(header);
    for (u32 part = 0; part < 3; ++part) {
        u32 offset = 0, length = 0, packed = 0;
        std::memcpy(&offset, header + 0x10 + part * 0x10, 4);
        std::memcpy(&length, header + 0x18 + part * 0x10, 4);
        std::memcpy(&packed, header + 0x60 + part * 4, 4);
        const u64 on_disk = flags & (1u << part) ? packed : length;
        if (!length || !on_disk || offset < previous_end || offset > size ||
            on_disk > size - offset) return Invalid;
        previous_end = offset + on_disk;
    }
    SdWriter writer;
    R_TRY(writer.begin(ctx_.fs_root, out, size));
    Sha256Context sha;
    sha256ContextCreate(&sha);
    sha256ContextUpdate(&sha, header, sizeof(header));
    R_TRY(writer.write(header, sizeof(header)));
    u8 buffer[4096];
    for (u64 remaining = size - sizeof(header); remaining;) {
        const size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        R_TRY(source.read(buffer, want, &received));
        if (received != want) return Invalid;
        sha256ContextUpdate(&sha, buffer, want);
        R_TRY(writer.write(buffer, want));
        remaining -= want;
    }
    R_TRY(writer.finish());
    u8 hash[32];
    sha256ContextGetHash(&sha, hash);
    if (std::memcmp(hash, request_.sha256, sizeof(hash))) return Invalid;
    // Verify the staged snapshot itself, not just the original source file.
    SdReader stage;
    u64 staged_size = 0;
    R_TRY(stage.begin(ctx_.fs_root, out, &staged_size));
    if (staged_size != size) return Invalid;
    sha256ContextCreate(&sha);
    for (u64 remaining = size; remaining;) {
        const size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        R_TRY(stage.read(buffer, want, &received));
        if (received != want) return Invalid;
        sha256ContextUpdate(&sha, buffer, want);
        remaining -= want;
    }
    sha256ContextGetHash(&sha, hash);
    return std::memcmp(hash, request_.sha256, sizeof(hash)) ? Invalid : 0;
}

Result BackgroundPluginManager::launchPackage(const char* executable, u64* out_pid) {
    *out_pid = 0;
    SignedPolicy policy;
    R_TRY(readSignedPolicy(ctx_.loader_root, policy_.program_id,
                           NcmStorageId_BuiltInSystem, 1, &policy));
    // This path has a fixed prefix plus two 16-digit identifiers. Keep its
    // actual bound visible to the compiler before adding the @Sdcard prefix.
    char package_name[128];
    char package_sd[FS_MAX_PATH];
    const int name_length = std::snprintf(package_name, sizeof(package_name),
                  "/kaihen/plugin-packages/on-demand-%016llx-%016llx.nsp",
                  static_cast<unsigned long long>(instance_tick_),
                  static_cast<unsigned long long>(serial_));
    if (name_length < 0 || static_cast<size_t>(name_length) >= sizeof(package_name)) return Invalid;
    const int sd_length = std::snprintf(package_sd, sizeof(package_sd), "@Sdcard:%s", package_name);
    if (sd_length < 0 || static_cast<size_t>(sd_length) >= sizeof(package_sd)) return Invalid;
    R_TRY(writeExeFsPackage(ctx_.fs_root, executable, package_name, policy));

    Reader memory(ctx_.memory, ctx_.reader, ctx_.fs_process);
    R_TRY(memory.begin());
    ounce::DomainMountConfig config = ctx_.mount;
    config.title = policy_.program_id;
    Package donor(memory, ctx_.fs_root);
    Result rc = donor.open(package_sd, config);
    if (R_FAILED(rc)) {
        donor.close();
        const Result restored = memory.restore();
        last_restore_result_.store(restored, std::memory_order_release);
        if (R_FAILED(restored)) {
            quarantine(restored);
            for (;;) svcSleepThread(1000000000ll);
        }
        gate_restored_.store(true, std::memory_order_release);
        return rc;
    }
    Gate gate(memory, ctx_.gate);
    Mount mount(memory, config);
    bool ever_parked = false;
    bool exchanged = false;
    report(HenBackgroundPluginPhase_Parking);
    rc = gate.inspectForPark();
    if (R_SUCCEEDED(rc)) {
        ever_parked = true; // park() can partially modify the wait handle.
        rc = gate.park();
    }
    gate_step_ = gate.failureStep();
    if (R_SUCCEEDED(rc)) {
        report(HenBackgroundPluginPhase_Launching);
        pm_finished_.store(false, std::memory_order_release);
        Result started = threadCreate(&pm_thread_, pmWorker, this, nullptr, 0x8000, 0x2c, -2);
        if (R_SUCCEEDED(started)) {
            pm_created_ = true;
            started = threadStart(&pm_thread_);
            if (R_SUCCEEDED(started)) pm_dispatched_.store(true, std::memory_order_release);
            else { threadClose(&pm_thread_); pm_created_ = false; }
        }
        rc = started;
        if (R_FAILED(started)) pm_finished_.store(true, std::memory_order_release);
        if (R_SUCCEEDED(rc)) {
            const u64 launch_started = armGetSystemTick();
            while (armTicksToNs(armGetSystemTick() - launch_started) < LaunchTimeoutNs) {
                bool ready = false;
                rc = gate.pending(ready);
                if (R_FAILED(rc)) break;
                if (!ready) {
                    if (pm_finished_.load(std::memory_order_acquire) && R_FAILED(pm_rc_)) {
                        rc = pm_rc_.load(std::memory_order_acquire);
                        break;
                    }
                    svcSleepThread(PollNs);
                    continue;
                }
                ounce::DomainEntry before[ounce::MaxDomainEntries];
                ounce::DomainEntry after[ounce::MaxDomainEntries];
                ounce::DomainEntry added;
                u32 before_count = 0, after_count = 0;
                rc = mount.snapshot(before, before_count);
                if (R_FAILED(rc)) break;
                alignas(16) u8 message[ounce::LoaderRequestSize];
                rc = ounce::pacedLoaderStep(gate, message,
                    [] { return armTicksToNs(armGetSystemTick()); },
                    [](u64 ns) { svcSleepThread(ns); }, KERNELRESULT(TimedOut));
                if (R_FAILED(rc)) break;
                rc = mount.snapshot(after, after_count);
                if (R_FAILED(rc)) break;
                bool expected = false;
                rc = mount.inspectRequest(message, expected);
                if (R_FAILED(rc)) break;
                if (!expected) continue;
                ++mount_count_;
                rc = mount.added(before, before_count, after, after_count, added);
                if (R_FAILED(rc)) break;
                if (mount_count_ == 2) {
                    report(HenBackgroundPluginPhase_Exchanging);
                    rc = mount.exchange(added);
                    exchanged = R_SUCCEEDED(rc);
                    break;
                }
            }
            if (!exchanged && R_SUCCEEDED(rc)) rc = KERNELRESULT(TimedOut);
        }
    }

    // Never release the GameArm gate before *both* original Loader resources
    // have been restored. A PM timeout is not proof that launch did not occur.
    launch_result_.store(rc, std::memory_order_release);
    if (R_FAILED(rc)) launch_failure_step_.store(gate.failureStep(), std::memory_order_release);
    report(HenBackgroundPluginPhase_Restoring);
    const auto restored = pluginmanager::restoreBackgroundGate(
        gate, memory, ever_parked, KERNELRESULT(TimedOut),
        [] { return armTicksToNs(armGetSystemTick()); },
        [](u64 ns) { svcSleepThread(ns); },
        [&](Result restore_result) {
            last_restore_result_.store(restore_result, std::memory_order_release);
            gate_step_ = gate.failureStep();
            // Refresh the recovery step without replacing the primary
            // failure/phase after the operation has entered quarantine.
            mutexLock(&lock_);
            status_.gate_step = gate_step_;
            mutexUnlock(&lock_);
        },
        [&](Result restore_result) { quarantine(R_FAILED(rc) ? rc : restore_result); },
        [&] { gate_restored_.store(true, std::memory_order_release); },
        [&] { gate_owned_.store(false, std::memory_order_release); });
    if (restored.abandoned) {
        // The original restoration has now completed, so games can proceed.
        // Keep ambiguous PM/package ownership alive, and never resume this
        // abandoned operation's monitor, StartProcess or plugin handoff.
        active_.store(false, std::memory_order_release);
        for (;;) svcSleepThread(1000000000ll);
    }

    if (pm_created_) {
        const bool completed = pluginmanager::waitForRestoredBackgroundPm(
            [&] { return pm_finished_.load(std::memory_order_acquire); },
            [] { return armTicksToNs(armGetSystemTick()); },
            [](u64 ns) { svcSleepThread(ns); }, LaunchTimeoutNs, PollNs,
            KERNELRESULT(TimedOut),
            [&](Result timeout) {
                if (R_SUCCEEDED(rc)) launch_result_.store(timeout, std::memory_order_release);
                quarantine(R_FAILED(rc) ? rc : timeout);
            },
            [&] { gate_owned_.store(false, std::memory_order_release); });
        if (!completed) {
            active_.store(false, std::memory_order_release);
            for (;;) svcSleepThread(1000000000ll);
        }
        threadWaitForExit(&pm_thread_);
        threadClose(&pm_thread_);
        pm_created_ = false;
    }
    // IMPORTANT: vanilla bootstrap retains the donor CodeFS object until
    // *after* starting the process and completing the plugin handoff.
    // Do not close it here: a valid PID does not prove the file is unused.
    // Keep the game arm leased until PID identity and PM StartProcess finish;
    // that code borrows HEN's global pm:dmnt service object.
    if (R_SUCCEEDED(rc) && R_FAILED(pm_rc_)) rc = pm_rc_;
    if (R_SUCCEEDED(rc) && (!exchanged || mount_count_ != 2 || !pm_pid_)) rc = Invalid;
    launch_result_.store(rc, std::memory_order_release);
    if (R_FAILED(rc)) {
        if (pm_dispatched_.load(std::memory_order_acquire) || pm_pid_) {
            quarantine(rc);
        } else {
            donor.close(); // No PM consumer ever received this package.
        }
        return rc;
    }
    // Verify exactly the program identity used for the signed package.
    Service* monitor = pmdmntGetServiceSession();
    serviceCreate(monitor, ctx_.monitor); // Borrowed.
    u64 actual = 0;
    rc = pmdmntGetProgramId(&actual, pm_pid_);
    if (R_SUCCEEDED(rc) && actual != policy_.program_id) rc = Invalid;
    if (R_SUCCEEDED(rc)) rc = pmdmntStartProcess(pm_pid_);
    launch_result_.store(rc, std::memory_order_release);
    if (R_FAILED(rc)) { quarantine(rc); return rc; }
    process_started_.store(true, std::memory_order_release);
    *out_pid = pm_pid_;
    gate_owned_.store(false, std::memory_order_release);
    report(HenBackgroundPluginPhase_Started);
    const Result handoff_rc = handOffPlugin(pm_pid_);
    if (R_FAILED(handoff_rc)) {
        launch_result_.store(handoff_rc, std::memory_order_release);
        quarantine(handoff_rc);
        active_.store(false, std::memory_order_release);
        // Ownership of donor/CodeFS can still be ambiguous after start.
        // This worker keeps the package and its own stack alive for the
        // rest of the boot, but does NOT monopolize Loader or GameArm.
        for (;;) svcSleepThread(1000000000ll);
    }
    donor.close();
    return 0;
}

void BackgroundPluginManager::lookupWorker(void* arg) {
    auto* self = static_cast<BackgroundPluginManager*>(arg);
    Handle port = INVALID_HANDLE;
    u32 failed_step = 0;
    const Result rc = pluginmanager::lookupMigrationOnDelegatedSm(
        self->sm_proxy_, &port, &failed_step);
    if (R_SUCCEEDED(rc)) self->plugin_migration_ = port;
    if (R_FAILED(rc)) self->launch_failure_step_.store(failed_step, std::memory_order_release);
    self->lookup_rc_ = rc;
    self->lookup_finished_.store(true, std::memory_order_release);
}

void BackgroundPluginManager::adoptionWorker(void* arg) {
    static_cast<BackgroundPluginManager*>(arg)->adoptHandles();
}

void BackgroundPluginManager::adoptHandles() {
    const u64 pid = pm_pid_.load(std::memory_order_acquire);
    (void)pid; // Used by the additive lifecycle patch.
    Service plugin = {};
    serviceCreate(&plugin, plugin_migration_);
    Handle release = INVALID_HANDLE, ready = INVALID_HANDLE, control = INVALID_HANDLE;
    Result rc = 0;
    launch_context_.process_id = pid;
    launch_context_.sequence = serial_;
    rc = pluginAdoptHandlesWithLifecycle(&plugin, sm_proxy_, fs_root_,
                                         &launch_context_, &release, &ready, &control);
    adoption_release_ = release;
    adoption_ready_ = ready;
    adoption_control_ = control;
    adoption_rc_ = rc;
    adoption_finished_.store(true, std::memory_order_release);
}

Result BackgroundPluginManager::handOffPlugin(u64 pid) {
    (void)pid;
    report(HenBackgroundPluginPhase_WaitingMigration);
    lookup_finished_.store(false, std::memory_order_release);
    Result rc = threadCreate(&lookup_thread_, lookupWorker, this, nullptr, 0x8000, 0x2c, -2);
    if (R_SUCCEEDED(rc)) {
        lookup_created_ = true;
        rc = threadStart(&lookup_thread_);
        if (R_FAILED(rc)) { threadClose(&lookup_thread_); lookup_created_ = false; }
    }
    if (R_FAILED(rc)) {
        lookup_finished_.store(true, std::memory_order_release);
        return rc;
    }
    // A dedicated worker keeps HEN's main IPC loop responsive. The per-plugin
    // proxy uses the existing FS-owned SM master; if the plugin never registers,
    // other service lookups can remain blocked even after this deadline.
    const u64 lookup_started = armGetSystemTick();
    while (!lookup_finished_.load(std::memory_order_acquire) &&
           armTicksToNs(armGetSystemTick() - lookup_started) < LookupTimeoutNs)
        svcSleepThread(PollNs);
    if (!lookup_finished_.load()) {
        // The helper's kernel IPC cannot safely be aborted; quarantine this
        // launch but don't hold Loader or GameArm.
        quarantine(KERNELRESULT(TimedOut));
        return KERNELRESULT(TimedOut);
    }
    threadWaitForExit(&lookup_thread_);
    threadClose(&lookup_thread_);
    lookup_created_ = false;
    closeOwned(lookup_sm_);
    if (R_FAILED(lookup_rc_)) return lookup_rc_;
    if (plugin_migration_ == INVALID_HANDLE) return Invalid;

    report(HenBackgroundPluginPhase_Provisioning);
    adoption_finished_.store(false, std::memory_order_release);
    rc = threadCreate(&adoption_thread_, adoptionWorker, this, nullptr, 0x8000, 0x2c, -2);
    if (R_SUCCEEDED(rc)) {
        adoption_created_ = true;
        rc = threadStart(&adoption_thread_);
        if (R_FAILED(rc)) {
            threadClose(&adoption_thread_);
            adoption_created_ = false;
        }
    }
    if (R_FAILED(rc)) {
        adoption_finished_.store(true, std::memory_order_release);
        return rc;
    }
    const u64 adoption_started = armGetSystemTick();
    while (!adoption_finished_.load(std::memory_order_acquire) &&
           armTicksToNs(armGetSystemTick() - adoption_started) < AdoptionTimeoutNs)
        svcSleepThread(PollNs);
    if (!adoption_finished_.load(std::memory_order_acquire))
        return KERNELRESULT(TimedOut);
    threadWaitForExit(&adoption_thread_);
    threadClose(&adoption_thread_);
    adoption_created_ = false;
    rc = adoption_rc_;
    Handle release = adoption_release_, ready = adoption_ready_, control = adoption_control_;
    adoption_release_ = adoption_ready_ = adoption_control_ = INVALID_HANDLE;
    closeOwned(sm_proxy_);
    closeOwned(fs_root_);
    closeOwned(plugin_migration_);
    if (R_FAILED(rc)) {
        closeOwned(release);
        closeOwned(ready);
        closeOwned(control);
        return rc;
    }
    rc = svcSignalEvent(release);
    closeOwned(release);
    if (R_SUCCEEDED(rc)) {
        report(HenBackgroundPluginPhase_WaitingReady);
        rc = svcWaitSynchronizationSingle(ready, ReadyTimeoutNs);
    }
    closeOwned(ready);
    if (R_FAILED(rc)) {
        closeOwned(control);
        return rc;
    }
    if (control == INVALID_HANDLE) {
        report(HenBackgroundPluginPhase_RunningUnpublished);
        return 0;
    }
    // The worker MUST NOT edit HEN's plugin array: the IPC thread owns it.
    mutexLock(&lock_);
    plugin_control_ = control;
    publish_pending_ = true;
    status_.phase = HenBackgroundPluginPhase_Ready;
    phase_started_tick_.store(armGetSystemTick(), std::memory_order_release);
    mutexUnlock(&lock_);
    return 0;
}

bool BackgroundPluginManager::takePublication(HenPluginName* name, u64* pid, Handle* control) {
    if (!name || !pid || !control) return false;
    mutexLock(&lock_);
    const bool available = publish_pending_.load(std::memory_order_acquire);
    if (available) {
        *name = {};
        std::memcpy(name->name, request_.name, HEN_PLUGIN_NAME_SIZE);
        *pid = pm_pid_;
        *control = plugin_control_;
        plugin_control_ = INVALID_HANDLE;
        publish_pending_ = false;
    }
    mutexUnlock(&lock_);
    return available;
}

void BackgroundPluginManager::publicationResult(Result rc) {
    report(R_SUCCEEDED(rc) ? HenBackgroundPluginPhase_Published :
           HenBackgroundPluginPhase_PublishFailed, rc);
}

void BackgroundPluginManager::noteExit(u64 pid) {
    if (!pid || active_.load(std::memory_order_acquire) ||
        quarantined_.load(std::memory_order_acquire)) return;
    mutexLock(&lock_);
    if (status_.pid == pid &&
        (status_.phase == HenBackgroundPluginPhase_Published ||
         status_.phase == HenBackgroundPluginPhase_RunningUnpublished ||
         status_.phase == HenBackgroundPluginPhase_PublishFailed)) {
        status_.phase = HenBackgroundPluginPhase_Exited;
        phase_started_tick_.store(armGetSystemTick(), std::memory_order_release);
    }
    mutexUnlock(&lock_);
}

void BackgroundPluginManager::execute() {
    report(HenBackgroundPluginPhase_Verifying);
    char executable[FS_MAX_PATH];
    Result rc = verifyAndStage(executable);
    launch_result_.store(rc, std::memory_order_release);
    if (R_SUCCEEDED(rc)) {
        report(HenBackgroundPluginPhase_Packaging);
        u64 pid = 0;
        rc = launchPackage(executable, &pid); // Includes mig:usr handoff.
        launch_result_.store(rc, std::memory_order_release);
    } else {
        // Verification never touched Loader. The main loop can resume games.
        gate_owned_.store(false, std::memory_order_release);
    }
    // Safe preflight/restore errors may return without reaching StartProcess.
    // If Loader restoration was proven (or never entered), the games must
    // still be allowed to run, even if the plugin itself failed.
    if (gate_restored_.load(std::memory_order_acquire) ||
        (!pm_dispatched_.load(std::memory_order_acquire) &&
         mount_count_ == 0 && !quarantined_.load()))
        gate_owned_.store(false, std::memory_order_release);
    if (R_FAILED(rc) && !quarantined_.load()) {
        // A launched process could still hold mig:usr or be running after
        // provision failure. Refuse a second launch until independently proved
        // gone; this is not a drop-in rollback of a running plugin.
        if (pm_pid_) quarantine(rc);
        else report(HenBackgroundPluginPhase_Failed, rc);
    }
    // The proxy clients are owned by the worker until AdoptHandles sends them.
    closeOwned(sm_proxy_);
    closeOwned(fs_root_);
    worker_finished_.store(true, std::memory_order_release);
    active_.store(false, std::memory_order_release);
}
