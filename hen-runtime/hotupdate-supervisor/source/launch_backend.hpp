// SPDX-License-Identifier: GPL-2.0-only
// Resident native launch transaction, NOT a proof of Switch 2 hardware safety.
#pragma once
#include <switch.h>
#include <kaihen/plugin.h>

namespace hotupdate {

enum class LaunchPhase : u32 {
    Idle = 0, CandidatePackage, OpenPackage, ParkGate, LaunchPM,
    ExchangeMount, RestoreGate, AwaitPM, StartProcess, Completed,
    FailedRestored, UnresolvedGate, UnresolvedWorker
};
struct LaunchReport {
    LaunchPhase phase = LaunchPhase::Idle;
    Result result = 0;
    Result restore_result = 0;
    Result pm_result = 0;
    u64 pid = 0;
    u32 mounts = 0;
    u32 restore_pending = 0;
};

/// Uses retained handle copies from bootstrap. Caller must hold previous HEN
/// at a safe point before calling launch(). Only one instance can ever be
/// active; an unresolved worker/gate irreversibly locks the launcher.
class Launcher {
public:
    Launcher(const KaihenSupervisorLaunchGeometry& g,
             const KaihenSupervisorLaunchHandles& h,
             const KaihenSupervisorEscrow& e, Handle fs_root)
        : geometry_(g), handles_(h), escrow_(e), fs_root_(fs_root) {}

    Result launch(const char* candidate_main, const char* package_file,
                  const char* package_sd_path, LaunchReport* out,
                  const u8 expected_digest[32], u64 expected_size);
    bool available() const { return !locked_; }
    bool isBusy() const { return busy_; }
    Result updateRoReader(Handle reader) {
        if (busy_ || locked_ || reader == INVALID_HANDLE)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        escrow_.ro_reader = reader;  // caller owns handle lifetime.
        return 0;
    }
private:
    struct PMWorker {
        Handle shell = INVALID_HANDLE;
        u64 program_id = 0;
        u32 flags = 0;
        u8 storage = 0;
        volatile u32 completed = 0;
        Result result = 0;
        u64 pid = 0;
        Thread thread = {};
        bool started = false;
    } pm_;
    static void pmEntry(void* context);
    void cleanupWorker(bool finished);
    KaihenSupervisorLaunchGeometry geometry_;
    KaihenSupervisorLaunchHandles handles_;
    KaihenSupervisorEscrow escrow_;
    Handle fs_root_;
    bool busy_ = false;
    bool locked_ = false;
};
}
