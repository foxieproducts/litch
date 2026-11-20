#include "firmware/local_control_requests.hpp"

#include "runtime/game_state_machine.hpp"
#include "modules/control_led_feedback_projection.hpp"

#ifdef ARDUINO
#include "firmware/firmware_internal.hpp"
#include "logger.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#endif

namespace firmware_internal {

bool LocalControlRequestTracker::Track(
    const uint16_t requestId,
    const LocalControlRequestOrigin origin) {
    if (requestId == 0U) {
        return false;
    }
    for (size_t index = 0U; index < m_count; ++index) {
        if (m_requests[index].requestId == requestId) {
            // The first tracker owns attribution; a duplicate callback must
            // never relabel Board work as Console work or vice versa.
            return true;
        }
    }
    if (m_count >= m_requests.size()) {
        return false;
    }
    m_requests[m_count++] = LocalControlRequest{requestId, origin};
    return true;
}

bool LocalControlRequestTracker::TryGet(
    const size_t index,
    LocalControlRequest& outRequest) const {
    if (index >= m_count) {
        return false;
    }
    outRequest = m_requests[index];
    return true;
}

bool LocalControlRequestTracker::Remove(const size_t index) {
    if (index >= m_count) {
        return false;
    }
    for (size_t next = index + 1U; next < m_count; ++next) {
        m_requests[next - 1U] = m_requests[next];
    }
    --m_count;
    m_requests[m_count] = LocalControlRequest{};
    return true;
}

BoardControlFeedback ClassifyBoardControlFeedback(
    const LocalControlRequestOrigin origin,
    const modules::ControlMutationOutcome& outcome) {
    if (origin != LocalControlRequestOrigin::Board ||
        outcome.status != modules::ControlMutationStatus::Rejected) {
        return BoardControlFeedback{};
    }
    // CPU Coaching rejection belongs to the attempted O control, not the
    // board perimeter or text. Its owner composes the two amber pulses.
    if (outcome.action == modules::ControlAction::Coaching &&
        outcome.reason == modules::ControlMutationReason::EngineRejected) {
        return BoardControlFeedback{};
    }

    // The board is not a readable text display. Preserve the precise reason in
    // the correlated outcome for app/console diagnostics, but keep physical
    // feedback spatial and action-neutral for every rejected board request.
    const bool occupiedBoard =
        outcome.reason == modules::ControlMutationReason::NotAtStart;
    return BoardControlFeedback{
        true,
        occupiedBoard,
        occupiedBoard ? modules::EdgeLedCueKind::OccupiedBoardBlocked
                      : modules::EdgeLedCueKind::Unavailable,
    };
}

modules::ControlMutationOutcome RunLocalControlAction(
    modules::ControlMutationService& service,
    LocalControlRequestTracker& tracker,
    const LocalControlRequestOrigin origin,
    const modules::ControlAction action,
    const modules::UiOwner owner,
    const modules::CoachingMutation coachingMutation,
    const uint8_t undoMoveCount,
    bool& outTrackingFailed) {
    outTrackingFailed = false;
    const modules::ControlMutationOutcome accepted = service.MutateAction(
        action, 0U, owner, coachingMutation, undoMoveCount);
    if (accepted.status != modules::ControlMutationStatus::Accepted) {
        return accepted;
    }

    service.Update();
    modules::ControlMutationOutcome completion;
    if (service.TryTakeCompletion(accepted.requestId, completion)) {
        return completion;
    }

    if (!tracker.Track(accepted.requestId, origin)) {
        outTrackingFailed = true;
        (void)service.AbandonCompletion(accepted.requestId);
    }
    return accepted;
}

modules::ControlMutationOutcome RunBoardUndoTap(
    runtime::GameStateMachine& stateMachine,
    const bool initialKingAssistOwnsX,
    modules::ControlMutationService& service,
    LocalControlRequestTracker& tracker,
    const uint8_t undoMoveCount,
    bool& outTrackingFailed, const modules::UiOwner owner) {
    uint64_t promptMask = 0ULL;
    if (initialKingAssistOwnsX &&
        !stateMachine.IsKingQueenSwapPromptActive() &&
        !stateMachine.IsUndoPromptActive() &&
        stateMachine.TryGetInitialKingPlacementPrompt(promptMask)) {
        stateMachine.CancelInitialKingPlacementPrompt();
        outTrackingFailed = false;
        modules::ControlMutationOutcome outcome;
        outcome.requestId = service.AllocateRequestId();
        outcome.action = modules::ControlAction::Undo;
        outcome.status = modules::ControlMutationStatus::Applied;
        outcome.reason = modules::ControlMutationReason::None;
        outcome.persistenceTier =
            modules::ControlMutationPersistenceTier::None;
        return outcome;
    }

    return RunLocalControlAction(service,
                                 tracker,
                                 LocalControlRequestOrigin::Board,
                                 modules::ControlAction::Undo,
                                 owner,
                                 modules::CoachingMutation::Toggle,
                                 undoMoveCount,
                                 outTrackingFailed);
}

modules::ControlMutationOutcome RunBoardHintTap(
    modules::ControlMutationService& service,
    LocalControlRequestTracker& tracker,
    bool& outTrackingFailed, const modules::UiOwner owner) {
    return RunLocalControlAction(service,
                                 tracker,
                                 LocalControlRequestOrigin::Board,
                                 modules::ControlAction::Hint,
                                 owner,
                                 modules::CoachingMutation::Toggle,
                                 1U,
                                 outTrackingFailed);
}

bool TryTakeNextLocalControlCompletion(
    modules::ControlMutationService& service,
    LocalControlRequestTracker& tracker,
    LocalControlCompletion& outCompletion) {
    for (size_t index = 0U; index < tracker.Count(); ++index) {
        LocalControlRequest request;
        if (!tracker.TryGet(index, request)) {
            continue;
        }
        modules::ControlMutationOutcome outcome;
        if (!service.TryTakeCompletion(request.requestId, outcome)) {
            continue;
        }
        (void)tracker.Remove(index);
        outCompletion = LocalControlCompletion{request, outcome};
        return true;
    }
    return false;
}

#ifdef ARDUINO
namespace {

void ShowBoardControlFeedback(FirmwareSystem& sys,
                              const LocalControlRequestOrigin origin,
                              const modules::ControlMutationOutcome& outcome,
                              const modules::UiOwner owner = modules::UiOwner::None) {
    if (origin == LocalControlRequestOrigin::Board &&
        outcome.status == modules::ControlMutationStatus::Rejected)
        modules::ObserveControlLedActionFeedback(sys.controlFeedback, outcome,
            owner, sys.controlMutation.GetActionState());
    if (origin == LocalControlRequestOrigin::Board &&
        outcome.action == modules::ControlAction::NewGame &&
        outcome.status == modules::ControlMutationStatus::Rejected) {
        sys.gameResetHoldRejected = true;
    }
    const BoardControlFeedback feedback =
        ClassifyBoardControlFeedback(origin, outcome);
    if (!feedback.visible) {
        return;
    }
    if (feedback.flashAllPieces) {
        sys.stateMachine.TriggerAllPiecesFlash(1000U);
    }
    sys.edgeCue.Show(
        feedback.edgeCue, modules::UiOwner::None, fxe::Millis());
}

} // namespace

modules::ControlMutationOutcome RunBoardControlAction(
    FirmwareSystem& sys,
    const modules::ControlAction action,
    const modules::UiOwner owner,
    const modules::CoachingMutation coachingMutation,
    const uint8_t undoMoveCount) {
    bool trackingFailed = false;
    const modules::ControlMutationOutcome outcome =
        action == modules::ControlAction::Hint
            ? RunBoardHintTap(sys.controlMutation,
                              sys.localControlRequests,
                              trackingFailed, owner)
            : RunLocalControlAction(sys.controlMutation,
                                    sys.localControlRequests,
                                    LocalControlRequestOrigin::Board,
                                    action,
                                    owner,
                                    coachingMutation,
                                    undoMoveCount,
                                    trackingFailed);
    if (trackingFailed) {
        logger::Warn("control: local completion capacity reached");
    }
    ShowBoardControlFeedback(
        sys, LocalControlRequestOrigin::Board, outcome, owner);
    return outcome;
}

modules::ControlMutationOutcome RunBoardUndoTap(
    FirmwareSystem& sys,
    const uint8_t undoMoveCount, const modules::UiOwner owner) {
    const runtime::InteractionModelSnapshot snapshot =
        runtime::BuildInteractionModelSnapshot(sys);
    const modules::led_frame_composer::internal::RenderIntent renderIntent =
        modules::led_frame_composer::internal::BuildRenderIntent({
            .snapshot = &snapshot,
            .menuRequested = sys.menuOverlay.IsActive(),
            .hintMoveValid = snapshot.render.hintMoveValid,
        });
    const bool initialKingAssistOwnsX =
        modules::led_frame_composer::internal::
            CanInitialKingPlacementAssistOwnBoard(
                sys.settings, snapshot, renderIntent);
    bool trackingFailed = false;
    const modules::ControlMutationOutcome outcome = RunBoardUndoTap(
        sys.stateMachine,
        initialKingAssistOwnsX,
        sys.controlMutation,
        sys.localControlRequests,
        undoMoveCount,
        trackingFailed, owner);
    if (trackingFailed) {
        logger::Warn("control: local completion capacity reached");
    }
    ShowBoardControlFeedback(
        sys, LocalControlRequestOrigin::Board, outcome);
    return outcome;
}

bool TrackLocalControlRequest(FirmwareSystem& sys,
                              const uint16_t requestId,
                              const LocalControlRequestOrigin origin) {
    if (sys.localControlRequests.Track(requestId, origin)) {
        return true;
    }
    logger::Warn("control: local completion capacity reached");
    (void)sys.controlMutation.AbandonCompletion(requestId);
    return false;
}

void DrainTrackedLocalControlCompletions(FirmwareSystem& sys) {
    LocalControlCompletion completion;
    while (TryTakeNextLocalControlCompletion(
        sys.controlMutation, sys.localControlRequests, completion)) {
        ShowBoardControlFeedback(
            sys, completion.request.origin, completion.outcome);
    }
}
#endif

} // namespace firmware_internal
