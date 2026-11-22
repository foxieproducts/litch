#include "modules/game_persistence_coordinator.hpp"

#include "modules/persisted_side.hpp"

namespace modules {

bool GamePersistenceCoordinator::BeginStagedDuckChessUndo() {
    if (m_duckTurnPhase != DuckTurnPhase::DuckRelocation ||
        !m_pendingDuckTurn.valid ||
        !m_stateMachine.CancelDuckRelocationForUndo()) {
        return false;
    }

    const PendingDuckChessTurn pending = m_pendingDuckTurn;
    const bool cpuTurn = runtime::IsCpuDirectedTransition(
        pending.source, pending.directedOrigin);
    if (!m_stateMachine.BeginUndoPrompt(
            pending.undoFields[5], pending.undoFields[6],
            PersistedSideFromU8(pending.undoFields[0]),
            static_cast<char>(pending.undoFields[1]),
            static_cast<char>(pending.undoFields[2]),
            pending.undoFields[3])) {
        (void)m_stateMachine.BeginDuckRelocation(
            m_duckSquare,
            cpuTurn ? pending.requiredDuckDestination
                    : runtime::CHESS_INTERACTION_INVALID_SQUARE,
            pending.source, pending.directedOrigin);
        return false;
    }

    // The CPU half has never entered history. Finish its physical inverse,
    // then undo the last committed human turn through the existing chain.
    // A CPU opening has no earlier human decision to rewind.
    m_chainedUndoRemaining = cpuTurn && HasUndoHistory() ? 1U : 0U;
    m_pendingDuckTurn = PendingDuckChessTurn{};
    m_duckTurnPhase = DuckTurnPhase::ChessMove;
    m_stagedDuckChessUndoPending = true;
    UpdateLastMovePulseFromUndoHistory();
    DispatchPendingInteractionActionResult(true);
    RecordVariantTraceCheckpoint();
    return true;
}

bool GamePersistenceCoordinator::BeginDuckCancelUndoRedoRelocation() {
    if (!m_cancelUndoDuckValid) {
        return false;
    }
    m_duckRedoRelocationPending = true;
    if (m_interactionEventsExternalized) {
        m_externalizedDuckRedoStartPending = true;
        return true;
    }
    return ResumeExternalizedDuckRedoAfterPositionAcknowledged();
}

bool GamePersistenceCoordinator::
    ResumeExternalizedDuckRedoAfterPositionAcknowledged() {
    if (!m_duckRedoRelocationPending || !m_cancelUndoDuckValid) {
        return true;
    }
    m_externalizedDuckRedoStartPending = false;
    const auto& model = m_stateMachine.GetChessInteractionModel();
    if (m_stateMachine.IsDuckRelocationActive() ||
        model.phase.tag != runtime::ChessInteractionPhase::Idle) {
        return true;
    }
    if (model.board.valid &&
        model.board.occupiedMask != model.acknowledged.occupiedMask) {
        // Cancel may arrive while the inverse chess piece is held. The
        // ordinary forward-position correction finishes before the fixed
        // Duck redo, and this existing owner stays pending throughout.
        m_stateMachine.ReconcileUndoOccupancy();
        return true;
    }
    if (!m_stateMachine.BeginDuckRelocation(
            m_cancelUndoDuckFrom, m_cancelUndoDuckTo)) {
        return false;
    }
    RecordVariantTraceCheckpoint();
    return true;
}

void GamePersistenceCoordinator::ContinueDuckUndoRelocationAction(
    const bool valid) {
    m_duckUndoRelocationPending = false;
    const UndoCommandOrigin origin =
        m_takebackGrace.undoInProgress
            ? UndoCommandOrigin::GraceTakeback
            : UndoCommandOrigin::Manual;
    const uint8_t requested = m_duckUndoRequestedCount;
    m_duckUndoRequestedCount = 0U;
    if (!valid || !BeginNextUndo(origin)) {
        m_chainedUndoRemaining = 0U;
        if (origin == UndoCommandOrigin::GraceTakeback) {
            ClearTakebackGrace();
        }
        ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
    } else {
        // One fixed Duck return belongs to one complete inverse turn. Keep
        // the rest of a VS-CPU decision-cycle request owned by the same chain.
        m_chainedUndoRemaining = requested == 0U ? 0U : static_cast<uint8_t>(requested - 1U);
    }
}

void GamePersistenceCoordinator::ContinueDuckRedoRelocationAction(
    const bool valid,
    const uint8_t duckTo) {
    if (!valid) {
        return;
    }
    m_duckSquare = duckTo;
    m_variantOutcome = m_cancelUndoVariantOutcome;
    m_duckRedoRelocationPending = false;
    m_externalizedDuckRedoStartPending = false;
    m_cancelUndoDuckValid = false;
    ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
    (void)PersistSnapshotDeferredWithRetries("duck_cancel_undo", false);
    RecordVariantTraceCheckpoint();
}

void GamePersistenceCoordinator::HandleDuckUndoRelocationAction(
    const runtime::PhysicalTransitionPlan& relocation) {
    const bool valid = relocation.moveFrom == m_duckSquare &&
                       relocation.moveTo < 64U &&
                       relocation.moveTo != relocation.moveFrom;
    DispatchPendingInteractionActionResult(valid);
    if (m_interactionEventsExternalized) {
        m_externalizedDuckUndoActionPending = true;
        m_externalizedDuckFixedActionValid = valid;
        m_externalizedDuckFrom = relocation.moveFrom;
        m_externalizedDuckTo = relocation.moveTo;
        return;
    }
    ContinueDuckUndoRelocationAction(valid);
}

void GamePersistenceCoordinator::HandleDuckRedoRelocationAction(
    const runtime::PhysicalTransitionPlan& relocation) {
    const bool valid = m_cancelUndoDuckValid &&
                       relocation.moveFrom == m_cancelUndoDuckFrom &&
                       relocation.moveTo == m_cancelUndoDuckTo;
    DispatchPendingInteractionActionResult(valid);
    if (m_interactionEventsExternalized) {
        m_externalizedDuckRedoActionPending = true;
        m_externalizedDuckFixedActionValid = valid;
        m_externalizedDuckFrom = relocation.moveFrom;
        m_externalizedDuckTo = relocation.moveTo;
        return;
    }
    ContinueDuckRedoRelocationAction(valid, relocation.moveTo);
}

} // namespace modules
