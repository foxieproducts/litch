#include "modules/engine_derived_state.hpp"
#include "king_safety.hpp"

#include <array>
#include <cstddef>
#include <cstring>

namespace modules {

namespace {

static constexpr size_t FEN_BUF_CAPACITY = 192;

bool FindKingSquareInFenPlacement(const char* fen,
                                  const SideToMove side,
                                  uint8_t& outSquare) {
    if (fen == nullptr || fen[0] == '\0') {
        return false;
    }

    const char kingChar = (side == SideToMove::White) ? 'K' : 'k';

    int rank = 0;
    int file = 0;
    for (const char* p = fen; *p != '\0' && *p != ' '; ++p) {
        const char c = *p;

        if (c == '/') {
            if (file != 8) {
                return false;
            }
            ++rank;
            file = 0;
            continue;
        }

        if (c >= '1' && c <= '8') {
            file += static_cast<int>(c - '0');
            if (file > 8) {
                return false;
            }
            continue;
        }

        const bool isPiece = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
        if (!isPiece) {
            return false;
        }

        if (rank < 0 || rank >= 8 || file < 0 || file >= 8) {
            return false;
        }

        if (c == kingChar) {
            outSquare = static_cast<uint8_t>(rank * 8 + file);
            return true;
        }

        ++file;
    }

    return false;
}

} // namespace

void EngineDerivedState::RefreshFromEngine(const IEngine& engine) {
    const uint32_t nowMs = fxe::Millis();
    const bool wasCheckmate = inCheckmate;
    const bool wasCheck = inCheck;
    const auto previousSide = sideToMove;
    const uint8_t previousKing = threatenedKingSquare;
    const uint64_t previousCheckers = checkingPiecesMask;

    // Refresh core state.
    sideToMove = engine.GetSideToMove();
    inCheck = engine.IsInCheck(sideToMove);
    inCheckmate = engine.IsInCheckmate(sideToMove);
    inStalemate = engine.IsInStalemate(sideToMove);

    // Base draw support: stalemate is an engine-provided forced draw.
    // Other forced draws (insufficient material / threefold / 50-move) are
    // coordinator-tracked.
    inDraw = inStalemate;
    drawReason = inStalemate ? DrawReason::Stalemate : DrawReason::None;
    threefoldRepetitionCount = 0;
    halfmoveClockPlies = 0;
    UpdateDrawStartTiming(nowMs);

    if (inCheckmate) {
        if (!wasCheckmate || !checkmateStartValid) {
            checkmateStartValid = true;
            checkmateStartMs = nowMs;
        }
    } else {
        checkmateStartValid = false;
        checkmateStartMs = 0;
    }

    threatenedKingSquareValid = false;
    threatenedKingSquare = 0;
    threatenedKingMask = 0;
    checkingPiecesMask = 0;
    if (!inCheck) {
        checkStartValid = false;
        checkStartMs = 0U;
    }

    char fen[FEN_BUF_CAPACITY] = {};
    if (!engine.GetFen(fen, sizeof(fen))) {
        checkStartValid = false;
        return;
    }

    uint8_t kingSq = 0;
    if (!FindKingSquareInFenPlacement(fen, sideToMove, kingSq)) {
        checkStartValid = false;
        return;
    }

    threatenedKingSquareValid = true;
    threatenedKingSquare = kingSq;
    threatenedKingMask = (kingSq < 64) ? (1ULL << kingSq) : 0;
    if (inCheck) {
        checkingPiecesMask = king_safety::CheckingPiecesMask(fen, kingSq, sideToMove);
        if (!wasCheck || !checkStartValid || sideToMove != previousSide ||
            kingSq != previousKing || checkingPiecesMask != previousCheckers) {
            checkStartValid = true;
            checkStartMs = nowMs;
        }
    }
}

} // namespace modules
