#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstdlib>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"

int main() {
    fxe::test_support::TestResults results("CASTLING_ROOK_FORCED_DEST_HINTS");
    results.Start();

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000);

    modules::SettingsStoreConfig storeCfg;
    storeCfg.path = "build/test_castling_rook_forced_dest_hints.json";
    storeCfg.deferredSaveDelayMs = 999999;

    (void)std::remove(storeCfg.path);
    (void)std::remove("build/test_castling_rook_forced_dest_hints.json.bak");
    (void)std::remove("build/test_castling_rook_forced_dest_hints.json.tmp");

    modules::SettingsStore settings(storeCfg);
    results.Check(settings.Load(), "settings load");

    runtime::GameStateMachineConfig smCfg;
    smCfg.placementStableRequiredMs = 0;
    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    results.Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    // Minimal pre-castle engine position where king-side castling is legal for white.
    static constexpr const char* FEN = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";

#ifndef ARDUINO
    // Keep physical occupancy consistent so the coordinator won't force placement mode.
    const uint64_t occ = (1ULL << 0) | (1ULL << 4) | (1ULL << 7) | (1ULL << 56) | (1ULL << 60) | (1ULL << 63);
    sm.DebugSetPhysicalOccupancyMask(occ);
#endif

    results.Check(coord.LoadExternalFen(FEN), "LoadExternalFen");

    // Simulate king-first castling choreography: e1 up, g1 down, then rook pickup from h1.
    sm.HandleSensorChange(60, false); // e1 up
    sm.HandleSensorChange(62, true);  // g1 down
    sm.HandleSensorChange(63, false); // h1 up
    results.Check(sm.IsHumanPieceInHand(), "rook pickup enters PickedUp");

    coord.UpdateLegalDestinationsMask();

    // Rook must be placed on f1 (61) to complete castling.
    const uint64_t f1 = (1ULL << 61);
    results.Check(sm.GetValidDestMask() == f1, "rook castling hint constrained to f1");

    return results.Finish();
}
