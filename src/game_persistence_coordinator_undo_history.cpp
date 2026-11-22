#include "modules/game_persistence_coordinator.hpp"

#include <cctype>
#include <cstring>

#include "logger.hpp"

namespace modules {

void GamePersistenceCoordinator::ClearUndoHistory() {
    ClearTakebackGrace();
    ++m_undoHistoryRevision;
    m_undoHistoryFieldCount = 0;
    m_lastUndoEntryPromotionPending = false;
    m_cancelUndoHistoryEntryValid = false;
    std::memset(m_undoHistoryFields, 0, sizeof(m_undoHistoryFields));
    std::memset(m_cancelUndoHistoryEntry, 0, sizeof(m_cancelUndoHistoryEntry));
    ClearDuckUndoHistory();
    m_pendingUndoCommand = PendingUndoCommand{};
    m_chainedUndoRemaining = 0U;
    m_stagedDuckChessUndoPending = false;
    if (m_stateMachine.IsUndoPromptActive()) {
        m_stateMachine.CancelUndoPrompt(false);
    }
}

bool GamePersistenceCoordinator::CanAppendUndoHistoryEntry() const {
    constexpr uint16_t STRIDE = UNDO_HISTORY_FIELDS_PER_MOVE;
    if ((m_undoHistoryFieldCount % STRIDE) != 0U) {
        return false;
    }
    const uint16_t moveCount = static_cast<uint16_t>(
        m_undoHistoryFieldCount / STRIDE);
    if (m_undoHistoryFieldCount <=
            (UNDO_HISTORY_FIELD_CAPACITY - STRIDE) &&
        m_duckUndoMoveCount < UNDO_HISTORY_MOVE_CAPACITY) {
        return m_duckUndoMoveCount == moveCount;
    }
    if (m_undoHistoryFieldCount != UNDO_HISTORY_FIELD_CAPACITY ||
        m_duckUndoMoveCount != UNDO_HISTORY_MOVE_CAPACITY ||
        m_fenHistorySize < 2U) {
        return false;
    }
    const uint8_t nextBaseIndex = static_cast<uint8_t>(
        (m_fenHistoryStart + 1U) % FEN_HISTORY_CAPACITY);
    FenString nextBase;
    return ReadFenHistory(nextBaseIndex, nextBase);
}

bool GamePersistenceCoordinator::EnsureUndoHistoryAppendCapacity() {
    constexpr uint16_t STRIDE = UNDO_HISTORY_FIELDS_PER_MOVE;
    if (!CanAppendUndoHistoryEntry()) {
        return false;
    }
    if (m_undoHistoryFieldCount <=
        (UNDO_HISTORY_FIELD_CAPACITY - STRIDE)) {
        return true;
    }

    const uint8_t nextBaseIndex = static_cast<uint8_t>(
        (m_fenHistoryStart + 1U) % FEN_HISTORY_CAPACITY);
    FenString nextBase;
    if (!ReadFenHistory(nextBaseIndex, nextBase)) {
        return false;
    }
    CopyFenWithCommittedFullmove(nextBase, 1U, m_undoBaseFen);
    m_undoBaseMovedSquaresMask = 0ULL;
    std::memmove(m_undoHistoryFields,
                 m_undoHistoryFields + STRIDE,
                 UNDO_HISTORY_FIELD_CAPACITY - STRIDE);
    std::memset(m_undoHistoryFields +
                    (UNDO_HISTORY_FIELD_CAPACITY - STRIDE),
                0, STRIDE);
    std::memmove(m_duckUndoFrom, m_duckUndoFrom + 1U,
                 UNDO_HISTORY_MOVE_CAPACITY - 1U);
    std::memmove(m_duckUndoTo, m_duckUndoTo + 1U,
                 UNDO_HISTORY_MOVE_CAPACITY - 1U);
    m_duckUndoFrom[UNDO_HISTORY_MOVE_CAPACITY - 1U] =
        INVALID_DUCK_SQUARE;
    m_duckUndoTo[UNDO_HISTORY_MOVE_CAPACITY - 1U] =
        INVALID_DUCK_SQUARE;
    m_undoHistoryFieldCount = static_cast<uint16_t>(
        UNDO_HISTORY_FIELD_CAPACITY - STRIDE);
    m_duckUndoMoveCount = static_cast<uint16_t>(
        UNDO_HISTORY_MOVE_CAPACITY - 1U);
    return true;
}

bool GamePersistenceCoordinator::AppendUndoHistoryEntry(uint8_t side,
                                                        uint8_t movedPiece,
                                                        uint8_t capturedPiece,
                                                        uint8_t capturedSquare,
                                                        uint8_t promotedPiece,
                                                        uint8_t from,
                                                        uint8_t to) {
    if (!EnsureUndoHistoryAppendCapacity() ||
        m_undoHistoryFieldCount >
            (UNDO_HISTORY_FIELD_CAPACITY - UNDO_HISTORY_FIELDS_PER_MOVE) ||
        m_duckUndoMoveCount >= UNDO_HISTORY_MOVE_CAPACITY) {
        return false;
    }

    const uint16_t base = m_undoHistoryFieldCount;
    m_undoHistoryFields[static_cast<uint16_t>(base + 0)] = side;
    m_undoHistoryFields[static_cast<uint16_t>(base + 1)] = movedPiece;
    m_undoHistoryFields[static_cast<uint16_t>(base + 2)] = capturedPiece;
    m_undoHistoryFields[static_cast<uint16_t>(base + 3)] = capturedSquare;
    m_undoHistoryFields[static_cast<uint16_t>(base + 4)] = promotedPiece;
    m_undoHistoryFields[static_cast<uint16_t>(base + 5)] = from;
    m_undoHistoryFields[static_cast<uint16_t>(base + 6)] = to;
    m_undoHistoryFieldCount =
        static_cast<uint16_t>(m_undoHistoryFieldCount + UNDO_HISTORY_FIELDS_PER_MOVE);
    m_duckUndoFrom[m_duckUndoMoveCount] = INVALID_DUCK_SQUARE;
    m_duckUndoTo[m_duckUndoMoveCount] = INVALID_DUCK_SQUARE;
    ++m_duckUndoMoveCount;
    ++m_undoHistoryRevision;
    RecordGameHistoryPlyMutation(true);
    return true;
}

bool GamePersistenceCoordinator::PopUndoHistoryEntry(
    uint8_t outFields[UNDO_HISTORY_FIELDS_PER_MOVE]) {
    if (outFields == nullptr) {
        return false;
    }
    if (m_undoHistoryFieldCount < UNDO_HISTORY_FIELDS_PER_MOVE) {
        return false;
    }

    m_cancelUndoDuckValid = false;
    if (m_duckUndoMoveCount != 0U) {
        const uint16_t lastDuck =
            static_cast<uint16_t>(m_duckUndoMoveCount - 1U);
        m_cancelUndoDuckFrom = m_duckUndoFrom[lastDuck];
        m_cancelUndoDuckTo = m_duckUndoTo[lastDuck];
        m_cancelUndoDuckValid =
            m_cancelUndoDuckFrom < 64U &&
            m_cancelUndoDuckTo < 64U;
        m_duckUndoFrom[lastDuck] = INVALID_DUCK_SQUARE;
        m_duckUndoTo[lastDuck] = INVALID_DUCK_SQUARE;
        m_duckUndoMoveCount = lastDuck;
    }

    const uint16_t base =
        static_cast<uint16_t>(m_undoHistoryFieldCount - UNDO_HISTORY_FIELDS_PER_MOVE);
    for (uint8_t i = 0; i < UNDO_HISTORY_FIELDS_PER_MOVE; ++i) {
        outFields[i] = m_undoHistoryFields[static_cast<uint16_t>(base + i)];
        m_undoHistoryFields[static_cast<uint16_t>(base + i)] = 0;
    }
    m_undoHistoryFieldCount = base;
    ++m_undoHistoryRevision;
    RecordGameHistoryPlyMutation(false);
    return true;
}

bool GamePersistenceCoordinator::UpdateLastUndoHistoryPromotionPiece(const uint8_t promotedPiece) {
    if (m_undoHistoryFieldCount < UNDO_HISTORY_FIELDS_PER_MOVE) {
        return false;
    }

    const uint16_t base =
        static_cast<uint16_t>(m_undoHistoryFieldCount - UNDO_HISTORY_FIELDS_PER_MOVE);
    m_undoHistoryFields[static_cast<uint16_t>(base + 4)] = promotedPiece;
    ++m_undoHistoryRevision;
    return true;
}

bool GamePersistenceCoordinator::RebuildEngineFromUndoState() {
    if (!m_engine.SetFen(m_undoBaseFen.CStr())) {
        logger::Warn("engine: SetFen(base) failed during undo rebuild");
        return false;
    }

    FenString replayFen = m_undoBaseFen;
    uint8_t replayDuckSquare = INVALID_DUCK_SQUARE;
    if (!InitializeUndoHistoryReplayDuck(replayDuckSquare)) {
        return false;
    }

    for (uint16_t i = 0;
         (i + UNDO_HISTORY_FIELDS_PER_MOVE) <= m_undoHistoryFieldCount;
         i = static_cast<uint16_t>(i + UNDO_HISTORY_FIELDS_PER_MOVE)) {
        if (!ApplyUndoHistoryReplayMove(
                i, replayFen, replayDuckSquare)) {
            logger::Warn("engine: history replay failed during undo rebuild");
            return false;
        }
    }

    m_cachedFen = replayFen;
    return !m_cachedFen.Empty();
}

uint16_t GamePersistenceCoordinator::ComputeHalfmoveClockPliesFromUndoHistory() const {
    // 50-move rule resets on any pawn move or capture.
    // We reconstruct from the coordinator-owned persisted undo schema.
    uint16_t halfmove = 0;

    for (uint16_t i = 0;
         (i + UNDO_HISTORY_FIELDS_PER_MOVE) <= m_undoHistoryFieldCount;
         i = static_cast<uint16_t>(i + UNDO_HISTORY_FIELDS_PER_MOVE)) {
        const char movedPiece = static_cast<char>(m_undoHistoryFields[static_cast<uint16_t>(i + 1)]);
        const char capturedPiece = static_cast<char>(m_undoHistoryFields[static_cast<uint16_t>(i + 2)]);

        const char movedLower = static_cast<char>(std::tolower(static_cast<unsigned char>(movedPiece)));
        const bool pawnMove = (movedLower == 'p');
        const bool capture = (capturedPiece != 0);

        if (pawnMove || capture) {
            halfmove = 0;
        } else {
            if (halfmove != 0xFFFFU) {
                ++halfmove;
            }
        }
    }

    return halfmove;
}

void GamePersistenceCoordinator::UpdateLastMovePulseFromUndoHistory() {
    m_stateMachine.ClearLastMoveForSide(modules::PersistedSide::White);
    m_stateMachine.ClearLastMoveForSide(modules::PersistedSide::Black);
    m_stateMachine.ClearLastMoveMostRecentSide();

    if (m_undoHistoryFieldCount < UNDO_HISTORY_FIELDS_PER_MOVE) {
        OverlayPendingDuckMovePulse();
        return;
    }

    static constexpr uint16_t STRIDE = UNDO_HISTORY_FIELDS_PER_MOVE;
    const uint16_t usable = static_cast<uint16_t>(m_undoHistoryFieldCount - (m_undoHistoryFieldCount % STRIDE));
    if (usable < STRIDE) {
        OverlayPendingDuckMovePulse();
        return;
    }

    bool haveWhite = false;
    bool haveBlack = false;
    bool mostRecentSet = false;

    for (int32_t base = static_cast<int32_t>(usable - STRIDE);
         base >= 0 && (!haveWhite || !haveBlack);
         base -= static_cast<int32_t>(STRIDE)) {
        const auto idx0 = static_cast<uint16_t>(base + 0);
        const auto idxFrom = static_cast<uint16_t>(base + 5);
        const auto idxTo = static_cast<uint16_t>(base + 6);

        const modules::PersistedSide side = modules::PersistedSideFromU8(m_undoHistoryFields[idx0]);
        if (side != modules::PersistedSide::White && side != modules::PersistedSide::Black) {
            continue;
        }

        if (!mostRecentSet) {
            m_stateMachine.SetLastMoveMostRecentSide(side);
            mostRecentSet = true;
        }

        const uint8_t fromSq = m_undoHistoryFields[idxFrom];
        const uint8_t toSq = m_undoHistoryFields[idxTo];

        if (side == modules::PersistedSide::White) {
            if (!haveWhite) {
                m_stateMachine.SetLastMoveForSide(side, fromSq, toSq);
                haveWhite = true;
            }
        } else {
            if (!haveBlack) {
                m_stateMachine.SetLastMoveForSide(side, fromSq, toSq);
                haveBlack = true;
            }
        }
    }
    OverlayPendingDuckMovePulse();
}

void GamePersistenceCoordinator::OverlayPendingDuckMovePulse() {
    if (!m_pendingDuckTurn.valid) {
        return;
    }
    const modules::PersistedSide side =
        modules::PersistedSideFromU8(m_pendingDuckTurn.undoFields[0]);
    const uint8_t from = m_pendingDuckTurn.undoFields[5];
    const uint8_t to = m_pendingDuckTurn.undoFields[6];
    if ((side != modules::PersistedSide::White &&
         side != modules::PersistedSide::Black) ||
        from >= 64U || to >= 64U) {
        return;
    }
    m_stateMachine.SetLastMoveForSide(side, from, to);
    m_stateMachine.SetLastMoveMostRecentSide(side);
}

} // namespace modules
