#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "hardware/led_layout.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/rgb.hpp"

namespace {

int g_failures = 0;

bool RgbEqual(const modules::Rgb lhs, const modules::Rgb rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

void Check(const bool condition, const char* label) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", label);
    ++g_failures;
}

using GridFrame = std::array<modules::Rgb, 64>;

GridFrame Render(const uint32_t elapsedMs) {
    modules::BoardLedFrame ledFrame{};
    GridFrame gridFrame{};
    const modules::led_frame_composer::internal::CheckmateBoardAnimationInputs inputs{
        modules::CheckmateAnimationStyle::Fireworks,
        elapsedMs,
        0x12345678U,
        0ULL,
        0ULL,
        255U,
    };
    modules::led_frame_composer::internal::ApplyCheckmateBoardAnimation(
        inputs, ledFrame, gridFrame);
    return gridFrame;
}

uint32_t FrameDistance(const GridFrame& a, const GridFrame& b) {
    uint32_t distance = 0U;
    for (size_t i = 0U; i < a.size(); ++i) {
        distance += (a[i].r > b[i].r) ? (a[i].r - b[i].r) : (b[i].r - a[i].r);
        distance += (a[i].g > b[i].g) ? (a[i].g - b[i].g) : (b[i].g - a[i].g);
        distance += (a[i].b > b[i].b) ? (a[i].b - b[i].b) : (b[i].b - a[i].b);
    }
    return distance;
}

bool AnyLit(const GridFrame& frame) {
    for (const modules::Rgb color : frame) {
        if (!RgbEqual(color, modules::RGB_BLACK)) {
            return true;
        }
    }
    return false;
}

bool FramesEqual(const GridFrame& a, const GridFrame& b) {
    for (size_t i = 0U; i < a.size(); ++i) {
        if (!RgbEqual(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    std::puts("[CHECKMATE_FIREWORKS_FIDELITY] start");

    // Intentional replacement for the historical pixel golden: animation
    // quality is now protected as deterministic temporal continuity rather
    // than preserving the whole-square jumps that caused the regression.
    const GridFrame frame913 = Render(913U);
    const GridFrame frame933 = Render(933U);
    const GridFrame frame953 = Render(953U);
    Check(AnyLit(frame913) && AnyLit(frame933) && AnyLit(frame953),
          "delayed particle launches remain visible");
    Check(FrameDistance(frame913, frame933) > 0U &&
              FrameDistance(frame933, frame953) > 0U,
          "particle energy advances on consecutive 20 ms render frames");
    Check(FrameDistance(frame913, frame933) < 300U &&
              FrameDistance(frame933, frame953) < 300U,
          "20 ms particle changes remain bounded without full-frame jumps");
    Check(FramesEqual(Render(1313U), Render(1313U)),
          "same seed and elapsed time render deterministically");

    // For this fixed seed an epoch that began two windows ago is still active
    // here. The renderer must carry it across 3000 ms rather than dropping it
    // solely because the current epoch number changed.
    const GridFrame beforeRollover = Render(2999U);
    const GridFrame afterRollover = Render(3000U);
    Check(AnyLit(beforeRollover) && AnyLit(afterRollover),
          "still-active fireworks survive the epoch rollover");
    Check(FrameDistance(beforeRollover, afterRollover) < 300U,
          "one-millisecond epoch rollover is visually continuous");

    uint32_t maxTwentyMsDistance = 0U;
    GridFrame prior = Render(0U);
    for (uint32_t elapsedMs = 20U; elapsedMs <= 4000U; elapsedMs += 20U) {
        const GridFrame current = Render(elapsedMs);
        const uint32_t distance = FrameDistance(prior, current);
        if (distance > maxTwentyMsDistance) {
            maxTwentyMsDistance = distance;
        }
        prior = current;
    }
    std::printf("[CHECKMATE_FIREWORKS_FIDELITY] max_20ms_distance=%u\n",
                static_cast<unsigned>(maxTwentyMsDistance));
    Check(maxTwentyMsDistance < 300U,
          "every 20 ms frame across four epochs stays within the continuity bound");

    if (g_failures == 0) {
        std::puts("[CHECKMATE_FIREWORKS_FIDELITY] ok");
        return 0;
    }
    std::printf("[CHECKMATE_FIREWORKS_FIDELITY] failures=%d\n", g_failures);
    return 1;
}
