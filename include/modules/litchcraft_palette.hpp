#pragma once

#include "modules/ui_color_wheel.hpp"

namespace modules::litchcraft_palette {
// Litchcraft's 32 visible swatches retain wheel hue at full RGB value. The
// shared player/fine-adjustment wheel deliberately keeps its existing mapping.
constexpr Rgb Color(const uint8_t index) {
    const auto raw = ui_color_wheel::WheelToRgb(static_cast<uint8_t>(index * 8U));
    const uint8_t rg = raw.r > raw.g ? raw.r : raw.g;
    const uint8_t peak = rg > raw.b ? rg : raw.b;
    const auto scale = [peak](const uint8_t value) constexpr {
        return static_cast<uint8_t>((uint32_t(value) * 255U + peak / 2U) / peak);
    };
    return {scale(raw.r), scale(raw.g), scale(raw.b)};
}
} // namespace modules::litchcraft_palette
