#ifdef ARDUINO
#include "firmware/firmware_tasks_internal.hpp"
#include "firmware/firmware_app_internal.hpp"
#include <string_view>

#include "modules/control_led_feedback_projection.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/quick_settings_hold_preview.hpp"
#include "modules/quick_settings_orchestration.hpp"

namespace firmware_internal {
void ProjectControlFeedbackSetting(void* context, const char* canonicalKey) {
    auto& sys = *static_cast<FirmwareSystem*>(context);
    sys.boardStylePreview.SettingAccepted(sys.settings, canonicalKey);
    const std::string_view key = canonicalKey != nullptr ? canonicalKey : "";
    if (key == "VS_CPU_LEVEL" && sys.quickSettingsOverlayActive &&
        sys.quickSettingsOverlaySelectedIndex == QUICK_SETTINGS_ITEM_CPU_DIFFICULTY) {
        modules::RefreshQuickSettingsCpuDifficulty(
            sys.settings, sys.menuOverlay, sys.ledAnimation.MenuBackground());
    }
    if (key == "MINIMAL_ANIMATIONS" &&
        sys.settings.ReadValue("MINIMAL_ANIMATIONS", false)) {
        sys.stateMachine.CancelHintRefinement();
    }
    if (key != "BOARD_ROTATE" && key != "GAME_FIRST_PLAYER") return;
    sys.controlFeedback.Reset();
    sys.gamePersistence.Litchcraft().CancelEntryHold();
    sys.hintButtonController.Reset();
    sys.resetHoldController.Reset();
    sys.quickSettingsOverlayController.Reset();
    sys.gameResetHoldActive = false;
    sys.gameResetHoldStartMs = 0U;
    sys.gameResetHoldOwner = modules::UiOwner::None;
    sys.gameResetHoldTriggered = false;
}

void ConfigureBoardControlMutations(FirmwareSystem& sys,
    const modules::ControlMutationCallbacks& callbacks) {
    sys.controlMutation.Configure(sys.settings, callbacks);
    sys.controlMutation.SetActionCompletedCallback([](void* context,
        const modules::ControlMutationOutcome& outcome, modules::UiOwner owner,
        modules::ControlActionState initialState) {
        auto& system = *static_cast<FirmwareSystem*>(context);
        modules::ObserveControlLedActionFeedback(system.controlFeedback, outcome, owner, initialState);
    });
    sys.paletteEditPreview.SetFeedback(&sys.controlFeedback);
    sys.gamePersistence.Litchcraft().SetControlFeedback(&sys.controlFeedback);
}

modules::ControlLedFeedbackInputs CaptureControlFeedback(FirmwareSystem& sys,
    const modules::led_frame_composer::ComposeInputs& presentation,
    const bool identifyActive) {
    if (sys.runtimeHardware.resolved.revision != hardware::BoardRevision::V3_3S3) {
        sys.controlFeedback.Reset();
        modules::ControlLedFeedbackInputs inputs;
        inputs.scene = modules::ControlFeedbackScene::Suppressed;
        return inputs;
    }
    const auto& art = sys.gamePersistence.Litchcraft();
    const bool suppressed = sys.safeModeActive || sys.stateMachine.IsTextOverlayActive() || identifyActive || sys.gameResetHoldActive ||
        sys.stateMachine.IsPromotionActive() ||
        sys.gamePersistence.GetKingResignationOwner() != modules::UiOwner::None ||
        runtime::IsRuntimeHealthPlayPaused(sys.runtimeHealth.Snapshot()) ||
        (sys.menuOverlay.IsActive() && !sys.quickSettingsOverlayActive);
    const char* startKey = sys.quickSettingsOverlaySelectedIndex == modules::quick_settings_items::FIRST_PLAYER ? "GAME_FIRST_PLAYER"
        : sys.quickSettingsOverlaySelectedIndex == modules::quick_settings_items::ROTATE ? "BOARD_ROTATE"
        : sys.quickSettingsOverlaySelectedIndex == modules::quick_settings_items::TIMER ? "TIMER" : nullptr;
    const bool adjustmentsAllowed = startKey == nullptr ||
        sys.gamePersistence.CheckControlStartOnlyGate(startKey) == modules::ControlCoordinatorResult::Applied;
    const auto preview = presentation.menuIntent;
    return modules::BuildControlLedFeedbackInputs({
        sys.settings, &art, &sys.hintButtonController, preview,
        {presentation.beginnerModeP1Enabled, presentation.beginnerModeP2Enabled},
        sys.quickSettingsOverlaySelectedIndex, QuickSettingsItemCount(sys),
        sys.quickSettingsOverlayActive, suppressed, adjustmentsAllowed, presentation.menuIntent.menuHoldActive ? &sys.quickSettingsOverlayController : nullptr, presentation.interactionSnapshot});
}
} // namespace firmware_internal
#endif
