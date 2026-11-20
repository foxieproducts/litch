#include "runtime/game_state_machine.hpp"

#include "logger.hpp"
#include "runtime/chess_interaction_persistence.hpp"

namespace runtime {

void GameStateMachine::ReduceInteraction(
    const ChessInteractionEvent& event,
    const CaptureLegality* const captureLegality) {
    const auto& phase = m_chessInteractionModel.phase;
    const ChessInteractionPhase beforeTag = phase.tag;
    const uint8_t beforeSelection = phase.selectedSquare;
    const uint64_t beforeOccupancy = m_chessInteractionModel.board.occupiedMask;
    const uint8_t unfinishedMoveFrom =
        phase.plan.source == PhysicalTransitionSource::Human &&
        !phase.plan.progress.actionAcknowledged
            ? phase.plan.moveFrom : INVALID_SQUARE;
    const bool continuingDuckHint = IsContinuousDuckHint();
    ReduceChessInteraction(m_chessInteractionModel, event, captureLegality);

    auto& reveal = m_renderState.legalDestinationReveal;
    if (phase.tag != ChessInteractionPhase::PieceLifted || phase.selectedSquare >= 64U) {
        reveal = {};
    } else if (beforeTag != ChessInteractionPhase::PieceLifted ||
               beforeSelection != phase.selectedSquare) {
        // Relifting a provisional landing reopens an existing move. Its legal
        // guidance returns at full strength rather than replaying the pickup.
        reveal = phase.selectedSquare == unfinishedMoveFrom
            ? fxe::ElapsedTime{} : fxe::ElapsedTime::StartNow();
    } else if (beforeOccupancy != m_chessInteractionModel.board.occupiedMask) {
        // Further handling can keep the same source during correction. End
        // the reveal so recovery returns directly to steady guidance.
        reveal = {};
    }

    const bool wasIdle = beforeTag == ChessInteractionPhase::Idle;
    m_renderState.turnIndicator.Request(
        m_chessInteractionModel.phase.tag == ChessInteractionPhase::Idle,
        m_chessInteractionModel.acknowledged.sideToMoveBlack);
    if (event.type == ChessInteractionEventType::ResetPreservingPosition ||
        m_chessInteractionModel.phase.tag != ChessInteractionPhase::Idle ||
        m_chessInteractionModel.board.occupiedMask !=
            m_chessInteractionModel.acknowledged.occupiedMask) {
        if (wasIdle) runtime::RetainVisibleHintPresentation(m_renderState,
            GetVisibleHintPresentation(true));
        else if (m_hintPresentationObserver == nullptr)
            runtime::RetainVisibleHintPresentation(m_renderState);
        if (continuingDuckHint || IsContinuousDuckHint()) {
            runtime::ClearHintRefinement(m_renderState);
        } else {
            CancelHintRefinement();
        }
        if (wasIdle && m_hintPresentationPublisher != nullptr)
            m_hintPresentationPublisher(m_hintPresentationContext);
    }
}

bool GameStateMachine::ProjectInteractionCorrection() {
    const TaggedInteractionState& phase =
        m_chessInteractionModel.phase;
    const bool duckRelocationCorrection =
        phase.tag == ChessInteractionPhase::DuckRelocation &&
        phase.plan.kind == PhysicalTransitionKind::DuckRelocation &&
        phase.requiredAction == RequiredPhysicalAction::CorrectBoard;
    if (phase.tag != ChessInteractionPhase::Correction &&
        !duckRelocationCorrection) {
        return false;
    }
    m_humanIllegalMask = phase.correctionMask;
    m_captureReturnMask = 0ULL;
    ClearValidDestMask();
    RestartIllegalFlashCadence();
    return true;
}

void GameStateMachine::HandleInteractionPositionAcknowledged(
    const uint64_t occupiedMask,
    const uint64_t kingMask,
    const uint64_t rookMask,
    const uint64_t moverMask,
    const uint64_t opponentMask,
    const uint64_t pawnMask,
    const uint8_t enPassantTargetSquare,
    const uint8_t enPassantCaptureSquare,
    const bool sideToMoveValid,
    const bool sideToMoveBlack,
    const bool legalCaptureTargetsValid,
    const uint64_t legalCaptureTargetsMask,
    const bool legalCastlingTargetsValid,
    const uint64_t legalCastlingKingTargetsMask,
    const CaptureLegality* const captureLegality) {
    const TaggedInteractionState before =
        m_chessInteractionModel.phase;
    const bool replaysAcknowledgedFollowup =
        before.tag == ChessInteractionPhase::ExecutingPlan &&
        (before.plan.source == PhysicalTransitionSource::Human ||
         IsCpuDirectedTransition(before.plan)) &&
        before.plan.progress.actionAcknowledged;
    ReduceInteraction(
        ChessInteractionEvent::MakePositionAcknowledged(
            occupiedMask,
            kingMask,
            rookMask,
            moverMask,
            opponentMask,
            pawnMask,
            enPassantTargetSquare,
            enPassantCaptureSquare,
            sideToMoveValid,
            sideToMoveBlack,
            legalCaptureTargetsValid,
            legalCaptureTargetsMask,
            legalCastlingTargetsValid,
            legalCastlingKingTargetsMask),
        captureLegality);
    const TaggedInteractionState& after =
        m_chessInteractionModel.phase;
    UpdateCastlingRelocationFeedback(before, after);
    if (replaysAcknowledgedFollowup &&
        after.tag == ChessInteractionPhase::Settling &&
        after.plan.source == PhysicalTransitionSource::Human &&
        !m_humanSettleActive) {
        // Position acknowledgement can replay the same physical observation
        // against the newly committed engine position. If that replay fully
        // completes the next move (for example atomic en passant), enter the
        // same settle lifecycle that a direct board observation would start.
        StartSettling(after.plan.moveTo);
    }
    if (replaysAcknowledgedFollowup &&
        after.tag == ChessInteractionPhase::AwaitingAction &&
        after.plan.source == PhysicalTransitionSource::Human &&
        after.plan.kind == PhysicalTransitionKind::Castling &&
        after.plan.relocationCount == 2U &&
        !m_humanSettleActive) {
        uint8_t settleSquare = INVALID_SQUARE;
        const uint64_t placedDestinations =
            m_chessInteractionModel.board.changedMask &
            m_chessInteractionModel.board.occupiedMask &
            ((1ULL << after.plan.relocations[0].to) |
             (1ULL << after.plan.relocations[1].to));
        for (uint8_t square = 0U; square < 64U; ++square) {
            if ((placedDestinations & (1ULL << square)) != 0ULL) {
                settleSquare = square;
            }
        }
        StartSettling(settleSquare < 64U
                          ? settleSquare
                          : after.plan.relocations[1].to);
        CommitCastlingComposite();
    }
    (void)ProjectInteractionCorrection();
}

void GameStateMachine::HandleInteractionActionResult(
    const uint32_t token,
    const bool succeeded) {
    const bool matchesPending =
        m_chessInteractionModel.phase.tag ==
            ChessInteractionPhase::AwaitingAction &&
        m_chessInteractionModel.phase.pendingActionToken == token;
    const bool completesCpuPlan =
        matchesPending && succeeded &&
        IsCpuDirectedTransition(m_chessInteractionModel.phase.plan);
    const bool completesUndoPlan =
        matchesPending && succeeded &&
        m_chessInteractionModel.phase.plan.source ==
            PhysicalTransitionSource::Undo;
    const uint64_t cpuSettleDestinationMask =
        m_renderState.settleSquare < 64U
            ? (1ULL << m_renderState.settleSquare)
            : 0ULL;
    const bool suppressedCpuDestinationAtAck =
        completesCpuPlan && m_cpuExecution.moveSettleActive &&
        m_cpuExecution.settleDestinationSuppressed &&
        (m_chessInteractionModel.board.occupiedMask ^
         m_chessInteractionModel.phase.plan.afterOccupiedMask) ==
            cpuSettleDestinationMask;
    ReduceInteraction(
        ChessInteractionEvent::MakeActionResult(token, succeeded));

    if (suppressedCpuDestinationAtAck) {
        // The orthogonal CPU settle window continues to own a destination-only
        // bounce. If it remains absent at expiry, that window replays it once
        // against the complete post-action position acknowledgement.
        ReduceInteraction(ChessInteractionEvent::MakePlanCanceled());
    }

    if (completesUndoPlan) {
        TryResolveUndoPromptIfSatisfied();
    }

    if (completesCpuPlan && m_cpuExecution.moveSettleActive &&
        m_chessInteractionModel.phase.tag != ChessInteractionPhase::Idle) {
        m_cpuExecution.settleDestinationSuppressed = false;
        FinalizeCpuMoveSettling();
    }

    if (!matchesPending) {
        return;
    }
    if (!succeeded) {
        CancelCurrentMove();
        (void)ProjectInteractionCorrection();
        return;
    }

    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    if ((phase.tag == ChessInteractionPhase::PieceLifted ||
         phase.tag == ChessInteractionPhase::CaptureStaged) &&
        phase.selectedSquare < 64U) {
        ClearValidDestMask();
        return;
    }
    (void)ProjectInteractionCorrection();
}

void GameStateMachine::HandleInteractionSearchResult(const bool succeeded) {
    // A canceled search is an acknowledged outcome, not malformed input.
    (void)succeeded;
}

void GameStateMachine::HandleInteractionAdapterResult(const bool succeeded) {
    if (!succeeded) {
        ++m_chessInteractionModel.inputFaultEpoch;
    }
}

bool GameStateMachine::RestoreInteractionDurableState(
    const ChessInteractionDurableState& state) {
    if (!RestoreChessInteractionDurableState(
            m_chessInteractionModel, state)) {
        return false;
    }
    const uint32_t nowMs = fxe::Millis();
    m_chessInteractionModel.board.valid = true;
    m_chessInteractionModel.board.changedMask = 0ULL;
    m_chessInteractionModel.board.timestampMs = nowMs;
    m_chessInteractionModel.board.stableSinceMs = nowMs;
    m_renderState.legalDestinationReveal = {};
    m_settleTimer.Stop();
    m_activeSettleDurationMs = 0U;
    m_humanSettleActive = false;
    m_renderState.settleSquare = INVALID_SQUARE;
    const TaggedInteractionState& restored = m_chessInteractionModel.phase;
    if (restored.tag == ChessInteractionPhase::Settling &&
        restored.plan.source == PhysicalTransitionSource::Human) {
        StartSettling(restored.plan.moveTo);
    }
    return true;
}

bool GameStateMachine::TryAcknowledgeInteractionPlan(
    const PhysicalTransitionKind kind,
    const uint8_t from,
    const uint8_t to) {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    const PhysicalTransitionPlan& plan = phase.plan;
    if (phase.tag != ChessInteractionPhase::AwaitingAction ||
        plan.kind != kind ||
        plan.source != PhysicalTransitionSource::Human ||
        plan.moveFrom != from || plan.moveTo != to ||
        plan.moveFrom >= 64U || plan.moveTo >= 64U) {
        return false;
    }
    AcknowledgePendingInteractionLegacyMove();
    m_visualActiveSideOverridePersist = modules::PersistedSide::Unknown;
    m_statusEdgeActiveSideOverridePersist = modules::PersistedSide::Unknown;
    m_checkCacheRefreshRequested = true;
    CancelInitialKingPlacementPrompt();
    return true;
}

void GameStateMachine::AcknowledgePendingInteractionLegacyMove() {
    const PhysicalTransitionPlan& plan = m_chessInteractionModel.phase.plan;
    if (m_chessInteractionModel.phase.tag !=
            ChessInteractionPhase::AwaitingAction ||
        (plan.kind != PhysicalTransitionKind::QuietMove &&
         plan.kind != PhysicalTransitionKind::Capture &&
         plan.kind != PhysicalTransitionKind::EnPassant &&
         plan.kind != PhysicalTransitionKind::Promotion &&
         plan.kind != PhysicalTransitionKind::Castling)) {
        return;
    }

    const uint32_t actionToken =
        m_chessInteractionModel.phase.pendingActionToken;
    if (actionToken != m_movedSquaresSnapshotActionToken) {
        m_movedSquaresMaskBeforeLastAcknowledgedHumanMove =
            m_movedSquaresMask;
        m_movedSquaresSnapshotActionToken = actionToken;
    }
    for (uint8_t i = 0; i < plan.relocationCount; ++i) {
        const PieceRelocation& relocation = plan.relocations[i];
        if (relocation.from < 64U) {
            m_movedSquaresMask |= (1ULL << relocation.from);
        }
        if (relocation.to < 64U) {
            m_movedSquaresMask |= (1ULL << relocation.to);
        }
    }
}

void GameStateMachine::ReduceInteractionBoardObservation(
    const uint64_t changedMask,
    const uint64_t occupiedMask,
    const uint32_t tMs,
    const uint32_t sequence,
    const uint8_t deferredDuckDestinationSquare) {
    ChessInteractionEvent observation =
        m_cpuThinkingActive
            ? ChessInteractionEvent::MakeDeferredBoardObservation(
                  tMs, sequence, changedMask, occupiedMask)
            : ChessInteractionEvent::MakeBoardObservation(
                  tMs, sequence, changedMask, occupiedMask);
    observation.deferredDuckDestinationSquare = deferredDuckDestinationSquare;
    ReduceInteraction(observation);

    const TaggedInteractionState& phase =
        m_chessInteractionModel.phase;
    if (phase.plan.kind == PhysicalTransitionKind::DuckRelocation &&
        (phase.tag == ChessInteractionPhase::DuckRelocation ||
         phase.tag == ChessInteractionPhase::Settling ||
         phase.tag == ChessInteractionPhase::AwaitingAction)) {
        if (phase.tag == ChessInteractionPhase::DuckRelocation &&
            phase.requiredAction == RequiredPhysicalAction::CorrectBoard) {
            m_humanIllegalMask = phase.correctionMask;
            RestartIllegalFlashCadence();
        } else if (m_humanIllegalMask != 0ULL) {
            m_humanIllegalMask = 0ULL;
            RestartIllegalFlashCadence();
        }
    }

    InvariantReport<8> report;
    CheckChessInteractionModelInvariants(m_chessInteractionModel, report);
    if (report.IsOk() || m_interactionInvariantStatus.IsActive()) {
        return;
    }

    m_interactionInvariantStatus.code =
        InteractionInvariantStatusCode::InvariantViolation;
    m_interactionInvariantStatus.sequence = sequence;
    m_interactionInvariantStatus.violationCount =
        static_cast<uint8_t>(report.Count());
    logger::Warn("interaction invariant: name=%s seq=%lu count=%lu",
                 report.At(0).name,
                 static_cast<unsigned long>(sequence),
                 static_cast<unsigned long>(report.Count()));
}

} // namespace runtime
