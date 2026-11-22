#pragma once

#include "modules/edge_led_intent.hpp"
#include "modules/litchcraft_session.hpp"
#include "modules/quick_settings_overlay_controller.hpp"

namespace modules {

// The chord arbiter retains input ownership. Reading its solitary X edge only
// fills the existing menu preview; it cannot open a menu or dispatch an action.
inline EdgeLedIntent BuildQuickSettingsHoldPreview(
    const QuickSettingsOverlayController& controller,
    const LitchcraftSession* chordOwner,
    const bool directOpenAllowed) {
    EdgeLedIntent intent;
    if (!directOpenAllowed) return intent;
    UiOwner owner = UiOwner::None;
    uint8_t progress = 0U;
    bool held = controller.TryGetOpenHoldProgress(owner, progress);
    if (!held && chordOwner != nullptr) {
        ButtonEvent pending;
        uint32_t pressedAtMs = 0U;
        if (chordOwner->TryGetPendingEntryButton(pending, pressedAtMs) &&
            IsXButton(pending.button)) {
            owner = ResolveButtonEventOwner(pending);
            progress = controller.GetOpenHoldProgress01_255(pressedAtMs);
            held = owner != UiOwner::None;
        }
    }
    if (held) {
        intent.menuActive = true;
        intent.menuOwner = owner;
        intent.menuValue01_255 = progress;
        intent.menuHoldActive = true;
    }
    return intent;
}

} // namespace modules
