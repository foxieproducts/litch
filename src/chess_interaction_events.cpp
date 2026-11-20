#include "runtime/chess_interaction_model.hpp"

#include "chess_interaction_capture.hpp"
#include "chess_interaction_model_internal.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"

namespace runtime {

ChessInteractionEvent ChessInteractionEvent::MakeResetPreservingPosition() {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::ResetPreservingPosition;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakeDurableStateRestored(
    const TaggedInteractionState& phase,
    const uint32_t nextActionToken) {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::DurableStateRestored;
    event.restoredPhase = phase;
    event.restoredNextActionToken = nextActionToken;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakePlanRequested(
    const PhysicalTransitionPlan& plan) {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::PlanRequested;
    event.requestedPlan = plan;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakePlanCanceled(
    const uint8_t selectedSquare) {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::PlanCanceled;
    event.square = selectedSquare;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakeUndoCompleted() {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::UndoCompleted;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakeSettleConfirmed() {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::SettleConfirmed;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakeSettleProgressRegressed() {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::SettleProgressRegressed;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakePlanProgressObserved(
    const uint64_t occupiedMask) {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::PlanProgressObserved;
    event.occupiedMask = occupiedMask;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakeUndoRestorationConfigured(
    const uint8_t square,
    const char pieceFen) {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::UndoRestorationConfigured;
    event.square = square;
    event.requestedPlan.restorationPieceFen = pieceFen;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakeBoardObservation(
    const uint32_t timestampMs,
    const uint32_t sequence,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::BoardObservation;
    event.timestampMs = timestampMs;
    event.sequence = sequence;
    event.changedMask = changedMask;
    event.occupiedMask = occupiedMask;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakeDeferredBoardObservation(
    const uint32_t timestampMs,
    const uint32_t sequence,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    ChessInteractionEvent event = MakeBoardObservation(
        timestampMs, sequence, changedMask, occupiedMask);
    event.type = ChessInteractionEventType::DeferredBoardObservation;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakeModalBoardObservation(
    const uint32_t timestampMs,
    const uint32_t sequence,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    ChessInteractionEvent event = MakeBoardObservation(
        timestampMs, sequence, changedMask, occupiedMask);
    event.type = ChessInteractionEventType::ModalBoardObservation;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakeCaptureProgressBoardObservation(
    const uint32_t timestampMs,
    const uint32_t sequence,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    ChessInteractionEvent event = MakeBoardObservation(
        timestampMs, sequence, changedMask, occupiedMask);
    event.type = ChessInteractionEventType::CaptureProgressBoardObservation;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakePositionAcknowledged(
    const uint64_t occupiedMask,
    const uint64_t kingMask,
    const uint64_t rookMask,
    const uint64_t moverOccupiedMask,
    const uint64_t opponentOccupiedMask,
    const uint64_t pawnMask,
    const uint8_t enPassantTargetSquare,
    const uint8_t enPassantCaptureSquare,
    const bool sideToMoveValid,
    const bool sideToMoveBlack,
    const bool legalCaptureTargetsValid,
    const uint64_t legalCaptureTargetsMask,
    const bool legalCastlingTargetsValid,
    const uint64_t legalCastlingKingTargetsMask) {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::PositionAcknowledged;
    auto& position = event.position;
    position.occupiedMask = occupiedMask;
    position.kingMask = kingMask;
    position.rookMask = rookMask;
    position.pawnMask = pawnMask;
    position.moverOccupiedMask = moverOccupiedMask;
    position.opponentOccupiedMask = opponentOccupiedMask;
    position.enPassantTargetSquare = enPassantTargetSquare;
    position.enPassantCaptureSquare = enPassantCaptureSquare;
    position.sideToMoveValid = sideToMoveValid;
    position.sideToMoveBlack = sideToMoveBlack;
    position.legalCaptureTargetsValid = legalCaptureTargetsValid;
    position.legalCaptureTargetsMask = legalCaptureTargetsMask;
    position.legalCastlingTargetsValid = legalCastlingTargetsValid;
    position.legalCastlingKingTargetsMask = legalCastlingKingTargetsMask;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakeActionResult(
    const uint32_t token,
    const bool succeeded) {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::ActionResult;
    event.actionToken = token;
    event.actionSucceeded = succeeded;
    return event;
}

ChessInteractionEvent ChessInteractionEvent::MakeTick(const uint32_t timestampMs,
                                                        const uint32_t sequence) {
    ChessInteractionEvent event;
    event.type = ChessInteractionEventType::Tick;
    event.timestampMs = timestampMs;
    event.sequence = sequence;
    return event;
}

namespace {

void ResetInteractionProgress(ChessInteractionModel& model) {
    model.phase = {};
    model.nextActionToken = 1U;
    model.feedbackEpoch = 0U;
    model.inputFaultEpoch = 0U;
}

} // namespace

void ResetChessInteractionModel(ChessInteractionModel& model) {
    model.acknowledged = {};
    model.acknowledgedCaptureLegality.Clear();
    model.board = {};
    ResetInteractionProgress(model);
}

void ReduceChessInteraction(ChessInteractionModel& model,
                            const ChessInteractionEvent& event,
                            const CaptureLegality* const captureLegality) {
    switch (event.type) {
        case ChessInteractionEventType::Reset:
            ResetChessInteractionModel(model);
            return;
        case ChessInteractionEventType::ResetPreservingPosition: {
            // Keep the sensed board and acknowledged capture rows in place
            // until the external position arrives through acknowledgement.
            ResetInteractionProgress(model);
            model.acknowledged.revision = 0U;
            return;
        }
        case ChessInteractionEventType::DurableStateRestored:
            if (model.phase.tag != ChessInteractionPhase::Idle) {
                ++model.inputFaultEpoch;
                return;
            }
            model.phase = event.restoredPhase;
            model.nextActionToken = event.restoredNextActionToken;
            if (model.nextActionToken <= model.phase.pendingActionToken) {
                model.nextActionToken = model.phase.pendingActionToken + 1U;
            }
            return;
        case ChessInteractionEventType::PlanRequested:
            ApplyChessInteractionPlanRequested(model, event);
            return;
        case ChessInteractionEventType::PlanCanceled:
            if ((model.phase.plan.source == PhysicalTransitionSource::Human &&
                 (model.phase.tag == ChessInteractionPhase::PieceLifted ||
                  model.phase.tag == ChessInteractionPhase::CaptureStaged ||
                  model.phase.tag == ChessInteractionPhase::ExecutingPlan ||
                  model.phase.tag == ChessInteractionPhase::Settling ||
                  model.phase.tag == ChessInteractionPhase::DuckRelocation ||
                  model.phase.tag == ChessInteractionPhase::AwaitingAction)) ||
                (IsCpuDirectedTransition(model.phase.plan) &&
                 (model.phase.tag == ChessInteractionPhase::ExecutingPlan ||
                  model.phase.tag == ChessInteractionPhase::DuckRelocation ||
                  model.phase.tag == ChessInteractionPhase::AwaitingAction)) ||
                (model.phase.plan.source == PhysicalTransitionSource::Undo &&
                 (model.phase.tag == ChessInteractionPhase::ExecutingPlan ||
                  model.phase.tag == ChessInteractionPhase::AwaitingAction))) {
                model.phase = TaggedInteractionState{};
                if (event.square < 64U) {
                    model.phase.tag = ChessInteractionPhase::PieceLifted;
                    model.phase.selectedSquare = event.square;
                }
            }
            return;
        case ChessInteractionEventType::UndoCompleted:
            if (model.phase.plan.source != PhysicalTransitionSource::Undo ||
                model.phase.plan.kind != PhysicalTransitionKind::Undo ||
                !model.phase.plan.progress.actionAcknowledged) {
                ++model.inputFaultEpoch;
                return;
            }
            model.phase = TaggedInteractionState{};
            return;
        case ChessInteractionEventType::SettleConfirmed:
            if (model.phase.tag != ChessInteractionPhase::Settling &&
                !(model.phase.tag == ChessInteractionPhase::ExecutingPlan &&
                  IsCpuDirectedTransition(model.phase.plan) &&
                  model.board.occupiedMask ==
                      model.phase.plan.afterOccupiedMask &&
                  chess_interaction_internal::GetNextPlanAction(
                      model.phase.plan) == RequiredPhysicalAction::None)) {
                ++model.inputFaultEpoch;
                return;
            }
            model.phase.tag = ChessInteractionPhase::AwaitingAction;
            model.phase.requiredAction =
                RequiredPhysicalAction::AwaitAcknowledgement;
            model.phase.pendingActionToken = model.nextActionToken++;
            return;
        case ChessInteractionEventType::SettleProgressRegressed:
            if ((model.phase.tag != ChessInteractionPhase::Settling &&
                 model.phase.tag != ChessInteractionPhase::AwaitingAction) ||
                !chess_interaction_internal::
                    ReopenHumanPlanForCurrentOccupancy(
                        model, ProjectHumanChessOccupancy(
                            model.acknowledged, model.phase,
                            model.board.occupiedMask))) {
                ++model.inputFaultEpoch;
            }
            return;
        case ChessInteractionEventType::PlanProgressObserved:
            ApplyChessInteractionPlanProgressObserved(model, event);
            return;
        case ChessInteractionEventType::UndoRestorationConfigured:
            if (model.phase.plan.kind != PhysicalTransitionKind::Undo ||
                event.square >= 64U) {
                ++model.inputFaultEpoch;
                return;
            }
            model.phase.plan.restorationRequired = true;
            model.phase.plan.restorationSquare = event.square;
            model.phase.plan.restorationPieceFen =
                event.requestedPlan.restorationPieceFen;
            model.phase.plan.afterOccupiedMask |= (1ULL << event.square);
            return;
        case ChessInteractionEventType::BoardObservation:
            ApplyChessInteractionBoardObservation(model, event);
            return;
        case ChessInteractionEventType::DeferredBoardObservation:
            if (model.board.valid && event.sequence <= model.board.sequence) {
                ++model.inputFaultEpoch;
                return;
            }
            model.board.valid = true;
            model.board.changedMask = event.changedMask;
            model.board.occupiedMask = event.occupiedMask;
            model.board.timestampMs = event.timestampMs;
            model.board.sequence = event.sequence;
            if (event.changedMask != 0ULL) {
                model.board.stableSinceMs = event.timestampMs;
            }
            // CPU-thinking input cannot begin or advance a human plan, but it
            // also must not erase a coordinator-owned undo prompt. The undo
            // plan remains authoritative until its explicit completion or
            // cancellation event; this observation only refreshes the board
            // snapshot used when normal input resumes.
            if (model.phase.plan.source == PhysicalTransitionSource::Undo &&
                model.phase.plan.kind == PhysicalTransitionKind::Undo &&
                (model.phase.tag == ChessInteractionPhase::ExecutingPlan ||
                 model.phase.tag == ChessInteractionPhase::AwaitingAction)) {
                return;
            }
            // Sensor changes observed while the engine is thinking are
            // corrections, never the beginning of a human move. Keep the
            // reducer in a corrective phase until the physical board returns
            // to the acknowledged position so the first correction pickup is
            // not retained as a future move origin.
            model.phase = TaggedInteractionState{};
            model.phase.correctionMask =
                event.occupiedMask ^ model.acknowledged.occupiedMask;
            if (model.phase.correctionMask != 0ULL) {
                model.phase.tag = ChessInteractionPhase::Correction;
                model.phase.requiredAction =
                    RequiredPhysicalAction::CorrectBoard;
            }
            return;
        case ChessInteractionEventType::ModalBoardObservation:
        case ChessInteractionEventType::CaptureProgressBoardObservation:
            if (model.board.valid && event.sequence <= model.board.sequence) {
                ++model.inputFaultEpoch;
                return;
            }
            model.board.valid = true;
            model.board.changedMask = event.changedMask;
            model.board.occupiedMask = event.occupiedMask;
            model.board.timestampMs = event.timestampMs;
            model.board.sequence = event.sequence;
            if (event.changedMask != 0ULL) {
                model.board.stableSinceMs = event.timestampMs;
            }
            if (event.type ==
                ChessInteractionEventType::CaptureProgressBoardObservation) {
                chess_interaction_capture::RecordProgressDuringCorrection(
                    model, event);
            }
            return;
        case ChessInteractionEventType::PositionAcknowledged:
            ApplyChessInteractionPositionAcknowledged(model, event, captureLegality);
            return;
        case ChessInteractionEventType::ActionResult:
            ApplyChessInteractionActionResult(model, event);
            return;
        case ChessInteractionEventType::Tick:
            return;
    }
}

} // namespace runtime
