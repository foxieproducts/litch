#pragma once

#include <algorithm>
#include <cstdint>

#include "modules/rgb.hpp"

namespace modules {

struct RgbwChannels {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t w = 0;
};

inline uint8_t ScaleU8By256Floor(const uint8_t value, const uint16_t scale256) {
    const uint32_t scaled = static_cast<uint32_t>(value) * std::min<uint16_t>(scale256, 256U);
    return static_cast<uint8_t>(scaled >> 8);
}

inline RgbwChannels AutoMapRgbToRgbw(Rgb rgb, const uint8_t coolToWarmThreshold,
                                     const uint16_t whiteMixScale256 = 256U) {
    // Preserve the established neutral rendering outside scoped presentation
    // modes. Litchcraft supplies a lower whiteMixScale256 and uses the same
    // bounded extraction path as every other colour.
    if (whiteMixScale256 >= 256U && rgb.r != 0 && rgb.r == rgb.g && rgb.g == rgb.b) {
        return RgbwChannels{rgb.r, rgb.g, rgb.b, rgb.r};
    }

    const uint8_t minRgb = std::min(rgb.r, std::min(rgb.g, rgb.b));
    uint8_t w = 0;
    if (minRgb > 0) {
        const uint8_t blueMax = std::max(rgb.r, rgb.g);
        const uint8_t blueBias = (rgb.b > blueMax) ? static_cast<uint8_t>(rgb.b - blueMax) : 0;

        const uint16_t denom = static_cast<uint16_t>(255U - coolToWarmThreshold);
        uint8_t warmness = 255;
        if (blueBias <= coolToWarmThreshold) {
            warmness = 255;
        } else if (denom == 0U) {
            warmness = 0;
        } else {
            const uint16_t over = static_cast<uint16_t>(blueBias - coolToWarmThreshold);
            const uint16_t scaled = (over * 255U) / denom;
            warmness = (scaled >= 255U) ? 0U : static_cast<uint8_t>(255U - scaled);
        }

        w = ScaleU8By256Floor(
            ScaleU8By256Floor(minRgb, warmness), whiteMixScale256);
        rgb.r = static_cast<uint8_t>(rgb.r - w);
        rgb.g = static_cast<uint8_t>(rgb.g - w);
        rgb.b = static_cast<uint8_t>(rgb.b - w);
    }

    return RgbwChannels{rgb.r, rgb.g, rgb.b, w};
}

} // namespace modules
