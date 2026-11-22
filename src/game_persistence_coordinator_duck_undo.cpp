#include "modules/game_persistence_coordinator.hpp"

#include <cstring>

namespace modules {
namespace {

bool IsKingCapture(const uint8_t capturedPiece) {
    return capturedPiece == static_cast<uint8_t>('K') ||
           capturedPiece == static_cast<uint8_t>('k');
}

bool ValidateDuckUndoSemantics(
    const uint8_t* const chessFields,
    const uint8_t* const duckFrom,
    const uint8_t* const duckTo,
    const uint16_t moveCount,
    const uint8_t currentDuckSquare,
    const VariantOutcome outcome) {
    uint8_t previousDuckSquare = INVALID_DUCK_SQUARE;
    bool previousDuckValid = false;
    bool terminalKingCapture = false;
    for (uint16_t move = 0U; move < moveCount; ++move) {
        const uint16_t base = static_cast<uint16_t>(
            move * UNDO_HISTORY_FIELDS_PER_MOVE);
        const bool capturedKing = IsKingCapture(chessFields[base + 2U]);
        const bool pairAbsent = duckFrom[move] == INVALID_DUCK_SQUARE &&
                                duckTo[move] == INVALID_DUCK_SQUARE;
        if (pairAbsent != capturedKing ||
            (capturedKing && move + 1U != moveCount)) {
            return false;
        }
        if (pairAbsent) {
            terminalKingCapture = true;
            continue;
        }
        if (duckFrom[move] >= 64U || duckTo[move] >= 64U ||
            duckFrom[move] == duckTo[move] ||
            (previousDuckValid &&
             duckFrom[move] != previousDuckSquare)) {
            return false;
        }
        previousDuckSquare = duckTo[move];
        previousDuckValid = true;
    }
    if (previousDuckValid && previousDuckSquare != currentDuckSquare) {
        return false;
    }
    return terminalKingCapture ==
           (outcome.reason == VariantOutcomeReason::KingCaptured);
}

} // namespace

void GamePersistenceCoordinator::ClearDuckUndoHistory() {
    std::memset(m_duckUndoFrom, INVALID_DUCK_SQUARE,
                sizeof(m_duckUndoFrom));
    std::memset(m_duckUndoTo, INVALID_DUCK_SQUARE,
                sizeof(m_duckUndoTo));
    m_duckUndoMoveCount = 0U;
    m_cancelUndoDuckValid = false;
    m_cancelUndoDuckFrom = INVALID_DUCK_SQUARE;
    m_cancelUndoDuckTo = INVALID_DUCK_SQUARE;
    m_cancelUndoVariantOutcome = VariantOutcome{};
}

bool GamePersistenceCoordinator::EncodeDuckUndoState(
    GamePersistenceState& out,
    const uint16_t historyStartMove,
    const uint16_t keepMoves) const {
    if (!IsDuckGame()) {
        out.duckUndoState.Clear();
        return true;
    }
    if (historyStartMove > m_duckUndoMoveCount ||
        keepMoves > m_duckUndoMoveCount - historyStartMove) {
        return false;
    }
    char encoded[(UNDO_HISTORY_MOVE_CAPACITY * 4U) + 1U] = {};
    if (!EncodeDuckUndoHistory(
            &m_duckUndoFrom[historyStartMove],
            &m_duckUndoTo[historyStartMove],
            keepMoves, encoded, sizeof(encoded))) {
        return false;
    }
    return out.duckUndoState.Assign(encoded);
}

bool GamePersistenceCoordinator::RestoreDuckUndoState(
    const GamePersistenceState& persisted) {
    const uint16_t expectedMoves = static_cast<uint16_t>(
        m_undoHistoryFieldCount / UNDO_HISTORY_FIELDS_PER_MOVE);
    if (expectedMoves > UNDO_HISTORY_MOVE_CAPACITY) {
        return false;
    }
    ClearDuckUndoHistory();
    if (!IsDuckGame()) {
        if (!persisted.duckUndoState.Empty()) {
            return false;
        }
        m_duckUndoMoveCount = expectedMoves;
        return true;
    }
    if (persisted.duckUndoState.Empty()) {
        if (expectedMoves != 0U) {
            return false;
        }
        m_duckUndoMoveCount = expectedMoves;
        return true;
    }
    if (!DecodeDuckUndoHistory(
            persisted.duckUndoState.CStr(),
            m_duckUndoFrom, m_duckUndoTo,
            UNDO_HISTORY_MOVE_CAPACITY, expectedMoves)) {
        return false;
    }
    if (!ValidateDuckUndoSemantics(
            m_undoHistoryFields, m_duckUndoFrom, m_duckUndoTo,
            expectedMoves, m_duckSquare, m_variantOutcome)) {
        return false;
    }
    m_duckUndoMoveCount = expectedMoves;
    return true;
}

bool GamePersistenceCoordinator::TryPeekLastDuckUndo(
    uint8_t& outFrom,
    uint8_t& outTo) const {
    outFrom = INVALID_DUCK_SQUARE;
    outTo = INVALID_DUCK_SQUARE;
    if (m_duckUndoMoveCount == 0U) {
        return false;
    }
    const uint16_t last =
        static_cast<uint16_t>(m_duckUndoMoveCount - 1U);
    outFrom = m_duckUndoFrom[last];
    outTo = m_duckUndoTo[last];
    return outFrom < 64U && outTo < 64U && outFrom != outTo;
}

bool GamePersistenceCoordinator::BeginDuckUndoIfNeeded(
    const uint8_t moveCount,
    const UndoCommandOrigin origin) {
    if (!IsDuckGame() || moveCount == 0U ||
        m_duckUndoRelocationPending ||
        m_duckRedoRelocationPending) {
        return false;
    }
    uint8_t previousDuck = INVALID_DUCK_SQUARE;
    uint8_t currentDuck = INVALID_DUCK_SQUARE;
    if (!TryPeekLastDuckUndo(previousDuck, currentDuck)) {
        return false;
    }
    if (m_duckSquare != currentDuck ||
        !m_stateMachine.BeginDuckRelocation(
            currentDuck, previousDuck)) {
        return false;
    }
    m_duckUndoRelocationPending = true;
    m_duckUndoRequestedCount = moveCount;
    if (origin == UndoCommandOrigin::GraceTakeback) {
        m_takebackGrace.undoInProgress = true;
    }
    return true;
}

} // namespace modules
