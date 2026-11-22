#include "runtime/game_state_machine.hpp"

#include "logger.hpp"

namespace runtime {
namespace {
bool TryGetSingleSquareFromMask(const uint64_t mask64, uint8_t& outSquare) {
    if (mask64 == 0) {
        return false;
    }

    if ((mask64 & (mask64 - 1ULL)) != 0) {
        return false;
    }

    for (uint8_t i = 0; i < 64; ++i) {
        if ((mask64 & (1ULL << i)) != 0) {
            outSquare = i;
            return true;
        }
    }

    return false;
}

} // namespace

void GameStateMachine::RestartIllegalFlashCadence() {
    m_illegalFlashStartMs = fxe::Millis();
}

void GameStateMachine::RestartCaptureReturnFlashCadence() {
    m_captureReturnFlashStartMs = fxe::Millis();
}

void GameStateMachine::ApplyResetOptions(const uint32_t nowMs, const ResetOptions& options) {
    m_lastInteractionElapsed.Reset();
    if (options.stabilizePlacement && m_config.placementStableRequiredMs != 0U) {
        m_lastInteractionElapsed = fxe::ElapsedTime(nowMs - m_config.placementStableRequiredMs);
    }

    if (options.resetHumanSettlePolicy) {
        m_humanSettleDurationMs = m_config.settleDurationMs;
        m_humanSettleRequiresO = false;
    }
    m_activeSettleDurationMs = 0;
    m_humanSettleActive = false;
    m_activeHumanSettleRequiresO = false;

    ClearCpuMove();
    m_cpuExecution.illegalMask = 0;
    m_cpuExecution.idleTimeoutFired = false;
    m_cpuExecution.moveSettleActive = false;
    m_cpuExecution.settleDestinationSuppressed = false;
    m_cpuExecution.undoCanceledQueuedMove = false;

    m_humanIllegalMask = 0;
    m_illegalFlashStartMs = nowMs;
    m_captureReturnMask = 0;
    m_captureReturnFlashStartMs = nowMs;
    ClearValidDestMask();
    m_kingSafetyTriggerSquare = INVALID_SQUARE;
    m_kingSafetyKingSquare = INVALID_SQUARE;
    m_kingSafetyAttackerSquare = INVALID_SQUARE;

    m_placementModeActive = false;
    m_placementMissingMask = 0;
    m_placementExtraMask = 0;
    m_interactionStartedFromMismatch = false;

    m_promotionUx.Cancel();
    m_promotionChoicePending = false;
    m_promotionChoice = modules::PromotionChoice{};

    m_buttonQueueHead = 0;
    m_buttonQueueTail = 0;
    m_buttonQueueCount = 0;

    m_settleTimer.Stop();
    ResetGameStateMachineRenderState(m_renderState, INVALID_SQUARE);

    m_claimedInteractionActionToken = 0U;
    m_undoPromptStartMs = 0;
    m_undoPromptMetadataConfigured = true;
    m_undoPromptMoverSidePersist = modules::PersistedSide::Unknown;
    m_undoPromptMoverPieceFen = '\0';
    m_visualActiveSideOverridePersist = modules::PersistedSide::Unknown;
    m_statusEdgeActiveSideOverridePersist = modules::PersistedSide::Unknown;

    m_kingQueenSwapPromptActive = false;
    m_kingQueenSwapSquaresMask64 = 0;
    m_kingQueenSwapLiftedMask64 = 0;
    m_kingQueenSwapInitialEmptyMask64 = 0;
    m_kingQueenSwapReadyToExit = false;

    m_movedSquaresMask = 0;
    m_movedSquaresMaskBeforeLastAcknowledgedHumanMove = 0;
    m_movedSquaresSnapshotActionToken = 0U;
    m_cpuThinkingActive = false;

    if (options.clearTextOverlay) {
        m_textOverlayActive = false;
    }
    m_checkCacheRefreshRequested = options.requestCheckCacheRefresh;

}

void GameStateMachine::Reset() {
    const uint32_t nowMs = fxe::Millis();
    ReduceInteraction(ChessInteractionEvent{});
    m_interactionInvariantStatus = InteractionInvariantStatus{};
    ApplyResetOptions(nowMs, ResetOptions{});
}

void GameStateMachine::SetMoveConfirmPolicy(const uint32_t settleDurationMs,
                                            const bool requireOConfirm) {
    if (m_humanSettleDurationMs == settleDurationMs &&
        m_humanSettleRequiresO == requireOConfirm) {
        return;
    }
    m_humanSettleDurationMs = settleDurationMs;
    m_humanSettleRequiresO = requireOConfirm;
    if (!m_humanSettleActive ||
        (m_activeHumanSettleRequiresO && m_settleTimer.IsActive())) {
        // A valid O already owns its final second. Preference changes apply
        // to the next settle without replacing that accepted deadline.
        return;
    }
    m_activeHumanSettleRequiresO = requireOConfirm;
    m_activeSettleDurationMs = requireOConfirm ? 0U : settleDurationMs;
    if (requireOConfirm) {
        m_settleTimer.Stop();
    } else {
        m_settleTimer.Start(settleDurationMs);
    }
}

ChessPositionState GameStateMachine::GetInteractionExpectedPosition() const {
    if (m_chessInteractionModel.phase.tag !=
            ChessInteractionPhase::AwaitingAction ||
        m_chessInteractionModel.phase.plan.source ==
            PhysicalTransitionSource::Undo) {
        return m_chessInteractionModel.acknowledged;
    }
    return ProjectChessPositionAfterPlan(
        m_chessInteractionModel.acknowledged,
        m_chessInteractionModel.phase.plan);
}

#ifndef ARDUINO
void GameStateMachine::DebugSetExpectedPositionModel(const ChessPositionState& model) {
    HandleInteractionPositionAcknowledged(model.occupiedMask,
                                          model.kingMask,
                                          model.rookMask,
                                          model.moverOccupiedMask,
                                          model.opponentOccupiedMask,
                                          model.pawnMask,
                                          model.enPassantTargetSquare,
                                          model.enPassantCaptureSquare,
                                          model.sideToMoveValid,
                                          model.sideToMoveBlack,
                                          model.legalCaptureTargetsValid,
                                          model.legalCaptureTargetsMask,
                                          model.legalCastlingTargetsValid,
                                          model.legalCastlingKingTargetsMask);
}

void GameStateMachine::SetExpectedOccupancyMask(const uint64_t expectedOcc64) {
    ChessPositionState& acknowledged = m_chessInteractionModel.acknowledged;
    HandleInteractionPositionAcknowledged(expectedOcc64,
                                          acknowledged.kingMask,
                                          acknowledged.rookMask,
                                          acknowledged.moverOccupiedMask,
                                          acknowledged.opponentOccupiedMask,
                                          acknowledged.pawnMask,
                                          acknowledged.enPassantTargetSquare,
                                          acknowledged.enPassantCaptureSquare,
                                          acknowledged.sideToMoveValid,
                                          acknowledged.sideToMoveBlack,
                                          acknowledged.legalCaptureTargetsValid,
                                          acknowledged.legalCaptureTargetsMask,
                                          acknowledged.legalCastlingTargetsValid,
                                          acknowledged.legalCastlingKingTargetsMask);
}

void GameStateMachine::SetExpectedSideToMove(const modules::PersistedSide side) {
    const ChessPositionState& acknowledged = m_chessInteractionModel.acknowledged;
    HandleInteractionPositionAcknowledged(
        acknowledged.occupiedMask,
        acknowledged.kingMask,
        acknowledged.rookMask,
        acknowledged.moverOccupiedMask,
        acknowledged.opponentOccupiedMask,
        acknowledged.pawnMask,
        acknowledged.enPassantTargetSquare,
        acknowledged.enPassantCaptureSquare,
        side != modules::PersistedSide::Unknown,
        side == modules::PersistedSide::Black,
        acknowledged.legalCaptureTargetsValid,
        acknowledged.legalCaptureTargetsMask,
        acknowledged.legalCastlingTargetsValid,
        acknowledged.legalCastlingKingTargetsMask);
}

void GameStateMachine::SetExpectedMoverAndOpponentOccupancyMasks(const uint64_t moverOcc64,
                                                                 const uint64_t opponentOcc64) {
    const ChessPositionState& acknowledged = m_chessInteractionModel.acknowledged;
    const uint64_t sideOccupiedMask = acknowledged.occupiedMask != 0ULL
                                          ? acknowledged.occupiedMask
                                          : (moverOcc64 | opponentOcc64);
    HandleInteractionPositionAcknowledged(sideOccupiedMask,
                                          acknowledged.kingMask,
                                          acknowledged.rookMask,
                                          moverOcc64,
                                          opponentOcc64,
                                          acknowledged.pawnMask,
                                          acknowledged.enPassantTargetSquare,
                                          acknowledged.enPassantCaptureSquare,
                                          acknowledged.sideToMoveValid,
                                          acknowledged.sideToMoveBlack,
                                          acknowledged.legalCaptureTargetsValid,
                                          acknowledged.legalCaptureTargetsMask,
                                          acknowledged.legalCastlingTargetsValid,
                                          acknowledged.legalCastlingKingTargetsMask);
}

void GameStateMachine::SetExpectedKingAndRookOccupancyMasks(const uint64_t kingOcc64,
                                                            const uint64_t rookOcc64) {
    const ChessPositionState& acknowledged = m_chessInteractionModel.acknowledged;
    HandleInteractionPositionAcknowledged(acknowledged.occupiedMask,
                                          kingOcc64,
                                          rookOcc64,
                                          acknowledged.moverOccupiedMask,
                                          acknowledged.opponentOccupiedMask,
                                          acknowledged.pawnMask,
                                          acknowledged.enPassantTargetSquare,
                                          acknowledged.enPassantCaptureSquare,
                                          acknowledged.sideToMoveValid,
                                          acknowledged.sideToMoveBlack,
                                          acknowledged.legalCaptureTargetsValid,
                                          acknowledged.legalCaptureTargetsMask,
                                          acknowledged.legalCastlingTargetsValid,
                                          acknowledged.legalCastlingKingTargetsMask);
}

void GameStateMachine::SetLegalCaptureTargetsMask(const uint64_t legalCaptureTargetsMask64,
                                                  const bool valid) {
    const ChessPositionState& acknowledged = m_chessInteractionModel.acknowledged;
    HandleInteractionPositionAcknowledged(
        acknowledged.occupiedMask,
        acknowledged.kingMask,
        acknowledged.rookMask,
        acknowledged.moverOccupiedMask,
        acknowledged.opponentOccupiedMask,
        acknowledged.pawnMask,
        acknowledged.enPassantTargetSquare,
        acknowledged.enPassantCaptureSquare,
        acknowledged.sideToMoveValid,
        acknowledged.sideToMoveBlack,
        valid,
        legalCaptureTargetsMask64,
        acknowledged.legalCastlingTargetsValid,
        acknowledged.legalCastlingKingTargetsMask);
}
#endif

} // namespace runtime
