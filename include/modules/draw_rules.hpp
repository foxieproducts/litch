#pragma once

#include <cstdint>

#include "foxie_embedded/fixed_string.hpp"
#include "modules/game_persistence.hpp"

namespace modules {

enum class DrawReason : uint8_t {
    None = 0,
    Stalemate = 1,
    ThreefoldRepetition = 2,
    FiftyMoveRule = 3,
    InsufficientMaterial = 4,
};

// Builds a raw position/cache key; en-passant metadata is preserved.
// Includes only the first 4 FEN fields:
//   <placement> <side> <castling> <ep>
// and intentionally ignores halfmove/fullmove counters.
bool TryBuildFenPositionKey4(const char* fen, fxe::FixedString<96>& outKey);

// Classic repetition compares available legal moves. An en-passant target
// distinguishes otherwise identical positions only when a legal capture exists.
// Raw FEN/protocol/cache authority remains unchanged. Not a Duck rules identity.
bool SameClassicRepetitionPosition(const char* leftFen, const char* rightFen);

// Counts occurrences of the current Classic position in the provided
// history list. If history includes the current position, the result includes it.
uint8_t CountThreefoldRepetition(const char* currentFen,
                                 const char* const* historyFens,
                                 uint8_t historyCount);

// Returns the next halfmove clock in plies (half-moves) after applying a move.
// Resets to 0 on any pawn move or any capture.
uint16_t NextHalfmoveClockPlies(const char* preMoveFen,
                                uint16_t priorHalfmoveClockPlies,
                                uint8_t fromSquare,
                                uint8_t toSquare,
                                bool captureSquareValid,
                                uint8_t captureSquare);

constexpr bool IsFiftyMoveDraw(const uint16_t halfmoveClockPlies) {
    // 50 full moves = 100 plies.
    return halfmoveClockPlies >= 100U;
}

// Returns true when the position is an immediate dead draw because neither side
// has mating material.
bool IsInsufficientMaterialDraw(const char* fen);

} // namespace modules
