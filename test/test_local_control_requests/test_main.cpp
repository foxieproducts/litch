#include <cstdint>
#include <cstdio>

#include "firmware/local_control_requests.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

struct Harness {
    modules::SettingsStore settings;
    modules::ControlMutationService service;
    modules::ControlCoordinatorResult hintStart =
        modules::ControlCoordinatorResult::Applied;
    modules::ControlCoordinatorResult undoStart =
        modules::ControlCoordinatorResult::Pending;
    modules::ControlCoordinatorResult newGameStart =
        modules::ControlCoordinatorResult::Applied;
    modules::ControlCoordinatorResult polled =
        modules::ControlCoordinatorResult::Pending;
    uint32_t hintStartCount = 0U;
    uint32_t undoStartCount = 0U;
    uint32_t newGameStartCount = 0U;
    uint32_t pollCount = 0U;

    Harness() {
        modules::ControlMutationCallbacks callbacks;
        callbacks.context = this;
        callbacks.requestHint = [](void* context, uint16_t) {
            auto& self = *static_cast<Harness*>(context);
            ++self.hintStartCount;
            return self.hintStart;
        };
        callbacks.requestUndo = [](void* context, uint16_t, uint8_t) {
            auto& self = *static_cast<Harness*>(context);
            ++self.undoStartCount;
            return self.undoStart;
        };
        callbacks.requestNewGame = [](void* context, uint16_t) {
            auto& self = *static_cast<Harness*>(context);
            ++self.newGameStartCount;
            return self.newGameStart;
        };
        callbacks.pollAction = [](void* context, uint16_t) {
            auto& self = *static_cast<Harness*>(context);
            ++self.pollCount;
            return self.polled;
        };
        service.Configure(settings, callbacks);
    }
};

modules::ControlMutationOutcome RunUndo(
    Harness& harness,
    firmware_internal::LocalControlRequestTracker& tracker,
    const firmware_internal::LocalControlRequestOrigin origin,
    bool& outTrackingFailed) {
    return firmware_internal::RunLocalControlAction(
        harness.service,
        tracker,
        origin,
        modules::ControlAction::Undo,
        modules::UiOwner::None,
        modules::CoachingMutation::Toggle,
        1U,
        outTrackingFailed);
}

void TestFeedbackMappingAndOriginIsolation() {
    using firmware_internal::ClassifyBoardControlFeedback;
    using firmware_internal::LocalControlRequestOrigin;
    using modules::ControlMutationReason;
    using modules::ControlMutationStatus;

    constexpr ControlMutationReason reasons[] = {
        ControlMutationReason::None,
        ControlMutationReason::BadRequest,
        ControlMutationReason::UnknownAction,
        ControlMutationReason::QueueFull,
        ControlMutationReason::UnknownKey,
        ControlMutationReason::TypeMismatch,
        ControlMutationReason::OutOfRange,
        ControlMutationReason::InvalidEnumValue,
        ControlMutationReason::NotAtStart,
        ControlMutationReason::Busy,
        ControlMutationReason::NoHistory,
        ControlMutationReason::GameOver,
        ControlMutationReason::Unsupported,
        ControlMutationReason::EngineRejected,
        ControlMutationReason::StorageUnavailable,
    };

    modules::ControlMutationOutcome outcome;
    outcome.status = ControlMutationStatus::Rejected;
    for (const ControlMutationReason reason : reasons) {
        outcome.action = modules::ControlAction::Unknown;
        outcome.reason = reason;
        const auto feedback = ClassifyBoardControlFeedback(
            LocalControlRequestOrigin::Board, outcome);
        const bool occupiedBoard =
            reason == ControlMutationReason::NotAtStart;
        Check(feedback.visible &&
                  feedback.flashAllPieces == occupiedBoard &&
                  feedback.edgeCue ==
                      (occupiedBoard
                           ? modules::EdgeLedCueKind::OccupiedBoardBlocked
                           : modules::EdgeLedCueKind::Unavailable),
              "board rejection distinguishes occupied-board feedback from the generic unavailable cue");

        const auto consoleFeedback = ClassifyBoardControlFeedback(
            LocalControlRequestOrigin::Console, outcome);
        Check(!consoleFeedback.visible,
              "console rejection never creates board feedback");
    }

    outcome.action = modules::ControlAction::Undo;
    for (const ControlMutationReason reason : {
             ControlMutationReason::Busy,
             ControlMutationReason::NoHistory,
         }) {
        outcome.reason = reason;
        const auto feedback = ClassifyBoardControlFeedback(
            LocalControlRequestOrigin::Board, outcome);
        Check(feedback.visible && feedback.edgeCue ==
                                      modules::EdgeLedCueKind::Unavailable,
              "unavailable board Undo selects the reusable perimeter cue");
    }

    outcome.action = modules::ControlAction::Hint;
    outcome.reason = ControlMutationReason::GameOver;
    const auto terminalHintFeedback = ClassifyBoardControlFeedback(
        LocalControlRequestOrigin::Board, outcome);
    Check(terminalHintFeedback.visible &&
              terminalHintFeedback.edgeCue ==
                  modules::EdgeLedCueKind::Unavailable,
          "terminal board Hint selects the reusable perimeter cue");

    constexpr ControlMutationStatus quietStatuses[] = {
        ControlMutationStatus::Accepted,
        ControlMutationStatus::Applied,
        ControlMutationStatus::Noop,
    };
    for (const ControlMutationStatus status : quietStatuses) {
        outcome.status = status;
        outcome.reason = ControlMutationReason::None;
        Check(!ClassifyBoardControlFeedback(
                   LocalControlRequestOrigin::Board, outcome)
                   .visible,
              "non-rejected board outcome has no success message");
    }
}

void TestImmediateTerminalOutcomeReplacesAccepted() {
    Harness harness;
    harness.undoStart = modules::ControlCoordinatorResult::NoHistory;
    firmware_internal::LocalControlRequestTracker tracker;
    bool trackingFailed = true;

    const modules::ControlMutationOutcome outcome = RunUndo(
        harness,
        tracker,
        firmware_internal::LocalControlRequestOrigin::Board,
        trackingFailed);
    const auto feedback = firmware_internal::ClassifyBoardControlFeedback(
        firmware_internal::LocalControlRequestOrigin::Board, outcome);

    Check(outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason == modules::ControlMutationReason::NoHistory,
          "immediate terminal completion is returned instead of Accepted");
    Check(outcome.requestId != 0U && harness.undoStartCount == 1U &&
              tracker.Count() == 0U && !trackingFailed,
          "immediate completion executes once without pending tracking");
    Check(feedback.visible && feedback.edgeCue ==
                                  modules::EdgeLedCueKind::Unavailable,
          "immediate no-history Undo selects perimeter-only feedback");

    harness.hintStart = modules::ControlCoordinatorResult::Busy;
    trackingFailed = true;
    const modules::ControlMutationOutcome hint =
        firmware_internal::RunLocalControlAction(
            harness.service,
            tracker,
            firmware_internal::LocalControlRequestOrigin::Board,
            modules::ControlAction::Hint,
            modules::UiOwner::None,
            modules::CoachingMutation::Toggle,
            1U,
            trackingFailed);
    Check(hint.status == modules::ControlMutationStatus::Rejected &&
              hint.reason == modules::ControlMutationReason::Busy &&
              harness.hintStartCount == 1U && !trackingFailed,
          "already-terminal Hint rejection remains terminal and untracked");
    const auto hintFeedback = firmware_internal::ClassifyBoardControlFeedback(
        firmware_internal::LocalControlRequestOrigin::Board, hint);
    Check(hintFeedback.visible &&
              hintFeedback.edgeCue ==
                  modules::EdgeLedCueKind::Unavailable,
          "busy Hint rejection uses non-text perimeter feedback");
}

void TestDelayedBoardRejectionAndTrackerRemoval() {
    Harness harness;
    firmware_internal::LocalControlRequestTracker tracker;
    bool trackingFailed = false;
    const modules::ControlMutationOutcome accepted = RunUndo(
        harness,
        tracker,
        firmware_internal::LocalControlRequestOrigin::Board,
        trackingFailed);

    Check(accepted.status == modules::ControlMutationStatus::Accepted &&
              tracker.Count() == 1U && !trackingFailed,
          "genuinely pending board action is tracked with Accepted outcome");

    harness.polled = modules::ControlCoordinatorResult::NotAtStart;
    harness.service.Update();
    firmware_internal::LocalControlCompletion completion;
    Check(firmware_internal::TryTakeNextLocalControlCompletion(
              harness.service, tracker, completion),
          "delayed tracked board completion is retrieved");
    Check(completion.request.requestId == accepted.requestId &&
              completion.request.origin ==
                  firmware_internal::LocalControlRequestOrigin::Board &&
              completion.outcome.status ==
                  modules::ControlMutationStatus::Rejected &&
              completion.outcome.reason ==
                  modules::ControlMutationReason::NotAtStart,
          "delayed completion retains board origin and terminal reason");
    const auto feedback = firmware_internal::ClassifyBoardControlFeedback(
        completion.request.origin, completion.outcome);
    Check(feedback.visible && feedback.flashAllPieces &&
              feedback.edgeCue ==
                  modules::EdgeLedCueKind::OccupiedBoardBlocked,
          "delayed not-at-start board rejection selects occupied-piece and amber perimeter feedback");
    Check(tracker.Count() == 0U &&
              !firmware_internal::TryTakeNextLocalControlCompletion(
                  harness.service, tracker, completion),
          "taking delayed completion removes exactly one tracker entry");
}

void TestMixedCompletionOwnershipAndConsoleIsolation() {
    Harness harness;
    firmware_internal::LocalControlRequestTracker tracker;
    bool trackingFailed = false;

    const auto boardAccepted = RunUndo(
        harness,
        tracker,
        firmware_internal::LocalControlRequestOrigin::Board,
        trackingFailed);
    harness.polled = modules::ControlCoordinatorResult::NoHistory;
    harness.service.Update();

    const auto unrelated = harness.service.MutateAction(
        modules::ControlAction::NewGame, 900U);
    Check(unrelated.status == modules::ControlMutationStatus::Accepted,
          "untracked completion setup action accepted");
    harness.service.Update();

    harness.undoStart = modules::ControlCoordinatorResult::Pending;
    harness.polled = modules::ControlCoordinatorResult::Pending;
    const auto consoleAccepted = RunUndo(
        harness,
        tracker,
        firmware_internal::LocalControlRequestOrigin::Console,
        trackingFailed);
    harness.polled = modules::ControlCoordinatorResult::Busy;
    harness.service.Update();

    Check(boardAccepted.status == modules::ControlMutationStatus::Accepted &&
              consoleAccepted.status ==
                  modules::ControlMutationStatus::Accepted &&
              tracker.Count() == 2U,
          "mixed board and console requests retain separate origins");

    firmware_internal::LocalControlCompletion completion;
    Check(firmware_internal::TryTakeNextLocalControlCompletion(
              harness.service, tracker, completion) &&
              completion.request.requestId == boardAccepted.requestId &&
              completion.request.origin ==
                  firmware_internal::LocalControlRequestOrigin::Board,
          "mixed drain first returns exact board-owned completion");
    Check(firmware_internal::ClassifyBoardControlFeedback(
              completion.request.origin, completion.outcome)
              .visible,
          "board-owned delayed rejection creates feedback");

    Check(firmware_internal::TryTakeNextLocalControlCompletion(
              harness.service, tracker, completion) &&
              completion.request.requestId == consoleAccepted.requestId &&
              completion.request.origin ==
                  firmware_internal::LocalControlRequestOrigin::Console,
          "mixed drain next returns exact console-owned completion");
    Check(!firmware_internal::ClassifyBoardControlFeedback(
               completion.request.origin, completion.outcome)
               .visible,
          "console-owned delayed rejection remains board-UI silent");

    modules::ControlMutationOutcome retained;
    Check(tracker.Count() == 0U &&
              harness.service.TryTakeCompletion(900U, retained) &&
              retained.status == modules::ControlMutationStatus::Applied,
          "targeted mixed drain preserves unrelated completion ownership");
}

void TestTrackingFailureAbandonsOnlyCompletionOwnership() {
    Harness harness;
    firmware_internal::LocalControlRequestTracker tracker;
    for (size_t index = 0U;
         index < firmware_internal::LocalControlRequestTracker::CAPACITY;
         ++index) {
        Check(tracker.Track(
                  static_cast<uint16_t>(100U + index),
                  firmware_internal::LocalControlRequestOrigin::Console),
              "tracker capacity setup entry accepted");
    }

    bool trackingFailed = false;
    const modules::ControlMutationOutcome accepted = RunUndo(
        harness,
        tracker,
        firmware_internal::LocalControlRequestOrigin::Board,
        trackingFailed);
    Check(accepted.status == modules::ControlMutationStatus::Accepted &&
              trackingFailed &&
              tracker.Count() ==
                  firmware_internal::LocalControlRequestTracker::CAPACITY,
          "full tracker reports failure without growing past its bound");

    harness.polled = modules::ControlCoordinatorResult::Applied;
    harness.service.Update();
    modules::ControlMutationOutcome completion;
    Check(harness.undoStartCount == 1U && harness.pollCount == 1U &&
              !harness.service.TryTakeCompletion(
                  accepted.requestId, completion),
          "tracking failure abandons response but accepted action executes once");
}

} // namespace

int main() {
    std::puts("[LOCAL_CONTROL_REQUESTS] start");
    TestFeedbackMappingAndOriginIsolation();
    TestImmediateTerminalOutcomeReplacesAccepted();
    TestDelayedBoardRejectionAndTrackerRemoval();
    TestMixedCompletionOwnershipAndConsoleIsolation();
    TestTrackingFailureAbandonsOnlyCompletionOwnership();

    if (g_failures == 0) {
        std::puts("[LOCAL_CONTROL_REQUESTS] ok");
        return 0;
    }
    std::printf("[LOCAL_CONTROL_REQUESTS] failures=%d\n", g_failures);
    return 1;
}
