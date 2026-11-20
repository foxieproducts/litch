#include "firmware/sleep_controller.hpp"

#include <cstring>

#include "logger.hpp"

namespace firmware {

namespace {

bool TryParseSleepTimeoutTokenMs(const char* token, uint32_t& outMs) {
    if (token == nullptr || token[0] == '\0') {
        return false;
    }

    if (std::strcmp(token, "5m") == 0) {
        outMs = 5U * 60U * 1000U;
        return true;
    }
    if (std::strcmp(token, "30m") == 0) {
        outMs = 30U * 60U * 1000U;
        return true;
    }
    if (std::strcmp(token, "60m") == 0) {
        outMs = 60U * 60U * 1000U;
        return true;
    }
    if (std::strcmp(token, "disabled") == 0) {
        outMs = 0U;
        return true;
    }

    return false;
}

} // namespace

uint32_t SleepController::ReadSleepTimeoutMs(const fxe::SettingsStore& settings) {
    uint32_t timeoutMs = 0;
    const char* token = settings.ReadValue("SLEEP_TIMEOUT", "60m");
    if (TryParseSleepTimeoutTokenMs(token, timeoutMs)) {
        return timeoutMs;
    }

    logger::Warn("sleep: invalid SLEEP_TIMEOUT token '%s'; using default 60m",
                 (token != nullptr) ? token : "<null>");
    return 60U * 60U * 1000U;
}

bool SleepController::NoteLocalActivity(const SleepTransitionCause cause) {
    if (m_transition.ShouldIgnoreActivity(cause)) {
        return false;
    }
    m_idleActivityElapsed.Reset();
    const bool cancelled = m_transition.Cancel(cause);
    if (cancelled) {
        m_persistenceStarted = false;
        m_wakeAfterMs = 0U;
    }
    return cancelled;
}

bool SleepController::NoteButtonPressActivity() {
    if (m_transition.ShouldIgnoreActivity(SleepTransitionCause::ButtonPress)) {
        return true;
    }
    return NoteLocalActivity(SleepTransitionCause::ButtonPress);
}

bool SleepController::NoteHallSensorActivity() {
    return NoteLocalActivity(SleepTransitionCause::BoardObservationChanged);
}

bool SleepController::NoteValidatedMotionOrTapActivity() {
    return NoteLocalActivity(SleepTransitionCause::ValidatedMotionOrTap);
}

bool SleepController::NoteHigherPriorityState() {
    m_idleActivityElapsed.Reset();
    const bool cancelled =
        m_transition.Cancel(SleepTransitionCause::HigherPriorityState);
    if (cancelled) {
        m_persistenceStarted = false;
        m_wakeAfterMs = 0U;
    }
    return cancelled;
}

bool SleepController::RequestSleep(const SleepRequestOrigin origin,
                                   const uint32_t nowMs,
                                   const bool allowActivityInterruption,
                                   const uint32_t wakeAfterMs) {
    const SleepState before = m_transition.GetState();
    if (!IsValidWakeAfterMs(wakeAfterMs) ||
        (wakeAfterMs != 0U && !m_hardwareProfile.isS3) ||
        (before != SleepState::Awake && wakeAfterMs != 0U &&
         wakeAfterMs != m_wakeAfterMs)) {
        return false;
    }
    const bool accepted = m_transition.Request(origin, nowMs, allowActivityInterruption);
    if (accepted && before == SleepState::Awake) {
        m_persistenceStarted = false;
        m_lastPreparationFailure = SleepPreparationFailure::None;
        m_wakeAfterMs = wakeAfterMs;
    }
    return accepted;
}

void SleepController::UpdateAutoSleepRequest(
    const fxe::SettingsStore& settings,
    const uint32_t nowMs) {
    if (settings.IsDisabled() || !m_idleActivityElapsed.Started()) {
        m_idleActivityElapsed.Reset();
        return;
    }

    if (m_transition.GetState() != SleepState::Awake) {
        return;
    }

    const uint32_t timeoutMs = ReadSleepTimeoutMs(settings);
    if (timeoutMs == 0U) {
        return;
    }

    if (!m_idleActivityElapsed.HasElapsed(timeoutMs)) {
        return;
    }

    logger::Info("sleep: idle timeout elapsed (%lu ms); requesting sleep",
                 static_cast<unsigned long>(timeoutMs));
    (void)RequestSleep(SleepRequestOrigin::Idle, nowMs, true);
    m_idleActivityElapsed.Reset();
}

void SleepController::Update(const fxe::SettingsStore& settings,
                             const uint32_t nowMs,
                             const SleepSuppression suppression) {
    if (suppression == SleepSuppression::HigherPriority) {
        (void)NoteHigherPriorityState();
        return;
    }

    if (settings.IsDisabled() &&
        m_transition.GetOrigin() == SleepRequestOrigin::Idle &&
        m_transition.GetState() != SleepState::Awake) {
        (void)NoteHigherPriorityState();
        return;
    }
    UpdateAutoSleepRequest(settings, nowMs);
    m_transition.Update(nowMs);
}

SleepPreparationResult SleepController::FailPreparation(
    const SleepPreparationFailure failure,
    const SleepPreflightStage stage) {
    m_lastPreparationFailure = failure;
    if (stage == SleepPreflightStage::Persistence) {
        (void)m_transition.ReportPersistencePreflight(false);
    } else {
        (void)m_transition.ReportWakeSourcePreflight(false);
    }
    m_idleActivityElapsed.Reset();
    m_persistenceStarted = false;
    m_wakeAfterMs = 0U;
    return SleepPreparationResult::Failed;
}

SleepPreparationResult SleepController::RunSleepPreparation(
    const SleepPreparationHooks& hooks) {
    if (m_transition.GetState() != SleepState::Preparing) {
        m_persistenceStarted = false;
        return SleepPreparationResult::NotPreparing;
    }

    if (!m_persistenceStarted) {
        if (hooks.beginPersistence == nullptr ||
            !hooks.beginPersistence(hooks.context)) {
            return FailPreparation(
                SleepPreparationFailure::GamePersistence,
                SleepPreflightStage::Persistence);
        }
        m_persistenceStarted = true;
        return SleepPreparationResult::Pending;
    }

    if (hooks.pollPersistence == nullptr) {
        return FailPreparation(
            SleepPreparationFailure::SettingsPersistence,
            SleepPreflightStage::Persistence);
    }
    const SleepPersistenceStatus persistenceStatus =
        hooks.pollPersistence(hooks.context);
    if (persistenceStatus == SleepPersistenceStatus::Pending) {
        return SleepPreparationResult::Pending;
    }
    if (persistenceStatus == SleepPersistenceStatus::Failed) {
        return FailPreparation(
            SleepPreparationFailure::SettingsPersistence,
            SleepPreflightStage::Persistence);
    }
    if (!m_transition.ReportPersistencePreflight(true)) {
        return FailPreparation(
            SleepPreparationFailure::PolicyRejected,
            SleepPreflightStage::Persistence);
    }

    if (hooks.wakeButtonsReleased == nullptr ||
        !hooks.wakeButtonsReleased(hooks.context)) {
        return FailPreparation(
            SleepPreparationFailure::WakeButtonsHeld,
            SleepPreflightStage::WakeSource);
    }
    if (hooks.prepareWakeSource == nullptr ||
        !hooks.prepareWakeSource(hooks.context)) {
        return FailPreparation(
            SleepPreparationFailure::WakeSourceUnavailable,
            SleepPreflightStage::WakeSource);
    }
    if (!m_transition.ReportWakeSourcePreflight(true) ||
        !m_transition.CommitSleep()) {
        return FailPreparation(
            SleepPreparationFailure::PolicyRejected,
            SleepPreflightStage::WakeSource);
    }

    m_lastPreparationFailure = SleepPreparationFailure::None;
    m_persistenceStarted = false;
    return SleepPreparationResult::Authorized;
}

} // namespace firmware
