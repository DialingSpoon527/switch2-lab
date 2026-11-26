// SPDX-License-Identifier: GPL-2.0-only
// Native supervisor entrypoint. This process survives HEN/RO generations.
#include <cstring>
#include <switch.h>
#include <kaihen/plugin.h>
#include "launch_backend.hpp"
#include "restart_controller.hpp"
#include "deferred_monitor.hpp"
#include "../../common/control.h"
#include "../../hen/source/game_trace.hpp"
#include <switch/arm/counter.h>

extern "C" {
u32 __nx_applet_type = AppletType_None;
size_t __nx_heap_size = 0x400000;
void __appInit(void) {}
void __appExit(void) {}
}

namespace {
using namespace hotupdate;
enum SupervisorCommand : u32 {
    Status = 0, Ping = 1, QueryHenSafe = 2,
    RestartHen = 10, RestartRo = 11, LastRestart = 12,
    RestartPackage = 13, LastPackage = 14, ProbeLaunch = 15, // legacy denied
};
struct SupervisorStatus {
    u32 protocol = 9;
    u32 capabilities = 0;
    u32 missing = 1; // Full recovery remains unqualified on hardware.
    u32 restart_in_progress = 0;
    u64 process_id = 0;
    u64 pings = 0;
    Result init_result = 0;
    Result latest_result = 0;
    u64 latest_pid = 0;
    u64 generation = 1;
};
static_assert(sizeof(SupervisorStatus) <= KAIHEN_PLUGIN_MESSAGE_SIZE);
static_assert(sizeof(RestartReport) <= KAIHEN_PLUGIN_MESSAGE_SIZE);

struct Context {
    SupervisorStatus status = {};
    DeferredMonitor* deferred = nullptr;
};
Result handler(u32 command, const void* input, size_t input_size,
               void* output, size_t* output_size, void* opaque) {
    auto* context = static_cast<Context*>(opaque);
    if (!context || !output || !output_size || !context->deferred)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    const Result Invalid = MAKERESULT(Module_Libnx, LibnxError_BadInput);

    // No immediate native restart endpoints exist in this supervisor.
    // Even a controller from a previous build cannot call them.
    if (command == RestartHen || command == RestartRo ||
        command == RestartPackage || command == LastPackage ||
        command == ProbeLaunch || command == DeferredQueueRoCommand || command == DeferredPreflightRoCommand)
        return Invalid;

    if (command == DeferredQueueHenPackageCommand) {
        if (!input || input_size != sizeof(DeferredHenPackageRequest)) return Invalid;
        DeferredHenPackageRequest request = {};
        std::memcpy(&request, input, sizeof(request));
        DeferredReport report = {};
        const Result rc = context->deferred->queueHenPackage(request, &report);
        if (R_FAILED(rc)) return rc;
        std::memcpy(output, &report, sizeof(report));
        *output_size = sizeof(report);
        return 0;
    }
    if (command == DeferredQueueCommand || command == DeferredQueueHenCommand) {
        if (!input || input_size != sizeof(DeferredRequest)) return Invalid;
        DeferredRequest request = {};
        std::memcpy(&request, input, sizeof(request));
        DeferredReport report = {};
        const Result rc = command == DeferredQueueHenCommand
            ? context->deferred->queueHen(request, &report)
            : context->deferred->queueDryRun(request, &report);
        if (R_FAILED(rc)) return rc;
        std::memcpy(output, &report, sizeof(report));
        *output_size = sizeof(report);
        return 0;
    }
    if (input || input_size) return Invalid;

    if (command == DeferredStatusCommand || command == DeferredCancelCommand) {
        DeferredReport report = {};
        const Result rc = command == DeferredStatusCommand
            ? context->deferred->status(&report)
            : context->deferred->cancel(&report);
        if (R_FAILED(rc)) return rc;
        std::memcpy(output, &report, sizeof(report));
        *output_size = sizeof(report);
        return 0;
    }
    if (command == DeferredCapabilitiesCommand) {
        DeferredCapabilities caps = {};
        const Result rc = context->deferred->capabilities(&caps);
        if (R_FAILED(rc)) return rc;
        std::memcpy(output, &caps, sizeof(caps));
        *output_size = sizeof(caps);
        return 0;
    }
    if (command == DeferredPreflightHenCommand) {
        DeferredHenPreflight report = {};
        const Result rc = context->deferred->preflightHen(&report);
        if (R_FAILED(rc)) return rc;
        std::memcpy(output, &report, sizeof(report));
        *output_size = sizeof(report);
        return 0;
    }
    if (command == DeferredRestartStatusCommand || command == LastRestart) {
        RestartReport report = {};
        const Result rc = context->deferred->lastRestart(&report);
        if (R_FAILED(rc)) return rc;
        std::memcpy(output, &report, sizeof(report));
        *output_size = sizeof(report);
        return 0;
    }
    if (command == QueryHenSafe) {
        u32 flags = 0;
        const Result rc = context->deferred->querySafety(&flags);
        if (R_FAILED(rc)) return rc;
        std::memcpy(output, &flags, sizeof(flags));
        *output_size = sizeof(flags);
        return 0;
    }
    if (command == Ping) context->status.pings++;
    else if (command != Status) return Invalid;

    SupervisorStatus s = context->status;
    DeferredReport d = {};
    RestartReport r = {};
    if (R_SUCCEEDED(context->deferred->status(&d))) {
        const auto p = static_cast<DeferredPhase>(d.phase);
        s.restart_in_progress = p == DeferredPhase::HenExecuting ? 1u : 0u;
        if (p == DeferredPhase::HenReplaced || p == DeferredPhase::HenFailed ||
            p == DeferredPhase::HenUnresolved) {
            s.latest_result = d.result;
            if (R_SUCCEEDED(context->deferred->lastRestart(&r))) {
                s.latest_pid = r.new_pid;
                if (p == DeferredPhase::HenReplaced)
                    s.generation = r.generation;
            }
        }
    }
    std::memcpy(output, &s, sizeof(s));
    *output_size = sizeof(s);
    return 0;
}
// Best-effort persistent flight recorder. No SD writes occur inside
// HEN's GameArm or Loader-interception functions.
constexpr u32 GameLogMagic = 0x474f4c4b; // KLOG
constexpr u32 GameLogEntries = 4096;
constexpr const char* GameLogPath = "/kaihen/hotupdate/game-launch-trace.bin";

struct GameLogRecord {
    u32 magic;
    u32 kind;      // 1=before query, 2=reply, 3=setup failure
    u32 ordinal;
    Result result;
    u64 ticks;
    HenDebugSnapshot snapshot;
};
static_assert(sizeof(GameLogRecord) == 72);

struct GameLogContext {
    Handle supervisor_session;
    Handle fs_root;
};

[[maybe_unused]] void gameLogThread(void* opaque) {
    auto* ctx = static_cast<GameLogContext*>(opaque);
    if (!ctx || ctx->supervisor_session == INVALID_HANDLE ||
        ctx->fs_root == INVALID_HANDLE) return;

    // Let bootstrap finish arming HEN before beginning periodic queries.
    svcSleepThread(2000000000ll);

    Service root = {};
    serviceCreate(&root, ctx->fs_root);
    root.own_handle = false;

    FsFileSystem sd = {};
    Result rc = serviceDispatch(&root, 18,
        .out_num_objects = 1,
        .out_objects = &sd.s);
    if (R_FAILED(rc)) return;

    // Use an already-present /kaihen/hotupdate directory.
    // One fixed-size file avoids extending the file during capture.
    fsFsDeleteFile(&sd, GameLogPath);
    rc = fsFsCreateFile(&sd, GameLogPath,
        static_cast<s64>(GameLogEntries * sizeof(GameLogRecord)), 0);
    if (R_FAILED(rc)) {
        fsFsClose(&sd);
        return;
    }
    rc = fsFsCommit(&sd);
    if (R_FAILED(rc)) {
        fsFsClose(&sd);
        return;
    }

    FsFile file = {};
    rc = fsFsOpenFile(&sd, GameLogPath, FsOpenMode_Write, &file);
    if (R_FAILED(rc)) {
        fsFsClose(&sd);
        return;
    }

    u32 next = 0;
    auto record = [&](u32 kind, Result result,
                      const HenDebugSnapshot& snapshot) -> bool {
        if (next >= GameLogEntries) return false;
        GameLogRecord entry = {};
        entry.magic = GameLogMagic;
        entry.kind = kind;
        entry.ordinal = next;
        entry.result = result;
        entry.ticks = armGetSystemTick();
        entry.snapshot = snapshot;

        // Flush before proceeding so a hard shutdown can leave
        // the last completed checkpoint on the SD card.
        rc = fsFileWrite(&file,
            static_cast<s64>(next * sizeof(entry)),
            &entry, sizeof(entry), FsWriteOption_Flush);
        if (R_FAILED(rc)) return false;
        ++next;
        return true;
    };

    Service supervisor = {};
    serviceCreate(&supervisor, ctx->supervisor_session);
    Handle diagnostic_client = INVALID_HANDLE;

    // Ask HEN for a separate private session. Do not share the
    // supervisor controller's session with the recorder.
    rc = henCreateRoControlSession(&supervisor, &diagnostic_client);

    if (R_FAILED(rc)) {
        record(3, rc, {});
    } else {
        Service diagnostic = {};
        serviceCreate(&diagnostic, diagnostic_client);

        for (u32 sample = 0; sample < GameLogEntries / 2; ++sample) {
            // If HEN never replies, the last record remains kind=1.
            if (!record(1, 0, {})) break;

            HenDebugSnapshot snapshot = {};
            Result query = serviceDispatchOut(&diagnostic,
                HenControlCommand_GetDebugSnapshot, snapshot);

            if (!record(2, query, snapshot) || R_FAILED(query)) break;
            svcSleepThread(1000000000ll);
        }
        svcCloseHandle(diagnostic_client);
    }

    fsFileClose(&file);
    fsFsClose(&sd);
}
}

int main(void) {
    Handle ready = INVALID_HANDLE, control = INVALID_HANDLE;
    KaihenSupervisorEscrow escrow = {};
    Result rc = pluginRuntimeInitializeSupervisor(&ready, &control, &escrow);
    if (R_FAILED(rc)) return static_cast<int>(rc);
    KaihenSupervisorLaunchGeometry geometry = {};
    KaihenSupervisorLaunchHandles handles = {};
    Handle control_client = INVALID_HANDLE, fs_root = INVALID_HANDLE;
    rc = pluginRuntimeGetSupervisorLaunch(&geometry, &handles);
    if (R_SUCCEEDED(rc))
        rc = pluginRuntimeGetSupervisorControlClient(&control_client, &fs_root);
    if (R_FAILED(rc)) {
        pluginRuntimeExit();
        return static_cast<int>(rc);
    }
    Launcher launcher(geometry, handles, escrow, fs_root);
    Controller controller(launcher, geometry, handles, escrow, fs_root, control_client);
    DeferredMonitor deferred(controller, escrow.monitor_session);
    rc = deferred.start();
    if (R_FAILED(rc)) {
        pluginRuntimeExit();
        return static_cast<int>(rc);
    }
    Context context = {};
    context.deferred = &deferred;
    context.status.init_result = 0;
    context.status.capabilities = 0x7f; // copied escrow handle PRESENCE only
    context.status.missing = 1; // intentionally unqualified until independently proven
    rc = svcGetProcessId(&context.status.process_id, CUR_PROCESS_HANDLE);
    if (R_FAILED(rc)) context.status.process_id = 0;
    PluginService server = {};
    rc = pluginServiceStart(&server, control, handler, &context);
    if (R_SUCCEEDED(rc)) {
        rc = svcSignalEvent(ready);
    }
    if (R_SUCCEEDED(rc)) {
        // QUIET_SUPERVISOR_V1
        // Keep IPC/controller functionality, but perform no
        // continuous RO/HEN polling or background SD logging.
        for (;;) svcSleepThread(10000000000ull);
    }
    if (server.started) pluginServiceStop(&server);
    deferred.stop();
    pluginRuntimeExit();
    return static_cast<int>(rc);
}
