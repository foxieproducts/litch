#include "modules/manufacturing_test_session.hpp"

#include <algorithm>

#include "modules/hardware_test_record.hpp"

namespace modules {

namespace {

uint8_t Popcount16(const uint16_t value) {
    return static_cast<uint8_t>(__builtin_popcount(value));
}

uint8_t Popcount64(const uint64_t value) {
    return static_cast<uint8_t>(__builtin_popcountll(value));
}

void ResetPendingLightResponse(LightSensorResponseEvidence& evidence) {
    if (evidence.stage == LightSensorResponseStage::Passed) return;
    const uint32_t lastLux = evidence.lastLux;
    evidence = {};
    evidence.lastLux = lastLux;
}

uint8_t ButtonOffset(const UserButton button) {
    switch (button) {
        case UserButton::Up:
            return 0U;
        case UserButton::Down:
            return 1U;
        case UserButton::Left:
            return 2U;
        case UserButton::Right:
            return 3U;
        case UserButton::P1X:
        case UserButton::P2X:
            return 5U;
        case UserButton::P1O:
        case UserButton::P2O:
            return 6U;
        case UserButton::Press:
        case UserButton::None:
            break;
    }
    return 16U;
}

} // namespace

void ManufacturingTestSession::Begin(const ManufacturingTestConfig& config,
                                     const uint32_t nowMs) {
    m_config = config;
    m_automaticVerifier = false;
    m_observedButtonMask = 0U;
    m_pressedButtonMask = 0U;
    m_hallClearMask = 0ULL;
    m_hallActiveMask = 0ULL;
    m_currentHallMask = 0ULL;
    m_currentHallValid = false;
    m_finalHallObserved = false;
    m_passedAutomatedCheckMask = 0U;
    m_failedAutomatedCheckMask = 0U;
    m_sensorDeadlineFailure = false;
    m_ledEmissionConfirmed = false;
    m_operatorReviewComplete = false;
    m_motionEventObserved = false;
    m_lightSensors[0] = {};
    m_lightSensors[1] = {};
    m_lightSensorResponseMask = 0U;
    m_startedAtMs = nowMs;
    m_automatedCompletedAtMs = nowMs;
    m_localCompletedAtMs = nowMs;
    m_radioCompletedAtMs = nowMs;
    m_automatedComplete = false;
    m_verifiedRadioMask = 0U;
    m_stage = ManufacturingTestStage::Local;
    m_requestedRadioResult = ManufacturingTestRadioResult::Unverified;
}

uint16_t ManufacturingTestSession::PhysicalButtonBit(
    const ButtonEvent& event) {
    if (event.owner != UiOwner::P1 && event.owner != UiOwner::P2) {
        return 0U;
    }
    const uint8_t offset = ButtonOffset(event.button);
    if (offset >= 7U) {
        return 0U;
    }
    const uint8_t ownerOffset = event.owner == UiOwner::P2 ? 7U : 0U;
    return static_cast<uint16_t>(1U << (ownerOffset + offset));
}

void ManufacturingTestSession::ResetButtonInput() {
    // Lost chronology cannot prove either complete button coverage or all-up.
    m_observedButtonMask = 0U;
    m_pressedButtonMask = 0U;
    InvalidateOperatorReview();
}

void ManufacturingTestSession::ObserveButton(const ButtonEvent& event,
                                             const uint32_t nowMs) {
    if (m_stage != ManufacturingTestStage::Local &&
        m_stage != ManufacturingTestStage::Radio &&
        !(m_stage == ManufacturingTestStage::Failed && m_sensorDeadlineFailure)) return;
    const uint16_t physicalBit = PhysicalButtonBit(event);
    if (physicalBit != 0U && (event.event == fxe::Button::Event::Press ||
                             event.event == fxe::Button::Event::Release)) {
        m_finalHallObserved = false;
    }
    if (event.event == fxe::Button::Event::Press) {
        m_observedButtonMask |= physicalBit;
        m_pressedButtonMask |= physicalBit;
    }
    if (event.event == fxe::Button::Event::Release) {
        m_pressedButtonMask &= static_cast<uint16_t>(~physicalBit);
    }
    AdvanceAfterLocalObservation(nowMs);
}

void ManufacturingTestSession::ObserveHallSnapshot(
    const bool valid,
    const uint64_t occupiedMask,
    const uint32_t nowMs) {
    if (m_stage != ManufacturingTestStage::Local &&
        m_stage != ManufacturingTestStage::Radio) {
        return;
    }
    m_currentHallValid = valid;
    if (!valid) {
        m_finalHallObserved = false;
        if (m_stage == ManufacturingTestStage::Radio &&
            m_config.revision != hardware::BoardRevision::V1C3) {
            m_stage = ManufacturingTestStage::Local;
        }
        return;
    }
    m_currentHallMask = occupiedMask;
    m_hallActiveMask |= occupiedMask;
    m_hallClearMask |= ~occupiedMask;
    m_finalHallObserved = m_operatorReviewComplete && IsOperatorReviewReady();
    if (!m_finalHallObserved) {
        if (m_stage == ManufacturingTestStage::Radio) m_stage = ManufacturingTestStage::Local;
        return;
    }
    AdvanceAfterLocalObservation(nowMs);
}

void ManufacturingTestSession::ReportAutomatedCheck(
    const ManufacturingTestLocalCheck check,
    const bool passed,
    const uint32_t nowMs) {
    if (m_stage != ManufacturingTestStage::Local) {
        return;
    }
    if (!passed) {
        m_stage = ManufacturingTestStage::Failed;
        return;
    }
    m_passedAutomatedCheckMask |= static_cast<uint8_t>(
        1U << static_cast<uint8_t>(check));
    UpdateAutomatedCompletion(nowMs);
    AdvanceAfterLocalObservation(nowMs);
}

void ManufacturingTestSession::ReportCurrentSensorCheck(
    const ManufacturingTestLocalCheck check, const bool passed, const uint32_t nowMs) {
    if (m_stage != ManufacturingTestStage::Local && m_stage != ManufacturingTestStage::Radio &&
        !(m_stage == ManufacturingTestStage::Failed && m_sensorDeadlineFailure)) return;
    const uint8_t bit = static_cast<uint8_t>(1U << static_cast<uint8_t>(check));
    if ((m_config.requiredAutomatedCheckMask & bit) == 0U) return;
    if (!passed) {
        m_failedAutomatedCheckMask |= bit;
        m_passedAutomatedCheckMask &= static_cast<uint8_t>(~bit);
        m_finalHallObserved = false;
        m_verifiedRadioMask = 0U;
        if (m_stage == ManufacturingTestStage::Radio) m_stage = ManufacturingTestStage::Local;
        return;
    }
    m_failedAutomatedCheckMask &= static_cast<uint8_t>(~bit);
    m_passedAutomatedCheckMask |= bit;
    if (m_stage == ManufacturingTestStage::Failed && m_failedAutomatedCheckMask == 0U) {
        m_stage = ManufacturingTestStage::Local;
        m_sensorDeadlineFailure = false;
    }
    UpdateAutomatedCompletion(nowMs);
    // Only the next fresh Hall observation may finalize current electronics.
}

void ManufacturingTestSession::UpdateAutomatedCompletion(
    const uint32_t nowMs) {
    const uint8_t passedRequired = static_cast<uint8_t>(
        m_passedAutomatedCheckMask & m_config.requiredAutomatedCheckMask);
    if (!m_automatedComplete &&
        passedRequired == m_config.requiredAutomatedCheckMask) {
        m_automatedComplete = true;
        m_automatedCompletedAtMs = nowMs;
    }
}

void ManufacturingTestSession::ConfirmLedEmission(const bool passed,
                                                  const uint32_t nowMs) {
    if (m_stage != ManufacturingTestStage::Local &&
        !(m_stage == ManufacturingTestStage::Failed && m_sensorDeadlineFailure)) {
        return;
    }
    if (!passed) {
        m_stage = ManufacturingTestStage::Failed;
        return;
    }
    m_ledEmissionConfirmed = true;
    m_finalHallObserved = false;
    AdvanceAfterLocalObservation(nowMs);
}

void ManufacturingTestSession::ObserveMotionEvent(const uint32_t nowMs) {
    if (m_stage != ManufacturingTestStage::Local) {
        return;
    }
    m_motionEventObserved = true;
    AdvanceAfterLocalObservation(nowMs);
}

void ManufacturingTestSession::ObserveLightSample(
    const ManufacturingLightSample& sample, const uint32_t /* nowMs */) {
    if (m_stage != ManufacturingTestStage::Local && m_stage != ManufacturingTestStage::Radio &&
        !(m_stage == ManufacturingTestStage::Failed && m_sensorDeadlineFailure)) { return; }
    // The response can brighten or dim. The 10-lux floor is larger than the
    // 1-lux reporting resolution; consecutive samples reject isolated flicker.
    constexpr uint32_t MIN_LIGHT_CHANGE_LUX = 10U;
    constexpr uint8_t STABLE_SAMPLES = 3U;
    for (uint8_t index = 0U; index < 2U; ++index) {
        auto& evidence = m_lightSensors[index];
        const uint8_t sensorBit = static_cast<uint8_t>(1U << index);
        if ((sample.validMask & sensorBit) == 0U) {
            // A recovered sensor must establish its baseline under the new
            // display and light conditions before it can qualify again.
            ResetPendingLightResponse(evidence);
            continue;
        }
        const uint32_t previousLux = evidence.lastLux;
        const uint32_t lux = sample.lux[index];
        evidence.lastLux = lux;
        if (evidence.stage == LightSensorResponseStage::Passed) continue;

        const uint32_t low = std::min(lux, evidence.roomLux);
        const uint32_t high = std::max(lux, evidence.roomLux);
        const uint32_t difference = high - low;
        if (evidence.stage == LightSensorResponseStage::Baseline) {
            const uint32_t tolerance = std::max<uint32_t>(1U, evidence.roomLux / 4U);
            if (evidence.consecutiveSamples == 0U || difference > tolerance) {
                evidence.roomLux = lux;
                evidence.consecutiveSamples = 0U;
            }
            if (++evidence.consecutiveSamples == STABLE_SAMPLES) {
                evidence.stage = LightSensorResponseStage::Change;
                evidence.consecutiveSamples = 0U;
            }
            continue;
        }
        if (difference < MIN_LIGHT_CHANGE_LUX || low > high / 2U) {
            evidence.consecutiveSamples = 0U;
            continue;
        }
        if ((lux < evidence.roomLux) != (previousLux < evidence.roomLux)) {
            evidence.consecutiveSamples = 0U;
        }
        if (++evidence.consecutiveSamples < STABLE_SAMPLES) continue;

        evidence.stage = LightSensorResponseStage::Passed;
        evidence.consecutiveSamples = 0U;
        evidence.coveredLux = low;
        evidence.recoveredLux = high;
        m_lightSensorResponseMask |= sensorBit;
    }
    // The next valid empty Hall observation owns the transition to Radio.
}

void ManufacturingTestSession::ResetPendingLightResponses() {
    for (auto& evidence : m_lightSensors) {
        ResetPendingLightResponse(evidence);
    }
}

bool ManufacturingTestSession::IsOperatorReviewReady() const {
    const ManufacturingTestProgress progress = GetProgress();
    const uint8_t passedRequired = static_cast<uint8_t>(
        m_passedAutomatedCheckMask & m_config.requiredAutomatedCheckMask);
    constexpr uint8_t ALS_LINK_CHECK = 1U << static_cast<uint8_t>(ManufacturingTestLocalCheck::LightSensors);
    const bool responsesReady = (m_config.requiredAutomatedCheckMask & ALS_LINK_CHECK) == 0U ||
        m_lightSensorResponseMask == 0x03U;
    return progress.buttonsComplete && progress.hallComplete &&
           passedRequired == m_config.requiredAutomatedCheckMask &&
           progress.automatedWithinTenSeconds && m_ledEmissionConfirmed && responsesReady;
}

bool ManufacturingTestSession::IsLocalComplete() const {
    return m_operatorReviewComplete && IsOperatorReviewReady() &&
           (m_config.revision == hardware::BoardRevision::V1C3 || m_finalHallObserved);
}

void ManufacturingTestSession::AdvanceAfterLocalObservation(
    const uint32_t nowMs) {
    if (m_stage == ManufacturingTestStage::Local && IsLocalComplete()) {
        m_localCompletedAtMs = nowMs;
        m_stage = ManufacturingTestStage::Radio;
    }
    if (m_stage == ManufacturingTestStage::Radio && IsLocalComplete()) {
        if (m_verifiedRadioMask == FACTORY_RADIO_ALL_CHANNELS) {
            m_requestedRadioResult = m_automaticVerifier
                ? ManufacturingTestRadioResult::PeerVerified
                : ManufacturingTestRadioResult::Verified;
            m_stage = ManufacturingTestStage::Persisting;
        }
    }
}

void ManufacturingTestSession::SetAutomaticVerifier(const bool verifier,
                                                   const uint32_t nowMs) {
    if (m_config.revision == hardware::BoardRevision::V1C3) {
        return;
    }
    if (m_automaticVerifier != verifier) { m_finalHallObserved = false; }
    m_automaticVerifier = verifier;
    (void)nowMs; // Election grants service admission, never completion proof.
}

void ManufacturingTestSession::Tick(const uint32_t nowMs) {
    if (m_stage == ManufacturingTestStage::Local) {
        constexpr uint32_t AUTOMATED_DEADLINE_MS = 10000U;
        if ((!m_automatedComplete || m_failedAutomatedCheckMask != 0U) &&
            static_cast<uint32_t>(nowMs - m_startedAtMs) >
                AUTOMATED_DEADLINE_MS) {
            m_sensorDeadlineFailure = m_failedAutomatedCheckMask != 0U;
            m_stage = ManufacturingTestStage::Failed;
        }
        return;
    }
    if (m_stage != ManufacturingTestStage::Radio) {
        return;
    }
}

void ManufacturingTestSession::InvalidateOperatorReview() {
    if (m_stage == ManufacturingTestStage::Local ||
        m_stage == ManufacturingTestStage::Radio) {
        m_operatorReviewComplete = m_finalHallObserved = false;
    }
}

void ManufacturingTestSession::ConfirmOperatorReview(const uint32_t nowMs) {
    m_operatorReviewComplete = true;
    m_finalHallObserved = false;
    AdvanceAfterLocalObservation(nowMs);
}

void ManufacturingTestSession::RequestOverride(const uint32_t nowMs) {
    if (m_stage == ManufacturingTestStage::Inactive ||
        m_stage == ManufacturingTestStage::Passed ||
        m_stage == ManufacturingTestStage::PassedRadioBypassed) {
        return;
    }
    m_automaticVerifier = false;
    m_verifiedRadioMask = 0U;
    m_automatedCompletedAtMs = nowMs;
    m_localCompletedAtMs = nowMs;
    m_radioCompletedAtMs = nowMs;
    m_requestedRadioResult = ManufacturingTestRadioResult::Override;
    m_stage = ManufacturingTestStage::Persisting;
}

void ManufacturingTestSession::ResetPendingRadioProof() {
    if (m_verifiedRadioMask != FACTORY_RADIO_ALL_CHANNELS &&
        (m_stage == ManufacturingTestStage::Local || m_stage == ManufacturingTestStage::Radio ||
         m_stage == ManufacturingTestStage::PassedRadioBypassed)) {
        m_verifiedRadioMask = 0U;
    }
}

void ManufacturingTestSession::ReportRadioVerified(
    const FactoryRadioChannel channel,
    const uint32_t nowMs) {
    if (m_stage != ManufacturingTestStage::Local &&
        m_stage != ManufacturingTestStage::Radio &&
        m_stage != ManufacturingTestStage::PassedRadioBypassed &&
        !(m_stage == ManufacturingTestStage::Persisting &&
          m_requestedRadioResult == ManufacturingTestRadioResult::Bypassed)) {
        return;
    }
    if (channel != FactoryRadioChannel::Ble) {
        return;
    }
    m_verifiedRadioMask |= FACTORY_RADIO_BLE_CHANNEL;
    if (m_verifiedRadioMask != FACTORY_RADIO_ALL_CHANNELS) {
        return;
    }
    m_radioCompletedAtMs = nowMs;
    if (m_config.revision != hardware::BoardRevision::V1C3 &&
        (m_stage == ManufacturingTestStage::Local || m_stage == ManufacturingTestStage::Radio)) {
        m_finalHallObserved = false;
        return; // Finalize only at the next production sensor observation.
    }
    if (m_stage == ManufacturingTestStage::Radio ||
        m_stage == ManufacturingTestStage::Persisting ||
        m_stage == ManufacturingTestStage::PassedRadioBypassed) {
        m_requestedRadioResult = (m_automaticVerifier ||
                                  m_stage == ManufacturingTestStage::PassedRadioBypassed)
            ? ManufacturingTestRadioResult::PeerVerified
            : ManufacturingTestRadioResult::Verified;
        m_stage = ManufacturingTestStage::Persisting;
    }
}

bool ManufacturingTestSession::BuildPendingRecord(
    HardwareTestRecord& out) const {
    if (m_stage != ManufacturingTestStage::Persisting ||
        (m_requestedRadioResult != ManufacturingTestRadioResult::Verified &&
         m_requestedRadioResult != ManufacturingTestRadioResult::Bypassed &&
         m_requestedRadioResult != ManufacturingTestRadioResult::PeerVerified &&
         m_requestedRadioResult != ManufacturingTestRadioResult::Forced &&
         m_requestedRadioResult != ManufacturingTestRadioResult::Override)) {
        return false;
    }
    out = HardwareTestRecord{
        HARDWARE_TEST_RECORD_VERSION,
        m_config.revision,
        m_requestedRadioResult,
        m_requestedRadioResult != ManufacturingTestRadioResult::Override &&
        m_requestedRadioResult != ManufacturingTestRadioResult::Forced &&
            (m_requestedRadioResult == ManufacturingTestRadioResult::Verified || m_automaticVerifier ||
             m_requestedRadioResult == ManufacturingTestRadioResult::Bypassed ||
             m_requestedRadioResult == ManufacturingTestRadioResult::PeerVerified),
        static_cast<uint8_t>(m_requestedRadioResult == ManufacturingTestRadioResult::Bypassed ||
                                     m_requestedRadioResult == ManufacturingTestRadioResult::Forced ||
                                     m_requestedRadioResult == ManufacturingTestRadioResult::Override
            ? 0U : m_verifiedRadioMask),
        static_cast<uint32_t>(m_automatedCompletedAtMs - m_startedAtMs),
        static_cast<uint32_t>(m_localCompletedAtMs -
                              m_automatedCompletedAtMs),
        m_verifiedRadioMask == FACTORY_RADIO_ALL_CHANNELS
            ? static_cast<uint32_t>(m_radioCompletedAtMs - m_startedAtMs)
            : 0U,
    };
    return true;
}

void ManufacturingTestSession::ReportPersistenceResult(const bool saved,
    const ManufacturingTestRadioResult savedRadio) {
    if (m_stage != ManufacturingTestStage::Persisting &&
        !(m_stage == ManufacturingTestStage::PassedRadioBypassed && !saved)) {
        return;
    }
    if (!saved) {
        m_stage = ManufacturingTestStage::Failed;
        return;
    }
    if (savedRadio != ManufacturingTestRadioResult::Unverified) {
        m_requestedRadioResult = savedRadio;
    }
    if (m_requestedRadioResult == ManufacturingTestRadioResult::Override) {
        m_stage = ManufacturingTestStage::OverrideSaved;
        return;
    }
    m_stage = m_requestedRadioResult ==
                      ManufacturingTestRadioResult::Bypassed
                  ? ManufacturingTestStage::PassedRadioBypassed
                  : ManufacturingTestStage::Passed;
    if (m_stage == ManufacturingTestStage::PassedRadioBypassed &&
        m_verifiedRadioMask == FACTORY_RADIO_ALL_CHANNELS) {
        m_requestedRadioResult = ManufacturingTestRadioResult::PeerVerified;
        m_stage = ManufacturingTestStage::Persisting;
    }
}

void ManufacturingTestSession::ReportPeerVerifierUpgrade() {
    if (m_stage != ManufacturingTestStage::PassedRadioBypassed ||
        m_verifiedRadioMask != FACTORY_RADIO_ALL_CHANNELS) {
        return;
    }
    m_requestedRadioResult = ManufacturingTestRadioResult::PeerVerified;
    m_stage = ManufacturingTestStage::Passed;
}

ManufacturingTestProgress ManufacturingTestSession::GetProgress() const {
    const uint16_t observed = static_cast<uint16_t>(
        m_observedButtonMask & m_config.expectedButtonMask);
    return ManufacturingTestProgress{
        Popcount16(observed),
        Popcount16(m_config.expectedButtonMask),
        observed == m_config.expectedButtonMask &&
            (m_config.revision == hardware::BoardRevision::V1C3 ||
             m_pressedButtonMask == 0U),
        Popcount64(m_hallClearMask),
        Popcount64(m_hallActiveMask),
        m_hallClearMask == UINT64_MAX &&
            m_hallActiveMask == UINT64_MAX &&
            (m_config.revision == hardware::BoardRevision::V1C3 ||
             (m_currentHallValid && m_currentHallMask == 0ULL)),
        m_passedAutomatedCheckMask,
        m_config.requiredAutomatedCheckMask,
        m_automatedComplete &&
            static_cast<uint32_t>(m_automatedCompletedAtMs - m_startedAtMs) <=
                10000U,
        m_motionEventObserved,
        m_ledEmissionConfirmed,
        m_verifiedRadioMask,
        observed,
        m_config.expectedButtonMask,
        m_hallClearMask,
        m_hallActiveMask,
        m_pressedButtonMask,
        0U,
        m_currentHallMask,
        m_currentHallValid,
        m_lightSensorResponseMask,
        {m_lightSensors[0], m_lightSensors[1]},
        m_failedAutomatedCheckMask,
    };
}

} // namespace modules
