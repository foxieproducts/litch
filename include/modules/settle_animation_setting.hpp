#pragma once

#include <cstdint>
#include <cstring>

#include "modules/settings_store.hpp"

namespace modules {

enum class SettleAnimationStyle : uint8_t {
    LandingFlash = 0,
    MiniOrbit = 1,
    Ripple = 2,
    None = 3,
    RainbowOrbit = 4,
};

namespace settle_animation_setting {

inline constexpr const char* KEY = "SETTLE_ANIM_STYLE";
inline constexpr const char* DEFAULT_VALUE = "ripple";
inline constexpr SettleAnimationStyle DEFAULT_STYLE = SettleAnimationStyle::Ripple;
inline constexpr uint32_t RAINBOW_ORBIT_DURATION_MS = 750U;

inline SettleAnimationStyle Parse(const char* value) {
    if (value != nullptr && std::strcmp(value, "none") == 0) return SettleAnimationStyle::None;
    if (value != nullptr && std::strcmp(value, "landing_flash") == 0) return SettleAnimationStyle::LandingFlash;
    if (value != nullptr && std::strcmp(value, "mini_orbit") == 0) return SettleAnimationStyle::MiniOrbit;
    if (value != nullptr && std::strcmp(value, "ripple") == 0) return SettleAnimationStyle::Ripple;
    if (value != nullptr && std::strcmp(value, "rainbow_orbit") == 0) return SettleAnimationStyle::RainbowOrbit;
    return DEFAULT_STYLE;
}

inline uint32_t DurationMs(const SettleAnimationStyle style, const bool minimal) {
    if (style == SettleAnimationStyle::None) return 0U;
    if (minimal) return 220U;
    if (style == SettleAnimationStyle::LandingFlash) return 160U;
    if (style == SettleAnimationStyle::RainbowOrbit) return RAINBOW_ORBIT_DURATION_MS;
    return style == SettleAnimationStyle::Ripple ? 560U : 320U;
}

inline SettleAnimationStyle Read(const modules::SettingsStore& settings) {
    return Parse(settings.ReadValue(KEY, DEFAULT_VALUE));
}

} // namespace settle_animation_setting
} // namespace modules
