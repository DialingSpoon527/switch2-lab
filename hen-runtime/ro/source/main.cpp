// SPDX-License-Identifier: GPL-2.0-only
#include <cstring>
#include <switch.h>
#include "handle.hpp"
#include "fs_memory.h"
#include "session.h"
#include "wire.h"
#include "control.h"
#include "startup_trace.hpp"

namespace {

constexpr u32 MaxSessions = 16;
constexpr u32 PrimaryPortCount = 3;
constexpr u32 PortCount = 4;
constexpr u32 PrivatePort = 3;
constexpr u64 RegistrationRetryNs = 50000000;

// Native RO registers ldr:ro in service space sets 1 and 2. Its private
// ro:dmnt endpoint belongs to set 1.
struct RoPort {
    const char* name;
    u32 space;
    bool fs_memory; ///< ro:dmnt: FS memory access and diagnostics.
};
constexpr RoPort ServicePorts[PortCount] = {
    {"ldr:ro", 1, false},
    {"ldr:ro", 2, false},
    {"ro:dmnt", 1, true},
    {nullptr, 1, false},
};

class RoServer {
  public:
    void run() {
        u64 pid = 0;
        const Result pid_result = svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
        if (R_FAILED(pid_result)) {
            roStartupTraceNote(RoStartupEvent::WaitError, 0, pid_result, 3);
            return;
        }
        private_name_[0] = 'k';
        for (u32 i = 0; i < 7; ++i)
            private_name_[i + 1] = "0123456789abcdef"[(pid >> ((6-i)*4)) & 15];
        private_name_[8] = 0;
        while (!stop_) {
            registerPorts();
            serveOnce();
        }
    }

  private:
    void registerPorts() {
        for (u32 i = 0; i < PortCount; i++) {
            // V8+ uses the authenticated ldr:ro session for management.
            // The legacy PID-private name was absent in the observed V8
            // process and is not needed for replacement. Do not let an
            // optional SM registration block the main RO service loop.
            if (i == PrivatePort) continue;
            if (i < PrimaryPortCount && suspended_) continue;
            if (ports_[i].get() != INVALID_HANDLE) {
                continue;
            }

            const u32 attempt = ++registration_attempts_[i];
            if (attempt == 1 || attempt % 20 == 0)
                roStartupTraceNote(RoStartupEvent::RegisterEnter, i, 0, attempt);
            Handle port = INVALID_HANDLE;
            Result rc =
                smRegisterServiceInSpace(&port, ServicePorts[i].space,
                                         smEncodeName(i == PrivatePort ? private_name_
                                                                         : ServicePorts[i].name), false, 4);
            if (R_SUCCEEDED(rc)) {
                ports_[i].reset(port);
            }
            if (attempt == 1 || attempt % 20 == 0 || R_SUCCEEDED(rc) ||
                rc != last_registration_[i])
                roStartupTraceNote(RoStartupEvent::RegisterReturn, i, rc, attempt);
            last_registration_[i] = rc;
        }
    }

    Result yieldPrimary() {
        roStartupTraceNote(RoStartupEvent::YieldEnter, 0, 0, static_cast<u32>(portMask()));
        suspended_ = true;
        Result error = 0;
        for (u32 i = 0; i < PrimaryPortCount; ++i) {
            if (ports_[i].get() == INVALID_HANDLE) continue;
            Result rc = smUnregisterServiceInSpace(ServicePorts[i].space,
                         smEncodeName(ServicePorts[i].name));
            if (R_SUCCEEDED(rc)) ports_[i].reset();
            else if (R_SUCCEEDED(error)) error = rc;
        }
        // Failure may have removed only a subset of the primary ports.
        // Let the old RO immediately reclaim all missing names rather than
        // leaving it permanently suspended without a controller rollback.
        if (R_FAILED(error)) suspended_ = false;
        roStartupTraceNote(RoStartupEvent::YieldReturn, 0, error,
                           static_cast<u32>(portMask()));
        return error;
    }
    void resumePrimary() {
        suspended_ = false;
        roStartupTraceNote(RoStartupEvent::Resume, 0, 0, static_cast<u32>(portMask()));
    }
    u64 portMask() const {
        u64 mask = 0;
        for (u32 i = 0; i < PrimaryPortCount; ++i)
            if (ports_[i].get() != INVALID_HANDLE) mask |= (1ull << i);
        if (suspended_) mask |= 0x100;
        // Diagnostic bit: the PID-private SM registration actually exists.
        if (ports_[PrivatePort].get() != INVALID_HANDLE) mask |= 0x200;
        return mask;
    }
    bool privateCommand(u32 client, void* message) {
        RoRequest request = {};
        if (!roParseRequest(message, &request)) return false;
        if (request.cmd_id != RoCmd_YieldPrimaryPorts &&
            request.cmd_id != RoCmd_ResumePrimaryPorts &&
            request.cmd_id != RoCmd_GetPrimaryPortMask &&
            request.cmd_id != RoCmd_ExitWhenYielded) return false;
        // Keep the old ldr:ro session usable for control after bootstrap has
        // authenticated the supervisor PID. Other ldr clients stay restricted
        // to the normal loader commands. No new privileged public endpoint.
        if (clients_[client].port_index != PrivatePort &&
            !roControlIsSupervisor(request.pid)) return false;
        roStartupTraceNote(RoStartupEvent::Request, request.cmd_id, 0,
                           static_cast<u32>(portMask()));
        Result rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        if (roControlIsSupervisor(request.pid)) {
            switch (request.cmd_id) {
            case RoCmd_YieldPrimaryPorts: rc = yieldPrimary(); break;
            case RoCmd_ResumePrimaryPorts: resumePrimary(); rc = 0; break;
            case RoCmd_GetPrimaryPortMask: rc = 0; break;
            case RoCmd_ExitWhenYielded:
                if (suspended_ && !(portMask() & 7)) { stop_ = true; rc = 0; }
                break;
            default: break;
            }
        }
        roWriteReply(message, rc, portMask(),
                     request.cmd_id == RoCmd_GetPrimaryPortMask);
        return true;
    }
    void acceptSession(u32 port) {
        Handle session = INVALID_HANDLE;
        Result rc = svcAcceptSession(&session, ports_[port].get());
        if (R_FAILED(rc)) {
            roStartupTraceNote(RoStartupEvent::AcceptFailure, port, rc, 0);
            return;
        }

        for (u32 i = 0; i < MaxSessions; i++) {
            if (sessions_[i].get() == INVALID_HANDLE) {
                sessions_[i].reset(session);
                fs_memory_[i] = ServicePorts[port].fs_memory;
                clients_[i].port_index = port;
                roRecordAcceptedSession(port);
                roStartupTraceNote(RoStartupEvent::SessionAccepted, port, 0, i);
                return;
            }
        }
        roStartupTraceNote(RoStartupEvent::SessionFull, port, 0, MaxSessions);
        svcCloseHandle(session);
    }

    void closeSession(u32 slot) {
        // roSessionClose always releases the client, so the slot can serve
        // the next one; RoDiagnostics counts views it could not unmap.
        if (!fs_memory_[slot]) {
            roSessionClose(&clients_[slot]);
        }
        fs_memory_[slot] = false;
        sessions_[slot].reset();
    }

    void serveOnce() {
        Handle handles[PortCount + MaxSessions];
        u32 slots[PortCount + MaxSessions];
        s32 count = 0;

        for (u32 i = 0; i < PortCount; i++) {
            if (ports_[i].get() != INVALID_HANDLE) {
                handles[count] = ports_[i].get();
                slots[count++] = i;
            }
        }
        for (u32 i = 0; i < MaxSessions; i++) {
            if (sessions_[i].get() != INVALID_HANDLE) {
                handles[count] = sessions_[i].get();
                slots[count++] = PortCount + i;
            }
        }
        if (!count) {
            svcSleepThread(RegistrationRetryNs);
            return;
        }

        void* message = armGetTls();
        std::memset(message, 0, 0x100);
        s32 index = -1;
        Result rc = svcReplyAndReceive(&index, handles, count, INVALID_HANDLE, RegistrationRetryNs);
        if (index < 0 || index >= count) {
            // A normal 50ms timeout isn't interesting, but a different
            // kernel error may explain why candidate initialization stalls.
            if (R_FAILED(rc) && rc != KERNELRESULT(TimedOut) &&
                rc != previous_wait_result_) {
                roStartupTraceNote(RoStartupEvent::WaitError, 0, rc, 4);
                previous_wait_result_ = rc;
            }
            return;
        }

        const u32 slot = slots[index];
        if (slot < PortCount) {
            if (R_SUCCEEDED(rc)) {
                acceptSession(slot);
            }
            return;
        }

        const u32 client = slot - PortCount;
        if (R_FAILED(rc)) {
            if (rc == KERNELRESULT(ConnectionClosed)) {
                closeSession(client);
            }
            return;
        }
        if (hipcParseRequest(message).meta.type == CmifCommandType_Close) {
            closeSession(client);
            return;
        }

        Handle copied_client = INVALID_HANDLE;
        if (fs_memory_[client]) {
            roFsMemoryDispatch(message);
        } else if (!privateCommand(client, message)) {
            copied_client = roDispatch(clients_, MaxSessions, client, message);
        }
        rc = svcReplyAndReceive(&index, nullptr, 0, sessions_[client].get(), 0);
        if (copied_client != INVALID_HANDLE) {
            svcCloseHandle(copied_client);
        }
        if (rc == KERNELRESULT(ConnectionClosed)) {
            closeSession(client);
        }
    }

    Result previous_wait_result_ = 0;
    u32 registration_attempts_[PortCount] = {};
    Result last_registration_[PortCount] = {};
    bool suspended_ = false;
    bool stop_ = false;
    char private_name_[9] = {};
    ounce::HandleOwner ports_[PortCount];
    ounce::HandleOwner sessions_[MaxSessions];
    RoSession clients_[MaxSessions] = {};
    bool fs_memory_[MaxSessions] = {};
};

RoServer g_server;

} // namespace

// libounce-nx runtime hooks must have C linkage.
extern "C" {
u32 __nx_applet_type = AppletType_None;
size_t __nx_heap_size = 0x200000;

void __appInit(void) {
}

void __appExit(void) {
}
}

int main(int argc, char** argv) {
    NX_IGNORE_ARG(argc);
    NX_IGNORE_ARG(argv);
    // Start the trace before SM initialization: an SM failure previously
    // produced no candidate-side evidence at all.
    u64 pid = 0;
    if (R_SUCCEEDED(svcGetProcessId(&pid, CUR_PROCESS_HANDLE)) && pid)
        roStartupTraceStart(pid);
    roStartupTraceNote(RoStartupEvent::SmInitEnter, 0, 0, 0);
    Result rc = smInitialize();
    roStartupTraceNote(RoStartupEvent::SmInitReturn, 0, rc, 0);
    if (R_FAILED(rc)) {
        // Give the independent writer a chance to persist a startup failure;
        // never require it for normal RO execution.
        svcSleepThread(3000000000ull);
        return static_cast<int>(rc);
    }
    g_server.run();
    roStartupTraceNote(RoStartupEvent::Exiting, 0, 0, 0);
}
