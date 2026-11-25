// SPDX-License-Identifier: GPL-2.0-only
#include "restart_controller.hpp"
#include <cstring>
#include <cstdio>
extern "C" {
#include <switch/crypto/sha256.h>
}
#include <switch/arm/counter.h>
#include "../../common/sd_writer.hpp"
#include "../../common/control.h"
#include "../../ro/source/wire.h"
#include "../../common/retained_memory.hpp"
#include "../../common/ro_dmnt.h"

namespace hotupdate {
namespace {
constexpr Result Invalid = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr char CandidateMain[] = "/kaihen/hotupdate/staged/hen/main";
constexpr char CandidatePackage[] = "/kaihen/hotupdate/staged/hen/exefs.nsp";
constexpr char PackageSd[] = "@Sdcard:/kaihen/hotupdate/staged/hen/exefs.nsp";
}
Result Controller::queryActiveHenSafety(u32* flags) const {
    if (!flags || active_supervisor_ == INVALID_HANDLE || unresolved_)
        return Invalid;
    Service hen = {};
    serviceCreate(&hen, active_supervisor_);
    return henQuerySupervisorQuiescence(&hen, flags);
}
Result Controller::queryActiveHenGameStatus(HenGameStatus* out) const {
    if (!out || active_supervisor_ == INVALID_HANDLE || unresolved_)
        return Invalid;
    Service hen = {};
    serviceCreate(&hen, active_supervisor_);
    return henGetGameStatus(&hen, out);
}

Result Controller::preflightCandidate(const char* path, u8 digest[32], u64* main_size) const {
    if (!digest || !main_size || fs_root_ == INVALID_HANDLE ||
        unresolved_ || !launcher_.available()) return Invalid;
    *main_size = 0;
    std::memset(digest, 0, 32);
    SdReader reader;
    u64 size = 0;
    Result rc = reader.begin(fs_root_, path, &size);
    if (R_FAILED(rc)) return rc;
    // Deliberately narrow to a plausible uncompressed HEN NSO. This is
    // not a signature check: trusted operator supplies the candidate.
    if (size < 16384u || size > 4u * 1024u * 1024u) return Invalid;
    Sha256Context sha = {};
    sha256ContextCreate(&sha);
    u8 bytes[4096] = {};
    u64 consumed = 0;
    while (consumed < size) {
        const size_t want = static_cast<size_t>(
            (size - consumed) < sizeof(bytes) ? (size - consumed) : sizeof(bytes));
        u64 count = 0;
        rc = reader.read(bytes, want, &count);
        if (R_FAILED(rc)) return rc;
        if (count != want) return Invalid;
        if (consumed == 0 && std::memcmp(bytes, "NSO0", 4) != 0)
            return Invalid;
        sha256ContextUpdate(&sha, bytes, static_cast<size_t>(count));
        consumed += count;
    }
    sha256ContextGetHash(&sha, digest);
    *main_size = size;
    return 0;
}

Result Controller::preflightHen(u8 digest[32], u64* main_size) const {
    return preflightCandidate(CandidateMain, digest, main_size);
}
Result Controller::writeHenCheckpoint(const char* message, size_t size) const {
    if (!message || !size || size > 512 || fs_root_ == INVALID_HANDLE)
        return Invalid;
    SdWriter writer;
    Result rc = writer.begin(fs_root_,
        "/kaihen/hotupdate/deferred-transaction.txt", size);
    if (R_SUCCEEDED(rc)) rc = writer.write(message, size);
    if (R_SUCCEEDED(rc)) rc = writer.finish();
    return rc;
}

Result Controller::writeHenTicketCheckpoint(u32 ticket,
                                         const char* message, size_t size) const {
    if (!message || !size || size > 512 || !ticket || fs_root_ == INVALID_HANDLE)
        return Invalid;
    char path[112] = {};
    const int len = std::snprintf(path, sizeof(path),
        "/kaihen/hotupdate/transaction-%u-hen.txt", ticket);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(path)) return Invalid;
    SdWriter writer;
    Result rc = writer.begin(fs_root_, path, size);
    if (R_SUCCEEDED(rc)) rc = writer.write(message, size);
    if (R_SUCCEEDED(rc)) rc = writer.finish();
    return rc;
}
Controller::~Controller() {
    if (owns_active_supervisor_ && active_supervisor_ != INVALID_HANDLE)
        svcCloseHandle(active_supervisor_);
}
Result Controller::serviceLookup(const char* name, Handle* out) {
    *out = INVALID_HANDLE;
    TipcService root = {};
    tipcCreate(&root, handles_.sm_master);
    const SmServiceName service_name = smEncodeName(name);
    return tipcDispatchIn(&root, 1, service_name,
        .out_handle_attrs = {SfOutHandleAttr_HipcMove},
        .out_handles = out);
}
Result Controller::connectMigratedHen(Handle* out) {
    // Direct FS-owned SM root is copied by bootstrap. The normal plugin SM
    // override goes through OLD HEN, so it is explicitly not used here.
    return serviceLookup("mig:usr", out);
}
Result Controller::configureNewHen(Handle new_control, Handle* supervisor) {
    Service hen = {};
    serviceCreate(&hen, new_control);
    Result rc = henAdoptFilesystemRoot(&hen, fs_root_);
    if (R_SUCCEEDED(rc)) rc = henAdoptServiceManager(&hen, handles_.sm_master);
    if (R_SUCCEEDED(rc)) rc = henCreateSupervisorSession(&hen, supervisor);
    if (R_SUCCEEDED(rc)) rc = henReleaseControlPort(&hen);
    if (R_FAILED(rc)) return rc;

    HenGameConfig config = {};
    config.data_offset = geometry_.data_offset;
    config.pointer_offset = geometry_.pointer_offset;
    config.server = geometry_.gate_server;
    config.vptr = geometry_.gate_vptr;
    config.domain = geometry_.gate_domain;
    config.metadata = geometry_.gate_metadata;
    config.request = geometry_.gate_request;
    std::memcpy(config.waiters, geometry_.gate_waiters, sizeof(config.waiters));
    config.table = geometry_.mount_table;
    config.loader_root_id = geometry_.mount_root_id;
    config.receive = geometry_.gate_receive;
    config.original_wait = geometry_.gate_original_wait;
    config.park = geometry_.gate_park;
    config.bootstrap_restore_pending = 0;
    rc = henConfigureGame(&hen, &config);
    if (R_SUCCEEDED(rc))
        rc = henAdoptGameHandles0(&hen, handles_.storage, handles_.workers);
    const Handle game_handles[] = {handles_.server_copy, handles_.event_read,
        handles_.event_write, escrow_.monitor_session, current_reader_,
        escrow_.fs_process, escrow_.fs_notifier};
    if (R_SUCCEEDED(rc)) rc = henAdoptGameHandles1(&hen, game_handles);
    if (R_SUCCEEDED(rc)) rc = henAdoptLoaderRoot(&hen, escrow_.loader_root);
    if (R_SUCCEEDED(rc)) rc = henStartGameArmPaused(&hen, CUR_PROCESS_HANDLE);
    if (R_FAILED(rc)) return rc;

    // Keep supervisor discoverable in the NEW HEN before the original exits.
    // The plugin runtime retained an original client handle to the same IPC
    // service, rather than closing it after the initial bootstrap COPY.
    HenPluginRecord plugin = {};
    std::memcpy(plugin.name.name, "hotupdate-supervisor", sizeof("hotupdate-supervisor"));
    rc = svcGetProcessId(&plugin.process_id, CUR_PROCESS_HANDLE);
    if (R_SUCCEEDED(rc))
        rc = henAdoptPlugin(&hen, &plugin, supervisor_control_client_);
    return rc;
}
Result Controller::stageRo(Handle next_hen, u64 generation) {
    Service ro = {};
    serviceCreate(&ro, active_ro_);
    const struct { u64 pid_placeholder; u64 generation; } in = {0, generation};
    static_assert(sizeof(in) == 16, "RO staging payload size");
    return serviceDispatchIn(&ro, RoCmd_StageHenControl, in,
        .in_send_pid = true,
        .in_num_handles = 1, .in_handles = {next_hen});
}
Result Controller::roCommand(u32 cmd, u64 generation) {
    Service ro = {};
    serviceCreate(&ro, active_ro_);
    const struct { u64 pid_placeholder; u64 generation; } in = {0, generation};
    static_assert(sizeof(in) == 16, "RO handoff payload size");
    return serviceDispatchIn(&ro, cmd, in, .in_send_pid = true);
}
Result Controller::replaceHen(RestartReport* report, const u8 expected_digest[32], u64 expected_size) {
    if (!report || !expected_digest || !expected_size) return Invalid;
    *report = {};
    report->generation = generation_;
    if (unresolved_ || !launcher_.available() ||
        active_supervisor_ == INVALID_HANDLE ||
        supervisor_control_client_ == INVALID_HANDLE) return Invalid;
    Result rc = 0;
    Service previous = {};
    serviceCreate(&previous, active_supervisor_);
    HenStatus old_status = {};
    Result old_status_rc = henGetStatus(&previous, &old_status);
    if (R_FAILED(old_status_rc)) return old_status_rc;
    report->previous_pid = old_status.process_id;
    u32 status = 0;
    rc = henQuerySupervisorQuiescence(&previous, &status);
    if (R_FAILED(rc) || !(status & 1u)) return R_FAILED(rc) ? rc : Invalid;
    report->phase = RestartPhase::HoldOldHen;
    rc = henHoldGameArm(&previous);
    if (R_FAILED(rc)) return rc;
    report->rollback_available = 1;

    Handle next_control = INVALID_HANDLE;
    Handle next_supervisor = INVALID_HANDLE;
    bool ro_staged = false;
    const u64 next_generation = generation_ + 1;
    LaunchReport launch = {};
    report->phase = RestartPhase::LaunchCandidate;
    rc = launcher_.launch(CandidateMain, CandidatePackage, PackageSd, &launch,
                          expected_digest, expected_size);
    if (R_FAILED(rc) || launch.phase != LaunchPhase::Completed) {
        if (R_SUCCEEDED(rc)) rc = Invalid;
        goto recover;
    }
    report->new_pid = launch.pid;

    report->phase = RestartPhase::ConnectCandidate;
    // New HEN registers mig:usr at startup. The original HEN released the
    // bootstrap registration, so no exclusive-name conflict exists here.
    for (unsigned n = 0; n < 100; ++n) {
        rc = connectMigratedHen(&next_control);
        if (R_SUCCEEDED(rc) && next_control != INVALID_HANDLE) break;
        svcSleepThread(50000000);
    }
    if (R_FAILED(rc) || next_control == INVALID_HANDLE) {
        if (R_SUCCEEDED(rc)) rc = Invalid;
        goto recover;
    }
    {
        Service hen = {};
        serviceCreate(&hen, next_control);
        HenStatus hen_status = {};
        rc = henGetStatus(&hen, &hen_status);
        if (R_SUCCEEDED(rc) && (hen_status.process_id != launch.pid ||
            hen_status.protocol_version != HEN_PROTOCOL_VERSION ||
            R_FAILED(hen_status.initialization_result))) rc = Invalid;
    }
    if (R_FAILED(rc)) goto recover;
    report->phase = RestartPhase::ConfigureCandidate;
    rc = configureNewHen(next_control, &next_supervisor);
    if (R_FAILED(rc)) goto recover;

    // Warm the new GameArm's FS reader/domain setup while it remains held.
    // Warmup cannot park or modify Loader, and OLD HEN remains held.
    report->phase = RestartPhase::WarmCandidate;
    {
        Service candidate = {};
        serviceCreate(&candidate, next_supervisor);
        rc = henWarmupPaused(&candidate);
        if (R_FAILED(rc)) goto recover;
        u32 phase = 0;
        rc = henQuerySupervisorQuiescence(&candidate, &phase);
        if (R_FAILED(rc) || !(phase & 2u)) {
            if (R_SUCCEEDED(rc)) rc = Invalid;
            goto recover;
        }
    }

    report->phase = RestartPhase::StageRo;
    rc = stageRo(next_control, next_generation);
    // The IPC helper does not distinguish a rejected stage from a lost reply.
    // Keep both held if we cannot establish what RO accepted.
    if (R_FAILED(rc)) goto failed_after_stop;
    ro_staged = true;
    report->phase = RestartPhase::SwitchRo;
    rc = roCommand(RoCmd_SwitchHenControl, next_generation);
    if (R_FAILED(rc)) goto recover;

    // Both generations were live and the new one accepted its full native
    // handoff. The old HEN now exits through its own safe-point server loop.
    report->phase = RestartPhase::StopOldHen;
    // A lost reply can mean the stop was accepted. Cross the irreversible
    // boundary BEFORE sending, so no uncertain stop can resume the old owner.
    report->rollback_available = 0;
    rc = henStopIfHeld(&previous);
    if (R_FAILED(rc)) goto failed_after_stop;
    // A successful CMIF reply means the old HEN committed to exiting, NOT
    // that it has exited. Never arm a second gate owner while PM still sees
    // the previous process. A timeout is unrecoverable by ordinary rollback:
    // the old instance might exit at any later moment.
    report->rollback_available = 0;
    {
        constexpr Result ProcessNotFound = MAKERESULT(15, 1);
        Service* monitor = pmdmntGetServiceSession();
        serviceCreate(monitor, escrow_.monitor_session);
        bool gone = false;
        for (u32 i = 0; i < 120; ++i) {
            u64 id = 0;
            Result query = pmdmntGetProgramId(&id, report->previous_pid);
            if (query == ProcessNotFound) { gone = true; break; }
            if (R_FAILED(query)) { rc = query; break; }
            svcSleepThread(50000000);
        }
        if (!gone) {
            if (R_SUCCEEDED(rc)) rc = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
            goto failed_after_stop;
        }
    }
    report->old_hen_stopped = 1;
    report->phase = RestartPhase::ActivateNewHen;
    {
        Service candidate = {};
        serviceCreate(&candidate, next_supervisor);
        rc = henActivatePaused(&candidate);
    }
    if (R_FAILED(rc)) goto failed_after_stop;
    report->phase = RestartPhase::CommitRo;
    rc = roCommand(RoCmd_FinalizeHenControl, next_generation);
    if (R_FAILED(rc)) goto failed_after_stop;

    if (owns_active_supervisor_) svcCloseHandle(active_supervisor_);
    active_supervisor_ = next_supervisor;
    owns_active_supervisor_ = true;
    next_supervisor = INVALID_HANDLE;
    generation_ = next_generation;
    report->generation = generation_;
    report->phase = RestartPhase::RunningReplacement;
    if (next_control != INVALID_HANDLE) svcCloseHandle(next_control);
    report->result = 0;
    return 0;

recover:
    // An unresolved Loader restoration or PM worker is not ordinary rollback.
    // Keep both HEN generations held; do not re-arm a gate owner speculatively.
    if (!launcher_.available()) {
        report->rollback_available = 0;
        goto failed_after_stop;
    }
    report->phase = RestartPhase::RestoreOld;
    if (ro_staged) {
        Result rr = roCommand(RoCmd_RollbackHenControl, next_generation);
        if (R_FAILED(rr)) { report->recovery = rr; goto failed_after_stop; }
    }
    // Candidate is still held and hasn't touched Loader. An unsuccessful
    // pre-stop transaction may leave it running, but not game-armed.
    if (report->new_pid) {
        Service* pm = pmshellGetServiceSession();
        serviceCreate(pm, escrow_.shell_session);
        Result terminate = pmshellTerminateProcess(report->new_pid);
        if (R_FAILED(terminate)) { report->recovery = terminate; goto failed_after_stop; }
    }
    {
        Result resume = henResumeGameArm(&previous);
        if (R_FAILED(resume)) { report->recovery = resume; unresolved_ = true; }
    }
    if (next_supervisor != INVALID_HANDLE) svcCloseHandle(next_supervisor);
    if (next_control != INVALID_HANDLE) svcCloseHandle(next_control);
    if (!launcher_.available()) unresolved_ = true;
    if (unresolved_) report->phase = RestartPhase::Unresolved;
    report->result = rc;
    return rc;
failed_after_stop:
    // The stop or launch outcome is unknown, or old HEN already exited.
    // Preserve routing and processes; never guess which generation can resume.
    unresolved_ = true;
    report->rollback_available = 0;
    report->phase = RestartPhase::Unresolved;
    report->result = rc;
    if (next_supervisor != INVALID_HANDLE) svcCloseHandle(next_supervisor);
    if (next_control != INVALID_HANDLE) svcCloseHandle(next_control);
    return rc;
}
} // namespace hotupdate
