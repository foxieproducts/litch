#include "runtime/game_state_machine.hpp"

namespace runtime {

bool GameStateMachine::IsSensorsStableForPlacement() const {
    const uint32_t stableMs = m_lastInteractionElapsed.ElapsedMs();
    return stableMs >= m_config.placementStableRequiredMs;
}

bool GameStateMachine::HasPlacementMismatch() const {
    const uint64_t expected = GetExpectedOccupancyMask();
    const uint64_t physical = m_chessInteractionModel.board.occupiedMask;
    const uint64_t missing = expected & ~physical;
    const uint64_t extra = physical & ~expected;
    return (missing | extra) != 0;
}

void GameStateMachine::ClearMovePreviewForPlacementMode() {
    // Placement mode is purely corrective; discard any in-flight move preview.
    ClearValidDestMask();
    m_captureReturnMask = 0;
    m_humanIllegalMask = 0;
    m_settleTimer.Stop();
    m_activeSettleDurationMs = 0U;
    m_humanSettleActive = false;
    m_cpuExecution.moveSettleActive = false;
    m_cpuExecution.settleDestinationSuppressed = false;
    m_renderState.settleSquare = INVALID_SQUARE;
    const TaggedInteractionState& phase =
        m_chessInteractionModel.phase;
    const bool unclaimedHumanAction =
        phase.tag == ChessInteractionPhase::AwaitingAction &&
        phase.plan.source == PhysicalTransitionSource::Human &&
        phase.pendingActionToken != m_claimedInteractionActionToken;
    if (IsHumanMoveInProgress() || unclaimedHumanAction) {
        ReduceInteraction(ChessInteractionEvent::MakePlanCanceled());
    }
}

void GameStateMachine::EnterPlacementMode() {
    m_placementModeActive = true;
    ClearMovePreviewForPlacementMode();
    UpdatePlacementMode();
}

bool GameStateMachine::TryEnterPlacementMode() {
    if (m_placementModeActive) {
        return true;
    }

    // Entry is only allowed while idle, and only once sensors have been stable
    // for long enough (no sensor deltas).
    if (m_chessInteractionModel.phase.tag != ChessInteractionPhase::Idle ||
        IsMoveSettlingActive() || m_cpuExecution.queued ||
        m_kingQueenSwapPromptActive) {
        return false;
    }

    if (!IsSensorsStableForPlacement()) {
        return false;
    }

    if (!HasPlacementMismatch()) {
        return false;
    }

    EnterPlacementMode();
    return true;
}

bool GameStateMachine::ForceEnterPlacementMode() {
    if (IsPromotionActive()) {
        return false;
    }

    // Recovery may project correction while placement already owns the board.
    // Reassert its transient cleanup as well as its flag on every forced entry.
    EnterPlacementMode();
    return true;
}

void GameStateMachine::ExitPlacementMode() {
    m_placementModeActive = false;
    m_placementMissingMask = 0;
    m_placementExtraMask = 0;
    m_interactionStartedFromMismatch = false;
    m_humanIllegalMask = 0;
    m_cpuExecution.illegalMask = 0;
    if (m_chessInteractionModel.phase.tag ==
        ChessInteractionPhase::Correction) {
        ReduceInteraction(
            ChessInteractionEvent::MakeBoardObservation(
                fxe::Millis(),
                m_chessInteractionModel.board.sequence + 1U,
                0ULL,
                m_chessInteractionModel.board.occupiedMask));
    }
}

void GameStateMachine::UpdatePlacementMode() {
    const uint64_t previousMissingMask = m_placementMissingMask;
    const uint64_t previousExtraMask = m_placementExtraMask;
    const uint64_t expected = GetExpectedOccupancyMask();
    const uint64_t physical = m_chessInteractionModel.board.occupiedMask;
    m_placementMissingMask = expected & ~physical;
    m_placementExtraMask = physical & ~expected;

    if (m_placementMissingMask != previousMissingMask ||
        m_placementExtraMask != previousExtraMask) {
        RestartIllegalFlashCadence();
    }

    // Exit only once the mismatch is fully resolved and sensors are stable.
    if ((m_placementMissingMask | m_placementExtraMask) == 0) {
        if (IsSensorsStableForPlacement()) {
            ExitPlacementMode();
        }
    }
}

} // namespace runtime
