#include "virtual_litch_session_internal.hpp"
#ifdef LITCH_NATIVE
#include "modules/control_led_feedback_projection.hpp"
#include "modules/quick_settings_hold_preview.hpp"

namespace runtime::virtual_litch_internal {
void Session::ComposeControlFeedback(const modules::led_frame_composer::ComposeInputs& presentation) {
    if (HardwareProfile(m_config).revision != hardware::BoardRevision::V3_3S3) {
        m_controlFeedback.Reset();
        return;
    }
    const auto* art = m_gamePersistence != nullptr ? &m_gamePersistence->Litchcraft() : nullptr;
    const bool suppressed = m_snapshot.system.safeModeActive || m_stateMachine.IsTextOverlayActive() || m_identifyOverlayActive || m_gameResetHoldActive ||
        m_stateMachine.IsPromotionActive() ||
        (m_gamePersistence != nullptr && m_gamePersistence->GetKingResignationOwner() != modules::UiOwner::None) ||
        runtime::IsRuntimeHealthPlayPaused(m_runtimeHealth.Snapshot()) ||
        (m_menuOverlay.IsActive() && !m_quickSettingsOverlayActive);
    const char* startKey = m_quickSettingsOverlaySelectedIndex == modules::quick_settings_items::FIRST_PLAYER ? "GAME_FIRST_PLAYER"
        : m_quickSettingsOverlaySelectedIndex == modules::quick_settings_items::ROTATE ? "BOARD_ROTATE"
        : m_quickSettingsOverlaySelectedIndex == modules::quick_settings_items::TIMER ? "TIMER" : nullptr;
    const bool adjustmentsAllowed = startKey == nullptr ||
        (m_gamePersistence != nullptr && m_gamePersistence->CheckControlStartOnlyGate(startKey) == modules::ControlCoordinatorResult::Applied);
    const auto preview = presentation.menuIntent;
    const auto count = m_ownerPairing ? modules::quick_settings_items::TOTAL_COUNT :
        modules::quick_settings_items::OWNER_PAIRING;
    m_controlFeedback.Compose(modules::BuildControlLedFeedbackInputs({
        m_settings, art, &m_hintButtonController, preview,
        {presentation.beginnerModeP1Enabled, presentation.beginnerModeP2Enabled},
        m_quickSettingsOverlaySelectedIndex, count, m_quickSettingsOverlayActive, suppressed, adjustmentsAllowed, presentation.menuIntent.menuHoldActive ? &m_quickSettingsOverlayController : nullptr, presentation.interactionSnapshot}),
        m_fixedPhysicalStatusOverlay);
}
} // namespace runtime::virtual_litch_internal
#endif
