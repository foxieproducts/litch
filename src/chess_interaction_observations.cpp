#include "runtime/chess_interaction_model.hpp"

#include "chess_interaction_capture.hpp"
#include "chess_interaction_model_internal.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"

namespace runtime {

using chess_interaction_internal::ClearPhase;
using chess_interaction_internal::ReduceIdleObservation;
using chess_interaction_internal::ReducePieceLiftedObservation;
using chess_interaction_internal::ReducePlanObservation;
using chess_interaction_internal::TryReduceSettlingProgressRegression;
using chess_interaction_internal::UpdatePlanProgress;

void ApplyChessInteractionBoardObservation(ChessInteractionModel& model,
                                           const ChessInteractionEvent& event) {
    if (model.board.valid && event.sequence <= model.board.sequence) {
        ++model.inputFaultEpoch;
        return;
    }

    const bool hadObservation = model.board.valid;
    model.board.valid = true;
    model.board.changedMask = event.changedMask;
    model.board.occupiedMask = event.occupiedMask;
    model.board.timestampMs = event.timestampMs;
    model.board.sequence = event.sequence;
    if (!hadObservation || event.changedMask != 0ULL) {
        model.board.stableSinceMs = event.timestampMs;
    }

    const ChessInteractionEvent chessObservation =
        ProjectHumanChessObservation(model, event);
    switch (model.phase.tag) {
        case ChessInteractionPhase::Idle:
            ReduceIdleObservation(model, chessObservation);
            return;
        case ChessInteractionPhase::PieceLifted:
            ReducePieceLiftedObservation(model, chessObservation);
            return;
        case ChessInteractionPhase::CaptureStaged:
            if ((GetKnownNeutralDuckMask(model.acknowledged) &
                 ~event.occupiedMask) != 0ULL) {
                EnterChessInteractionCorrection(
                    model, event.occupiedMask ^ model.acknowledged.occupiedMask);
                return;
            }
            chess_interaction_capture::ReduceCaptureStagedObservation(
                model, chessObservation);
            return;
        case ChessInteractionPhase::ExecutingPlan:
            ReducePlanObservation(model, chessObservation);
            return;
        case ChessInteractionPhase::AwaitingAction:
            if (model.phase.plan.kind ==
                PhysicalTransitionKind::DuckRelocation) {
                ApplyDuckRelocationBoardObservation(model, chessObservation);
                return;
            }
            UpdatePlanProgress(model.phase.plan, chessObservation.occupiedMask);
            return;
        case ChessInteractionPhase::Settling: {
            if (model.phase.plan.kind == PhysicalTransitionKind::DuckRelocation) {
                ApplyDuckRelocationBoardObservation(model, chessObservation);
                return;
            }
            // A neutral-only lift or landing cannot regress incomplete chess
            // choreography (in particular, an en passant removal).
            if (event.changedMask != 0ULL && chessObservation.changedMask == 0ULL) {
                return;
            }
            if (TryReduceSettlingProgressRegression(model, chessObservation)) {
                return;
            }
            UpdatePlanProgress(model.phase.plan, chessObservation.occupiedMask);
            return;
        }
        case ChessInteractionPhase::DuckRelocation:
            ApplyDuckRelocationBoardObservation(model, chessObservation);
            return;
        case ChessInteractionPhase::Correction:
            if (model.phase.plan.kind ==
                PhysicalTransitionKind::DuckRelocation) {
                ApplyDuckRelocationBoardObservation(model, chessObservation);
                return;
            }
            if (chessObservation.occupiedMask == model.acknowledged.occupiedMask) {
                ClearPhase(model.phase);
            } else {
                model.phase.correctionMask =
                    chessObservation.occupiedMask ^ model.acknowledged.occupiedMask;
            }
            return;
        default:
            return;
    }
}

} // namespace runtime
