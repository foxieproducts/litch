#include "runtime/game_state_machine.hpp"

#include "logger.hpp"
#include "runtime/chess_castling_eligibility.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"

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
bool IsHumanPlan(const TaggedInteractionState& phase) {
    return phase.plan.source == PhysicalTransitionSource::Human &&
           phase.plan.kind != PhysicalTransitionKind::None;
}

uint64_t CastleContinuationPlacementMask(
    const ChessInteractionModel& model,
    const TaggedInteractionState& before,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    if (before.tag != ChessInteractionPhase::PieceLifted ||
        before.selectedSquare >= 64U) {
        return 0ULL;
    }
    const uint64_t removed = changedMask & ~occupiedMask;
    for (const litch_max::CastlingPattern& castle : litch_max::CHESS_CASTLING_PATTERNS) {
        const uint64_t kingSource = 1ULL << castle.kingFrom;
        const uint64_t rookSource = 1ULL << castle.rookFrom;
        const bool partnerRemoved =
            (before.selectedSquare == castle.kingFrom &&
             (removed & rookSource) != 0ULL) ||
            (before.selectedSquare == castle.rookFrom &&
             (removed & kingSource) != 0ULL);
        if (partnerRemoved &&
            IsCastlingPatternEligible(model.acknowledged, castle)) {
            return (1ULL << castle.kingTo) | (1ULL << castle.rookTo);
        }
    }
    return 0ULL;
}

} // namespace

bool GameStateMachine::IsConfirmedNextSidePickupObservation(
    const TaggedInteractionState& phase,
    const uint64_t changedMask,
    const uint64_t occupiedMask) const {
    if (phase.tag != ChessInteractionPhase::Settling ||
        !m_humanSettleActive || !IsHumanPlan(phase) ||
        !m_settleTimer.IsActive()) {
        return false;
    }
    const uint64_t removedMask = changedMask & ~occupiedMask;
    const uint64_t nextSideOccupiedMask =
        phase.plan.kind == PhysicalTransitionKind::DuckRelocation
            ? m_chessInteractionModel.acknowledged.moverOccupiedMask
            : m_chessInteractionModel.acknowledged.opponentOccupiedMask;
    return changedMask == removedMask && removedMask != 0ULL &&
           (removedMask & (removedMask - 1ULL)) == 0ULL &&
           (removedMask & nextSideOccupiedMask) != 0ULL;
}

void GameStateMachine::HandleBoardObservation(const uint64_t changedMask,
                                              const uint64_t occupiedMask,
                                              const uint32_t tMs,
                                              const uint32_t sequence,
                                              const uint8_t deferredDuckDestinationSquare) {
    const uint32_t effectiveSequence = sequence != 0U
                                           ? sequence
                                           : m_chessInteractionModel.board.sequence + 1U;
    const TaggedInteractionState before = m_chessInteractionModel.phase;
    ChessInteractionEvent rawObservation = ChessInteractionEvent::MakeBoardObservation(
        tMs, effectiveSequence, changedMask, occupiedMask);
    const uint64_t coalescedLandings = GetCoalescedDuckLandings(
        m_chessInteractionModel, rawObservation);
    uint8_t deferredDuckDestination = deferredDuckDestinationSquare;
    if (deferredDuckDestination >= 64U) {
        deferredDuckDestination = GetDeferredDuckDestinationForObservation(
            changedMask, occupiedMask);
    }
    if (deferredDuckDestination < 64U) {
        const uint64_t duckLanding = 1ULL << deferredDuckDestination;
        const uint64_t chessLanding = coalescedLandings & ~duckLanding;
        if ((coalescedLandings & duckLanding) == 0ULL ||
            (m_validDestMaskKnown &&
             (coalescedLandings & m_validDestMask) != chessLanding)) {
            deferredDuckDestination = CHESS_INTERACTION_INVALID_SQUARE;
        }
    }
    rawObservation.deferredDuckDestinationSquare = deferredDuckDestination;
    const ChessInteractionEvent chessObservation =
        ProjectHumanChessObservation(m_chessInteractionModel, rawObservation);
    uint64_t transitionChangedMask = changedMask;
    const bool wasUndo = IsUndoPromptActive();
    const bool wasMismatched =
        ProjectHumanChessOccupancy(
            m_chessInteractionModel.acknowledged, before,
            m_chessInteractionModel.board.occupiedMask) !=
        GetInteractionExpectedPosition().occupiedMask;

    if (changedMask != 0ULL) {
        m_lastSensorSquare = LastSquareInMask(changedMask);
        m_lastInteractionElapsed.Reset();
        m_cpuExecution.idleTimeoutFired = false;
    }
    if (before.tag == ChessInteractionPhase::Idle &&
        !m_placementModeActive && !m_kingQueenSwapPromptActive &&
        !m_promotionUx.IsActive()) {
        m_interactionStartedFromMismatch = wasMismatched;
    }

    // These modes own board input. They need the canonical atomic snapshot but
    // must not let the normal move reducer infer a move behind the modal.
    const bool correctiveModal = m_placementModeActive ||
                                 m_kingQueenSwapPromptActive ||
                                 m_promotionUx.IsActive() || m_promotionChoicePending;
    if (correctiveModal) {
        ReduceInteraction(
            ChessInteractionEvent::MakeModalBoardObservation(
                tMs, effectiveSequence, changedMask, occupiedMask));
        if (m_placementModeActive) {
            UpdatePlacementMode();
        } else if (m_kingQueenSwapPromptActive) {
            UpdateKingQueenSwapPromptProgress();
        }
        return;
    }

    if (m_cpuThinkingActive) {
        ReduceInteractionBoardObservation(changedMask,
                                          occupiedMask,
                                          tMs,
                                          effectiveSequence);
        (void)ProjectInteractionCorrection();
        return;
    }

    if (wasUndo) {
        ReduceInteractionBoardObservation(changedMask,
                                          occupiedMask,
                                          tMs,
                                          effectiveSequence);
        const PhysicalTransitionPlan& plan =
            before.plan;
        const uint64_t target = plan.afterOccupiedMask;
        const uint64_t disallowed =
            changedMask & (occupiedMask ^ target) &
            ~GetLegalProgressPhysicalMask(before);
        m_humanIllegalMask |= disallowed;
        m_humanIllegalMask &= ~(changedMask & ~(occupiedMask ^ target));
        if (disallowed != 0ULL) {
            RestartIllegalFlashCadence();
        }
        TryResolveUndoPromptIfSatisfied();
        return;
    }

    const bool restoringSettledDuckPlan =
        before.tag == ChessInteractionPhase::Correction &&
        before.plan.kind == PhysicalTransitionKind::DuckRelocation;
    const uint64_t expectedMask = restoringSettledDuckPlan
                                      ? before.plan.afterOccupiedMask
                                      : GetInteractionExpectedPosition().occupiedMask;
    if (m_humanIllegalMask != 0ULL || m_captureReturnMask != 0ULL) {
        // A marked selected source can only be a coalesced landing to lift.
        const uint64_t selectedLanding =
            before.tag == ChessInteractionPhase::PieceLifted &&
                    before.selectedSquare < 64U
                ? 1ULL << before.selectedSquare : 0ULL;
        if (before.tag == ChessInteractionPhase::Correction || IsDuckRelocationActive()) {
            ReduceInteractionBoardObservation(changedMask,
                                              occupiedMask,
                                              tMs,
                                              effectiveSequence);
        } else {
            ReduceInteraction(
                ChessInteractionEvent::MakeCaptureProgressBoardObservation(
                    tMs, effectiveSequence, changedMask, occupiedMask));
        }
        const uint64_t legalProgressMask =
            GetLegalProgressPhysicalMask(before) |
            GetLegalProgressPhysicalMask(m_chessInteractionModel.phase);
        const uint64_t newlyIncorrect =
            changedMask & (occupiedMask ^ expectedMask) &
            ~legalProgressMask;
        const uint64_t humanCorrected =
            changedMask & m_humanIllegalMask &
            ~(occupiedMask ^ (expectedMask & ~selectedLanding));
        const uint64_t captureCorrected = changedMask & occupiedMask &
                                          m_captureReturnMask;
        m_humanIllegalMask |= newlyIncorrect;
        if (humanCorrected != 0ULL) {
            m_humanIllegalMask &= ~humanCorrected;
        }
        if (newlyIncorrect != 0ULL || humanCorrected != 0ULL) {
            // A changed correction target begins a new graded-feedback
            // interval so its strong stage starts on the informative phase.
            RestartIllegalFlashCadence();
        }
        if (captureCorrected != 0ULL) {
            m_captureReturnMask &= ~captureCorrected;
            RestartCaptureReturnFlashCadence();
        }
        ReconcileHumanPlanAfterModal();
        return;
    }

    if (m_cpuExecution.illegalMask != 0ULL &&
        !m_cpuExecution.queued) {
        ReduceInteraction(
            ChessInteractionEvent::MakeModalBoardObservation(
                tMs, effectiveSequence, changedMask, occupiedMask));
        const uint64_t corrected =
            changedMask & m_cpuExecution.illegalMask;
        if (corrected != 0ULL) {
            m_cpuExecution.illegalMask &= ~corrected;
            RestartIllegalFlashCadence();
        }
        return;
    }

    if (m_cpuExecution.queued &&
        IsCpuDirectedTransition(before.plan)) {
        if (HandleQueuedCpuCorrectionObservation(
                before, changedMask, occupiedMask,
                tMs, effectiveSequence)) {
            return;
        }
    }

    if (coalescedLandings != 0ULL && deferredDuckDestination >= 64U) {
        // The observation has no temporal order. Retain the selected mover
        // and mark both landings until both are lifted, then accept chess first.
        ReduceInteraction(ChessInteractionEvent::MakeModalBoardObservation(
            tMs, effectiveSequence, changedMask, occupiedMask));
        m_humanIllegalMask |= coalescedLandings;
        RestartIllegalFlashCadence();
        return;
    }

    if (before.tag == ChessInteractionPhase::ExecutingPlan &&
        before.plan.source == PhysicalTransitionSource::Human &&
        !(before.plan.kind == PhysicalTransitionKind::EnPassant &&
          before.plan.removalRequired && !before.plan.progress.removalComplete)) {
        const uint64_t disallowed = chessObservation.changedMask &
                                    ~GetLegalProgressPhysicalMask(before);
        if (disallowed != 0ULL) {
            ReduceInteraction(
                ChessInteractionEvent::MakeCaptureProgressBoardObservation(
                    tMs, effectiveSequence, changedMask, occupiedMask));
            m_humanIllegalMask |= disallowed;
            RestartIllegalFlashCadence();
            return;
        }
    }
    // Once a selected source has engine-derived legal guidance, a later
    // placement outside that mask is immediately local feedback. A source and
    // destination first seen in one atomic scan has no source-specific mask and
    // remains transactionally engine-validated.
    const uint64_t selectedSourceMask =
        before.selectedSquare < 64U ? (1ULL << before.selectedSquare) : 0ULL;
    const uint64_t castleContinuation = CastleContinuationPlacementMask(
        m_chessInteractionModel, before, changedMask, occupiedMask);
    const uint64_t invalidSequentialPlacement =
        chessObservation.changedMask & chessObservation.occupiedMask &
        ~(m_validDestMask | selectedSourceMask | castleContinuation);
    if (before.tag == ChessInteractionPhase::PieceLifted &&
        m_validDestMaskKnown && invalidSequentialPlacement != 0ULL) {
        ReduceInteraction(
            ChessInteractionEvent::MakeCaptureProgressBoardObservation(
                tMs, effectiveSequence, changedMask, occupiedMask));
        m_humanIllegalMask |= invalidSequentialPlacement;
        RestartIllegalFlashCadence();
        return;
    }

    // Human settle yields to a destination relift or full capture restoration.
    // An incomplete en-passant plan additionally accepts only its required
    // capture removal; the reducer turns every mixed/unrelated observation
    // into full-board correction. Every other observation confirms the old
    // move exactly once, then remains available for action-result replay as the
    // next move's atomic input.
    bool confirmedSettleForObservation = false;
    bool settleProgressRegressed = false;
    if (m_humanSettleActive && IsHumanPlan(before) &&
        changedMask != 0ULL) {
        if ((m_activeHumanSettleRequiresO || IsEnPassantSettleBlocked()) &&
            IsEarlyDuckLiftAllowed(before) &&
            chessObservation.changedMask == 0ULL) {
            ReduceInteractionBoardObservation(
                changedMask, occupiedMask, tMs, effectiveSequence,
                deferredDuckDestination);
            return;
        }
        uint64_t destinationMask = 0ULL;
        if (before.plan.kind == PhysicalTransitionKind::Castling) {
            for (uint8_t i = 0U; i < before.plan.relocationCount; ++i) {
                if (before.plan.relocations[i].to < 64U) {
                    destinationMask |=
                        1ULL << before.plan.relocations[i].to;
                }
            }
        } else if (m_renderState.settleSquare < 64U) {
            destinationMask = 1ULL << m_renderState.settleSquare;
        }
        const uint64_t removedMask = changedMask & ~occupiedMask;
        const bool destinationOnlyRelift =
            changedMask == removedMask &&
            (removedMask & destinationMask) != 0ULL &&
            (removedMask & (removedMask - 1ULL)) == 0ULL;
        const bool awaitingClaimedAction =
            before.tag == ChessInteractionPhase::AwaitingAction &&
            before.pendingActionToken == m_claimedInteractionActionToken;
        const bool canCancelDestinationRelift =
            destinationOnlyRelift && !awaitingClaimedAction;
        const bool incompleteEnPassant =
            before.plan.kind == PhysicalTransitionKind::EnPassant &&
            before.plan.removalRequired &&
            !before.plan.progress.removalComplete;
        const uint64_t captureMask =
            before.plan.removalSquare < 64U
                ? (1ULL << before.plan.removalSquare)
                : 0ULL;
        const bool requiredCaptureOnly =
            incompleteEnPassant && chessObservation.changedMask == captureMask &&
            (chessObservation.occupiedMask & captureMask) == 0ULL;
        const bool enPassantCaptureRestored =
            before.plan.kind == PhysicalTransitionKind::EnPassant &&
            before.plan.progress.removalComplete &&
            changedMask == captureMask &&
            (occupiedMask & captureMask) != 0ULL;
        const bool unclaimedCastleNoLongerComplete =
            before.tag == ChessInteractionPhase::AwaitingAction &&
            before.plan.kind == PhysicalTransitionKind::Castling &&
            before.pendingActionToken != m_claimedInteractionActionToken &&
            chessObservation.occupiedMask != before.plan.afterOccupiedMask;
        const bool captureFullyRestored =
            before.tag == ChessInteractionPhase::Settling &&
            !before.plan.progress.actionAcknowledged && before.plan.removalRequired &&
            before.plan.removalSquare == before.plan.moveTo &&
            chessObservation.occupiedMask == before.plan.beforeOccupiedMask;
        settleProgressRegressed = canCancelDestinationRelift ||
                                  enPassantCaptureRestored ||
                                  captureFullyRestored ||
                                  unclaimedCastleNoLongerComplete;
        const bool confirmedNextSidePickup =
            IsConfirmedNextSidePickupObservation(
                before, changedMask, occupiedMask);
        const bool invalidTimedDuckTransition =
            before.tag == ChessInteractionPhase::Settling &&
            before.plan.kind == PhysicalTransitionKind::DuckRelocation &&
            !m_activeHumanSettleRequiresO &&
            !confirmedNextSidePickup && !settleProgressRegressed;
        if (before.tag == ChessInteractionPhase::Settling &&
            ((m_activeHumanSettleRequiresO && !confirmedNextSidePickup) ||
             invalidTimedDuckTransition) &&
            !settleProgressRegressed && !requiredCaptureOnly) {
            // A selectable Duck placement and an accepted Circle animation
            // yield only to one next-side pickup. Extra placements, mover-side
            // lifts and mixed changes need correction before commit. Timed
            // Duck correction restarts its confirmation window on recovery;
            // an accepted Circle retains its existing deadline.
            HandleBlockedHumanSettleObservation(
                ChessInteractionEvent::MakeBoardObservation(
                    tMs, effectiveSequence, changedMask, occupiedMask),
                invalidTimedDuckTransition);
            return;
        }
        if (unclaimedCastleNoLongerComplete) {
            if (before.pendingActionToken ==
                m_movedSquaresSnapshotActionToken) {
                // Castling reaches AwaitingAction immediately so its composite
                // settle can animate while the adapter runs. If physical work
                // regresses before that action is claimed, restore the
                // pre-action UI/history classifier snapshot along with the
                // token; otherwise an abandoned castle can make an untouched
                // game look as though pieces have permanently moved.
                m_movedSquaresMask =
                    m_movedSquaresMaskBeforeLastAcknowledgedHumanMove;
                m_movedSquaresSnapshotActionToken = 0U;
            }
            const uint64_t unrelated =
                chessObservation.changedMask &
                ~GetLegalProgressPhysicalMask(before);
            if (unrelated != 0ULL) {
                m_humanIllegalMask |= unrelated;
                RestartIllegalFlashCadence();
            }
        }
        if (!settleProgressRegressed && !requiredCaptureOnly &&
            !incompleteEnPassant) {
            FinalizeMove();
            confirmedSettleForObservation = true;
        }
    } else if (m_cpuExecution.moveSettleActive && changedMask != 0ULL) {
        const uint64_t destinationMask =
            m_renderState.settleSquare < 64U
                ? (1ULL << m_renderState.settleSquare)
                : 0ULL;
        if (changedMask == destinationMask) {
            ReduceInteraction(
                ChessInteractionEvent::MakeModalBoardObservation(
                    tMs, effectiveSequence, changedMask, occupiedMask));
            m_cpuExecution.settleDestinationSuppressed =
                (occupiedMask & destinationMask) == 0ULL;
            return;
        }
        if (m_cpuExecution.settleDestinationSuppressed) {
            transitionChangedMask |= destinationMask;
        }
        FinalizeCpuMoveSettling(false);
    }
    ReduceInteractionBoardObservation(transitionChangedMask,
                                      occupiedMask,
                                      tMs,
                                      effectiveSequence,
                                      deferredDuckDestination);
    if (m_humanSettleActive && settleProgressRegressed) {
        const uint8_t settledSquare = m_renderState.settleSquare;
        if (settledSquare < 64U) {
            StartSettleAfterglow(m_renderState, settledSquare, true,
                                 INVALID_SQUARE);
        }
        const ChessInteractionPhase tag =
            m_chessInteractionModel.phase.tag;
        if (tag == ChessInteractionPhase::Settling ||
            tag == ChessInteractionPhase::AwaitingAction) {
            ReduceInteraction(
                ChessInteractionEvent::MakeSettleProgressRegressed());
        }
        m_settleTimer.Stop();
        m_activeSettleDurationMs = 0U;
        m_humanSettleActive = false;
        m_renderState.settleSquare = INVALID_SQUARE;
        ClearValidDestMask();
    }
    ReconcileObservedMoveProgress(
        before,
        ChessInteractionEvent::MakeBoardObservation(
            tMs, effectiveSequence, changedMask, occupiedMask),
        confirmedSettleForObservation);
}

} // namespace runtime
