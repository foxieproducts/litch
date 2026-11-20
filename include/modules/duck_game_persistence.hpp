#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/game_variant.hpp"

namespace modules {

constexpr uint8_t DUCK_GAME_DURABLE_VERSION = 1U;

struct DuckGameDurableState {
    GameVariant variant = GameVariant::Undecided;
    uint8_t duckSquare = INVALID_DUCK_SQUARE;
    DuckTurnPhase phase = DuckTurnPhase::ChessMove;
    VariantOutcome outcome{};
    bool pendingChessMoveValid = false;
    uint8_t pendingMoveFrom = INVALID_DUCK_SQUARE;
    uint8_t pendingMoveTo = INVALID_DUCK_SQUARE;
    char pendingPromotionSuffix = '\0';
};

bool EncodeDuckGameDurableState(const DuckGameDurableState& state,
                                char* out,
                                size_t outSize);

bool DecodeDuckGameDurableState(const char* encoded,
                                DuckGameDurableState& out);

bool EncodeDuckUndoHistory(const uint8_t* fromSquares,
                           const uint8_t* toSquares,
                           uint16_t moveCount,
                           char* out,
                           size_t outSize);

bool DecodeDuckUndoHistory(const char* encoded,
                           uint8_t* outFromSquares,
                           uint8_t* outToSquares,
                           uint16_t capacity,
                           uint16_t expectedMoveCount);

} // namespace modules
