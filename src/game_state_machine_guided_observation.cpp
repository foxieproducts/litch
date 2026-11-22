#include "runtime/game_state_machine.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"

namespace runtime {

void GameStateMachine::HandleGuidedBoardObservation(
    const uint64_t changedMask,
    const uint64_t occupiedMask,
    const uint32_t tMs,
    const uint32_t sequence) {
    const uint32_t effectiveSequence =
        sequence != 0U
            ? sequence
            : m_chessInteractionModel.board.sequence + 1U;
    ReduceInteraction(
        ChessInteractionEvent::MakeModalBoardObservation(
            tMs, effectiveSequence, changedMask, occupiedMask));
}

void GameStateMachine::HandleRecoveryBoardObservation(
    const uint64_t changedMask,
    const uint64_t occupiedMask,
    const uint32_t tMs,
    const uint32_t sequence) {
    const uint32_t effectiveSequence =
        sequence != 0U
            ? sequence
            : m_chessInteractionModel.board.sequence + 1U;
    const TaggedInteractionState phase = m_chessInteractionModel.phase;
    if (IsConfirmedNextSidePickupObservation(
            phase, changedMask, occupiedMask)) {
        HandleBoardObservation(
            changedMask, occupiedMask, tMs, effectiveSequence);
        return;
    }
    const uint64_t interactionOccupancy =
        ProjectPhysicalOccupancyForInteractionProgress(
            m_chessInteractionModel.acknowledged,
            m_chessInteractionModel.phase);
    const auto& model = m_chessInteractionModel;
    const uint64_t duckMask = GetKnownNeutralDuckMask(model.acknowledged);
    const bool retainedEarlyDuckProgress =
        IsEarlyDuckLiftAllowed(phase) && model.board.valid &&
        (duckMask & ~occupiedMask) != 0ULL &&
        occupiedMask == model.board.occupiedMask;
    const ChessInteractionEvent observation =
        (occupiedMask == interactionOccupancy || retainedEarlyDuckProgress)
            ? ChessInteractionEvent::MakeModalBoardObservation(
                  tMs, effectiveSequence, changedMask, occupiedMask)
            : ChessInteractionEvent::MakeDeferredBoardObservation(
                  tMs, effectiveSequence, changedMask, occupiedMask);
    ReduceInteraction(observation);
    (void)ProjectInteractionCorrection();
}

} // namespace runtime
