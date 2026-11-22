#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include "modules/initial_fen.hpp"

namespace modules {

bool GamePersistenceCoordinator::RetireInitialDuckForSetup(
    const uint8_t replacementCandidate) {
    if (!QuiesceAllThinking()) {
        m_cancelAllThinkingRequested = true;
        return false;
    }
    // Retire Duck analysis and its physical projection together, including an
    // unfinished free opening relocation. The chess position is unchanged.
    if (!m_engine.SetFen(m_cachedFen.CStr())) {
        logger::Warn("game: initial duck removal position reload failed");
        return false;
    }
    const bool readyWasPresented = m_standardSetupWasComplete;
    ResetGameVariantForNewGame();
    // Reset publishes before the old expected Duck square is rebuilt. Keep
    // that retained animation snapshot under setup validation throughout.
    m_initialVariantValidationPending = true;
    m_standardSetupWasComplete = readyWasPresented;
    if (replacementCandidate < 64U) {
        m_setupDuckCandidateSquare = replacementCandidate;
    }
    m_stateMachine.ResetForExternalEngineMutation();
    ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
    m_initialVariantValidationPending = false;
    m_stateMachine.TriggerInitialKingPlacementPrompt();
    RecordVariantTraceCheckpoint();
    logger::Info("game: initial duck removed; variant=Undecided");
    (void)PersistSnapshotDeferredWithRetries("initial_duck_removed", false);
    return true;
}

bool GamePersistenceCoordinator::CanReturnEmptyBoardToSetup(
    const uint64_t occupiedMask) const {
    if (m_litchcraft.IsActive() || HasClockTerminalOutcome() || occupiedMask != 0ULL ||
        m_gameHasAcceptedMove || !m_cachedFen.Equals(INITIAL_FEN) ||
        m_stateMachine.GetMovedSquaresMask() != 0ULL || HasUndoHistory() ||
        m_pendingDuckTurn.valid || m_variantOutcome.IsTerminal() ||
        m_stateMachine.IsUndoPromptActive() || HasPendingUndoCommand() ||
        m_stateMachine.IsKingQueenSwapPromptActive() ||
        m_stateMachine.IsPromotionActive() ||
        (m_stateMachine.IsMoveActionAwaitingResult() &&
         !m_stateMachine.HasCommittedMovePending())) {
        return false;
    }
    return true;
}

bool GamePersistenceCoordinator::TryHandleEmptyBoardSetupObservation(
    const runtime::AppEvent& event, const uint64_t changedMask,
    const uint64_t occupiedMask) {
    if (!CanReturnEmptyBoardToSetup(occupiedMask)) return false;
    // Empty pregame input must be admitted before an unrelated observation
    // can confirm a settling first move. The existing modal observation owns
    // occupancy while cancellation waits for engine-worker quiescence.
    m_stateMachine.HandleGuidedBoardObservation(
        changedMask, occupiedMask, event.tMs, event.seq);
    (void)UpdateEmptyBoardSetup();
    return true;
}

bool GamePersistenceCoordinator::UpdateEmptyBoardSetup() {
    const auto& interaction = m_stateMachine.GetChessInteractionModel();
    if (!interaction.board.valid ||
        !CanReturnEmptyBoardToSetup(interaction.board.occupiedMask)) return true;
    if (m_stateMachine.IsPlacementModeActive() &&
        m_gameVariant == GameVariant::Undecided) {
        return true;
    }

    // Update() has already given an accepted Litchcraft session exclusive
    // ownership. Here an empty chess board abandons only pregame physical work;
    // it is not New Game and cannot refill a clock that started on the pickup.
    if (!QuiesceAllThinking()) return false;
    ClearPendingCpuMoveSelection();
    m_stateMachine.CancelQueuedCpuMove();
    m_hintPendingStart = false;
    ResetHintRetry();
    ClearCachedHintResults();
    ClearCachedLegalDestinations();
    m_stateMachine.SetHintDisplayRequested(false);
    m_stateMachine.ClearHintMove();
    const bool variantChanged = m_gameVariant != GameVariant::Undecided;
    const bool duckRetired = m_gameVariant == GameVariant::Duck;
    if (duckRetired) {
        if (!RetireInitialDuckForSetup(INVALID_DUCK_SQUARE)) return false;
    } else {
        m_gameVariant = GameVariant::Undecided;
        m_variantDetectionArmed = true;
        m_setupDuckCandidateSquare = INVALID_DUCK_SQUARE;
    }
    (void)m_stateMachine.ForceEnterPlacementMode();
    // A canceled settled first move may already have persisted its Classic
    // latch and interaction. Retire that record once; ordinary piece removal
    // and continued empty-board ticks need no document or flash mutation.
    if (variantChanged && !duckRetired) {
        (void)PersistSnapshotDeferredWithRetries("empty_initial_setup", false);
    }
    return true;
}

} // namespace modules
