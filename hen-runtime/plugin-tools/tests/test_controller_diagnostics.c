// Actual controller main/diagnostics/list/checkpoint functions, compiled with
// the existing deterministic FS seam and the real diagnostics ABI header.
#include <stdarg.h>
#include <setjmp.h>
#define main runOriginalIoTests
#include "test_controller_io.c"
#undef main
#include <kaihen/diagnostics.h>

typedef struct { bool active; unsigned identity; } Service;
static unsigned ro_calls, open_calls, diag_calls, count_calls, item_calls, status_calls;
static unsigned scan_calls, hash_calls, action_calls, hen_close_calls, sd_close_calls;
static Result open_result, diag_result;
static HenManagerDiagnosticsV1 supplied;
static bool block_open, block_diag, block_scan;
static jmp_buf simulated_block;

static bool reportContains(const char* text) {
    return files[1].exists && strstr(files[1].data, text) != NULL;
}
static void assertCheckpoint(const char* stage) {
    assert(commits >= 2 && !files[2].exists && reportContains(stage));
    assert(!files[0].exists); // Present commands were durably consumed first.
}
static Result fsOpenSdCardFileSystem(FsFileSystem* sd) { (void)sd; return 0; }
static Result fsFsCreateDirectory(FsFileSystem* sd, const char* path) { (void)sd; (void)path; return 0; }
static void fsFsClose(FsFileSystem* sd) { (void)sd; ++sd_close_calls; }
static bool serviceIsActive(const Service* service) { return service->active; }
static void serviceClose(Service* service) {
    assert(service->active && service->identity == 2); // Borrowed RO is never closed.
    service->active = false; ++hen_close_calls;
}
static Result kaihenGetRoSession(Service* ro) {
    ++ro_calls; assertCheckpoint("checkpoint=before-open-hen\n");
    ro->active = true; ro->identity = 1;
    return 0;
}
static Result kaihenOpenHomebrewSession(Service* ro, Service* hen) {
    assert(ro->active && ro->identity == 1);
    ++open_calls; assertCheckpoint("checkpoint=before-open-hen\n");
    if (block_open) longjmp(simulated_block, 1);
    if (R_SUCCEEDED(open_result)) { hen->active = true; hen->identity = 2; }
    return open_result;
}
static Result henGetManagerDiagnostics(Service* hen, HenManagerDiagnosticsV1* out) {
    assert(hen->active && hen->identity == 2);
    ++diag_calls; assertCheckpoint("checkpoint=before-cached-diagnostics\n");
    assert(!count_calls && !item_calls && !status_calls && !scan_calls && !hash_calls);
    if (block_diag) longjmp(simulated_block, 2);
    *out = supplied;
    return diag_result;
}
static Result henGetManagedPluginCount(Service* hen, u32* count) {
    assert(hen->active && diag_calls == 1);
    assertCheckpoint("checkpoint=before-managed-list\n");
    assert(reportContains("game_steps_suppressed=9\n"));
    ++count_calls; *count = 0; return 0;
}
static Result henGetManagedPluginInfo(Service* hen, u32 index, HenManagedPluginInfo* info) {
    (void)hen; (void)index; (void)info; ++item_calls; return Failed;
}
static Result henGetBackgroundPluginStatus(Service* hen, HenBackgroundPluginStatus* out) {
    assert(hen->active); ++status_calls; *out = supplied.background; return 0;
}
static void listInstalled(FsFileSystem* sd) {
    (void)sd; assertCheckpoint("checkpoint=before-installed-scan\n");
    ++scan_calls;
    if (block_scan) longjmp(simulated_block, 3);
}
static Result hashPlugin(FsFileSystem* sd, const char* name, u8 hash[32]) {
    (void)sd; (void)name; memset(hash, 1, 32); ++hash_calls; return 0;
}
static Result demoRequest(Service* hen, const char* action, const char* name) {
    (void)hen; (void)action; (void)name; ++action_calls; return 0;
}
static Result henCancelBackgroundPlugin(Service* hen) { (void)hen; ++action_calls; return 0; }
static Result henStopManagedPlugin(Service* hen, const HenPluginName* name) {
    (void)hen; (void)name; ++action_calls; return 0;
}
static Result henLaunchBackgroundPlugin(Service* hen, const HenBackgroundPluginRequest* request) {
    (void)hen; (void)request; ++action_calls; return 0;
}
static Result henRestartManagedPlugin(Service* hen, const HenBackgroundPluginRequest* request) {
    (void)hen; (void)request; ++action_calls; return 0;
}

#include "controller_diagnostics_under_test.h"

static void resetController(const char* command) {
    reset();
    ro_calls = open_calls = diag_calls = count_calls = item_calls = status_calls = 0;
    scan_calls = hash_calls = action_calls = hen_close_calls = sd_close_calls = 0;
    open_result = diag_result = 0;
    block_open = block_diag = block_scan = false;
    supplied = (HenManagerDiagnosticsV1){0};
    supplied.version = HEN_MANAGER_DIAGNOSTICS_VERSION;
    supplied.size = sizeof(supplied);
    supplied.build_id = HEN_MANAGER_DIAGNOSTICS_BUILD_ID;
    supplied.flags = HenManagerFlag_ShellAvailable | HenManagerFlag_Quarantined |
                     HenManagerFlag_GateRestored | HenManagerFlag_GameHousekeepingAllowed;
    supplied.background.phase = HenBackgroundPluginPhase_Quarantined;
    supplied.background.gate_step = 0x42;
    supplied.launch_failure_step = 0xabc;
    supplied.game_step_calls = 120;
    supplied.game_steps_suppressed = 9;
    supplied.game_phase = 3;
    supplied.game_start_attempts = 1;
    supplied.pm_queries_started = 4;
    supplied.pm_queries_completed = 4;
    if (command) put_string(0, command);
}
static void assertDiagnosticOnly(void) {
    assert(ro_calls == 1 && open_calls == 1 && diag_calls == 1);
    assert(!count_calls && !item_calls && !status_calls && !scan_calls && !hash_calls && !action_calls);
}

int main(void) {
    _Static_assert(sizeof(HenManagerDiagnosticsV1) == 176, "diagnostics ABI");
    assert(runOriginalIoTests() == 0);

    // An absent command performs only cached diagnostics, even with a
    // quarantined manager. Distinguish code42 from boolean gate ownership.
    resetController(NULL);
    assert(controllerMain() == 0); assertDiagnosticOnly();
    assert(hen_close_calls == 1 && sd_close_calls == 1);
    char expected_build[64];
    snprintf(expected_build, sizeof(expected_build), "hen-build=%u", (unsigned)HEN_MANAGER_DIAGNOSTICS_BUILD_ID);
    assert(reportContains(expected_build));
    assert(reportContains("launch_failure_step=abc (saved before restoration)"));
    assert(reportContains("gate_owned=0 gate_restored=1"));
    assert(reportContains("gate_step=42"));
    assert(reportContains("phase 17 alone does not establish a retained Loader gate"));
    assert(reportContains("checkpoint=cached-diagnostics-returned\n"));
    assert(reportContains("controller-final  result=00000000"));

    resetController("diag\n");
    supplied.flags |= HenManagerFlag_GateOwned;
    assert(controllerMain() == 0); assertDiagnosticOnly();
    assert(reportContains("gate_owned=1 gate_restored=1"));

    // Unknown legacy APIs and malformed newer layouts must not be decoded
    // into false all-zero health reports or fall back to PM-refresh calls.
    resetController("diag\n"); diag_result = Failed;
    supplied.flags = 0xffffffff;
    assert(controllerMain() == (int)Failed); assertDiagnosticOnly();
    assert(reportContains("Diagnostics unavailable"));
    assert(!reportContains("manager flags="));
    assert(reportContains("checkpoint=cached-diagnostics-returned\n"));

    resetController("list\n"); diag_result = Failed;
    assert(controllerMain() == (int)Failed); assertDiagnosticOnly();
    assert(!reportContains("managed plugins:"));

    resetController("diag\n"); ++supplied.version;
    assert(controllerMain() != 0); assertDiagnosticOnly();
    assert(reportContains("Unsupported diagnostics layout"));
    assert(!reportContains("manager flags="));
    resetController("diag\n"); --supplied.size;
    assert(controllerMain() != 0); assertDiagnosticOnly();
    assert(reportContains("Unsupported diagnostics layout"));

    // Simulate a call never returning: the preceding committed checkpoint
    // must exist without any need to complete or unwind that call.
    resetController(NULL); block_open = true;
    if (setjmp(simulated_block) == 0) { (void)controllerMain(); assert(!"open must block"); }
    assert(open_calls == 1 && !diag_calls && !count_calls);
    assertCheckpoint("checkpoint=before-open-hen\n");
    assert(!reportContains("controller-final"));

    resetController("diag\n"); block_diag = true;
    if (setjmp(simulated_block) == 0) { (void)controllerMain(); assert(!"diagnostics must block"); }
    assertDiagnosticOnly(); assertCheckpoint("checkpoint=before-cached-diagnostics\n");
    assert(!reportContains("cached-diagnostics-returned"));

    // A failed report checkpoint refuses dispatch, including launch actions.
    resetController("launch p2ping\n"); failure_operation = "write"; failure_path = ResultTemporary;
    assert(controllerMain() == (int)Failed);
    assert(!ro_calls && !open_calls && !diag_calls && !action_calls && !hash_calls);
    assert(reportContains("dispatch-refused-checkpoint"));

    resetController(NULL); open_result = Failed;
    assert(controllerMain() == (int)Failed && !diag_calls && !hen_close_calls);
    assert(reportContains("open-hen  result="));

    // Explicit list preserves the diagnostic data before later service and
    // directory calls. A blocked scan leaves a useful diagnostic report.
    resetController("list\n");
    assert(controllerMain() == 0);
    assert(diag_calls == 1 && count_calls == 1 && status_calls == 1 && scan_calls == 1);
    assert(!hash_calls && !action_calls);
    resetController("list\n"); block_scan = true;
    if (setjmp(simulated_block) == 0) { (void)controllerMain(); assert(!"scan must block"); }
    assertCheckpoint("checkpoint=before-installed-scan\n");
    assert(reportContains("game_steps_suppressed=9\n") && reportContains("managed plugins: 0\n"));

    resetController("help\n"); assert(controllerMain() == 0 && !ro_calls && !diag_calls);
    assert(reportContains("No command file defaults to diag"));
    resetController("diag extra\n"); assert(controllerMain() != 0 && !ro_calls && !diag_calls);
    assert(reportContains("read/consume/parse-command"));

    puts("controller diagnostic dispatch, checkpoints, unsupported API, and blocked-call evidence: PASS");
    return 0;
}
