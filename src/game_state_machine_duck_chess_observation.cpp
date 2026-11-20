#include "runtime/game_state_machine.hpp"

#include "runtime/chess_interaction_duck_projection.hpp"

namespace runtime {

uint8_t GameStateMachine::GetDeferredDuckDestinationForObservation(
    const uint64_t changedMask,
    const uint64_t occupiedMask) const {
    if (!m_validDestMaskKnown || m_cpuThinkingActive ||
        m_placementModeActive || m_kingQueenSwapPromptActive ||
        m_promotionUx.IsActive() || m_humanIllegalMask != 0ULL ||
        m_captureReturnMask != 0ULL) return CHESS_INTERACTION_INVALID_SQUARE;
    const ChessInteractionEvent event =
        ChessInteractionEvent::MakeBoardObservation(0U, 0U, changedMask, occupiedMask);
    const uint64_t landings = GetCoalescedDuckLandings(m_chessInteractionModel, event);
    const uint64_t chessLanding = landings & m_validDestMask;
    if (chessLanding == 0ULL ||
        (chessLanding & (chessLanding - 1ULL)) != 0ULL) {
        return CHESS_INTERACTION_INVALID_SQUARE;
    }
    const uint64_t duckLanding = landings & ~chessLanding;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((duckLanding & (1ULL << square)) != 0ULL) return square;
    }
    return CHESS_INTERACTION_INVALID_SQUARE;
}

} // namespace runtime
