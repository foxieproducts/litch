#include "runtime/game_state_machine.hpp"

#include "runtime/chess_interaction_duck_projection.hpp"

namespace runtime {

bool GameStateMachine::IsPhysicalOccupancyConsistentWithInteraction() const {
    return ProjectHumanChessOccupancy(
               m_chessInteractionModel.acknowledged,
               m_chessInteractionModel.phase,
               m_chessInteractionModel.board.occupiedMask) ==
           ProjectPhysicalOccupancyForInteractionProgress(
               m_chessInteractionModel.acknowledged,
               m_chessInteractionModel.phase);
}

bool GameStateMachine::IsMoveSettlingActive() const {
    return m_cpuExecution.moveSettleActive || m_humanSettleActive ||
           m_chessInteractionModel.phase.tag == ChessInteractionPhase::Settling;
}

bool GameStateMachine::IsHumanPieceInHand() const {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    const PhysicalTransitionPlan* castlingPlan =
        GetActiveHumanCastlingPlan();
    const bool rookStillOnOrigin =
        castlingPlan != nullptr && castlingPlan->relocationCount == 2U &&
        castlingPlan->relocations[1].from < 64U &&
        (m_chessInteractionModel.board.occupiedMask &
         (1ULL << castlingPlan->relocations[1].from)) != 0ULL;
    const bool waitingForCastlingRookLift =
        castlingPlan != nullptr &&
        (castlingPlan->progress.relocationsPlacedMask & 0x01U) != 0U &&
        (castlingPlan->progress.relocationsPlacedMask & 0x02U) == 0U &&
        rookStillOnOrigin;
    if (waitingForCastlingRookLift) {
        return false;
    }
    uint8_t deferredSource = CHESS_INTERACTION_INVALID_SQUARE;
    if (TryGetDeferredFollowupSource(deferredSource) ||
        phase.tag == ChessInteractionPhase::PieceLifted) {
        return true;
    }
    return phase.tag == ChessInteractionPhase::ExecutingPlan &&
           phase.plan.source == PhysicalTransitionSource::Human;
}

const char* GameStateMachine::DescribeInteractionState() const {
    if (m_promotionUx.IsActive()) {
        return "Promotion";
    }
    if (m_placementModeActive || m_kingQueenSwapPromptActive) {
        return "Placing";
    }
    if (IsMoveSettlingActive()) {
        return "Settling";
    }
    if (m_cpuExecution.queued &&
        IsCpuDirectedTransition(m_chessInteractionModel.phase.plan)) {
        return "CpuMoving";
    }
    return IsHumanPieceInHand() ? "PickedUp" : "Idle";
}

bool GameStateMachine::IsHumanMoveInProgress() const {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    return phase.tag == ChessInteractionPhase::PieceLifted ||
           phase.tag == ChessInteractionPhase::CaptureStaged ||
           ((phase.tag == ChessInteractionPhase::ExecutingPlan ||
             phase.tag == ChessInteractionPhase::Settling) &&
            phase.plan.source == PhysicalTransitionSource::Human);
}

bool GameStateMachine::TryGetHumanPieceInHandSquare(
    uint8_t& outSquare) const {
    if (!IsHumanPieceInHand()) {
        return false;
    }
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    if (TryGetDeferredFollowupSource(outSquare)) {
        return true;
    }
    if (phase.tag == ChessInteractionPhase::PieceLifted) {
        outSquare = phase.selectedSquare;
        return outSquare < 64U;
    }
    if (phase.tag == ChessInteractionPhase::ExecutingPlan &&
        phase.plan.kind == PhysicalTransitionKind::Castling &&
        phase.plan.relocationCount == 2U) {
        const bool secondary =
            phase.requiredAction == RequiredPhysicalAction::PlaceSecondary ||
            phase.requiredAction == RequiredPhysicalAction::LiftSecondary;
        outSquare = phase.plan.relocations[secondary ? 1U : 0U].from;
        return true;
    }
    outSquare = phase.plan.moveFrom;
    return outSquare < 64U;
}

bool GameStateMachine::TryGetDeferredFollowupSource(
    uint8_t& outSquare) const {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    if (phase.tag != ChessInteractionPhase::AwaitingAction ||
        (phase.plan.source != PhysicalTransitionSource::Human &&
         !IsCpuDirectedTransition(phase.plan))) {
        return false;
    }
    const ChessPositionState projected = ProjectChessPositionAfterPlan(
        m_chessInteractionModel.acknowledged, phase.plan);
    const uint64_t removedMover =
        ~m_chessInteractionModel.board.occupiedMask &
        projected.moverOccupiedMask;
    if (removedMover == 0ULL ||
        (removedMover & (removedMover - 1ULL)) != 0ULL) {
        return false;
    }
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((removedMover & (1ULL << square)) != 0ULL) {
            outSquare = square;
            return true;
        }
    }
    return false;
}

} // namespace runtime
