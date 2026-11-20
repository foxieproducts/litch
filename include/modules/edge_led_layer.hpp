#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/edge_led_constants.hpp"
#include "modules/rgb.hpp"

namespace modules {

enum class EdgeLedLayerMode : uint8_t {
    Overlay = 0,
    Replace,
};

struct EdgeLedLayer {
    EdgeLedLayerMode mode = EdgeLedLayerMode::Overlay;
    uint8_t alpha255 = 255U;
    std::array<Rgb, EDGE_LED_COUNT> pixels{};
};

inline void ApplyEdgeLedLayer(
    const EdgeLedLayer& layer,
    std::array<Rgb, EDGE_LED_COUNT>& ioFrame) {
    const uint16_t overlayScale256 = layer.alpha255 == 255U
                                         ? 256U
                                         : static_cast<uint16_t>(
                                               (static_cast<uint16_t>(
                                                    layer.alpha255) *
                                                    256U +
                                                127U) /
                                               255U);
    for (size_t index = 0U; index < ioFrame.size(); ++index) {
        if (layer.mode == EdgeLedLayerMode::Replace) {
            ioFrame[index] = AlphaBlendRgb(
                ioFrame[index], layer.pixels[index], layer.alpha255);
            continue;
        }
        ioFrame[index] = AddRgbSaturating(
            ioFrame[index],
            ScaleRgbSaturatingBy256(
                layer.pixels[index], overlayScale256));
    }
}

} // namespace modules
