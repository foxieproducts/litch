#pragma once

#include "modules/edge_led_intent.hpp"

namespace modules {

struct GameResetHoldContext {
    bool menuVisualActive = false;
    bool menuOverlayActive = false;
    bool quickSettingsOverlayActive = false;
    bool textOverlayActive = false;
    bool promotionActive = false;
};

inline bool IsGameResetHoldEligible(const GameResetHoldContext& ctx) {
    return !ctx.menuVisualActive && !ctx.menuOverlayActive && !ctx.quickSettingsOverlayActive &&
           !ctx.textOverlayActive && !ctx.promotionActive;
}

// Consent progress is separate from the terminal action result. A rejected
// action must expose its existing failure cue even while both buttons stay down.
inline void ProjectGameResetHold(EdgeLedIntent& intent, const UiOwner owner,
                                 const uint32_t elapsedMs, const bool triggered,
                                 const bool rejected) {
    if (rejected) return;
    constexpr uint32_t HOLD_MS = 2000U;
    intent.menuActive = true;
    intent.menuOwner = owner;
    intent.menuSelectionIndex = 0U;
    intent.menuHoldActive = true;
    intent.menuHoldFillAll = true;
    intent.menuValue01_255 = triggered || elapsedMs >= HOLD_MS
        ? 255U : static_cast<uint8_t>((elapsedMs * 255U) / HOLD_MS);
}

} // namespace modules
