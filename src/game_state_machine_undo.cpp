#include "runtime/game_state_machine.hpp"

#include "runtime/game_state_machine_castling.hpp"

namespace runtime {

bool GameStateMachine::TryGetUndoPromptCastlingRookMove(uint8_t& outRookFrom, uint8_t& outRookTo) const {
    if (!IsUndoPromptActive()) {
        return false;
    }

    const PhysicalTransitionPlan& plan = m_chessInteractionModel.phase.plan;
    if (plan.relocationCount != 2U ||
        plan.relocations[1].from >= 64U || plan.relocations[1].to >= 64U) {
        return false;
    }

    outRookFrom = plan.relocations[1].to;
    outRookTo = plan.relocations[1].from;
    return true;
}

bool GameStateMachine::TryGetCastlingRookPromptMove(uint8_t& outRookFrom, uint8_t& outRookTo) const {
    if (!IsCastlingRookPromptActive()) {
        return false;
    }

    const PhysicalTransitionPlan* plan = GetActiveHumanCastlingPlan();
    if (plan == nullptr) {
        return false;
    }
    outRookFrom = plan->relocations[1].from;
    outRookTo = plan->relocations[1].to;
    return true;
}

void GameStateMachine::CancelCurrentMove() {
    m_settleTimer.Stop();
    m_activeSettleDurationMs = 0U;
    m_cpuExecution.moveSettleActive = false;
    m_cpuExecution.settleDestinationSuppressed = false;
    m_humanSettleActive = false;
    m_renderState.settleSquare = INVALID_SQUARE;
    m_interactionStartedFromMismatch = false;
    ClearValidDestMask();
    if (IsHumanMoveInProgress()) {
        ReduceInteraction(ChessInteractionEvent::MakePlanCanceled());
    }
}

void GameStateMachine::ConfigureUndoPromptMetadata(const modules::PersistedSide moverSidePersist,
                                                   const char movedPieceFen,
                                                   const char capturedPieceFen,
                                                   const uint8_t capturedSquare) {
    if (!IsUndoPromptActive()) {
        return;
    }

    // Prevent the undo prompt from auto-clearing before we tighten the target
    // occupancy with any required capture restore.
    m_undoPromptMetadataConfigured = true;

    m_undoPromptMoverSidePersist = moverSidePersist;
    m_undoPromptMoverPieceFen = movedPieceFen;

    if (capturedPieceFen == '\0' || capturedSquare >= 64U) {
        TryResolveUndoPromptIfSatisfied();
        return;
    }

    ReduceInteraction(
        ChessInteractionEvent::MakeUndoRestorationConfigured(
            capturedSquare, capturedPieceFen));

    TryResolveUndoPromptIfSatisfied();
}

bool GameStateMachine::IsUndoPromptAllowedRemoval(const uint8_t square) const {
    if (!IsUndoPromptActive() || square >= 64U) {
        return false;
    }

    if (square == m_chessInteractionModel.phase.plan.moveTo) {
        return true;
    }

    uint8_t rookFrom = 0;
    uint8_t rookTo = 0;
    return TryGetUndoPromptCastlingRookMove(rookFrom, rookTo) && square == rookTo;
}

bool GameStateMachine::IsUndoPromptAllowedPlacement(const uint8_t square) const {
    if (!IsUndoPromptActive() || square >= 64U) {
        return false;
    }

    if (square == m_chessInteractionModel.phase.plan.moveFrom) {
        return true;
    }

    uint8_t rookFrom = 0;
    uint8_t rookTo = 0;
    if (TryGetUndoPromptCastlingRookMove(rookFrom, rookTo) && square == rookFrom) {
        return true;
    }

    const PhysicalTransitionPlan& plan = m_chessInteractionModel.phase.plan;
    return plan.restorationRequired && square == plan.restorationSquare;
}

void GameStateMachine::HandleUndoPromptSensorChange(const uint8_t square,
                                                    const bool nowOccupied) {
    if (!IsUndoPromptActive() || square >= 64U) {
        TryResolveUndoPromptIfSatisfied();
        return;
    }

    const uint64_t squareMask = (1ULL << square);
    const bool targetOccupied =
        (m_chessInteractionModel.phase.plan.afterOccupiedMask & squareMask) != 0ULL;

    if ((m_humanIllegalMask & squareMask) != 0ULL && targetOccupied == nowOccupied) {
        m_humanIllegalMask &= ~squareMask;
        RestartIllegalFlashCadence();
        TryResolveUndoPromptIfSatisfied();
        return;
    }

    const bool allowed = nowOccupied ? IsUndoPromptAllowedPlacement(square)
                                     : IsUndoPromptAllowedRemoval(square);
    if (!allowed && targetOccupied != nowOccupied) {
        m_humanIllegalMask |= squareMask;
        RestartIllegalFlashCadence();
        return;
    }

    TryResolveUndoPromptIfSatisfied();
}

void GameStateMachine::TryResolveUndoPromptIfSatisfied() {
    if (!IsUndoPromptActive()) {
        m_undoPromptStartMs = 0U;
        m_undoPromptMetadataConfigured = true;
        m_undoPromptMoverSidePersist = modules::PersistedSide::Unknown;
        m_undoPromptMoverPieceFen = '\0';
        return;
    }

    // Prevent clearing before we have coordinator-provided metadata, which may
    // tighten the target occupancy with required capture restoration.
    if (!m_undoPromptMetadataConfigured) {
        return;
    }

    if (m_humanIllegalMask != 0ULL || m_cpuExecution.illegalMask != 0ULL) {
        return;
    }

    const PhysicalTransitionPlan& plan = m_chessInteractionModel.phase.plan;
    if (plan.moveFrom >= 64U || plan.moveTo >= 64U) {
        return;
    }

    const uint64_t moverFromMask = (1ULL << plan.moveFrom);
    const uint64_t moverToMask = (1ULL << plan.moveTo);
    const uint64_t physical = m_chessInteractionModel.board.occupiedMask;
    const bool restoreOnMoverToSquare = plan.restorationRequired &&
                                        plan.restorationSquare == plan.moveTo;
    const bool moverToSatisfied = restoreOnMoverToSquare
                                      ? ((physical & moverToMask) != 0ULL)
                                      : ((physical & moverToMask) == 0ULL);
    const bool moverReturned = ((physical & moverFromMask) != 0ULL) &&
                               moverToSatisfied;
    if (!moverReturned) {
        return;
    }

    // Castling undo requires the rook to return as part of satisfying the
    // prompt. Keep this local to the undo move rather than requiring full-board
    // occupancy equality.
    uint8_t rookFrom = 0;
    uint8_t rookTo = 0;
    if (TryGetUndoPromptCastlingRookMove(rookFrom, rookTo)) {
        const uint64_t rookFromMask = (1ULL << rookFrom);
        const uint64_t rookToMask = (1ULL << rookTo);
        const bool rookReturned = ((physical & rookFromMask) != 0ULL) &&
                                  ((physical & rookToMask) == 0ULL);
        if (!rookReturned) {
            return;
        }
    }

    if (plan.restorationRequired && plan.restorationSquare < 64U) {
        const uint64_t restoreMask = (1ULL << plan.restorationSquare);
        if ((physical & restoreMask) == 0ULL) {
            return;
        }
    }

    if (plan.progress.actionAcknowledged) {
        ReduceInteraction(ChessInteractionEvent::MakeUndoCompleted());
        TryResolveUndoPromptIfSatisfied();
    }
}

} // namespace runtime
