// SPDX-License-Identifier: GPL-2.0-only
#include "launch_backend.hpp"
#include <cstring>
#include <switch/arm/counter.h>
#include <switch/services/pm.h>
#include "../../common/domain_mount.hpp"
#include "../../common/loader_gate.hpp"
#include "../../common/paced_loader_step.hpp"
#include "../../common/package.hpp"
#include "../../common/policy_package.hpp"

namespace hotupdate {
namespace {
constexpr Result Invalid = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr Result Busy = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
constexpr u64 PollNs = 1000000;
constexpr u32 MountPollLimit = 30000;
constexpr u32 RestoreRetries = 4;
constexpr u64 WorkerTimeoutNs = 35000000000ull;
using Memory = ounce::RetainedMemory<ounce::MemoryWriter::RoProcess>;
using Gate = ounce::LoaderGate<ounce::MemoryWriter::RoProcess>;
using Mount = ounce::DomainMount<ounce::MemoryWriter::RoProcess>;
using Package = ounce::NativePackage<ounce::MemoryWriter::RoProcess>;
}

void Launcher::pmEntry(void* argument) {
    PMWorker* w = static_cast<PMWorker*>(argument);
    Service* shell = pmshellGetServiceSession();
    serviceCreate(shell, w->shell); // Borrowed kernel handle; no pmshellExit.
    NcmProgramLocation location = {};
    location.program_id = w->program_id;
    location.storageID = w->storage;
    u64 pid = 0;
    const Result result = pmshellLaunchProgram(w->flags, &location, &pid);
    w->pid = pid;
    w->result = result;
    __atomic_store_n(&w->completed, 1u, __ATOMIC_RELEASE);
}

void Launcher::cleanupWorker(bool finished) {
    if (!pm_.started || !finished) return;
    threadWaitForExit(&pm_.thread);
    threadClose(&pm_.thread);
    pm_.started = false;
}

Result Launcher::launch(const char* candidate_main, const char* package_file,
                        const char* package_sd_path, LaunchReport* out,
                        const u8 expected_digest[32], u64 expected_size) {
    if (!out || !candidate_main || !package_file || !package_sd_path ||
        !expected_digest || !expected_size)
        return Invalid;
    *out = {};
    // Strict SD write boundary. Never permit a future caller to turn this
    // launcher into a writer of saves, installed homebrew or system content.
    const bool hen_path =
        std::strcmp(candidate_main, "/kaihen/hotupdate/staged/hen/main") == 0 &&
        std::strcmp(package_file, "/kaihen/hotupdate/staged/hen/exefs.nsp") == 0 &&
        std::strcmp(package_sd_path, "@Sdcard:/kaihen/hotupdate/staged/hen/exefs.nsp") == 0;
    if (!hen_path) return Invalid; // RO and probe launches belong to a separate updater.
    if (busy_ || locked_) return Busy;
    if (geometry_.version != KaihenSupervisorLaunchGeometryVersion ||
        !geometry_.program_id || escrow_.loader_root == INVALID_HANDLE ||
        escrow_.fs_process == INVALID_HANDLE ||
        escrow_.ro_reader == INVALID_HANDLE ||
        escrow_.shell_session == INVALID_HANDLE ||
        escrow_.monitor_session == INVALID_HANDLE ||
        fs_root_ == INVALID_HANDLE)
        return Invalid;
    busy_ = true;
    Result rc = 0;
    // Signed policy comes from installed system migration title; no forged
    // NPDM. New candidate is always inside /kaihen/hotupdate/staged.
    out->phase = LaunchPhase::CandidatePackage;
    SignedPolicy policy = {};
    rc = readSignedPolicy(escrow_.loader_root, geometry_.program_id,
                          NcmStorageId_BuiltInSystem, 1, &policy);
    if (R_SUCCEEDED(rc))
        rc = writeExeFsPackage(fs_root_, candidate_main, package_file, policy,
                               expected_digest, expected_size);
    if (R_FAILED(rc)) goto failed;
    {
        ounce::RetainedMemoryConfig m = {};
        m.storage = handles_.storage;
        m.data_offset = geometry_.data_offset;
        m.pointer_offset = geometry_.pointer_offset;
        Memory memory(m, escrow_.ro_reader, escrow_.fs_process);
        rc = memory.begin();
        if (R_FAILED(rc)) goto failed;
        ounce::LoaderGateConfig gate_config = {};
        gate_config.server = geometry_.gate_server;
        gate_config.vptr = geometry_.gate_vptr;
        gate_config.domain = geometry_.gate_domain;
        gate_config.metadata = geometry_.gate_metadata;
        gate_config.request = geometry_.gate_request;
        std::memcpy(gate_config.waiters, geometry_.gate_waiters,
                    sizeof(gate_config.waiters));
        std::memcpy(gate_config.workers, handles_.workers,
                    sizeof(gate_config.workers));
        gate_config.receive = geometry_.gate_receive;
        gate_config.original_wait = geometry_.gate_original_wait;
        gate_config.park = geometry_.gate_park;
        gate_config.server_copy = handles_.server_copy;
        gate_config.event_read = handles_.event_read;
        gate_config.event_write = handles_.event_write;
        Gate gate(memory, gate_config);
        ounce::DomainMountConfig mount_config = {};
        mount_config.table = geometry_.mount_table;
        mount_config.domain = geometry_.mount_domain;
        mount_config.root_id = geometry_.mount_root_id;
        mount_config.title = geometry_.program_id;
        Package package(memory, fs_root_);
        out->phase = LaunchPhase::OpenPackage;
        rc = package.open(package_sd_path, mount_config);
        if (R_FAILED(rc)) { package.close(); goto failed; } // no gate touched
        {
            Mount mount(memory, mount_config);
            out->phase = LaunchPhase::ParkGate;
            rc = gate.park();
            if (R_SUCCEEDED(rc)) {
                // PM launch runs on its own thread, just like bootstrap. It
                // may block until the package exchange is delivered.
                out->phase = LaunchPhase::LaunchPM;
                pm_.shell = escrow_.shell_session;
                pm_.program_id = geometry_.program_id;
                pm_.flags = geometry_.flags;
                pm_.storage = static_cast<u8>(geometry_.storage_id);
                pm_.pid = 0; pm_.result = 0;
                __atomic_store_n(&pm_.completed, 0u, __ATOMIC_RELAXED);
                rc = threadCreate(&pm_.thread, pmEntry, &pm_, nullptr,
                                  0x4000, 0x2c, -2);
                if (R_SUCCEEDED(rc)) {
                    rc = threadStart(&pm_.thread);
                    if (R_SUCCEEDED(rc)) pm_.started = true;
                    else threadClose(&pm_.thread);
                }
                if (R_SUCCEEDED(rc)) {
                    out->phase = LaunchPhase::ExchangeMount;
                    for (u32 i = 0; i < MountPollLimit; ++i) {
                        bool ready = false;
                        rc = gate.pending(ready);
                        if (R_FAILED(rc)) break;
                        if (!ready) {
                            if (__atomic_load_n(&pm_.completed, __ATOMIC_ACQUIRE) &&
                                R_FAILED(pm_.result)) { rc = pm_.result; break; }
                            svcSleepThread(PollNs);
                            continue;
                        }
                        ounce::DomainEntry before[ounce::MaxDomainEntries],
                            after[ounce::MaxDomainEntries], added = {};
                        u32 before_count = 0, after_count = 0;
                        rc = mount.snapshot(before, before_count);
                        if (R_FAILED(rc)) break;
                        alignas(16) u8 request[ounce::LoaderRequestSize] = {};
                        rc = ounce::pacedLoaderStep(gate, request,
                            [] { return armTicksToNs(armGetSystemTick()); },
                            [](u64 ns) { svcSleepThread(ns); }, KERNELRESULT(TimedOut));
                        if (R_FAILED(rc)) break;
                        rc = mount.snapshot(after, after_count);
                        if (R_FAILED(rc)) break;
                        bool target = false;
                        rc = mount.inspectRequest(request, target);
                        if (R_FAILED(rc)) break;
                        if (!target) continue;
                        out->mounts++;
                        rc = mount.added(before, before_count, after,
                                         after_count, added);
                        if (R_FAILED(rc)) break;
                        if (out->mounts == 2) {
                            rc = mount.exchange(added);
                            break;
                        }
                    }
                    if (out->mounts != 2 && R_SUCCEEDED(rc)) rc = Invalid;
                }
            }
            out->phase = LaunchPhase::RestoreGate;
            out->restore_result = gate.restore();
            for (u32 i = 0; i < RestoreRetries &&
                 out->restore_result == KERNELRESULT(TimedOut) &&
                 gate.pendingRestoreWithReceipt(); ++i) {
                out->restore_result = gate.restore();
            }
            out->restore_pending = gate.pendingRestoreWithReceipt() ? 1 : 0;
            if (R_FAILED(out->restore_result) || out->restore_pending) {
                // The remote gate may still be changed. Never try another
                // launch, and don't close the donor package here.
                locked_ = true;
                out->phase = LaunchPhase::UnresolvedGate;
                if (R_SUCCEEDED(rc)) rc = out->restore_result;
                if (R_SUCCEEDED(rc)) rc = Invalid;
                goto failed;
            }
        }
        // The remote gate is restored, so finishing the PM worker can no
        // longer obstruct system Loader. Wait only bounded time.
        out->phase = LaunchPhase::AwaitPM;
        if (pm_.started) {
            for (u64 t = 0; t < WorkerTimeoutNs &&
                 !__atomic_load_n(&pm_.completed, __ATOMIC_ACQUIRE); t += PollNs)
                svcSleepThread(PollNs);
            if (!__atomic_load_n(&pm_.completed, __ATOMIC_ACQUIRE)) {
                locked_ = true;
                out->phase = LaunchPhase::UnresolvedWorker;
                rc = Invalid;
                goto failed;
            }
            cleanupWorker(true);
            out->pm_result = pm_.result;
            out->pid = pm_.pid;
            if (R_SUCCEEDED(rc)) rc = pm_.result;
        }
        package.close();
    }
    if (R_FAILED(rc) || !out->pid || out->mounts != 2) {
        if (R_SUCCEEDED(rc)) rc = Invalid;
        goto failed;
    }
    out->phase = LaunchPhase::StartProcess;
    {
        Service* monitor = pmdmntGetServiceSession();
        serviceCreate(monitor, escrow_.monitor_session);
        u64 program_id = 0;
        rc = pmdmntGetProgramId(&program_id, out->pid);
        if (R_SUCCEEDED(rc) && program_id != geometry_.program_id) rc = Invalid;
        if (R_SUCCEEDED(rc)) rc = pmdmntStartProcess(out->pid);
    }
    if (R_FAILED(rc)) goto failed;
    out->phase = LaunchPhase::Completed;
    out->result = 0;
    busy_ = false;
    return 0;
failed:
    out->result = rc;
    if (!locked_) out->phase = LaunchPhase::FailedRestored;
    busy_ = false;
    return rc;
}
} // namespace hotupdate
