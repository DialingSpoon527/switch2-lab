// SPDX-License-Identifier: GPL-2.0-only
#include "startup_trace.hpp"
#include "../../common/sd_writer.hpp"
#include <switch/arm/counter.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr u32 MaxEntries = 160;
constexpr u64 TracePeriodNs = 250000000ull;
constexpr u32 MaxPeriods = 160; // Keep gathering startup evidence for 40 s.
struct RoStartupLogEntry {
    u64 tick;
    u32 sequence;
    u32 kind;
    u32 port;
    Result result;
    u32 extra;
};
Mutex log_mutex = {};
RoStartupLogEntry events[MaxEntries] = {};
u32 events_written = 0;
u32 sequence = 0;
u64 process_pid = 0;
bool active = false;
Thread log_thread = {};

void writerEntry(void*) {
    // IMPORTANT: the RO thread is concurrently registering services through
    // libnx's global SM session. Never share that session (or its locks) with
    // diagnostics: acquire a separate private SM connection for this worker.
    Handle sm_handle = INVALID_HANDLE;
    Result rc = svcConnectToNamedPort(&sm_handle, "sm:");
    roStartupTraceNote(RoStartupEvent::LoggerSmConnect, 0, rc, 0);
    if (R_FAILED(rc)) return;
    // sm: on Horizon 23 uses TIPC. An HIPC Service dispatch here silently
    // prevented all candidate-side logging in V10.
    TipcService sm = {};
    tipcCreate(&sm, sm_handle);
    const u64 placeholder = 0;
    rc = tipcDispatchIn(&sm, 0, placeholder, .in_send_pid = true);
    roStartupTraceNote(RoStartupEvent::LoggerSmInit, 0, rc, 0);
    Handle fsp_handle = INVALID_HANDLE;
    if (R_SUCCEEDED(rc)) {
        const SmServiceName fsp = smEncodeName("fsp-srv");
        rc = tipcDispatchIn(&sm, 1, fsp,
            .out_handle_attrs = {SfOutHandleAttr_HipcMove},
            .out_handles = &fsp_handle);
        roStartupTraceNote(RoStartupEvent::LoggerFspLookup, 0, rc, 0);
    }
    // tipcCreate borrows the passed kernel handle.
    svcCloseHandle(sm_handle);
    if (R_FAILED(rc) || fsp_handle == INVALID_HANDLE) return;
    Service fs = {};
    serviceCreate(&fs, fsp_handle);
    // SetCurrentProcess. Failure is non-fatal: the logger simply gives up.
    rc = serviceDispatchIn(&fs, 1, placeholder, .in_send_pid = true);
    roStartupTraceNote(RoStartupEvent::LoggerFsInit, 0, rc, 0);
    if (R_FAILED(rc)) { serviceClose(&fs); return; }
    char paths[2][104] = {};
    const int a = std::snprintf(paths[0], sizeof(paths[0]),
        "/kaihen/hotupdate/ro-v11-candidate-%llu-a.txt",
        static_cast<unsigned long long>(process_pid));
    const int b = std::snprintf(paths[1], sizeof(paths[1]),
        "/kaihen/hotupdate/ro-v11-candidate-%llu-b.txt",
        static_cast<unsigned long long>(process_pid));
    if (a <= 0 || b <= 0 || a >= static_cast<int>(sizeof(paths[0])) ||
        b >= static_cast<int>(sizeof(paths[1]))) { serviceClose(&fs); return; }
    u32 last_saved = 0, slot = 0;
    for (u32 period = 0; period < MaxPeriods; ++period) {
        RoStartupLogEntry snapshot[MaxEntries];
        u32 count = 0, latest = 0;
        mutexLock(&log_mutex);
        const u32 end = events_written;
        const u32 start = end > MaxEntries ? end - MaxEntries : 0;
        for (u32 n = start; n < end; ++n)
            snapshot[count++] = events[n % MaxEntries];
        latest = sequence;
        mutexUnlock(&log_mutex);
        if (latest != last_saved) {
            char output[14336] = {};
            int len = std::snprintf(output, sizeof(output),
                "RO_V11_CANDIDATE pid=%llu entries=%u seq=%u\n"
                "kind: 1=MAIN 2=REGISTER_ENTER 3=REGISTER_RETURN 4=YIELD_ENTER "
                "5=YIELD_RETURN 6=RESUME 7=REQUEST 8=ACCEPT_FAIL "
                "9=SESSION_FULL 10=ACCEPT_OK 11=WAIT_ERROR 12=EXIT "
                "13=SM_INIT_ENTER 14=SM_INIT_RETURN 15=LOGGER_SM_CONNECT "
                "16=LOGGER_SM_INIT 17=LOGGER_FSP 18=LOGGER_FS_INIT\n",
                static_cast<unsigned long long>(process_pid), count, latest);
            if (len > 0 && len < static_cast<int>(sizeof(output))) {
                for (u32 i = 0; i < count; ++i) {
                    const RoStartupLogEntry& e = snapshot[i];
                    const int written = std::snprintf(output + len, sizeof(output) - len,
                        "seq=%u tick=%llu kind=%u port=%u rc=%08x extra=%u\n",
                        e.sequence, static_cast<unsigned long long>(e.tick),
                        e.kind, e.port, e.result, e.extra);
                    if (written <= 0 || static_cast<size_t>(written) >= sizeof(output) - len) break;
                    len += written;
                }
                SdWriter writer;
                Result write_result = writer.begin(fs.session, paths[slot], len);
                if (R_SUCCEEDED(write_result)) write_result = writer.write(output, len);
                if (R_SUCCEEDED(write_result)) write_result = writer.finish();
                if (R_SUCCEEDED(write_result)) {
                    last_saved = latest;
                    slot ^= 1; // Alternate snapshots; never erase both at once.
                }
            }
        }
        svcSleepThread(TracePeriodNs);
    }
    serviceClose(&fs);
}
} // namespace

void roStartupTraceNote(RoStartupEvent event, u32 port, Result result, u32 extra) {
    if (!__atomic_load_n(&active, __ATOMIC_ACQUIRE)) return;
    mutexLock(&log_mutex);
    RoStartupLogEntry& e = events[events_written++ % MaxEntries];
    e.tick = armGetSystemTick();
    e.sequence = ++sequence;
    e.kind = static_cast<u32>(event);
    e.port = port;
    e.result = result;
    e.extra = extra;
    mutexUnlock(&log_mutex);
}

void roStartupTraceStart(u64 pid) {
    mutexInit(&log_mutex);
    process_pid = pid;
    __atomic_store_n(&active, true, __ATOMIC_RELEASE);
    roStartupTraceNote(RoStartupEvent::MainEntered, 0, 0, 0);
    // The process can still run if the worker cannot be created or started.
    Result rc = threadCreate(&log_thread, writerEntry, nullptr,
                             nullptr, 0x8000, 0x38, -2);
    if (R_FAILED(rc)) { roStartupTraceNote(RoStartupEvent::WaitError, 0, rc, 1); return; }
    rc = threadStart(&log_thread);
    if (R_FAILED(rc)) {
        roStartupTraceNote(RoStartupEvent::WaitError, 0, rc, 2);
        threadClose(&log_thread);
    }
}
