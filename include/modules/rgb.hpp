#pragma once

#include "foxie_embedded/rgb.hpp"

namespace modules {

using Rgb = fxe::Rgb;
static constexpr Rgb RGB_BLACK = fxe::RGB_BLACK;
static constexpr Rgb RGB_WHITE = fxe::RGB_WHITE;
static constexpr Rgb RGB_RED = fxe::RGB_RED;
static constexpr Rgb RGB_GREEN = fxe::RGB_GREEN;
static constexpr Rgb RGB_BLUE = fxe::RGB_BLUE;
static constexpr Rgb RGB_YELLOW = fxe::RGB_YELLOW;
static constexpr Rgb RGB_CYAN = fxe::RGB_CYAN;
static constexpr Rgb RGB_MAGENTA = fxe::RGB_MAGENTA;
static constexpr Rgb RGB_ORANGE = fxe::RGB_ORANGE;

inline uint8_t ScaleU8By256(const uint8_t value, const uint16_t scale256) {
    const uint32_t scaled = static_cast<uint32_t>(value) * static_cast<uint32_t>(scale256);
    return static_cast<uint8_t>((scaled + 128U) >> 8);
}

inline uint8_t ScaleU8SaturatingBy256(const uint8_t value, const uint16_t scale256) {
    const uint32_t scaled = static_cast<uint32_t>(value) * static_cast<uint32_t>(scale256);
    const uint32_t out = (scaled + 128U) >> 8;
    return (out > 255U) ? 255U : static_cast<uint8_t>(out);
}

inline uint8_t AddU8Saturating(const uint8_t lhs, const uint8_t rhs) {
    const uint16_t sum = static_cast<uint16_t>(lhs) + static_cast<uint16_t>(rhs);
    return (sum > 255U) ? 255U : static_cast<uint8_t>(sum);
}

inline uint8_t AlphaBlendU8(const uint8_t base, const uint8_t overlay, const uint8_t alpha255) {
    const uint16_t alpha = static_cast<uint16_t>(alpha255);
    const uint16_t inverse = static_cast<uint16_t>(255U - alpha255);
    return static_cast<uint8_t>(
        (static_cast<uint16_t>(base) * inverse + static_cast<uint16_t>(overlay) * alpha + 127U) / 255U);
}

inline Rgb ScaleRgbSaturatingBy256(const Rgb color, const uint16_t scale256) {
    return Rgb{ScaleU8SaturatingBy256(color.r, scale256),
               ScaleU8SaturatingBy256(color.g, scale256),
               ScaleU8SaturatingBy256(color.b, scale256)};
}

inline Rgb AlphaBlendRgb(const Rgb base, const Rgb overlay, const uint8_t alpha255) {
    return Rgb{AlphaBlendU8(base.r, overlay.r, alpha255),
               AlphaBlendU8(base.g, overlay.g, alpha255),
               AlphaBlendU8(base.b, overlay.b, alpha255)};
}

inline Rgb AddRgbSaturating(const Rgb lhs, const Rgb rhs) {
    return Rgb{AddU8Saturating(lhs.r, rhs.r),
               AddU8Saturating(lhs.g, rhs.g),
               AddU8Saturating(lhs.b, rhs.b)};
}

} // namespace modules
