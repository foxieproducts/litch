#pragma once

#include <cstdint>

#include "modules/engine.hpp"

namespace modules {

// VS-CPU policy:
// - VS_CPU_PLAYER selects which player the CPU controls (P2 by default).
// - P1 is always White and P2 is always Black.
// - GAME_FIRST_PLAYER and BOARD_ROTATE affect physical mapping only.
constexpr SideToMove GetVsCpuSide(const bool firstPlayerIsP2,
                                  const bool cpuPlayerIsP2 = true) {
    (void)firstPlayerIsP2;
    return cpuPlayerIsP2 ? SideToMove::Black : SideToMove::White;
}

constexpr bool IsVsCpuTurn(const SideToMove sideToMove,
                           const uint32_t vsCpuLevel,
                           const bool firstPlayerIsP2,
                           const bool cpuPlayerIsP2 = true) {
    return (vsCpuLevel != 0U) &&
           (sideToMove == GetVsCpuSide(firstPlayerIsP2, cpuPlayerIsP2));
}

// Rewind a complete human decision cycle only after the CPU has replied.
// While the engine is waiting on the CPU side, the latest committed move was
// the human's, so one ply is enough to return control to that human. This also
// covers a human-delivered terminal move, where no CPU reply will follow.
constexpr uint8_t GetVsCpuUndoPlyCount(const SideToMove sideToMove,
                                       const uint32_t vsCpuLevel,
                                       const bool firstPlayerIsP2,
                                       const bool cpuPlayerIsP2 = true) {
    if (vsCpuLevel == 0U ||
        IsVsCpuTurn(sideToMove, vsCpuLevel, firstPlayerIsP2, cpuPlayerIsP2)) {
        return 1U;
    }
    return 2U;
}

} // namespace modules
