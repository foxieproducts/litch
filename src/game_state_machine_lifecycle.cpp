#include "runtime/game_state_machine.hpp"

#include "logger.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"

namespace runtime {
namespace {

bool TryGetSingleSquareFromMask(const uint64_t mask64, uint8_t& outSquare) {
    if (mask64 == 0ULL) {
        return false;
    }

    if ((mask64 & (mask64 - 1ULL)) != 0ULL) {
        return false;
    }

    for (uint8_t square = 0; square < 64U; ++square) {
        if ((mask64 & (1ULL << square)) != 0ULL) {
            outSquare = square;
            return true;
        }
    }

    return false;
}

} // namespace

bool GameStateMachine::TryGetSettleElapsedAndDurationMs(uint32_t& outElapsedMs,
                                                        uint32_t& outDurationMs) const {
    outElapsedMs = 0U;
    outDurationMs = 0U;
    if (!IsMoveSettlingActive() || !m_settleTimer.IsActive()) {
        return false;
    }

    const uint32_t settleDurationMs = (m_activeSettleDurationMs == 0U)
                                          ? (m_cpuExecution.moveSettleActive ? m_config.settleDurationMs
                                                                             : m_humanSettleDurationMs)
                                          : m_activeSettleDurationMs;
    const uint32_t duration = (settleDurationMs == 0U) ? 1U : settleDurationMs;
    const uint32_t startMs = m_settleTimer.DeadlineMs() - duration;
    const fxe::ElapsedTime sinceStart(startMs);
    const uint32_t elapsed = sinceStart.ElapsedMs();
    outElapsedMs = (elapsed > duration) ? duration : elapsed;
    outDurationMs = duration;
    return true;
}

void GameStateMachine::ResetForExternalEngineMutation() {
    const uint32_t nowMs = fxe::Millis();
    ResetOptions options;
    options.stabilizePlacement = true;
    options.resetHumanSettlePolicy = false;
    options.requestCheckCacheRefresh = true;
    ApplyResetOptions(nowMs, options);
    ReduceInteraction(
        ChessInteractionEvent::MakeResetPreservingPosition());
    m_interactionInvariantStatus = InteractionInvariantStatus{};
}

void GameStateMachine::ApplyPhysicalOccupancySnapshot(const uint64_t occ64) {
    const uint32_t nowMs = fxe::Millis();
    const ChessInteractionPhase phase = m_chessInteractionModel.phase.tag;
    const bool restoredDuckSettlingCorrection =
        phase == ChessInteractionPhase::Correction &&
        m_chessInteractionModel.phase.plan.kind ==
            PhysicalTransitionKind::DuckRelocation &&
        m_chessInteractionModel.phase.plan.source ==
            PhysicalTransitionSource::Human &&
        m_chessInteractionModel.phase.selectedSquare ==
            CHESS_INTERACTION_INVALID_SQUARE;
    const bool restoredCpuPlan =
        phase == ChessInteractionPhase::ExecutingPlan &&
        IsCpuDirectedTransition(m_chessInteractionModel.phase.plan);
    if (phase == ChessInteractionPhase::ExecutingPlan ||
        phase == ChessInteractionPhase::AwaitingAction ||
        phase == ChessInteractionPhase::Settling ||
        phase == ChessInteractionPhase::DuckRelocation ||
        phase == ChessInteractionPhase::Correction) {
        ReduceInteraction(
            ChessInteractionEvent::MakePlanProgressObserved(occ64));
    }
    if (restoredDuckSettlingCorrection &&
        m_chessInteractionModel.phase.tag ==
            ChessInteractionPhase::Settling &&
        !m_humanSettleActive) {
        StartSettling(m_chessInteractionModel.phase.plan.moveTo);
    }
    if (m_humanSettleActive &&
        m_chessInteractionModel.phase.tag != ChessInteractionPhase::Settling &&
        m_chessInteractionModel.phase.tag !=
            ChessInteractionPhase::AwaitingAction) {
        m_settleTimer.Stop();
        m_activeSettleDurationMs = 0U;
        m_humanSettleActive = false;
        m_renderState.settleSquare = INVALID_SQUARE;
    }
    m_chessInteractionModel.board.valid = true;
    m_chessInteractionModel.board.changedMask = 0ULL;
    m_chessInteractionModel.board.occupiedMask = occ64;
    m_chessInteractionModel.board.timestampMs = nowMs;
    m_chessInteractionModel.board.stableSinceMs = nowMs;
    m_interactionStartedFromMismatch = false;

    m_lastInteractionElapsed.Reset();
    if (!restoredCpuPlan && m_config.placementStableRequiredMs != 0U) {
        m_lastInteractionElapsed = fxe::ElapsedTime(nowMs - m_config.placementStableRequiredMs);
    }
}

void GameStateMachine::Tick() {
    const uint32_t nowMs = fxe::Millis();
    RetireExpiredAllPiecesFlash(nowMs);
    auto& reveal = m_renderState.legalDestinationReveal;
    if (reveal.Started() && nowMs - reveal.StartMs() >= LEGAL_DESTINATION_REVEAL_MS) {
        reveal = {};
    }
    RefreshThinkingControls();
    if ((m_renderState.hintRefinementReady || m_renderState.hintFeedbackElapsed.Started()) &&
        ((m_chessInteractionModel.phase.tag != ChessInteractionPhase::Idle &&
          !IsContinuousDuckHint()) ||
         m_placementModeActive || m_textOverlayActive || m_kingQueenSwapPromptActive ||
         m_cpuThinkingActive || HasHumanIllegal() || HasCpuIllegal())) {
        CancelHintRefinement();
    }
    AdvanceHintRefinement(m_renderState);
    if (IsUndoPromptActive()) {
        return;
    }

    if (m_kingQueenSwapPromptActive) {
        if (m_kingQueenSwapReadyToExit && IsSensorsStableForPlacement()) {
            logger::Trace("kq-swap: done");
            ClearKingQueenSwapPrompt();
        }
        return;
    }

    if (IsPromotionActive()) {
        UpdatePromotion();
        return;
    }

    if (m_placementModeActive) {
        UpdatePlacementMode();
        return;
    }

    const uint64_t interactionExpectedOcc64 =
        GetInteractionExpectedPosition().occupiedMask;

    if (m_humanIllegalMask != 0 &&
        m_chessInteractionModel.board.occupiedMask == interactionExpectedOcc64) {
        m_humanIllegalMask = 0;
        RestartIllegalFlashCadence();
    }

    if (m_captureReturnMask != 0 &&
        m_chessInteractionModel.board.occupiedMask == interactionExpectedOcc64) {
        m_captureReturnMask = 0;
        RestartCaptureReturnFlashCadence();
    }

    if (m_cpuExecution.illegalMask != 0 &&
        m_chessInteractionModel.board.occupiedMask == interactionExpectedOcc64) {
        m_cpuExecution.illegalMask = 0;
        RestartIllegalFlashCadence();
    }

    ClearExpiredSettleAfterglow(m_renderState, INVALID_SQUARE);
    auto& promotion = m_renderState.promotionConfirmation;
    if (promotion.square < 64U &&
        fxe::Millis() - promotion.startedAtMs >= PromotionConfirmation::NOTICE_MS) {
        promotion = {};
    }

    if ((m_humanSettleActive || m_cpuExecution.moveSettleActive) &&
        m_settleTimer.IsExpired()) {
        if (IsEnPassantSettleBlocked()) {
            return;
        }
        const TaggedInteractionState& phase = m_chessInteractionModel.phase;
        if (m_humanSettleActive) {
            const bool correctingPendingDuck =
                phase.tag == ChessInteractionPhase::Correction &&
                phase.plan.kind == PhysicalTransitionKind::DuckRelocation;
            const bool incompleteSettlingPosition =
                phase.tag == ChessInteractionPhase::Settling &&
                (m_humanIllegalMask != 0ULL || m_captureReturnMask != 0ULL ||
                 ProjectHumanChessOccupancy(
                     m_chessInteractionModel.acknowledged, phase,
                     m_chessInteractionModel.board.occupiedMask) !=
                     phase.plan.afterOccupiedMask);
            if (correctingPendingDuck || incompleteSettlingPosition) {
                // An accepted Circle deadline may expire during Duck
                // correction. Retain that deadline until restoration instead
                // of consuming it and requiring a second confirmation.
                return;
            }
        }

        if (m_cpuExecution.moveSettleActive) {
            FinalizeCpuMoveSettling();
        } else {
            FinalizeMove();
        }
        return;
    }

    if (m_cpuExecution.queued) {
        (void)TryFinalizeCpuMove();
    }

    const bool canStartCpuIdleDelay =
        m_chessInteractionModel.phase.tag == ChessInteractionPhase::Idle &&
        !IsMoveSettlingActive() && !m_cpuExecution.queued;
    if (canStartCpuIdleDelay && !m_cpuExecution.idleTimeoutFired) {
        if (m_lastInteractionElapsed.ElapsedMs() >= m_config.cpuIdleDelayMs) {
            m_cpuExecution.idleTimeoutFired = true;
        }
    }
}

void GameStateMachine::StartCpuMoveSettling(const uint8_t settleSquare) {
    m_cpuExecution.moveSettleActive = true;
    m_cpuExecution.settleDestinationSuppressed = false;
    m_renderState.settleSquare = settleSquare;
    m_renderState.promotionArrivalSquare = INVALID_SQUARE;
    m_activeSettleDurationMs = m_config.settleDurationMs;
    m_settleTimer.Start(m_config.settleDurationMs);
    m_renderState.settleAnimStartEvent = true;
    m_renderState.settleVisualElapsed.Reset();
    m_renderState.settleMoverSideKnown =
        m_chessInteractionModel.acknowledged.sideToMoveValid;
    // Physical completion starts presentation before the coordinator claims
    // and acknowledges the move. Keep that mover through the later side flip.
    m_renderState.settleMoverIsBlack =
        m_chessInteractionModel.acknowledged.sideToMoveBlack;
}

void GameStateMachine::FinalizeCpuMoveSettling(
    const bool replaySuppressedDestination) {
    const bool replayDestination =
        replaySuppressedDestination &&
        m_cpuExecution.settleDestinationSuppressed &&
        m_renderState.settleSquare < 64U;
    const uint8_t settleSquare = m_renderState.settleSquare;
    m_settleTimer.Stop();
    m_activeSettleDurationMs = 0;
    m_cpuExecution.moveSettleActive = false;
    m_cpuExecution.settleDestinationSuppressed = false;

    StartSettleAfterglow(m_renderState, INVALID_SQUARE, false, INVALID_SQUARE);
    m_renderState.settleSquare = INVALID_SQUARE;
    if (replayDestination) {
        ReduceInteractionBoardObservation(
            1ULL << settleSquare,
            m_chessInteractionModel.board.occupiedMask,
            m_chessInteractionModel.board.timestampMs,
            m_chessInteractionModel.board.sequence + 1U);
        if (ProjectInteractionCorrection()) {
            // The physical absence already aged while settle suppressed bounce.
            // Replaying it must not grant a second quiet/gentle grace interval.
            m_illegalFlashStartMs = m_lastInteractionElapsed.StartMs();
        }
    }
}

bool GameStateMachine::RejectCurrentPickupAsIllegal(const uint8_t illegalSquare) {
    if (illegalSquare >= 64U) {
        return false;
    }

    m_humanIllegalMask |= (1ULL << illegalSquare);
    ClearKingSafetyPrompt();
    ClearValidDestMask();
    CancelCurrentMove();
    return true;
}

void GameStateMachine::SetKingSafetyPrompt(const uint8_t triggerSquare,
                                          const uint8_t kingSquare,
                                          const uint8_t attackerSquare) {
    if (triggerSquare >= 64U || kingSquare >= 64U || attackerSquare >= 64U) {
        ClearKingSafetyPrompt();
        return;
    }

    if (m_kingSafetyTriggerSquare != triggerSquare ||
        m_kingSafetyKingSquare != kingSquare ||
        m_kingSafetyAttackerSquare != attackerSquare) {
        RestartIllegalFlashCadence();
    }
    m_kingSafetyTriggerSquare = triggerSquare;
    m_kingSafetyKingSquare = kingSquare;
    m_kingSafetyAttackerSquare = attackerSquare;
}

void GameStateMachine::ClearKingSafetyPrompt() {
    m_kingSafetyTriggerSquare = INVALID_SQUARE;
    m_kingSafetyKingSquare = INVALID_SQUARE;
    m_kingSafetyAttackerSquare = INVALID_SQUARE;
}

bool GameStateMachine::TryGetKingSafetyPrompt(uint8_t& outKingSquare,
                                             uint8_t& outAttackerSquare) const {
    if (m_kingSafetyTriggerSquare >= 64U || m_kingSafetyKingSquare >= 64U ||
        m_kingSafetyAttackerSquare >= 64U) {
        return false;
    }

    const uint64_t physical = GetPhysicalOccupancyMask();
    if ((physical & (1ULL << m_kingSafetyTriggerSquare)) != 0ULL ||
        (physical & (1ULL << m_kingSafetyAttackerSquare)) == 0ULL) {
        return false;
    }

    const bool rejected = (m_humanIllegalMask & (1ULL << m_kingSafetyTriggerSquare)) != 0ULL;
    const auto& phase = m_chessInteractionModel.phase;
    const bool held = phase.tag == ChessInteractionPhase::PieceLifted &&
                      phase.selectedSquare == m_kingSafetyTriggerSquare &&
                      phase.correctionMask == 0ULL && m_humanIllegalMask == 0ULL;
    // A legal along-ray move or capturing the pinner must hand presentation
    // back to the physical plan as soon as it takes over.
    if (!rejected && !held) {
        return false;
    }

    outKingSquare = m_kingSafetyKingSquare;
    outAttackerSquare = m_kingSafetyAttackerSquare;
    return true;
}

bool GameStateMachine::TryGetExpectedMoverKingSquare(uint8_t& outSquare) const {
    const ChessPositionState expected = GetInteractionExpectedPosition();
    return TryGetSingleSquareFromMask(expected.moverOccupiedMask &
                                          expected.kingMask,
                                      outSquare);
}

bool GameStateMachine::TryGetSettleAfterglow(uint8_t& outSquare, uint32_t& outElapsedMs) const {
    return runtime::TryGetSettleAfterglow(m_renderState, outSquare, outElapsedMs);
}

void GameStateMachine::SetLastMoveForSide(const modules::PersistedSide side,
                                          const uint8_t fromSquare,
                                          const uint8_t toSquare) {
    runtime::SetLastMoveForSide(m_renderState, side, fromSquare, toSquare);
}

void GameStateMachine::ClearLastMoveForSide(const modules::PersistedSide side) {
    runtime::ClearLastMoveForSide(m_renderState, side);
}

void GameStateMachine::SetLastMoveMostRecentSide(const modules::PersistedSide side) {
    runtime::SetLastMoveMostRecentSide(m_renderState, side);
}

void GameStateMachine::ClearLastMoveMostRecentSide() {
    runtime::ClearLastMoveMostRecentSide(m_renderState);
}

bool GameStateMachine::TryGetLastMoveMostRecentSide(modules::PersistedSide& outSide) const {
    return runtime::TryGetLastMoveMostRecentSide(m_renderState, outSide);
}

bool GameStateMachine::TryGetLastMoveForSide(const modules::PersistedSide side,
                                             uint8_t& outFromSquare,
                                             uint8_t& outToSquare) const {
    return runtime::TryGetLastMoveForSide(m_renderState, side, outFromSquare, outToSquare);
}

void GameStateMachine::StartSettling(const uint8_t toSquare) {
    m_humanSettleActive = true;
    m_activeHumanSettleRequiresO = m_humanSettleRequiresO;
    m_renderState.settleSquare = toSquare;

    if (m_activeHumanSettleRequiresO) {
        m_activeSettleDurationMs = 0;
        m_settleTimer.Stop();
    } else {
        m_activeSettleDurationMs = m_humanSettleDurationMs;
        m_settleTimer.Start(m_humanSettleDurationMs);
    }

    m_renderState.settleAfterglowAbort = false;
    m_renderState.settleAnimStartEvent = true;
    m_renderState.settleVisualElapsed.Reset();
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    m_renderState.promotionArrivalSquare = phase.plan.promotionRequired
        ? toSquare : INVALID_SQUARE;
    m_renderState.settleMoverSideKnown =
        m_chessInteractionModel.acknowledged.sideToMoveValid;
    m_renderState.settleMoverIsBlack =
        phase.plan.progress.actionAcknowledged
            ? !m_chessInteractionModel.acknowledged.sideToMoveBlack
            : m_chessInteractionModel.acknowledged.sideToMoveBlack;
}

void GameStateMachine::StartHumanOConfirmFinalizeSettle() {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    const bool retainedCastleSettle =
        phase.tag == ChessInteractionPhase::Idle ||
        (phase.tag == ChessInteractionPhase::AwaitingAction &&
         phase.plan.kind == PhysicalTransitionKind::Castling);
    const uint64_t expected = phase.tag == ChessInteractionPhase::Idle
                                  ? m_chessInteractionModel.acknowledged.occupiedMask
                                  : phase.plan.afterOccupiedMask;
    if (!IsHumanOConfirmSettleActive() || m_settleTimer.IsActive() ||
        (phase.tag != ChessInteractionPhase::Settling && !retainedCastleSettle) ||
        m_humanIllegalMask != 0ULL || m_captureReturnMask != 0ULL ||
        ProjectHumanChessOccupancy(
            m_chessInteractionModel.acknowledged, phase,
            m_chessInteractionModel.board.occupiedMask) != expected) {
        return;
    }

    m_activeSettleDurationMs = CIRCLE_CONFIRM_FINALIZE_SETTLE_MS;
    m_settleTimer.Start(CIRCLE_CONFIRM_FINALIZE_SETTLE_MS);
    m_renderState.settleAnimStartEvent = true;
}

void GameStateMachine::FinalizeMove() {
    m_settleTimer.Stop();
    m_activeSettleDurationMs = 0;
    m_humanSettleActive = false;
    m_interactionStartedFromMismatch = false;

    const PhysicalTransitionPlan committedPlan =
        m_chessInteractionModel.phase.plan;
    const uint8_t committedFrom = committedPlan.moveFrom;
    const uint8_t committedTo = committedPlan.moveTo;
    if (m_chessInteractionModel.phase.tag ==
        ChessInteractionPhase::Settling) {
        ReduceInteraction(ChessInteractionEvent::MakeSettleConfirmed());
    }
    if (committedFrom < 64U && committedTo < 64U) {
        (void)TryAcknowledgeInteractionPlan(committedPlan.kind,
                                            committedFrom,
                                            committedTo);
    }

    StartSettleAfterglow(m_renderState, INVALID_SQUARE, false, INVALID_SQUARE);
    m_renderState.settleSquare = INVALID_SQUARE;
}

bool GameStateMachine::IsEnPassantSettleBlocked() const {
    uint8_t captureSquare = 0U;
    if (!TryGetPendingCaptureSquare(captureSquare) ||
        captureSquare == m_chessInteractionModel.phase.plan.moveTo) {
        return false;
    }

    return (m_chessInteractionModel.board.occupiedMask &
            (1ULL << captureSquare)) != 0ULL;
}

} // namespace runtime
