#pragma once

#include <cstdint>

namespace fxe {

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    constexpr Rgb() = default;
    constexpr Rgb(const uint8_t r_, const uint8_t g_, const uint8_t b_) : r(r_), g(g_), b(b_) {}
};

constexpr Rgb RGB_BLACK(0, 0, 0);
constexpr Rgb RGB_WHITE(255, 255, 255);
constexpr Rgb RGB_GREEN(0, 255, 0);
constexpr Rgb RGB_YELLOW(255, 255, 0);

constexpr Rgb RGB_RED(255, 0, 0);
constexpr Rgb RGB_BLUE(0, 0, 255);
constexpr Rgb RGB_CYAN(0, 255, 255);
constexpr Rgb RGB_MAGENTA(255, 0, 255);
constexpr Rgb RGB_ORANGE(255, 128, 0);

} // namespace fxe
