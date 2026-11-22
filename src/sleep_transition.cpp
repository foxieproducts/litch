#include "firmware/sleep_transition.hpp"

namespace firmware {

bool SleepTransition::IsAcceptedOrigin(const SleepRequestOrigin origin) {
    switch (origin) {
        case SleepRequestOrigin::Idle:
        case SleepRequestOrigin::Menu:
        case SleepRequestOrigin::Console:
        case SleepRequestOrigin::App:
            return true;
        case SleepRequestOrigin::None:
        default:
            return false;
    }
}

bool SleepTransition::IsCancellationCause(const SleepTransitionCause cause) {
    switch (cause) {
        case SleepTransitionCause::ButtonPress:
        case SleepTransitionCause::BoardObservationChanged:
        case SleepTransitionCause::ValidatedMotionOrTap:
        case SleepTransitionCause::PreflightFailure:
        case SleepTransitionCause::HigherPriorityState:
            return true;
        case SleepTransitionCause::None:
        default:
            return false;
    }
}

void SleepTransition::NoteObservableChange() {
    m_revision = static_cast<uint16_t>(m_revision + 1U);
}

bool SleepTransition::Request(const SleepRequestOrigin origin,
                              const uint32_t nowMs,
                              const bool allowActivityInterruption) {
    if (!IsAcceptedOrigin(origin)) {
        return false;
    }

    switch (m_state) {
        case SleepState::Awake:
            m_allowActivityInterruption = allowActivityInterruption;
            m_state = origin == SleepRequestOrigin::App
                ? SleepState::Fading : SleepState::Countdown;
            m_origin = origin;
            m_cancelCause = SleepTransitionCause::None;
            m_countdownStartedAtMs = nowMs;
            m_remainingCountdownMs = GetCountdownDurationMs();
            m_fadeScale256 = 256U;
            m_persistencePreflightComplete = false;
            m_wakeSourcePreflightComplete = false;
            m_sleepAuthorizationPending = false;
            m_wakeButtonsReleased = false;
            NoteObservableChange();
            return true;
        case SleepState::Countdown:
        case SleepState::Fading:
        case SleepState::Preparing:
            if (!allowActivityInterruption &&
                (m_allowActivityInterruption ||
                 (origin == SleepRequestOrigin::App && m_origin != origin))) {
                if (origin == SleepRequestOrigin::App && m_origin != origin) {
                    if (m_state == SleepState::Countdown) {
                        m_state = SleepState::Fading;
                        m_countdownStartedAtMs = nowMs;
                        m_remainingCountdownMs = 0U;
                    } else if (m_state == SleepState::Fading) {
                        // Remove only the elapsed cue, preserving fade progress.
                        m_countdownStartedAtMs += GetCountdownDurationMs();
                    }
                }
                m_allowActivityInterruption = false;
                m_origin = origin;
                NoteObservableChange();
            }
            // Repeated requests preserve the active fade and preparation.
            return true;
        case SleepState::Sleeping:
        case SleepState::WakeGuard:
        default:
            return false;
    }
}

void SleepTransition::Update(const uint32_t nowMs) {
    switch (m_state) {
        case SleepState::Countdown:
        case SleepState::Fading: {
            const uint32_t elapsedMs =
                static_cast<uint32_t>(nowMs - m_countdownStartedAtMs);
            const uint32_t countdownMs = GetCountdownDurationMs();
            const SleepState state = elapsedMs < countdownMs
                ? SleepState::Countdown
                : elapsedMs < countdownMs + FADE_MS
                    ? SleepState::Fading : SleepState::Preparing;
            const uint32_t remainingMs = elapsedMs < countdownMs
                ? countdownMs - elapsedMs : 0U;
            const uint32_t fadeElapsedMs = elapsedMs < countdownMs
                ? 0U : elapsedMs - countdownMs;
            const uint16_t fadeScale256 = fadeElapsedMs >= FADE_MS ? 0U
                : static_cast<uint16_t>(
                    ((FADE_MS - fadeElapsedMs) * 256U) / FADE_MS);
            if (state != m_state || remainingMs != m_remainingCountdownMs ||
                fadeScale256 != m_fadeScale256) {
                m_state = state;
                m_remainingCountdownMs = remainingMs;
                m_fadeScale256 = fadeScale256;
                NoteObservableChange();
            }
            return;
        }
        case SleepState::WakeGuard:
            if (m_wakeButtonsReleased &&
                static_cast<uint32_t>(nowMs - m_wakeGuardStartedAtMs) >=
                    WAKE_GUARD_MS) {
                m_state = SleepState::Awake;
                m_persistencePreflightComplete = false;
                m_wakeSourcePreflightComplete = false;
                m_wakeButtonsReleased = false;
                NoteObservableChange();
            }
            return;
        case SleepState::Awake:
        case SleepState::Preparing:
        case SleepState::Sleeping:
        default:
            return;
    }
}

bool SleepTransition::ShouldIgnoreActivity(const SleepTransitionCause cause) const {
    const bool normal = cause == SleepTransitionCause::ValidatedMotionOrTap ||
                        cause == SleepTransitionCause::ButtonPress ||
                        cause == SleepTransitionCause::BoardObservationChanged;
    return !m_allowActivityInterruption && normal &&
           (m_state == SleepState::Countdown || m_state == SleepState::Fading ||
            m_state == SleepState::Preparing);
}

bool SleepTransition::Cancel(const SleepTransitionCause cause) {
    if (!IsCancellationCause(cause) || ShouldIgnoreActivity(cause)) {
        return false;
    }

    switch (m_state) {
        case SleepState::Countdown:
        case SleepState::Fading:
        case SleepState::Preparing:
            m_state = SleepState::Awake;
            m_cancelCause = cause;
            m_remainingCountdownMs = 0U;
            m_fadeScale256 = 256U;
            m_persistencePreflightComplete = false;
            m_wakeSourcePreflightComplete = false;
            m_sleepAuthorizationPending = false;
            NoteObservableChange();
            return true;
        case SleepState::Awake:
        case SleepState::Sleeping:
        case SleepState::WakeGuard:
        default:
            return false;
    }
}

bool SleepTransition::ReportPersistencePreflight(const bool succeeded) {
    if (m_state != SleepState::Preparing) {
        return false;
    }
    if (!succeeded) {
        Cancel(SleepTransitionCause::PreflightFailure);
        return false;
    }
    if (!m_persistencePreflightComplete) {
        m_persistencePreflightComplete = true;
        NoteObservableChange();
    }
    return true;
}

bool SleepTransition::ReportWakeSourcePreflight(const bool succeeded) {
    if (m_state != SleepState::Preparing) {
        return false;
    }
    if (!succeeded) {
        Cancel(SleepTransitionCause::PreflightFailure);
        return false;
    }
    if (!m_wakeSourcePreflightComplete) {
        m_wakeSourcePreflightComplete = true;
        NoteObservableChange();
    }
    return true;
}

bool SleepTransition::CommitSleep() {
    if (m_state != SleepState::Preparing ||
        !m_persistencePreflightComplete || !m_wakeSourcePreflightComplete) {
        return false;
    }

    m_state = SleepState::Sleeping;
    m_sleepAuthorizationPending = true;
    NoteObservableChange();
    return true;
}

bool SleepTransition::ConsumeSleepAuthorization() {
    if (!m_sleepAuthorizationPending) {
        return false;
    }

    m_sleepAuthorizationPending = false;
    NoteObservableChange();
    return true;
}

bool SleepTransition::BeginWakeGuard(const uint32_t nowMs) {
    if (m_state != SleepState::Sleeping) {
        return false;
    }

    m_state = SleepState::WakeGuard;
    m_wakeGuardStartedAtMs = nowMs;
    m_fadeScale256 = 256U;
    m_wakeButtonsReleased = false;
    m_persistencePreflightComplete = false;
    m_wakeSourcePreflightComplete = false;
    m_sleepAuthorizationPending = false;
    NoteObservableChange();
    return true;
}

bool SleepTransition::ReportWakeButtonsReleased() {
    if (m_state != SleepState::WakeGuard) {
        return false;
    }
    if (!m_wakeButtonsReleased) {
        m_wakeButtonsReleased = true;
        NoteObservableChange();
    }
    return true;
}

void SleepTransition::Reset() {
    if (m_state == SleepState::Awake &&
        m_origin == SleepRequestOrigin::None &&
        m_cancelCause == SleepTransitionCause::None &&
        m_remainingCountdownMs == 0U &&
        !m_persistencePreflightComplete && !m_wakeSourcePreflightComplete &&
        !m_sleepAuthorizationPending && !m_wakeButtonsReleased) {
        return;
    }

    m_state = SleepState::Awake;
    m_origin = SleepRequestOrigin::None;
    m_allowActivityInterruption = true;
    m_cancelCause = SleepTransitionCause::None;
    m_remainingCountdownMs = 0U;
    m_fadeScale256 = 256U;
    m_persistencePreflightComplete = false;
    m_wakeSourcePreflightComplete = false;
    m_sleepAuthorizationPending = false;
    m_wakeButtonsReleased = false;
    NoteObservableChange();
}

} // namespace firmware
