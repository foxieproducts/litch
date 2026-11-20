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

std::array<bool, 64> Occ64ToOccupiedPhysical(const uint64_t occ64) {
    std::array<bool, 64> out{};
    for (uint8_t sq = 0; sq < 64; ++sq) {
        out[sq] = ((occ64 >> sq) & 1ULL) != 0ULL;
    }
    return out;
}

modules::Rgb PieceLedAtSquare(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
                              const uint8_t sq) {
    return ledFrame[hardware::LedIndexForSquare(sq, hardware::SquareLedSlot::Piece)];
}

bool RgbEq(const modules::Rgb a, const modules::Rgb b) {
    return (a.r == b.r) && (a.g == b.g) && (a.b == b.b);
}

} // namespace

int main() {
    // Validate: when the CPU has queued a move and is waiting for the user to
    // physically execute it (CpuMoving), the mover piece LED blinks with a
    // deterministic cadence: 600ms on, 200ms off.

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_cpu_move_piece_blink_ux.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    // Fixed wheels for deterministic piece color.
    settings.Doc()["P1_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_PIECE_WHEEL"] = 128;

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 0;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;

    // Minimal position: a single white pawn on e2.
    const char* fen = "8/8/8/8/8/8/4P3/8 w - - 0 1";
    Check(engine.SetFen(fen), "SetFen pawn e2");
    derived.RefreshFromEngine(engine);

    // Square indices: 0=a8 ... 63=h1. e2 = 52, e4 = 36.
    static constexpr uint8_t FROM_E2 = 52;
    static constexpr uint8_t TO_E4 = 36;

    const uint64_t expectedOcc64 = (1ULL << FROM_E2);
    sm.SetExpectedOccupancyMask(expectedOcc64);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(expectedOcc64, 0ULL);
    sm.DebugSetPhysicalOccupancyMask(expectedOcc64);

    // Queue the CPU move; this transitions to CpuMoving immediately.
    Check(sm.QueueCpuMove(FROM_E2, TO_E4, /*captureSquareValid=*/false, /*captureSquare=*/0, /*promotionSuffixLower=*/'\0'),
          "QueueCpuMove e2->e4");
    Check((sm.HasQueuedCpuMove() && !sm.IsMoveSettlingActive()), "entered CpuMoving");

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};

    const std::array<bool, 64> occupiedPhysical = Occ64ToOccupiedPhysical(expectedOcc64);

    modules::led_frame_composer::ComposeInputs in{};
    in.settings = &settings;
    in.firstPlayerIsP2 = false;
    in.menuOverlay = nullptr;
    in.menuActive = false;

    // t=0ms => ON window.
    fxe::ManualMillis::SetNowMs(0);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    const modules::Rgb onRgb = PieceLedAtSquare(ledFrame, FROM_E2);
    Check(!RgbEq(onRgb, modules::RGB_BLACK), "mover piece LED is on at t=0ms");

    // t=700ms => OFF window.
    fxe::ManualMillis::SetNowMs(700);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    const modules::Rgb offRgb = PieceLedAtSquare(ledFrame, FROM_E2);
    Check(RgbEq(offRgb, modules::RGB_BLACK), "mover piece LED is off at t=700ms");

    if (g_failures == 0) {
        return 0;
    }

    std::printf("[CPU_MOVE_PIECE_BLINK_UX] failures=%d\n", g_failures);
    return 1;
}
