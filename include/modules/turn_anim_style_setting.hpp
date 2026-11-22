#pragma once

#include <cstring>

#include "modules/settings_store.hpp"
#include "modules/turn_anim_style.hpp"

namespace modules::turn_anim_style_setting {

static constexpr const char* KEY_TURN_ANIM_STYLE = "TURN_ANIM_STYLE";

static constexpr const char* VALUE_ACTIVE_END_ONLY = "active_end_only";
static constexpr const char* VALUE_ACTIVE_END_PINGPONG_SIDES = "active_end_pingpong_sides";
static constexpr const char* VALUE_BOTH_ENDS = "both_ends";
static constexpr const char* VALUE_SIDE_MIDDLE_BAND = "side_middle_band";

static constexpr const char* VALUE_AWAY_FROM_ACTIVE_END = "away_from_active_end";
static constexpr const char* VALUE_NONE = "none";

// Default preserves current (post-ec2ec3e674c9bd) behavior.
static constexpr const char* DEFAULT_VALUE = VALUE_ACTIVE_END_PINGPONG_SIDES;

inline TurnAnimStyle ParseTurnAnimStyle(const char* value) {
    if (value == nullptr) {
        return TurnAnimStyle::ActiveEndPingpongSides;
    }

    if (std::strcmp(value, VALUE_ACTIVE_END_ONLY) == 0) {
        return TurnAnimStyle::ActiveEndOnly;
    }
    if (std::strcmp(value, VALUE_ACTIVE_END_PINGPONG_SIDES) == 0) {
        return TurnAnimStyle::ActiveEndPingpongSides;
    }
    if (std::strcmp(value, VALUE_BOTH_ENDS) == 0) {
        return TurnAnimStyle::BothEnds;
    }
    if (std::strcmp(value, VALUE_SIDE_MIDDLE_BAND) == 0) {
        return TurnAnimStyle::SideMiddleBand;
    }

    if (std::strcmp(value, VALUE_AWAY_FROM_ACTIVE_END) == 0) {
        return TurnAnimStyle::AwayFromActiveEnd;
    }
    if (std::strcmp(value, VALUE_NONE) == 0) {
        return TurnAnimStyle::None;
    }

    // Unknown values fall back to the default behavior.
    return TurnAnimStyle::ActiveEndPingpongSides;
}

inline TurnAnimStyle ReadTurnAnimStyle(const modules::SettingsStore& settings) {
    const char* raw = settings.ReadValue(KEY_TURN_ANIM_STYLE, DEFAULT_VALUE);
    return ParseTurnAnimStyle(raw);
}

inline void WriteTurnAnimStyle(modules::SettingsStore& settings, const char* value) {
    settings.Doc()[KEY_TURN_ANIM_STYLE] = value;
    settings.MarkDirty();
}

} // namespace modules::turn_anim_style_setting
