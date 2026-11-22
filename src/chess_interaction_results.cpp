#include "runtime/chess_interaction_model.hpp"

#include "chess_interaction_capture.hpp"
#include "chess_interaction_model_internal.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"

namespace runtime {
namespace {

bool ShouldPreserveProgress(const ChessInteractionModel& model,
                            const ChessPositionState& position) {
    return position.occupiedMask == model.acknowledged.occupiedMask &&
           (model.phase.tag == ChessInteractionPhase::PieceLifted ||
            model.phase.tag == ChessInteractionPhase::CaptureStaged ||
            model.phase.tag == ChessInteractionPhase::ExecutingPlan ||
            model.phase.tag == ChessInteractionPhase::Settling ||
            model.phase.tag == ChessInteractionPhase::AwaitingAction ||
            model.phase.tag == ChessInteractionPhase::DuckRelocation ||
            model.phase.tag == ChessInteractionPhase::Correction);
}

void ReplayFollowupObservation(ChessInteractionModel& model) {
    const uint64_t changed = model.board.occupiedMask ^
                             model.acknowledged.occupiedMask;
    if (changed == 0ULL) {
        chess_interaction_internal::ClearPhase(model.phase);
        return;
    }

    chess_interaction_internal::ClearPhase(model.phase);
    const ChessInteractionEvent followup =
        ChessInteractionEvent::MakeBoardObservation(
            model.board.timestampMs,
            model.board.sequence,
            changed,
            model.board.occupiedMask);
    chess_interaction_internal::ReduceIdleObservation(
        model, ProjectHumanChessObservation(model, followup));
    if (model.phase.tag == ChessInteractionPhase::Idle) {
        const uint64_t unexpected = changed &
            ~GetKnownNeutralDuckMask(model.acknowledged);
        if (unexpected != 0ULL) {
            EnterChessInteractionCorrection(model, changed);
        }
    }
}

bool TryContinueUndoPlan(ChessInteractionModel& model,
                         const PhysicalTransitionPlan& completedPlan) {
    if (completedPlan.source != PhysicalTransitionSource::Undo) {
        return false;
    }
    if (model.board.occupiedMask == completedPlan.afterOccupiedMask) {
        chess_interaction_internal::ClearPhase(model.phase);
    } else {
        model.phase.tag = ChessInteractionPhase::ExecutingPlan;
        model.phase.requiredAction =
            chess_interaction_internal::GetNextPlanAction(model.phase.plan);
    }
    return true;
}

void EnterSelectableDuckRollbackCorrection(ChessInteractionModel& model,
                                           const uint64_t correctionMask) {
    TaggedInteractionState& phase = model.phase;
    PhysicalTransitionPlan& plan = phase.plan;
    phase.tag = ChessInteractionPhase::DuckRelocation;
    phase.requiredAction = RequiredPhysicalAction::CorrectBoard;
    phase.pendingActionToken = 0U;
    phase.correctionMask = correctionMask;
    plan.afterOccupiedMask = plan.beforeOccupiedMask;
    plan.moveTo = CHESS_INTERACTION_INVALID_SQUARE;
    plan.relocations[0].to = CHESS_INTERACTION_INVALID_SQUARE;
    plan.progress.relocationsLiftedMask = 0U;
    plan.progress.relocationsPlacedMask = 0U;
    plan.progress.actionAcknowledged = false;
    ++model.feedbackEpoch;
}

void ApplySuccessfulActionResult(ChessInteractionModel& model) {
    const PhysicalTransitionPlan completedPlan = model.phase.plan;
    model.acknowledged = ProjectChessPositionAfterPlan(
        model.acknowledged, completedPlan);
    // A projected position has no exact engine-derived capture rights yet.
    model.acknowledgedCaptureLegality.Clear();
    ++model.acknowledged.revision;
    model.phase.plan.progress.actionAcknowledged = true;
    model.phase.pendingActionToken = 0U;
    if (TryContinueUndoPlan(model, completedPlan)) {
        return;
    }
    if (model.board.occupiedMask == completedPlan.afterOccupiedMask) {
        chess_interaction_internal::ClearPhase(model.phase);
        return;
    }
    // Keep the acknowledged plan as a short-lived tagged bridge until the
    // coordinator publishes the complete post-move position. Replaying now
    // would use projected (and deliberately invalidated) capture metadata.
    model.phase.tag = ChessInteractionPhase::ExecutingPlan;
    model.phase.requiredAction = RequiredPhysicalAction::None;
}

} // namespace

void ApplyChessInteractionPlanProgressObserved(
    ChessInteractionModel& model,
    const ChessInteractionEvent& event) {
    const bool restoresDuckSettlingCorrection =
        model.phase.tag == ChessInteractionPhase::Correction &&
        model.phase.plan.kind == PhysicalTransitionKind::DuckRelocation &&
        model.phase.plan.source == PhysicalTransitionSource::Human &&
        model.phase.selectedSquare == CHESS_INTERACTION_INVALID_SQUARE;
    model.board.valid = true;
    model.board.changedMask = model.board.occupiedMask ^ event.occupiedMask;
    model.board.occupiedMask = event.occupiedMask;
    ++model.board.sequence;
    if (model.phase.plan.kind == PhysicalTransitionKind::DuckRelocation &&
        (model.phase.tag == ChessInteractionPhase::DuckRelocation ||
         model.phase.tag == ChessInteractionPhase::Settling ||
         model.phase.tag == ChessInteractionPhase::AwaitingAction ||
         restoresDuckSettlingCorrection)) {
        ChessInteractionEvent observation = event;
        observation.changedMask = model.board.changedMask;
        ApplyDuckRelocationBoardObservation(model, observation);
        return;
    }
    if (model.phase.tag == ChessInteractionPhase::Correction) {
        if (event.occupiedMask == model.acknowledged.occupiedMask) {
            chess_interaction_internal::ClearPhase(model.phase);
        } else {
            model.phase.correctionMask =
                event.occupiedMask ^ model.acknowledged.occupiedMask;
        }
        return;
    }
    if (model.phase.tag == ChessInteractionPhase::ExecutingPlan) {
        chess_interaction_internal::ReducePlanObservation(
            model, ProjectHumanChessObservation(model, event));
        return;
    }
    const uint64_t chessOccupancy = ProjectHumanChessOccupancy(
        model.acknowledged, model.phase, event.occupiedMask);
    if (model.phase.tag == ChessInteractionPhase::AwaitingAction ||
        model.phase.tag == ChessInteractionPhase::Settling) {
        chess_interaction_internal::UpdatePlanProgress(
            model.phase.plan, chessOccupancy);
        if (model.phase.plan.source == PhysicalTransitionSource::Human &&
            chessOccupancy != model.phase.plan.afterOccupiedMask) {
            (void)chess_interaction_internal::
                ReopenHumanPlanForCurrentOccupancy(
                    model, chessOccupancy);
        }
    }
}

void ApplyChessInteractionPositionAcknowledged(
    ChessInteractionModel& model,
    const ChessInteractionEvent& event,
    const CaptureLegality* const captureLegality) {
    const bool replayAcknowledgedFollowup =
        model.phase.tag == ChessInteractionPhase::ExecutingPlan &&
        model.phase.plan.progress.actionAcknowledged &&
        (model.phase.plan.source == PhysicalTransitionSource::Human ||
         IsCpuDirectedTransition(model.phase.plan));
    const bool preserveProgress = ShouldPreserveProgress(model, event.position);
    const uint64_t revision = model.acknowledged.revision + 1U;
    model.acknowledged = event.position;
    model.acknowledged.revision = revision;
    if (captureLegality == nullptr) {
        model.acknowledgedCaptureLegality.Clear();
    } else if (captureLegality != &model.acknowledgedCaptureLegality) {
        model.acknowledgedCaptureLegality = *captureLegality;
    }
    if (replayAcknowledgedFollowup) {
        ReplayFollowupObservation(model);
    } else if (!preserveProgress) {
        chess_interaction_internal::ClearPhase(model.phase);
    } else if (model.board.valid && !model.phase.plan.progress.actionAcknowledged &&
               chess_interaction_capture::IsKnownInvalidHumanCapture(
                   model.acknowledgedCaptureLegality, model.phase.plan)) {
        EnterChessInteractionCorrection(model, model.board.occupiedMask ^ model.acknowledged.occupiedMask);
    }
}

void ApplyChessInteractionActionResult(ChessInteractionModel& model,
                                       const ChessInteractionEvent& event) {
    if (model.phase.tag != ChessInteractionPhase::AwaitingAction ||
        event.actionToken != model.phase.pendingActionToken) {
        ++model.inputFaultEpoch;
        return;
    }
    if (!event.actionSucceeded) {
        uint64_t correction = model.board.occupiedMask ^
                              model.acknowledged.occupiedMask;
        if (correction == 0ULL) {
            correction = model.phase.plan.beforeOccupiedMask ^
                         model.phase.plan.afterOccupiedMask;
        }
        const bool humanDuck =
            model.phase.plan.kind == PhysicalTransitionKind::DuckRelocation &&
            model.phase.plan.source == PhysicalTransitionSource::Human;
        if (humanDuck && model.phase.selectedSquare < 64U) {
            // A failed fixed human Duck action is a rollback to the
            // acknowledged source, not resumable pre-commit settling. Drop
            // the rejected plan so restoring that source completes normally.
            // CPU/history plans retain their fixed target for durable retry.
            EnterChessInteractionCorrection(model, correction);
        } else if (humanDuck) {
            // A rejected selectable placement still belongs to a staged
            // compound turn. Give rollback its existing DuckRelocation
            // correction state so it remains distinct from a resumable
            // pre-commit settling correction across persistence and reboot.
            EnterSelectableDuckRollbackCorrection(model, correction);
        } else {
            chess_interaction_internal::EnterPlanCorrection(model, correction);
        }
        return;
    }

    ApplySuccessfulActionResult(model);
}

} // namespace runtime
