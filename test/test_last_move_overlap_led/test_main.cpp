#include <array>
#include <cstdint>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"
#include "../support/led_frame_composer_snapshot_adapter.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

bool RgbEq(const modules::Rgb a, const modules::Rgb b) {
    return (a.r == b.r) && (a.g == b.g) && (a.b == b.b);
}

} // namespace

int main() {
    std::puts("[LAST_MOVE_OVERLAP] start");

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_last_move_overlap_led.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    settings.Doc()["P1_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_PIECE_WHEEL"] = 128;

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};

    const std::array<bool, 64> occupiedPhysical{};

    modules::led_frame_composer::ComposeInputs in;
    in.settings = &settings;
    in.firstPlayerIsP2 = false;

    fxe::ManualMillis::SetNowMs(0);

    const uint8_t p1From = 52;
    const uint8_t p2From = 28;
    const uint8_t sharedTo = 36;

    const auto CaptureSharedSquare = [&](const bool setWhite,
                                         const bool setBlack,
                                         const modules::PersistedSide mostRecent) -> modules::Rgb {
        sm.ClearLastMoveForSide(modules::PersistedSide::White);
        sm.ClearLastMoveForSide(modules::PersistedSide::Black);
        sm.ClearLastMoveMostRecentSide();

        if (setWhite) {
            sm.SetLastMoveForSide(modules::PersistedSide::White, p1From, sharedTo);
        }
        if (setBlack) {
            sm.SetLastMoveForSide(modules::PersistedSide::Black, p2From, sharedTo);
        }
        if (mostRecent != modules::PersistedSide::Unknown) {
            sm.SetLastMoveMostRecentSide(mostRecent);
        }

        test_support::ComposeWithSnapshot(
            in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
        return gridFrame[sharedTo];
    };

    const modules::Rgb p1Only = CaptureSharedSquare(true, false, modules::PersistedSide::White);
    const modules::Rgb p2Only = CaptureSharedSquare(false, true, modules::PersistedSide::Black);

    Check(!RgbEq(p1Only, p2Only), "overlap: p1-only and p2-only colors differ");

    const modules::Rgb preferP1 = CaptureSharedSquare(true, true, modules::PersistedSide::White);
    Check(RgbEq(preferP1, p1Only), "overlap: prefer most recent white square color");

    const modules::Rgb preferP2 = CaptureSharedSquare(true, true, modules::PersistedSide::Black);
    Check(RgbEq(preferP2, p2Only), "overlap: prefer most recent black square color");

    if (g_failures == 0) {
        std::puts("[LAST_MOVE_OVERLAP] ok");
        return 0;
    }

    std::printf("[LAST_MOVE_OVERLAP] failures=%d\n", g_failures);
    return 1;
}
