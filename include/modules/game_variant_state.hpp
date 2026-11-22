#pragma once

#include <cstdint>

#include "modules/game_variant.hpp"

namespace modules {

struct GameVariantStateSnapshot {
    GameVariant variant = GameVariant::Classic;
    uint8_t duckSquare = INVALID_DUCK_SQUARE;
    DuckTurnPhase duckTurnPhase = DuckTurnPhase::ChessMove;
    bool pendingChessMove = false;
    uint8_t pendingMoveFrom = INVALID_DUCK_SQUARE;
    uint8_t pendingMoveTo = INVALID_DUCK_SQUARE;
    char pendingPromotionSuffix = '\0';
    VariantOutcome outcome{};
};

bool IsCanonicalClassicVariantState(
    const GameVariantStateSnapshot& state);

uint64_t ComputeGameVariantStateHash(
    const GameVariantStateSnapshot& state);

// Combines the long-lived interaction-model hash with variant state. Canonical
// Classic state deliberately keeps the historical hash value byte-for-byte.
uint64_t ComputeAppStateHash(
    uint64_t interactionModelHash,
    const GameVariantStateSnapshot& variantState);

} // namespace modules
