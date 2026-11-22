#pragma once

#include "hardware/board_revision.hpp"

namespace hardware {

inline constexpr bool ShouldEnterHardwareTestFromBoot(
    const BoardRevision revision,
    const bool hasP2Controls,
    const bool p1UpPressed,
    const bool p2UpPressed) {
    return SupportsProductionHardwareTest(revision) &&
           (p1UpPressed || (hasP2Controls && p2UpPressed));
}

} // namespace hardware
