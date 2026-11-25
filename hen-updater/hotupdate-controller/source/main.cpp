// SPDX-License-Identifier: GPL-2.0-only
// HEN-only controller. RO backend updates have a separate controller/protocol.
// No immediate native RESTART_* commands; all requests are deferred.
#include <cstdio>
#include <cstring>
#include <switch.h>
#include <kaihen.h>
#include "../../hotupdate-supervisor/source/deferred_protocol.hpp"
#include "../../hotupdate-supervisor/source/restart_controller.hpp"
#include "hen_request.hpp"
#include "release_version.hpp"

namespace {
constexpr char RequestPath[] = "/kaihen/hotupdate/hen-command.txt";
constexpr char ResultPath[] = "/kaihen/hotupdate/hen-result.txt";
constexpr char PackagePath[] = "/kaihen/hotupdate/hen-package.txt";

struct SupervisorStatus {
    u32 protocol;
    u32 capabilities;
    u32 missing;
    u32 restart_in_progress;
    u64 process_id;
    u64 pings;
    Result init_result;
    Result latest_result;
    u64 latest_pid;
    u64 generation;
};
static_assert(sizeof(SupervisorStatus) <= KAIHEN_PLUGIN_MESSAGE_SIZE);

using Request = hotupdate::henonly::Request;
Request readRequest(FsFileSystem* sd) {
    FsFile input = {};
    const Result open_rc = fsFsOpenFile(sd, RequestPath, FsOpenMode_Read, &input);
    if (R_FAILED(open_rc))
        return open_rc == MAKERESULT(2, 1) ? Request::Status : Request::Invalid;
    char buffer[48] = {};
    s64 size = 0;
    Result rc = fsFileGetSize(&input, &size);
    if (R_SUCCEEDED(rc) && size > 0 && size < static_cast<s64>(sizeof(buffer))) {
        u64 read = 0;
        rc = fsFileRead(&input, 0, buffer, static_cast<size_t>(size),
                        FsReadOption_None, &read);
        if (R_SUCCEEDED(rc) && read != static_cast<u64>(size))
            rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    } else rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    fsFileClose(&input);
    return R_FAILED(rc) ? Request::Invalid
        : hotupdate::henonly::parseRequest(buffer, static_cast<size_t>(size));
}

Result readPackage(FsFileSystem* sd, u8 expected[32]) {
    if (!sd || !expected) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    FsFile file = {};
    Result rc = fsFsOpenFile(sd, PackagePath, FsOpenMode_Read, &file);
    if (R_FAILED(rc)) return rc;
    char text[128] = {};
    s64 size = 0;
    rc = fsFileGetSize(&file, &size);
    if (R_SUCCEEDED(rc) && size > 0 && size <= static_cast<s64>(sizeof(text))) {
        u64 n = 0;
        rc = fsFileRead(&file, 0, text, static_cast<size_t>(size),
                        FsReadOption_None, &n);
        if (R_SUCCEEDED(rc) && n != static_cast<u64>(size))
            rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    } else rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    fsFileClose(&file);
    if (R_FAILED(rc)) return rc;
    return hotupdate::henonly::parseManifest(text, static_cast<size_t>(size), expected)
        ? 0 : MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

Result writeLog(FsFileSystem* sd, const char* buffer, size_t size) {
    if (!size) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    fsFsDeleteFile(sd, ResultPath);
    Result rc = fsFsCreateFile(sd, ResultPath, size, 0);
    if (R_FAILED(rc)) return rc;
    FsFile file = {};
    rc = fsFsOpenFile(sd, ResultPath, FsOpenMode_Write, &file);
    if (R_SUCCEEDED(rc)) {
        rc = fsFileWrite(&file, 0, buffer, size, FsWriteOption_Flush);
        fsFileClose(&file);
        if (R_SUCCEEDED(rc)) rc = fsFsCommit(sd);
    }
    return rc;
}

void appendDeferred(char* buffer, size_t capacity,
                    const hotupdate::DeferredReport& d) {
    const size_t n = std::strlen(buffer);
    if (n >= capacity) return;
    std::snprintf(buffer + n, capacity - n,
        "DEFERRED protocol=%u ticket=%u phase=%u rc=%08x host=%llu exited=%u "
        "hen_flags=%x hen_phase=%u hen_result=%08x "
        "mounts=%u game_pid=%llu elapsed_ms=%u checks=%u kind=%u\n",
        d.protocol, d.ticket, d.phase, d.result,
        static_cast<unsigned long long>(d.host_pid), d.host_exited,
        d.hen_flags, d.hen_phase, d.hen_result,
        d.hen_mounts, static_cast<unsigned long long>(d.hen_process_id),
        d.elapsed_ms, d.checks, d.kind);
}
} // namespace

int main(void) {
    FsFileSystem sd = {};
    Result rc = fsOpenSdCardFileSystem(&sd);
    if (R_FAILED(rc)) return static_cast<int>(rc);
    Request request = readRequest(&sd);
    char result[1024] = {};
    u8 package_digest[32] = {};
    const bool via_package = request == Request::ApplyManifest;
    if (request == Request::Invalid) {
        std::snprintf(result, sizeof(result), "INVALID HEN COMMAND: no operation attempted.\n");
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        goto log_and_exit;
    }
    if (request != Request::Status) {
        // ALL mutating commands, INCLUDING manifest apply, are one-shot.
        // Consume and commit before parsing a package or doing ANY service IPC.
        // No reboot/relaunch can silently apply the same command twice.
        rc = fsFsDeleteFile(&sd, RequestPath);
        if (R_SUCCEEDED(rc)) rc = fsFsCommit(&sd);
        if (R_FAILED(rc)) {
            std::snprintf(result, sizeof(result),
                          "ABORT: could not consume HEN command (%08x)\n", rc);
            goto log_and_exit;
        }
    }
    if (via_package) {
        const Result package_rc = readPackage(&sd, package_digest);
        if (R_FAILED(package_rc)) {
            rc = package_rc;
            std::snprintf(result, sizeof(result),
                "INVALID HEN PACKAGE: %08x (command consumed, nothing queued).\n", rc);
            goto log_and_exit;
        }
        request = Request::QueueHen;
    }
    {
        Service ro = {}, hen = {}, plugin = {};
        rc = kaihenGetRoSession(&ro);
        if (R_SUCCEEDED(rc)) rc = kaihenOpenHomebrewSession(&ro, &hen);
        HenPluginName name = {};
        std::memcpy(name.name, "hotupdate-supervisor",
                    sizeof("hotupdate-supervisor"));
        if (R_SUCCEEDED(rc)) rc = henOpenPlugin(&hen, &name, &plugin);
        if (R_SUCCEEDED(rc) && request != Request::Status) {
            hotupdate::DeferredCapabilities caps = {};
            rc = serviceDispatchOut(&plugin, hotupdate::DeferredCapabilitiesCommand, caps);
            if (R_SUCCEEDED(rc) && caps.protocol != hotupdate::DeferredProtocolVersion)
                rc = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
            if (R_FAILED(rc))
                std::snprintf(result, sizeof(result),
                    "UPDATER MISMATCH: protocol 5 supervisor required (%08x). "
                    "No request queued; use the matching first-install set.\n", rc);
        }
        if (R_SUCCEEDED(rc)) {
            hotupdate::DeferredReport d = {};
            if (request == Request::Status) {
                SupervisorStatus s = {};
                rc = serviceDispatchOut(&plugin, 0, s);
                if (R_SUCCEEDED(rc)) {
                    std::snprintf(result, sizeof(result),
                        "HEN_UPDATER supervisor_protocol=%u pid=%llu generation=%llu flags=%x "
                        "missing=%x busy=%u last_rc=%08x\n",
                        s.protocol, static_cast<unsigned long long>(s.process_id),
                        static_cast<unsigned long long>(s.generation),
                        s.capabilities, s.missing, s.restart_in_progress,
                        s.latest_result);
                }
                if (R_SUCCEEDED(rc)) {
                    u32 flags = 0;
                    const Result safe_rc = serviceDispatchOut(&plugin, 2, flags);
                    const size_t len = std::strlen(result);
                    std::snprintf(result + len, sizeof(result) - len,
                        "HEN_SAFE query_rc=%08x flags=%08x can_hold=%u held=%u\n",
                        safe_rc, flags, (flags & 1u) ? 1u : 0u,
                        (flags & 2u) ? 1u : 0u);
                }
                if (R_SUCCEEDED(rc)) {
                    const Result deferred_rc = serviceDispatchOut(
                        &plugin, hotupdate::DeferredStatusCommand, d);
                    if (R_SUCCEEDED(deferred_rc)) appendDeferred(result, sizeof(result), d);
                    else {
                        const size_t len = std::strlen(result);
                        std::snprintf(result + len, sizeof(result) - len,
                            "DEFERRED SERVICE ERROR: %08x\n", deferred_rc);
                    }
                }
                if (R_SUCCEEDED(rc)) {
                    hotupdate::DeferredHenPreflight p = {};
                    const Result check = serviceDispatchOut(
                        &plugin, hotupdate::DeferredPreflightHenCommand, p);
                    const size_t len = std::strlen(result);
                    std::snprintf(result + len, sizeof(result) - len,
                        "HEN_STAGE query_rc=%08x check_rc=%08x size=%llu sha256=",
                        check, p.result,
                        static_cast<unsigned long long>(p.main_size));
                    size_t pos = std::strlen(result);
                    if (R_SUCCEEDED(check) && R_SUCCEEDED(p.result)) {
                        for (unsigned i = 0; i < sizeof(p.sha256); ++i) {
                            if (pos + 3 >= sizeof(result)) break;
                            std::snprintf(result + pos, sizeof(result) - pos,
                                "%02x", static_cast<unsigned>(p.sha256[i]));
                            pos = std::strlen(result);
                        }
                    } else {
                        std::snprintf(result + pos, sizeof(result) - pos, "unavailable");
                    }
                    pos = std::strlen(result);
                    std::snprintf(result + pos, sizeof(result) - pos, "\n");
                }
                if (R_SUCCEEDED(rc)) {
                    hotupdate::DeferredCapabilities caps = {};
                    const Result cap_rc = serviceDispatchOut(
                        &plugin, hotupdate::DeferredCapabilitiesCommand, caps);
                    const size_t len = std::strlen(result);
                    std::snprintf(result + len, sizeof(result) - len,
                        "CAPS query_rc=%08x attempts=%u successful=%u limit=%u may_queue=%u\n",
                        cap_rc, caps.live_attempts, caps.successful_live,
                        caps.max_live, caps.may_queue_live);
                }
                if (R_SUCCEEDED(rc)) {
                    hotupdate::RestartReport r = {};
                    const Result check = serviceDispatchOut(
                        &plugin, hotupdate::DeferredRestartStatusCommand, r);
                    const size_t len = std::strlen(result);
                    std::snprintf(result + len, sizeof(result) - len,
                        "RESTART query_rc=%08x phase=%u result=%08x "
                        "recovery=%08x old_pid=%llu new_pid=%llu generation=%llu "
                        "rollback=%u old_hen_stopped=%u\n",
                        check, static_cast<unsigned>(r.phase), r.result,
                        r.recovery, static_cast<unsigned long long>(r.previous_pid),
                        static_cast<unsigned long long>(r.new_pid),
                        static_cast<unsigned long long>(r.generation),
                        r.rollback_available, r.old_hen_stopped);
                }
            } else if (request == Request::DryRun ||
                       request == Request::QueueHen) {
                u64 host_pid = 0;
                u64 package_size = 0;
                rc = svcGetProcessId(&host_pid, CUR_PROCESS_HANDLE);
                if (R_SUCCEEDED(rc) && via_package) {
                    hotupdate::DeferredHenPreflight preflight = {};
                    const u32 command = hotupdate::DeferredPreflightHenCommand;
                    rc = serviceDispatchOut(&plugin, command, preflight);
                    if (R_SUCCEEDED(rc) && preflight.protocol != hotupdate::DeferredProtocolVersion)
                        rc = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
                    if (R_SUCCEEDED(rc)) rc = preflight.result;
                    if (R_SUCCEEDED(rc) &&
                        std::memcmp(preflight.sha256, package_digest, 32) != 0)
                        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
                    package_size = preflight.main_size;
                    if (R_FAILED(rc)) {
                        std::snprintf(result, sizeof(result),
                            "PACKAGE HASH PREFLIGHT FAILED: %08x (nothing queued).\n", rc);
                    }
                }
                if (R_SUCCEEDED(rc)) {
                    const bool hen_live = request == Request::QueueHen;
                    const hotupdate::DeferredRequest req = {
                        hen_live ? hotupdate::DeferredHenConfirmation
                                 : hotupdate::DeferredDryRunConfirmation,
                        host_pid
                    };
                    const u32 command = hen_live
                        ? hotupdate::DeferredQueueHenCommand
                        : hotupdate::DeferredQueueCommand;
                    if (via_package) {
                        hotupdate::DeferredHenPackageRequest bound = {
                            hotupdate::DeferredHenConfirmation, host_pid, package_size, {}
                        };
                        std::memcpy(bound.sha256, package_digest, sizeof(bound.sha256));
                        rc = serviceDispatchInOut(&plugin,
                            hotupdate::DeferredQueueHenPackageCommand, bound, d);
                    } else {
                        rc = serviceDispatchInOut(&plugin, command, req, d);
                    }
                    if (R_SUCCEEDED(rc)) {
                        std::snprintf(result, sizeof(result),
                            hen_live ? "QUEUED HEN-ONLY REPLACEMENT: "
                                       "close the host application to execute.\n"
                                     : "QUEUED HEN DRY RUN: "
                                       "close the host application.\n");
                        appendDeferred(result, sizeof(result), d);
                    }
                }
            } else {
                rc = serviceDispatchOut(
                    &plugin, hotupdate::DeferredCancelCommand, d);
                if (R_SUCCEEDED(rc)) {
                    std::snprintf(result, sizeof(result),
                        "CANCELLED pending deferred request, if not yet executing.\n");
                    appendDeferred(result, sizeof(result), d);
                }
            }
        }
        if (serviceIsActive(&plugin)) serviceClose(&plugin);
        if (serviceIsActive(&hen)) serviceClose(&hen);
        // RO is borrowed from the current homebrew bootstrap runtime.
        if (!result[0])
            std::snprintf(result, sizeof(result), "SERVICE ERROR: %08x\n", rc);
    }
log_and_exit:
    const Result log_rc = writeLog(&sd, result, std::strlen(result));
    fsFsClose(&sd);
    consoleInit(nullptr);
    std::printf("HEN updater - kaiHEN " HEN_UPSTREAM_LABEL " / updater protocol 5\n\n%s\n", result);
    if (R_SUCCEEDED(log_rc))
        std::printf("Result saved to /kaihen/hotupdate/hen-result.txt\n\n");
    else
        std::printf("Could not save result (%08x). Read the screen before leaving.\n\n", log_rc);
    std::printf("Press + to return to hbmenu.\n"
                "For a queued request: HOME, then X to close the host game.\n"
                "Returning to hbmenu alone does not close that game.\n");
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
    return static_cast<int>(rc);
}
