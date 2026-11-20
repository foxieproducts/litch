#pragma once

#include "modules/engine.hpp"
#include "modules/game_persistence.hpp"

namespace modules {

// Bounded RAM evidence. Reads never enter the engine lane.
struct CpuPonderStatus {
    bool enabled = false;
    bool active = false;
    bool ready = false;
    uint32_t starts = 0U;
    uint32_t hits = 0U;
    uint32_t misses = 0U;
    uint32_t cancels = 0U;
    uint32_t nodes = 0U;
    bool nodesValid = false;
    uint32_t completedRootNodes = 0U;
    uint32_t elapsedMs = 0U;
    uint8_t completedDepth = 0U;
    bool fullRoot = false;
    bool reusedFloor = false;
    UciMove predictedHumanMove;
    UciMove reply;
    FenString sourceFen;
};

} // namespace modules
