// SPDX-License-Identifier: GPL-2.0-only
// Core of an EXPERIMENTAL RO/HEN control handoff. Does NOT start or terminate a process.
// This templated core is compiled in host tests with a fake kernel and can be
// compiled into kaiHEN RO with an SDK-backed Platform implementation.
#pragma once
#include <cstdint>

namespace hen_handoff {
enum class Status : std::uint8_t {
    Ok, InvalidCaller, InvalidGeneration, InvalidHandle, InvalidHen,
    AlreadyInitialized, NotInitialized, Busy, NotStaged, NotSwitched,
    SupervisorUnavailable
};

template<class Platform>
class Manager final {
public:
    using Handle = typename Platform::Handle;
    static constexpr Handle Invalid = Platform::Invalid;

    explicit Manager(Platform& platform) : platform_(platform) {}
    ~Manager() { release(&pending_); release(&previous_); release(&active_); release(&supervisor_process_); }
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    // Accept one initial HEN connection from bootstrap. Takes ownership of
    // incoming even on failure; never sends CMIF Close to a shared session.
    Status adoptInitial(Handle incoming, std::uint64_t caller_pid, std::uint64_t* out_hen_pid) {
        if (active_ != Invalid) return rejectIncoming(incoming, Status::AlreadyInitialized);
        if (!caller_pid || incoming == Invalid || !out_hen_pid)
            return rejectIncoming(incoming, Status::InvalidHandle);
        std::uint64_t pid = 0;
        if (!platform_.probeHen(incoming, &pid) || !pid)
            return rejectIncoming(incoming, Status::InvalidHen);
        active_ = incoming;
        active_pid_ = pid;
        bootstrap_pid_ = caller_pid;
        *out_hen_pid = pid;
        return Status::Ok;
    }

    // Only original bootstrap may authorize a specific supervisor process.
    // This is deliberately one-shot. Its process handle stays owned here.
    Status registerSupervisor(Handle supervisor_process, std::uint64_t caller_pid) {
        if (supervisor_process == Invalid) return Status::InvalidHandle;
        if (active_ == Invalid || bootstrap_pid_ != caller_pid || !caller_pid)
            return rejectIncoming(supervisor_process, Status::InvalidCaller);
        if (supervisor_process_ != Invalid)
            return rejectIncoming(supervisor_process, Status::AlreadyInitialized);
        std::uint64_t pid = 0;
        if (!platform_.processId(supervisor_process, &pid) || !pid ||
            pid == active_pid_ || pid == bootstrap_pid_)
            return rejectIncoming(supervisor_process, Status::InvalidHandle);
        supervisor_process_ = supervisor_process;
        supervisor_pid_ = pid;
        return Status::Ok;
    }

    // Bootstrap already knows the PID returned by PM when it starts the plugin.
    // The kernel provides the caller PID on every future IPC request, so we
    // do not need an impossible svcOpenProcess on an unrelated process.
    Status registerSupervisorPid(std::uint64_t pid, std::uint64_t caller_pid) {
        if (!pid || !caller_pid) return Status::InvalidHandle;
        if (active_ == Invalid) return Status::NotInitialized;
        if (caller_pid != bootstrap_pid_) return Status::InvalidCaller;
        if (pid == active_pid_) return Status::InvalidHen;
        if (supervisor_pid_) return Status::AlreadyInitialized;
        supervisor_pid_ = pid;
        return Status::Ok;
    }

    // Stage while old HEN is alive; stage never changes which HEN receives
    // newly requested homebrew sessions. Ownership of incoming is transferred.
    Status stage(Handle incoming, std::uint64_t caller_pid, std::uint64_t generation) {
        if (!allowed(caller_pid)) return rejectIncoming(incoming, Status::InvalidCaller);
        if (incoming == Invalid) return Status::InvalidHandle;
        if (generation <= generation_ || generation == UINT64_MAX)
            return rejectIncoming(incoming, Status::InvalidGeneration);
        if (pending_ != Invalid || previous_ != Invalid)
            return rejectIncoming(incoming, Status::Busy);
        std::uint64_t pid = 0;
        if (!platform_.probeHen(incoming, &pid) || !pid || pid == active_pid_)
            return rejectIncoming(incoming, Status::InvalidHen);
        pending_ = incoming;
        pending_pid_ = pid;
        pending_generation_ = generation;
        return Status::Ok;
    }

    // Single-threaded RO dispatch ensures no concurrent CreateHomebrewSession
    // call can occur during this swap. Old HEN's control is retained for rollback.
    Status switchToStaged(std::uint64_t caller_pid, std::uint64_t generation) {
        if (!allowed(caller_pid)) return Status::InvalidCaller;
        if (pending_ == Invalid) return Status::NotStaged;
        if (pending_generation_ != generation) return Status::InvalidGeneration;
        if (!platform_.probeHen(pending_, nullptr)) return Status::InvalidHen;
        previous_ = active_;
        previous_pid_ = active_pid_;
        active_ = pending_;
        active_pid_ = pending_pid_;
        pending_ = Invalid;
        pending_pid_ = 0;
        return Status::Ok;
    }

    // May return to original connection before commit if old HEN still lives.
    Status rollback(std::uint64_t caller_pid, std::uint64_t generation) {
        if (!allowed(caller_pid)) return Status::InvalidCaller;
        if (pending_generation_ != generation) return Status::InvalidGeneration;
        if (previous_ != Invalid) {
            if (!platform_.probeHen(previous_, nullptr)) return Status::InvalidHen;
            release(&active_);
            active_ = previous_;
            active_pid_ = previous_pid_;
            previous_ = Invalid;
            previous_pid_ = 0;
        } else if (pending_ != Invalid) {
            release(&pending_);
        } else return Status::NotStaged;
        pending_generation_ = 0;
        pending_pid_ = 0;
        return Status::Ok;
    }

    // The supervisor may finalize ONLY after verifying replacement startup.
    // This alone does not establish safe removal of the previous HEN process.
    Status finalize(std::uint64_t caller_pid, std::uint64_t generation) {
        if (!allowed(caller_pid)) return Status::InvalidCaller;
        if (previous_ == Invalid) return Status::NotSwitched;
        if (pending_generation_ != generation) return Status::InvalidGeneration;
        if (!platform_.probeHen(active_, nullptr)) return Status::InvalidHen;
        release(&previous_);
        previous_pid_ = 0;
        generation_ = generation;
        pending_generation_ = 0;
        return Status::Ok;
    }

    bool isSupervisorCaller(std::uint64_t kernel_pid) const {
        return kernel_pid != 0 && kernel_pid == supervisor_pid_;
    }
    Handle activeHandle() const { return active_; } // Borrowed only; caller must not close.
    std::uint64_t activePid() const { return active_pid_; }
    std::uint64_t generation() const { return generation_; }
    bool hasRollbackConnection() const { return previous_ != Invalid; }
    bool hasPendingConnection() const { return pending_ != Invalid; }
    bool hasSupervisor() const { return supervisor_pid_ != 0; }

private:
    Status rejectIncoming(Handle incoming, Status status) {
        if (incoming != Invalid) platform_.close(incoming);
        return status;
    }
    void release(Handle* h) {
        if (*h != Invalid) platform_.close(*h);
        *h = Invalid;
    }
    bool allowed(std::uint64_t pid) {
        if (!pid || pid != supervisor_pid_) return false;
        // In the scalar-bootstrap authorization variant the kernel-inserted
        // HIPC sender PID authenticates each request directly. No forgeable
        // PID is accepted from ordinary payload data.
        if (supervisor_process_ == Invalid) return true;
        std::uint64_t actual = 0;
        return platform_.processId(supervisor_process_, &actual) && actual == pid;
    }

    Platform& platform_;
    Handle active_ = Invalid, pending_ = Invalid, previous_ = Invalid, supervisor_process_ = Invalid;
    std::uint64_t bootstrap_pid_ = 0, supervisor_pid_ = 0;
    std::uint64_t active_pid_ = 0, pending_pid_ = 0, previous_pid_ = 0;
    std::uint64_t generation_ = 1, pending_generation_ = 0;
};
} // namespace hen_handoff
