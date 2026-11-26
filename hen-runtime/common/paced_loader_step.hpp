// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>

namespace ounce {
// Resident workers must yield between observations so Loader's waiter can run.
// Keep the original Gate object for rollback if any observation fails.
template<class Gate, class Clock, class Sleep>
std::uint32_t pacedLoaderStep(Gate& gate, void* request, Clock now_ns, Sleep sleep_ns,
                             std::uint32_t timeout,
                             std::uint64_t deadline_ns = 1500000000ull) {
    auto rc = gate.beginStep();
    if (rc) return rc;
    const auto started = now_ns();
    for (;;) {
        bool complete = false;
        rc = gate.pollStep(request, complete);
        if (rc || complete) return rc;
        if (now_ns() - started >= deadline_ns) return timeout;
        sleep_ns(25000000ull);
    }
}

// Stable running titles have already released Loader. Startup, restoration,
// capture, partial request steps and a supervisor hold cannot lend its state.
constexpr bool canBorrowPluginLoader(bool initialized, bool held, bool restoring,
        bool stepping, bool capturing, bool waiting_for_game, bool waiting_for_exit,
        bool healthy) {
    return initialized && !held && !restoring && !stepping && !capturing && healthy &&
           (waiting_for_game || waiting_for_exit);
}
} // namespace ounce
