#pragma once

#include <array>
#include <cstdint>
#include "modules/rgb.hpp"

namespace modules::boot_fox_asset {
extern const std::array<Rgb, 256U> PIXELS;
extern const std::array<uint8_t, 32U> YELLOW_MASK;
bool IsYellow(uint16_t index);
} // namespace modules::boot_fox_asset
