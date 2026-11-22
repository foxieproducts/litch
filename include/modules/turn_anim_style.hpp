#pragma once

#include <cstdint>

namespace modules {

// Player-turn indicator animation style.
enum class TurnAnimStyle : uint8_t {
    ActiveEndOnly = 0,
    ActiveEndPingpongSides = 1,
    BothEnds = 2,
    SideMiddleBand = 3,
    AwayFromActiveEnd = 4,
    None = 5,
};

} // namespace modules
