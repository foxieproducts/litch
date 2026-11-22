#pragma once

#include <cstring>

namespace modules {

// Standard chess starting position (piece placement).
// White moves first.
//
// P1 always owns White and White always moves first. GAME_FIRST_PLAYER is
// compatibility storage for which physical end P1 occupies.
static constexpr const char* INITIAL_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Legacy variant from an earlier iteration: swapped piece colors at game start.
// This produces a legal FEN string, but it blocks white pawns behind the back rank
// and is not used for new games.
static constexpr const char* LEGACY_INITIAL_FEN_P2_OWNS_WHITE_SWAPPED_COLORS =
    "RNBQKBNR/PPPPPPPP/8/8/8/8/pppppppp/rnbqkbnr w KQkq - 0 1";

// Legacy variant: historical persisted/traced data may have used a black-to-move
// initial FEN. Keep this only for compatibility recognition (e.g. prompt gating).
static constexpr const char* LEGACY_INITIAL_FEN_BLACK_TO_MOVE =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1";

inline bool FenStartsWithPositionFields(const char* fen, const char* firstFourFieldsPrefix) {
    if (fen == nullptr || firstFourFieldsPrefix == nullptr) {
        return false;
    }

    const char* f = fen;
    const char* p = firstFourFieldsPrefix;
    while (*p != '\0') {
        if (*f == '\0' || *f != *p) {
            return false;
        }
        ++f;
        ++p;
    }

    // Allow either exact 4-field FENs or full FENs with halfmove/fullmove counters.
    return (*f == '\0') || (*f == ' ');
}

constexpr const char* InitialFenForFirstPlayerIsP2(const bool firstPlayerIsP2) {
    (void)firstPlayerIsP2;
    return INITIAL_FEN;
}

inline bool IsAnyInitialFen(const char* fen) {
    static constexpr const char* INITIAL_FEN_PREFIX =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -";
    static constexpr const char* LEGACY_SWAPPED_PREFIX =
        "RNBQKBNR/PPPPPPPP/8/8/8/8/pppppppp/rnbqkbnr w KQkq -";
    static constexpr const char* LEGACY_BLACK_TO_MOVE_PREFIX =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq -";

    return FenStartsWithPositionFields(fen, INITIAL_FEN_PREFIX) ||
           FenStartsWithPositionFields(fen, LEGACY_SWAPPED_PREFIX) ||
           FenStartsWithPositionFields(fen, LEGACY_BLACK_TO_MOVE_PREFIX);
}

} // namespace modules
