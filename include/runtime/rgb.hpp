#pragma once

#include <cstdint>

#include "foxie_embedded/rgb.hpp"

namespace runtime {

// Compatibility: keep the runtime-facing name, but share the canonical RGB type.
using Rgb8 = modules::Rgb;

constexpr Rgb8 RGB_BLACK = modules::RGB_BLACK;
constexpr Rgb8 RGB_WHITE = modules::RGB_WHITE;
constexpr Rgb8 RGB_GREEN = modules::RGB_GREEN;
constexpr Rgb8 RGB_YELLOW = modules::RGB_YELLOW;

constexpr Rgb8 Scale(const Rgb8 c, const uint8_t scale255) {
    return Rgb8{static_cast<uint8_t>((static_cast<uint16_t>(c.r) * scale255) / 255U),
                static_cast<uint8_t>((static_cast<uint16_t>(c.g) * scale255) / 255U),
                static_cast<uint8_t>((static_cast<uint16_t>(c.b) * scale255) / 255U)};
}

} // namespace runtime
