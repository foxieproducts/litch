#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/app_event.hpp"
#include "runtime/app_event_dispatch.hpp"
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

void PressP1(runtime::ManualClock& clock, runtime::GameStateMachine& sm) {
    runtime::DispatchAppEventToStateMachine(
        sm, runtime::AppEvent::MakeButton(clock.NowMs(), modules::UserButton::P1X, fxe::Button::Event::Press));
}

} // namespace

int main() {
    std::puts("[UNDO_PROMOTION_REGRESSION] start");

#ifndef ARDUINO
    static constexpr const char* PROMO_FEN = "7k/P7/8/8/8/8/8/7K w - - 0 1";
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_undo_promotion_regression.json";
    cfg.deferredSaveDelayMs = 999999;
    (void)std::remove(cfg.path);
    (void)std::remove("build/test_undo_promotion_regression.json.bak");
    (void)std::remove("build/test_undo_promotion_regression.json.tmp");

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

    // Seed persisted FEN so LoadOrInitialize starts here.
    settings.Doc()["PROMOTION_MODE"] = "choose";
    settings.Doc()[modules::KEY_GAME_FEN] = PROMO_FEN;

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    // Seed physical occupancy to match engine expected occupancy.
    sm.DebugSetPhysicalOccupancyMask(FenToOcc64(PROMO_FEN));
    Check(sm.IsExpectedOccupancyMatched(), "physical matches expected before move");

    // Perform promotion move: a7 (8) -> a8 (0).
    static constexpr uint8_t FROM_SQUARE = 8;
    static constexpr uint8_t TO_SQUARE = 0;
    sm.SetValidDestMask(1ULL << TO_SQUARE);

    sm.HandleSensorChange(FROM_SQUARE, false);
    Check(sm.IsHumanPieceInHand(), "pickup enters PickedUp");

    sm.HandleSensorChange(TO_SQUARE, true);
    Check(sm.IsMoveSettlingActive(), "place enters Settling");

    clock.AdvanceMs(20);
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "settle exposes its promotion action");

    coord.Update();
    Check(sm.IsPromotionActive(), "promotion modal active");

    // Underpromote to knight: queen->rook->bishop->knight.
    sm.HandleButtonEvent(modules::UserButton::Right, fxe::Button::Event::Press);
    sm.HandleButtonEvent(modules::UserButton::Right, fxe::Button::Event::Press);
    sm.HandleButtonEvent(modules::UserButton::Right, fxe::Button::Event::Press);

    sm.HandleButtonEvent(modules::UserButton::P1O, fxe::Button::Event::Press);
    Check(!sm.IsPromotionActive(), "promotion modal exits after confirm");

    coord.Update();

    char afterPromo[128] = {};
    Check(engine.GetFen(afterPromo, sizeof(afterPromo)), "engine GetFen after promotion");
    Check(FenStartsWith(afterPromo, "N6k/"), "engine has promoted knight");

    // Undo should restore the pre-promotion FEN (not reset to initial position).
    Check(coord.RequestUndo(1U), "undo request accepted");
    Check(sm.IsUndoPromptActive(), "undo prompt active after undo");

    coord.Update();

    char afterUndo[128] = {};
    Check(engine.GetFen(afterUndo, sizeof(afterUndo)), "engine GetFen after undo");
    Check(FenStartsWith(afterUndo, "7k/P7/"), "undo restores pawn on a7");
    Check(std::strcmp(afterUndo, modules::INITIAL_FEN) != 0, "undo does not reset to initial fen");

    // Cancel-undo should re-apply the promotion.
    Check(coord.RequestUndo(1U), "cancel-undo request accepted");
    coord.Update();

    char afterCancel[128] = {};
    Check(engine.GetFen(afterCancel, sizeof(afterCancel)), "engine GetFen after cancel-undo");
    Check(FenStartsWith(afterCancel, "N6k/"), "cancel-undo restores promoted knight");
#else
    std::puts("[UNDO_PROMOTION_REGRESSION] skipped (ARDUINO)");
#endif

    if (g_failures == 0) {
        std::puts("[UNDO_PROMOTION_REGRESSION] ok");
        return 0;
    }

    std::printf("[UNDO_PROMOTION_REGRESSION] failures=%d\n", g_failures);
    return 1;
}
