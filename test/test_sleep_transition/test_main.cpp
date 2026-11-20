#include <cstdint>
#include <initializer_list>
#include <cstdio>

#include "firmware/sleep_transition.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }

    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

void CheckDefaultState() {
    firmware::SleepTransition transition;

    Check(transition.GetState() == firmware::SleepState::Awake,
          "default state is awake");
    Check(transition.GetOrigin() == firmware::SleepRequestOrigin::None,
          "default origin is none");
    Check(transition.GetCancelCause() == firmware::SleepTransitionCause::None,
          "default cancel cause is none");
    Check(transition.GetRemainingCountdownMs() == 0U,
          "awake state has no remaining countdown");
    Check(!transition.IsPersistencePreflightComplete(),
          "persistence preflight starts incomplete");
    Check(!transition.IsWakeSourcePreflightComplete(),
          "wake-source preflight starts incomplete");
    Check(!transition.HasPendingSleepAuthorization(),
          "default state has no authorization");
}

void CheckOriginsAndStateRejections() {
    const firmware::SleepRequestOrigin origins[] = {
        firmware::SleepRequestOrigin::Idle,
        firmware::SleepRequestOrigin::Menu,
        firmware::SleepRequestOrigin::Console,
    };

    for (const auto origin : origins) {
        firmware::SleepTransition transition;
        Check(transition.Request(origin, 100U, true), "public origin is accepted");
        Check(transition.GetState() == firmware::SleepState::Countdown,
              "accepted origin enters countdown");
        Check(transition.GetOrigin() == origin,
              "accepted origin remains observable");
        Check(transition.GetRemainingCountdownMs() ==
                  firmware::SleepTransition::DEFAULT_COUNTDOWN_MS,
              "accepted origin gets the default countdown");
        Check(transition.Request(firmware::SleepRequestOrigin::App, 100U, true),
              "duplicate app request preserves the first origin and deadline");
        Check(!transition.Request(static_cast<firmware::SleepRequestOrigin>(99U),
                                  100U, true),
              "invalid origin is rejected");
        Check(transition.GetOrigin() == origin,
              "rejected origin does not replace accepted origin");
    }

    firmware::SleepTransition transition;
    Check(!transition.Request(firmware::SleepRequestOrigin::None, 0U, true),
          "none origin is rejected");
    Check(!transition.ReportPersistencePreflight(true),
          "persistence result is rejected before preparing");
    Check(!transition.ReportWakeSourcePreflight(true),
          "wake-source result is rejected before preparing");
    Check(!transition.CommitSleep(),
          "commit is rejected before preparing");
    Check(!transition.BeginWakeGuard(0U),
          "wake guard is rejected before sleeping");
}

void CheckDeadlineAndIdempotence() {
    firmware::SleepTransition transition;
    Check(transition.Request(firmware::SleepRequestOrigin::Menu, 1000U, true),
          "menu request starts countdown");
    const uint16_t revisionAfterRequest = transition.GetRevision();
    Check(transition.Request(firmware::SleepRequestOrigin::Console, 1001U, true),
          "repeated request is idempotently accepted");
    Check(transition.GetOrigin() == firmware::SleepRequestOrigin::Menu,
          "first request origin wins during countdown");
    Check(transition.GetRevision() == revisionAfterRequest,
          "repeated request does not change revision");

    transition.Update(3999U);
    Check(transition.GetState() == firmware::SleepState::Countdown,
          "2999 ms remains cancellable countdown");
    Check(transition.GetRemainingCountdownMs() == 1U,
          "2999 ms leaves one millisecond");

    transition.Update(4000U);
    Check(transition.GetState() == firmware::SleepState::Fading &&
              transition.GetFadeScale256() == 256U,
          "exact countdown deadline starts the full-brightness fade");
    transition.Update(4500U);
    Check(transition.GetState() == firmware::SleepState::Preparing &&
              transition.GetFadeScale256() == 0U,
          "fade completes before preparation");
    Check(transition.GetRemainingCountdownMs() == 0U,
          "preparing has no remaining countdown");
    const uint16_t revisionAtDeadline = transition.GetRevision();
    Check(transition.Request(firmware::SleepRequestOrigin::Idle, 5000U, true),
          "repeated request is idempotently accepted while preparing");
    Check(transition.GetRevision() == revisionAtDeadline,
          "preparing request does not change revision or deadline");
    transition.Update(4501U);
    Check(transition.GetState() == firmware::SleepState::Preparing,
          "after deadline remains preparing");
    Check(transition.GetRevision() == revisionAtDeadline,
          "repeated update after deadline is stable");
}

void CheckWrapSafeDeadline() {
    firmware::SleepTransition transition;
    const uint32_t start = UINT32_MAX - 1000U;
    Check(transition.Request(firmware::SleepRequestOrigin::Idle, start, true),
          "wrap test request starts");
    transition.Update(start + 2999U);
    Check(transition.GetState() == firmware::SleepState::Countdown,
          "wrap-safe 2999 ms remains countdown");
    transition.Update(start + 3500U);
    Check(transition.GetState() == firmware::SleepState::Preparing,
          "wrap-safe deadline enters preparing");
}

void CheckCancellationCauses() {
    const firmware::SleepTransitionCause causes[] = {
        firmware::SleepTransitionCause::ButtonPress,
        firmware::SleepTransitionCause::BoardObservationChanged,
        firmware::SleepTransitionCause::ValidatedMotionOrTap,
        firmware::SleepTransitionCause::PreflightFailure,
        firmware::SleepTransitionCause::HigherPriorityState,
    };

    for (const auto cause : causes) {
        firmware::SleepTransition transition;
        transition.Request(firmware::SleepRequestOrigin::Idle, 0U, true);
        Check(transition.Cancel(cause), "local or priority cause cancels countdown");
        Check(transition.GetState() == firmware::SleepState::Awake,
              "countdown cancellation returns awake");
        Check(transition.GetCancelCause() == cause,
              "countdown cancellation cause remains observable");
    }

    firmware::SleepTransition preparing;
    preparing.Request(firmware::SleepRequestOrigin::Console, 0U, true);
    preparing.Update(firmware::SleepTransition::DEFAULT_COUNTDOWN_MS +
                     firmware::SleepTransition::FADE_MS);
    Check(preparing.GetState() == firmware::SleepState::Preparing,
          "preparing cancellation setup reaches preparing");
    Check(preparing.Cancel(firmware::SleepTransitionCause::ButtonPress),
          "button cancels partial preflight");
    Check(preparing.GetState() == firmware::SleepState::Awake,
          "partial-preflight cancellation returns awake");

    firmware::SleepTransition failed;
    failed.Request(firmware::SleepRequestOrigin::Menu, 0U, true);
    failed.Update(firmware::SleepTransition::DEFAULT_COUNTDOWN_MS +
                     firmware::SleepTransition::FADE_MS);
    Check(!failed.ReportPersistencePreflight(false),
          "failed persistence preflight is not reported as success");
    Check(failed.GetState() == firmware::SleepState::Awake,
          "persistence failure returns awake");
    Check(failed.GetCancelCause() ==
              firmware::SleepTransitionCause::PreflightFailure,
          "persistence failure has preflight cancel cause");

    firmware::SleepTransition failedWake;
    failedWake.Request(firmware::SleepRequestOrigin::Idle, 0U, true);
    failedWake.Update(firmware::SleepTransition::DEFAULT_COUNTDOWN_MS +
                     firmware::SleepTransition::FADE_MS);
    Check(failedWake.ReportPersistencePreflight(true),
          "persistence preflight succeeds");
    Check(!failedWake.ReportWakeSourcePreflight(false),
          "failed wake-source preflight is not reported as success");
    Check(failedWake.GetState() == firmware::SleepState::Awake,
          "wake-source failure returns awake");
    Check(failedWake.GetCancelCause() ==
              firmware::SleepTransitionCause::PreflightFailure,
          "wake-source failure has preflight cancel cause");
}

void CheckCommitAndAuthorization() {
    firmware::SleepTransition transition;
    transition.Request(firmware::SleepRequestOrigin::Menu, 10U, true);
    transition.Update(3510U);

    Check(!transition.CommitSleep(),
          "commit cannot happen before either preflight");
    Check(transition.GetState() == firmware::SleepState::Preparing,
          "early commit leaves preparing");
    Check(transition.ReportPersistencePreflight(true),
          "persistence preflight reports success");
    Check(!transition.CommitSleep(),
          "commit cannot happen before wake preflight");
    Check(transition.ReportWakeSourcePreflight(true),
          "wake-source preflight reports success");
    Check(!transition.HasPendingSleepAuthorization(),
          "preflights alone do not authorize sleep");
    Check(transition.CommitSleep(),
          "explicit final commit authorizes sleep");
    Check(transition.GetState() == firmware::SleepState::Sleeping,
          "final commit enters sleeping");
    Check(transition.HasPendingSleepAuthorization(),
          "sleep authorization is observable once");
    Check(transition.ConsumeSleepAuthorization(),
          "sleep authorization is emitted once");
    Check(!transition.ConsumeSleepAuthorization(),
          "sleep authorization cannot be emitted twice");
    Check(!transition.Cancel(firmware::SleepTransitionCause::ButtonPress),
          "sleeping is non-cancellable");
    Check(!transition.Request(firmware::SleepRequestOrigin::Idle, 4000U, true),
          "sleeping rejects new requests");
}

void CheckWakeGuardLifecycle() {
    firmware::SleepTransition transition;
    transition.Request(firmware::SleepRequestOrigin::Console, 0U, true);
    transition.Update(firmware::SleepTransition::DEFAULT_COUNTDOWN_MS +
                     firmware::SleepTransition::FADE_MS);
    transition.ReportPersistencePreflight(true);
    transition.ReportWakeSourcePreflight(true);
    transition.CommitSleep();
    transition.ConsumeSleepAuthorization();

    Check(transition.BeginWakeGuard(100U), "sleeping enters wake guard");
    Check(transition.GetState() == firmware::SleepState::WakeGuard,
          "wake guard state is observable");
    Check(!transition.Request(firmware::SleepRequestOrigin::Idle, 100U, true),
          "wake guard rejects sleep requests");
    transition.Update(299U);
    Check(transition.GetState() == firmware::SleepState::WakeGuard,
          "wake guard waits for release and guard time");
    Check(transition.ReportWakeButtonsReleased(),
          "caller can report wake buttons released");
    transition.Update(299U);
    Check(transition.GetState() == firmware::SleepState::WakeGuard,
          "wake guard requires the full 200 ms after entry");
    transition.Update(300U);
    Check(transition.GetState() == firmware::SleepState::Awake,
          "released wake guard returns to awake");

    transition.Reset();
    Check(transition.GetState() == firmware::SleepState::Awake,
          "reset leaves the owner awake");
    Check(transition.GetOrigin() == firmware::SleepRequestOrigin::None,
          "reset clears prior origin");
    Check(transition.GetCancelCause() == firmware::SleepTransitionCause::None,
          "reset clears prior cancel cause");
}

void CheckSharedEntryActivityOption() {
    for (const auto origin : {firmware::SleepRequestOrigin::App,
                              firmware::SleepRequestOrigin::Menu,
                              firmware::SleepRequestOrigin::Console}) {
        firmware::SleepTransition transition;
        const bool app = origin == firmware::SleepRequestOrigin::App;
        const uint32_t countdownMs = app ? 0U : 3000U;
        const auto initialState = app ? firmware::SleepState::Fading
                                     : firmware::SleepState::Countdown;
        Check(transition.Request(origin, 100U, false) &&
                  transition.GetState() == initialState &&
                  transition.GetRemainingCountdownMs() == countdownMs && transition.GetFadeScale256() == 256U,
              "app entry starts the fade; other directed origins retain the countdown");
        const auto revision = transition.GetRevision();
        for (const auto cause : {firmware::SleepTransitionCause::ValidatedMotionOrTap,
                                 firmware::SleepTransitionCause::ButtonPress,
                                 firmware::SleepTransitionCause::BoardObservationChanged}) {
            Check(!transition.Cancel(cause) && transition.GetState() == initialState &&
                      transition.GetRevision() == revision,
                  "false ignores all normal activity without changing entry state");
        }
        Check(transition.Request(firmware::SleepRequestOrigin::Idle, 200U, true) &&
                  transition.GetOrigin() == origin && transition.GetRevision() == revision,
              "true cannot downgrade a committed false request");
        Check(!transition.ReportPersistencePreflight(true), "manual entry cannot prepare before the fade");
        transition.Update(100U + countdownMs);
        Check(transition.GetState() == firmware::SleepState::Fading && transition.GetFadeScale256() == 256U,
              "manual fade starts at its origin's countdown boundary");
        transition.Update(350U + countdownMs);
        Check(transition.GetState() == firmware::SleepState::Fading && transition.GetFadeScale256() == 128U,
              "manual fade exposes an intermediate half-brightness frame");
        Check(!transition.Cancel(firmware::SleepTransitionCause::ValidatedMotionOrTap),
              "manual fade retains activity immunity");
        transition.Update(600U + countdownMs);
        Check(transition.GetState() == firmware::SleepState::Preparing && transition.GetFadeScale256() == 0U &&
                  transition.ReportPersistencePreflight(true) &&
                  transition.ReportWakeSourcePreflight(true) && transition.CommitSleep(),
              "both options authorize only after the complete fade and common preflights");
    }
    for (const uint32_t elapsed : {0U, 3250U, 3500U}) {
        firmware::SleepTransition idle;
        idle.Request(firmware::SleepRequestOrigin::Idle, 0U, true);
        idle.Update(elapsed);
        Check(idle.Cancel(firmware::SleepTransitionCause::ValidatedMotionOrTap) &&
                  idle.GetState() == firmware::SleepState::Awake,
              "true retains cancellation through count-up, fade and preparation");
    }
    firmware::SleepTransition overlapping;
    overlapping.Request(firmware::SleepRequestOrigin::Idle, 0U, true);
    overlapping.Update(1500U);
    Check(overlapping.Request(firmware::SleepRequestOrigin::App, 1500U, false) &&
              overlapping.GetOrigin() == firmware::SleepRequestOrigin::App &&
              overlapping.GetState() == firmware::SleepState::Fading &&
              overlapping.GetRemainingCountdownMs() == 0U,
          "app takeover starts fading immediately instead of finishing the automatic cue");
    firmware::SleepTransition fading;
    fading.Request(firmware::SleepRequestOrigin::Idle, 0U, true);
    fading.Update(3250U);
    Check(fading.Request(firmware::SleepRequestOrigin::App, 3250U, false) &&
              fading.GetState() == firmware::SleepState::Fading && fading.GetFadeScale256() == 128U,
          "manual takeover preserves an in-progress fade without a brightness jump");
    fading.Update(3500U);
    Check(fading.GetState() == firmware::SleepState::Preparing,
          "manual takeover preserves the original fade completion time");
    for (const auto cause : {firmware::SleepTransitionCause::PreflightFailure,
                             firmware::SleepTransitionCause::HigherPriorityState}) {
        firmware::SleepTransition guarded;
        guarded.Request(firmware::SleepRequestOrigin::App, 0U, false);
        Check(guarded.Cancel(cause) && guarded.GetState() == firmware::SleepState::Awake,
              "false preserves common failure and safety cancellation");
    }
}

void CheckInvalidCausesFailClosed() {
    firmware::SleepTransition transition;
    transition.Request(firmware::SleepRequestOrigin::Idle, 0U, true);
    const uint16_t revision = transition.GetRevision();
    Check(!transition.Cancel(firmware::SleepTransitionCause::None),
          "none cause is rejected");
    Check(!transition.Cancel(static_cast<firmware::SleepTransitionCause>(99U)),
          "invalid cause is rejected");
    Check(transition.GetState() == firmware::SleepState::Countdown,
          "invalid causes do not alter state");
    Check(transition.GetRevision() == revision,
          "invalid causes do not alter revision");
}

} // namespace

int main() {
    std::puts("[SLEEP_TRANSITION] start");

    CheckDefaultState();
    CheckOriginsAndStateRejections();
    CheckDeadlineAndIdempotence();
    CheckWrapSafeDeadline();
    CheckCancellationCauses();
    CheckCommitAndAuthorization();
    CheckWakeGuardLifecycle();
    CheckSharedEntryActivityOption();
    CheckInvalidCausesFailClosed();

    if (g_failures == 0) {
        std::puts("[SLEEP_TRANSITION] ok");
        return 0;
    }

    std::printf("[SLEEP_TRANSITION] failures=%d\n", g_failures);
    return 1;
}
