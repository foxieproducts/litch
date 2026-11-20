#include "modules/game_history_snapshot_reader.hpp"

#include <algorithm>
#include <cstring>

#include "modules/duck_game_persistence.hpp"

namespace modules::game_history {
namespace {

constexpr uint8_t SIDE_BLACK = 1U << 0U;
constexpr uint8_t CASTLE_WHITE_KING = 1U << 1U;
constexpr uint8_t CASTLE_WHITE_QUEEN = 1U << 2U;
constexpr uint8_t CASTLE_BLACK_KING = 1U << 3U;
constexpr uint8_t CASTLE_BLACK_QUEEN = 1U << 4U;

uint8_t GetPiece(const Position& position, const uint8_t square) {
    const uint8_t packed = position.bytes[square / 2U];
    return (square & 1U) == 0U
               ? static_cast<uint8_t>(packed & 0x0FU)
               : static_cast<uint8_t>(packed >> 4U);
}

void SetPiece(Position& position,
              const uint8_t square,
              const uint8_t piece) {
    uint8_t& packed = position.bytes[square / 2U];
    if ((square & 1U) == 0U) {
        packed = static_cast<uint8_t>((packed & 0xF0U) | piece);
    } else {
        packed = static_cast<uint8_t>((packed & 0x0FU) | (piece << 4U));
    }
}

uint8_t PieceCode(const uint8_t value) {
    switch (static_cast<char>(value)) {
        case 'P': return 1U;
        case 'N': return 2U;
        case 'B': return 3U;
        case 'R': return 4U;
        case 'Q': return 5U;
        case 'K': return 6U;
        case 'p': return 7U;
        case 'n': return 8U;
        case 'b': return 9U;
        case 'r': return 10U;
        case 'q': return 11U;
        case 'k': return 12U;
        default: return 0U;
    }
}

bool IsWhite(const uint8_t piece) {
    return piece >= 1U && piece <= 6U;
}

bool IsPawn(const uint8_t piece) {
    return piece == 1U || piece == 7U;
}

bool IsKing(const uint8_t piece) {
    return piece == 6U || piece == 12U;
}

bool IsRook(const uint8_t piece) {
    return piece == 4U || piece == 10U;
}

bool IsCapturedKing(const uint8_t piece) {
    return IsKing(piece);
}

Promotion DecodePromotion(const uint8_t suffix) {
    switch (static_cast<char>(suffix)) {
        case 'q': return Promotion::Queen;
        case 'r': return Promotion::Rook;
        case 'b': return Promotion::Bishop;
        case 'n': return Promotion::Knight;
        default: return Promotion::None;
    }
}

uint8_t PromotedPiece(const Promotion promotion, const bool white) {
    const uint8_t whiteCode = [&]() {
        switch (promotion) {
            case Promotion::Queen: return static_cast<uint8_t>(5U);
            case Promotion::Rook: return static_cast<uint8_t>(4U);
            case Promotion::Bishop: return static_cast<uint8_t>(3U);
            case Promotion::Knight: return static_cast<uint8_t>(2U);
            case Promotion::None: return static_cast<uint8_t>(0U);
        }
        return static_cast<uint8_t>(0U);
    }();
    return white || whiteCode == 0U
               ? whiteCode
               : static_cast<uint8_t>(whiteCode + 6U);
}

void ClearRookCastlingRight(Position& position,
                            const uint8_t rook,
                            const uint8_t square) {
    uint8_t flag = 0U;
    if (rook == 4U && square == 63U) flag = CASTLE_WHITE_KING;
    if (rook == 4U && square == 56U) flag = CASTLE_WHITE_QUEEN;
    if (rook == 10U && square == 7U) flag = CASTLE_BLACK_KING;
    if (rook == 10U && square == 0U) flag = CASTLE_BLACK_QUEEN;
    position.bytes[32U] = static_cast<uint8_t>(
        position.bytes[32U] & static_cast<uint8_t>(~flag));
}

bool CastleSquares(const uint8_t kingFrom,
                   const uint8_t kingTo,
                   uint8_t& outRookFrom,
                   uint8_t& outRookTo) {
    if (kingFrom == 60U && kingTo == 62U) {
        outRookFrom = 63U;
        outRookTo = 61U;
        return true;
    }
    if (kingFrom == 60U && kingTo == 58U) {
        outRookFrom = 56U;
        outRookTo = 59U;
        return true;
    }
    if (kingFrom == 4U && kingTo == 6U) {
        outRookFrom = 7U;
        outRookTo = 5U;
        return true;
    }
    if (kingFrom == 4U && kingTo == 2U) {
        outRookFrom = 0U;
        outRookTo = 3U;
        return true;
    }
    return false;
}

} // namespace

void GameHistorySnapshotReader::ResetReplay() {
    m_replayPosition = m_basePosition;
    m_replayNextIndex = 0U;
}

bool GameHistorySnapshotReader::Build(
    GameHistoryCaptureSnapshot& outSnapshot) {
    outSnapshot = {};
    if (!m_state.gameHistory.valid || !m_state.gameHistory.present ||
        m_state.gameHistory.IsCaptureExhausted() ||
        !IsValidGameId(m_state.gameHistory.gameId) ||
        m_state.gameHistory.lineRevision > MAX_LINE_REVISION ||
        (m_state.undoHistoryCount % UNDO_HISTORY_FIELDS_PER_MOVE) != 0U) {
        return false;
    }
    m_transitionCount = static_cast<uint16_t>(
        m_state.undoHistoryCount / UNDO_HISTORY_FIELDS_PER_MOVE);
    if (m_transitionCount > UNDO_HISTORY_MOVE_CAPACITY ||
        m_state.gameHistory.currentPly < m_transitionCount) {
        return false;
    }

    DuckGameDurableState variantState;
    if (!DecodeDuckGameDurableState(
            m_state.variantState.CStr(), variantState) ||
        variantState.variant == GameVariant::Undecided) {
        return false;
    }
    if (variantState.outcome.reason == VariantOutcomeReason::Resigned) {
        if (!m_state.gameHistory.shouldSeal ||
            m_state.gameHistory.outcome != Outcome::Resignation ||
            m_state.gameHistory.winner != (variantState.outcome.winner == GameWinner::White
                ? Winner::White : Winner::Black)) return false;
    } else if (m_state.gameHistory.outcome == Outcome::Resignation) {
        return false;
    }
    if (variantState.outcome.reason == VariantOutcomeReason::KingCaptured) {
        if (!m_state.gameHistory.shouldSeal ||
            m_state.gameHistory.outcome != Outcome::KingCaptured ||
            m_state.gameHistory.winner != (variantState.outcome.winner == GameWinner::White
                ? Winner::White : Winner::Black)) return false;
    } else if (m_state.gameHistory.outcome == Outcome::KingCaptured) {
        return false;
    }
    m_variant = variantState.variant == GameVariant::Duck
                    ? Variant::Duck
                    : Variant::Classic;
    if (variantState.variant == GameVariant::Duck) {
        if (!DecodeDuckUndoHistory(
                m_state.duckUndoState.CStr(), m_duckFrom.data(),
                m_duckTo.data(), m_duckFrom.size(), m_transitionCount)) {
            return false;
        }
    } else {
        if (!m_state.duckUndoState.Empty()) return false;
        m_duckFrom.fill(INVALID_SQUARE);
        m_duckTo.fill(INVALID_SQUARE);
    }

    uint8_t baseDuck = INVALID_SQUARE;
    if (m_variant == Variant::Duck) {
        baseDuck = m_transitionCount != 0U && m_duckFrom[0U] < 64U
                       ? m_duckFrom[0U]
                       : variantState.duckSquare;
    }
    const char* const baseFen = m_transitionCount == 0U
                                    ? m_state.fen.CStr()
                                    : m_state.undoBaseFen.CStr();
    if (!EncodePosition(baseFen, m_variant, baseDuck, m_basePosition)) {
        return false;
    }

    if (m_state.gameHistory.outcome == Outcome::KingCaptured &&
        (m_variant != Variant::Duck || m_transitionCount == 0U ||
         !IsCapturedKing(PieceCode(m_state.undoHistory[
             (m_transitionCount - 1U) * UNDO_HISTORY_FIELDS_PER_MOVE + 2U])))) {
        return false;
    }
    ResetReplay();
    GameHistoryCaptureStep step;
    for (uint16_t index = 0U; index < m_transitionCount; ++index) {
        if (!ReadStep(index, step)) return false;
    }
    Position finalPosition;
    if (!EncodePosition(m_state.fen.CStr(), m_variant,
                        variantState.duckSquare, finalPosition) ||
        !SamePositionAuthority(m_replayPosition, finalPosition)) {
        return false;
    }

    outSnapshot.gameId = m_state.gameHistory.gameId;
    outSnapshot.lineRevision = m_state.gameHistory.lineRevision;
    outSnapshot.variant = m_variant;
    outSnapshot.basePly = static_cast<uint16_t>(
        m_state.gameHistory.currentPly - m_transitionCount);
    outSnapshot.basePosition = m_basePosition;
    outSnapshot.transitionCount = m_transitionCount;
    outSnapshot.stepContext = this;
    outSnapshot.readStep = &ReadStepThunk;
    outSnapshot.shouldSeal = m_state.gameHistory.shouldSeal;
    outSnapshot.status = m_state.gameHistory.status;
    outSnapshot.outcome = m_state.gameHistory.outcome;
    outSnapshot.winner = m_state.gameHistory.winner;
    ResetReplay();
    return true;
}

bool GameHistorySnapshotReader::ReadStepThunk(
    void* const context,
    const uint16_t index,
    GameHistoryCaptureStep& outStep) {
    return context != nullptr &&
           static_cast<GameHistorySnapshotReader*>(context)->ReadStep(
               index, outStep);
}

bool GameHistorySnapshotReader::ReadStep(
    const uint16_t index,
    GameHistoryCaptureStep& outStep) {
    if (index >= m_transitionCount) {
        return false;
    }
    if (index < m_replayNextIndex) ResetReplay();
    Transition transition;
    while (m_replayNextIndex <= index) {
        if (!ApplyStep(m_replayNextIndex, m_replayPosition, transition)) {
            return false;
        }
        ++m_replayNextIndex;
    }
    outStep.transition = transition;
    outStep.afterPosition = m_replayPosition;
    return true;
}

bool GameHistorySnapshotReader::ApplyStep(
    const uint16_t index,
    Position& position,
    Transition& transition) const {
    const uint16_t offset = static_cast<uint16_t>(
        index * UNDO_HISTORY_FIELDS_PER_MOVE);
    const uint8_t* const fields = m_state.undoHistory + offset;
    const uint8_t side = fields[0U];
    const uint8_t recordedMover = PieceCode(fields[1U]);
    const uint8_t recordedCaptured = PieceCode(fields[2U]);
    const uint8_t captureSquare = fields[3U];
    const Promotion promotion = DecodePromotion(fields[4U]);
    const uint8_t from = fields[5U];
    const uint8_t to = fields[6U];
    if (side > 1U || from >= 64U || to >= 64U || from == to ||
        recordedMover == 0U ||
        ((position.bytes[32U] & SIDE_BLACK) != 0U) != (side == 1U) ||
        GetPiece(position, from) != recordedMover ||
        IsWhite(recordedMover) == (side == 1U)) {
        return false;
    }

    const bool hasCapture = fields[2U] != 0U;
    if (hasCapture != (captureSquare < 64U) ||
        (!hasCapture && captureSquare != INVALID_SQUARE) ||
        (hasCapture &&
         (recordedCaptured == 0U ||
          GetPiece(position, captureSquare) != recordedCaptured ||
          IsWhite(recordedCaptured) == IsWhite(recordedMover))) ||
        (!hasCapture && GetPiece(position, to) != 0U) ||
        (hasCapture && captureSquare != to &&
         (!IsPawn(recordedMover) || GetPiece(position, to) != 0U))) {
        return false;
    }

    if (hasCapture && captureSquare != to) {
        const uint8_t fromFile = from % 8U;
        const uint8_t toFile = to % 8U;
        const uint8_t fileDistance = fromFile > toFile ? fromFile - toFile : toFile - fromFile;
        if (recordedCaptured != (side == 0U ? 7U : 1U) ||
            position.bytes[33U] != to || from / 8U != (side == 0U ? 3U : 4U) ||
            to / 8U != (side == 0U ? 2U : 5U) || fileDistance != 1U ||
            captureSquare != (from / 8U) * 8U + toFile) {
            return false;
        }
    }

    const bool finalRank = to / 8U == (side == 0U ? 0U : 7U);
    if ((fields[4U] != 0U && promotion == Promotion::None) ||
        (promotion != Promotion::None &&
         (!IsPawn(recordedMover) || !finalRank)) ||
        (promotion == Promotion::None &&
         IsPawn(recordedMover) && finalRank)) {
        return false;
    }

    transition = {};
    transition.side = side == 0U ? Side::White : Side::Black;
    transition.from = from;
    transition.to = to;
    transition.captureSquare = hasCapture ? captureSquare : INVALID_SQUARE;
    transition.promotion = promotion;

    uint8_t rookFrom = INVALID_SQUARE;
    uint8_t rookTo = INVALID_SQUARE;
    const bool castle = IsKing(recordedMover) &&
                        CastleSquares(from, to, rookFrom, rookTo);
    if (castle) {
        const uint8_t rook = side == 0U ? 4U : 10U;
        if (hasCapture || GetPiece(position, rookFrom) != rook ||
            GetPiece(position, rookTo) != 0U ||
            GetPiece(position, to) != 0U) {
            return false;
        }
        transition.secondaryFrom = rookFrom;
        transition.secondaryTo = rookTo;
    }

    if (!AdvancePositionCounters(position, transition, position)) return false;

    if (recordedMover == 6U) {
        position.bytes[32U] = static_cast<uint8_t>(
            position.bytes[32U] &
            static_cast<uint8_t>(~(CASTLE_WHITE_KING |
                                   CASTLE_WHITE_QUEEN)));
    } else if (recordedMover == 12U) {
        position.bytes[32U] = static_cast<uint8_t>(
            position.bytes[32U] &
            static_cast<uint8_t>(~(CASTLE_BLACK_KING |
                                   CASTLE_BLACK_QUEEN)));
    } else if (IsRook(recordedMover)) {
        ClearRookCastlingRight(position, recordedMover, from);
    }
    if (hasCapture && IsRook(recordedCaptured)) {
        ClearRookCastlingRight(position, recordedCaptured, captureSquare);
    }

    SetPiece(position, from, 0U);
    if (hasCapture) SetPiece(position, captureSquare, 0U);
    if (castle) {
        SetPiece(position, rookTo, GetPiece(position, rookFrom));
        SetPiece(position, rookFrom, 0U);
    }
    SetPiece(position, to,
             promotion == Promotion::None
                 ? recordedMover
                 : PromotedPiece(promotion, side == 0U));

    if (m_variant == Variant::Duck) {
        const uint8_t duckFrom = m_duckFrom[index];
        const uint8_t duckTo = m_duckTo[index];
        const bool kingCapture = IsCapturedKing(recordedCaptured);
        if (to == position.bytes[42U]) {
            return false;
        }
        if (kingCapture) {
            const auto& history = m_state.gameHistory;
            if (index + 1U != m_transitionCount || captureSquare != to ||
                !history.shouldSeal || history.status != ArchiveStatus::Completed ||
                history.outcome != Outcome::KingCaptured ||
                history.winner != (side == 0U ? Winner::White : Winner::Black) ||
                duckFrom != INVALID_SQUARE || duckTo != INVALID_SQUARE) {
                return false;
            }
        } else if (duckFrom != position.bytes[42U] || duckTo >= 64U ||
                   duckFrom == duckTo || GetPiece(position, duckTo) != 0U) {
            return false;
        } else {
            position.bytes[42U] = duckTo;
            transition.duckFrom = duckFrom;
            transition.duckTo = duckTo;
        }
    } else if (IsCapturedKing(recordedCaptured)) {
        return false;
    }

    position.bytes[33U] = INVALID_SQUARE;
    const uint8_t distance = from > to ? from - to : to - from;
    if (IsPawn(recordedMover) && distance == 16U) {
        position.bytes[33U] = static_cast<uint8_t>(
            (static_cast<uint16_t>(from) + to) / 2U);
    }
    position.bytes[32U] ^= SIDE_BLACK;
    return true;
}

} // namespace modules::game_history
