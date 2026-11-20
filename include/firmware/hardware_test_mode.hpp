#pragma once

#include <cstdint>
#include <string_view>

#include "hardware/board_revision.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "modules/board_input.hpp"
#include "modules/hardware_test_record.hpp"
#include "modules/manufacturing_test_session.hpp"

namespace firmware {

class HardwareTestMode final {
  public:
    static constexpr uint32_t LED_SCAN_DURATION_MS = 3000U;
    static constexpr uint32_t VERIFIER_SUCCESS_CUE_DURATION_MS = 1200U;
    static constexpr uint32_t OVERRIDE_HOLD_MS = 5000U;
    static constexpr uint32_t OPERATOR_HOLD_INTENT_MS = 500U;
    uint8_t GetOperatorHoldProgress() const { return m_operatorHoldProgress; }

    enum class Step : uint8_t {
        LedRainbow = 0,
        Buttons = 1,
        HallSensors = 2,
        LightSensors = 3,
        WhiteChannel = 4,
        MotionSensor = 5,
        ConfirmLeds = 6,
        Radio = 7,
        Persisting = 8,
        Passed = 9,
        PassedRadioBypassed = 10,
        Failed = 11,
        LedInspect = 12,
    };

    enum class VerifierFeedback : uint8_t {
        Waiting = 0,
        BleProof,
        Success,
    };

    void Enter(hardware::BoardRevision revision, uint32_t nowMs);
    void Stop();
    bool RestoreSavedVerifier(const modules::HardwareTestRecord& record, uint32_t nowMs);
    void RepeatLedScan(uint32_t nowMs);
    uint32_t GetLedScanElapsedMs(uint32_t nowMs) const {
        return static_cast<uint32_t>(nowMs - m_ledScanStartedAtMs);
    }
    void SetAutomaticVerifier(bool verifier, uint32_t nowMs);
    bool IsAutomaticVerifier() const { return m_automaticVerifier; }
    bool IsOverride() const {
        return m_session.GetRequestedRadioResult() ==
            modules::ManufacturingTestRadioResult::Override;
    }
    void ResetPendingRadioProof() { m_session.ResetPendingRadioProof(); }
    modules::ManufacturingTestRadioResult GetRequestedRadioResult() const { return m_session.GetRequestedRadioResult(); }
    bool TakeCompletedBatchExit() { const bool exited = m_completedBatchExit; m_completedBatchExit = false; return exited; }
    bool IsActive() const { return m_active; }

    Step GetStep() const { return m_savedVerifierView ? Step::Passed : m_step; }
    modules::ManufacturingTestStage GetStage() const {
        return m_savedVerifierView ? modules::ManufacturingTestStage::Passed : m_session.GetStage();
    }
    modules::ManufacturingTestProgress GetProgress() const {
        modules::ManufacturingTestProgress progress = m_session.GetProgress();
        progress.lightSensorLinkMask = EffectiveLightSensorLinkMask();
        progress.expanderLinkMask = m_realExpanderLinkMask & static_cast<uint8_t>(~m_diagnosticFaultMask);
        progress.expanderLinksKnown = m_expanderLinksKnown;
        progress.motionPresenceKnown = m_motionPresenceKnown;
        progress.motionPresent = m_realMotionPresent && !(m_diagnosticFaultMask & 0x10U);
        progress.diagnosticFaultMask = m_diagnosticFaultMask;
        return progress;
    }

    uint16_t GetBrightnessScale256() const {
        const uint16_t gain = hardware::BoardHardwareProfileFor(m_config.revision).ledOutputScale256;
        // Resolve the established safe baseline first; the manufacturing output
        // accent dims its actual post-budget emitters by 25%, including limited fields.
        const uint16_t target = m_config.revision == hardware::BoardRevision::V3_3S3 ? 45U : 90U;
        return static_cast<uint16_t>((static_cast<uint32_t>(target) * 256U * 256U + gain * 50U) / (100U * gain));
    }
    uint8_t GetLedChannel() const { return m_operatorStage < 4U ? m_operatorStage : 3U; }
    bool IsOverridePending() const { return IsOverride() && GetStage() == modules::ManufacturingTestStage::Persisting; }
    uint32_t GetStepElapsedMs(uint32_t nowMs) const { return nowMs - m_stepStartedAtMs; }
    uint32_t GetRunElapsedMs(const uint32_t nowMs) const {
        return static_cast<uint32_t>(nowMs - m_startedAtMs);
    }
    uint32_t GetRunId() const { return m_runId; }
    hardware::BoardRevision GetRevision() const { return m_config.revision; }
    VerifierFeedback GetVerifierFeedback() const {
        return m_verifierFeedback;
    }
    uint32_t GetVerifierFeedbackElapsedMs(const uint32_t nowMs) const {
        return static_cast<uint32_t>(nowMs - m_verifierFeedbackStartedAtMs);
    }
    bool IsButtonPressed(modules::UserButton button) const;

    void OnButtonEvent(const modules::ButtonEvent& event, uint32_t nowMs);
    void ResetButtonInput();
    void SetPhysicalButtonTicksAllowed(const bool allowed) { m_physicalButtonTicksAllowed = allowed; }
    void SetPhysicalButtonTickTime(const uint32_t throughMs) {
        m_physicalButtonTickMs = throughMs;
        m_physicalButtonTickTimeValid = true;
    }
    void UpdateSensors(modules::IBoardInput& boardInput, uint32_t nowMs);
    void ReportAutomatedCheck(modules::ManufacturingTestLocalCheck check,
                              bool passed,
                              uint32_t nowMs);
    void ReportLightSensorLinks(bool sensor0Ok, bool sensor1Ok,
                                uint32_t nowMs);
    void ReportExpanderLinks(uint8_t linkMask, uint32_t nowMs);
    void ReportMotionPresence(bool present, uint32_t nowMs);
    // Faults only remove real observations. No state is persisted or device mutated.
    bool SetDiagnosticFault(std::string_view target, uint32_t nowMs);
    void ObserveLightSample(const modules::ManufacturingLightSample& sample, uint32_t nowMs);
    void ConfirmLedEmission(bool passed, uint32_t nowMs);
    bool ForceExit(uint32_t nowMs);
    void ObserveMotionEvent(uint32_t nowMs);
    void ReportRadioVerified(modules::FactoryRadioChannel channel,
                             uint32_t nowMs);
    void Tick(uint32_t nowMs);
    void SetVerifierBleProofActive(bool active, uint32_t nowMs);
    void ReportVerifierProofComplete(uint32_t nowMs);

    bool BuildPendingRecord(modules::HardwareTestRecord& out) const {
        return m_session.BuildPendingRecord(out);
    }
    void ReportPersistenceResult(bool saved, modules::ManufacturingTestRadioResult savedRadio =
        modules::ManufacturingTestRadioResult::Unverified);
    void ReportPeerVerifierUpgrade();

  private:
    uint8_t EffectiveLightSensorLinkMask() const {
        // Physical light1 is A1/driver1; light2 is H8/driver0.
        const uint8_t removed = static_cast<uint8_t>(
            ((m_diagnosticFaultMask & 0x20U) ? 0x02U : 0U) |
            ((m_diagnosticFaultMask & 0x40U) ? 0x01U : 0U));
        return m_lightSensorLinkMask & static_cast<uint8_t>(~removed);
    }
    void RefreshStep();
    void UpdateOverrideGesture(uint32_t nowMs);

    uint8_t m_operatorStage = 0U; // R/G/B/W, Hall, buttons, radio.
    uint8_t m_operatorHoldProgress = 0U;
    uint8_t m_operatorEligibleMask = 0U;
    uint8_t m_chordBlockedMask = 0U;
    uint8_t m_buttonExitReadyMask = 0U; // One completed X cycle; any other control resets it.
    bool m_hallAutoAdvancePending = true;
    bool m_explicitReviewPending = false;
    bool m_savedVerifierView = false;
    bool m_active = false;
    bool m_completedBatchExit = false;
    bool m_automaticVerifier = false;
    uint32_t m_ledScanStartedAtMs = 0U;
    Step m_step = Step::LedRainbow;
    modules::ManufacturingTestConfig m_config{};
    modules::ManufacturingTestSession m_session;
    uint32_t m_startedAtMs = 0U;
    uint32_t m_lastNowMs = 0U;
    uint32_t m_stepStartedAtMs = 0U;
    uint32_t m_runId = 0U;
    uint8_t m_passedAutomatedCheckMask = 0U;
    uint8_t m_lightSensorLinkMask = 0U;
    uint8_t m_diagnosticFaultMask = 0U;
    uint8_t m_realExpanderLinkMask = 0U;
    bool m_expanderLinksKnown = false;
    bool m_motionPresenceKnown = false;
    bool m_realMotionPresent = false;
    uint16_t m_pressedMask = 0U;
    bool m_physicalButtonTicksAllowed = true;
    bool m_physicalButtonTickTimeValid = false;
    uint32_t m_physicalButtonTickMs = 0U;
    bool m_overrideTracking[2] = {false, false};
    uint32_t m_overrideStartedAtMs[2] = {0U, 0U};
    VerifierFeedback m_verifierFeedback = VerifierFeedback::Waiting;
    uint32_t m_verifierFeedbackStartedAtMs = 0U;
};

} // namespace firmware
