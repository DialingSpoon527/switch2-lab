// SPDX-License-Identifier: MIT
// Project 2 lifecycle demo: a normal kaiHEN plugin. It does not need a custom
// process launcher or privileged handles. Commands: 0 = ping, 1 = exit,
// 2 = return launch context (48 bytes). Shutdown request is delivered on
// the PRIVATE plugin control session by HEN, not the public ping session.
#include <string.h>
#include <switch.h>
#include <kaihen/plugin.h>

u32 __nx_applet_type = AppletType_None;
size_t __nx_heap_size = 0x200000;
void __appInit(void) {}
void __appExit(void) {}

typedef struct {
    Handle exit_write;
    KaihenPluginLaunchContext startup;
    bool has_startup;
    u32 shutdown_reason;
} DemoContext;

static void onShutdown(const KaihenPluginShutdownRequest* request, void* argument) {
    DemoContext* ctx = (DemoContext*)argument;
    ctx->shutdown_reason = request->reason;
    // Clean up additional threads/resources here on the plugin's main thread.
    // An eventual return from main + pluginRuntimeExit is a clean stop.
    svcSignalEvent(ctx->exit_write);
}

static Result pingCommand(u32 id, const void* in, size_t size,
                          void* out, size_t* out_size, void* argument) {
    DemoContext* ctx = (DemoContext*)argument;
    if (id == 0 && size == sizeof(u32)) {
        u32 x = 0;
        memcpy(&x, in, sizeof(x));
        ++x;
        memcpy(out, &x, sizeof(x));
        *out_size = sizeof(x);
        return 0;
    }
    if (id == 1 && size == 0) {
        *out_size = 0;
        return svcSignalEvent(ctx->exit_write);
    }
    if (id == 2 && size == 0 && ctx->has_startup) {
        memcpy(out, &ctx->startup, sizeof(ctx->startup));
        *out_size = sizeof(ctx->startup);
        return 0;
    }
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

int main(void) {
    Handle ready = INVALID_HANDLE;
    Handle control = INVALID_HANDLE;
    Result rc = pluginRuntimeInitializeWithService(&ready, &control);
    if (R_FAILED(rc)) return rc;

    Handle exit_write = INVALID_HANDLE;
    Handle exit_read = INVALID_HANDLE;
    DemoContext state = {};
    PluginService server = {};
    state.has_startup = R_SUCCEEDED(pluginRuntimeGetLaunchContext(&state.startup));

    rc = svcCreateEvent(&exit_write, &exit_read);
    if (R_SUCCEEDED(rc)) {
        state.exit_write = exit_write;
        rc = pluginServiceStart(&server, control, pingCommand, &state);
    }
    if (R_SUCCEEDED(rc)) {
        pluginServiceSetShutdownHandler(&server, onShutdown, &state);
        rc = svcSignalEvent(ready);
    }
    // The callback executes here, not in the control server thread.
    while (R_SUCCEEDED(rc)) {
        const Result dying = svcWaitSynchronizationSingle(exit_read, 0);
        if (R_SUCCEEDED(dying)) break;
        if (dying != KERNELRESULT(TimedOut)) {
            rc = dying;
            break;
        }
        rc = svcWaitSynchronizationSingle(pluginServiceLifecycleEvent(&server),
                                          100000000ll);
        if (rc == KERNELRESULT(TimedOut)) rc = 0;
        if (R_SUCCEEDED(rc)) pluginServiceDispatchLifecycle(&server);
    }

    if (server.started) {
        const Result stop_rc = pluginServiceStop(&server);
        if (R_SUCCEEDED(rc)) rc = stop_rc;
    }
    if (exit_write != INVALID_HANDLE) svcCloseHandle(exit_write);
    if (exit_read != INVALID_HANDLE) svcCloseHandle(exit_read);
    pluginRuntimeExit();
    return rc;
}
