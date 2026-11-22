#include <array>
#include <cstdint>
#include <cstdio>

#include "../support/led_frame_composer_snapshot_adapter.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/led_mapping.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/initial_fen.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/orientation.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "modules/ui_color_wheel.hpp"
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

void CheckRgbEq(const modules::Rgb actual, const modules::Rgb expected, const char* msg) {
    if (actual.r == expected.r && actual.g == expected.g && actual.b == expected.b) {
        return;
    }

    std::printf("[FAIL] %s (actual=%u,%u,%u expected=%u,%u,%u)\n",
                msg,
                static_cast<unsigned>(actual.r),
                static_cast<unsigned>(actual.g),
                static_cast<unsigned>(actual.b),
                static_cast<unsigned>(expected.r),
                static_cast<unsigned>(expected.g),
                static_cast<unsigned>(expected.b));
    ++g_failures;
}

uint16_t Intensity(const modules::Rgb c) {
    return static_cast<uint16_t>(c.r) + static_cast<uint16_t>(c.g) + static_cast<uint16_t>(c.b);
}

uint8_t ScaleU8Saturating(const uint8_t v, const uint16_t scale256) {
    const uint32_t scaled = static_cast<uint32_t>(v) * static_cast<uint32_t>(scale256);
    const uint32_t out = (scaled + 128U) >> 8;
    return (out > 255U) ? 255U : static_cast<uint8_t>(out);
}

modules::Rgb ScaleRgbSaturating(const modules::Rgb c, const uint16_t scale256) {
    return modules::Rgb{ScaleU8Saturating(c.r, scale256), ScaleU8Saturating(c.g, scale256),
                        ScaleU8Saturating(c.b, scale256)};
}

modules::Rgb ApplyComposerPostScales(const modules::Rgb c) {
    // Must match the compositor scales in `led_frame_composer_compose_board.cpp` and
    // `led_frame_composer_compose_edge.cpp`.
    static constexpr uint16_t PIECE_BRIGHTER_SCALE256 = 304;          // ~1.19 in Q8.8
    static constexpr uint16_t GLOBAL_BRIGHTNESS_BOOST_SCALE256 = 320; // 1.25 in Q8.8

    return ScaleRgbSaturating(ScaleRgbSaturating(c, PIECE_BRIGHTER_SCALE256), GLOBAL_BRIGHTNESS_BOOST_SCALE256);
}

modules::Rgb PieceLedAtSquare(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
                              const uint8_t physicalSq) {
    return ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Piece)];
}

std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> MapPhysical(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& semantic,
    const bool squareRotate) {
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> physical{};
    for (uint16_t index = 0U; index < hardware::BOARD_LED_COUNT; ++index) {
        physical[hardware::MapLedIndexForSquareAndEdgeRotate(
            index, squareRotate, false)] = semantic[index];
    }
    return physical;
}

} // namespace

int main() {
    std::puts("[FIRST_PLAYER_COLOR_STABLE] start");

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_flip_does_not_swap_piece_colors.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    // Use two clearly distinct wheels.
    settings.Doc()["P1_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_PIECE_WHEEL"] = 128;
    settings.Doc()["P1_END_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_END_PIECE_WHEEL"] = 128;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 20;
    smCfg.settleFlashMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::LitchMaxEngine engine;
    engine.Reset();

    // Engine position is the standard chess starting position. P1 owns White.
    Check(engine.SetFen(modules::INITIAL_FEN), "engine SetFen(initial)");

    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);

    // Occupancy snapshot is physical-square indexed (0=a8..63=h1).
    std::array<bool, 64> occupiedPhysical{};

    // Pick a known White piece square in the chosen initial position.
    // Standard initial: e2 is White pawn.
    // Mapping is 0=a8..63=h1 => e2 = rank 6, file 4 => 6*8+4 = 52.
    static constexpr uint8_t E2 = 52;
    static constexpr uint8_t E7 = 12;
    occupiedPhysical[E2] = true;
    occupiedPhysical[E7] = true;
    sm.DebugSetPhysicalOccupancyMask((1ULL << E2) | (1ULL << E7));

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    modules::led_frame_composer::ComposeInputs in;
    in.settings = &settings;
    in.firstPlayerIsP2 = false;

    // Baseline.
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    const auto physical0 = MapPhysical(ledFrame, false);
    const modules::Rgb c0 = PieceLedAtSquare(physical0, E2);
    const modules::Rgb black0 = PieceLedAtSquare(physical0, E7);
    Check(Intensity(c0) > 0U, "baseline: piece LED is non-black");

    const modules::Rgb expectedP1 = ApplyComposerPostScales(modules::ui_color_wheel::WheelToRgb(0));
    const modules::Rgb expectedP2 = ApplyComposerPostScales(modules::ui_color_wheel::WheelToRgb(128));
    CheckRgbEq(c0, expectedP1, "baseline: white piece uses P1 wheel color");
    CheckRgbEq(black0, expectedP2, "baseline: black piece uses P2 wheel color");

    // End slots retain their colors; logical players select their new ends.
    settings.Doc()["GAME_FIRST_PLAYER"] = 2U;
    in.firstPlayerIsP2 = true;
    const bool squareRotate = modules::ComputeSquareRotate(false, true);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    const auto physical1 = MapPhysical(ledFrame, squareRotate);
    const uint8_t whitePhysical = runtime::MapSquareIndex(E2, squareRotate);
    const uint8_t blackPhysical = runtime::MapSquareIndex(E7, squareRotate);
    CheckRgbEq(PieceLedAtSquare(physical1, whitePhysical), expectedP2,
               "logical P1 selects the far end's unchanged palette");
    CheckRgbEq(PieceLedAtSquare(physical1, blackPhysical), expectedP1,
               "logical P2 selects the near end's unchanged palette");

    if (g_failures == 0) {
        std::puts("[FIRST_PLAYER_COLOR_STABLE] ok");
        return 0;
    }

    std::printf("[FIRST_PLAYER_COLOR_STABLE] failures=%d\n", g_failures);
    return 1;
}
