#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/game_variant_state.hpp"

int main() {
    fxe::test_support::TestResults results("GAME_VARIANT_STATE_HASH");
    results.Start();

    constexpr uint64_t LEGACY_HASH = 0x0123456789abcdefULL;
    modules::GameVariantStateSnapshot classic;
    results.Check(modules::IsCanonicalClassicVariantState(classic),
                  "default snapshot is canonical Classic state");
    results.Check(modules::ComputeAppStateHash(LEGACY_HASH, classic) ==
                      LEGACY_HASH,
                  "canonical Classic preserves legacy app hash");

    modules::GameVariantStateSnapshot duck;
    duck.variant = modules::GameVariant::Duck;
    duck.duckSquare = 18U;
    const uint64_t duckHash =
        modules::ComputeAppStateHash(LEGACY_HASH, duck);
    results.Check(modules::ComputeGameVariantStateHash(duck) ==
                      0x7ae89bb73c1cda13ULL,
                  "Duck variant hash matches stable schema fixture");
    results.Check(duckHash == 0x0bddd90af233c92bULL,
                  "Duck app hash matches stable schema fixture");
    results.Check(duckHash != LEGACY_HASH,
                  "Duck state extends the app hash");
    results.Check(modules::ComputeAppStateHash(LEGACY_HASH, duck) == duckHash,
                  "Duck app hash is deterministic");

    modules::GameVariantStateSnapshot changed = duck;
    changed.duckSquare = 19U;
    results.Check(modules::ComputeAppStateHash(LEGACY_HASH, changed) != duckHash,
                  "duck square participates in app hash");
    changed = duck;
    changed.duckTurnPhase = modules::DuckTurnPhase::DuckRelocation;
    results.Check(modules::ComputeAppStateHash(LEGACY_HASH, changed) != duckHash,
                  "duck phase participates in app hash");
    changed = duck;
    changed.pendingChessMove = true;
    changed.pendingMoveFrom = 52U;
    changed.pendingMoveTo = 36U;
    changed.pendingPromotionSuffix = 'q';
    results.Check(modules::ComputeAppStateHash(LEGACY_HASH, changed) != duckHash,
                  "pending Duck move participates in app hash");
    changed = duck;
    changed.outcome.reason = modules::VariantOutcomeReason::Fowled;
    changed.outcome.winner = modules::GameWinner::Black;
    results.Check(modules::ComputeAppStateHash(LEGACY_HASH, changed) != duckHash,
                  "Duck outcome participates in app hash");

    return results.Finish();
}
