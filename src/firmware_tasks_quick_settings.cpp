#ifdef ARDUINO

#include "firmware/firmware_tasks_internal.hpp"

#include <string_view>

#include "firmware/firmware_version.hpp"
#include "modules/quick_settings_orchestration.hpp"
#include "modules/board_owner_pairing_feature.hpp"
#include "modules/orientation.hpp"

namespace firmware_internal {
namespace {

void ShowBlockedQuickSetting(
    FirmwareSystem& sys,
    const modules::ControlMutationReason reason,
    const uint32_t nowMs) {
    const modules::QuickSettingsBlockedFeedback feedback =
        modules::ClassifyQuickSettingsBlockedFeedback(reason);
    if (!feedback.visible) {
        return;
    }
    if (feedback.flashAllPieces) {
        sys.stateMachine.TriggerAllPiecesFlash(1000U);
    }
    if (feedback.edgeCue != modules::EdgeLedCueKind::None) {
        sys.edgeCue.Show(
            feedback.edgeCue, sys.quickSettingsOverlayOwner, nowMs);
    }
}

modules::QuickSettingsOrchestrationContext MakeContext(
    FirmwareSystem& sys, const uint32_t nowMs,
    const std::array<modules::Rgb, 64>& background) {
    return modules::QuickSettingsOrchestrationContext{
        .settings = sys.settings,
        .controlMutation = sys.controlMutation,
        .overlay = sys.menuOverlay,
        .background = background,
        .owner = sys.quickSettingsOverlayOwner,
        .selectedIndex = sys.quickSettingsOverlaySelectedIndex,
        .nowMs = nowMs,
        .settingsLoaded =
            !sys.settings.IsDisabled() &&
            sys.settings.GetLastError() ==
                modules::SettingsStoreError::None,
        .firmwareVersion =
            std::string_view(firmware::FIRMWARE_VERSION),
    };
}

} // namespace

uint8_t QuickSettingsItemCount(const FirmwareSystem& sys) {
    return modules::OWNER_PAIRING_ENABLED && sys.ownerPairingPrepared
        ? modules::quick_settings_items::TOTAL_COUNT
        : modules::quick_settings_items::OWNER_PAIRING;
}

void SyncOwnerPairingControls(FirmwareSystem& sys) {
    if (!modules::OWNER_PAIRING_ENABLED || !sys.ownerPairingPrepared) return;
    const bool selected = sys.quickSettingsOverlayActive &&
        !sys.hardwareTest.IsActive() && !sys.comptest.IsActive() &&
        sys.quickSettingsOverlaySelectedIndex == modules::quick_settings_items::OWNER_PAIRING;
    sys.boardOwnerPairing.SyncLocalClearContext(
        selected, sys.quickSettingsOverlayPhysicalEnd);
}

void ShowQuickSettingsSelection(FirmwareSystem& sys,
                                const uint32_t nowMs) {
    const auto background = sys.ledAnimation.MenuBackground();
    modules::ShowQuickSettingsSelection(MakeContext(sys, nowMs, background));
    SyncOwnerPairingControls(sys);
}


void OpenBoardQuickSettings(FirmwareSystem& sys, const modules::UiOwner owner,
                            const uint32_t nowMs) {
    sys.quickSettingsOverlayActive = true;
    sys.gamePersistence.SetBoardSettingsActive(true);
    sys.quickSettingsOverlayOwner = owner;
    const bool swapped = modules::ComputeControlBankSwap(GetFlipSidesSetting(sys.settings),
        GetFirstPlayerIsP2Setting(sys.settings), sys.gamePersistence.Litchcraft().IsActive());
    sys.quickSettingsOverlayPhysicalEnd = static_cast<uint8_t>(
        (owner == modules::UiOwner::P2) != swapped);
    if (sys.quickSettingsOverlaySelectedIndex >= QuickSettingsItemCount(sys))
        sys.quickSettingsOverlaySelectedIndex = QUICK_SETTINGS_ITEM_CPU_DIFFICULTY;
    sys.quickSettingsOverlayIdleElapsed.Reset();
    sys.gameResetHoldActive = false;
    sys.gameResetHoldStartMs = 0U;
    sys.gameResetHoldOwner = modules::UiOwner::None;
    sys.gameResetHoldTriggered = false;
    sys.hintButtonController.Reset();
    sys.resetHoldController.Reset();
    sys.quickSettingsOverlayController.Reset();
    ShowQuickSettingsSelection(sys, nowMs);
}
void CloseBoardQuickSettings(FirmwareSystem& sys) {
    sys.quickSettingsOverlayActive = false;
    sys.gamePersistence.SetBoardSettingsActive(false);
    sys.quickSettingsOverlayOwner = modules::UiOwner::None;
    sys.quickSettingsOverlayPhysicalEnd = 0U;
    sys.hintButtonController.Reset();
    sys.resetHoldController.Reset();
    sys.quickSettingsOverlayController.Reset();
    sys.menuOverlay.Stop();
    SyncOwnerPairingControls(sys);
}
void HandleBoardQuickSettingsAction(FirmwareSystem& sys,
    const modules::QuickSettingsAction& action, const uint32_t nowMs) {
    using Type = modules::QuickSettingsActionType;
    if (action.type == Type::CloseOverlay) {
        CloseBoardQuickSettings(sys);
        sys.controlFeedback.Trigger(modules::ControlFeedbackAction::Cancel,
            action.owner, modules::ControlRole::X);
        return;
    }
    bool navigated = false;
    if (action.type == Type::SelectPrevious && sys.quickSettingsOverlaySelectedIndex > 0U) {
        --sys.quickSettingsOverlaySelectedIndex;
        navigated = true;
    }
    if (action.type == Type::SelectNext &&
        sys.quickSettingsOverlaySelectedIndex + 1U < QuickSettingsItemCount(sys)) {
        ++sys.quickSettingsOverlaySelectedIndex;
        navigated = true;
    }
    if (navigated) {
        sys.controlFeedback.Trigger(modules::ControlFeedbackAction::Navigate, action.owner,
            action.type == Type::SelectPrevious ? modules::ControlRole::Left : modules::ControlRole::Right,
            {}, modules::ControlFeedbackScene::QuickSetup);
        ShowQuickSettingsSelection(sys, nowMs);
    }
    if (action.type == Type::AdjustIncrease || action.type == Type::AdjustDecrease)
        HandleQuickSettingsAdjust(sys, action, nowMs);
    if (action.type != Type::None) sys.quickSettingsOverlayIdleElapsed.Reset();
}

void HandleQuickSettingsAdjust(
    FirmwareSystem& sys,
    const modules::QuickSettingsAction& action,
    const uint32_t nowMs) {
    const auto background = sys.ledAnimation.MenuBackground();
    const bool oldSwap = modules::ComputeControlBankSwap(GetFlipSidesSetting(sys.settings),
        GetFirstPlayerIsP2Setting(sys.settings), false);
    const modules::QuickSettingsMutationResult result =
        modules::ApplyQuickSettingsAdjustment(
            MakeContext(sys, nowMs, background), action);
    const bool newSwap = modules::ComputeControlBankSwap(GetFlipSidesSetting(sys.settings),
        GetFirstPlayerIsP2Setting(sys.settings), false);
    const auto feedbackOwner = oldSwap == newSwap ? action.owner : modules::SwapP1P2Owner(action.owner);
    if (result.hasOutcome && result.outcome.status == modules::ControlMutationStatus::Applied)
        sys.controlFeedback.Trigger(modules::ControlFeedbackAction::Acknowledge, feedbackOwner,
            action.type == modules::QuickSettingsActionType::AdjustIncrease
                ? modules::ControlRole::Up : modules::ControlRole::Down,
            {}, modules::ControlFeedbackScene::QuickSetup);
    if (result.showBlockedFeedback && result.hasOutcome &&
        result.outcome.status ==
            modules::ControlMutationStatus::Rejected) {
        ShowBlockedQuickSetting(sys, result.outcome.reason, nowMs);
    }
}

} // namespace firmware_internal

#endif
