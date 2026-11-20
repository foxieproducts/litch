#include <algorithm>
#include <array>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "modules/turn_anim_style_setting.hpp"
#include "modules/ui_color_wheel.hpp"
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

bool AnyLit(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& frame) {
    for (const modules::Rgb c : frame) {
        if (IsLit(c)) {
            return true;
        }
    }
    return false;
}

bool AllOffOutsideP1Home(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& frame) {
    for (uint8_t index = 0U; index < frame.size(); ++index) {
        if ((index < 16U || index >= 24U) && IsLit(frame[index])) {
            return false;
        }
    }
    return true;
}

template <size_t N>
bool FramesEqual(const std::array<modules::Rgb, N>& a,
                 const std::array<modules::Rgb, N>& b) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) {
            return false;
        }
    }
    return true;
}

uint32_t FrameDistance(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& a,
                       const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& b) {
    uint32_t distance = 0U;
    for (size_t i = 0; i < a.size(); ++i) {
        distance += (a[i].r > b[i].r) ? (a[i].r - b[i].r) : (b[i].r - a[i].r);
        distance += (a[i].g > b[i].g) ? (a[i].g - b[i].g) : (b[i].g - a[i].g);
        distance += (a[i].b > b[i].b) ? (a[i].b - b[i].b) : (b[i].b - a[i].b);
    }
    return distance;
}

bool EmbeddedStatusRegionIsDark(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& frame) {
    for (uint16_t led = hardware::BOARD_LED_COUNT;
         led < hardware::TOTAL_LED_COUNT;
         ++led) {
        if (IsLit(frame[led])) {
            return false;
        }
    }
    return true;
}

uint8_t MaxChannel(const modules::Rgb color) {
    return std::max(color.r, std::max(color.g, color.b));
}

uint32_t NormalizedColorDistanceSq(const modules::Rgb left,
                                   const modules::Rgb right) {
    const uint8_t leftMax = MaxChannel(left);
    const uint8_t rightMax = MaxChannel(right);
    if (leftMax == 0U || rightMax == 0U) return UINT32_MAX;
    const auto normalize = [](const uint8_t channel, const uint8_t maximum) {
        return static_cast<int32_t>(
            (static_cast<uint32_t>(channel) * 255U + maximum / 2U) /
            maximum);
    };
    const int32_t red = normalize(left.r, leftMax) -
                        normalize(right.r, rightMax);
    const int32_t green = normalize(left.g, leftMax) -
                          normalize(right.g, rightMax);
    const int32_t blue = normalize(left.b, leftMax) -
                         normalize(right.b, rightMax);
    return static_cast<uint32_t>(red * red + green * green + blue * blue);
}

bool EveryLitPixelUsesHue(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& frame,
    const modules::Rgb expected) {
    bool found = false;
    for (const modules::Rgb color : frame) {
        if (!IsLit(color)) continue;
        found = true;
        if (NormalizedColorDistanceSq(color, expected) > 9U) return false;
    }
    return found;
}

} // namespace

int main() {
    std::puts("[PINGPONG_TURN_ANIMATES] start");

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_pingpong_turn_animates.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    settings.Doc()[modules::turn_anim_style_setting::KEY_TURN_ANIM_STYLE] =
        modules::turn_anim_style_setting::VALUE_ACTIVE_END_PINGPONG_SIDES;
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

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame2{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame2{};
    std::array<modules::Rgb, 64> gridFrame2{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrameCycle{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrameCycle{};
    std::array<modules::Rgb, 64> gridFrameCycle{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledEndpointBefore{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeEndpointBefore{};
    std::array<modules::Rgb, 64> gridEndpointBefore{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledEndpoint{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeEndpoint{};
    std::array<modules::Rgb, 64> gridEndpoint{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledEndpointAfter{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeEndpointAfter{};
    std::array<modules::Rgb, 64> gridEndpointAfter{};

    fxe::ManualMillis::SetNowMs(0U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame0, edgeFrame0, gridFrame0);

    fxe::ManualMillis::SetNowMs(20U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame1, edgeFrame1, gridFrame1);

    fxe::ManualMillis::SetNowMs(40U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame2, edgeFrame2, gridFrame2);

    fxe::ManualMillis::SetNowMs(2240U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrameCycle, edgeFrameCycle, gridFrameCycle);

    fxe::ManualMillis::SetNowMs(780U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine,
        ledEndpointBefore, edgeEndpointBefore, gridEndpointBefore);
    fxe::ManualMillis::SetNowMs(800U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledEndpoint, edgeEndpoint, gridEndpoint);
    fxe::ManualMillis::SetNowMs(820U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine,
        ledEndpointAfter, edgeEndpointAfter, gridEndpointAfter);

    Check(AnyLit(edgeFrame0), "pingpong frame at t0 emits edge light");
    Check(AnyLit(edgeFrame1), "pingpong frame at t1 emits edge light");
    Check(!FramesEqual(edgeFrame0, edgeFrame1),
          "smooth turn indicator advances between 20 ms render frames");
    Check(!FramesEqual(edgeFrame1, edgeFrame2),
          "smooth turn indicator keeps advancing without whole-LED dwell");
    Check(FrameDistance(edgeFrame0, edgeFrame1) < 160U &&
              FrameDistance(edgeFrame1, edgeFrame2) < 160U,
          "adjacent turn frames change by a bounded amount");
    Check(FramesEqual(edgeFrame0, edgeFrameCycle),
          "smooth turn indicator repeats after one 2240 ms round trip");
    Check(!FramesEqual(edgeEndpointBefore, edgeEndpoint) &&
              !FramesEqual(edgeEndpoint, edgeEndpointAfter),
          "smooth turn indicator reverses at the endpoint without a dwell frame");
    Check(FrameDistance(edgeEndpointBefore, edgeEndpoint) < 160U &&
              FrameDistance(edgeEndpoint, edgeEndpointAfter) < 160U,
          "turn endpoint reversal remains continuous");
    Check(AllOffOutsideP1Home(edgeFrame0) &&
              AllOffOutsideP1Home(edgeEndpoint),
          "default Turn indicator stays confined to the active home edge");
    Check(EmbeddedStatusRegionIsDark(ledFrame0) &&
              EmbeddedStatusRegionIsDark(ledFrame1),
          "composer keeps status output semantic instead of embedding a target-specific chain");

    static constexpr const char* TURN_STYLES[] = {
        modules::turn_anim_style_setting::VALUE_ACTIVE_END_ONLY,
        modules::turn_anim_style_setting::VALUE_ACTIVE_END_PINGPONG_SIDES,
        modules::turn_anim_style_setting::VALUE_BOTH_ENDS,
        modules::turn_anim_style_setting::VALUE_SIDE_MIDDLE_BAND,
    };
    const modules::Rgb directP1Color =
        modules::ui_color_wheel::WheelToRgb(170U);
    for (const char* style : TURN_STYLES) {
        modules::turn_anim_style_setting::WriteTurnAnimStyle(settings, style);
        for (const uint32_t nowMs : {100U, 800U}) {
            std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> styleLed{};
            std::array<modules::Rgb, modules::EDGE_LED_COUNT> styleEdge{};
            std::array<modules::Rgb, 64> styleGrid{};
            fxe::ManualMillis::SetNowMs(nowMs);
            test_support::ComposeWithSnapshot(
                in, sm, derived, &engine, styleLed, styleEdge, styleGrid);
            Check(EveryLitPixelUsesHue(styleEdge, directP1Color),
                  "every Turn style and phase uses the direct player color");
        }
    }

    settings.Doc()["MINIMAL_ANIMATIONS"] = true;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> minimalLed0{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> minimalEdge0{};
    std::array<modules::Rgb, 64> minimalGrid0{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> minimalLed1{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> minimalEdge1{};
    std::array<modules::Rgb, 64> minimalGrid1{};
    fxe::ManualMillis::SetNowMs(0U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, minimalLed0, minimalEdge0, minimalGrid0);
    fxe::ManualMillis::SetNowMs(700U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, minimalLed1, minimalEdge1, minimalGrid1);

    Check(FramesEqual(minimalEdge0, minimalEdge1),
          "Minimal Animations replaces Turn motion with a static cue");
    Check(AllOffOutsideP1Home(minimalEdge0),
          "Minimal Turn cue stays on the active home edge");
    for (uint8_t index = 0U; index < 8U; ++index) {
        const bool expectedLit = index >= 2U && index <= 5U;
        Check(IsLit(minimalEdge0[static_cast<uint8_t>(16U + index)]) ==
                  expectedLit,
              "Minimal Turn cue uses the four readable center LEDs");
    }
    Check(!FramesEqual(edgeFrame0, minimalEdge0),
          "Minimal Turn cue is distinct from the moving default");

    for (const char* style : TURN_STYLES) {
        modules::turn_anim_style_setting::WriteTurnAnimStyle(settings, style);
        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> styleLedA{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> styleEdgeA{};
        std::array<modules::Rgb, 64> styleGridA{};
        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> styleLedB{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> styleEdgeB{};
        std::array<modules::Rgb, 64> styleGridB{};
        fxe::ManualMillis::SetNowMs(100U);
        test_support::ComposeWithSnapshot(
            in, sm, derived, &engine, styleLedA, styleEdgeA, styleGridA);
        fxe::ManualMillis::SetNowMs(800U);
        test_support::ComposeWithSnapshot(
            in, sm, derived, &engine, styleLedB, styleEdgeB, styleGridB);
        Check(FramesEqual(styleLedA, styleLedB) &&
                  FramesEqual(styleEdgeA, styleEdgeB) &&
                  FramesEqual(styleGridA, styleGridB),
              "Minimal Turn output is time-invariant for every stored style");
    }

    sm.SetLastMoveForSide(modules::PersistedSide::White, 52U, 36U);
    sm.SetLastMoveMostRecentSide(modules::PersistedSide::White);
    sm.SetCpuThinkingActive(true);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> thinkingLedA{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> thinkingEdgeA{};
    std::array<modules::Rgb, 64> thinkingGridA{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> thinkingLedB{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> thinkingEdgeB{};
    std::array<modules::Rgb, 64> thinkingGridB{};
    fxe::ManualMillis::SetNowMs(100U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine,
        thinkingLedA, thinkingEdgeA, thinkingGridA);
    fxe::ManualMillis::SetNowMs(800U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine,
        thinkingLedB, thinkingEdgeB, thinkingGridB);
    Check(FramesEqual(thinkingLedA, thinkingLedB) &&
              FramesEqual(thinkingEdgeA, thinkingEdgeB) &&
              FramesEqual(thinkingGridA, thinkingGridB),
          "Minimal Thinking output is time-invariant");
    Check(AnyLit(thinkingEdgeA),
          "Minimal Thinking retains a visible owner-local cue");
    sm.SetCpuThinkingActive(false);

    derived.inDraw = true;
    derived.drawReason = modules::DrawReason::InsufficientMaterial;
    derived.drawStartValid = true;
    derived.drawStartMs = 0U;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> drawLedA{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> drawEdgeA{};
    std::array<modules::Rgb, 64> drawGridA{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> drawLedB{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> drawEdgeB{};
    std::array<modules::Rgb, 64> drawGridB{};
    fxe::ManualMillis::SetNowMs(100U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, drawLedA, drawEdgeA, drawGridA);
    fxe::ManualMillis::SetNowMs(800U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, drawLedB, drawEdgeB, drawGridB);
    Check(FramesEqual(drawLedA, drawLedB) &&
              FramesEqual(drawEdgeA, drawEdgeB) &&
              FramesEqual(drawGridA, drawGridB),
          "Minimal Draw full-frame output is time-invariant");
    Check(AnyLit(drawEdgeA),
          "Minimal Draw retains a visible result cue");

    if (g_failures == 0) {
        std::puts("[PINGPONG_TURN_ANIMATES] ok");
        return 0;
    }

    std::printf("[PINGPONG_TURN_ANIMATES] failures=%d\n", g_failures);
    return 1;
}
