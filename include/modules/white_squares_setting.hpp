#pragma once

#include "modules/settings_store.hpp"
#include "modules/rgb.hpp"

namespace modules::white_squares_setting {

inline Rgb PreviewGlow(const uint32_t nowMs) {
    const uint16_t phase = static_cast<uint16_t>((nowMs / 6U) & 0x1FFU);
    const uint8_t triangle = phase < 256U ? static_cast<uint8_t>(phase)
        : static_cast<uint8_t>(511U - phase);
    const uint8_t value = static_cast<uint8_t>(48U + uint16_t(triangle) * 80U / 256U);
    return {value, value, value};
}

static constexpr const char* KEY_WHITE_SQUARES_ON = "WHITE_SQUARES_ON";
static constexpr const char* KEY_BRIGHTNESS = "BRIGHTNESS";

inline uint32_t ReadBrightness0To8(const modules::SettingsStore& settings) {
    uint32_t raw = settings.ReadValue(KEY_BRIGHTNESS, static_cast<uint32_t>(5U));
    if (raw > 8U) {
        raw = 8U;
    }
    return raw;
}

inline bool ReadWhiteSquaresOn(const modules::SettingsStore& settings) {
    return settings.ReadValue(KEY_WHITE_SQUARES_ON, true);
}

inline void WriteWhiteSquaresOn(modules::SettingsStore& settings, const bool whiteSquaresOn) {
    settings.Doc()[KEY_WHITE_SQUARES_ON] = whiteSquaresOn ? 1U : 0U;
    settings.MarkDirty();
}

inline void MigrateLegacyWhiteSquaresSetting(modules::SettingsStore& settings) {
    auto& doc = settings.Doc();
    if (doc.containsKey(KEY_WHITE_SQUARES_ON)) {
        return;
    }

    const uint32_t brightness = settings.ReadValue(KEY_BRIGHTNESS, static_cast<uint32_t>(5U));
    if (brightness != 0U) {
        return;
    }

    doc[KEY_WHITE_SQUARES_ON] = 0U;
    doc[KEY_BRIGHTNESS] = 1U;
    settings.MarkDirty();
}

} // namespace modules::white_squares_setting
