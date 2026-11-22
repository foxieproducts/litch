#include "firmware/hardware_test_mode.hpp"

namespace firmware {

void HardwareTestMode::ReportExpanderLinks(const uint8_t linkMask, const uint32_t nowMs) {
    if (!m_active) return;
    const uint8_t links = linkMask & 0x0FU;
    if (m_step == Step::Buttons && (!m_expanderLinksKnown || m_realExpanderLinkMask != links)) {
        m_session.ResetPendingLightResponses();
    }
    m_realExpanderLinkMask = links;
    m_expanderLinksKnown = true;
    m_session.ReportCurrentSensorCheck(modules::ManufacturingTestLocalCheck::I2cExpanders,
        (m_realExpanderLinkMask & static_cast<uint8_t>(~m_diagnosticFaultMask)) == 0x0FU, nowMs);
    m_lastNowMs = nowMs;
    RefreshStep();
}

void HardwareTestMode::ReportMotionPresence(const bool present, const uint32_t nowMs) {
    if (!m_active) return;
    if (m_step == Step::Buttons && (!m_motionPresenceKnown || m_realMotionPresent != present)) {
        m_session.ResetPendingLightResponses();
    }
    m_realMotionPresent = present;
    m_motionPresenceKnown = true;
    m_session.ReportCurrentSensorCheck(modules::ManufacturingTestLocalCheck::MotionSensor,
        present && !(m_diagnosticFaultMask & 0x10U), nowMs);
    m_lastNowMs = nowMs;
    RefreshStep();
}

bool HardwareTestMode::SetDiagnosticFault(const std::string_view target, const uint32_t nowMs) {
    if (!m_active || (GetStage() != modules::ManufacturingTestStage::Local &&
        GetStage() != modules::ManufacturingTestStage::Radio &&
        GetStage() != modules::ManufacturingTestStage::Failed)) return false;
    uint8_t mask = m_diagnosticFaultMask;
    if (target == "clear") mask = 0U;
    else if (target == "motion") mask |= 0x10U;
    else if (target == "light1") mask |= 0x20U;
    else if (target == "light2") mask |= 0x40U;
    else if (target == "expander1") mask |= 0x01U;
    else if (target == "expander2") mask |= 0x02U;
    else if (target == "expander3") mask |= 0x04U;
    else if (target == "expander4") mask |= 0x08U;
    else return false;
    if (m_step == Step::Buttons && mask != m_diagnosticFaultMask) {
        m_session.ResetPendingLightResponses();
    }
    m_diagnosticFaultMask = mask;
    // Reapply actual observations. Clearing cannot make an absent device pass.
    if (m_expanderLinksKnown) ReportExpanderLinks(m_realExpanderLinkMask, nowMs);
    if (m_motionPresenceKnown) ReportMotionPresence(m_realMotionPresent, nowMs);
    const auto progress = m_session.GetProgress();
    const uint8_t lightBit = 1U << static_cast<uint8_t>(modules::ManufacturingTestLocalCheck::LightSensors);
    if (m_lightSensorLinkMask != 0U ||
        ((progress.automatedChecksPassedMask | progress.failedAutomatedCheckMask) & lightBit) != 0U) {
        ReportLightSensorLinks((m_lightSensorLinkMask & 0x01U) != 0U,
            (m_lightSensorLinkMask & 0x02U) != 0U, nowMs);
    }
    return true;
}

} // namespace firmware
