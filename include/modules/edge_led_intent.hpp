#pragma once

#include <cstdint>

#include "modules/ui_types.hpp"

namespace modules {

struct EdgeLedIntent {
    // Menu/hold contribution only. Game-over, correction, thinking, and turn
    // priority come from the immutable interaction snapshot and are resolved
    // once by the frame composer's RenderIntent.
    bool menuActive = false;
    UiOwner menuOwner = UiOwner::None;
    uint8_t menuSelectionIndex = 0; // 0-based menu option index
    uint8_t menuValue01_255 = 0;    // bar fill 0..255
    bool menuHoldActive = false;
    bool menuHoldFillAll = false;
};

} // namespace modules
