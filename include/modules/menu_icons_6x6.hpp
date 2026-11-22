#pragma once

#include <array>
#include <cstdint>

#include "modules/game_display_icon_overlay.hpp"
#include "modules/menu_option_indices.hpp"
#include "modules/rgb.hpp"

namespace modules {

namespace menu_icons_6x6 {

using IconPixels = std::array<Rgb, GameDisplayIconOverlay::ICON_PIXELS>;

// Human-editable 6x6 multicolor icons.
//
// Legend:
// - '.' = transparent
// - 'W' = white
// - 'Y' = yellow
// - 'G' = green
// - 'R' = red
// - 'B' = blue
// - 'C' = cyan
// - 'M' = magenta
// - 'O' = orange
struct IconDef {
    const char* rows[GameDisplayIconOverlay::ICON_H] = {};
};

// Standard Bluetooth rune: vertical stem, crossed strokes and two right lobes.
inline constexpr IconDef BLUETOOTH_ICON = {{
    "..BB..",
    "B.B.B.",
    ".BBB..",
    ".BBB..",
    "B.B.B.",
    "..BB..",
}};

inline bool ParseIcon(const IconDef& def, IconPixels& outPixels) {
    outPixels.fill(RGB_BLACK);

    for (uint8_t y = 0; y < GameDisplayIconOverlay::ICON_H; ++y) {
        const char* row = def.rows[y];
        if (row == nullptr) {
            return false;
        }

        for (uint8_t x = 0; x < GameDisplayIconOverlay::ICON_W; ++x) {
            const char c = row[x];
            const size_t idx = static_cast<size_t>(y) * GameDisplayIconOverlay::ICON_W + x;

            switch (c) {
                case '.':
                    outPixels[idx] = RGB_BLACK;
                    break;
                case 'W':
                    outPixels[idx] = RGB_WHITE;
                    break;
                case 'Y':
                    outPixels[idx] = RGB_YELLOW;
                    break;
                case 'G':
                    outPixels[idx] = RGB_GREEN;
                    break;
                case 'R':
                    outPixels[idx] = RGB_RED;
                    break;
                case 'B':
                    outPixels[idx] = RGB_BLUE;
                    break;
                case 'C':
                    outPixels[idx] = RGB_CYAN;
                    break;
                case 'M':
                    outPixels[idx] = RGB_MAGENTA;
                    break;
                case 'O':
                    outPixels[idx] = RGB_ORANGE;
                    break;
                default:
                    return false;
            }
        }
    }

    return true;
}

inline const IconDef* GetIconForOptionIndex(const uint8_t optionIndex) {
    // VS CPU: chip-like outline with a "pawn dot".
    static constexpr IconDef VS_CPU_ICON = {
        {
            ".BBBB.",
            "B.RR.B",
            "B.RC.B",
            "B.RR.B",
            "B..W.B",
            ".BBBB.",
        },
    };

    // CPU player: robot head. The following number selects player 1 or 2.
    static constexpr IconDef CPU_PLAYER_ICON = {
        {
            "..C...",
            "..C...",
            ".WWWW.",
            "W.BB.W",
            "W....W",
            ".WWWW.",
        },
    };

    // Player end: fixed opposing control-end rails and a two-way shuttle.
    static constexpr IconDef PLAYER_END_ICON = {
        {
            "BBBBBB",
            "..W...",
            ".WWW..",
            "..W...",
            ".WWW..",
            "RRRRRR",
        },
    };

    // BRIGHTNESS: sun.
    static constexpr IconDef BRIGHTNESS_ICON = {
        {
            "..Y...",
            ".YWWY.",
            "YWWWWY",
            "YWWWWY",
            ".YWWY.",
            "...Y..",
        },
    };

    // FLIP SIDES: two opposing arrows.
    static constexpr IconDef FLIP_ICON = {
        {
            "..G...",
            ".GGG..",
            "G..M.M",
            "..M..M",
            "..MMM.",
            "...M..",
        },
    };

    // SLEEP: crescent + stars.
    static constexpr IconDef SLEEP_ICON = {
        {
            "..W...",
            ".WBW..",
            "WB..W.",
            "WBW...",
            ".WBW..",
            "...W..",
        },
    };

    // Timer: clock face and hands.
    static constexpr IconDef TIMER_ICON = {
        {
            "..WW..",
            ".W..W.",
            "W.YY.W",
            "W..Y.W",
            ".W..W.",
            "..WW..",
        },
    };

    switch (optionIndex) {
        case menu_option_indices::VS_CPU:
            return &VS_CPU_ICON;
        case menu_option_indices::CPU_PLAYER:
            return &CPU_PLAYER_ICON;
        case menu_option_indices::BRIGHTNESS:
            return &BRIGHTNESS_ICON;
        case menu_option_indices::FLIP_SIDES:
            return &FLIP_ICON;
        case menu_option_indices::GAME_FIRST_PLAYER:
            return &PLAYER_END_ICON;
        case menu_option_indices::SLEEP:
            return &SLEEP_ICON;
        case menu_option_indices::TIMER:
            return &TIMER_ICON;
        default:
            return nullptr;
    }
}

} // namespace menu_icons_6x6

} // namespace modules
