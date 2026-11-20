#include "king_safety.hpp"

#include <initializer_list>

#include "modules/chess_strings.hpp"

namespace modules::king_safety {
namespace {

bool IsWhiteFenPieceLocal(const char piece) {
    return piece >= 'A' && piece <= 'Z';
}

bool IsFenPieceForSideLocal(const char piece, const modules::SideToMove side) {
    if (piece == '\0') {
        return false;
    }

    const bool pieceIsWhite = IsWhiteFenPieceLocal(piece);
    return side == modules::SideToMove::White ? pieceIsWhite : !pieceIsWhite;
}

uint64_t FindAttackers(const Lift& lift,
                       uint8_t& outAttackerSquare) {
    const auto [fen, removedSquare, kingSquare, moverSide] = lift;
    outAttackerSquare = 255;
    uint64_t attackers = 0ULL;
    const auto recordAttacker = [&](const uint8_t square) {
        attackers |= UINT64_C(1) << square;
        if (outAttackerSquare >= 64U) outAttackerSquare = square;
    };
    if (fen == nullptr || fen[0] == '\0' || kingSquare >= 64U) {
        return 0ULL;
    }

    char board[64] = {};
    for (uint8_t sq = 0; sq < 64U; ++sq) {
        char piece = '\0';
        if (modules::TryGetFenPieceAtSquare(fen, sq, piece)) {
            board[sq] = piece;
        }
    }

    if (removedSquare < 64U) {
        board[removedSquare] = '\0';
    }

    const modules::SideToMove attackerSide =
        moverSide == modules::SideToMove::White ? modules::SideToMove::Black : modules::SideToMove::White;
    const int kingRank = static_cast<int>(kingSquare / 8U);
    const int kingFile = static_cast<int>(kingSquare % 8U);

    const auto squareAt = [](const int rank, const int file) -> int {
        if (rank < 0 || rank >= 8 || file < 0 || file >= 8) {
            return -1;
        }
        return rank * 8 + file;
    };

    const auto matchesAttacker = [&](const int sq, const char whitePiece, const char blackPiece) -> bool {
        if (sq < 0 || sq >= 64) {
            return false;
        }

        const char piece = board[sq];
        if (!IsFenPieceForSideLocal(piece, attackerSide)) {
            return false;
        }

        return piece == whitePiece || piece == blackPiece;
    };

    const int pawnRank = kingRank + (attackerSide == modules::SideToMove::White ? 1 : -1);
    for (const int fileDelta : {-1, 1}) {
        const int attackerSq = squareAt(pawnRank, kingFile + fileDelta);
        if (matchesAttacker(attackerSq, 'P', 'p')) {
            recordAttacker(static_cast<uint8_t>(attackerSq));
        }
    }

    static constexpr int KNIGHT_DELTAS[8][2] = {
        {-2, -1},
        {-2, 1},
        {-1, -2},
        {-1, 2},
        {1, -2},
        {1, 2},
        {2, -1},
        {2, 1},
    };
    for (const auto& delta : KNIGHT_DELTAS) {
        const int attackerSq = squareAt(kingRank + delta[0], kingFile + delta[1]);
        if (matchesAttacker(attackerSq, 'N', 'n')) {
            recordAttacker(static_cast<uint8_t>(attackerSq));
        }
    }

    for (int rankDelta = -1; rankDelta <= 1; ++rankDelta) {
        for (int fileDelta = -1; fileDelta <= 1; ++fileDelta) {
            if (rankDelta == 0 && fileDelta == 0) {
                continue;
            }

            const int attackerSq = squareAt(kingRank + rankDelta, kingFile + fileDelta);
            if (matchesAttacker(attackerSq, 'K', 'k')) {
                recordAttacker(static_cast<uint8_t>(attackerSq));
            }
        }
    }

    static constexpr int BISHOP_DIRS[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    for (const auto& dir : BISHOP_DIRS) {
        int rank = kingRank + dir[0];
        int file = kingFile + dir[1];
        while (true) {
            const int attackerSq = squareAt(rank, file);
            if (attackerSq < 0) {
                break;
            }

            const char piece = board[attackerSq];
            if (piece != '\0') {
                if (IsFenPieceForSideLocal(piece, attackerSide) &&
                    (piece == 'B' || piece == 'b' || piece == 'Q' || piece == 'q')) {
                    recordAttacker(static_cast<uint8_t>(attackerSq));
                }
                break;
            }

            rank += dir[0];
            file += dir[1];
        }
    }

    static constexpr int ROOK_DIRS[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const auto& dir : ROOK_DIRS) {
        int rank = kingRank + dir[0];
        int file = kingFile + dir[1];
        while (true) {
            const int attackerSq = squareAt(rank, file);
            if (attackerSq < 0) {
                break;
            }

            const char piece = board[attackerSq];
            if (piece != '\0') {
                if (IsFenPieceForSideLocal(piece, attackerSide) &&
                    (piece == 'R' || piece == 'r' || piece == 'Q' || piece == 'q')) {
                    recordAttacker(static_cast<uint8_t>(attackerSq));
                }
                break;
            }

            rank += dir[0];
            file += dir[1];
        }
    }

    return attackers;
}

} // namespace

bool TryFindSelfCheckAttacker(const Lift& lift, uint8_t& outAttackerSquare) {
    return FindAttackers(lift, outAttackerSquare) != 0ULL;
}

uint64_t CheckingPiecesMask(const char* fen, const uint8_t kingSquare, const SideToMove side) {
    uint8_t firstAttacker = 255U;
    return FindAttackers({fen, 255U, kingSquare, side}, firstAttacker);
}

bool TryFindPinningPiece(const Lift& lift, uint8_t& outAttackerSquare) {
    outAttackerSquare = 255U;
    if (lift.fromSquare >= 64U || lift.kingSquare >= 64U ||
        lift.fromSquare == lift.kingSquare) {
        return false;
    }

    const int rankDelta = static_cast<int>(lift.fromSquare / 8U) - lift.kingSquare / 8;
    const int fileDelta = static_cast<int>(lift.fromSquare % 8U) - lift.kingSquare % 8;
    const int rankDistance = rankDelta < 0 ? -rankDelta : rankDelta;
    const int fileDistance = fileDelta < 0 ? -fileDelta : fileDelta;
    const bool diagonal = rankDistance == fileDistance;
    if (rankDelta != 0 && fileDelta != 0 && !diagonal) {
        return false;
    }

    char liftedPiece = '\0';
    if (!TryGetFenPieceAtSquare(lift.fen, lift.fromSquare, liftedPiece) ||
        !IsFenPieceForSideLocal(liftedPiece, lift.moverSide)) {
        return false;
    }

    const int rankStep = (rankDelta > 0) - (rankDelta < 0);
    const int fileStep = (fileDelta > 0) - (fileDelta < 0);
    int rank = lift.kingSquare / 8 + rankStep;
    int file = lift.kingSquare % 8 + fileStep;
    bool passedLift = false;
    while (rank >= 0 && rank < 8 && file >= 0 && file < 8) {
        const uint8_t square = static_cast<uint8_t>(rank * 8 + file);
        if (square == lift.fromSquare) {
            passedLift = true;
        } else {
            char piece = '\0';
            (void)TryGetFenPieceAtSquare(lift.fen, square, piece);
            if (piece != '\0') {
                const bool enemy = !IsFenPieceForSideLocal(piece, lift.moverSide);
                const bool slider = piece == 'Q' || piece == 'q' ||
                    (diagonal ? piece == 'B' || piece == 'b' : piece == 'R' || piece == 'r');
                if (passedLift && enemy && slider) {
                    outAttackerSquare = square;
                    return true;
                }
                return false;
            }
        }
        rank += rankStep;
        file += fileStep;
    }
    return false;
}

} // namespace modules::king_safety
