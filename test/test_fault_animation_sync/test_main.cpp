#include <array>
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

modules::Rgb SquareLedAt(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
                         const uint8_t physicalSq) {
    return ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Square0)];
}

modules::BoardSquareLeds BoardSquareLedsAt(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
    const uint8_t physicalSq) {
    return modules::BoardSquareLeds{
        ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Square0)],
        ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Piece)],
        ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Square2)],
        ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Piece)],
    };
}

bool AnyEdgeLit(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edgeFrame) {
    for (const modules::Rgb rgb : edgeFrame) {
        if (rgb.r != 0U || rgb.g != 0U || rgb.b != 0U) {
            return true;
        }
    }

    return false;
}

bool RgbEq(const modules::Rgb a, const modules::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool BoardSquareLedsEq(const modules::BoardSquareLeds& a,
                       const modules::BoardSquareLeds& b) {
    return RgbEq(a.left, b.left) &&
           RgbEq(a.top, b.top) && RgbEq(a.bottom, b.bottom) &&
           RgbEq(a.right, b.right);
}

} // namespace

int main() {
    std::puts("[FAULT_ANIMATION_SYNC] start");

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_fault_animation_sync.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    runtime::GameStateMachineConfig smCfg;
    smCfg.illegalFlashMs = 10U;
    smCfg.cpuIdleDelayMs = 999999U;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);

    std::array<bool, 64> occupiedPhysical{};
    occupiedPhysical.fill(false);

    modules::led_frame_composer::ComposeInputs in;
    in.settings = &settings;
    in.firstPlayerIsP2 = false;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledOn{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledOff{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeOn{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeOff{};
    std::array<modules::Rgb, 64> gridOn{};
    std::array<modules::Rgb, 64> gridOff{};

    static constexpr uint8_t ILLEGAL_SQUARE = 1U;

    fxe::ManualMillis::SetNowMs(10U);
    sm.HandleSensorChange(ILLEGAL_SQUARE, true);
    occupiedPhysical[ILLEGAL_SQUARE] = true;

    fxe::ManualMillis::SetNowMs(1510U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledOn, edgeOn, gridOn);
    Check(!RgbEq(SquareLedAt(ledOn, ILLEGAL_SQUARE), modules::RGB_BLACK),
          "strong board fault starts lit after the graded interval");
    Check(AnyEdgeLit(edgeOn), "strong edge error starts lit after the graded interval");

    fxe::ManualMillis::SetNowMs(1520U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledOff, edgeOff, gridOff);
    Check(RgbEq(SquareLedAt(ledOff, ILLEGAL_SQUARE), modules::RGB_BLACK),
          "board fault square reaches off-phase at configured period");
    Check(!AnyEdgeLit(edgeOff), "edge error reaches off-phase with board fault timing");

    runtime::GameStateMachine lateSm(smCfg);
    fxe::ManualMillis::SetNowMs(1210U);
    lateSm.Reset();
    lateSm.HandleSensorChange(ILLEGAL_SQUARE, true);
    fxe::ManualMillis::SetNowMs(2710U);

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> lateLedOn{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> lateEdgeOn{};
    std::array<modules::Rgb, 64> lateGridOn{};
    test_support::ComposeWithSnapshot(
        in, lateSm, derived, &engine, lateLedOn, lateEdgeOn, lateGridOn);

    Check(BoardSquareLedsEq(BoardSquareLedsAt(ledOn, ILLEGAL_SQUARE),
                            BoardSquareLedsAt(lateLedOn, ILLEGAL_SQUARE)),
          "invalid rainbow starts from one interaction-relative phase regardless of uptime");
    Check(AnyEdgeLit(lateEdgeOn),
          "late invalid feedback starts its edge status on the same phase");

    runtime::InteractionModelSnapshot earlyCueSnapshot;
    modules::led_frame_composer::internal::ComposeContext earlyCueContext;
    earlyCueContext.snapshot = &earlyCueSnapshot;
    earlyCueContext.nowMs = 0U;
    earlyCueContext.expectedOcc64 = 1ULL << ILLEGAL_SQUARE;
    earlyCueContext.physicalOcc64 = 0ULL;
    earlyCueSnapshot.interaction.lastInteractionElapsedMs = 0U;

    modules::led_frame_composer::internal::ComposeContext lateCueContext = earlyCueContext;
    lateCueContext.nowMs = 240U;

    const auto earlyCueRuntime =
        modules::led_frame_composer::internal::BuildBoardComposeRuntime(earlyCueContext);
    const auto lateCueRuntime =
        modules::led_frame_composer::internal::BuildBoardComposeRuntime(lateCueContext);
    Check(earlyCueRuntime.mismatchRainbowBaseWheel ==
              lateCueRuntime.mismatchRainbowBaseWheel,
          "invalid rainbow phase is anchored to the interaction, not absolute uptime");

    if (g_failures == 0) {
        std::puts("[FAULT_ANIMATION_SYNC] ok");
        return 0;
    }

    std::printf("[FAULT_ANIMATION_SYNC] failures=%d\n", g_failures);
    return 1;
}
