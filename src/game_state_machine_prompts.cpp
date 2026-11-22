#include "runtime/game_state_machine.hpp"

namespace runtime {

void GameStateMachine::TriggerAllPiecesFlash(const uint32_t durationMs) {
    runtime::TriggerAllPiecesFlash(m_renderState, fxe::Millis(), durationMs);
}

bool GameStateMachine::IsAllPiecesFlashActive() const {
    return runtime::IsAllPiecesFlashActive(m_renderState, fxe::Millis());
}

bool GameStateMachine::IsAllPiecesFlashOn() const {
    return runtime::IsAllPiecesFlashOn(m_renderState, fxe::Millis());
}

void GameStateMachine::TriggerInitialKingPlacementPrompt() {
    uint64_t kingSquares = GetInteractionExpectedPosition().kingMask;
    if (kingSquares == 0ULL) {
        kingSquares = (1ULL << 4U) | (1ULL << 60U);
    }
    runtime::TriggerInitialKingPlacementPrompt(
        m_renderState, kingSquares);
}

void GameStateMachine::CancelInitialKingPlacementPrompt() {
    runtime::CancelInitialKingPlacementPrompt(m_renderState);
}

bool GameStateMachine::TryGetInitialKingPlacementPrompt(
    uint64_t& outSquaresMask64) const {
    return runtime::TryGetInitialKingPlacementPrompt(
        m_renderState, outSquaresMask64);
}

} // namespace runtime
