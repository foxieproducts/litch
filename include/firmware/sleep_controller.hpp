#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "firmware/sleep_transition.hpp"
#include "firmware/sleep_wake_pin_policy.hpp"
#include "hardware/board_hardware_profile.hpp"

namespace firmware {

enum class BootKind : uint8_t {
    ColdBoot = 0,
    DeepSleepWake = 1,
};

enum class SleepPreparationFailure : uint8_t {
    None = 0,
    GamePersistence,
    SettingsPersistence,
    WakeButtonsHeld,
    WakeSourceUnavailable,
    PolicyRejected,
};

enum class SleepPreparationResult : uint8_t {
    NotPreparing = 0,
    Pending,
    Authorized,
    Failed,
};

enum class SleepPersistenceStatus : uint8_t {
    Pending = 0,
    Succeeded,
    Failed,
};

enum class SleepSuppression : uint8_t {
    Allow = 0,
    HigherPriority,
};

struct SleepPreparationHooks {
    using StepFn = bool (*)(void* context);
    using PersistenceStatusFn = SleepPersistenceStatus (*)(void* context);

    void* context = nullptr;
    StepFn beginPersistence = nullptr;
    PersistenceStatusFn pollPersistence = nullptr;
    StepFn wakeButtonsReleased = nullptr;
    StepFn prepareWakeSource = nullptr;
};

struct SleepHigherPriorityState {
    bool recovery = false;
    bool runtimeHealthPaused = false;
    bool otaTransfer = false;
    bool otaHealthConfirmationPending = false;
    bool hardwareTest = false;
    bool ampTest = false;
    bool ampTestVerify = false;
};

constexpr bool IsSleepSuppressedByHigherPriority(
    const SleepHigherPriorityState& state,
    const SleepRequestOrigin pendingOrigin = SleepRequestOrigin::None) {
    const bool directed = pendingOrigin == SleepRequestOrigin::App ||
                          pendingOrigin == SleepRequestOrigin::Menu ||
                          pendingOrigin == SleepRequestOrigin::Console;
    // A directed request waits at the bounded persistence preflight for boot
    // confirmation. Automatic entry stays suppressed until that owner finishes.
    return state.recovery || state.runtimeHealthPaused || state.otaTransfer ||
           (state.otaHealthConfirmationPending && !directed) || state.hardwareTest ||
           state.ampTest || state.ampTestVerify;
}

constexpr bool IsSleepEntrySafeToTearDown(const bool wakeButtonsReleased,
                                          const bool wakeSourceConfigured) {
    return wakeButtonsReleased && wakeSourceConfigured;
}

constexpr bool ShouldRestoreRtcWakePinsForRuntime(
    const bool wokeFromDeepSleepReset,
    const bool wokeFromExt1) {
    return wokeFromDeepSleepReset || wokeFromExt1;
}

class SleepController final {
  public:
    // False means teardown is incomplete; entry must use restart recovery.
    using PreSleepCallback = bool (*)(void* context);
    using PrepareOptionalWakeSourceFn = bool (*)(void* context);
    using IsOptionalWakeSourceArmedFn = bool (*)(void* context);

    void ConfigureHardwareProfile(
        const hardware::BoardHardwareProfile& profile) {
        m_hardwareProfile = profile;
        RefreshButtonWakePins();
        m_activeWakePins = m_buttonWakePins;
        m_runtimeButtonPins = BuildRuntimeButtonPinSet(profile);
        m_restoreWakePins = BuildDeepSleepWakePinSet(profile, true, true);
    }

    bool IsP1XDeepWakeEnabled() const {
        return m_hardwareProfile.isS3;
    }

    uint64_t GetConfiguredButtonWakeMask() const {
        return m_buttonWakePins.Mask();
    }

    void ConfigureOptionalActiveLowWakeSource(
        const uint8_t pin,
        PrepareOptionalWakeSourceFn prepare,
        IsOptionalWakeSourceArmedFn isArmed,
        void* context) {
        m_optionalWakePin = pin;
        m_prepareOptionalWakeSource = prepare;
        m_isOptionalWakeSourceArmed = isArmed;
        m_optionalWakeSourceContext = context;
    }

    void Begin();

    BootKind GetBootKind() const { return m_bootKind; }
    bool IsDeepSleepWake() const { return m_bootKind == BootKind::DeepSleepWake; }
    bool WokeFromTimer() const { return m_wokeFromTimer; }

    static constexpr uint32_t MIN_WAKE_AFTER_MS = 100U;
    static constexpr uint32_t MAX_WAKE_AFTER_MS = 60000U;
    static constexpr bool IsValidWakeAfterMs(const uint32_t wakeAfterMs) {
        return wakeAfterMs == 0U ||
               (wakeAfterMs >= MIN_WAKE_AFTER_MS && wakeAfterMs <= MAX_WAKE_AFTER_MS);
    }

    // Returns true only when the hardware wake source was accepted.
    bool ConfigureWakeOnButtons();

    // Configures wake-button inputs and returns false while any is held.
    bool CanBeginSleepTeardown();

    // Installs and verifies the best available wake source. Call only after
    // persistence work, but before powering down runtime peripherals.
    bool PrepareWakeSourceForSleep();

    void UpdateIgnoreGate();
    bool ShouldIgnoreButtonEvents() const { return m_ignoreButtonsActive; }

    // The only sleep-request ingress. Every accepted origin enters the same
    // SleepTransition. App fades immediately; other origins use the cue then fade.
    // False ignores normal activity cancellation.
    // A nonzero wakeAfterMs adds a one-shot S3 timer to a fresh request only.
    // A repeated nonzero interval must match the retained request.
    bool RequestSleep(SleepRequestOrigin origin, uint32_t nowMs,
                      bool allowActivityInterruption, uint32_t wakeAfterMs = 0U);

    // Advances idle-request and transition policy. A higher-priority runtime
    // state cancels a pending transition and keeps the idle timer suppressed.
    void Update(const fxe::SettingsStore& settings,
                uint32_t nowMs,
                SleepSuppression suppression);

    // User-activity hooks used by idle auto-sleep.
    // A true button return consumes a cancellation or an ignored Press, so it
    // cannot also dispatch a gameplay/control action.
    bool NoteButtonPressActivity();
    bool NoteHallSensorActivity();
    bool NoteValidatedMotionOrTapActivity();
    bool NoteHigherPriorityState();
    bool ShouldIgnoreActivity(SleepTransitionCause cause) const {
        return m_transition.ShouldIgnoreActivity(cause);
    }

    SleepState GetState() const { return m_transition.GetState(); }
    SleepRequestOrigin GetRequestOrigin() const {
        return m_transition.GetOrigin();
    }
    SleepTransitionCause GetCancelCause() const {
        return m_transition.GetCancelCause();
    }
    uint32_t GetRemainingCountdownMs() const {
        return m_transition.GetRemainingCountdownMs();
    }
    uint32_t GetCountdownDurationMs() const {
        return m_transition.GetCountdownDurationMs();
    }
    uint32_t GetCountdownStartedMs() const {
        return m_transition.GetCountdownStartedMs();
    }
    uint16_t GetFadeScale256() const {
        return m_transition.GetFadeScale256();
    }
    SleepPreparationFailure GetLastPreparationFailure() const {
        return m_lastPreparationFailure;
    }

    // Runs persistence, released-button, and wake-source preflights in that
    // exact order. This does not tear down hardware; it only asks the policy
    // for one sleep authorization after every preflight succeeds.
    SleepPreparationResult RunSleepPreparation(
        const SleepPreparationHooks& hooks);
    bool ConsumeSleepAuthorization() {
        return m_transition.ConsumeSleepAuthorization();
    }

    void SetPreSleepCallback(PreSleepCallback callback, void* context) {
        m_preSleepCallback = callback;
        m_preSleepCallbackContext = context;
    }

    // Enters the lowest-power sleep mode available while still allowing
    // wake-on-buttons for the current hardware. A successful entry does not
    // return; false means sleep was not entered and the caller must recover.
    //
    // On targets/pinmaps where deep-sleep GPIO wake is not possible (ESP32-C3
    // prototype wiring), this falls back to light sleep.
    [[nodiscard]] bool EnterSleepNow();

  private:
    enum class SleepPreflightStage : uint8_t {
        Persistence = 0,
        WakeSource,
    };

    bool AnyWakeButtonsDown() const;
    bool AnyActiveWakeSourcesDown() const;
    void RestoreWakeButtonsForRuntimeOrRestart();
    void RestoreWakeButtonsAfterFailedPreparation();
    static uint32_t ReadSleepTimeoutMs(const fxe::SettingsStore& settings);
    void UpdateAutoSleepRequest(const fxe::SettingsStore& settings,
                                uint32_t nowMs);
    bool NoteLocalActivity(SleepTransitionCause cause);
    SleepPreparationResult FailPreparation(
        SleepPreparationFailure failure,
        SleepPreflightStage stage);
    void RefreshButtonWakePins() {
        m_buttonWakePins = BuildDeepSleepWakePinSet(
            m_hardwareProfile, false, false);
    }

    void MarkWokeFromSleep();

    BootKind m_bootKind = BootKind::ColdBoot;
    bool m_wakeSourcePrepared = false;
    bool m_preparedForDeepSleep = false;
    bool m_wokeFromTimer = false;
    uint32_t m_wakeAfterMs = 0U;

    PreSleepCallback m_preSleepCallback = nullptr;
    void* m_preSleepCallbackContext = nullptr;

    hardware::BoardHardwareProfile m_hardwareProfile =
        hardware::BoardHardwareProfileFor(hardware::BoardRevision::Unknown);
    SleepWakePinSet m_buttonWakePins =
        BuildDeepSleepWakePinSet(m_hardwareProfile, false);
    SleepWakePinSet m_activeWakePins = m_buttonWakePins;
    SleepWakePinSet m_runtimeButtonPins =
        BuildRuntimeButtonPinSet(m_hardwareProfile);
    SleepWakePinSet m_restoreWakePins =
        BuildDeepSleepWakePinSet(m_hardwareProfile, true, true);
    uint8_t m_optionalWakePin = hardware::NO_GPIO;
    PrepareOptionalWakeSourceFn m_prepareOptionalWakeSource = nullptr;
    IsOptionalWakeSourceArmedFn m_isOptionalWakeSourceArmed = nullptr;
    void* m_optionalWakeSourceContext = nullptr;

    bool m_ignoreButtonsActive = false;
    fxe::ElapsedTime m_allReleasedElapsed;
    fxe::ElapsedTime m_idleActivityElapsed;
    SleepTransition m_transition;
    bool m_persistenceStarted = false;
    SleepPreparationFailure m_lastPreparationFailure =
        SleepPreparationFailure::None;
};

} // namespace firmware
