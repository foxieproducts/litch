#include <cstdint>

#include "modules/game_persistence_coordinator.hpp"

namespace modules {

bool GamePersistenceCoordinator::QueueLiveCpuMove(
    const uint8_t from,
    const uint8_t to,
    const bool isCapture,
    const uint8_t captureSquare,
    const char promotionSuffix) {
    if (!m_stateMachine.QueueCpuMove(
            from, to, isCapture, captureSquare, promotionSuffix)) {
        return false;
    }
    // Queueing hands the physical move to the players. Re-evaluate at this
    // exact boundary so a CPU-owned opening is recorded as valid progress and
    // an already-running CPU clock pauses without coordinator-loop leakage.
    UpdateGameClock();
    ClearTakebackGrace();
    return true;
}

} // namespace modules
