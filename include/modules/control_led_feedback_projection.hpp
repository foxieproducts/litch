#pragma once

#include "modules/control_led_feedback.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/edge_led_intent.hpp"
#include "modules/hint_button_gesture_controller.hpp"
#include "modules/litchcraft_session.hpp"
#include "modules/settings_store.hpp"
#include "modules/quick_settings_overlay_controller.hpp"

namespace runtime { struct InteractionModelSnapshot; }

namespace modules {
struct ControlFeedbackProjection {
    const fxe::SettingsStore& settings;
    const LitchcraftSession* craft = nullptr;
    const HintButtonGestureController* hint = nullptr;
    EdgeLedIntent holdPreview{};
    std::array<bool, 2> coaching{};
    uint8_t menuIndex = 0U;
    uint8_t menuCount = 7U;
    bool menuActive = false;
    bool suppressed = false;
    bool adjustmentsAllowed = true;
    const QuickSettingsOverlayController* quick = nullptr;
    const runtime::InteractionModelSnapshot* snapshot = nullptr;
};
void ObserveControlLedActionFeedback(ControlLedFeedback& feedback,
    const ControlMutationOutcome& outcome, UiOwner owner, ControlActionState initialState);
ControlLedFeedbackInputs BuildControlLedFeedbackInputs(const ControlFeedbackProjection& context);
} // namespace modules
