#pragma once

#include <cstdint>

#include "hardware/board_revision.hpp"
#include "modules/factory_radio_protocol.hpp"
#include "modules/user_input.hpp"

namespace modules {

struct HardwareTestRecord;

enum class ManufacturingTestLocalCheck : uint8_t {
    LedDrive = 0U,
    I2cExpanders = 1U,
    LightSensors = 2U,
    MotionSensor = 3U,
};

enum class ManufacturingTestStage : uint8_t {
    Inactive = 0U,
    Local,
    Radio,
    Persisting,
    Passed,
    PassedRadioBypassed,
    Failed,
    OverrideSaved,
};

enum class ManufacturingTestRadioResult : uint8_t {
    Unverified = 0U,
    Verified,
    Bypassed,
    PeerVerified,
    Forced, // Historical v1 engineering bypass, never new verified proof.
    Override,
};

struct ManufacturingTestConfig {
    hardware::BoardRevision revision = hardware::BoardRevision::Unknown;
    uint16_t expectedButtonMask = 0U;
    uint8_t requiredAutomatedCheckMask = 0U;
    bool motionSensorRequired = false;

    static constexpr ManufacturingTestConfig ForRevision(
        const hardware::BoardRevision revision) {
        const bool hasP2 = revision != hardware::BoardRevision::V1C3;
        const bool isV1 = revision == hardware::BoardRevision::V1C3;
        const bool isV3 = hardware::HasFourLedSquares(revision);
        const uint8_t requiredChecks = static_cast<uint8_t>(
            (1U << static_cast<uint8_t>(ManufacturingTestLocalCheck::I2cExpanders)) |
            (isV1 ? 0U
                  : (1U << static_cast<uint8_t>(
                         ManufacturingTestLocalCheck::LightSensors))) |
            (isV3 ? (1U << static_cast<uint8_t>(
                        ManufacturingTestLocalCheck::MotionSensor))
                  : 0U));
        return ManufacturingTestConfig{
            revision,
            static_cast<uint16_t>(hasP2 ? 0x37EFU : 0x006FU),
            requiredChecks,
            isV3,
        };
    }
};

enum class LightSensorResponseStage : uint8_t { Baseline, Change, Passed };

struct LightSensorResponseEvidence {
    LightSensorResponseStage stage = LightSensorResponseStage::Baseline;
    uint32_t lastLux = 0U;
    // The Console retains the historical room/covered/recovered field names.
    // These now hold the baseline and the lower/upper accepted lux readings.
    uint32_t roomLux = 0U;
    uint32_t coveredLux = 0U;
    uint32_t recoveredLux = 0U;
    uint8_t consecutiveSamples = 0U;
};

struct ManufacturingLightSample {
    uint8_t validMask = 0U;
    uint32_t lux[2] = {};
};

struct ManufacturingTestProgress {
    uint8_t buttonsObserved = 0U;
    uint8_t buttonsRequired = 0U;
    bool buttonsComplete = false;
    uint8_t hallClearObserved = 0U;
    uint8_t hallActiveObserved = 0U;
    bool hallComplete = false;
    uint8_t automatedChecksPassedMask = 0U;
    uint8_t automatedChecksRequiredMask = 0U;
    bool automatedWithinTenSeconds = false;
    bool motionEventObserved = false;
    bool ledEmissionConfirmed = false;
    uint8_t radioChannelMask = 0U;
    uint16_t observedButtonMask = 0U;
    uint16_t expectedButtonMask = 0U;
    uint64_t hallClearMask = 0ULL;
    uint64_t hallActiveMask = 0ULL;
    // Physical control-bank state. Production mode deliberately disables the
    // gameplay orientation transform before delivering these events.
    uint16_t pressedButtonMask = 0U;
    // Bit 0/1 identify the two individually responding ambient-light devices.
    // The automated LightSensors link check requires both bits.
    uint8_t lightSensorLinkMask = 0U;
    uint64_t hallOccupiedMask = 0ULL;
    bool hallSnapshotValid = false;
    uint8_t lightSensorResponseMask = 0U;
    LightSensorResponseEvidence lightSensors[2] = {};
    uint8_t failedAutomatedCheckMask = 0U;
    uint8_t expanderLinkMask = 0U;
    bool expanderLinksKnown = false;
    bool motionPresenceKnown = false;
    bool motionPresent = false;
    uint8_t diagnosticFaultMask = 0U;
};

class ManufacturingTestSession final {
  public:
    void Begin(const ManufacturingTestConfig& config, uint32_t nowMs);

    // The caller must supply unrotated events so event.owner identifies the
    // printed physical P1/P2 control bank, not a logical gameplay owner.
    void ObserveButton(const ButtonEvent& event, uint32_t nowMs);
    void ResetButtonInput();
    void ObserveHallSnapshot(bool valid, uint64_t occupiedMask,
                             uint32_t nowMs);
    void ReportAutomatedCheck(ManufacturingTestLocalCheck check,
                              bool passed,
                              uint32_t nowMs);
    void ReportCurrentSensorCheck(ManufacturingTestLocalCheck check, bool passed, uint32_t nowMs);
    void ConfirmLedEmission(bool passed, uint32_t nowMs);
    void ObserveMotionEvent(uint32_t nowMs);
    void ObserveLightSample(const ManufacturingLightSample& sample, uint32_t nowMs);
    // An operator-driven display change can alter the light reaching the board.
    // Start a new baseline for unfinished checks and retain completed proof.
    void ResetPendingLightResponses();
    uint8_t GetLightSensorResponseMask() const { return m_lightSensorResponseMask; }
    bool HasPressedButtons() const { return m_pressedButtonMask != 0U; }
    bool HasCurrentSensorFailure() const { return m_failedAutomatedCheckMask != 0U; }
    void ReportRadioVerified(FactoryRadioChannel channel, uint32_t nowMs);
    void Tick(uint32_t nowMs);
    void SetAutomaticVerifier(bool verifier, uint32_t nowMs);
    void ResetPendingRadioProof();
    void RequestOverride(uint32_t nowMs);
    // Reinspection preserves collected proof, but requires a fresh O review
    // and subsequent current empty Hall before persistence may be admitted.
    void InvalidateOperatorReview();
    void ConfirmOperatorReview(uint32_t nowMs);
    bool IsOperatorReviewReady() const;

    bool BuildPendingRecord(HardwareTestRecord& out) const;
    void ReportPersistenceResult(bool saved, ManufacturingTestRadioResult savedRadio =
        ManufacturingTestRadioResult::Unverified);
    void ReportPeerVerifierUpgrade();

    ManufacturingTestProgress GetProgress() const;
    ManufacturingTestStage GetStage() const { return m_stage; }
    ManufacturingTestRadioResult GetRequestedRadioResult() const {
        return m_requestedRadioResult;
    }

  private:
    static uint16_t PhysicalButtonBit(const ButtonEvent& event);
    bool IsLocalComplete() const;
    void AdvanceAfterLocalObservation(uint32_t nowMs);
    void UpdateAutomatedCompletion(uint32_t nowMs);

    ManufacturingTestConfig m_config{};
    uint16_t m_observedButtonMask = 0U;
    uint16_t m_pressedButtonMask = 0U;
    uint64_t m_hallClearMask = 0ULL;
    uint64_t m_hallActiveMask = 0ULL;
    uint64_t m_currentHallMask = 0ULL;
    bool m_currentHallValid = false;
    bool m_finalHallObserved = false;
    uint8_t m_passedAutomatedCheckMask = 0U;
    uint8_t m_failedAutomatedCheckMask = 0U;
    bool m_sensorDeadlineFailure = false;
    bool m_automaticVerifier = false;
    bool m_ledEmissionConfirmed = false;
    bool m_operatorReviewComplete = false;
    bool m_motionEventObserved = false;
    LightSensorResponseEvidence m_lightSensors[2] = {};
    uint8_t m_lightSensorResponseMask = 0U;
    uint32_t m_startedAtMs = 0U;
    uint32_t m_automatedCompletedAtMs = 0U;
    uint32_t m_localCompletedAtMs = 0U;
    uint32_t m_radioCompletedAtMs = 0U;
    bool m_automatedComplete = false;
    uint8_t m_verifiedRadioMask = 0U;
    ManufacturingTestStage m_stage = ManufacturingTestStage::Inactive;
    ManufacturingTestRadioResult m_requestedRadioResult =
        ManufacturingTestRadioResult::Unverified;
};

} // namespace modules
