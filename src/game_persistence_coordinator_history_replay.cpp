#include "modules/game_persistence_coordinator.hpp"

#include <litch_max/duck_chess_rules.hpp>
#include "modules/game_persistence_engine_helpers.hpp"

namespace modules {

bool GamePersistenceCoordinator::InitializeUndoHistoryReplayDuck(
    uint8_t& outDuckSquare) const {
    outDuckSquare = INVALID_DUCK_SQUARE;
    if (!IsDuckGame()) {
        return true;
    }
    const uint16_t moveCount = static_cast<uint16_t>(
        m_undoHistoryFieldCount / UNDO_HISTORY_FIELDS_PER_MOVE);
    if ((m_undoHistoryFieldCount % UNDO_HISTORY_FIELDS_PER_MOVE) != 0U ||
        moveCount != m_duckUndoMoveCount || m_duckSquare >= 64U) {
        return false;
    }
    outDuckSquare = moveCount != 0U && m_duckUndoFrom[0] < 64U
                        ? m_duckUndoFrom[0]
                        : m_duckSquare;
    return true;
}

bool GamePersistenceCoordinator::ApplyUndoHistoryReplayMove(
    const uint16_t fieldOffset,
    FenString& inOutFen,
    uint8_t& inOutDuckSquare) {
    if (fieldOffset + UNDO_HISTORY_FIELDS_PER_MOVE >
            m_undoHistoryFieldCount ||
        (fieldOffset % UNDO_HISTORY_FIELDS_PER_MOVE) != 0U) {
        return false;
    }
    const uint8_t promotedPiece = m_undoHistoryFields[fieldOffset + 4U];
    const uint8_t from = m_undoHistoryFields[fieldOffset + 5U];
    const uint8_t to = m_undoHistoryFields[fieldOffset + 6U];
    const char promotion = promotedPiece == 0U
                               ? '\0'
                               : static_cast<char>(promotedPiece);
    UciMove uci;
    if (!MakeUciMove(from, to, promotion, uci)) {
        return false;
    }
    if (!IsDuckGame()) {
        return m_engine.ApplyUciMove(uci.CStr()) &&
               game_persistence_engine_helpers::SafeGetFen(
                   m_engine, inOutFen, false);
    }

    const uint16_t moveIndex = static_cast<uint16_t>(
        fieldOffset / UNDO_HISTORY_FIELDS_PER_MOVE);
    if (moveIndex >= m_duckUndoMoveCount) {
        return false;
    }
    const uint8_t duckFrom = m_duckUndoFrom[moveIndex];
    const uint8_t duckTo = m_duckUndoTo[moveIndex];
    const bool duckRelocated = duckFrom < 64U && duckTo < 64U;
    const uint8_t capturedPiece = m_undoHistoryFields[fieldOffset + 2U];
    const bool capturedKing = capturedPiece == static_cast<uint8_t>('K') ||
                              capturedPiece == static_cast<uint8_t>('k');
    if (duckRelocated == capturedKing ||
        (duckRelocated &&
         (duckFrom != inOutDuckSquare || duckFrom == duckTo)) ||
        (!duckRelocated &&
         (duckFrom != INVALID_DUCK_SQUARE ||
          duckTo != INVALID_DUCK_SQUARE))) {
        return false;
    }
    litch_max::DuckChessMoveResult result;
    if (!litch_max::TryApplyDuckMove(
            inOutFen.CStr(), inOutDuckSquare, uci.CStr(), result) ||
        result.capturedKing != capturedKing ||
        !m_engine.SetFen(result.resultingFen.CStr())) {
        return false;
    }
    inOutFen = result.resultingFen;
    if (duckRelocated) {
        inOutDuckSquare = duckTo;
    }
    return true;
}

} // namespace modules
