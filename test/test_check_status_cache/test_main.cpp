#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "modules/engine_derived_state.hpp"
#include "modules/game_persistence.hpp"
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

uint8_t Square(const char file, const uint8_t rank1to8) {
    // Square mapping: 0=a8, 63=h1.
    const uint8_t f = static_cast<uint8_t>(file - 'a');
    const uint8_t r = static_cast<uint8_t>(8 - rank1to8);
    return static_cast<uint8_t>(r * 8 + f);
}

void DoSimpleMove(runtime::ManualClock& clock,
                  runtime::GameStateMachine& sm,
                  const uint64_t initialOcc64,
                  const uint8_t from,
                  const uint8_t to) {
    sm.SetExpectedOccupancyMask(initialOcc64);
    sm.DebugSetPhysicalOccupancyMask(initialOcc64);

    sm.HandleSensorChange(from, false);
    sm.HandleSensorChange(to, true);

    clock.AdvanceMs(1000);
    sm.Tick();
}

bool FenToOcc64(const char* fen, uint64_t& outOcc64) {
    outOcc64 = 0;
    if (fen == nullptr) {
        return false;
    }

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

        const uint8_t sq = static_cast<uint8_t>(rank * 8 + file);
        outOcc64 |= (1ULL << sq);
        ++file;
    }

    return true;
}

} // namespace

int main() {
    std::puts("[CHECK_STATUS_CACHE] start");

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_check_status_cache.json";
    cfg.deferredSaveDelayMs = 999999;

    (void)std::remove(cfg.path);
    (void)std::remove("build/test_check_status_cache.json.bak");
    (void)std::remove("build/test_check_status_cache.json.tmp");

    runtime::ManualClock clock(1000);
    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::SettingsStore settings(cfg);
    Check(settings.Load(), "settings load");

    // Seed a simple position: white queen on b4, black king on e8, white to move.
    // Then play Qb5+.
    const char* fenStart = "4k3/8/8/8/1Q6/8/8/4K3 w - - 0 1";
    settings.Doc()[modules::KEY_GAME_FEN] = fenStart;
    Check(settings.SaveNow(), "settings SaveNow");

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    // White plays Qb5+, making black (side-to-move) in check.
    const uint8_t from = Square('b', 4);
    const uint8_t to = Square('b', 5);
    uint64_t startOcc64 = 0;
    Check(FenToOcc64(fenStart, startOcc64), "FenToOcc64(start)");
    DoSimpleMove(clock, sm, startOcc64, from, to);

    coord.Update();

    const auto& derived = coord.GetEngineDerivedState();
    Check(derived.inCheck, "derived.inCheck after move");
    Check(!derived.inCheckmate, "not checkmate");
    Check(derived.threatenedKingSquareValid, "king square valid");
    Check(derived.threatenedKingSquare == Square('e', 8), "king square is e8");

    // Sanity: a known simple checkmate position.
    const char* fenMate = "7k/6Q1/7K/8/8/8/8/8 b - - 0 1";
    Check(coord.LoadExternalFen(fenMate), "LoadExternalFen (mate)");

    const auto& derivedMate = coord.GetEngineDerivedState();
    Check(derivedMate.inCheck, "mate position: in check");
    Check(derivedMate.inCheckmate, "mate position: in checkmate");
    Check(derivedMate.checkmateStartValid, "mate position: checkmate episode start valid");

    // Every external engine mutation must refresh both the terminal state and
    // its episode clock. A stale checkmate flag would keep the fatal animation
    // alive after reset; a stale start would resume it at an arbitrary phase.
    coord.ResetEngine();
    const auto& derivedAfterReset = coord.GetEngineDerivedState();
    Check(!derivedAfterReset.inCheckmate, "reset clears checkmate state");
    Check(!derivedAfterReset.checkmateStartValid, "reset clears checkmate episode timing");
    Check(coord.GetUndoHistoryMoveCount() == 0U,
          "reset clears the previous game's undo history");
    Check(!coord.RequestUndo(1U),
          "undo cannot cross the new-game boundary");
    Check(!coord.GetEngineDerivedState().inCheckmate,
          "rejected cross-boundary undo leaves the new game active");

    Check(coord.LoadExternalFen(fenStart), "external non-checkmate FEN loads");
    const auto& derivedAfterExternalFen = coord.GetEngineDerivedState();
    Check(!derivedAfterExternalFen.inCheckmate, "external FEN clears checkmate state");
    Check(!derivedAfterExternalFen.checkmateStartValid,
          "external FEN clears checkmate episode timing");

    if (g_failures == 0) {
        std::puts("[CHECK_STATUS_CACHE] ok");
        return 0;
    }

    std::printf("[CHECK_STATUS_CACHE] failures=%d\n", g_failures);
    return 1;
}
