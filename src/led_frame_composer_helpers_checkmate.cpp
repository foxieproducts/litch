#include "modules/led_frame_composer_internal.hpp"

#include <array>

namespace modules::led_frame_composer {

namespace {

constexpr bool IsWhitePieceLocal(const char c) {
    return (c >= 'A' && c <= 'Z');
}

constexpr bool IsBlackPieceLocal(const char c) {
    return (c >= 'a' && c <= 'z');
}

constexpr bool InBoundsLocal(const int r, const int f) {
    return (r >= 0 && r < 8 && f >= 0 && f < 8);
}

bool PieceAttacksSquareLocal(const std::array<char, 64>& pieces,
                             const uint8_t fromSq,
                             const uint8_t targetSq) {
    if (fromSq >= 64U || targetSq >= 64U) {
        return false;
    }

    const char pc = pieces[fromSq];
    if (pc == '\0') {
        return false;
    }

    const int fr = static_cast<int>(fromSq / 8U);
    const int ff = static_cast<int>(fromSq % 8U);
    const int tr = static_cast<int>(targetSq / 8U);
    const int tf = static_cast<int>(targetSq % 8U);

    const int dr = tr - fr;
    const int df = tf - ff;

    const auto stepSlide = [&](const int stepR, const int stepF) -> bool {
        int r = fr + stepR;
        int f = ff + stepF;
        while (InBoundsLocal(r, f)) {
            const uint8_t sq = static_cast<uint8_t>(r * 8 + f);
            if (sq == targetSq) {
                return true;
            }
            if (pieces[sq] != '\0') {
                return false;
            }
            r += stepR;
            f += stepF;
        }
        return false;
    };

    const char up = (pc >= 'a' && pc <= 'z') ? static_cast<char>(pc - 'a' + 'A') : pc;
    switch (up) {
        case 'P': {
            const bool isWhite = IsWhitePieceLocal(pc);
            const int pawnDr = isWhite ? -1 : 1;
            return (dr == pawnDr) && ((df == -1) || (df == 1));
        }
        case 'N': {
            const int adr = (dr < 0) ? -dr : dr;
            const int adf = (df < 0) ? -df : df;
            return (adr == 2 && adf == 1) || (adr == 1 && adf == 2);
        }
        case 'B': {
            const int adr = (dr < 0) ? -dr : dr;
            const int adf = (df < 0) ? -df : df;
            if (adr == 0 || adr != adf) {
                return false;
            }
            return stepSlide((dr > 0) ? 1 : -1, (df > 0) ? 1 : -1);
        }
        case 'R': {
            if (dr != 0 && df != 0) {
                return false;
            }
            return stepSlide((dr == 0) ? 0 : ((dr > 0) ? 1 : -1),
                             (df == 0) ? 0 : ((df > 0) ? 1 : -1));
        }
        case 'Q': {
            if (dr == 0 || df == 0) {
                return stepSlide((dr == 0) ? 0 : ((dr > 0) ? 1 : -1),
                                 (df == 0) ? 0 : ((df > 0) ? 1 : -1));
            }
            const int adr = (dr < 0) ? -dr : dr;
            const int adf = (df < 0) ? -df : df;
            if (adr == 0 || adr != adf) {
                return false;
            }
            return stepSlide((dr > 0) ? 1 : -1, (df > 0) ? 1 : -1);
        }
        case 'K': {
            const int adr = (dr < 0) ? -dr : dr;
            const int adf = (df < 0) ? -df : df;
            return (adr <= 1) && (adf <= 1) && !(adr == 0 && adf == 0);
        }
        default:
            return false;
    }
}

} // namespace

bool TryComputeCheckmateContributorsMask(
                                         const runtime::InteractionModelSnapshot& snapshot,
                                         uint64_t& outContributorsMask64) {
    outContributorsMask64 = 0;
    const auto& game = snapshot.game;
    if (!game.inCheckmate || !game.threatenedKingSquareValid ||
        game.threatenedKingSquare >= 64U || !snapshot.board.enginePiecesValid) {
        return false;
    }

    const bool winnerIsBlack = (game.sideToMove == modules::SideToMove::White);
    const auto& pieces = snapshot.board.enginePieces;

    const uint8_t kingSq = game.threatenedKingSquare;
    const int kingRank = static_cast<int>(kingSq / 8U);
    const int kingFile = static_cast<int>(kingSq % 8U);

    const auto isWinnerPiece = [&](const char pc) -> bool {
        return winnerIsBlack ? IsBlackPieceLocal(pc) : IsWhitePieceLocal(pc);
    };

    const auto isLoserPiece = [&](const char pc) -> bool {
        return winnerIsBlack ? IsWhitePieceLocal(pc) : IsBlackPieceLocal(pc);
    };

    for (uint8_t sq = 0; sq < 64; ++sq) {
        const char pc = pieces[sq];
        if (pc != '\0' && isWinnerPiece(pc) && PieceAttacksSquareLocal(pieces, sq, kingSq)) {
            outContributorsMask64 |= (1ULL << sq);
        }
    }

    for (int dr = -1; dr <= 1; ++dr) {
        for (int df = -1; df <= 1; ++df) {
            if (dr == 0 && df == 0) {
                continue;
            }

            const int rr = kingRank + dr;
            const int ff = kingFile + df;
            if (!InBoundsLocal(rr, ff)) {
                continue;
            }

            const uint8_t escapeSq = static_cast<uint8_t>(rr * 8 + ff);
            const char escapePiece = pieces[escapeSq];
            if (escapePiece != '\0' && isLoserPiece(escapePiece)) {
                continue;
            }

            if (escapePiece != '\0' && isWinnerPiece(escapePiece)) {
                outContributorsMask64 |= (1ULL << escapeSq);
            }

            for (uint8_t sq = 0; sq < 64; ++sq) {
                const char pc = pieces[sq];
                if (pc != '\0' && isWinnerPiece(pc) && PieceAttacksSquareLocal(pieces, sq, escapeSq)) {
                    outContributorsMask64 |= (1ULL << sq);
                }
            }
        }
    }

    return outContributorsMask64 != 0;
}

} // namespace modules::led_frame_composer
