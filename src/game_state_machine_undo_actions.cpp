#include "runtime/game_state_machine.hpp"

#include "runtime/game_state_machine_castling.hpp"

namespace runtime {

namespace {

modules::PersistedSide OppositePersistedSide(const modules::PersistedSide side) {
    switch (side) {
        case modules::PersistedSide::White:
            return modules::PersistedSide::Black;
        case modules::PersistedSide::Black:
            return modules::PersistedSide::White;
        case modules::PersistedSide::Unknown:
        default:
            return modules::PersistedSide::Unknown;
    }
}

modules::PersistedSide PersistedSideFromPosition(
    const ChessPositionState& position) {
    if (!position.sideToMoveValid) {
        return modules::PersistedSide::Unknown;
    }
    return position.sideToMoveBlack ? modules::PersistedSide::Black
                                    : modules::PersistedSide::White;
}

} // namespace

bool GameStateMachine::BeginUndoPrompt(
    const uint8_t fromSquare,
    const uint8_t toSquare,
    const modules::PersistedSide moverSidePersist,
    const char movedPieceFen,
    const char capturedPieceFen,
    const uint8_t capturedSquare) {
    m_renderState.moveTransition = MoveRenderTransition{};
    if (fromSquare >= 64U || toSquare >= 64U || IsUndoPromptActive()) {
        return false;
    }

    const PhysicalTransitionPlan pendingForwardPlan =
        m_chessInteractionModel.phase.plan;
    const bool replacesPendingPromotion =
        m_chessInteractionModel.phase.tag == ChessInteractionPhase::AwaitingAction &&
        pendingForwardPlan.source == PhysicalTransitionSource::Human &&
        pendingForwardPlan.kind == PhysicalTransitionKind::Promotion &&
        pendingForwardPlan.moveFrom == fromSquare &&
        pendingForwardPlan.moveTo == toSquare;
    if (m_chessInteractionModel.phase.tag != ChessInteractionPhase::Idle &&
        !replacesPendingPromotion) {
        return false;
    }

    PhysicalTransitionPlan undoPlan;
    undoPlan.kind = PhysicalTransitionKind::Undo;
    undoPlan.source = PhysicalTransitionSource::Undo;
    undoPlan.beforeOccupiedMask =
        m_chessInteractionModel.acknowledged.occupiedMask;
    undoPlan.moveFrom = fromSquare;
    undoPlan.moveTo = toSquare;
    undoPlan.relocationCount = 1U;
    undoPlan.relocations[0] = {toSquare, fromSquare};

    uint8_t rookFrom = 0;
    uint8_t rookTo = 0;
    const bool kingMoved = movedPieceFen == 'K' || movedPieceFen == 'k' ||
        (movedPieceFen == '\0' &&
         (m_chessInteractionModel.acknowledged.kingMask & (1ULL << toSquare)) != 0ULL);
    if (kingMoved && TryGetCastlingRookMoveForKingMove(fromSquare, toSquare,
                                          rookFrom, rookTo)) {
        undoPlan.relocationCount = 2U;
        undoPlan.relocations[1] = {rookTo, rookFrom};
    }
    undoPlan.afterOccupiedMask = undoPlan.beforeOccupiedMask;
    for (uint8_t i = 0U; i < undoPlan.relocationCount; ++i) {
        const auto& relocation = undoPlan.relocations[i];
        undoPlan.afterOccupiedMask =
            (undoPlan.afterOccupiedMask | (1ULL << relocation.to)) &
            ~(1ULL << relocation.from);
    }

    const bool restoreCapture =
        capturedPieceFen != '\0' && capturedSquare < 64U;
    if (restoreCapture) {
        undoPlan.restorationRequired = true;
        undoPlan.restorationSquare = capturedSquare;
        undoPlan.restorationPieceFen = capturedPieceFen;
        undoPlan.afterOccupiedMask |= (1ULL << capturedSquare);
    } else if (replacesPendingPromotion && pendingForwardPlan.removalRequired) {
        undoPlan.restorationRequired = true;
        undoPlan.restorationSquare = pendingForwardPlan.removalSquare;
        undoPlan.afterOccupiedMask |= (1ULL << pendingForwardPlan.removalSquare);
    }

    ReduceInteraction(ChessInteractionEvent::MakePlanRequested(undoPlan));
    if (!IsUndoPromptActive()) {
        return false;
    }

    m_renderState.promotionConfirmation = {};
    m_undoPromptStartMs = fxe::Millis();
    m_undoPromptMetadataConfigured = true;
    m_undoPromptMoverSidePersist = moverSidePersist;
    m_undoPromptMoverPieceFen = movedPieceFen;
    if (moverSidePersist != modules::PersistedSide::Unknown) {
        m_visualActiveSideOverridePersist = moverSidePersist;
        m_statusEdgeActiveSideOverridePersist = moverSidePersist;
    } else {
        const modules::PersistedSide expectedSideToMove =
            PersistedSideFromPosition(m_chessInteractionModel.acknowledged);
        const modules::PersistedSide inferredMover =
            OppositePersistedSide(expectedSideToMove);
        m_undoPromptMoverSidePersist = inferredMover;
        m_visualActiveSideOverridePersist = inferredMover;
        m_statusEdgeActiveSideOverridePersist = inferredMover;
    }

    m_settleTimer.Stop();
    m_cpuExecution.moveSettleActive = false;
    m_humanSettleActive = false;
    m_renderState.settleSquare = INVALID_SQUARE;
    ClearValidDestMask();
    return true;
}

bool GameStateMachine::BeginPendingPromotionUndo() {
    const PhysicalTransitionPlan forward = m_chessInteractionModel.phase.plan;
    if (!IsPromotionActive() ||
        m_chessInteractionModel.phase.tag != ChessInteractionPhase::AwaitingAction ||
        forward.source != PhysicalTransitionSource::Human ||
        forward.kind != PhysicalTransitionKind::Promotion ||
        forward.moveFrom >= 64U || forward.moveTo >= 64U) {
        return false;
    }

    m_promotionUx.Cancel();
    m_promotionChoice = modules::PromotionChoice{
        modules::PromotionPiece::Queen, true, false};
    m_promotionChoicePending = true;

    const char pendingRestore = forward.removalRequired ? '?' : '\0';
    if (!BeginUndoPrompt(forward.moveFrom,
                         forward.moveTo,
                         modules::PersistedSide::Unknown,
                         '\0',
                         pendingRestore,
                         forward.removalSquare)) {
        return false;
    }
    m_undoPromptMetadataConfigured = false;
    return true;
}

void GameStateMachine::CancelUndoPrompt(const bool restoreForwardSide) {
    if (!IsUndoPromptActive()) {
        return;
    }

    const PhysicalTransitionPlan undo = m_chessInteractionModel.phase.plan;
    ReduceInteraction(ChessInteractionEvent::MakePlanCanceled());
    m_undoPromptStartMs = 0U;
    m_undoPromptMetadataConfigured = true;
    m_undoPromptMoverSidePersist = modules::PersistedSide::Unknown;
    m_undoPromptMoverPieceFen = '\0';
    if (restoreForwardSide) {
        m_renderState.moveTransition = MoveRenderTransition{
            MoveRenderTransitionKind::CancelUndo,
            undo.moveFrom,
            undo.moveTo,
            undo.relocationCount == 2U};
        if (m_visualActiveSideOverridePersist != modules::PersistedSide::Unknown) {
            m_visualActiveSideOverridePersist =
                OppositePersistedSide(m_visualActiveSideOverridePersist);
        }
        if (m_statusEdgeActiveSideOverridePersist != modules::PersistedSide::Unknown) {
            m_statusEdgeActiveSideOverridePersist =
                OppositePersistedSide(m_statusEdgeActiveSideOverridePersist);
        }
    } else {
        m_renderState.moveTransition = MoveRenderTransition{};
        m_visualActiveSideOverridePersist = modules::PersistedSide::Unknown;
        m_statusEdgeActiveSideOverridePersist = modules::PersistedSide::Unknown;
    }
    m_checkCacheRefreshRequested = true;
}

void GameStateMachine::ReconcileUndoOccupancy() {
    ClearMoveRenderTransition();
    if (m_chessInteractionModel.phase.tag != ChessInteractionPhase::Idle ||
        !m_chessInteractionModel.board.valid) {
        return;
    }
    EnterChessInteractionCorrection(
        m_chessInteractionModel,
        m_chessInteractionModel.board.occupiedMask ^
            m_chessInteractionModel.acknowledged.occupiedMask);
    (void)ProjectInteractionCorrection();
}

void GameStateMachine::ClearMoveRenderTransition() {
    m_renderState.moveTransition = MoveRenderTransition{};
}

void GameStateMachine::CancelMovePreviewForUndo() {
    CancelCurrentMove();
}

bool GameStateMachine::CancelCompletedSettleForUndo() {
    if (m_chessInteractionModel.phase.tag != ChessInteractionPhase::Idle ||
        !IsMoveSettlingActive()) {
        return false;
    }
    CancelCurrentMove();
    return true;
}

bool GameStateMachine::CancelUnclaimedMoveActionForUndo() {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    if (!IsMoveActionAwaitingResult() ||
        phase.pendingActionToken == m_claimedInteractionActionToken) {
        return false;
    }
    CancelCurrentMove();
    ReduceInteraction(ChessInteractionEvent::MakePlanCanceled());
    EnterChessInteractionCorrection(
        m_chessInteractionModel,
        m_chessInteractionModel.board.occupiedMask ^
            m_chessInteractionModel.acknowledged.occupiedMask);
    m_claimedInteractionActionToken = 0U;
    m_cpuExecution.illegalMask = 0ULL;
    (void)ProjectInteractionCorrection();
    return true;
}

} // namespace runtime
