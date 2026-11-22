#pragma once

namespace modules::ui_color_keys {

static constexpr const char* P1_COLOR_INDEX = "P1_COLOR_INDEX";
static constexpr const char* P2_COLOR_INDEX = "P2_COLOR_INDEX";

// 0..255 color wheel positions for piece LEDs.
// These deployed keys remain logical-player compatibility aliases at protocol
// boundaries. New firmware keeps them synchronized for rollback to older
// firmware, while rendering from the physical-end keys below.
static constexpr const char* P1_PIECE_WHEEL = "P1_PIECE_WHEEL";
static constexpr const char* P2_PIECE_WHEEL = "P2_PIECE_WHEEL";

// Canonical physical-control-end palettes. They are available through direct
// settings read/write, but deliberately omitted from enumeration so an older
// app does not show duplicate color controls.
static constexpr const char* P1_END_PIECE_WHEEL = "P1_END_PIECE_WHEEL";
static constexpr const char* P2_END_PIECE_WHEEL = "P2_END_PIECE_WHEEL";
} // namespace modules::ui_color_keys
