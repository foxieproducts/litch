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
inline constexpr uint8_t DEFAULT_BRIGHTNESS = 15U;
inline constexpr uint8_t MIN_BRIGHTNESS = 1U;
inline constexpr uint8_t MAX_BRIGHTNESS = 15U;
inline constexpr uint8_t LEGACY_LEVELS = 8U;

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
    // Stored 1..8 keep their original light levels. Slots 9..15 hold the
    // seven midpoints. The slider presents both in brightness order.
    const uint32_t stored = settings.ReadValue(KEY_BRIGHTNESS, uint32_t{LEGACY_LEVELS});
    if (stored == 0U) return MIN_BRIGHTNESS;
    if (stored <= LEGACY_LEVELS) return static_cast<uint8_t>(2U * stored - 1U);
    if (stored <= MAX_BRIGHTNESS) return static_cast<uint8_t>(2U * (stored - LEGACY_LEVELS));
    return MAX_BRIGHTNESS;
}

inline uint32_t StoredBrightness(uint32_t step) {
    return step % 2U == 0U ? LEGACY_LEVELS + step / 2U : (step + 1U) / 2U;
}

} // namespace modules::light_square_appearance
