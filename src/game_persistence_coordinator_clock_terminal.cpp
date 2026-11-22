#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include <litch_max/duck_chess_rules.hpp>

namespace modules {

bool GamePersistenceCoordinator::SetClockTerminalPresentation(
    const std::string_view move, const FenString& afterFen) {
    const size_t piecesEnd = afterFen.View().find(' ');
    const size_t canonicalPiecesEnd = m_cachedFen.View().find(' ');
    if (piecesEnd == std::string_view::npos ||
        canonicalPiecesEnd == std::string_view::npos) {
        return false;
    }
    FenString presentation;
    // Only piece placement changed physically. Side-to-move, counters and
    // rights remain at the last complete turn, which is still GAME_FEN.
    if (!presentation.Assign(afterFen.View().substr(0U, piecesEnd)) ||
        !presentation.Append(m_cachedFen.View().substr(canonicalPiecesEnd)) ||
        !m_clockTerminalDuckChessMove.Assign(move)) {
        return false;
    }
    m_clockTerminalFen = presentation;
    return true;
}

bool GamePersistenceCoordinator::RestoreClockTerminalPresentation(
    const std::string_view move) {
    if (move.empty()) {
        return true;
    }
    fxe::FixedString<6> uci;
    litch_max::DuckChessMoveResult applied;
    if (!IsDuckGame() || !uci.Assign(move) ||
        !litch_max::TryApplyDuckMove(m_cachedFen.CStr(), m_duckSquare,
                          uci.CStr(), applied) || applied.capturedKing) {
        return false;
    }
    return SetClockTerminalPresentation(move, applied.resultingFen);
}

void GamePersistenceCoordinator::ClearClockTerminalOutcome() {
    ClearKingResignation();
    m_variantTerminalStartMs = 0U;
    m_clockTerminalWinner = GameWinner::None;
    m_clockTerminalStartMs = 0U;
    m_clockTerminalDuckChessMove.Clear();
    m_clockTerminalFen.Clear();
    m_terminalAbandonPending = false;
}

bool GamePersistenceCoordinator::ConsumePendingClockTimeout() {
    if (!m_gameClock.ConsumeTimeoutEvent()) {
        return false;
    }

    // A game result committed before the clock observation remains
    // authoritative. Otherwise zero immediately becomes the terminal owner.
    if (!IsGameOverVariantAware()) {
        LatchClockTerminalOutcome(m_gameClock.GetTimedOutSide());
    }
    return true;
}

bool GamePersistenceCoordinator::AbandonStagedWorkForTerminalOutcome() {
    // The clock result is authoritative immediately, but reducer repair and
    // engine-derived board guidance must wait until async search is completely
    // out of the shared engine lane. A later Update retries this transition.
    CancelPonder(true);
    if (!QuiesceAllThinking()) {
        return false;
    }
    m_hintPendingStart = false;
    m_hintLastSideToMoveValid = false;
    m_cancelAllThinkingRequested = false;
    m_cancelCpuWorkRequested = false;
    m_cancelHintWorkRequested = false;
    ResetHintRetry();
    ClearPendingCpuMoveSelection();
    ClearCachedHintResults();
    ClearCachedLegalDestinations();

    m_stateMachine.CancelQueuedCpuMove();
    m_stateMachine.SetHintDisplayRequested(false);
    m_stateMachine.ClearHintMove();

    m_promotionPending = false;
    m_pendingUndoCommand = PendingUndoCommand{};
    ClearTakebackGrace();

    // Retire executable work; the immutable terminal presentation was captured
    // before cleanup. It never advances canonical FEN, history or the clock.
    m_pendingDuckTurn = PendingDuckChessTurn{};
    m_stagedDuckChessUndoPending = false;
    m_duckTurnPhase = DuckTurnPhase::ChessMove;
    m_externalizedDuckChessActionPending = false;
    m_externalizedDuckRelocationActionPending = false;
    m_externalizedDuckUndoActionPending = false;
    m_externalizedDuckRedoActionPending = false;
    m_externalizedDuckFixedActionValid = false;
    m_externalizedDuckRedoStartPending = false;
    m_externalizedDuckFrom = INVALID_DUCK_SQUARE;
    m_externalizedDuckTo = INVALID_DUCK_SQUARE;

    m_stateMachine.ResetForExternalEngineMutation();
    ApplyEngineFenToStateMachineExpected(GetPresentationFen());
    return true;
}

void GamePersistenceCoordinator::LatchClockTerminalOutcome(
    const runtime::ClockSide losingSide) {
    if (HasClockTerminalOutcome()) {
        return;
    }
    m_clockTerminalWinner =
        losingSide == runtime::ClockSide::White ? GameWinner::Black
                                                : GameWinner::White;
    m_clockTerminalStartMs = fxe::Millis();
    if (m_pendingDuckTurn.valid && !m_pendingDuckTurn.capturedKing &&
        !SetClockTerminalPresentation(m_pendingDuckTurn.uci.View(),
                                      m_pendingDuckTurn.afterFen)) {
        logger::Warn("clock: terminal presentation unavailable");
    }
    m_terminalAbandonPending =
        !AbandonStagedWorkForTerminalOutcome();
}

} // namespace modules
