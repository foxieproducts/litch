#pragma once

#include <cstdint>
#include <cstring>

#include "modules/brightness_scale_policy.hpp"
#include "modules/settings_store.hpp"

namespace modules::light_square_appearance {

inline constexpr const char* KEY_MODE = "LIGHT_SQUARE_MODE";
inline constexpr const char* KEY_HUE = "LIGHT_SQUARE_HUE";
inline constexpr const char* KEY_BRIGHTNESS = "LIGHT_SQUARE_BRIGHTNESS";

inline constexpr const char* MODE_WHITE = "white";
inline constexpr const char* MODE_COLOR = "color";

inline constexpr uint8_t DEFAULT_HUE = 0U;
inline constexpr uint8_t DEFAULT_BRIGHTNESS = 4U;
inline constexpr uint8_t MIN_BRIGHTNESS = 1U;
inline constexpr uint8_t MAX_BRIGHTNESS = 8U;
inline constexpr const char* KEY_WIDE_RANGE = "LIGHT_SQUARE_RANGE_V2";

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
    if (!settings.Doc().containsKey(KEY_BRIGHTNESS)) return DEFAULT_BRIGHTNESS;
    const uint32_t stored = settings.ReadValue(KEY_BRIGHTNESS, uint32_t{MAX_BRIGHTNESS});
    const uint8_t step = stored < MIN_BRIGHTNESS ? MIN_BRIGHTNESS :
                         stored > MAX_BRIGHTNESS ? MAX_BRIGHTNESS : static_cast<uint8_t>(stored);
    // Pre-1.0.5 filled its entire range below the new midpoint. Preserve the
    // closest setting on upgrade, including the exact previous default.
    constexpr uint8_t previousSteps[] = {1, 2, 2, 3, 3, 3, 4, 4};
    return settings.ReadValue(KEY_WIDE_RANGE, false) ? step : previousSteps[step - 1U];
}

inline void StoreBrightness(fxe::SettingsStore& settings, uint32_t step) {
    settings.Doc()[KEY_BRIGHTNESS] = step;
    settings.Doc()[KEY_WIDE_RANGE] = true;
}

inline uint8_t MinimumVisibleChannel(uint32_t displayLevel, bool automatic) {
    // Allow one output count on a lit emitter even at the dark end of the
    // ambient curve. The normal compositor boost, fade and current budget
    // still apply to the entire square.
    const uint16_t scale = brightness_scale_policy::ComputeEffectiveScale256(
        displayLevel, false, 0.0f, 256U, automatic);
    return static_cast<uint8_t>((128U + scale - 1U) / scale);
}

} // namespace modules::light_square_appearance
