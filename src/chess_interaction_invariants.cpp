#include "runtime/chess_interaction_model.hpp"

namespace runtime {

void CheckChessInteractionModelInvariants(const ChessInteractionModel& model,
                                           InvariantReport<8>& outReport) {
    const bool correctionPlan =
        model.phase.tag == ChessInteractionPhase::Correction &&
        model.phase.plan.kind != PhysicalTransitionKind::None;
    const bool planPhase = model.phase.tag == ChessInteractionPhase::ExecutingPlan ||
                           model.phase.tag == ChessInteractionPhase::AwaitingAction ||
                           model.phase.tag == ChessInteractionPhase::Settling ||
                           model.phase.tag == ChessInteractionPhase::DuckRelocation ||
                           correctionPlan;
    if (planPhase != (model.phase.plan.kind != PhysicalTransitionKind::None)) {
        outReport.Add("interaction_plan_phase");
    }
    if (model.phase.tag == ChessInteractionPhase::Idle &&
        model.phase.selectedSquare != CHESS_INTERACTION_INVALID_SQUARE) {
        outReport.Add("interaction_idle_selection", model.phase.selectedSquare);
    }
    if (model.phase.tag == ChessInteractionPhase::PieceLifted &&
        model.phase.selectedSquare >= 64U) {
        outReport.Add("interaction_lift_selection", model.phase.selectedSquare);
    }
    if (model.phase.tag == ChessInteractionPhase::AwaitingAction &&
        model.phase.pendingActionToken == 0U) {
        outReport.Add("interaction_missing_action_token");
    }
    if (model.phase.tag == ChessInteractionPhase::Correction &&
        model.phase.correctionMask == 0ULL) {
        outReport.Add("interaction_empty_correction");
    }
    if (model.phase.tag == ChessInteractionPhase::DuckRelocation &&
        model.phase.plan.kind != PhysicalTransitionKind::DuckRelocation) {
        outReport.Add("interaction_duck_plan_kind");
    }
    if (model.phase.plan.kind == PhysicalTransitionKind::DuckRelocation) {
        const PhysicalTransitionPlan& plan = model.phase.plan;
        const uint64_t sourceMask = plan.moveFrom < 64U
                                        ? (1ULL << plan.moveFrom)
                                        : 0ULL;
        const uint64_t typedChessMask =
            model.acknowledged.kingMask |
            model.acknowledged.rookMask |
            model.acknowledged.pawnMask |
            model.acknowledged.moverOccupiedMask |
            model.acknowledged.opponentOccupiedMask;
        const bool validDuckPhase =
            model.phase.tag == ChessInteractionPhase::DuckRelocation ||
            model.phase.tag == ChessInteractionPhase::Settling ||
            model.phase.tag == ChessInteractionPhase::AwaitingAction ||
            model.phase.tag == ChessInteractionPhase::Correction;
        const bool validDuckSource =
            plan.source == PhysicalTransitionSource::Human ||
            IsCpuDirectedTransition(plan);
        if (!validDuckSource ||
            plan.relocationCount != 1U || plan.moveFrom >= 64U ||
            plan.relocations[0].from != plan.moveFrom ||
            plan.relocations[0].to != plan.moveTo ||
            !validDuckPhase ||
            plan.beforeOccupiedMask != model.acknowledged.occupiedMask ||
            (plan.beforeOccupiedMask & sourceMask) == 0ULL ||
            (typedChessMask & sourceMask) != 0ULL ||
            plan.removalRequired || plan.promotionRequired ||
            plan.restorationRequired ||
            (plan.progress.relocationsLiftedMask & ~0x01U) != 0U ||
            (plan.progress.relocationsPlacedMask & ~0x01U) != 0U ||
            (plan.moveTo < 64U && plan.moveTo == plan.moveFrom) ||
            (model.phase.tag == ChessInteractionPhase::AwaitingAction &&
             plan.moveTo >= 64U)) {
            outReport.Add("interaction_duck_plan");
        }
        if (model.phase.selectedSquare < 64U &&
            (model.phase.selectedSquare != plan.moveTo ||
             model.phase.selectedSquare == plan.moveFrom ||
             (plan.beforeOccupiedMask &
              (1ULL << model.phase.selectedSquare)) != 0ULL)) {
            outReport.Add("interaction_duck_fixed_destination");
        }
        const uint64_t destinationMask = plan.moveTo < 64U
                                             ? (1ULL << plan.moveTo)
                                             : 0ULL;
        const uint64_t expectedAfter =
            (plan.beforeOccupiedMask & ~sourceMask) | destinationMask;
        if (model.phase.tag == ChessInteractionPhase::Settling) {
            if (plan.source != PhysicalTransitionSource::Human ||
                model.phase.selectedSquare != CHESS_INTERACTION_INVALID_SQUARE ||
                destinationMask == 0ULL || plan.afterOccupiedMask != expectedAfter ||
                plan.progress.relocationsLiftedMask != 0x01U ||
                plan.progress.relocationsPlacedMask != 0x01U ||
                model.phase.requiredAction != RequiredPhysicalAction::None ||
                model.phase.pendingActionToken != 0U) {
                outReport.Add("interaction_duck_settling");
            }
        } else if (model.phase.tag == ChessInteractionPhase::AwaitingAction) {
            if (destinationMask == 0ULL ||
                plan.afterOccupiedMask != expectedAfter ||
                plan.progress.relocationsLiftedMask != 0x01U ||
                plan.progress.relocationsPlacedMask != 0x01U ||
                model.phase.requiredAction !=
                    RequiredPhysicalAction::AwaitAcknowledgement) {
                outReport.Add("interaction_duck_awaiting_action");
            }
        } else if (model.phase.tag == ChessInteractionPhase::DuckRelocation) {
            const bool validAction =
                model.phase.requiredAction ==
                    RequiredPhysicalAction::LiftPrimary ||
                model.phase.requiredAction ==
                    RequiredPhysicalAction::PlacePrimary ||
                model.phase.requiredAction ==
                    RequiredPhysicalAction::CorrectBoard;
            const bool destinationStateValid =
                model.phase.selectedSquare < 64U
                    ? plan.moveTo == model.phase.selectedSquare
                    : plan.moveTo == CHESS_INTERACTION_INVALID_SQUARE;
            const bool actionProgressValid =
                model.phase.requiredAction ==
                    RequiredPhysicalAction::CorrectBoard ||
                (model.phase.requiredAction ==
                     RequiredPhysicalAction::LiftPrimary &&
                 plan.progress.relocationsLiftedMask == 0U) ||
                (model.phase.requiredAction ==
                     RequiredPhysicalAction::PlacePrimary &&
                 plan.progress.relocationsLiftedMask == 0x01U);
            if (!validAction || !destinationStateValid ||
                !actionProgressValid ||
                plan.afterOccupiedMask != plan.beforeOccupiedMask ||
                plan.progress.relocationsPlacedMask != 0U ||
                ((model.phase.requiredAction ==
                      RequiredPhysicalAction::CorrectBoard) !=
                 (model.phase.correctionMask != 0ULL))) {
                outReport.Add("interaction_duck_active_state");
            }
        } else if (destinationMask == 0ULL ||
                   plan.afterOccupiedMask != expectedAfter ||
                   plan.progress.relocationsLiftedMask != 0x01U ||
                   plan.progress.relocationsPlacedMask != 0x01U ||
                   model.phase.requiredAction !=
                       RequiredPhysicalAction::CorrectBoard) {
            outReport.Add("interaction_duck_action_correction");
        }
    }
    if (model.phase.plan.relocationCount > model.phase.plan.relocations.size()) {
        outReport.Add("interaction_relocation_count", model.phase.plan.relocationCount);
    }
    const ChessPositionState& position = model.acknowledged;
    if ((position.kingMask & ~position.occupiedMask) != 0ULL) {
        outReport.Add("interaction_king_not_occupied");
    }
    if ((position.rookMask & ~position.occupiedMask) != 0ULL) {
        outReport.Add("interaction_rook_not_occupied");
    }
    if ((position.pawnMask & ~position.occupiedMask) != 0ULL) {
        outReport.Add("interaction_pawn_not_occupied");
    }
    if ((position.moverOccupiedMask & ~position.occupiedMask) != 0ULL ||
        (position.opponentOccupiedMask & ~position.occupiedMask) != 0ULL ||
        (position.moverOccupiedMask & position.opponentOccupiedMask) != 0ULL) {
        outReport.Add("interaction_side_occupancy");
    }
    const bool epPairValid =
        position.enPassantTargetSquare < 64U &&
        position.enPassantCaptureSquare < 64U;
    const bool epPairAbsent =
        position.enPassantTargetSquare == CHESS_INTERACTION_INVALID_SQUARE &&
        position.enPassantCaptureSquare == CHESS_INTERACTION_INVALID_SQUARE;
    if (!epPairValid && !epPairAbsent) {
        outReport.Add("interaction_en_passant_metadata");
    }
    if (model.phase.tag != ChessInteractionPhase::AwaitingAction &&
        model.phase.pendingActionToken != 0U) {
        outReport.Add("interaction_unexpected_action_token");
    }
}

} // namespace runtime
