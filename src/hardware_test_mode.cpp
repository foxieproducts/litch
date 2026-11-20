#include "firmware/hardware_test_mode.hpp"

#include <algorithm>

#ifdef ARDUINO
#include "logger.hpp"
#endif

namespace firmware {

namespace {

uint8_t CheckBit(const modules::ManufacturingTestLocalCheck check) {
    return static_cast<uint8_t>(1U << static_cast<uint8_t>(check));
}

} // namespace

void HardwareTestMode::Enter(const hardware::BoardRevision revision,
                             const uint32_t nowMs) {
    m_savedVerifierView = false;
    m_lastNowMs = nowMs;
    if (!hardware::SupportsProductionHardwareTest(revision)) {
        Stop();
        return;
    }
    m_config = modules::ManufacturingTestConfig::ForRevision(revision);
    m_session.Begin(m_config, nowMs);
    m_active = true;
    m_completedBatchExit = false;
    m_step = Step::LedRainbow;
    m_startedAtMs = m_stepStartedAtMs = m_lastNowMs = nowMs;
    m_ledScanStartedAtMs = nowMs;
    m_automaticVerifier = false;
    ++m_runId;
    if (m_runId == 0U) {
        m_runId = 1U;
    }
    m_passedAutomatedCheckMask = 0U;
    m_lightSensorLinkMask = 0U;
    m_diagnosticFaultMask = 0U;
    m_realExpanderLinkMask = 0U;
    m_expanderLinksKnown = m_motionPresenceKnown = m_realMotionPresent = false;
    m_pressedMask = 0U;
    m_operatorStage = 0U;
    m_operatorHoldProgress = 0U; m_operatorEligibleMask = 0U; m_chordBlockedMask = 0U;
    m_buttonExitReadyMask = 0U;
    m_hallAutoAdvancePending = true;
    m_explicitReviewPending = false;
    m_overrideTracking[0] = false;
    m_overrideTracking[1] = false;
    m_verifierFeedback = VerifierFeedback::Waiting;
    m_verifierFeedbackStartedAtMs = nowMs;
#ifdef ARDUINO
    logger::Info(
        "hwtest: production run started revision=%u; O advances R/G/B/W or confirms reinspection; X backs up; test and release all controls, then change light at A1/H8 to start the wireless test; same-bank X+O 5s exits with override",
        static_cast<unsigned>(revision));
#endif
}

bool HardwareTestMode::RestoreSavedVerifier(const modules::HardwareTestRecord& record,
                                            const uint32_t nowMs) {
    if (!modules::IsFactoryVerifierEligible(record)) return false;
    Stop();
    // A passed active run can become the saved service without exiting the
    // batch. Stop's one-shot withdrawal belongs only to an actual exit.
    m_completedBatchExit = false;
    m_config = modules::ManufacturingTestConfig::ForRevision(record.revision);
    m_savedVerifierView = true;
    m_automaticVerifier = true;
    m_startedAtMs = m_stepStartedAtMs = m_lastNowMs = nowMs;
    m_verifierFeedback = VerifierFeedback::Waiting;
    m_verifierFeedbackStartedAtMs = nowMs;
    return true;
}

void HardwareTestMode::Stop() {
    if (m_active && GetStage() == modules::ManufacturingTestStage::Passed) m_completedBatchExit = true;
    m_active = false;
    m_savedVerifierView = false;
    m_diagnosticFaultMask = 0U;
    m_operatorHoldProgress = 0U; m_operatorEligibleMask = 0U;
    m_verifierFeedback = VerifierFeedback::Waiting;
#ifdef ARDUINO
    logger::Info("hwtest: stopped; an accepted save may still finish without acknowledging this run");
#endif
}

bool HardwareTestMode::IsButtonPressed(
    const modules::UserButton button) const {
    const uint8_t index = static_cast<uint8_t>(button);
    return index < 16U &&
           (m_pressedMask & static_cast<uint16_t>(1U << index)) != 0U;
}

void HardwareTestMode::ResetButtonInput() {
    m_pressedMask = 0U;
    m_operatorEligibleMask = m_chordBlockedMask = m_buttonExitReadyMask = 0U;
    m_operatorHoldProgress = 0U;
    m_overrideTracking[0] = m_overrideTracking[1] = false;
    m_session.ResetButtonInput();
}

void HardwareTestMode::OnButtonEvent(const modules::ButtonEvent& event,
                                     const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active) { return; }
    if (event.event == fxe::Button::Event::Release) {
        UpdateOverrideGesture(nowMs);
        if (!m_active) return;
    }
    // Radio X's deliberate release returns to reinspection. Revoke the old
    // review before this release can combine with newly arrived radio proof.
    const uint8_t radioBackBit = event.button == modules::UserButton::P1X ? 1U
        : event.button == modules::UserButton::P2X ? 4U : 0U;
    if (m_step == Step::Radio && event.event == fxe::Button::Event::Release &&
        (m_operatorEligibleMask & radioBackBit) != 0U) {
        m_session.InvalidateOperatorReview();
        m_explicitReviewPending = true;
    }
    if (m_step == Step::Buttons && (event.event == fxe::Button::Event::Press ||
                                  event.event == fxe::Button::Event::Release)) {
        // Button LEDs change on both edges. Their light cannot serve as the
        // before-and-after evidence for an uncovered physical sensor.
        m_session.ResetPendingLightResponses();
    }
    // Observe the specific physical end before interpreting navigation. Repeats
    // never navigate; a same-bank chord consumes both releases until fully up.
    m_session.ObserveButton(event, nowMs);
    const uint8_t index = static_cast<uint8_t>(event.button);
    if (index < 16U && event.event == fxe::Button::Event::Press)
        m_pressedMask |= static_cast<uint16_t>(1U << index);
    if (index < 16U && event.event == fxe::Button::Event::Release)
        m_pressedMask &= static_cast<uint16_t>(~(1U << index));
    if (m_step == Step::Buttons) {
        const auto candidate = m_buttonExitReadyMask == 1U
            ? modules::UserButton::P1X : modules::UserButton::P2X;
        if (m_buttonExitReadyMask != 0U && event.button != candidate)
            m_buttonExitReadyMask = 0U;
        // Any overlapping control cancels an in-flight X cycle. Its release
        // cannot later become the first or second deliberate complete cycle.
        if (event.event == fxe::Button::Event::Press &&
            (m_pressedMask & static_cast<uint16_t>(m_pressedMask - 1U)) != 0U) {
            m_buttonExitReadyMask = 0U;
            m_operatorEligibleMask &= static_cast<uint8_t>(~0x05U);
        }
    }
    int8_t control = -1;
    switch (event.button) {
        case modules::UserButton::P1X: control = 0; break;
        case modules::UserButton::P1O: control = 1; break;
        case modules::UserButton::P2X: control = 2; break;
        case modules::UserButton::P2O: control = 3; break;
        default: break;
    }
    if (control >= 0) {
        const uint8_t bank = control / 2;
        const uint8_t bit = 1U << control;
        const auto x = bank == 0U ? modules::UserButton::P1X : modules::UserButton::P2X;
        const auto o = bank == 0U ? modules::UserButton::P1O : modules::UserButton::P2O;
        if (IsButtonPressed(x) && IsButtonPressed(o)) {
            m_chordBlockedMask |= 1U << bank;
            m_buttonExitReadyMask = 0U;
            m_operatorEligibleMask &= static_cast<uint8_t>(~(3U << (bank * 2U)));
        }
        if (event.event == fxe::Button::Event::Press && !(m_chordBlockedMask & (1U << bank)) &&
            (m_step != Step::Buttons || (control % 2) != 0 ||
             m_pressedMask == static_cast<uint16_t>(1U << index)))
            m_operatorEligibleMask |= bit;
        if (event.event == fxe::Button::Event::Release) {
            bool navigate = (m_operatorEligibleMask & bit) != 0U;
            m_operatorEligibleMask &= static_cast<uint8_t>(~bit);
            if (navigate && (control % 2) == 0 && m_step == Step::Buttons) {
                const uint8_t bankBit = 1U << bank;
                // Only consecutive fresh complete cycles of this same physical
                // X count. Intervening controls and chords clear the candidate.
                navigate = (m_buttonExitReadyMask & bankBit) != 0U;
                m_buttonExitReadyMask = bankBit;
                if (!navigate) m_operatorStage = 5U;
            }
            if (navigate && (GetStage() == modules::ManufacturingTestStage::Local ||
                             GetStage() == modules::ManufacturingTestStage::Radio ||
                             (GetStage() == modules::ManufacturingTestStage::Failed && GetProgress().failedAutomatedCheckMask != 0U))) {
                if ((control % 2) == 0) {
                    if (m_step == Step::Buttons) {
                        m_operatorStage = 4U;
                        m_hallAutoAdvancePending = false;
                    }
                    else if (m_operatorStage > 0U) --m_operatorStage;
                } else if (m_operatorStage < 6U) {
                    if (m_operatorStage == 3U) {
                        ReportAutomatedCheck(modules::ManufacturingTestLocalCheck::LedDrive, true, nowMs);
                        m_session.ConfirmLedEmission(true, nowMs);
                    }
                    if (m_operatorStage == 5U) {
                        if (m_explicitReviewPending && m_session.GetProgress().buttonsComplete) {
                            m_session.ConfirmOperatorReview(nowMs);
                            m_explicitReviewPending = false;
                            ++m_operatorStage;
                        }
                    } else {
                        ++m_operatorStage;
                    }
                }
                m_ledScanStartedAtMs = nowMs;
            }
            if (!IsButtonPressed(x) && !IsButtonPressed(o))
                m_chordBlockedMask &= static_cast<uint8_t>(~(1U << bank));
        }
    }
    UpdateOverrideGesture(nowMs);
    RefreshStep();
}

void HardwareTestMode::UpdateOverrideGesture(const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    m_operatorHoldProgress = 0U;
    const bool held[2] = {
        IsButtonPressed(modules::UserButton::P1X) &&
            IsButtonPressed(modules::UserButton::P1O),
        IsButtonPressed(modules::UserButton::P2X) &&
            IsButtonPressed(modules::UserButton::P2O),
    };
    for (uint8_t owner = 0U; owner < 2U; ++owner) {
        if (!held[owner]) {
            m_overrideTracking[owner] = false;
            continue;
        }
        if (!m_overrideTracking[owner]) {
            m_overrideTracking[owner] = true;
            m_overrideStartedAtMs[owner] = nowMs;
            continue;
        }
        const uint32_t elapsed = nowMs - m_overrideStartedAtMs[owner];
        if (elapsed >= 0x80000000U) continue;
        if (elapsed >= OPERATOR_HOLD_INTENT_MS)
            m_operatorHoldProgress = static_cast<uint8_t>(std::min<uint32_t>(255U, elapsed * 255U / OVERRIDE_HOLD_MS));
        if (elapsed < OVERRIDE_HOLD_MS) {
            continue;
        }
        ForceExit(nowMs);
        return;
    }
}

bool HardwareTestMode::ForceExit(const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active) return false;
    if (GetStage() != modules::ManufacturingTestStage::Passed) {
        m_session.RequestOverride(nowMs);
    }
    m_automaticVerifier = false;
    m_overrideTracking[0] = m_overrideTracking[1] = false;
    // The firmware boot profile owns exit. Its tracked Override save must
    // complete before a minimal factory boot can reboot into normal firmware.
    Stop();
    RefreshStep();
    return true;
}

void HardwareTestMode::RepeatLedScan(const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active) return;
    m_operatorStage = 0U;
    m_ledScanStartedAtMs = nowMs;
    RefreshStep();
}

void HardwareTestMode::SetAutomaticVerifier(const bool verifier,
                                           const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    // Discovery may restart during local reinspection after a genuine responder
    // proof. Keep that run's proven role; election alone is still insufficient.
    const bool provedResponder = m_automaticVerifier &&
        m_session.GetProgress().radioChannelMask == modules::FACTORY_RADIO_ALL_CHANNELS;
    const bool next = (verifier || provedResponder) && !IsOverride();
    if (next != m_automaticVerifier) m_verifierFeedbackStartedAtMs = nowMs;
    m_automaticVerifier = next;
    m_session.SetAutomaticVerifier(m_automaticVerifier, nowMs);
    RefreshStep();
}

void HardwareTestMode::UpdateSensors(modules::IBoardInput& boardInput,
                                     const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active) {
        return;
    }
    // Keep the final clear evidence fresh while the operator reviews LEDs;
    // a new occupied/stuck input must not reuse an earlier clear snapshot.
    if (m_session.GetStage() == modules::ManufacturingTestStage::Local ||
        m_session.GetStage() == modules::ManufacturingTestStage::Radio) {
        boardInput.Resync();
    }
    uint64_t occupied = 0ULL;
    const bool valid = boardInput.TryReadSnapshot(occupied) && !(m_diagnosticFaultMask & 0x0FU);
    const bool hadSensorFailure = m_session.HasCurrentSensorFailure();
    m_session.ReportCurrentSensorCheck(modules::ManufacturingTestLocalCheck::I2cExpanders, valid, nowMs);
    if (m_step == Step::Buttons && hadSensorFailure != m_session.HasCurrentSensorFailure()) {
        m_session.ResetPendingLightResponses();
    }
    if (m_operatorStage == 5U && !m_explicitReviewPending && valid && occupied == 0ULL &&
        m_session.IsOperatorReviewReady()) {
        m_session.ConfirmOperatorReview(nowMs);
        m_operatorStage = 6U;
    }
    m_session.ObserveHallSnapshot(valid, occupied, nowMs);
    if (m_operatorStage == 4U && m_hallAutoAdvancePending &&
        valid && m_session.GetProgress().hallComplete) {
        m_operatorStage = 5U;
        m_hallAutoAdvancePending = false;
    }
    if (valid && !m_expanderLinksKnown) {
        // Native/legacy board-input fixtures expose only complete snapshots.
        ReportExpanderLinks(0x0FU, nowMs);
    }
    RefreshStep();
}

void HardwareTestMode::ReportAutomatedCheck(
    const modules::ManufacturingTestLocalCheck check,
    const bool passed,
    const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active) {
        return;
    }
    if (check == modules::ManufacturingTestLocalCheck::MotionSensor) {
        ReportMotionPresence(passed, nowMs); return;
    }
    if (check == modules::ManufacturingTestLocalCheck::I2cExpanders) {
        ReportExpanderLinks(passed ? 0x0FU : 0U, nowMs); return;
    }
    if (passed) {
        m_passedAutomatedCheckMask |= CheckBit(check);
    }
    m_session.ReportAutomatedCheck(check, passed, nowMs);
    RefreshStep();
}

void HardwareTestMode::ReportLightSensorLinks(
    const bool sensor0Ok,
    const bool sensor1Ok,
    const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active) {
        return;
    }
    const uint8_t previousLinks = EffectiveLightSensorLinkMask();
    m_lightSensorLinkMask = static_cast<uint8_t>(
        (sensor0Ok ? 0x01U : 0U) | (sensor1Ok ? 0x02U : 0U));
    if (m_step == Step::Buttons && previousLinks != EffectiveLightSensorLinkMask()) {
        m_session.ResetPendingLightResponses();
    }
    m_session.ReportCurrentSensorCheck(modules::ManufacturingTestLocalCheck::LightSensors,
        EffectiveLightSensorLinkMask() == 0x03U, nowMs);
    RefreshStep();
}

void HardwareTestMode::ObserveLightSample(
    const modules::ManufacturingLightSample& sample, const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    // Held controls and fault markers can change the light the board emits.
    if (!m_active || m_step != Step::Buttons || m_session.HasPressedButtons() ||
        m_session.HasCurrentSensorFailure() || m_diagnosticFaultMask != 0U) {
        return;
    }
    auto qualifiedSample = sample;
    qualifiedSample.validMask &= EffectiveLightSensorLinkMask();
    const uint8_t previousResponses = m_session.GetLightSensorResponseMask();
    m_session.ObserveLightSample(qualifiedSample, nowMs);
    if (m_session.GetLightSensorResponseMask() != previousResponses) {
        // A confirmation marker changes the display seen by the other sensor.
        m_session.ResetPendingLightResponses();
    }
    RefreshStep();
}

void HardwareTestMode::ConfirmLedEmission(const bool passed,
                                          const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active) {
        return;
    }
    m_session.ConfirmLedEmission(passed, nowMs);
    RefreshStep();
}

void HardwareTestMode::ObserveMotionEvent(const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active) {
        return;
    }
    m_session.ObserveMotionEvent(nowMs);
    RefreshStep();
}

void HardwareTestMode::ReportRadioVerified(
    const modules::FactoryRadioChannel channel,
    const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active) {
        return;
    }
    m_session.ReportRadioVerified(channel, nowMs);
    RefreshStep();
}

void HardwareTestMode::RefreshStep() {
    const auto next = [this]() {
        const auto stage = GetStage();
        if (stage == modules::ManufacturingTestStage::Persisting) return Step::Persisting;
        if (stage == modules::ManufacturingTestStage::Passed) return Step::Passed;
        if (stage == modules::ManufacturingTestStage::OverrideSaved) return Step::PassedRadioBypassed;
        if (m_operatorStage < 4U && GetProgress().failedAutomatedCheckMask != 0U) return Step::LedRainbow;
        if (stage == modules::ManufacturingTestStage::Failed) return Step::Failed;
        if (m_operatorStage < 4U) return Step::LedRainbow;
        if (m_operatorStage == 4U) return Step::HallSensors;
        if (m_operatorStage == 5U) return Step::Buttons;
        const auto progress = GetProgress();
        const bool allContactsObserved = (progress.observedButtonMask & progress.expectedButtonMask) == progress.expectedButtonMask;
        if (!progress.hallComplete) return Step::HallSensors;
        // Expose Radio on initial entry only after the session admits it. Keep
        // the established view stable while a held control revokes Hall authority.
        return !allContactsObserved || (stage != modules::ManufacturingTestStage::Radio && m_step != Step::Radio)
            ? Step::Buttons : Step::Radio;
    }();
    if (next != m_step) {
        m_stepStartedAtMs = m_lastNowMs;
        if (next == Step::Buttons) {
            m_buttonExitReadyMask = 0U;
            m_operatorEligibleMask = 0U;
            m_session.ResetPendingLightResponses();
        }
    }
    m_step = next;
}

void HardwareTestMode::Tick(const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active) return;
    if (m_physicalButtonTicksAllowed) UpdateOverrideGesture(
        m_physicalButtonTickTimeValid ? m_physicalButtonTickMs : nowMs);
    if (!m_active) return;
    m_session.Tick(nowMs);
    if (m_verifierFeedback == VerifierFeedback::Success &&
        nowMs - m_verifierFeedbackStartedAtMs >= VERIFIER_SUCCESS_CUE_DURATION_MS) {
        m_verifierFeedback = VerifierFeedback::Waiting;
        m_verifierFeedbackStartedAtMs = nowMs;
    }
    RefreshStep();
}

void HardwareTestMode::SetVerifierBleProofActive(
    const bool active,
    const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active && !m_savedVerifierView) {
        return;
    }
    if (m_verifierFeedback == VerifierFeedback::Success &&
        static_cast<uint32_t>(nowMs - m_verifierFeedbackStartedAtMs) <
            VERIFIER_SUCCESS_CUE_DURATION_MS) {
        return;
    }
    const VerifierFeedback next = active
        ? VerifierFeedback::BleProof
        : VerifierFeedback::Waiting;
    if (m_verifierFeedback == next) {
        return;
    }
    m_verifierFeedback = next;
    m_verifierFeedbackStartedAtMs = nowMs;
}

void HardwareTestMode::ReportVerifierProofComplete(const uint32_t nowMs) {
    m_lastNowMs = nowMs;
    if (!m_active && !m_savedVerifierView) {
        return;
    }
    m_verifierFeedback = VerifierFeedback::Success;
    m_verifierFeedbackStartedAtMs = nowMs;
}

void HardwareTestMode::ReportPersistenceResult(const bool saved,
    const modules::ManufacturingTestRadioResult savedRadio) {
    m_session.ReportPersistenceResult(saved, savedRadio);
    RefreshStep();
}

void HardwareTestMode::ReportPeerVerifierUpgrade() {
    m_session.ReportPeerVerifierUpgrade();
    RefreshStep();
}

} // namespace firmware
