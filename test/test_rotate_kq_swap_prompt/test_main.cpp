#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstdlib>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
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

void MakeSimpleMoveAndSettle(runtime::GameStateMachine& sm,
                             modules::GamePersistenceCoordinator& coord,
                             const uint8_t fromSquare,
                             const uint8_t toSquare,
                             const uint32_t settleMs) {
    sm.HandleSensorChange(fromSquare, false);
    coord.UpdateLegalDestinationsMask();
    sm.HandleSensorChange(toSquare, true);
    fxe::ManualMillis::AdvanceMs(settleMs);
    sm.Tick();
    coord.Update();
}

} // namespace

int main() {
    std::puts("[ROTATE_KQ_SWAP_PROMPT] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000);

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_rotate_kq_swap_prompt.json";
    cfg.deferredSaveDelayMs = 999999;

    (void)std::remove(cfg.path);
    (void)std::remove("build/test_rotate_kq_swap_prompt.json.bak");
    (void)std::remove("build/test_rotate_kq_swap_prompt.json.tmp");

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 20;
    smCfg.placementStableRequiredMs = 10;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::SettingsStore settings(cfg);
    Check(settings.Load(), "settings load");

    // Start with rotate already enabled.
    auto& doc = settings.Doc();
    doc["BOARD_ROTATE"] = true;
    doc["GAME_FIRST_PLAYER"] = 1U;
    doc["VS_CPU_LEVEL"] = 0U;
    settings.SaveNow();

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    // Start in a playable state: physical occupancy matches expected.
    sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());

    // Initial-position matching should tolerate differing halfmove/fullmove counters.
    Check(modules::IsAnyInitialFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 4 7"),
          "initial fen recognized with non-canonical move counters");

    static constexpr uint64_t KQ_MASK64 = (1ULL << 60) | (1ULL << 59) | (1ULL << 4) | (1ULL << 3);

    auto ClearPrompt = [&]() {
        uint64_t mask64 = 0;
        Check(sm.TryGetKingQueenSwapPromptMask(mask64), "prompt active");
        Check(mask64 == KQ_MASK64, "prompt mask matches kings+queens");

        // Clear prompt by lifting/restoring each required square at least
        // once (does not require all 4 to be empty simultaneously).
        uint64_t physical = sm.GetPhysicalOccupancyMask();
        uint32_t sequence = sm.GetLastBoardObservationSequence() + 1U;
        for (uint8_t sq = 0; sq < 64; ++sq) {
            if (((mask64 >> sq) & 1ULL) != 0ULL) {
                const uint64_t squareMask = 1ULL << sq;
                physical &= ~squareMask;
                sm.HandleBoardObservation(squareMask,
                                          physical,
                                          fxe::Millis(),
                                          sequence++);
                physical |= squareMask;
                sm.HandleBoardObservation(squareMask,
                                          physical,
                                          fxe::Millis(),
                                          sequence++);
            }
        }

        // Prompt exits only after stability window.
        fxe::ManualMillis::AdvanceMs(10);
        sm.Tick();
        Check(!sm.IsKingQueenSwapPromptActive(), "prompt cleared after restore+stable");
    };

    // First update should NOT start the prompt just because rotate is enabled.
    coord.Update();
    Check(!sm.IsKingQueenSwapPromptActive(), "no prompt on first update without a toggle");

    // Toggle rotate at the initial FEN: should prompt.
    doc["BOARD_ROTATE"] = false;
    settings.SaveNow();

    coord.Update();

    Check(sm.IsKingQueenSwapPromptActive(), "prompt active after rotate toggle");

    // Revert before any lifts: prompt should cancel immediately and not re-queue.
    doc["BOARD_ROTATE"] = true;
    settings.SaveNow();
    coord.Update();
    Check(!sm.IsKingQueenSwapPromptActive(), "prompt canceled when rotate reverted before lifts");

    coord.Update();
    Check(!sm.IsKingQueenSwapPromptActive(), "reverted prompt does not requeue");
    // Further updates should not re-trigger while settings remain stable.
    coord.Update();
    Check(!sm.IsKingQueenSwapPromptActive(), "no further prompt after cleared");

    // Player-end placement changes the effective square orientation and
    // therefore owns the same physical K/Q correction journey.
    doc["GAME_FIRST_PLAYER"] = 2U;
    settings.SaveNow();

    coord.Update();
    Check(sm.IsKingQueenSwapPromptActive(),
          "K/Q prompt starts after player-end placement toggle");
    doc["GAME_FIRST_PLAYER"] = 1U;
    settings.SaveNow();
    coord.Update();
    Check(!sm.IsKingQueenSwapPromptActive(),
          "reverting untouched player-end placement cancels its prompt");
    coord.Update();
    Check(!sm.IsKingQueenSwapPromptActive(),
          "canceled player-end prompt does not requeue");

    doc["GAME_FIRST_PLAYER"] = 2U;
    settings.SaveNow();
    coord.Update();
    Check(sm.IsKingQueenSwapPromptActive(),
          "player-end placement can start a fresh prompt after cancellation");
    ClearPrompt();

    // Literal BOARD_ROTATE also changes the effective orientation while P1 is
    // seated at the opposite physical end.
    doc["BOARD_ROTATE"] = false;
    settings.SaveNow();
    coord.Update();
    Check(sm.IsKingQueenSwapPromptActive(),
          "literal rotate prompts with player-end placement swapped");
    ClearPrompt();
    coord.Update();
    Check(!sm.IsKingQueenSwapPromptActive(),
          "completed literal rotate prompt does not queue twice");

    doc["GAME_FIRST_PLAYER"] = 1U;
    settings.SaveNow();
    coord.Update();
    Check(sm.IsKingQueenSwapPromptActive(),
          "reverting player-end placement queues its effective-orientation prompt");
    ClearPrompt();

    // Undoing to the opening re-enables both start-only orientation settings.
    settings.Doc()["BOARD_ROTATE"] = false;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
    settings.SaveNow();

    sm.Reset();
    engine.Reset();
    Check(coord.LoadOrInitialize(), "LoadOrInitialize for undo-to-start regression");
    sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());

    // Make one move, then undo it back to the beginning.
    MakeSimpleMoveAndSettle(sm, coord, 52U, 36U, smCfg.settleDurationMs);
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "after move: idle");

    Check(coord.RequestUndo(1U), "undo request succeeds");
    Check(sm.IsUndoPromptActive(), "undo prompt active");

    uint8_t undoFrom = 255;
    uint8_t undoTo = 255;
    Check(sm.TryGetUndoPromptMove(undoFrom, undoTo), "undo prompt move readable");
    // Undo prompt fields are stored as original move from->to; physically undo by
    // moving the piece back from the destination square to the origin square.
    sm.HandleSensorChange(undoTo, false);
    sm.HandleSensorChange(undoFrom, true);
    fxe::ManualMillis::AdvanceMs(smCfg.settleDurationMs);
    sm.Tick();
    coord.Update();

    Check(!sm.IsUndoPromptActive(), "undo prompt cleared at opening position");

    settings.Doc()["GAME_FIRST_PLAYER"] = 2U;
    settings.SaveNow();
    coord.Update();
    Check(sm.IsKingQueenSwapPromptActive(),
          "player-end toggle after undo-to-start queues one K/Q prompt");
    ClearPrompt();

    settings.Doc()["BOARD_ROTATE"] = true;
    settings.SaveNow();
    coord.Update();
    Check(sm.IsKingQueenSwapPromptActive(),
          "literal rotate after undo-to-start queues one K/Q prompt");
    ClearPrompt();
    coord.Update();
    Check(!sm.IsKingQueenSwapPromptActive(),
          "undo-to-start literal rotate prompt clears without requeue");

    if (g_failures == 0) {
        std::puts("[ROTATE_KQ_SWAP_PROMPT] ok");
        return 0;
    }

    std::printf("[ROTATE_KQ_SWAP_PROMPT] failures=%d\n", g_failures);
    return 1;
}
