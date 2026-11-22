#pragma once

#include <cstdint>
#include <cstring>

#include "modules/settings_store.hpp"

namespace modules {

enum class CheckmateAnimationStyle : uint8_t {
    Fireworks = 0,
    FireLob = 1,
    RippleBloom = 2,
    WinnerSweep = 3,
    WinnerConstellation = 4,
};

namespace checkmate_animation_setting {

inline constexpr const char* KEY = "CHECKMATE_ANIM_STYLE";
inline constexpr const char* VALUE_FIREWORKS = "fireworks";
inline constexpr const char* VALUE_FIRE_LOB = "fire-lob";
inline constexpr const char* VALUE_RIPPLE_BLOOM = "ripple-bloom";
inline constexpr const char* VALUE_WINNER_SWEEP = "winner-sweep";
inline constexpr const char* VALUE_WINNER_CONSTELLATION =
    "winner-constellation";
inline constexpr const char* DEFAULT_VALUE = VALUE_FIRE_LOB;

inline CheckmateAnimationStyle Parse(const char* value) {
    if (value != nullptr && std::strcmp(value, VALUE_FIREWORKS) == 0) {
        return CheckmateAnimationStyle::Fireworks;
    }
    if (value != nullptr && std::strcmp(value, VALUE_FIRE_LOB) == 0) {
        return CheckmateAnimationStyle::FireLob;
    }
    if (value != nullptr && std::strcmp(value, VALUE_RIPPLE_BLOOM) == 0) {
        return CheckmateAnimationStyle::RippleBloom;
    }
    if (value != nullptr && std::strcmp(value, VALUE_WINNER_SWEEP) == 0) {
        return CheckmateAnimationStyle::WinnerSweep;
    }
    if (value != nullptr &&
        std::strcmp(value, VALUE_WINNER_CONSTELLATION) == 0) {
        return CheckmateAnimationStyle::WinnerConstellation;
    }
    return CheckmateAnimationStyle::FireLob;
}

inline CheckmateAnimationStyle Read(const modules::SettingsStore& settings) {
    return Parse(settings.ReadValue(KEY, DEFAULT_VALUE));
}

} // namespace checkmate_animation_setting
} // namespace modules
