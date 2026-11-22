#include <array>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/led_layout.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "modules/turn_anim_style_setting.hpp"
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

bool IsLit(const modules::Rgb& c) {
    return (c.r != 0U) || (c.g != 0U) || (c.b != 0U);
}

bool SameRgb(const modules::Rgb lhs, const modules::Rgb rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

bool FramesEqual(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& a,
                 const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& b) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    std::puts("[SIDE_MIDDLE_BAND_STATIC_TURN] start");

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_side_middle_band_static_turn.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    settings.Doc()[modules::turn_anim_style_setting::KEY_TURN_ANIM_STYLE] =
        modules::turn_anim_style_setting::VALUE_SIDE_MIDDLE_BAND;
    settings.Doc()["P1_PIECE_WHEEL"] = 170;
    settings.Doc()["P2_PIECE_WHEEL"] = 0;

    runtime::GameStateMachine sm;
    sm.Reset();

    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;
    derived.sideToMove = modules::SideToMove::White;

    std::array<bool, 64> occupiedPhysical{};
    occupiedPhysical.fill(false);

    modules::led_frame_composer::ComposeInputs in;
    in.settings = &settings;
    in.firstPlayerIsP2 = false;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame0{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame0{};
    std::array<modules::Rgb, 64> gridFrame0{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame1{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame1{};
    std::array<modules::Rgb, 64> gridFrame1{};

    fxe::ManualMillis::SetNowMs(0U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame0, edgeFrame0, gridFrame0);

    fxe::ManualMillis::SetNowMs(700U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame1, edgeFrame1, gridFrame1);

    Check(FramesEqual(edgeFrame0, edgeFrame1),
          "side_middle_band idle-turn edge frame stays static across time");

    for (uint8_t i = 0; i < modules::EDGE_LED_COUNT; ++i) {
        const bool inBand = ((i >= 10U) && (i <= 13U)) || ((i >= 26U) && (i <= 29U));
        if (inBand) {
            Check(IsLit(edgeFrame0[i]), "side_middle_band lights expected side middle LEDs");
        } else {
            Check(!IsLit(edgeFrame0[i]), "side_middle_band keeps non-band LEDs off");
        }
    }

    // A persistence warning owns exactly the fixed USB-side pair, including
    // its dark interval, even when the selected Turn style normally keeps that
    // same side band continuously lit.
    settings.DisableForBoot();
    settings.ReportAsyncSaveFailure(modules::SettingsStoreError::WriteFailed,
        modules::SettingsStore::StorageFailureEvidence::Confirmed);
    settings.Doc()[modules::turn_anim_style_setting::KEY_TURN_ANIM_STYLE] =
        modules::turn_anim_style_setting::VALUE_SIDE_MIDDLE_BAND;
    settings.Doc()["P1_PIECE_WHEEL"] = 170;
    settings.Doc()["P2_PIECE_WHEEL"] = 0;
    for (const bool boardRotate : {false, true}) {
        constexpr uint16_t first = hardware::BOARD_LED_COUNT + 27U;
        constexpr uint16_t second = hardware::BOARD_LED_COUNT + 28U;
        modules::BoardLedFrame warningBoard{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> warningEdge{};
        std::array<modules::Rgb, 64> warningGrid{};
        modules::status_led_frame_adapter::FixedPhysicalStatusOverlay overlay{};
        const auto composePhysical = [&](const uint32_t nowMs) {
            fxe::ManualMillis::SetNowMs(nowMs);
            auto snapshot = test_support::BuildInteractionSnapshot(
                sm, derived, &engine);
            in.interactionSnapshot = &snapshot;
            modules::led_frame_composer::Compose(
                in, warningBoard, warningEdge, warningGrid, nullptr, nullptr,
                nullptr, nullptr, &overlay);
            auto physical = hardware::MapSemanticLedFrame(
                {hardware::BoardRevision::V2S3, boardRotate, boardRotate},
                warningBoard, warningEdge);
            hardware::ApplyFixedPhysicalStatusOverlay(
                hardware::BoardRevision::V2S3, overlay, physical);
            return physical;
        };
        auto physical = composePhysical(0U);
        Check(SameRgb(physical[first], modules::RGB_YELLOW) &&
                  !IsLit(physical[second]),
              "post-map health warning replaces the physical USB pair with yellow/dark at phase zero");
        physical = composePhysical(250U);
        Check(!IsLit(physical[first]) &&
                  SameRgb(physical[second], modules::RGB_ORANGE),
              "post-map health warning alternates to dark/orange without Turn-color mixing");
        physical = composePhysical(1000U);
        Check(!IsLit(physical[first]) && !IsLit(physical[second]),
              "post-map health warning keeps both physical USB pixels dark for the four-second rest");
    }

    if (g_failures == 0) {
        std::puts("[SIDE_MIDDLE_BAND_STATIC_TURN] ok");
        return 0;
    }

    std::printf("[SIDE_MIDDLE_BAND_STATIC_TURN] failures=%d\n", g_failures);
    return 1;
}
