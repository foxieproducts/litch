#include <array>
#include <cstdint>
#include <cstdio>

#include "firmware/sleep_controller.hpp"
#include "firmware/sleep_entry_orchestration.hpp"
#include "foxie_embedded/millis.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

fxe::SettingsStore MakeSettings() {
    fxe::SettingsStoreConfig config;
    config.path = "build/test_sleep_controller_integration.json";
    return fxe::SettingsStore(config);
}

struct PreparationProbe {
    std::array<uint8_t, 4U> order{};
    uint8_t callCount = 0U;
    uint8_t failStep = 0U;
    uint8_t pendingPolls = 0U;

    bool Step(const uint8_t step) {
        if (callCount < order.size()) {
            order[callCount] = step;
        }
        ++callCount;
        return failStep != step;
    }

    static bool BeginPersistence(void* context) {
        return static_cast<PreparationProbe*>(context)->Step(1U);
    }
    static firmware::SleepPersistenceStatus PollPersistence(void* context) {
        auto& probe = *static_cast<PreparationProbe*>(context);
        if (!probe.Step(2U)) return firmware::SleepPersistenceStatus::Failed;
        if (probe.pendingPolls != 0U) {
            --probe.pendingPolls;
            return firmware::SleepPersistenceStatus::Pending;
        }
        return firmware::SleepPersistenceStatus::Succeeded;
    }
    static bool ButtonsReleased(void* context) {
        return static_cast<PreparationProbe*>(context)->Step(3U);
    }
    static bool PrepareWake(void* context) {
        return static_cast<PreparationProbe*>(context)->Step(4U);
    }
};

firmware::SleepPreparationHooks Hooks(PreparationProbe& probe) {
    return firmware::SleepPreparationHooks{
        &probe,
        &PreparationProbe::BeginPersistence,
        &PreparationProbe::PollPersistence,
        &PreparationProbe::ButtonsReleased,
        &PreparationProbe::PrepareWake,
    };
}

void ReachPreparing(firmware::SleepController& controller,
                    fxe::SettingsStore& settings,
                    const firmware::SleepRequestOrigin origin =
                        firmware::SleepRequestOrigin::Console,
                    const bool allowActivityInterruption = true) {
    Check(controller.RequestSleep(origin, 100U, allowActivityInterruption),
          "request enters controller");
    controller.Update(
        settings, 3600U, firmware::SleepSuppression::Allow);
    Check(controller.GetState() == firmware::SleepState::Preparing,
          "three-second countdown and half-second fade reach preparing");
}

void CheckTypedRequestsAndDeadline() {
    auto settings = MakeSettings();
    firmware::SleepController controller;

    Check(controller.RequestSleep(
              firmware::SleepRequestOrigin::Console, 1000U, true),
          "console request is accepted");
    Check(controller.GetState() == firmware::SleepState::Countdown,
          "console request enters countdown");
    Check(controller.GetRequestOrigin() ==
              firmware::SleepRequestOrigin::Console,
          "console origin is retained");
    Check(controller.RequestSleep(
              firmware::SleepRequestOrigin::Menu, 1001U, true),
          "repeat menu request is idempotently accepted");
    Check(controller.GetRequestOrigin() ==
              firmware::SleepRequestOrigin::Console,
          "repeat request does not replace first origin");
    Check(controller.RequestSleep(
              firmware::SleepRequestOrigin::App, 1002U, true),
          "duplicate app request retains the first console deadline");

    controller.Update(
        settings, 3999U, firmware::SleepSuppression::Allow);
    Check(controller.GetState() == firmware::SleepState::Countdown &&
              controller.GetRemainingCountdownMs() == 1U,
          "2999 ms remains cancellable with one millisecond left");
    controller.Update(
        settings, 4500U, firmware::SleepSuppression::Allow);
    Check(controller.GetState() == firmware::SleepState::Preparing,
          "countdown plus cooperative fade reaches preparing exactly");
}

void CheckManualActivityKeepsTheOriginalEntry() {
    auto settings = MakeSettings();
    for (const auto origin : {firmware::SleepRequestOrigin::App,
                              firmware::SleepRequestOrigin::Menu,
                              firmware::SleepRequestOrigin::Console}) {
        firmware::SleepController controller;
        fxe::ManualMillis::SetNowMs(100U);
        controller.Update(settings, 100U, firmware::SleepSuppression::Allow);
        const bool app = origin == firmware::SleepRequestOrigin::App;
        const uint32_t countdownMs = app ? 0U : 3000U;
        Check(controller.RequestSleep(origin, 100U, false) &&
                  controller.GetState() == (app ? firmware::SleepState::Fading
                                               : firmware::SleepState::Countdown) &&
                  controller.GetRemainingCountdownMs() == countdownMs &&
                  controller.GetCountdownDurationMs() == countdownMs,
              "app Sleep fades immediately; physical and console entry retain the count-up");
        PreparationProbe probe;
        Check(controller.RunSleepPreparation(Hooks(probe)) == firmware::SleepPreparationResult::NotPreparing &&
                  probe.callCount == 0U, "manual preparation cannot start before the fade");
        controller.Update(settings, 350U + countdownMs, firmware::SleepSuppression::Allow);
        Check(controller.GetState() == firmware::SleepState::Fading && controller.GetFadeScale256() == 128U &&
                  !controller.NoteValidatedMotionOrTapActivity() && controller.NoteButtonPressActivity(),
              "manual entry keeps the half-fade frame while ignoring normal activity");
        fxe::ManualMillis::SetNowMs(600U + countdownMs);
        controller.Update(settings, 600U + countdownMs, firmware::SleepSuppression::Allow);
        probe.pendingPolls = 2U;
        Check(controller.RunSleepPreparation(Hooks(probe)) == firmware::SleepPreparationResult::Pending,
              "manual save begins once");
        for (unsigned pending = 0U; pending < 2U; ++pending) {
            fxe::ManualMillis::AdvanceMs(200U);
            Check(!controller.NoteValidatedMotionOrTapActivity() &&
                      !controller.NoteHallSensorActivity() && controller.NoteButtonPressActivity() &&
                      controller.GetState() == firmware::SleepState::Preparing,
                  "false ignores normal activity and consumes buttons before gameplay dispatch");
            Check(controller.RequestSleep(origin, fxe::Millis(), false) &&
                      controller.RunSleepPreparation(Hooks(probe)) == firmware::SleepPreparationResult::Pending,
                  "repeated false does not restart pending persistence");
        }
        Check(probe.callCount == 3U && probe.order[0] == 1U && probe.order[1] == 2U && probe.order[2] == 2U,
              "only persistence polling repeats after ignored normal activity");
        Check(controller.RunSleepPreparation(Hooks(probe)) == firmware::SleepPreparationResult::Authorized &&
                  controller.ConsumeSleepAuthorization() && !controller.ConsumeSleepAuthorization(),
              "false uses the same verified one-shot sleep authorization");
    }
    for (const auto origin : {firmware::SleepRequestOrigin::App,
                              firmware::SleepRequestOrigin::Menu,
                              firmware::SleepRequestOrigin::Console}) {
        Check(!firmware::IsSleepSuppressedByHigherPriority(
                  {.otaHealthConfirmationPending = true}, origin),
              "directed Sleep waits for startup health through preparation instead of cancellation");
        Check(firmware::IsSleepSuppressedByHigherPriority(
                  {.otaTransfer = true, .otaHealthConfirmationPending = true}, origin) &&
                  firmware::IsSleepSuppressedByHigherPriority(
                      {.runtimeHealthPaused = true, .otaHealthConfirmationPending = true}, origin),
              "startup wait never bypasses active OTA or unsafe runtime health");
    }
    firmware::SleepController overlapping;
    overlapping.RequestSleep(firmware::SleepRequestOrigin::Idle, 0U, true);
    overlapping.Update(settings, 1500U, firmware::SleepSuppression::Allow);
    Check(overlapping.RequestSleep(firmware::SleepRequestOrigin::App, 1500U, false) &&
              overlapping.GetState() == firmware::SleepState::Fading &&
              overlapping.GetRemainingCountdownMs() == 0U &&
              overlapping.GetFadeScale256() == 256U &&
              overlapping.GetRequestOrigin() == firmware::SleepRequestOrigin::App,
          "app Sleep takes over the automatic cue with an immediate fade and owns diagnostics");
}

void CheckIdleUsesTheSameCountdown() {
    auto settings = MakeSettings();
    firmware::SleepController controller;

    fxe::ManualMillis::SetNowMs(0U);
    controller.Update(settings, 0U, firmware::SleepSuppression::Allow);
    fxe::ManualMillis::SetNowMs(5U * 60U * 1000U);
    controller.Update(
        settings,
        fxe::ManualMillis::NowMs(),
        firmware::SleepSuppression::Allow);
    Check(controller.GetState() == firmware::SleepState::Awake,
          "new factory default remains awake after five idle minutes");
    fxe::ManualMillis::SetNowMs(60U * 60U * 1000U);
    controller.Update(
        settings,
        fxe::ManualMillis::NowMs(),
        firmware::SleepSuppression::Allow);
    Check(controller.GetState() == firmware::SleepState::Countdown,
          "idle timeout enters countdown instead of direct preparation");
    Check(controller.GetRequestOrigin() ==
              firmware::SleepRequestOrigin::Idle,
          "idle timeout carries typed idle origin");
    Check(controller.GetRemainingCountdownMs() ==
              firmware::SleepTransition::DEFAULT_COUNTDOWN_MS,
          "idle gets the same 3000 ms countdown as console");

    auto persistedSettings = MakeSettings();
    persistedSettings.Doc()["SLEEP_TIMEOUT"] = "5m";
    persistedSettings.NotifyDocChanged();
    firmware::SleepController persistedController;
    fxe::ManualMillis::SetNowMs(0U);
    persistedController.Update(
        persistedSettings, 0U, firmware::SleepSuppression::Allow);
    fxe::ManualMillis::SetNowMs(5U * 60U * 1000U);
    persistedController.Update(
        persistedSettings,
        fxe::ManualMillis::NowMs(),
        firmware::SleepSuppression::Allow);
    Check(persistedController.GetState() == firmware::SleepState::Countdown,
          "persisted five-minute choice survives the factory-default change");
}

void CheckActivityAndPriorityCancellation() {
    auto settings = MakeSettings();
    Check(!firmware::IsSleepSuppressedByHigherPriority({}),
          "ordinary runtime does not suppress sleep");
    Check(firmware::IsSleepSuppressedByHigherPriority(
              {.otaTransfer = true}),
          "active OTA transfer suppresses sleep");
    Check(firmware::IsSleepSuppressedByHigherPriority(
              {.otaHealthConfirmationPending = true}),
          "pending post-boot OTA health confirmation suppresses sleep");
    Check(firmware::IsSleepSuppressedByHigherPriority(
              {.hardwareTest = true}),
          "hardware test suppresses sleep");
    Check(firmware::IsSleepSuppressedByHigherPriority(
              {.ampTest = true}),
          "amptest suppresses sleep");
    Check(firmware::IsSleepSuppressedByHigherPriority(
              {.ampTestVerify = true}),
          "amptest verification suppresses sleep");
    Check(firmware::IsSleepSuppressedByHigherPriority(
              {.recovery = true}),
          "recovery suppresses sleep");
    Check(firmware::IsSleepSuppressedByHigherPriority(
              {.runtimeHealthPaused = true}),
          "paused runtime health suppresses sleep");
    const std::array<firmware::SleepTransitionCause, 3U> causes = {
        firmware::SleepTransitionCause::ButtonPress,
        firmware::SleepTransitionCause::BoardObservationChanged,
        firmware::SleepTransitionCause::ValidatedMotionOrTap,
    };

    for (const auto cause : causes) {
        firmware::SleepController controller;
        controller.RequestSleep(firmware::SleepRequestOrigin::Idle, 0U, true);
        bool cancelled = false;
        switch (cause) {
            case firmware::SleepTransitionCause::ButtonPress:
                cancelled = controller.NoteButtonPressActivity();
                break;
            case firmware::SleepTransitionCause::BoardObservationChanged:
                cancelled = controller.NoteHallSensorActivity();
                break;
            case firmware::SleepTransitionCause::ValidatedMotionOrTap:
                cancelled = controller.NoteValidatedMotionOrTapActivity();
                break;
            default:
                break;
        }
        Check(cancelled, "wired local activity cancels countdown");
        Check(controller.GetState() == firmware::SleepState::Awake &&
                  controller.GetCancelCause() == cause,
              "local cancellation returns awake with stable cause");
    }

    firmware::SleepController transportIgnored;
    transportIgnored.RequestSleep(
        firmware::SleepRequestOrigin::Console, 0U, true);
    // BLE keepalives, console output, and transport traffic deliberately have
    // no activity hook. Advancing policy alone must not cancel them.
    transportIgnored.Update(
        settings, 2999U, firmware::SleepSuppression::Allow);
    Check(transportIgnored.GetState() == firmware::SleepState::Countdown,
          "transport activity is ignored by the local-activity contract");

    firmware::SleepController priority;
    priority.RequestSleep(firmware::SleepRequestOrigin::Console, 0U, true);
    priority.Update(
        settings, 1U, firmware::SleepSuppression::HigherPriority);
    Check(priority.GetState() == firmware::SleepState::Awake &&
              priority.GetCancelCause() ==
                  firmware::SleepTransitionCause::HigherPriorityState,
          "active OTA, unsafe runtime health and test modes cancel by priority");

    fxe::ManualMillis::SetNowMs(10U * 60U * 1000U);
    priority.Update(
        settings,
        fxe::ManualMillis::NowMs(),
        firmware::SleepSuppression::HigherPriority);
    fxe::ManualMillis::AdvanceMs(1U);
    priority.Update(
        settings,
        fxe::ManualMillis::NowMs(),
        firmware::SleepSuppression::Allow);
    Check(priority.GetState() == firmware::SleepState::Awake,
          "priority suppression resets idle time instead of re-sleeping immediately");
}

firmware::SleepPreparationFailure FailureForStep(const uint8_t step) {
    switch (step) {
        case 1U:
            return firmware::SleepPreparationFailure::GamePersistence;
        case 2U:
            return firmware::SleepPreparationFailure::SettingsPersistence;
        case 3U:
            return firmware::SleepPreparationFailure::WakeButtonsHeld;
        case 4U:
            return firmware::SleepPreparationFailure::WakeSourceUnavailable;
        default:
            return firmware::SleepPreparationFailure::PolicyRejected;
    }
}

void CheckPreflightOrderingAndAuthorization() {
    auto settings = MakeSettings();

    for (const bool allowActivityInterruption : {true, false}) {
      for (uint8_t failStep = 1U; failStep <= 4U; ++failStep) {
        firmware::SleepController failed;
        ReachPreparing(failed, settings, firmware::SleepRequestOrigin::Console, allowActivityInterruption);
        PreparationProbe probe;
        probe.failStep = failStep;
        const firmware::SleepPreparationResult beginResult =
            failed.RunSleepPreparation(Hooks(probe));
        const firmware::SleepPreparationResult result =
            beginResult == firmware::SleepPreparationResult::Pending
                ? failed.RunSleepPreparation(Hooks(probe))
                : beginResult;
        Check(result == firmware::SleepPreparationResult::Failed,
              "failed preflight refuses authorization");
        Check(probe.callCount == failStep,
              "preflight failure stops all later steps");
        for (uint8_t i = 0U; i < probe.callCount; ++i) {
            Check(probe.order[i] == static_cast<uint8_t>(i + 1U),
                  "preflights run game, settings, buttons, wake in order");
        }
        Check(failed.GetState() == firmware::SleepState::Awake,
              "preflight failure returns controller to awake");
        Check(failed.GetLastPreparationFailure() == FailureForStep(failStep),
              "preflight failure reason remains stable");
        Check(!failed.ConsumeSleepAuthorization(),
              "failed preflight emits no teardown authorization");
      }
    }

    firmware::SleepController success;
    ReachPreparing(success, settings);
    PreparationProbe probe;
    Check(success.RunSleepPreparation(Hooks(probe)) ==
              firmware::SleepPreparationResult::Pending,
          "persistence starts once and yields to the cooperative loop");
    Check(success.RunSleepPreparation(Hooks(probe)) ==
              firmware::SleepPreparationResult::Authorized,
          "all preflights authorize sleep");
    Check(probe.order == std::array<uint8_t, 4U>{1U, 2U, 3U, 4U},
          "successful preflights preserve exact ordering");
    Check(success.GetState() == firmware::SleepState::Sleeping,
          "authorization transitions to sleeping");
    Check(success.ConsumeSleepAuthorization(),
          "teardown authorization is consumable once");
    Check(!success.ConsumeSleepAuthorization(),
          "teardown authorization cannot be consumed twice");

    firmware::SleepController cancellable;
    ReachPreparing(cancellable, settings);
    PreparationProbe cancellableProbe;
    Check(cancellable.RunSleepPreparation(Hooks(cancellableProbe)) ==
              firmware::SleepPreparationResult::Pending,
          "preparing remains pending while persistence is in flight");
    Check(cancellable.NoteButtonPressActivity(),
          "button Press cancels while persistence is in flight");
    Check(cancellable.GetState() == firmware::SleepState::Awake,
          "in-flight persistence cancellation returns awake");
    Check(cancellableProbe.callCount == 1U,
          "cancellation prevents durability poll and wake preflights");
    Check(!cancellable.ConsumeSleepAuthorization(),
          "cancelled in-flight persistence never authorizes teardown");
}

struct LifecycleProbe {
    std::array<uint8_t, 11U> order{};
    std::array<uint32_t, 2U> waits{};
    uint8_t waitCount = 0U;
    uint8_t callCount = 0U;
    bool wakeSourceAvailable = true;
    bool enterSleepResult = false;
    bool restartRequested = false;

    void Note(const uint8_t step) {
        if (callCount < order.size()) {
            order[callCount] = step;
        }
        ++callCount;
    }

    static bool BeginPersistence(void* context) {
        static_cast<LifecycleProbe*>(context)->Note(1U);
        return true;
    }
    static firmware::SleepPersistenceStatus PollPersistence(void* context) {
        static_cast<LifecycleProbe*>(context)->Note(2U);
        return firmware::SleepPersistenceStatus::Succeeded;
    }
    static bool ButtonsReleased(void* context) {
        static_cast<LifecycleProbe*>(context)->Note(3U);
        return true;
    }
    static bool PrepareWake(void* context) {
        auto& probe = *static_cast<LifecycleProbe*>(context);
        probe.Note(4U);
        return probe.wakeSourceAvailable;
    }
    static bool ClearLeds(void* context) {
        static_cast<LifecycleProbe*>(context)->Note(5U);
        return true;
    }
    static void Wait(void* context, const uint32_t milliseconds) {
        auto& probe = *static_cast<LifecycleProbe*>(context);
        if (probe.waitCount < probe.waits.size()) {
            probe.waits[probe.waitCount] = milliseconds;
        }
        ++probe.waitCount;
        probe.Note(milliseconds == firmware::SLEEP_LED_OFF_HOLD_MS ? 6U : 9U);
    }
    static void ShutdownAmbient(void* context) {
        static_cast<LifecycleProbe*>(context)->Note(7U);
    }
    static void PreparePowerRail(void* context) {
        static_cast<LifecycleProbe*>(context)->Note(8U);
    }
    static bool EnterSleep(void* context) {
        auto& probe = *static_cast<LifecycleProbe*>(context);
        probe.Note(10U);
        return probe.enterSleepResult;
    }
    static void Restart(void* context, bool) {
        auto& probe = *static_cast<LifecycleProbe*>(context);
        probe.Note(11U);
        probe.restartRequested = true;
    }
};

firmware::SleepPreparationHooks PreparationHooks(LifecycleProbe& probe) {
    return firmware::SleepPreparationHooks{
        &probe,
        &LifecycleProbe::BeginPersistence,
        &LifecycleProbe::PollPersistence,
        &LifecycleProbe::ButtonsReleased,
        &LifecycleProbe::PrepareWake,
    };
}

firmware::SleepEntryHooks EntryHooks(LifecycleProbe& probe) {
    return firmware::SleepEntryHooks{
        &probe,
        &LifecycleProbe::ClearLeds,
        &LifecycleProbe::Wait,
        &LifecycleProbe::ShutdownAmbient,
        &LifecycleProbe::PreparePowerRail,
        &LifecycleProbe::EnterSleep,
        &LifecycleProbe::Restart,
    };
}

void CheckSleepLifecycleOrchestration() {
    auto settings = MakeSettings();

    for (const bool enterSleepResult : {false, true}) {
        firmware::SleepController controller;
        ReachPreparing(controller, settings);
        LifecycleProbe probe;
        probe.enterSleepResult = enterSleepResult;

        Check(firmware::RunSleepLifecycleStep(
                  controller, PreparationHooks(probe), EntryHooks(probe)) ==
                  firmware::SleepLifecycleResult::Pending,
              "sleep lifecycle yields while persistence begins");
        Check(probe.callCount == 1U,
              "pending persistence performs no wake or teardown work");
        Check(firmware::RunSleepLifecycleStep(
                  controller, PreparationHooks(probe), EntryHooks(probe)) ==
                  firmware::SleepLifecycleResult::RestartReturned,
              "returned sleep and restart hooks produce a terminal result");
        Check(probe.order ==
                  std::array<uint8_t, 11U>{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U},
              "wake setup precedes fade-to-black, 50 ms hold, rail-off, 100 ms hold, sleep, and restart work");
        Check(probe.waits == std::array<uint32_t, 2U>{50U, 100U},
              "sleep teardown holds black for 50 ms and the disabled rail for 100 ms");
        Check(probe.restartRequested,
              "every returned sleep entry requests a restart");
    }

    firmware::SleepController failed;
    ReachPreparing(failed, settings);
    LifecycleProbe failedProbe;
    failedProbe.wakeSourceAvailable = false;
    Check(firmware::RunSleepLifecycleStep(
              failed, PreparationHooks(failedProbe), EntryHooks(failedProbe)) ==
              firmware::SleepLifecycleResult::Pending,
          "failed-wake lifecycle still begins with persistence");
    Check(firmware::RunSleepLifecycleStep(
              failed, PreparationHooks(failedProbe), EntryHooks(failedProbe)) ==
              firmware::SleepLifecycleResult::PreparationFailed,
          "failed wake-source setup aborts the lifecycle");
    Check(failedProbe.callCount == 4U,
          "failed wake-source setup performs no teardown work");
    Check(!failedProbe.restartRequested,
          "pre-teardown failure does not request a restart");
}

} // namespace

int main() {
    std::puts("[SLEEP_CONTROLLER_INTEGRATION] start");
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);

    CheckTypedRequestsAndDeadline();
    CheckManualActivityKeepsTheOriginalEntry();
    CheckIdleUsesTheSameCountdown();
    CheckActivityAndPriorityCancellation();
    CheckPreflightOrderingAndAuthorization();
    CheckSleepLifecycleOrchestration();

    if (g_failures == 0) {
        std::puts("[SLEEP_CONTROLLER_INTEGRATION] ok");
        return 0;
    }
    std::printf("[SLEEP_CONTROLLER_INTEGRATION] failures=%d\n", g_failures);
    return 1;
}
