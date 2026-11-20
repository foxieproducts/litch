#include "virtual_litch_session_internal.hpp"
#ifdef LITCH_NATIVE
#include <string_view>
namespace runtime::virtual_litch_internal {
void Session::OpenQuickSettings(const modules::UiOwner owner) {
    m_displayLabel.Clear();
    m_quickSettingsOverlayActive = true;
    m_gamePersistence->SetBoardSettingsActive(true);
    m_quickSettingsOverlayOwner = owner;
    m_quickSettingsOverlayPhysicalEnd = static_cast<uint8_t>(
        (owner == modules::UiOwner::P2) != ControlBanksSwapped());
    m_quickSettingsOverlayIdleElapsed.Reset();
    if (m_quickSettingsOverlaySelectedIndex >=
        (m_ownerPairing ? modules::quick_settings_items::TOTAL_COUNT :
                         modules::quick_settings_items::OWNER_PAIRING)) {
        m_quickSettingsOverlaySelectedIndex =
            modules::quick_settings_items::CPU_DIFFICULTY;
    }
    m_quickSettingsOverlayController.Reset();
    ShowQuickSettingsSelection();
}

void Session::CloseQuickSettings() {
    m_quickSettingsOverlayActive = false;
    m_gamePersistence->SetBoardSettingsActive(false);
    m_quickSettingsOverlayOwner = modules::UiOwner::None;
    m_quickSettingsOverlayPhysicalEnd = 0U;
    m_quickSettingsOverlayController.Reset();
    m_menuOverlay.Stop();
    m_displayLabel.Clear();
}

void Session::ShowQuickSettingsSelection() {
    m_displayLabel.Clear();
    modules::ShowQuickSettingsSelection({
        .settings = m_settings,
        .controlMutation = m_controlMutation,
        .overlay = m_menuOverlay,
        .background = m_gridFrameBeforeMenuOverlay,
        .owner = m_quickSettingsOverlayOwner,
        .selectedIndex = m_quickSettingsOverlaySelectedIndex,
        .nowMs = m_nowMs,
        .settingsLoaded = true,
        .firmwareVersion = std::string_view{},
    });
}

void Session::SyncOwnerPairingContext() {
    if (!m_ownerPairing) return;
    const bool clearContext = m_quickSettingsOverlayActive &&
        m_quickSettingsOverlaySelectedIndex == modules::quick_settings_items::OWNER_PAIRING;
    m_ownerPairing->SyncLocalClearContext(clearContext, m_quickSettingsOverlayPhysicalEnd);
}

void Session::HandleQuickSettingsAction(
    const modules::QuickSettingsAction& action) {
    if (m_quickSettingsOverlayActive &&
        (action.type == modules::QuickSettingsActionType::SelectPrevious ||
         action.type == modules::QuickSettingsActionType::SelectNext ||
         action.type == modules::QuickSettingsActionType::AdjustIncrease ||
         action.type == modules::QuickSettingsActionType::AdjustDecrease)) {
        m_quickSettingsOverlayIdleElapsed.Reset();
    }
    switch (action.type) {
        case modules::QuickSettingsActionType::OpenOverlay:
            OpenQuickSettings(action.owner);
            return;
        case modules::QuickSettingsActionType::CloseOverlay:
            CloseQuickSettings();
            m_controlFeedback.Trigger(modules::ControlFeedbackAction::Cancel,
                action.owner, modules::ControlRole::X);
            return;
        case modules::QuickSettingsActionType::SelectPrevious:
            if (m_quickSettingsOverlaySelectedIndex > 0U) {
                --m_quickSettingsOverlaySelectedIndex;
                m_controlFeedback.Trigger(modules::ControlFeedbackAction::Navigate,
                    action.owner, modules::ControlRole::Left, {}, modules::ControlFeedbackScene::QuickSetup);
                ShowQuickSettingsSelection();
            }
            return;
        case modules::QuickSettingsActionType::SelectNext:
            if (m_quickSettingsOverlaySelectedIndex + 1U <
                (m_ownerPairing ? modules::quick_settings_items::TOTAL_COUNT :
                                 modules::quick_settings_items::OWNER_PAIRING)) {
                ++m_quickSettingsOverlaySelectedIndex;
                m_controlFeedback.Trigger(modules::ControlFeedbackAction::Navigate,
                    action.owner, modules::ControlRole::Right, {}, modules::ControlFeedbackScene::QuickSetup);
                ShowQuickSettingsSelection();
            }
            return;
        case modules::QuickSettingsActionType::AdjustIncrease:
        case modules::QuickSettingsActionType::AdjustDecrease:
            break;
        default:
            return;
    }

    const bool oldSwap = ControlBanksSwapped();
    const modules::QuickSettingsMutationResult result =
        modules::ApplyQuickSettingsAdjustment({
                                                  .settings = m_settings,
                                                  .controlMutation = m_controlMutation,
                                                  .overlay = m_menuOverlay,
                                                  .background = m_gridFrameBeforeMenuOverlay,
                                                  .owner = m_quickSettingsOverlayOwner,
                                                  .selectedIndex = m_quickSettingsOverlaySelectedIndex,
                                                  .nowMs = m_nowMs,
                                                  .settingsLoaded = true,
                                                  .firmwareVersion = std::string_view{},
                                              },
                                              action);
    if (!result.handled) {
        return;
    }
    if (result.hasOutcome) {
        const auto feedbackOwner = oldSwap == ControlBanksSwapped() ? action.owner : modules::SwapP1P2Owner(action.owner);
        if (result.outcome.status == modules::ControlMutationStatus::Applied)
            m_controlFeedback.Trigger(modules::ControlFeedbackAction::Acknowledge, feedbackOwner,
                action.type == modules::QuickSettingsActionType::AdjustIncrease
                    ? modules::ControlRole::Up : modules::ControlRole::Down,
                {}, modules::ControlFeedbackScene::QuickSetup);
        const bool blocked =
            result.showBlockedFeedback &&
            result.outcome.status ==
                modules::ControlMutationStatus::Rejected;
        RecordBoardControlOutcome(result.outcome, !blocked);
        if (blocked) {
            const modules::QuickSettingsBlockedFeedback feedback =
                modules::ClassifyQuickSettingsBlockedFeedback(
                    result.outcome.reason);
            if (feedback.visible) {
                if (feedback.flashAllPieces) {
                    m_stateMachine.TriggerAllPiecesFlash(1000U);
                }
                if (feedback.edgeCue != modules::EdgeLedCueKind::None) {
                    m_edgeCue.Show(
                        feedback.edgeCue,
                        m_quickSettingsOverlayOwner,
                        m_nowMs);
                }
            }
        }
    }
}

} // namespace runtime::virtual_litch_internal
#endif
