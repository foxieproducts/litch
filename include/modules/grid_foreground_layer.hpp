#pragma once

#include <array>
#include <cstdint>

#include "modules/led_text_renderer.hpp"
#include "modules/rgb.hpp"

namespace modules {

// Explicit square-surface ownership for a rasterized foreground. Background
// and foreground operation are retained separately so output mapping can remove the
// semantic-orientation copy and place the foreground at its literal physical
// orientation without inferring coverage from RGB differences.
struct GridForegroundLayer {
    std::array<Rgb, led_text_renderer::GRID_PIXELS> background{};
    std::array<Rgb, led_text_renderer::GRID_PIXELS> colors{};
    uint64_t sourceCoverageMask = 0ULL;
    uint64_t placementCoverageMask = 0ULL;
    uint16_t brightnessScale256 = 256U;
    uint8_t alpha255 = 0U;

    void Clear() {
        background.fill(RGB_BLACK);
        colors.fill(RGB_BLACK);
        sourceCoverageMask = 0ULL;
        placementCoverageMask = 0ULL;
        brightnessScale256 = 256U;
        alpha255 = 0U;
    }

    bool IsVisible() const {
        return sourceCoverageMask != 0ULL || placementCoverageMask != 0ULL;
    }
};

} // namespace modules
