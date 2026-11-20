#include <array>
#include <cstdio>

#include "modules/animation_timing.hpp"
#include "modules/board_led_frame.hpp"

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
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

} // namespace

int main() {
    std::puts("[LED_ANIMATION_TOKENS] start");

    {
        modules::animation::ValueToken token;
        token.phase.periodMs = 100U;
        token.phase.playback = modules::animation::PlaybackMode::Loop;
        token.curve.waveform = modules::animation::Waveform::Blink;
        token.curve.minValue = 0U;
        token.curve.maxValue = 255U;
        token.curve.onWidth255 = 127U;

        Check(modules::animation::SampleValue({0U}, token) == 255U,
              "loop blink starts on");
        Check(modules::animation::SampleValue({60U}, token) == 0U,
              "loop blink turns off after duty window");
        Check(modules::animation::SampleValue({100U}, token) == 255U,
              "loop blink repeats");
    }

    {
        modules::animation::ValueToken token;
        token.phase.periodMs = 120U;
        token.phase.playback = modules::animation::PlaybackMode::OneShot;
        token.curve.waveform = modules::animation::Waveform::Triangle;
        token.curve.minValue = 0U;
        token.curve.maxValue = 255U;

        Check(modules::animation::SampleValue({0U}, token) == 0U,
              "one-shot triangle starts at minimum");
        Check(modules::animation::SampleValue({60U}, token) >= 250U,
              "one-shot triangle peaks mid-flight");
        Check(modules::animation::SamplePhase({120U}, token.phase).finished,
              "one-shot phase reports finished at end");
        Check(modules::animation::SampleValue({120U}, token) == 0U,
              "one-shot triangle returns to minimum after completion");
    }

    {
        modules::animation::ValueToken token;
        token.phase.periodMs = 100U;
        token.phase.playback = modules::animation::PlaybackMode::PingPongLoop;
        token.curve.waveform = modules::animation::Waveform::Saw;
        token.curve.minValue = 0U;
        token.curve.maxValue = 255U;

        const uint8_t start = modules::animation::SampleValue({0U}, token);
        const uint8_t peak = modules::animation::SampleValue({100U}, token);
        const uint8_t returnValue = modules::animation::SampleValue({150U}, token);

        Check(start == 0U, "pingpong saw starts at minimum");
        Check(peak >= 250U, "pingpong saw reaches maximum at turnaround");
        Check(returnValue > 100U && returnValue < 200U,
              "pingpong saw comes back down on the return leg");
    }

    {
        modules::BoardSquareLeds square;
        modules::ApplyToBoardSquare(square,
                                    modules::BoardLedSlots::Right,
                                    modules::RGB_YELLOW,
                                    255U,
                                    modules::animation::BlendMode::Replace);

        Check(RgbEq(square.left, modules::RGB_BLACK),
              "slot-targeting leaves square0 untouched");
        Check(RgbEq(square.top, modules::RGB_BLACK) && RgbEq(square.bottom, modules::RGB_BLACK),
              "slot-targeting leaves piece untouched");
        Check(RgbEq(square.right, modules::RGB_YELLOW),
              "slot-targeting updates square2 independently");
    }

    if (g_failures == 0) {
        std::puts("[LED_ANIMATION_TOKENS] ok");
        return 0;
    }

    std::printf("[LED_ANIMATION_TOKENS] failures=%d\n", g_failures);
    return 1;
}