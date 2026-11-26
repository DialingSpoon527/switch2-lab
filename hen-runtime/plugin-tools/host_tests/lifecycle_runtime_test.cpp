// SPDX-License-Identifier: ISC
// Executes actual generated plugin code and libounce CMIF encoding on the host.
// Only kernel/session/event/thread operations are fixture implementations.
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "plugin_runtime.cpp"
#include "plugin_server.cpp"

namespace fixture {
alignas(16) thread_local u8 message[0x100] = {};
std::mutex mailbox_lock;
std::atomic<bool> signaled{false};
std::atomic<unsigned> signals{0};
bool fail_signal = false;
bool fail_clear = false;
unsigned callbacks = 0;
std::thread::id callback_thread;
Result sm_result = 0;
unsigned sm_calls = 0;
unsigned close_calls = 0;
unsigned unregister_calls = 0;
std::vector<Handle> closed;

void request(u32 command, const void* data, u32 size, u32 handles = 0,
             bool pid = false, u32 context = 0) {
    std::memset(message, 0, sizeof(message));
    CmifRequest built = cmifMakeRequest(message, (CmifRequestFormat){
        .request_id = command, .context = context, .data_size = size,
        .num_handles = handles, .send_pid = pid,
    });
    if (size) std::memcpy(built.data, data, size);
    for (u32 n = 0; n < handles; ++n) built.hipc.copy_handles[n] = 0x100 + n;
}

void callback(const KaihenPluginShutdownRequest* request, void*) {
    assert(request->reason == KaihenPluginShutdown_UserStop);
    ++callbacks;
    callback_thread = std::this_thread::get_id();
}

Result echo(u32 command, const void* in, size_t size, void* out,
            size_t* out_size, void*) {
    if (command == 0 && size == sizeof(u32)) {
        u32 value = 0;
        std::memcpy(&value, in, sizeof(value));
        ++value;
        std::memcpy(out, &value, sizeof(value));
        *out_size = sizeof(value);
        return 0;
    }
    if (command == 2 && !in && size == 0) {
        *out_size = 0;
        return 0;
    }
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}
} // namespace fixture

extern "C" {
void* armGetTls(void) { return fixture::message; }
void mutexLock(Mutex*) { fixture::mailbox_lock.lock(); }
void mutexUnlock(Mutex*) { fixture::mailbox_lock.unlock(); }
Result svcSignalEvent(Handle) {
    if (fixture::fail_signal) return 0xBAD;
    fixture::signaled.store(true);
    ++fixture::signals;
    return 0;
}
Result svcClearEvent(Handle) {
    if (fixture::fail_clear) return 0xBAD;
    fixture::signaled.store(false);
    return 0;
}
Result svcCloseHandle(Handle handle) {
    ++fixture::close_calls;
    fixture::closed.push_back(handle);
    return 0;
}
Result svcCreateEvent(Handle* write, Handle* read) {
    static Handle next = 1000;
    *write = next++;
    *read = next++;
    return 0;
}
Result svcCreateSession(Handle* server, Handle* client, u32, u64) {
    *server = 2000; *client = 2001; return 0;
}
Result svcWaitSynchronization(s32* index, const Handle*, s32, u64) {
    if (index) *index = 0;
    return 0;
}
Result svcReplyAndReceive(s32* index, const Handle*, s32, Handle, u64) {
    if (index) *index = -1;
    return KERNELRESULT(TimedOut);
}
Result svcAcceptSession(Handle* session, Handle) { *session = 42; return 0; }
Result smInitialize(void) { ++fixture::sm_calls; return fixture::sm_result; }
void smExit(void) {}
void smAddOverrideHandle(SmServiceName, Handle) {}
Result smRegisterServiceInSpace(Handle* port, u32, SmServiceName, bool, s32) {
    *port = 3; return 0;
}
Result smUnregisterServiceInSpace(u32, SmServiceName) {
    ++fixture::unregister_calls; return 0;
}
}

static void test_wire() {
    // Real libounce encodes scalar payloads using alignment budget that the
    // original server forwarded as user bytes. Test all supported handle/PID
    // alignments and context request form, plus zero, 24 and 48 byte payloads.
    const u8 bytes[96] = {};
    static_assert(sizeof(HenBackgroundPluginRequest) == 96);
    static_assert(sizeof(HenBackgroundPluginStatus) == 32);
    static_assert(sizeof(HenManagedPluginInfo) == 96);
    static_assert(offsetof(HenManagedPluginInfo, shutdown_flags) == 92);
    for (u32 size : {0u, 4u, 16u, 24u, 48u, 64u, 68u, 72u, 96u}) {
        for (u32 handles : {0u, 1u, 2u, 3u}) {
            for (bool pid : {false, true}) {
                for (u32 context : {0u, 7u}) {
                    fixture::request(9, bytes, size, handles, pid, context);
                    const auto parsed = ounce::parseRequest(fixture::message);
                    size_t actual = 999;
                    assert(ounce::inlinePayloadSize(parsed, &actual));
                    assert(actual == size);
                }
            }
        }
    }
    fixture::request(0, bytes, 4);
    auto parsed = ounce::parseRequest(fixture::message);
    assert(parsed.data_size > 4); // Confirms the original failure condition.
    size_t actual = 999;
    parsed.hipc.meta.num_send_buffers = 1;
    assert(!ounce::inlinePayloadSize(parsed, &actual) && actual == 0);
    parsed.hipc.meta.num_send_buffers = 0;
    parsed.hipc.meta.num_data_words = 0;
    assert(!ounce::inlinePayloadSize(parsed, &actual));
    parsed.hipc.meta.num_data_words = UINT32_MAX;
    assert(!ounce::inlinePayloadSize(parsed, &actual));
    parsed.hipc.meta.num_data_words = 9;
    parsed.data_size = 3;
    assert(!ounce::inlinePayloadSize(parsed, &actual));
    assert(!ounce::inlinePayloadSize(parsed, nullptr));

    PluginService service = {};
    service.handler = fixture::echo;
    const u32 input = 41;
    fixture::request(0, &input, sizeof(input));
    u8 output[KAIHEN_PLUGIN_MESSAGE_SIZE] = {};
    size_t out_size = 0;
    assert(callHandler(&service, ounce::parseRequest(fixture::message), output, &out_size) == 0);
    u32 answer = 0;
    std::memcpy(&answer, output, sizeof(answer));
    assert(out_size == 4 && answer == 42);
    fixture::request(2, nullptr, 0);
    assert(callHandler(&service, ounce::parseRequest(fixture::message), output, &out_size) == 0);
    assert(out_size == 0);
    fixture::request(0, bytes, 68);
    assert(R_FAILED(callHandler(&service, ounce::parseRequest(fixture::message), output, &out_size)));

    KaihenPluginShutdownRequest stop = {1, sizeof(stop), KaihenPluginShutdown_UserStop, 2500, 1};
    fixture::request(KAIHEN_PLUGIN_CONTROL_REQUEST_SHUTDOWN, &stop, sizeof(stop));
    assert(isLifecycleShutdown(ounce::parseRequest(fixture::message)));
    fixture::request(KAIHEN_PLUGIN_CONTROL_REQUEST_SHUTDOWN, &stop, sizeof(stop), 1);
    assert(!isLifecycleShutdown(ounce::parseRequest(fixture::message)));
    fixture::request(KAIHEN_PLUGIN_CONTROL_REQUEST_SHUTDOWN, &stop, sizeof(stop)-4);
    assert(!isLifecycleShutdown(ounce::parseRequest(fixture::message)));
    std::puts("wire: encoder alignment, malformed inputs, 41->42 and empty context query PASS");
}

static void test_context() {
    KaihenPluginLaunchContext startup = {1, sizeof(startup), KaihenPluginStart_Bootstrap,
        KaihenPluginReason_BootPlugin, KaihenPluginSession_InitialActivation,
        KAIHEN_PLUGIN_FIRST_START_THIS_HEN_INSTANCE, 1, 0, 0, 1234};
    fixture::request(PluginStartupCommand_AdoptHandles, &startup, sizeof(startup), 2);
    auto parsed = ounce::parseRequest(fixture::message);
    assert(hasLifecyclePayload(parsed));
    AdoptReply reply;
    Handle release = 0;
    assert(adoptHandles(fixture::message, parsed, &reply, &release) == 0);
    assert(g_has_launch_context && g_launch_context.process_id == 1234);
    g_initialized = true;
    KaihenPluginLaunchContext out = {};
    assert(pluginRuntimeGetLaunchContext(&out) == 0);
    assert(out.start_source == KaihenPluginStart_Bootstrap && out.launch_number == 1);
    assert(R_FAILED(pluginRuntimeGetLaunchContext(nullptr)));
    const unsigned calls = fixture::sm_calls;
    Handle ready = 55;
    assert(R_FAILED(pluginRuntimeInitialize(&ready)) && fixture::sm_calls == calls);
    assert(ready == 55); // A repeat call does not invalidate an existing runtime.
    pluginRuntimeExit();
    assert(!g_has_launch_context && !g_initialized);
    out = startup;
    assert(R_FAILED(pluginRuntimeGetLaunchContext(&out)) && out.process_id == 0);

    // No optional payload from an old bootstrap must not reuse stale context.
    fixture::request(PluginStartupCommand_AdoptHandles, nullptr, 0, 2);
    parsed = ounce::parseRequest(fixture::message);
    assert(!hasLifecyclePayload(parsed));
    assert(adoptHandles(fixture::message, parsed, &reply, &release) == 0);
    assert(!g_has_launch_context);
    pluginRuntimeExit();
    startup.reserved = 1;
    fixture::request(PluginStartupCommand_AdoptHandles, &startup, sizeof(startup), 2);
    parsed = ounce::parseRequest(fixture::message);
    assert(adoptHandles(fixture::message, parsed, &reply, &release) == 0);
    assert(!g_has_launch_context);
    pluginRuntimeExit();
    assert(R_FAILED(pluginRuntimeInitialize(nullptr)));
    assert(R_FAILED(pluginRuntimeInitializeWithService(&ready, nullptr)));
    assert(fixture::sm_calls == calls);
    // A failed handoff releases the temporary service before returning.
    const unsigned unregister_before = fixture::unregister_calls;
    assert(R_FAILED(pluginRuntimeInitialize(&ready)));
    assert(ready == INVALID_HANDLE);
    assert(fixture::unregister_calls == unregister_before + 1);
    std::puts("runtime: launch context, old bootstrap, bad metadata, init guards and failed-handoff cleanup PASS");
}

static void test_mailbox() {
    PluginService service = {};
    service.started = true;
    service.lifecycle_read = 51;
    service.lifecycle_write = 52;
    pluginServiceSetShutdownHandler(&service, fixture::callback, nullptr);
    assert(pluginServiceLifecycleEvent(&service) == 51);
    assert(pluginServiceLifecycleEvent(nullptr) == INVALID_HANDLE);
    KaihenPluginShutdownRequest stop = {1, sizeof(stop), KaihenPluginShutdown_UserStop, 2500, 1};
    auto invalid = stop;
    invalid.version = 99;
    assert(R_FAILED(noteLifecycleShutdown(&service, &invalid)));
    assert(!service.lifecycle_has_pending);
    fixture::fail_signal = true;
    assert(R_FAILED(noteLifecycleShutdown(&service, &stop)));
    assert(!service.lifecycle_has_request && !service.lifecycle_has_pending);
    fixture::fail_signal = false;
    assert(noteLifecycleShutdown(&service, &stop) == 0);
    const unsigned signals = fixture::signals.load();
    assert(noteLifecycleShutdown(&service, &stop) == 0 && fixture::signals == signals);
    auto next = stop;
    next.sequence = 2;
    assert(R_FAILED(noteLifecycleShutdown(&service, &next)));
    assert(service.lifecycle_pending.sequence == 1);
    KaihenPluginShutdownRequest taken = {};
    fixture::fail_clear = true;
    assert(!pluginServiceTakeLifecycle(&service, &taken) && service.lifecycle_has_pending);
    fixture::fail_clear = false;
    assert(pluginServiceDispatchLifecycle(&service));
    assert(fixture::callbacks == 1 && fixture::callback_thread == std::this_thread::get_id());
    assert(!pluginServiceDispatchLifecycle(&service));
    assert(noteLifecycleShutdown(&service, &stop) == 0 && !service.lifecycle_has_pending);
    stop.reason = KaihenPluginShutdown_Restart;
    assert(R_FAILED(noteLifecycleShutdown(&service, &stop)));
    fixture::fail_signal = true;
    assert(R_FAILED(noteLifecycleShutdown(&service, &next)) && !service.lifecycle_has_pending);
    assert(service.lifecycle_pending.sequence == 1);
    fixture::fail_signal = false;
    assert(noteLifecycleShutdown(&service, &next) == 0);
    assert(pluginServiceTakeLifecycle(&service, &taken) && taken.sequence == 2);
    assert(!fixture::signaled.load());

    constexpr unsigned count = 2000;
    std::thread producer([&] {
        for (unsigned i = 0; i < count; ++i) {
            auto value = next;
            value.sequence = 3 + i;
            while (R_FAILED(noteLifecycleShutdown(&service, &value))) std::this_thread::yield();
        }
    });
    for (unsigned i = 0; i < count; ++i) {
        while (!pluginServiceTakeLifecycle(&service, &taken)) std::this_thread::yield();
        assert(taken.sequence == 3 + i);
    }
    producer.join();
    assert(!service.lifecycle_has_pending && !fixture::signaled.load());
    service.started = false;
    assert(!pluginServiceTakeLifecycle(&service, &taken));
    assert(pluginServiceLifecycleEvent(&service) == INVALID_HANDLE);
    std::puts("mailbox: retries, duplicates, pending backpressure, callback thread and 2000 concurrent notices PASS");
}

int main() {
    test_wire();
    test_context();
    test_mailbox();
    std::puts("generated lifecycle runtime behavioral tests: PASS");
}
