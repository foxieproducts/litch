#include "virtual_litch_session_internal.hpp"

#ifdef LITCH_NATIVE

#include "hardware/motion_board_mapping.hpp"
#include "modules/water_slosh_presentation.hpp"
#include "runtime/square_mapping.hpp"

namespace runtime::virtual_litch_internal {

void Session::ObserveMotionSample(const hardware::Lis2dw12Sample& sample) {
    if (!m_sleep.ShouldIgnoreActivity(firmware::SleepTransitionCause::ValidatedMotionOrTap) &&
        m_gamePersistence != nullptr &&
        hardware::HasFourLedSquares(HardwareProfile(m_config).revision)) {
        m_gamePersistence->Litchcraft().ObserveMotionSample(
            sample.milliGx, sample.milliGy, sample.milliGz);
    }
    m_waterSample = sample;
    m_waterSamplePending = true;
    CaptureFrame();
}

void Session::ObserveWaterAmbientSample(const float lux, const bool available) {
    m_waterAmbientLux = lux;
    m_waterAmbientAvailable = available;
    CaptureFrame();
}

void Session::CaptureWaterSlosh(
    const modules::led_frame_composer::ComposeInputs& inputs) {
    const auto mapping = hardware::MotionBoardMappingFor(
        HardwareProfile(m_config).revision);
    m_waterSlosh.SetAdmitted(
        mapping.IsValid() && m_settings.ReadValue("MOTION_WAKE_ENABLED", true) &&
        modules::CanPresentWaterSlosh(m_snapshot,
            {inputs.runtimeHealth.state, m_boardObservationSequence != 0U,
             inputs.higherPriorityStatusOverlayActive ||
                 m_settings.IsDisabled() ||
                 m_settings.GetLastError() != modules::SettingsStoreError::None ||
                 inputs.menuActive || inputs.menuIntent.menuActive ||
                 inputs.edgeCueIntent.kind != modules::EdgeLedCueKind::None,
             m_sleep.GetState() == firmware::SleepState::Awake,
             m_settings.ReadValue("MINIMAL_ANIMATIONS", false)}), m_nowMs);
    if (m_waterSamplePending) {
        modules::WaterSloshMotion motion;
        if (mapping.Transform(m_waterSample, motion)) {
            m_waterSlosh.Observe(motion, m_nowMs);
        }
        m_waterSamplePending = false;
    }
    m_waterSlosh.SetAmbientLight(m_waterAmbientLux, m_waterAmbientAvailable);
    m_waterSlosh.Update(m_nowMs);
}

} // namespace runtime::virtual_litch_internal

#endif
