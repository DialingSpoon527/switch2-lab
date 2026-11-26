// SPDX-License-Identifier: MIT
// One-shot SD controller. HEN owns privileged PM sessions; acceptance of a
// launch is asynchronous and this NRO must exit before HEN can start it.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <switch/crypto/sha256.h>
#include <kaihen.h>
#include <kaihen/lifecycle.h>
#include "command_parser.h"

_Static_assert(HEN_PLUGIN_NAME_SIZE == P2_COMMAND_NAME_BYTES, "plugin-name ABI changed");
_Static_assert(sizeof(HenManagerDiagnosticsV1) == 176, "manager diagnostics v1 ABI changed");
#define P2_TRY(expression) do { const Result p2_result = (expression); if (R_FAILED(p2_result)) return p2_result; } while (0)

static const char* Command = "/kaihen/plugin-manager/command.txt";
static const char* ResultPath = "/kaihen/plugin-manager/result.txt";
static const char* ResultTemporary = "/kaihen/plugin-manager/result.pending.txt";
static const char* ResultPrevious = "/kaihen/plugin-manager/result.previous.txt";
static const size_t MaxNso = 64ull * 1024 * 1024;
static char report[16384];
static bool report_truncated;
static bool report_truncation_noted;

// Exact FS mapping from libounce nx/source/runtime/devices/fs_dev.c.
// Only ENOENT means an absent command; other I/O errors must be visible.
static bool pathMissing(Result rc) { return R_VALUE(rc) == 0x202; }
static Result badInput(void) { return MAKERESULT(Module_Libnx, LibnxError_BadInput); }

static void note(const char* format, ...) {
    const size_t used = strlen(report);
    if (used >= sizeof(report) - 64) { report_truncated = true; return; }
    va_list arguments;
    va_start(arguments, format);
    const int needed = vsnprintf(report + used, sizeof(report) - used - 64, format, arguments);
    va_end(arguments);
    if (needed < 0 || (size_t)needed >= sizeof(report) - used - 64) report_truncated = true;
}

static void append(const char* action, const char* name, Result rc) {
    note("%s %s result=%08x\n", action, name ? name : "", (unsigned)rc);
}

static Result deleteIfPresent(FsFileSystem* sd, const char* path) {
    Result rc = fsFsDeleteFile(sd, path);
    return pathMissing(rc) ? 0 : rc;
}

static Result saveResult(FsFileSystem* sd) {
    // A run saves several checkpoints. Append the reserved truncation marker
    // only once; repeated saves must not grow a full report past its buffer.
    if (report_truncated && !report_truncation_noted) {
        strcat(report, "\nREPORT TRUNCATED: reduce installed-file listing.\n");
        report_truncation_noted = true;
    }
    const size_t length = strlen(report);
    P2_TRY(deleteIfPresent(sd, ResultTemporary));
    P2_TRY(fsFsCreateFile(sd, ResultTemporary, length, 0));
    FsFile file = {};
    P2_TRY(fsFsOpenFile(sd, ResultTemporary, FsOpenMode_Write, &file));
    Result rc = fsFileWrite(&file, 0, report, length, FsWriteOption_Flush);
    fsFileClose(&file);
    if (R_FAILED(rc)) return rc; // Preserve the previous complete result.
    P2_TRY(fsFsCommit(sd)); // Preserve the complete temporary report before rotation.
    P2_TRY(deleteIfPresent(sd, ResultPrevious));
    rc = fsFsRenameFile(sd, ResultPath, ResultPrevious);
    const bool previous_moved = R_SUCCEEDED(rc);
    if (R_FAILED(rc) && !pathMissing(rc)) return rc;
    rc = fsFsRenameFile(sd, ResultTemporary, ResultPath);
    if (R_FAILED(rc)) {
        if (previous_moved) {
            const Result restore = fsFsRenameFile(sd, ResultPrevious, ResultPath);
            const Result committed = fsFsCommit(sd);
            if (R_FAILED(restore) || R_FAILED(committed))
                printf("report rotation recovery: restore=%08x commit=%08x; preserve result.previous.txt and result.pending.txt\n",
                       (unsigned)restore, (unsigned)committed);
        }
        return rc;
    }
    return fsFsCommit(sd);
}

// Save the stage before entering a potentially blocking IPC call. This is
// evidence of the last checkpoint reached, not proof of where a later hang
// occurred. A failed save stops further dispatch in this invocation.
static Result checkpoint(FsFileSystem* sd, const char* stage) {
    note("checkpoint=%s\n", stage);
    const Result rc = saveResult(sd);
    if (R_FAILED(rc)) append("save-checkpoint", stage, rc);
    return rc;
}

static Result hashPlugin(FsFileSystem* sd, const char* name, u8 hash[32]) {
    char path[96];
    snprintf(path, sizeof(path), "/kaihen/plugins/%s.nso", name);
    FsFile file = {};
    P2_TRY(fsFsOpenFile(sd, path, FsOpenMode_Read, &file));
    s64 size = 0;
    Result rc = fsFileGetSize(&file, &size);
    if (R_FAILED(rc) || size < 0x100 || (u64)size > MaxNso) {
        fsFileClose(&file);
        return R_FAILED(rc) ? rc : badInput();
    }
    const u64 total = (u64)size;
    Sha256Context sha;
    sha256ContextCreate(&sha);
    u8 bytes[4096];
    for (u64 offset = 0; offset < total && R_SUCCEEDED(rc);) {
        const u64 count = total - offset < sizeof(bytes) ? total - offset : sizeof(bytes);
        u64 got = 0;
        rc = fsFileRead(&file, offset, bytes, count, FsReadOption_None, &got);
        if (R_SUCCEEDED(rc) && got != count) rc = badInput();
        if (R_SUCCEEDED(rc)) sha256ContextUpdate(&sha, bytes, count);
        offset += count;
    }
    fsFileClose(&file);
    if (R_SUCCEEDED(rc)) sha256ContextGetHash(&sha, hash);
    return rc;
}

// Consume even invalid commands, before any dispatch. A crash after consumption
// can lose a request, so inspect status before submitting it again. Never replay.
static Result readCommand(FsFileSystem* sd, char action[16], char name[64]) {
    // Absent command defaults to the cached-only diagnostic request. A list
    // remains explicit because older HEN builds refresh it through PM IPC.
    strcpy(action, "diag");
    name[0] = 0;
    FsFile file = {};
    Result rc = fsFsOpenFile(sd, Command, FsOpenMode_Read, &file);
    if (pathMissing(rc)) return 0;
    if (R_FAILED(rc)) return rc;
    s64 size = 0;
    char data[P2_COMMAND_MAX_BYTES] = {};
    rc = fsFileGetSize(&file, &size);
    if (R_SUCCEEDED(rc) && (size <= 0 || (u64)size > sizeof(data))) rc = badInput();
    if (R_SUCCEEDED(rc)) {
        u64 actual = 0;
        rc = fsFileRead(&file, 0, data, size, FsReadOption_None, &actual);
        if (R_SUCCEEDED(rc) && actual != (u64)size) rc = badInput();
    }
    fsFileClose(&file);
    Result consumed = fsFsDeleteFile(sd,Command);
    if (R_SUCCEEDED(consumed)) consumed = fsFsCommit(sd);
    if (R_FAILED(consumed)) return consumed;
    if (R_FAILED(rc)) return rc;
    return p2ParseCommand(data, (size_t)size, action, name) ? 0 : badInput();
}

static const char* phaseName(HenBackgroundPluginPhase phase) {
    switch (phase) {
#define PHASE(value) case HenBackgroundPluginPhase_##value: return #value
        PHASE(Idle); PHASE(Queued); PHASE(Verifying); PHASE(Packaging);
        PHASE(Parking); PHASE(Launching); PHASE(Exchanging); PHASE(Restoring);
        PHASE(Started); PHASE(WaitingMigration); PHASE(Provisioning); PHASE(WaitingReady);
        PHASE(RunningUnpublished); PHASE(Ready); PHASE(Published); PHASE(PublishFailed);
        PHASE(Failed); PHASE(Quarantined); PHASE(Cancelled); PHASE(Exited);
#undef PHASE
        default: return "Unknown";
    }
}

static const char* stateName(HenManagedState state) {
    switch (state) {
#define STATE(value) case HenManagedState_##value: return #value
        STATE(Running); STATE(Stopping); STATE(Stopped); STATE(StopFailed); STATE(RestartQueued);
#undef STATE
        default: return "Unknown";
    }
}

static Result diagnostics(Service* hen, FsFileSystem* sd) {
    P2_TRY(checkpoint(sd, "before-cached-diagnostics"));
    HenManagerDiagnosticsV1 snapshot = {};
    Result rc = henGetManagerDiagnostics(hen, &snapshot);
    append("cached-diagnostics", NULL, rc);
    if (R_FAILED(rc)) {
        note("Diagnostics unavailable: this HEN may not support the command, or the IPC failed. No snapshot interpreted.\n");
    } else if (snapshot.version != HEN_MANAGER_DIAGNOSTICS_VERSION ||
               snapshot.size != sizeof(snapshot)) {
        note("Unsupported diagnostics layout: version=%u size=%u expected-version=%u expected-size=%u. No snapshot interpreted.\n",
             (unsigned)snapshot.version, (unsigned)snapshot.size,
             (unsigned)HEN_MANAGER_DIAGNOSTICS_VERSION, (unsigned)sizeof(snapshot));
        rc = badInput();
    } else {
        const u32 flags = snapshot.flags;
        const HenBackgroundPluginStatus* bg = &snapshot.background;
        note("diagnostics version=%u size=%u hen-build=%u sampled-tick=%llu\n",
             (unsigned)snapshot.version, (unsigned)snapshot.size, (unsigned)snapshot.build_id,
             (unsigned long long)snapshot.sampled_tick);
        note("manager flags=%08x shell_available=%u gate_owned=%u gate_restored=%u active=%u pending=%u quarantined=%u\n",
             (unsigned)flags, !!(flags & HenManagerFlag_ShellAvailable),
             !!(flags & HenManagerFlag_GateOwned), !!(flags & HenManagerFlag_GateRestored),
             !!(flags & HenManagerFlag_Active), !!(flags & HenManagerFlag_Pending),
             !!(flags & HenManagerFlag_Quarantined));
        note("workers pm_dispatched=%u pm_finished=%u process_started=%u termination_active=%u housekeeping_allowed=%u\n",
             !!(flags & HenManagerFlag_PmDispatched), !!(flags & HenManagerFlag_PmFinished),
             !!(flags & HenManagerFlag_ProcessStarted), !!(flags & HenManagerFlag_TerminationActive),
             !!(flags & HenManagerFlag_GameHousekeepingAllowed));
        note("startup shell_adoption_result=%08x game_start_result=%08x game_start_attempts=%u bootstrap_flags=%08x\n",
             (unsigned)snapshot.shell_adoption_result, (unsigned)snapshot.game_start_result,
             (unsigned)snapshot.game_start_attempts, (unsigned)snapshot.bootstrap_flags);
        note("loop iterations=%llu game_steps=%llu game_steps_suppressed=%llu\n",
             (unsigned long long)snapshot.loop_iterations, (unsigned long long)snapshot.game_step_calls,
             (unsigned long long)snapshot.game_steps_suppressed);
        note("manager-PM queries_started=%llu queries_completed=%llu last_result=%08x\n",
             (unsigned long long)snapshot.pm_queries_started, (unsigned long long)snapshot.pm_queries_completed,
             (unsigned)snapshot.last_pm_result);
        note("launch phase=%s(%u) serial=%llu pid=%llu result=%08x gate_step=%x mounts=%u\n",
             phaseName(bg->phase), (unsigned)bg->phase, (unsigned long long)bg->serial,
             (unsigned long long)bg->pid, (unsigned)bg->result, (unsigned)bg->gate_step,
             (unsigned)bg->mounts);
        note("launch_result=%08x restore_result=%08x\n", (unsigned)snapshot.launch_result,
             (unsigned)snapshot.restore_result);
        note("launch_failure_step=%x (saved before restoration)\n", (unsigned)snapshot.launch_failure_step);
        note("game phase=%u result=%08x serial=%u mounts=%u pid=%llu\n",
             (unsigned)snapshot.game_phase, (unsigned)snapshot.game_result,
             (unsigned)snapshot.game_serial, (unsigned)snapshot.game_mounts,
             (unsigned long long)snapshot.game_pid);
        note("game-trace sequence=%u point=%u result=%08x value=%x requests=%u steps=%u\n",
             (unsigned)snapshot.trace_sequence, (unsigned)snapshot.trace_point,
             (unsigned)snapshot.trace_result, (unsigned)snapshot.trace_value,
             (unsigned)snapshot.trace_requests, (unsigned)snapshot.trace_steps);
        note("Snapshot uses cached HEN state; it does not prove current process liveness. gate_step is a diagnostic code, not gate ownership.\n");
        if (flags & HenManagerFlag_Quarantined)
            note("QUARANTINED: gate_owned=%u in this snapshot; phase 17 alone does not establish a retained Loader gate. Preserve this report.\n",
                 !!(flags & HenManagerFlag_GateOwned));
    }
    const Result saved = checkpoint(sd, "cached-diagnostics-returned");
    return R_FAILED(saved) ? saved : rc;
}

static void listInstalled(FsFileSystem* sd) {
    FsDir directory = {};
    Result rc = fsFsOpenDirectory(sd, "/kaihen/plugins", FsDirOpenMode_ReadFiles, &directory);
    if (R_FAILED(rc)) { append("scan-plugins", NULL, rc); return; }
    note("installed NSO files (presence does not imply running):\n");
    FsDirectoryEntry entries[4] = {};
    bool complete = false;
    for (unsigned batch = 0; batch < 512 && !report_truncated; ++batch) {
        s64 count = 0;
        rc = fsDirRead(&directory, &count, 4, entries);
        if (R_FAILED(rc)) break;
        if (count == 0) { complete = true; break; }
        if (count < 0 || count > 4) { rc = badInput(); break; }
        for (s64 i = 0; i < count; ++i) {
            const size_t length = strnlen(entries[i].name, sizeof(entries[i].name));
            if (length == sizeof(entries[i].name) || length <= 4 || strcmp(entries[i].name + length - 4, ".nso")) continue;
            char name[HEN_PLUGIN_NAME_SIZE] = {};
            if (length - 4 >= sizeof(name)) continue;
            memcpy(name, entries[i].name, length - 4);
            if (p2SafeName(name)) note(" - %s.nso size=%lld\n", name, (long long)entries[i].file_size);
        }
    }
    fsDirClose(&directory);
    if (R_FAILED(rc)) append("read-dir", NULL, rc);
    if (!complete) note("installed scan incomplete (error, report bound, or 2048-entry limit)\n");
}

static Result list(Service* hen, FsFileSystem* sd) {
    // Capture and persist the no-refresh snapshot before enumeration. Never
    // fall back to old synchronous PM-refresh commands after a failed or
    // unsupported diagnostic request.
    P2_TRY(diagnostics(hen, sd));
    P2_TRY(checkpoint(sd, "before-managed-list"));
    u32 count = 0;
    Result rc = henGetManagedPluginCount(hen, &count);
    if (R_FAILED(rc)) { append("list", NULL, rc); return rc; }
    note("managed plugins: %u\n", (unsigned)count);
    if (count > 32) { append("invalid-managed-count", NULL, badInput()); return badInput(); }
    for (u32 i = 0; i < count; ++i) {
        HenManagedPluginInfo info = {};
        rc = henGetManagedPluginInfo(hen, i, &info);
        if (R_FAILED(rc)) { append("item", NULL, rc); return rc; }
        note("[%u] %.63s pid=%llu serial=%llu state=%s(%u) origin=%u error=%08x\n",
             (unsigned)i, info.name.name, (unsigned long long)info.process_id,
             (unsigned long long)info.serial, stateName(info.state), (unsigned)info.state,
             (unsigned)info.origin, (unsigned)info.last_result);
        note("    shutdown flags=%x grace-requested=%u force-requested=%u exit-confirmed=%u exit-timeout=%u\n",
             (unsigned)info.shutdown_flags,
             !!(info.shutdown_flags & HenManagedShutdown_GraceRequested),
             !!(info.shutdown_flags & HenManagedShutdown_ForceRequested),
             !!(info.shutdown_flags & HenManagedShutdown_ExitConfirmed),
             !!(info.shutdown_flags & HenManagedShutdown_ExitDeadlineExpired));
    }
    HenBackgroundPluginStatus bg = {};
    rc = henGetBackgroundPluginStatus(hen, &bg);
    if (R_FAILED(rc)) { append("launch-status", NULL, rc); return rc; }
    note("launch phase=%s(%u) serial=%llu pid=%llu rc=%08x gate_step=%x mounts=%u\n",
         phaseName(bg.phase), (unsigned)bg.phase, (unsigned long long)bg.serial,
         (unsigned long long)bg.pid, (unsigned)bg.result, (unsigned)bg.gate_step, (unsigned)bg.mounts);
    if (bg.phase == HenBackgroundPluginPhase_Queued)
        note("WAITING: manager will launch when Loader is idle; the current host can remain open. Inspect diag again for the final state.\n");
    if (bg.phase == HenBackgroundPluginPhase_Quarantined)
        note("QUARANTINED: preserve this report; do not retry launches or delete live package snapshots.\n");
    return 0;
}

static Result demoRequest(Service* hen, const char* action, const char* name) {
    HenPluginName target = {};
    strcpy(target.name, name);
    Service plugin = {};
    Result rc = henOpenPlugin(hen, &target, &plugin);
    if (R_FAILED(rc)) return rc;
    if (!strcmp(action, "ping")) {
        const u32 input = 41;
        u32 output = 0;
        rc = serviceDispatchInOut(&plugin, 0, input, output);
        if (R_SUCCEEDED(rc) && output != 42) rc = badInput();
        note("ping %s input=41 output=%llu result=%08x\n", name, (unsigned long long)output, (unsigned)rc);
    } else {
        KaihenPluginLaunchContext context = {};
        rc = serviceDispatchOut(&plugin, 2, context);
        if (R_SUCCEEDED(rc) && (context.version != KAIHEN_PLUGIN_LIFECYCLE_VERSION || context.size != sizeof(context))) {
            note("invalid context version=%u size=%u\n", (unsigned)context.version, (unsigned)context.size);
            rc = badInput();
        }
        if (R_SUCCEEDED(rc)) note("context %s source=%u session=%u reason=%u first=%u count=%u serial=%llu pid=%llu\n",
            name, (unsigned)context.start_source, (unsigned)context.session_phase, (unsigned)context.start_reason,
            (unsigned)!!(context.flags & KAIHEN_PLUGIN_FIRST_START_THIS_HEN_INSTANCE), (unsigned)context.launch_number,
            (unsigned long long)context.sequence, (unsigned long long)context.process_id);
    }
    serviceClose(&plugin);
    return rc;
}

int main(void) {
    report[0] = 0;
    report_truncated = false;
    report_truncation_noted = false;
    FsFileSystem sd = {};
    Result rc = fsOpenSdCardFileSystem(&sd);
    if (R_FAILED(rc)) return rc;
    // Existing-directory errors are harmless; the first actual file operation
    // still reports permissions, missing parent or other filesystem failure.
    (void)fsFsCreateDirectory(&sd, "/kaihen");
    (void)fsFsCreateDirectory(&sd, "/kaihen/plugin-manager");
    Service ro = {}, hen = {};
    char action[16] = {}, name[64] = {};
    note("Project 2 controller report v4 controller-build=%u\n", (unsigned)HEN_MANAGER_DIAGNOSTICS_BUILD_ID);
    rc = readCommand(&sd, action, name);
    if (R_FAILED(rc)) {
        append("read/consume/parse-command", NULL, rc);
    } else if (!strcmp(action, "help")) {
        note("Commands: diag | list | help | cancel | launch NAME | stop NAME | restart NAME | ping p2ping | context p2ping\n");
        note("No command file defaults to diag. diag requests only cached HEN diagnostics; list additionally reads the registry and scans installed files.\n");
        note("launch/restart result=0 is queue acceptance. Keep the host open; run diag/list afterward to check completion.\n");
    } else {
        note("request=%s %s\n", action, name);
        rc = checkpoint(&sd, "before-open-hen");
        const bool dispatch_allowed = R_SUCCEEDED(rc);
        if (R_SUCCEEDED(rc)) rc = kaihenGetRoSession(&ro);
        if (R_SUCCEEDED(rc)) rc = kaihenOpenHomebrewSession(&ro, &hen);
        if (R_FAILED(rc)) {
            append(dispatch_allowed ? "open-hen" : "dispatch-refused-checkpoint", NULL, rc);
        } else if (!strcmp(action, "diag")) {
            rc = diagnostics(&hen, &sd);
        } else if (!strcmp(action, "list")) {
            rc = list(&hen, &sd);
            if (R_SUCCEEDED(rc)) rc = checkpoint(&sd, "before-installed-scan");
            if (R_SUCCEEDED(rc)) listInstalled(&sd);
        } else if (!strcmp(action, "cancel")) {
            rc = henCancelBackgroundPlugin(&hen);
            append("cancel-pending-request", NULL, rc);
        } else if (!strcmp(action, "stop")) {
            HenPluginName target = {};
            strcpy(target.name, name);
            rc = henStopManagedPlugin(&hen, &target);
            append("stop-requested", name, rc);
            note("A successful stop request is not proof of process exit; inspect Stopped on a later invocation.\n");
            (void)list(&hen, &sd);
        } else if (!strcmp(action, "context") || !strcmp(action, "ping")) {
            rc = demoRequest(&hen, action, name);
            append(action, name, rc);
        } else {
            HenBackgroundPluginRequest request = {};
            strcpy(request.name, name);
            rc = hashPlugin(&sd, name, request.sha256);
            if (R_SUCCEEDED(rc)) {
                note("requested-sha256=");
                for (size_t i = 0; i < sizeof(request.sha256); ++i) note("%02x", (unsigned)request.sha256[i]);
                note("\n");
                rc = !strcmp(action, "launch") ? henLaunchBackgroundPlugin(&hen, &request) : henRestartManagedPlugin(&hen, &request);
                append("queue-request", name, rc);
                if (R_SUCCEEDED(rc)) note("ACCEPTED, not ready: keep the host open, then run list, ping p2ping and context p2ping to verify.\n");
            } else append("hash-plugin", name, rc);
            (void)list(&hen, &sd);
        }
    }
    append("controller-final", NULL, rc);
    if (serviceIsActive(&hen)) serviceClose(&hen);
    // RO is a borrowed service override and must not be closed.
    const Result saved = saveResult(&sd);
    if (R_FAILED(saved)) {
        printf("%s\nsave-result result=%08x\n", report, (unsigned)saved);
        if (R_SUCCEEDED(rc)) rc = saved;
    }
    fsFsClose(&sd);
    return rc;
}
