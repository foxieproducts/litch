#pragma once

#include <cstdint>

#include "modules/rgb.hpp"

namespace modules::rgb_optical_calibration {

// Side-by-side V3 observation shows mixed RGB output washes toward white and
// the blue emitter contributes more apparent chroma than equal 8-bit phone RGB.
// Preserve neutral white/grey, first reduce the shared floor of chromatic RGB,
// then restrain blue above the red/green neutral component.
// This is a presentation transform: documents, BLE values, and settings remain
// exact canonical RGB. Raw RGBW diagnostics deliberately bypass it.
constexpr uint16_t S3_SHARED_FLOOR_REMOVAL_SCALE256 = 160U;
constexpr uint16_t S3_BLUE_CHROMA_SCALE256 = 144U;

constexpr Rgb PresentForS3(const Rgb color) {
    const uint8_t low = color.r < color.g
        ? (color.r < color.b ? color.r : color.b)
        : (color.g < color.b ? color.g : color.b);
    const uint8_t high = color.r > color.g
        ? (color.r > color.b ? color.r : color.b)
        : (color.g > color.b ? color.g : color.b);

    Rgb presented = color;
    if (low != 0U && low != high) {
        const uint8_t removal = static_cast<uint8_t>(
            (static_cast<uint16_t>(low) * S3_SHARED_FLOOR_REMOVAL_SCALE256 +
             128U) >> 8U);
        const uint16_t remainingHigh = static_cast<uint16_t>(high - removal);
        const auto rescale = [removal, remainingHigh, high](const uint8_t value) {
            const uint16_t reduced = static_cast<uint16_t>(value - removal);
            return static_cast<uint8_t>(
                (reduced * high + remainingHigh / 2U) / remainingHigh);
        };
        presented = {rescale(color.r), rescale(color.g), rescale(color.b)};
    }

    const uint8_t neutral = presented.r < presented.g
        ? presented.r : presented.g;
    if (presented.b <= neutral) {
        return presented;
    }

    const uint16_t chroma = static_cast<uint16_t>(presented.b - neutral);
    const uint8_t blue = static_cast<uint8_t>(
        neutral + ((chroma * S3_BLUE_CHROMA_SCALE256 + 128U) >> 8U));
    return {presented.r, presented.g, blue};
}

} // namespace modules::rgb_optical_calibration
