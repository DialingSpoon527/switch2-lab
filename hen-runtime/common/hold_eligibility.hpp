// SPDX-License-Identifier: MIT
// Shared between native GameArm and host tests: an IDLE-ONLY admission gate.
#pragma once
namespace ounce {
constexpr bool safeToHoldGameArm(bool started, bool already_held,
        bool step_started, bool capture_pending, bool restore_pending,
        bool waiting_for_input, bool waiting_for_game, bool result_ok) {
    return started && !already_held && !step_started && !capture_pending &&
           !restore_pending && waiting_for_input && waiting_for_game && result_ok;
}
}
