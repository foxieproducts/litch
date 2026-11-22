#pragma once

#include <cstdint>

namespace modules {

// Logical owner inside the current game/UI. This is not a physical PCB control
// bank and not a chess color. The current game contract maps P1 to White and P2
// to Black; orientation maps either logical owner onto a physical board end.
enum class UiOwner : uint8_t {
    None = 0,
    P1 = 1,
    P2 = 2,
};

// Logical two-player slot used by owner-scoped UI state. Use PersistedSide or
// engine side types where White/Black chess identity is what matters.
enum class Side : uint8_t {
    P1 = 0,
    P2 = 1,
};

} // namespace modules
