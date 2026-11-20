#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
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

uint64_t FenToOcc64(const char* fen) {
    if (!fen) {
        return 0;
    }

    uint64_t occ64 = 0;
    uint8_t rank = 0;
    uint8_t file = 0;

    for (const char* p = fen; *p != '\0'; ++p) {
        const char c = *p;
        if (c == ' ') {
            break;
        }

        if (c == '/') {
            ++rank;
            file = 0;
            continue;
        }

        if (c >= '1' && c <= '8') {
            file = static_cast<uint8_t>(file + static_cast<uint8_t>(c - '0'));
            continue;
        }

        const bool isPiece = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!isPiece) {
            break;
        }

        if (rank < 8 && file < 8) {
            const uint8_t sq = static_cast<uint8_t>((rank * 8) + file);
            occ64 |= (1ULL << sq);
        }

        ++file;
    }

    return occ64;
}

bool FenStartsWith(const char* fen, const char* prefix) {
    if (!fen || !prefix) {
        return false;
    }

    while (*prefix) {
        if (*fen != *prefix) {
            return false;
        }
        ++fen;
        ++prefix;
    }
    return true;
}

const char* FenBoardPartEnd(const char* fen) {
    if (!fen) {
        return nullptr;
    }

    for (const char* p = fen; *p != '\0'; ++p) {
        if (*p == ' ') {
            return p;
        }
    }

    return nullptr;
}

bool FenBoardStartsWith(const char* fen, const char* boardPrefix) {
    if (!fen || !boardPrefix) {
        return false;
    }

    const char* fenEnd = FenBoardPartEnd(fen);
    const char* p = fen;
    const char* q = boardPrefix;

    while (*q != '\0') {
        if (fenEnd && p >= fenEnd) {
            return false;
        }
        if (*p != *q) {
            return false;
        }
        ++p;
        ++q;
    }

    return true;
}

bool FenBoardEquals(const char* fen, const char* board) {
    if (!fen || !board) {
        return false;
    }

    const char* fenEnd = FenBoardPartEnd(fen);
    if (!fenEnd) {
        return false;
    }

    const size_t fenBoardLen = static_cast<size_t>(fenEnd - fen);
    size_t boardLen = 0;
    while (board[boardLen] != '\0') {
        ++boardLen;
    }

    if (fenBoardLen != boardLen) {
        return false;
    }

    for (size_t i = 0; i < fenBoardLen; ++i) {
        if (fen[i] != board[i]) {
            return false;
        }
    }

    return true;
}

bool FenBoardEqualsFenBoard(const char* fenA, const char* fenB) {
    if (!fenA || !fenB) {
        return false;
    }

    const char* endA = FenBoardPartEnd(fenA);
    const char* endB = FenBoardPartEnd(fenB);
    if (!endA || !endB) {
        return false;
    }

    const size_t lenA = static_cast<size_t>(endA - fenA);
    const size_t lenB = static_cast<size_t>(endB - fenB);
    if (lenA != lenB) {
        return false;
    }

    for (size_t i = 0; i < lenA; ++i) {
        if (fenA[i] != fenB[i]) {
            return false;
        }
    }

    return true;
}

void TestChoiceRevalidationAfterHallChange(const bool cancelAfterRejection) {
    constexpr char PRE_FEN[] = "7k/P7/8/8/8/8/2P5/7K w - - 0 1";
    constexpr uint8_t PAWN_FROM = 8U;
    constexpr uint8_t PAWN_TO = 0U;
    constexpr uint8_t C2 = 50U;
    modules::SettingsStoreConfig config;
    config.path = "build/test_promotion_choice_revalidation.json";
    config.deferredSaveDelayMs = 999999U;
    (void)std::remove(config.path);
    (void)std::remove("build/test_promotion_choice_revalidation.json.bak");
    (void)std::remove("build/test_promotion_choice_revalidation.json.tmp");
    fxe::ManualMillis::SetNowMs(1000U);
    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 10U;
    stateConfig.placementStableRequiredMs = 0U;
    stateConfig.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine state(stateConfig);
    state.Reset();
    modules::SettingsStore settings(config);
    Check(settings.Load(), "choice revalidation settings load");
    settings.Doc()["PROMOTION_MODE"] = "choose";
    settings.Doc()[modules::KEY_GAME_FEN] = PRE_FEN;
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(settings, engine, state);
    Check(coordinator.LoadOrInitialize(), "choice revalidation restores its fixture");
    uint64_t physical = FenToOcc64(PRE_FEN);
    state.ApplyPhysicalOccupancySnapshot(physical);
    const auto observe = [&](const uint8_t square, const bool occupied) {
        physical = occupied ? physical | (UINT64_C(1) << square)
                            : physical & ~(UINT64_C(1) << square);
        state.HandleBoardObservation(UINT64_C(1) << square, physical,
            fxe::Millis(), state.GetChessInteractionModel().board.sequence + 1U);
    };
    observe(PAWN_FROM, false);
    coordinator.UpdateLegalDestinationsMask();
    observe(PAWN_TO, true);
    fxe::ManualMillis::AdvanceMs(20U);
    state.Tick();
    coordinator.Update();
    for (uint8_t i = 0U; i < 3U; ++i) {
        state.HandleButtonEvent(modules::UserButton::Right, fxe::Button::Event::Press);
    }
    const uint32_t token = state.GetChessInteractionModel().phase.pendingActionToken;
    const uint8_t history = coordinator.GetUndoHistoryMoveCount();
    const auto side = coordinator.GetGameClock().GetActiveSide();
    Check(state.IsPromotionActive() && state.GetPromotionSelection() == modules::PromotionPiece::Knight,
          "choice revalidation starts with Knight selected");
    // Direct production input permits the Hall event before coordinator consumption;
    // this supporting seam bypasses raw ButtonManager debounce and session scheduling.
    state.HandleButtonEvent(modules::UserButton::P1O, fxe::Button::Event::Press);
    Check(!state.IsPromotionActive(), "coherent O produces a deferred one-shot choice");
    observe(C2, false);
    state.HandleButtonEvent(modules::UserButton::P1O, fxe::Button::Event::Press);
    coordinator.Update();
    char fen[128]{};
    Check(engine.GetFen(fen, sizeof(fen)), "choice revalidation reads the actual engine");
    std::printf("[PROMOTION_CHOICE_REVALIDATION] cancel=%u physical=%016llx "
                "phase=%s active=%u choice=%u token=%u expected_token=%u "
                "history=%u fen=\"%s\" | next=restore c2 then fresh O, or cancel\n",
                cancelAfterRejection, static_cast<unsigned long long>(physical),
                runtime::ChessInteractionPhaseToString(state.GetChessInteractionModel().phase.tag),
                state.IsPromotionActive(), static_cast<unsigned>(state.GetPromotionSelection()),
                state.GetChessInteractionModel().phase.pendingActionToken, token,
                coordinator.GetUndoHistoryMoveCount(), fen);
    Check(state.IsPromotionActive() && state.GetPromotionSelection() == modules::PromotionPiece::Knight &&
              state.GetChessInteractionModel().phase.pendingActionToken == token &&
              std::strcmp(fen, PRE_FEN) == 0 && engine.GetSideToMove() == modules::SideToMove::White &&
              coordinator.GetUndoHistoryMoveCount() == history &&
              coordinator.GetGameClock().GetActiveSide() == side,
          "Hall change before choice consumption restores the same uncommitted Knight modal");
    if (cancelAfterRejection) {
        Check(coordinator.RequestUndo(1U), "revalidated promotion remains cancellable with c2 absent");
        coordinator.Update();
        Check(!state.IsPromotionActive() && coordinator.GetUndoHistoryMoveCount() == history,
              "cancel after revalidation never undoes older history");
        return;
    }
    // A stale additional O while correction owns the task must not survive restoration.
    state.HandleButtonEvent(modules::UserButton::P1O, fxe::Button::Event::Press);
    observe(C2, true);
    state.Tick();
    coordinator.Update();
    Check(state.IsPromotionActive() && state.GetPromotionSelection() == modules::PromotionPiece::Knight &&
              coordinator.GetUndoHistoryMoveCount() == history,
          "restoring c2 cannot accept the discarded O or reset Knight to Queen");
    const bool needsFreshO = state.IsPromotionActive() && coordinator.GetUndoHistoryMoveCount() == history;
    state.HandleButtonEvent(modules::UserButton::P1O, fxe::Button::Event::Press);
    coordinator.Update();
    Check(needsFreshO && engine.GetFen(fen, sizeof(fen)) && FenBoardStartsWith(fen, "N6k/") &&
              engine.GetSideToMove() == modules::SideToMove::Black &&
              coordinator.GetUndoHistoryMoveCount() == history + 1U && !state.IsPromotionActive(),
          "fresh O after revalidation commits exactly one Knight");
}

} // namespace

int main() {
    std::puts("[GAME_PERSISTENCE_PROMOTION] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

#ifndef ARDUINO
    static constexpr const char* PROMO_FEN_WHITE = "7k/P7/8/8/8/8/8/7K w - - 0 1";
    static constexpr const char* PROMO_FEN_BLACK = "7k/8/8/8/8/8/p7/7K b - - 0 1";

    auto runScenario = [&](const char* fen,
                           const uint8_t fromSq,
                           const uint8_t toSq,
                           const uint8_t rightPressCount,
                           const char* expectedFenPrefixAfterConfirm,
                           const bool automatic = false) {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_game_persistence_promotion.json";
        cfg.deferredSaveDelayMs = 999999;

        (void)std::remove(cfg.path);
        (void)std::remove("build/test_game_persistence_promotion.json.bak");
        (void)std::remove("build/test_game_persistence_promotion.json.tmp");

        runtime::ManualClock clock(1000);
        fxe::ManualMillis::SetNowMs(clock.NowMs());

        runtime::GameStateMachineConfig smCfg;
        smCfg.settleDurationMs = 10;
        smCfg.cpuIdleDelayMs = 999999;
        smCfg.placementStableRequiredMs = 0;

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load");
        if (!automatic) settings.Doc()["PROMOTION_MODE"] = "choose";

        // Seed persisted FEN so LoadOrInitialize starts here.
        settings.Doc()[modules::KEY_GAME_FEN] = fen;

        test_support::BoundedHintEngine engine;
        engine.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize");

        // Seed physical occupancy to match engine expected occupancy.
        const uint64_t occ64 = FenToOcc64(fen);
        sm.DebugSetPhysicalOccupancyMask(occ64);
        Check(sm.IsExpectedOccupancyMatched(), "physical matches expected before move");

        sm.SetValidDestMask(1ULL << toSq);

        sm.HandleSensorChange(fromSq, false);
        Check(sm.IsHumanPieceInHand(), "promotion pickup enters PickedUp");

        sm.HandleSensorChange(toSq, true);
        Check(sm.IsMoveSettlingActive(), "promotion place enters Settling");

        clock.AdvanceMs(20);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        sm.Tick();
        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction,
              "settle exposes the pending promotion action");

        // Coordinator should detect pawn-to-last-rank and enter promotion modal.
        coord.Update();
        if (automatic) {
            char automaticFen[128] = {};
            Check(!sm.IsPromotionActive(), "default Queen needs no selection or extra O");
            Check(engine.GetFen(automaticFen, sizeof(automaticFen)) &&
                      FenBoardStartsWith(automaticFen, expectedFenPrefixAfterConfirm),
                  "default Queen commits explicit suffix through production action");
            const auto history = coord.GetUndoHistoryMoveCount();
            coord.Update();
            Check(history == 1U && coord.GetUndoHistoryMoveCount() == history,
                  "automatic Queen commits history exactly once");
            return;
        }
        Check(sm.IsPromotionActive(), "promotion modal active");
        Check(sm.GetPromotionSelection() == modules::PromotionPiece::Queen, "default promotion queen");

        // During promotion, the move is deferred: engine should still be at the pre-move FEN.
        char beforeBuf[128] = {};
        Check(engine.GetFen(beforeBuf, sizeof(beforeBuf)), "engine GetFen (before confirm)");
        Check(FenToOcc64(beforeBuf) == FenToOcc64(fen), "engine occ unchanged during promotion modal");
        Check(FenBoardEqualsFenBoard(beforeBuf, fen), "engine board unchanged during promotion modal");

        for (uint8_t i = 0; i < rightPressCount; ++i) {
            sm.HandleButtonEvent(modules::UserButton::Right, fxe::Button::Event::Press);
        }

        settings.Doc()["PROMOTION_MODE"] = "queen";
        coord.Update();
        Check(sm.IsPromotionActive(), "mode change cannot auto-commit an open Choose modal");
        // Confirm selection.
        sm.HandleButtonEvent(modules::UserButton::P1O, fxe::Button::Event::Press);
        Check(!sm.IsPromotionActive(), "promotion modal exits after confirm");

        coord.Update();

        char afterBuf[128] = {};
        Check(engine.GetFen(afterBuf, sizeof(afterBuf)), "engine GetFen (after confirm)");
        Check(FenBoardStartsWith(afterBuf, expectedFenPrefixAfterConfirm), "engine board has promoted piece");
    };

    TestChoiceRevalidationAfterHallChange(false);
    TestChoiceRevalidationAfterHallChange(true);

    runScenario(PROMO_FEN_WHITE, 8, 0, 0, "Q6k/", true);
    runScenario(PROMO_FEN_BLACK, 48, 56, 0, "7k/8/8/8/8/8/8/q6K", true);

    // White promotions (a7->a8, uppercase pieces).
    runScenario(PROMO_FEN_WHITE, 8, 0, 0, "Q6k/");
    runScenario(PROMO_FEN_WHITE, 8, 0, 1, "R6k/");
    runScenario(PROMO_FEN_WHITE, 8, 0, 2, "B6k/");
    runScenario(PROMO_FEN_WHITE, 8, 0, 3, "N6k/");

    // Black promotions (a2->a1, lowercase pieces).
    runScenario(PROMO_FEN_BLACK, 48, 56, 0, "7k/8/8/8/8/8/8/q6K");
    runScenario(PROMO_FEN_BLACK, 48, 56, 1, "7k/8/8/8/8/8/8/r6K");
    runScenario(PROMO_FEN_BLACK, 48, 56, 2, "7k/8/8/8/8/8/8/b6K");
    runScenario(PROMO_FEN_BLACK, 48, 56, 3, "7k/8/8/8/8/8/8/n6K");

    // Undo during promotion should cancel the modal and revert the move.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_game_persistence_promotion.json";
        cfg.deferredSaveDelayMs = 999999;

        (void)std::remove(cfg.path);
        (void)std::remove("build/test_game_persistence_promotion.json.bak");
        (void)std::remove("build/test_game_persistence_promotion.json.tmp");

        runtime::ManualClock clock(1000);
        fxe::ManualMillis::SetNowMs(clock.NowMs());

        runtime::GameStateMachineConfig smCfg;
        smCfg.settleDurationMs = 10;
        smCfg.cpuIdleDelayMs = 999999;
        smCfg.placementStableRequiredMs = 0;

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (undo scenario)");
        settings.Doc()["PROMOTION_MODE"] = "choose";

        settings.Doc()[modules::KEY_GAME_FEN] = PROMO_FEN_WHITE;

        test_support::BoundedHintEngine engine;
        engine.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (undo scenario)");

        const uint64_t occ64 = FenToOcc64(PROMO_FEN_WHITE);
        sm.DebugSetPhysicalOccupancyMask(occ64);
        Check(sm.IsExpectedOccupancyMatched(), "physical matches expected (undo scenario)");

        sm.SetValidDestMask(1ULL << 0);
        sm.HandleSensorChange(8, false);
        sm.HandleSensorChange(0, true);
        clock.AdvanceMs(20);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        sm.Tick();

        coord.Update();
        Check(sm.IsPromotionActive(), "promotion modal active (undo scenario)");

        // Undo tap.
        Check(coord.RequestUndo(1U), "promotion undo request accepted");
        coord.Update();

        char afterBuf[128] = {};
        Check(engine.GetFen(afterBuf, sizeof(afterBuf)), "engine GetFen (after undo)");
        Check(FenToOcc64(afterBuf) == FenToOcc64(PROMO_FEN_WHITE), "engine occ restored after undo");
        Check(FenBoardEqualsFenBoard(afterBuf, PROMO_FEN_WHITE), "engine board restored after undo");
    }
#else
    std::puts("[GAME_PERSISTENCE_PROMOTION] skipped (ARDUINO)");
#endif

    if (g_failures == 0) {
        std::puts("[GAME_PERSISTENCE_PROMOTION] ok");
        return 0;
    }

    std::printf("[GAME_PERSISTENCE_PROMOTION] failures=%d\n", g_failures);
    return 1;
}
