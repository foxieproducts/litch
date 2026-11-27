#include <array>
#include <cstdint>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "bounded_hint_engine.hpp"
#include "modules/ambient_lux_averager.hpp"
#include "modules/brightness_scale_policy.hpp"
#include "modules/settings_registry.hpp"
#include "hardware/led_layout.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/light_square_appearance.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "modules/white_squares_setting.hpp"
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

uint16_t Intensity(const modules::Rgb c) {
    return static_cast<uint16_t>(c.r) + static_cast<uint16_t>(c.g) + static_cast<uint16_t>(c.b);
}

bool RgbEq(const modules::Rgb a, const modules::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

modules::Rgb SquareGridAt(const std::array<modules::Rgb, 64>& gridFrame, const uint8_t physicalSq) {
    return gridFrame[physicalSq];
}

modules::Rgb PieceLedAt(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame, const uint8_t physicalSq) {
    return ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Piece)];
}

void CheckLightSquareSteps(uint8_t boardBrightness, bool automatic, int hue) {
    modules::AmbientLuxAverager ambient;
    ambient.OnLuxSample(0U, 2.0f);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.strong_power_source = 1U;
    config.brightness_scale_256 = modules::brightness_scale_policy::ComputeEffectiveScale256(
        boardBrightness, true, ambient.ComputeAmbientFactor01(0U), 256U, automatic);

    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    auto& settings = board.SettingsForPersistenceTest();
    settings.Doc()["BRIGHTNESS"] = boardBrightness;
    settings.Doc()["AUTO_BRIGHTNESS"] = automatic;
    settings.Doc()["LIGHT_SQUARE_MODE"] = hue < 0 ? "white" : "color";
    settings.Doc()["LIGHT_SQUARE_HUE"] = hue < 0 ? 0 : hue;
    board.ObserveHallMask(0U);
    board.Advance(3000U);
    const auto measure = [&]() {
        board.Advance(20U);
        const auto& pixels = board.PhysicalFrame().pixels;
        const auto total = [](const virtual_litch_color& p) { return p.r + p.g + p.b + p.w; };
        constexpr size_t first = 27U * 4U; // both background emitters on D4
        return total(pixels[first]) + total(pixels[first + 2U]);
    };

    std::array<int, 8> levels{};
    for (uint8_t step = 1U; step <= levels.size(); ++step) {
        StaticJsonDocument<32> value;
        value.set(step);
        Check(modules::settings_registry::ValidateAndSet(settings, "LIGHT_SQUARE_BRIGHTNESS",
                  value.as<JsonVariantConst>()) == modules::settings_registry::ValidateAndSetResult::Ok,
              "each of the eight app slider positions is accepted");
        levels[step - 1U] = measure();
        Check(levels[step - 1U] == measure(), "square LEDs stay steady between frames");
    }
    if (boardBrightness == 0U) {
        for (int level : levels) Check(level == 0, "whole-display zero keeps all square levels off");
        return;
    }
    Check(levels.front() > 0 && levels.front() <= 3, "minimum gives only a faint light on each square");
    Check(levels.back() >= (boardBrightness == 1U ? 2 : 3) * levels[3],
          "brightest square is substantially brighter than the midpoint");
    if (boardBrightness == 8U && automatic && (hue < 0 || hue == 255)) {
        Check(levels[3] == (hue < 0 ? 12 : 4), "midpoint matches the 1.0.4 maximum at 2 lux");
    }
    for (size_t i = 1; i < levels.size(); ++i) {
        Check(levels[i] >= levels[i - 1U], "raising square brightness never dims the square");
        if (boardBrightness == 8U || (boardBrightness >= 5U && i >= 4U))
            Check(levels[i] > levels[i - 1U], "square levels remain distinct after output quantization");
    }
}
} // namespace

int main() {
    std::puts("[BRIGHTNESS_ZERO_WHITE_SQUARES_OFF] start");
    for (const bool automatic : {false, true}) {
        for (const uint8_t level : {0U, 1U, 5U}) {
            for (const int hue : {-1, 255, 42, 170}) CheckLightSquareSteps(level, automatic, hue);
        }
        for (int hue = -1; hue <= 255; ++hue) CheckLightSquareSteps(8U, automatic, hue);
    }

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_brightness_zero_white_squares_off.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    // Ensure some piece LED is non-black so the test isn't accidentally checking a fully empty scene.
    settings.Doc()["P1_PIECE_WHEEL"] = 10;
    settings.Doc()["P2_PIECE_WHEEL"] = 200;

    runtime::GameStateMachineConfig smCfg;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);

    std::array<bool, 64> occupiedPhysical{};
    occupiedPhysical.fill(false);
    occupiedPhysical[0] = true; // a8 (light square) is also occupied in the initial position.
    sm.DebugSetPhysicalOccupancyMask(1ULL << 0U);

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame0{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame0{};
    std::array<modules::Rgb, 64> gridFrame0{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame1{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame1{};
    std::array<modules::Rgb, 64> gridFrame1{};

    modules::led_frame_composer::ComposeInputs in;
    in.settings = &settings;
    in.firstPlayerIsP2 = false;
    in.menuOverlay = nullptr;
    in.menuActive = false;

    // Level zero suppresses ordinary background without erasing its preference.
    // Above zero, WHITE_SQUARES_ON controls light-square visibility.
    settings.Doc()["BRIGHTNESS"] = 0;
    settings.Doc()["WHITE_SQUARES_ON"] = true;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame0, edgeFrame0, gridFrame0);

    Check(settings.ReadValue("WHITE_SQUARES_ON", false),
          "near-dark suppression preserves the enabled white-square preference");

    // Turning white squares off should disable the light-square background even at BRIGHTNESS=1.
    settings.Doc()["BRIGHTNESS"] = 1;
    settings.Doc()["WHITE_SQUARES_ON"] = false;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame1, edgeFrame1, gridFrame1);

    const modules::Rgb lightSq0 = SquareGridAt(gridFrame0, 0);
    const modules::Rgb lightSq1 = SquareGridAt(gridFrame1, 0);

    Check(Intensity(lightSq0) == 0U, "level zero suppresses ordinary background with preference enabled");
    Check(Intensity(lightSq1) == 0U, "WHITE_SQUARES_ON=0 turns off light-square background");

    // Piece LEDs should still be lit in both cases.
    const modules::Rgb piece0 = PieceLedAt(ledFrame0, 0);
    const modules::Rgb piece1 = PieceLedAt(ledFrame1, 0);
    Check(Intensity(piece0) > 0U, "BRIGHTNESS=0 does not turn off piece LEDs");
    Check(Intensity(piece1) > 0U, "BRIGHTNESS=1 keeps piece LEDs on");

    settings.Doc()["WHITE_SQUARES_ON"] = true;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame1, edgeFrame1, gridFrame1);
    Check(Intensity(SquareGridAt(gridFrame1, 0)) > 0U,
          "level one restores enabled ordinary background after near-dark suppression");
    Check(settings.ReadValue("WHITE_SQUARES_ON", false),
          "restored background retains the enabled preference");

    settings.Doc()["BRIGHTNESS"] = 8U;
    (void)settings.Doc().remove("LIGHT_SQUARE_MODE");
    (void)settings.Doc().remove("LIGHT_SQUARE_HUE");
    (void)settings.Doc().remove("LIGHT_SQUARE_BRIGHTNESS");
    (void)settings.Doc().remove(modules::light_square_appearance::KEY_WIDE_RANGE);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame0, edgeFrame0, gridFrame0);
    const modules::Rgb legacyDefault = SquareGridAt(gridFrame0, 0);

    settings.Doc()["LIGHT_SQUARE_MODE"] = "white";
    constexpr uint8_t upgraded[] = {1, 2, 2, 3, 3, 3, 4, 4};
    for (uint8_t old = 1U; old <= 8U; ++old) {
        settings.Doc()["LIGHT_SQUARE_BRIGHTNESS"] = old;
        Check(modules::light_square_appearance::ReadBrightness(settings) == upgraded[old - 1U],
              "saved 1.0.4 brightness maps into the lower half of the new range");
    }
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame1, edgeFrame1, gridFrame1);
    const modules::Rgb defaultWhite = SquareGridAt(gridFrame1, 0);
    Check(RgbEq(legacyDefault, defaultWhite),
          "missing appearance keys preserve the exact established White/current-brightness output");

    settings.Doc()["LIGHT_SQUARE_MODE"] = "color";
    settings.Doc()["LIGHT_SQUARE_HUE"] = 0U;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame0, edgeFrame0, gridFrame0);
    const modules::Rgb fullColor = SquareGridAt(gridFrame0, 0);
    Check(!RgbEq(defaultWhite, fullColor) &&
              fullColor.r == defaultWhite.r &&
              fullColor.g == 0U && fullColor.b == 0U,
          "Color mode replaces neutral gray with the selected hue at the same restrained peak channel");

    settings.Doc()["LIGHT_SQUARE_BRIGHTNESS"] = 1U;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame1, edgeFrame1, gridFrame1);
    const modules::Rgb dimColor = SquareGridAt(gridFrame1, 0);
    Check(Intensity(dimColor) > 0U && Intensity(dimColor) < Intensity(fullColor),
          "light-square brightness scales the selected color independently without becoming a second Off control");

    modules::BoardLedFrame startupFrame{};
    modules::led_frame_composer::ComposeStartupGrid(settings, startupFrame);
    Check(RgbEq(startupFrame[0].left, defaultWhite) &&
              RgbEq(startupFrame[0].right, defaultWhite),
          "startup grid stays at the established neutral full-intensity cue despite configured color and intensity");

    settings.Doc()["WHITE_SQUARES_ON"] = false;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame1, edgeFrame1, gridFrame1);
    Check(Intensity(SquareGridAt(gridFrame1, 0)) == 0U,
          "the existing light-square switch remains the sole Off authority for colored backgrounds");
    modules::led_frame_composer::ComposeStartupGrid(settings, startupFrame);
    Check(Intensity(startupFrame[0].left) == 0U &&
              Intensity(startupFrame[0].right) == 0U,
          "startup grid still honors the established light-square Off preference");

    settings.Doc()["WHITE_SQUARES_ON"] = true;
    settings.Doc()["BRIGHTNESS"] = 0U;
    modules::led_frame_composer::ComposeStartupGrid(settings, startupFrame);
    Check(Intensity(startupFrame[0].left) == 0U &&
              Intensity(startupFrame[0].right) == 0U,
          "startup grid still honors the established global minimum-brightness suppression");

    // Migration: legacy BRIGHTNESS=0 with missing WHITE_SQUARES_ON should map to
    // WHITE_SQUARES_ON=0 and BRIGHTNESS=1.
    (void)settings.Doc().remove("WHITE_SQUARES_ON");
    settings.Doc()["BRIGHTNESS"] = 0;
    modules::white_squares_setting::MigrateLegacyWhiteSquaresSetting(settings);
    Check(!settings.ReadValue("WHITE_SQUARES_ON", true),
          "Migration maps legacy BRIGHTNESS=0 to WHITE_SQUARES_ON=0");
    Check(settings.ReadValue("BRIGHTNESS", static_cast<uint32_t>(0U)) == 1U,
          "Migration normalizes BRIGHTNESS to 1");

    if (g_failures == 0) {
        std::puts("[BRIGHTNESS_ZERO_WHITE_SQUARES_OFF] ok");
        return 0;
    }

    std::printf("[BRIGHTNESS_ZERO_WHITE_SQUARES_OFF] failures=%d\n", g_failures);
    return 1;
}
