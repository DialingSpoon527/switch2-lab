// SPDX-License-Identifier: GPL-2.0-only
// Experimental HEN connection switch: separate from privileged process launch.
#include "control.h"
#include "hen_handoff.hpp"
#include "../../common/control.h"

namespace {
struct ConsolePlatform final {
    using Handle = ::Handle;
    static constexpr Handle Invalid = INVALID_HANDLE;
    bool probeHen(Handle h, std::uint64_t* out_pid) {
        Service borrowed = {};
        serviceCreate(&borrowed, h);
        HenStatus status = {};
        if (R_FAILED(henGetStatus(&borrowed, &status)) ||
            status.protocol_version != HEN_PROTOCOL_VERSION ||
            R_FAILED(status.initialization_result) || !status.process_id)
            return false;
        if (out_pid) *out_pid = status.process_id;
        return true;
    }
    bool processId(Handle h, std::uint64_t* out_pid) {
        return R_SUCCEEDED(svcGetProcessId(out_pid, h));
    }
    void close(Handle h) { svcCloseHandle(h); } // Never send CMIF Close on shared control.
};
ConsolePlatform g_console;
hen_handoff::Manager<ConsolePlatform> g_handoff(g_console);

Result asResult(hen_handoff::Status s) {
    switch (s) {
    case hen_handoff::Status::Ok: return 0;
    case hen_handoff::Status::AlreadyInitialized:
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    case hen_handoff::Status::NotInitialized:
    case hen_handoff::Status::SupervisorUnavailable:
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    default:
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
}
} // namespace

Result roControlAdopt(Handle copied_session, u64 sender_pid, u64* out_hen_pid) {
    return asResult(g_handoff.adoptInitial(copied_session, sender_pid, out_hen_pid));
}
Result roControlAuthorizeSupervisor(u64 supervisor_pid, u64 sender_pid) {
    const auto status = g_handoff.registerSupervisorPid(supervisor_pid, sender_pid);
    if (status == hen_handoff::Status::Ok) return 0;
    return MAKERESULT(0x1EE, 0x100 + static_cast<u32>(status));
}
Result roControlStage(Handle copied_session, u64 sender_pid, u64 generation) {
    return asResult(g_handoff.stage(copied_session, sender_pid, generation));
}
Result roControlSwitch(u64 sender_pid, u64 generation) {
    return asResult(g_handoff.switchToStaged(sender_pid, generation));
}
Result roControlRollback(u64 sender_pid, u64 generation) {
    return asResult(g_handoff.rollback(sender_pid, generation));
}
Result roControlFinalize(u64 sender_pid, u64 generation) {
    return asResult(g_handoff.finalize(sender_pid, generation));
}
bool roControlIsSupervisor(u64 sender_pid) {
    return g_handoff.isSupervisorCaller(sender_pid);
}
Result roControlCreateHomebrewSession(Handle process, Handle* out_client) {
    if (g_handoff.activeHandle() == INVALID_HANDLE)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    // This Service does NOT own the manager's underlying kernel handle.
    Service borrowed = {};
    serviceCreate(&borrowed, g_handoff.activeHandle());
    return henCreateHomebrewSession(&borrowed, process, out_client);
}
