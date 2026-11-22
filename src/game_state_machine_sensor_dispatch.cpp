#include "runtime/game_state_machine.hpp"

#include "logger.hpp"

namespace runtime {

void GameStateMachine::HandleSensorChange(const uint8_t square,
                                          const bool nowOccupied) {
    const uint32_t nowMs = fxe::Millis();
    HandleSensorChange(square,
                       nowOccupied,
                       nowMs,
                       m_chessInteractionModel.board.sequence + 1U);
}

void GameStateMachine::HandleSensorChange(const uint8_t square,
                                          const bool nowOccupied,
                                          const uint32_t tMs,
                                          const uint32_t sequence) {
    if (square >= 64U) {
        return;
    }
    const uint64_t squareMask = 1ULL << square;
    const uint64_t occupiedMask = nowOccupied
                                      ? m_chessInteractionModel.board.occupiedMask | squareMask
                                      : m_chessInteractionModel.board.occupiedMask & ~squareMask;
    logger::Trace("sensor-adapter: sq=%u occ=%u",
                  static_cast<unsigned>(square),
                  nowOccupied ? 1U : 0U);
    HandleBoardObservation(squareMask,
                           occupiedMask,
                           tMs,
                           sequence);
}

} // namespace runtime
