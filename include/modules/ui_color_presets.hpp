#pragma once

#include <array>
#include <cstdint>

#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"

namespace modules::ui_color_presets {

static constexpr std::array<modules::Rgb, 8> PLAYER_COLOR_PRESETS = {
    modules::Rgb{0, 128, 255},  // blue (default P1)
    modules::Rgb{255, 64, 0},   // orange
    modules::Rgb{0, 255, 0},    // green
    modules::Rgb{255, 0, 0},    // red (default P2)
    modules::Rgb{255, 255, 0},  // yellow
    modules::Rgb{255, 0, 255},  // magenta
    modules::Rgb{0, 255, 255},  // cyan
    modules::Rgb{255, 255, 255} // white
};

static constexpr std::array<modules::Rgb, 8> SQUARE_COLOR_PRESETS = {
    modules::Rgb{13, 13, 13}, // restrained gray (default light)
    modules::Rgb{20, 18, 12}, // warm
    modules::Rgb{12, 16, 20}, // cool
    modules::Rgb{12, 20, 12}, // green-ish
    modules::Rgb{18, 12, 20}, // purple-ish
    modules::Rgb{40, 40, 40}, // brighter gray
    modules::Rgb{10, 10, 10}, // dark gray (default dark)
    modules::Rgb{0, 0, 0},    // black
};

inline uint8_t ClampIndex0To7(const uint32_t raw) {
    return static_cast<uint8_t>((raw > 7U) ? 7U : raw);
}

inline uint8_t ReadIndex0To7(const fxe::SettingsStore& settings,
                             const char* key,
                             const uint8_t defaultValue) {
    const uint32_t raw = settings.ReadValue(key, static_cast<uint32_t>(defaultValue));
    return ClampIndex0To7(raw);
}

inline modules::Rgb ReadPlayerColor(const fxe::SettingsStore& settings,
                                    const char* key,
                                    const uint8_t defaultIndex) {
    const uint8_t idx = ReadIndex0To7(settings, key, defaultIndex);
    return PLAYER_COLOR_PRESETS[idx];
}

inline modules::Rgb ReadSquareColor(const fxe::SettingsStore& settings,
                                    const char* key,
                                    const uint8_t defaultIndex) {
    const uint8_t idx = ReadIndex0To7(settings, key, defaultIndex);
    return SQUARE_COLOR_PRESETS[idx];
}

} // namespace modules::ui_color_presets
