#include "modules/coach_insight.hpp"

#include <array>
#include <cstddef>

#include <litch_max/chess_notation.hpp>

namespace modules {
namespace {

struct ParsedCoachPosition {
    std::array<char, 64> board{};
    SideToMove sideToMove = SideToMove::White;
    uint8_t enPassantSquare = 64U;
};

bool IsFenPiece(const char piece) {
    switch (piece) {
        case 'P':
        case 'N':
        case 'B':
        case 'R':
        case 'Q':
        case 'K':
        case 'p':
        case 'n':
        case 'b':
        case 'r':
        case 'q':
        case 'k':
            return true;
        default:
            return false;
    }
}

bool IsWhitePiece(const char piece) {
    return piece >= 'A' && piece <= 'Z';
}

CoachPieceType ToCoachPieceType(const char piece) {
    const char lower = IsWhitePiece(piece)
                           ? static_cast<char>(piece - 'A' + 'a')
                           : piece;
    switch (lower) {
        case 'p':
            return CoachPieceType::Pawn;
        case 'n':
            return CoachPieceType::Knight;
        case 'b':
            return CoachPieceType::Bishop;
        case 'r':
            return CoachPieceType::Rook;
        case 'q':
            return CoachPieceType::Queen;
        case 'k':
            return CoachPieceType::King;
        default:
            return CoachPieceType::None;
    }
}

const char* SkipField(const char* cursor) {
    if (cursor == nullptr) {
        return nullptr;
    }
    while (*cursor == ' ') {
        ++cursor;
    }
    if (*cursor == '\0') {
        return nullptr;
    }
    while (*cursor != '\0' && *cursor != ' ') {
        ++cursor;
    }
    return cursor;
}

bool ParseCoachPosition(const char* fen, ParsedCoachPosition& out) {
    out = ParsedCoachPosition{};
    if (fen == nullptr || fen[0] == '\0') {
        return false;
    }

    uint8_t rank = 0U;
    uint8_t file = 0U;
    const char* cursor = fen;
    for (; *cursor != '\0' && *cursor != ' '; ++cursor) {
        if (*cursor == '/') {
            if (file != 8U || rank >= 7U) {
                return false;
            }
            ++rank;
            file = 0U;
        } else if (*cursor >= '1' && *cursor <= '8') {
            file = static_cast<uint8_t>(file +
                                        static_cast<uint8_t>(*cursor - '0'));
            if (file > 8U) {
                return false;
            }
        } else {
            if (!IsFenPiece(*cursor) || rank >= 8U || file >= 8U) {
                return false;
            }
            out.board[static_cast<size_t>(rank * 8U + file)] = *cursor;
            ++file;
        }
    }
    if (rank != 7U || file != 8U || *cursor != ' ') {
        return false;
    }

    while (*cursor == ' ') {
        ++cursor;
    }
    if (cursor[0] == 'w' && cursor[1] == ' ') {
        out.sideToMove = SideToMove::White;
    } else if (cursor[0] == 'b' && cursor[1] == ' ') {
        out.sideToMove = SideToMove::Black;
    } else {
        return false;
    }

    cursor = SkipField(cursor); // side
    cursor = SkipField(cursor); // castling
    if (cursor == nullptr) {
        return false;
    }
    while (*cursor == ' ') {
        ++cursor;
    }
    if (*cursor == '-') {
        if (cursor[1] != ' ') {
            return false;
        }
    } else if (!litch_max::TryParseUciSquare(cursor, out.enPassantSquare) ||
               cursor[2] != ' ') {
        return false;
    }

    cursor = SkipField(cursor); // en-passant
    cursor = SkipField(cursor); // halfmove
    cursor = SkipField(cursor); // fullmove
    if (cursor == nullptr) {
        return false;
    }
    while (*cursor == ' ') {
        ++cursor;
    }
    return *cursor == '\0';
}

bool IsPieceForSide(const char piece, const SideToMove side) {
    return IsFenPiece(piece) &&
           (IsWhitePiece(piece) == (side == SideToMove::White));
}

CoachPieceType PromotionPiece(const char suffix) {
    switch (litch_max::NormalizeUciPromotionSuffixLower(suffix)) {
        case 'q':
            return CoachPieceType::Queen;
        case 'r':
            return CoachPieceType::Rook;
        case 'b':
            return CoachPieceType::Bishop;
        case 'n':
            return CoachPieceType::Knight;
        default:
            return CoachPieceType::None;
    }
}

} // namespace

bool TryBuildCoachInsight(const char* fen,
                          const SideToMove moverSide,
                          const bool startsInCheck,
                          const ClassicThinkResult& result,
                          CoachInsight& outInsight) {
    outInsight = CoachInsight{};
    if (!result.bestScoreValid || result.candidateCount == 0U ||
        result.candidateCount > CLASSIC_THINK_CANDIDATE_CAPACITY ||
        (result.bestMoveFacts & ClassicThinkOutcomeKnown) == 0U ||
        ((result.bestMoveFacts & ClassicThinkGivesMate) != 0U &&
         (result.bestMoveFacts & ClassicThinkGivesCheck) == 0U)) {
        return false;
    }

    ParsedCoachPosition position;
    if (!ParseCoachPosition(fen, position) ||
        position.sideToMove != moverSide) {
        return false;
    }

    const ClassicThinkCandidate move = result.candidates[0];
    if (move.fromSquare >= 64U || move.toSquare >= 64U ||
        move.fromSquare == move.toSquare) {
        return false;
    }
    const char movingFenPiece = position.board[move.fromSquare];
    if (!IsPieceForSide(movingFenPiece, moverSide)) {
        return false;
    }
    const CoachPieceType movingPiece = ToCoachPieceType(movingFenPiece);

    outInsight.moverSide = moverSide;
    outInsight.fromSquare = move.fromSquare;
    outInsight.toSquare = move.toSquare;
    outInsight.movingPiece = movingPiece;

    const char destinationPiece = position.board[move.toSquare];
    if (IsPieceForSide(destinationPiece, moverSide)) {
        outInsight = CoachInsight{};
        return false;
    }
    if (IsFenPiece(destinationPiece)) {
        outInsight.moveFacts = static_cast<uint8_t>(
            outInsight.moveFacts | CoachMoveCaptures);
        outInsight.captureSquare = move.toSquare;
        outInsight.capturedPiece = ToCoachPieceType(destinationPiece);
    } else if (movingPiece == CoachPieceType::Pawn &&
               move.fromSquare % 8U != move.toSquare % 8U) {
        if (position.enPassantSquare != move.toSquare) {
            outInsight = CoachInsight{};
            return false;
        }
        const uint8_t captureSquare = static_cast<uint8_t>(
            (move.fromSquare / 8U) * 8U + (move.toSquare % 8U));
        const char capturedPawn = position.board[captureSquare];
        if (!IsFenPiece(capturedPawn) ||
            IsPieceForSide(capturedPawn, moverSide) ||
            ToCoachPieceType(capturedPawn) != CoachPieceType::Pawn) {
            outInsight = CoachInsight{};
            return false;
        }
        outInsight.moveFacts = static_cast<uint8_t>(
            outInsight.moveFacts | CoachMoveCaptures);
        outInsight.captureSquare = captureSquare;
        outInsight.capturedPiece = CoachPieceType::Pawn;
    }

    const bool promotionRank = move.toSquare < 8U || move.toSquare >= 56U;
    if (movingPiece == CoachPieceType::Pawn && promotionRank) {
        outInsight.promotionPiece = PromotionPiece(move.promotion);
        if (outInsight.promotionPiece == CoachPieceType::None) {
            outInsight = CoachInsight{};
            return false;
        }
        outInsight.moveFacts = static_cast<uint8_t>(
            outInsight.moveFacts | CoachMovePromotes);
    } else if (move.promotion != '\0') {
        outInsight = CoachInsight{};
        return false;
    }

    if (movingPiece == CoachPieceType::King &&
        move.fromSquare / 8U == move.toSquare / 8U) {
        const int fromFile = static_cast<int>(move.fromSquare % 8U);
        const int toFile = static_cast<int>(move.toSquare % 8U);
        if (toFile - fromFile == 2 || fromFile - toFile == 2) {
            outInsight.moveFacts = static_cast<uint8_t>(
                outInsight.moveFacts | CoachMoveCastles);
        }
    }
    if ((result.bestMoveFacts & ClassicThinkGivesCheck) != 0U) {
        outInsight.moveFacts = static_cast<uint8_t>(
            outInsight.moveFacts | CoachMoveGivesCheck);
    }
    if ((result.bestMoveFacts & ClassicThinkGivesMate) != 0U) {
        outInsight.moveFacts = static_cast<uint8_t>(
            outInsight.moveFacts | CoachMoveGivesMate);
    }

    if (startsInCheck) {
        outInsight.topic = CoachTopic::EscapeCheck;
        outInsight.priority = CoachPriority::Urgent;
    } else if ((outInsight.moveFacts & CoachMoveGivesMate) != 0U) {
        outInsight.topic = CoachTopic::MateInOne;
        outInsight.priority = CoachPriority::Urgent;
    } else if ((outInsight.moveFacts & CoachMovePromotes) != 0U) {
        outInsight.topic = CoachTopic::Promotion;
        outInsight.priority = CoachPriority::Tactical;
    } else if ((outInsight.moveFacts & CoachMoveCaptures) != 0U) {
        outInsight.topic = CoachTopic::Capture;
        outInsight.priority = CoachPriority::Tactical;
    } else if ((outInsight.moveFacts & CoachMoveGivesCheck) != 0U) {
        outInsight.topic = CoachTopic::GiveCheck;
        outInsight.priority = CoachPriority::Tactical;
    } else if ((outInsight.moveFacts & CoachMoveCastles) != 0U) {
        outInsight.topic = CoachTopic::Castle;
        outInsight.priority = CoachPriority::Positional;
    } else {
        outInsight.topic = CoachTopic::GeneralMove;
        outInsight.priority = CoachPriority::General;
    }
    return true;
}

} // namespace modules
