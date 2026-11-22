#pragma once

#include <cstdint>

namespace firmware {

enum class SleepState : uint8_t {
    Awake = 0,
    Countdown,
    Preparing,
    Sleeping,
    WakeGuard,
    Fading, // Append-only: existing observable state values stay stable.
};

enum class SleepRequestOrigin : uint8_t {
    Idle = 0,
    Menu,
    Console,
    App,
    None = 0xffU,
};

enum class SleepTransitionCause : uint8_t {
    None = 0,
    ButtonPress,
    BoardObservationChanged,
    ValidatedMotionOrTap,
    PreflightFailure,
    HigherPriorityState,
};

/**
 * \brief Pure, allocation-free owner of the sleep lifecycle.
 *
 * The caller supplies a uint32_t monotonic millisecond clock. Time comparisons
 * use modulo subtraction and therefore require the caller to sample the same
 * clock without a gap of 2^31 milliseconds or more. This is the usual
 * unsigned-clock wrap precondition and permits deadlines to cross UINT32_MAX.
 * The policy has no hardware, persistence, transport, or rendering behavior.
 */
class SleepTransition final {
  public:
    static constexpr uint32_t DEFAULT_COUNTDOWN_MS = 3000U;
    static constexpr uint32_t FADE_MS = 500U;
    static constexpr uint32_t WAKE_GUARD_MS = 200U;

    bool Request(SleepRequestOrigin origin, uint32_t nowMs,
                 bool allowActivityInterruption);
    void Update(uint32_t nowMs);

    // Handles local activity, preflight failure, and higher-priority takeover.
    // Non-interruptible entry ignores normal activity, not safety failures.
    // There is intentionally no generic transport/keepalive activity method.
    bool ShouldIgnoreActivity(SleepTransitionCause cause) const;
    bool Cancel(SleepTransitionCause cause);

    bool ReportPersistencePreflight(bool succeeded);
    bool ReportWakeSourcePreflight(bool succeeded);
    bool CommitSleep();
    bool ConsumeSleepAuthorization();

    // WakeGuard models the post-wake held-input gate; hardware sampling stays
    // with the caller. Awake resumes only after release plus WAKE_GUARD_MS.
    bool BeginWakeGuard(uint32_t nowMs);
    bool ReportWakeButtonsReleased();

    void Reset();

    SleepState GetState() const { return m_state; }
    SleepRequestOrigin GetOrigin() const { return m_origin; }
    SleepTransitionCause GetCancelCause() const { return m_cancelCause; }
    uint32_t GetRemainingCountdownMs() const { return m_remainingCountdownMs; }
    uint32_t GetCountdownDurationMs() const {
        return m_origin == SleepRequestOrigin::App ? 0U : DEFAULT_COUNTDOWN_MS;
    }
    uint16_t GetFadeScale256() const { return m_fadeScale256; }
    uint32_t GetCountdownStartedMs() const { return m_countdownStartedAtMs; }
    uint16_t GetRevision() const { return m_revision; }
    bool IsPersistencePreflightComplete() const {
        return m_persistencePreflightComplete;
    }
    bool IsWakeSourcePreflightComplete() const {
        return m_wakeSourcePreflightComplete;
    }
    bool HasPendingSleepAuthorization() const {
        return m_sleepAuthorizationPending;
    }

  private:
    static bool IsAcceptedOrigin(SleepRequestOrigin origin);
    static bool IsCancellationCause(SleepTransitionCause cause);
    void NoteObservableChange();

    SleepState m_state = SleepState::Awake;
    SleepRequestOrigin m_origin = SleepRequestOrigin::None;
    SleepTransitionCause m_cancelCause = SleepTransitionCause::None;
    bool m_allowActivityInterruption = true;
    uint32_t m_countdownStartedAtMs = 0U;
    uint32_t m_wakeGuardStartedAtMs = 0U;
    uint32_t m_remainingCountdownMs = 0U;
    uint16_t m_revision = 0U;
    uint16_t m_fadeScale256 = 256U;
    bool m_persistencePreflightComplete = false;
    bool m_wakeSourcePreflightComplete = false;
    bool m_sleepAuthorizationPending = false;
    bool m_wakeButtonsReleased = false;
};

} // namespace firmware
