#include "modules/game_variant_state.hpp"

namespace modules {
namespace {

constexpr uint64_t FNV_OFFSET_BASIS = 1469598103934665603ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;
constexpr uint8_t APP_STATE_HASH_SCHEMA_VERSION = 1U;

uint64_t AddByte(const uint64_t hash, const uint8_t value) {
    return (hash ^ value) * FNV_PRIME;
}

uint64_t AddU64(uint64_t hash, const uint64_t value) {
    for (uint8_t shift = 0U; shift < 64U; shift += 8U) {
        hash = AddByte(hash, static_cast<uint8_t>(value >> shift));
    }
    return hash;
}

} // namespace

bool IsCanonicalClassicVariantState(
    const GameVariantStateSnapshot& state) {
    return state.variant == GameVariant::Classic &&
           state.duckSquare == INVALID_DUCK_SQUARE &&
           state.duckTurnPhase == DuckTurnPhase::ChessMove &&
           !state.pendingChessMove &&
           state.pendingMoveFrom == INVALID_DUCK_SQUARE &&
           state.pendingMoveTo == INVALID_DUCK_SQUARE &&
           state.pendingPromotionSuffix == '\0' &&
           state.outcome.reason == VariantOutcomeReason::None &&
           state.outcome.winner == GameWinner::None;
}

uint64_t ComputeGameVariantStateHash(
    const GameVariantStateSnapshot& state) {
    uint64_t hash = FNV_OFFSET_BASIS;
    hash = AddByte(hash, static_cast<uint8_t>(state.variant));
    hash = AddByte(hash, state.duckSquare);
    hash = AddByte(hash, static_cast<uint8_t>(state.duckTurnPhase));
    hash = AddByte(hash, state.pendingChessMove ? 1U : 0U);
    hash = AddByte(hash, state.pendingMoveFrom);
    hash = AddByte(hash, state.pendingMoveTo);
    hash = AddByte(hash, static_cast<uint8_t>(state.pendingPromotionSuffix));
    hash = AddByte(hash, static_cast<uint8_t>(state.outcome.reason));
    return AddByte(hash, static_cast<uint8_t>(state.outcome.winner));
}

uint64_t ComputeAppStateHash(
    const uint64_t interactionModelHash,
    const GameVariantStateSnapshot& variantState) {
    if (IsCanonicalClassicVariantState(variantState)) {
        return interactionModelHash;
    }

    uint64_t hash = AddByte(
        FNV_OFFSET_BASIS, APP_STATE_HASH_SCHEMA_VERSION);
    hash = AddU64(hash, interactionModelHash);
    return AddU64(hash, ComputeGameVariantStateHash(variantState));
}

} // namespace modules
