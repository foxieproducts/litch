#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

uint8_t g_nextHistoryIdentity = 1U;

bool GenerateHistoryIdentity(
    void*, modules::game_history::GameId& outIdentity) {
    outIdentity = {};
    outIdentity.bytes[0U] = g_nextHistoryIdentity++;
    outIdentity.bytes[15U] = 0xA5U;
    return true;
}

} // namespace

int main() {
    std::puts("[EXTERNAL_FEN_CHANGE_DETECTION] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000);

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_external_fen_change_detection.json";
    cfg.deferredSaveDelayMs = 999999;

    (void)std::remove(cfg.path);
    (void)std::remove("build/test_external_fen_change_detection.json.bak");
    (void)std::remove("build/test_external_fen_change_detection.json.tmp");

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
    coord.SetGameHistoryIdentityGenerator(&GenerateHistoryIdentity);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");
    const modules::GameHistoryPersistenceState firstHistory =
        coord.GetGameHistoryPersistenceState();
    Check(firstHistory.present && firstHistory.gameId.bytes[0U] == 1U &&
              firstHistory.lineRevision == 1U &&
              firstHistory.currentPly == 0U &&
              !firstHistory.captureIncomplete,
          "fresh game receives a persistent archive identity");

    const uint32_t savesBefore = settings.GetSaveCount();

    // Suppress polling while thinking is active (engine GetFen can be transient).
    sm.SetCpuThinkingActive(true);
    Check(engine.SetFen("8/8/8/8/8/8/8/8 w - - 0 1"), "engine SetFen (thinking)");
    coord.Update();
    Check(settings.GetSaveCount() == savesBefore, "no save while cpu thinking active");
    Check(!sm.ConsumeCheckCacheRefreshRequested(), "no refresh while cpu thinking active");
    sm.SetCpuThinkingActive(false);

    // Suppress polling while overlay active.
    sm.SetTextOverlayActive(true);
    sm.SetExpectedOccupancyMask((1ULL << 5) | (1ULL << 10));
    Check(engine.SetFen("8/8/8/8/8/8/8/8 w - - 0 1"), "engine SetFen");
    coord.Update();

    Check(settings.GetSaveCount() == savesBefore, "no save while overlay active");
    Check(!sm.ConsumeCheckCacheRefreshRequested(), "no refresh while overlay active");

    // Allow polling and confirm external mutation is detected.
    sm.SetTextOverlayActive(false);
    coord.Update();

    Check(settings.GetSaveCount() == savesBefore,
          "external mutation queues a debounced save after overlay clears");
    const modules::GameHistoryPersistenceState replacedHistory =
        coord.GetGameHistoryPersistenceState();
    Check(replacedHistory.present &&
              replacedHistory.gameId.bytes[0U] == 2U &&
              replacedHistory.gameId != firstHistory.gameId &&
              replacedHistory.currentPly == 1U &&
              replacedHistory.captureIncomplete,
          "external position replacement starts an explicitly partial archive");
    Check(sm.ConsumeCheckCacheRefreshRequested(), "refresh requested after mutation");
    Check(sm.GetExpectedOccupancyMask() == 0, "external mutation reapplies acknowledged expected occupancy");
    Check(settings.SaveNow(),
          "external mutation reaches storage after debounce simulation");
    Check(settings.GetSaveCount() == savesBefore + 1U,
          "external mutation completes one deferred save");

    sm.ApplyPhysicalOccupancySnapshot(1ULL << 5);
    sm.SetExpectedOccupancyMask((1ULL << 5) | (1ULL << 10));
    sm.SetExpectedSideToMove(modules::PersistedSide::White);
    sm.SetExpectedMoverAndOpponentOccupancyMasks((1ULL << 60), (1ULL << 4));
    sm.SetExpectedKingAndRookOccupancyMasks((1ULL << 60), 0);
    sm.SetHintDisplayRequested(true);
    sm.SetHintMoveSquares(12, 20);
    sm.SetLastMoveForSide(modules::PersistedSide::White, 8, 16);
    sm.SetLastMoveMostRecentSide(modules::PersistedSide::White);
    sm.TriggerAllPiecesFlash(5000);
    Check(sm.StartKingQueenSwapPrompt((1ULL << 4) | (1ULL << 60)), "seed swap prompt before direct reset");

    sm.ResetForExternalEngineMutation();

    Check(sm.GetPhysicalOccupancyMask() == (1ULL << 5), "external reset preserves physical occupancy");
    Check(sm.GetExpectedOccupancyMask() == ((1ULL << 5) | (1ULL << 10)), "external reset preserves expected occupancy");
    uint8_t moverKingSquare = 0;
    Check(sm.TryGetExpectedMoverKingSquare(moverKingSquare) && moverKingSquare == 60,
          "external reset preserves the rest of the acknowledged expected model");
    Check(sm.ConsumeCheckCacheRefreshRequested(), "direct external reset requests refresh");
    Check(!sm.IsHintDisplayRequested(), "external mutation clears hint display request");
    uint8_t hintFrom = 0;
    uint8_t hintTo = 0;
    Check(!sm.TryGetHintMoveSquares(hintFrom, hintTo), "external mutation clears hint move");
    modules::PersistedSide recentSide = modules::PersistedSide::Unknown;
    Check(!sm.TryGetLastMoveMostRecentSide(recentSide), "external mutation clears last-move side");
    Check(!sm.IsAllPiecesFlashActive(), "external mutation clears all-pieces flash");
    Check(!sm.IsKingQueenSwapPromptActive(), "external mutation clears king/queen swap prompt");
    Check(sm.TryEnterPlacementMode(), "external reset preserves immediate placement stability");

    if (g_failures == 0) {
        std::puts("[EXTERNAL_FEN_CHANGE_DETECTION] ok");
        return 0;
    }

    std::printf("[EXTERNAL_FEN_CHANGE_DETECTION] failures=%d\n", g_failures);
    return 1;
}
