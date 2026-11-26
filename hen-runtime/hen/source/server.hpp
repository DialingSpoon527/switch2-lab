// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <switch.h>
#include "../../common/control.h"
#include "game_arm.hpp"
#include "sm_proxy.hpp"
#include "background_plugin.hpp"
#include "managed_registry.hpp"
#include "graceful_shutdown.hpp"
#include "../../libkaihen/source/cmif_server.hpp"

/// HEN's IPC server. It serves the mig:usr control port, on which the
/// bootstrap hands HEN its handles and configuration, and the homebrew
/// sessions that it creates for processes that RO initialized. Between
/// requests its loop advances game arming.
class HenServer {
  public:
    HenServer();
    ~HenServer();
    HenServer(const HenServer&) = delete;
    HenServer& operator=(const HenServer&) = delete;

    /// Registers mig:usr.
    Result initialize();

    /// Serves requests until a kernel call fails.
    Result run();

  private:
    static constexpr u32 MaxSessions = 16;
    /// The most handles a request can copy (HIPC's limit).
    static constexpr u32 MaxCopyHandles = 15;

    enum class SessionKind {
        Control,  ///< A session on mig:usr.
        Supervisor, ///< A separate, private session minted during bootstrap.
        Homebrew, ///< A session from CreateHomebrewSession.
    };

    struct SessionRef {
        SessionKind kind;
        u32 index;
    };

    /// The sessions that the loop waits on, and the port before them while
    /// HEN has it.
    struct WaitList {
        Handle handles[2 + 2 * MaxSessions];
        SessionRef sessions[2 + 2 * MaxSessions]; ///< sessions[i] for handles[i], except the port.
        s32 count;
        s32 port_index; ///< -1 once the port is released.
    };

    /// A request, copied out of the message buffer. Commands that make IPC
    /// calls of their own overwrite the buffer.
    struct ReceivedRequest {
        bool accepted;   ///< A valid CMIF request with inline data and copied handles only.
        u32 type;        ///< CmifCommandType
        u32 command_id;
        u32 num_copy_handles;
        Handle copy_handles[MaxCopyHandles];
        u8 data[ounce::MessageSize];
        size_t data_size;
    };

    /// What a command answers.
    struct CommandReply {
        Result result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        u8 data[sizeof(HenManagerDiagnosticsV1)] = {}; ///< Largest cached reply.
        size_t data_size = 0;
        /// A handle the reply copies to the client, or INVALID_HANDLE.
        Handle copy_handle = INVALID_HANDLE;
        /// The copied handle is a temporary of HEN's: close it once sent.
        bool close_copy_after_send = false;
        /// HEN keeps the handles that the request copied; otherwise they are closed.
        bool keeps_request_handles = false;

        template <typename T>
        void setData(const T& value) {
            static_assert(sizeof(T) <= sizeof(data));
            __builtin_memcpy(data, &value, sizeof(T));
            data_size = sizeof(T);
        }
    };

    struct PluginRecord {
        HenPluginName name = {};
        u64 process_id = 0;
        Handle control_session = INVALID_HANDLE;
    };

    // The loop
    void collectWaits(WaitList& waits) const;
    void stepBetweenRequests();
    void acceptSession();
    Result serveSession(SessionRef session);
    Handle& sessionHandle(SessionRef session);
    void closeSession(SessionRef session);

    // Requests
    ReceivedRequest receiveRequest() const;
    CommandReply serveRequest(const ReceivedRequest& request, SessionRef session);
    void serveControlCommand(const ReceivedRequest& request, CommandReply& reply);
    void serveHomebrewCommand(const ReceivedRequest& request, u32 index, CommandReply& reply);
    void serveSupervisorCommand(const ReceivedRequest& request, CommandReply& reply);
    Result createSupervisorSession(Handle* out_client);

    // Control commands
    Result startGameArm(Handle bootstrap_process);
    Result createHomebrewSession(Handle copied_process, Handle* out_client);
    Result adoptFilesystemRoot(Handle copied_root);
    Result adoptServiceManager(Handle copied_session);
    Result adoptPlugin(const HenPluginRecord& plugin, Handle control_session);
    Result releaseControlPort();

    // Homebrew commands
    Result openPlugin(const HenPluginName& name, Handle* out_session);
    void removePlugin(u32 index);
    void removeExitedPlugins();
    bool pluginWorkIdleForReplacement() const;

    bool stop_requested_ = false;
    Handle port_ = INVALID_HANDLE;
    Handle supervisor_session_ = INVALID_HANDLE;
    Handle fs_root_ = INVALID_HANDLE;
    /// FS's SM session. Only the SM proxy uses it: programs get proxy
    /// sessions (GetServiceManager).
    Handle sm_session_ = INVALID_HANDLE;
    SmProxy sm_proxy_;
    bool settings_ready_ = false;
    Handle sessions_[MaxSessions] = {};
    Handle homebrew_sessions_[MaxSessions] = {};
    Handle homebrew_processes_[MaxSessions] = {};
    PluginRecord plugins_[HEN_MAX_PLUGINS] = {};
    u32 plugin_count_ = 0;
    HenStatus status_ = {};
    GameArm game_ = {};
    BackgroundPluginManager background_ = {};
    GracefulShutdown graceful_ = {};
    pluginmanager::Registry managed_ = {};
    bool restart_pending_ = false;
    HenBackgroundPluginRequest restart_request_ = {};
    u64 request_host_pid_ = 0;
    u64 restart_host_pid_ = 0;
    u64 manager_loop_iterations_ = 0;
    u64 manager_game_steps_ = 0;
    u64 manager_game_suppressed_ = 0;
    u64 manager_pm_started_ = 0;
    u64 manager_pm_completed_ = 0;
    u64 manager_tracked_serial_ = 0;
    u64 manager_poll_tick_ = 0;
    bool manager_poll_started_ = false;
    Result manager_shell_result_ = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    Result manager_game_start_result_ = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    Result manager_last_pm_result_ = 0;
    u32 manager_game_start_attempts_ = 0;
    u32 manager_bootstrap_flags_ = 0;
    HenManagerDiagnosticsV1 managerDiagnostics() const;
    Result managerQueryProcessPresence(u64 pid, bool* exists);
    Result managerQueryProgramId(u64* program, u64 pid);
    void recordStartedPlugin();
    bool managerNeedsPoll() const;
    void expireManagedStops();
    Result queueManagedBackground(const HenBackgroundPluginRequest& request,
                                  u64 host_pid, bool from_restart = false);
    void tickGracefulShutdown();
    Result stopManaged(const HenPluginName& name, u32 reason = KaihenPluginShutdown_UserStop);
};
