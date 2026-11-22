#pragma once

#include <string_view>

namespace modules::hint_thinking_style_setting {
inline constexpr const char* KEY = "HINT_THINKING_STYLE";
inline constexpr const char* DEFAULT_VALUE = "cauldron_brewing";
enum class Style { FoxieSweep, PlayerClockwise, None, Beachball, CauldronBrewing };
inline Style Parse(const char* value) {
    const std::string_view token = value != nullptr ? value : "";
    if (token == "cauldron_brewing") return Style::CauldronBrewing;
    if (token == "beachball") return Style::Beachball;
    if (token == "none") return Style::None;
    if (token == "foxie_sweep") return Style::FoxieSweep;
    return token == "player_clockwise" ? Style::PlayerClockwise : Style::CauldronBrewing;
}
} // namespace modules::hint_thinking_style_setting
