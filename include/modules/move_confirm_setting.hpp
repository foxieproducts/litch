#pragma once

#include <cstdint>
#include <cstring>

#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"

namespace modules::move_confirm_setting {

static constexpr const char* KEY_MOVE_CONFIRM = "MOVE_CONFIRM";

static constexpr const char* VALUE_SETTLE_2S = "settle_2s";
static constexpr const char* VALUE_SETTLE_5S = "settle_5s";
static constexpr const char* VALUE_PRESS_CIRCLE_TO_CONFIRM = "press_circle_to_confirm";

static constexpr const char* DEFAULT_VALUE = VALUE_SETTLE_2S;

struct MoveConfirmPolicy {
    uint32_t settleDurationMs = 2000U;
    bool requireOConfirm = false;
};

inline MoveConfirmPolicy ParseMoveConfirmPolicy(const char* value) {
    if (value == nullptr) {
        return MoveConfirmPolicy{};
    }

    if (std::strcmp(value, VALUE_SETTLE_5S) == 0) {
        return MoveConfirmPolicy{5000U, false};
    }

    if (std::strcmp(value, VALUE_PRESS_CIRCLE_TO_CONFIRM) == 0) {
        return MoveConfirmPolicy{2000U, true};
    }

    // Unknown values fall back to default behavior.
    return MoveConfirmPolicy{};
}

inline MoveConfirmPolicy ReadMoveConfirmPolicy(const modules::SettingsStore& settings) {
    const char* raw = settings.ReadValue(KEY_MOVE_CONFIRM, DEFAULT_VALUE);
    return ParseMoveConfirmPolicy(raw);
}

inline void Apply(runtime::GameStateMachine& stateMachine,
                  const modules::SettingsStore& settings) {
    const MoveConfirmPolicy policy = ReadMoveConfirmPolicy(settings);
    stateMachine.SetMoveConfirmPolicy(policy.settleDurationMs, policy.requireOConfirm);
}

} // namespace modules::move_confirm_setting
