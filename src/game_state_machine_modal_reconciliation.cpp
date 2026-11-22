#include "runtime/game_state_machine.hpp"

#include "runtime/chess_interaction_duck_projection.hpp"

namespace runtime {

void GameStateMachine::HandleBlockedHumanSettleObservation(
    const ChessInteractionEvent& event,
    const bool restartConfirmationAfterCorrection) {
    ReduceInteraction(event);
    m_humanIllegalMask |= ProjectHumanChessOccupancy(
                             m_chessInteractionModel.acknowledged,
                             m_chessInteractionModel.phase, event.occupiedMask) ^
                         m_chessInteractionModel.phase.plan.afterOccupiedMask;
    RestartIllegalFlashCadence();
    if (restartConfirmationAfterCorrection) {
        m_settleTimer.Stop();
        m_activeSettleDurationMs = 0U;
        m_humanSettleActive = false;
        m_renderState.settleSquare = INVALID_SQUARE;
        ClearValidDestMask();
    }
    ReconcileHumanPlanAfterModal();
}

void GameStateMachine::ReconcileCpuPlanAfterModal() {
    if (!m_cpuExecution.queued || m_cpuExecution.illegalMask != 0ULL ||
        m_cpuExecution.tentativeCorrectionMask != 0ULL ||
        m_chessInteractionModel.phase.tag !=
            ChessInteractionPhase::ExecutingPlan ||
        !IsCpuDirectedTransition(m_chessInteractionModel.phase.plan)) {
        return;
    }
    ReduceInteraction(ChessInteractionEvent::MakePlanProgressObserved(
        m_chessInteractionModel.board.occupiedMask));
    (void)TryFinalizeCpuMove();
}

void GameStateMachine::ReconcileHumanPlanAfterModal() {
    const TaggedInteractionState& current = m_chessInteractionModel.phase;
    if (current.tag == ChessInteractionPhase::Settling &&
        current.plan.source == PhysicalTransitionSource::Human &&
        ((m_chessInteractionModel.board.occupiedMask ^
          current.plan.afterOccupiedMask) &
         GetLegalProgressPhysicalMask(current)) != 0ULL) {
        // A destination relift or source return still abandons the pending
        // move while another square needs correction. Restore the original
        // position in either order; an old confirmation timer cannot commit it.
        CancelCurrentMove();
        EnterChessInteractionCorrection(
            m_chessInteractionModel,
            m_chessInteractionModel.board.occupiedMask ^
                m_chessInteractionModel.acknowledged.occupiedMask);
        m_humanIllegalMask = m_chessInteractionModel.phase.correctionMask;
        m_captureReturnMask = 0ULL;
        RestartIllegalFlashCadence();
        return;
    }
    if (m_humanIllegalMask != 0ULL || m_captureReturnMask != 0ULL) {
        return;
    }
    if (current.tag == ChessInteractionPhase::Settling &&
        current.plan.source == PhysicalTransitionSource::Human &&
        !m_humanSettleActive) {
        // Correction can temporarily suspend an already-complete human plan.
        // Once the unrelated square is restored, resume the one shared
        // confirmation lifecycle instead of leaving the move uncommittable.
        StartSettling(current.plan.moveTo);
        return;
    }
    if (current.tag == ChessInteractionPhase::PieceLifted &&
        m_chessInteractionModel.board.valid &&
        current.selectedSquare < 64U &&
        (m_chessInteractionModel.board.occupiedMask &
         (1ULL << current.selectedSquare)) != 0ULL) {
        // The source can return before the extra square and capture victim.
        // Once feedback clears, cancel selection and guide any remaining
        // restoration instead of stranding a physically returned source.
        const uint8_t returnedSource = current.selectedSquare;
        const uint64_t remaining = m_chessInteractionModel.board.occupiedMask ^
            GetInteractionExpectedPosition().occupiedMask;
        CancelCurrentMove();
        if (remaining != 0ULL) {
            EnterChessInteractionCorrection(m_chessInteractionModel, remaining);
            (void)ProjectInteractionCorrection();
        } else {
            StartSettleAfterglow(m_renderState, returnedSource, true,
                                 INVALID_SQUARE);
        }
        return;
    }
    if (current.tag != ChessInteractionPhase::ExecutingPlan ||
        current.plan.source != PhysicalTransitionSource::Human) {
        return;
    }
    ReduceInteraction(ChessInteractionEvent::MakePlanProgressObserved(
        m_chessInteractionModel.board.occupiedMask));
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    if (phase.tag == ChessInteractionPhase::Settling) {
        StartSettling(phase.plan.moveTo);
    } else if (phase.tag == ChessInteractionPhase::AwaitingAction &&
               phase.plan.kind == PhysicalTransitionKind::Castling) {
        StartSettling(phase.plan.moveTo);
        CommitCastlingComposite();
    }
}

} // namespace runtime
