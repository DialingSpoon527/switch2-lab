// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>

namespace pluginmanager {

// The host tests execute these same recovery loops with fault-injected Gate,
// memory and clock objects. They do not emulate Loader or establish device
// compatibility. A timeout quarantines future operations; it never authorizes
// releasing an unrestored Loader transaction.
constexpr std::uint64_t BackgroundRestoreDeadlineNs = 5000000000ull;
constexpr std::uint64_t BackgroundRestorePollNs = 25000000ull;
constexpr std::uint64_t BackgroundRecoveryPollNs = 250000000ull;

inline bool canQueueBackground(bool pending, bool active, bool publication_pending,
                               bool termination_active, bool quarantined) {
    return !pending && !active && !publication_pending && !termination_active &&
           !quarantined;
}

struct BackgroundRestoration {
    // Once the deadline has expired, success only permits releasing the game
    // lease. The caller must retain ambiguous launch resources and must never
    // resume the abandoned launch's monitor, StartProcess or handoff actions.
    bool abandoned;
};

template <typename Gate, typename Memory, typename Clock, typename Sleep,
          typename Observe, typename Quarantine, typename Proved, typename Release>
BackgroundRestoration restoreBackgroundGate(
        Gate& gate, Memory& memory, bool ever_parked, std::uint32_t timeout_result,
        Clock now_ns, Sleep sleep_ns, Observe observe, Quarantine quarantine,
        Proved proved, Release release_game_lease) {
    const std::uint64_t started = now_ns();
    bool abandoned = false;
    for (;;) {
        // Keep the exact Gate object, including its partial rollback/receipt
        // flags. Reconstructing it would lose ownership of the original park.
        const std::uint32_t gate_result = ever_parked ? gate.restore(1) : 0;
        const std::uint32_t memory_result = memory.restore();
        const std::uint32_t result = gate_result ? gate_result : memory_result;
        observe(result);
        if (!abandoned && now_ns() - started >= BackgroundRestoreDeadlineNs) {
            abandoned = true;
            quarantine(result ? result : timeout_result);
        }
        if (!result) {
            proved();
            if (abandoned) release_game_lease();
            return {abandoned};
        }
        // Non-timeout errors can also be transient: the selected waiter may
        // still be running. Continue the same guarded restoration operation.
        sleep_ns(abandoned ? BackgroundRecoveryPollNs : BackgroundRestorePollNs);
    }
}

// This helper is called only after restoreBackgroundGate proved restoration.
// A stalled pm:shell worker owns no pm:dmnt monitor; on abandonment it retains
// its shell, thread and donor but must no longer prevent normal game arming.
template <typename Finished, typename Clock, typename Sleep,
          typename Quarantine, typename Release>
bool waitForRestoredBackgroundPm(Finished finished, Clock now_ns, Sleep sleep_ns,
                                std::uint64_t timeout_ns, std::uint64_t poll_ns,
                                std::uint32_t timeout_result,
                                Quarantine quarantine, Release release_game_lease) {
    const std::uint64_t started = now_ns();
    while (!finished()) {
        if (now_ns() - started >= timeout_ns) {
            quarantine(timeout_result);
            release_game_lease();
            return false;
        }
        sleep_ns(poll_ns);
    }
    return true;
}

} // namespace pluginmanager
