#pragma once

#include <cstdint>
#include <cstring>

#include "modules/settings_store.hpp"

namespace modules::light_square_appearance {

inline constexpr const char* KEY_MODE = "LIGHT_SQUARE_MODE";
inline constexpr const char* KEY_HUE = "LIGHT_SQUARE_HUE";
inline constexpr const char* KEY_BRIGHTNESS = "LIGHT_SQUARE_BRIGHTNESS";

inline constexpr const char* MODE_WHITE = "white";
inline constexpr const char* MODE_COLOR = "color";

inline constexpr uint8_t DEFAULT_HUE = 0U;
inline constexpr uint8_t DEFAULT_BRIGHTNESS = 8U;
inline constexpr uint8_t MIN_BRIGHTNESS = 1U;
inline constexpr uint8_t MAX_BRIGHTNESS = 8U;

inline bool UsesColor(const fxe::SettingsStore& settings) {
    const char* const mode = settings.ReadValue(KEY_MODE, MODE_WHITE);
    return mode != nullptr && std::strcmp(mode, MODE_COLOR) == 0;
}

inline uint8_t ReadHue(const fxe::SettingsStore& settings) {
    const uint32_t raw = settings.ReadValue(
        KEY_HUE, static_cast<uint32_t>(DEFAULT_HUE));
    return static_cast<uint8_t>(raw > 255U ? 255U : raw);
}

inline uint8_t ReadBrightness(const fxe::SettingsStore& settings) {
    const uint32_t raw = settings.ReadValue(
        KEY_BRIGHTNESS, static_cast<uint32_t>(DEFAULT_BRIGHTNESS));
    if (raw < MIN_BRIGHTNESS) {
        return MIN_BRIGHTNESS;
    }
    return static_cast<uint8_t>(raw > MAX_BRIGHTNESS ? MAX_BRIGHTNESS : raw);
}

} // namespace modules::light_square_appearance
