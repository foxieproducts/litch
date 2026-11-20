#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "foxie_embedded/millis.hpp"

#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"

namespace {

int g_failures = 0;
constexpr const char* INSUFFICIENT_KINGS_FEN = "4k3/8/8/8/8/8/8/4K3 w - - 0 1";

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void DoSimpleMove(runtime::ManualClock& clock,
                  runtime::GameStateMachine& sm,
                  const uint8_t from,
                  const uint8_t to) {
    sm.SetExpectedOccupancyMask(1ULL << from);
    sm.DebugSetPhysicalOccupancyMask(1ULL << from);

    sm.HandleSensorChange(from, false);
    sm.HandleSensorChange(to, true);

    clock.AdvanceMs(1000);
    sm.Tick();
}

void CheckInsufficientMaterialVisible(runtime::GameStateMachine& sm,
                                      modules::GamePersistenceCoordinator& coord,
                                      const char* context) {
    const auto& derived = coord.GetEngineDerivedState();
    Check(derived.inDraw, context);
    Check(derived.drawReason == modules::DrawReason::InsufficientMaterial,
          "insufficient material draw reason visible immediately");

    const runtime::InteractionModelSnapshot snapshot = runtime::BuildInteractionModelSnapshot({
        .stateMachine = &sm,
        .derived = &derived,
    });
    Check(snapshot.game.inDraw, "interaction snapshot exposes draw immediately");
}

bool GenerateHistoryIdentity(void* context, modules::game_history::GameId& id) {
    auto& calls = *static_cast<unsigned*>(context);
    id = {};
    id.bytes[0] = static_cast<uint8_t>(++calls);
    return true;
}

} // namespace

int main() {
    std::puts("[GAME_PERSISTENCE_SAVE_SEMANTICS] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_game_persistence_save_semantics.json";
    cfg.deferredSaveDelayMs = 2000U;
    cfg.mustSaveDelayMs = 100U;

    (void)std::remove(cfg.path);
    (void)std::remove("build/test_game_persistence_save_semantics.json.bak");
    (void)std::remove("build/test_game_persistence_save_semantics.json.tmp");

    runtime::ManualClock clock(1000);
    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::SettingsStore settings(cfg);
    Check(settings.Load(), "settings load");

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    unsigned identityCalls = 0U;
    coord.SetGameHistoryIdentityGenerator(&GenerateHistoryIdentity, &identityCalls);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");
    const auto initialHistory = coord.GetGameHistoryPersistenceState();
    Check(initialHistory.present && initialHistory.currentPly == 0U &&
              initialHistory.lineRevision == 1U && identityCalls == 1U,
          "new game has one identity and an empty authoritative line");

    // Move finalization updates the live snapshot immediately, then uses the
    // 100 ms must-save debounce.
    const uint32_t savesBeforeMove = settings.GetSaveCount();
    DoSimpleMove(clock, sm, 52, 44); // e2 -> e3 (0=a8 mapping)
    coord.Update();

    Check(settings.GetSaveCount() == savesBeforeMove,
          "move finalize does not save synchronously");
    Check(settings.Doc().containsKey(modules::KEY_GAME_FEN), "GAME_FEN written");
    Check(coord.GetUndoHistoryMoveCount() == 1, "undo history tracked after move");
    const auto movedHistory = coord.GetGameHistoryPersistenceState();
    Check(movedHistory.gameId == initialHistory.gameId &&
              movedHistory.currentPly == 1U && movedHistory.lineRevision == 2U,
          "committed move advances same game identity and line revision");
    Check(settings.Doc().containsKey(modules::KEY_GAME_UNDO_HISTORY), "undo history persisted");
    fxe::ManualMillis::AdvanceMs(cfg.mustSaveDelayMs - 1U);
    settings.Update();
    Check(settings.GetSaveCount() == savesBeforeMove,
          "move snapshot remains queued through 99 ms");
    fxe::ManualMillis::AdvanceMs(1U);
    settings.Update();
    Check(settings.GetSaveCount() == savesBeforeMove + 1U,
          "move snapshot saves at the 100 ms deadline");

    // Simulate a reboot: reload settings and coordinator, then verify undo history restores.
    {
        modules::SettingsStore settings2(cfg);
        Check(settings2.Load(), "settings reload");

        runtime::ManualClock clock2(clock.NowMs());
        runtime::GameStateMachine sm2(smCfg);
        sm2.Reset();

        test_support::BoundedHintEngine engine2;
        engine2.Reset();

        modules::GamePersistenceCoordinator coord2(settings2, engine2, sm2);
        coord2.SetGameHistoryIdentityGenerator(&GenerateHistoryIdentity, &identityCalls);
        Check(coord2.LoadOrInitialize(), "LoadOrInitialize after reboot");
        const auto restoredHistory = coord2.GetGameHistoryPersistenceState();
        Check(restoredHistory.present && restoredHistory.gameId == movedHistory.gameId &&
                  restoredHistory.currentPly == movedHistory.currentPly &&
                  restoredHistory.lineRevision == movedHistory.lineRevision &&
                  identityCalls == 1U,
              "reboot restores exact identity/revision without generating another game");
        Check(coord2.GetUndoHistoryMoveCount() == 1, "undo history restored after reboot");
    }

    // External FEN load uses the same must-save tier and clears undo history.
    const uint32_t savesBeforeFen = settings.GetSaveCount();
    Check(coord.LoadExternalFen(INSUFFICIENT_KINGS_FEN), "LoadExternalFen");
    Check(settings.GetSaveCount() == savesBeforeFen,
          "external fen does not save synchronously");

    Check(!settings.Doc().containsKey(modules::KEY_GAME_UNDO_HISTORY), "undo history cleared on external fen");
    CheckInsufficientMaterialVisible(sm, coord, "external insufficient material draw visible immediately");
    fxe::ManualMillis::AdvanceMs(cfg.mustSaveDelayMs);
    settings.Update();
    Check(settings.GetSaveCount() == savesBeforeFen + 1U,
          "external fen snapshot saves at the must-save deadline");

    // Simulate a reboot from an insufficient-material save and verify draw state restores immediately.
    {
        modules::SettingsStore settings2(cfg);
        Check(settings2.Load(), "settings reload after insufficient draw");

        runtime::ManualClock clock2(clock.NowMs());
        runtime::GameStateMachine sm2(smCfg);
        sm2.Reset();

        test_support::BoundedHintEngine engine2;
        engine2.Reset();

        modules::GamePersistenceCoordinator coord2(settings2, engine2, sm2);
        Check(coord2.LoadOrInitialize(), "LoadOrInitialize after insufficient draw reboot");
        CheckInsufficientMaterialVisible(sm2, coord2, "persisted insufficient material draw visible on load");
    }

    if (g_failures == 0) {
        std::puts("[GAME_PERSISTENCE_SAVE_SEMANTICS] ok");
        return 0;
    }

    std::printf("[GAME_PERSISTENCE_SAVE_SEMANTICS] failures=%d\n", g_failures);
    return 1;
}
