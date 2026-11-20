#include "runtime/game_state_machine.hpp"

namespace runtime {
namespace {

uint8_t LastSquareInMask(const uint64_t mask) {
    uint8_t result = CHESS_INTERACTION_INVALID_SQUARE;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((mask & (1ULL << square)) != 0ULL) {
            result = square;
        }
    }
    return result;
}

} // namespace

void GameStateMachine::ClearVariantValidationFeedback() {
    m_placementModeActive = false;
    m_placementMissingMask = 0ULL;
    m_placementExtraMask = 0ULL;
    m_humanIllegalMask = 0ULL;
    m_cpuExecution.illegalMask = 0ULL;
    ClearCpuTentativeCorrection();
    m_captureReturnMask = 0ULL;
    m_interactionStartedFromMismatch = false;
}

void GameStateMachine::HandleVariantValidationBoardObservation(
    const uint64_t changedMask,
    const uint64_t occupiedMask,
    const uint32_t tMs,
    const uint32_t sequence) {
    const uint32_t effectiveSequence =
        sequence != 0U
            ? sequence
            : m_chessInteractionModel.board.sequence + 1U;
    if (changedMask != 0ULL) {
        m_lastSensorSquare = LastSquareInMask(changedMask);
        m_lastInteractionElapsed.Reset();
        m_cpuExecution.idleTimeoutFired = false;
    }

    // This exact initial snapshot belongs to the variant classifier, not
    // chess correction. Discard only stale setup feedback.
    ClearVariantValidationFeedback();
    ReduceInteraction(
        ChessInteractionEvent::MakeModalBoardObservation(
            tMs, effectiveSequence, changedMask, occupiedMask));
}

} // namespace runtime
