// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <switch.h>
#include <kaihen/plugin.h>
#include "launch_backend.hpp"
#include "../../common/game_arm.h"

namespace hotupdate {

enum class RestartPhase : u32 {
    Idle, HoldOldHen, LaunchCandidate, ConnectCandidate, ConfigureCandidate,
    WarmCandidate, StageRo, SwitchRo, StopOldHen, ActivateNewHen,
    CommitRo, RunningReplacement, RestoreOld, Unresolved
};
struct RestartReport {
    RestartPhase phase = RestartPhase::Idle;
    Result result = 0;
    Result recovery = 0;
    u64 previous_pid = 0;
    u64 new_pid = 0;
    u64 generation = 1;
    u32 rollback_available = 0;
    u32 old_hen_stopped = 0;
    u32 old_ro_stopped = 0;
};

/// A *real* native HEN switchover. Before StopOldHen every failure attempts
/// RO rollback and reactivation of the original. No OS kill is used on old
/// HEN: its own single-threaded loop performs controlled exit at safe point.
class Controller {
public:
    Controller(Launcher& launcher, const KaihenSupervisorLaunchGeometry& geometry,
               const KaihenSupervisorLaunchHandles& handles,
               const KaihenSupervisorEscrow& escrow, Handle fs_root,
               Handle supervisor_control_client)
        : launcher_(launcher), geometry_(geometry), handles_(handles),
          escrow_(escrow), fs_root_(fs_root),
          supervisor_control_client_(supervisor_control_client),
          active_supervisor_(escrow.hen_control), active_ro_(handles.ro_session),
          current_reader_(escrow.ro_reader) {}
    ~Controller();
    Result replaceHen(RestartReport* report, const u8 expected_digest[32], u64 expected_size);
    bool available() const { return !unresolved_ && launcher_.available(); }
    Result queryActiveHenSafety(u32* flags) const;
    Result queryActiveHenGameStatus(HenGameStatus* out) const;
    // Read-only, streamed SHA-256 of the immutable staged HEN main. Never
    // parks Loader or holds the current HEN. The generated exefs.nsp is NOT
    // required before queueing, because launch() creates it.
    Result preflightHen(u8 digest[32], u64* main_size) const;
    // Durable last known phase. Writes happen before gate work and after
    // return; never while the Loader interception is installed.
    Result writeHenCheckpoint(const char* message, size_t size) const;
    // Unique files per ticket avoid FTP cached reads from earlier attempts.
    Result writeHenTicketCheckpoint(u32 ticket,
                                const char* message, size_t size) const;
private:
    Launcher& launcher_;
    KaihenSupervisorLaunchGeometry geometry_;
    KaihenSupervisorLaunchHandles handles_;
    KaihenSupervisorEscrow escrow_;
    Handle fs_root_;
    Handle supervisor_control_client_;
    Handle active_supervisor_;
    Handle active_ro_;
    Handle current_reader_;
    bool owns_active_supervisor_ = false;
    bool unresolved_ = false;
    u64 generation_ = 1;
    Result connectMigratedHen(Handle* out);
    Result configureNewHen(Handle new_control, Handle* supervisor);
    Result stageRo(Handle next_hen, u64 generation);
    Result roCommand(u32 cmd, u64 generation);
    Result serviceLookup(const char* name, Handle* out);
    Result preflightCandidate(const char* path, u8 digest[32], u64* size) const;
};
}
