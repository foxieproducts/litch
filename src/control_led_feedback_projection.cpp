#include "modules/control_led_feedback_projection.hpp"

#include "modules/quick_settings_items.hpp"
#include "modules/player_end_palette.hpp"
#include "modules/ui_color_keys.hpp"
#include "modules/ui_color_wheel.hpp"
#include "runtime/interaction_domain_types.hpp"

namespace modules {
void ObserveControlLedActionFeedback(ControlLedFeedback& feedback,
    const ControlMutationOutcome& outcome, const UiOwner owner,
    const ControlActionState initialState) {
    if (outcome.status == ControlMutationStatus::Rejected &&
        outcome.action == ControlAction::Coaching &&
        outcome.reason == ControlMutationReason::EngineRejected) {
        feedback.Trigger(ControlFeedbackAction::Unavailable, owner, ControlRole::O);
        return;
    }
    if (outcome.status != ControlMutationStatus::Applied) return;
    switch (outcome.action) {
        case ControlAction::Undo:
            feedback.Trigger(initialState == ControlActionState::UndoPending
                ? ControlFeedbackAction::Cancel : ControlFeedbackAction::Undo,
                owner, ControlRole::X);
            break;
        case ControlAction::Hint:
            feedback.Trigger(ControlFeedbackAction::Hint, owner, ControlRole::O);
            break;
        case ControlAction::Coaching:
            feedback.Trigger(ControlFeedbackAction::Coaching, owner, ControlRole::O);
            break;
        case ControlAction::NewGame:
        case ControlAction::Sleep:
            feedback.Reset();
            break;
        default: break;
    }
}

ControlLedFeedbackInputs BuildControlLedFeedbackInputs(const ControlFeedbackProjection& ctx) {
    ControlLedFeedbackInputs in;
    in.minimalAnimations = ctx.settings.ReadValue("MINIMAL_ANIMATIONS", false);
    const uint8_t nearWheel = ui_color_wheel::ClampWheelValue01To255(player_end_palette::ReadSettingUInt32(
        ctx.settings, ui_color_keys::P1_END_PIECE_WHEEL, player_end_palette::P1_END_DEFAULT_WHEEL));
    const uint8_t farWheel = ui_color_wheel::ClampWheelValue01To255(player_end_palette::ReadSettingUInt32(
        ctx.settings, ui_color_keys::P2_END_PIECE_WHEEL, player_end_palette::P2_END_DEFAULT_WHEEL));
    const bool firstP2 = ctx.settings.ReadValue("GAME_FIRST_PLAYER", uint32_t{1U}) == 2U;
    in.playerColors = {ui_color_wheel::WheelToRgb(firstP2 ? farWheel : nearWheel),
        ui_color_wheel::WheelToRgb(firstP2 ? nearWheel : farWheel)};
    if (ctx.snapshot != nullptr) {
        const auto& state = *ctx.snapshot;
        in.hintFeedbackAllowed = state.render.manualHintAcknowledgmentValid &&
            runtime::IsIdleInteraction(state) &&
            state.board.physicalOccupiedMask == state.board.expectedOccupiedMask &&
            !state.game.cpuThinking && !state.game.inCheckmate && !state.game.inDraw &&
            !state.game.inStalemate && !state.interaction.kingQueenSwapPromptActive &&
            !state.interaction.undoPromptActive;
    }

    if (ctx.suppressed || (ctx.craft != nullptr &&
        (ctx.craft->IsEntryHoldActive() || ctx.craft->IsExitHoldActive()))) {
        in.scene = ControlFeedbackScene::Suppressed;
        return in;
    }
    if (ctx.craft != nullptr && ctx.craft->IsActive()) {
        ctx.craft->FillControlFeedbackInputs(in);
        return in;
    }
    in.coaching = ctx.coaching;
    if (ctx.menuActive) {
        in.scene = ControlFeedbackScene::QuickSetup;
        uint8_t mask = 1U << static_cast<uint8_t>(ControlRole::X);
        if (ctx.menuIndex > 0U) mask |= 1U << static_cast<uint8_t>(ControlRole::Left);
        if (ctx.menuIndex + 1U < ctx.menuCount) mask |= 1U << static_cast<uint8_t>(ControlRole::Right);
        bool up = ctx.menuIndex != quick_settings_items::OWNER_PAIRING;
        bool down = up;
        const auto bound = [&](const char* key, const uint32_t maximum) {
            const uint32_t value = ctx.settings.ReadValue(key, maximum == 8U ? uint32_t{5U} : uint32_t{0U});
            up = value < maximum;
            down = value > 0U;
        };
        switch (ctx.menuIndex) {
            case quick_settings_items::CPU_DIFFICULTY: bound("VS_CPU_LEVEL", 11U); break;
            case quick_settings_items::BRIGHTNESS: bound("BRIGHTNESS", 8U); break;
            case quick_settings_items::TIMER: bound("TIMER", 6U); break;
            default: break;
        }
        up = up && ctx.adjustmentsAllowed;
        down = down && ctx.adjustmentsAllowed;
        if (up) mask |= 1U << static_cast<uint8_t>(ControlRole::Up);
        if (down) mask |= 1U << static_cast<uint8_t>(ControlRole::Down);
        // The existing menu accepts either physical bank; no O selection action.
        in.availableControls = {mask, mask};
        return in;
    }
    if (ctx.hint != nullptr) {
        for (uint8_t bank = 0U; bank < 2U; ++bank) {
            const auto owner = bank == 1U ? UiOwner::P2 : UiOwner::P1;
            in.holds[bank][1] = {ControlRole::O, ctx.hint->HeldMs(owner),
                ctx.hint->HoldThresholdMs(), false, ctx.hint->IsHeld(owner)};
        }
    }
    if (ctx.quick != nullptr) {
        for (uint8_t bank = 0U; bank < 2U; ++bank) {
            uint8_t progress = 0U;
            if (ctx.quick->TryGetOpenHoldProgressFor(bank == 1U ? UiOwner::P2 : UiOwner::P1, progress)) {
                const uint32_t threshold = ctx.quick->HoldThresholdMs();
                in.holds[bank][0] = {ControlRole::X, progress * threshold / 255U, threshold, false, true};
            }
        }
    }
    if (ctx.holdPreview.menuHoldActive && ctx.holdPreview.menuOwner != UiOwner::None) {
        const uint8_t bank = ctx.holdPreview.menuOwner == UiOwner::P2 ? 1U : 0U;
        in.holds[bank][0] = {ControlRole::X,
            static_cast<uint32_t>(ctx.holdPreview.menuValue01_255) *
                (ctx.quick != nullptr ? ctx.quick->HoldThresholdMs() : 1000U) / 255U,
            ctx.quick != nullptr ? ctx.quick->HoldThresholdMs() : 1000U, false, true};
    }
    return in;
}
} // namespace modules
