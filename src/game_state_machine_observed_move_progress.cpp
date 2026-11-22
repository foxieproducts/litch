#include "runtime/game_state_machine.hpp"

#include "logger.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"

namespace runtime {
namespace {

uint8_t LastSquareInMask(const uint64_t mask) {
    uint8_t result = CHESS_INTERACTION_INVALID_SQUARE;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((mask & (1ULL << square)) != 0ULL) result = square;
    }
    return result;
}

bool IsHumanPlan(const TaggedInteractionState& phase) {
    return phase.plan.source == PhysicalTransitionSource::Human &&
           phase.plan.kind != PhysicalTransitionKind::None;
}

bool IsActivePlanPhase(const ChessInteractionPhase tag) {
    return tag == ChessInteractionPhase::ExecutingPlan ||
           tag == ChessInteractionPhase::Settling ||
           tag == ChessInteractionPhase::AwaitingAction;
}

} // namespace

void GameStateMachine::ReconcileObservedMoveProgress(
    const TaggedInteractionState& before,
    const ChessInteractionEvent& event,
    const bool confirmedSettleForObservation) {
    const uint64_t changedMask = event.changedMask;
    const uint64_t occupiedMask = event.occupiedMask;
    const uint64_t expectedMask = GetInteractionExpectedPosition().occupiedMask;
    const uint64_t earlyDuckMask = IsEarlyDuckLiftAllowed(before)
        ? GetKnownNeutralDuckMask(m_chessInteractionModel.acknowledged) : 0ULL;
    const TaggedInteractionState& after = m_chessInteractionModel.phase;

    if (confirmedSettleForObservation &&
        after.tag == ChessInteractionPhase::AwaitingAction) {
        const ChessPositionState projected = ProjectChessPositionAfterPlan(
            m_chessInteractionModel.acknowledged, after.plan);
        const uint64_t chessOccupancy = ProjectHumanChessOccupancy(
            m_chessInteractionModel.acknowledged, before, occupiedMask);
        const uint64_t unexpectedPlacement =
            changedMask & chessOccupancy & ~projected.occupiedMask;
        const uint64_t removedProjectedPiece =
            changedMask & ~occupiedMask & projected.occupiedMask;
        const bool sideModelAvailable =
            (projected.moverOccupiedMask |
             projected.opponentOccupiedMask) != 0ULL;
        const bool plausibleNextMoverPickup =
            sideModelAvailable &&
            (removedProjectedPiece & projected.moverOccupiedMask) != 0ULL;
        if (unexpectedPlacement != 0ULL && !plausibleNextMoverPickup) {
            // A bare extra placement is immediately actionable feedback. A
            // source+destination observation from the projected next mover is
            // preserved for engine acknowledgement and authoritative replay;
            // this is the window used by fast legal moves such as castling.
            m_humanIllegalMask |= unexpectedPlacement;
            RestartIllegalFlashCadence();
        }
    }

    if (ProjectInteractionCorrection()) {
        m_settleTimer.Stop();
        m_activeSettleDurationMs = 0U;
        m_humanSettleActive = false;
        m_renderState.settleSquare = INVALID_SQUARE;
        return;
    }

    // Clear correction markers only on squares that physically match the
    // authoritative board again. Other markers remain latched.
    const uint64_t corrected =
        changedMask & ~(occupiedMask ^ GetInteractionExpectedPosition().occupiedMask);
    if ((m_humanIllegalMask & corrected) != 0ULL) {
        m_humanIllegalMask &= ~corrected;
        RestartIllegalFlashCadence();
    }
    if ((m_captureReturnMask & corrected) != 0ULL) {
        m_captureReturnMask &= ~corrected;
        RestartCaptureReturnFlashCadence();
    }

    UpdateCastlingRelocationFeedback(before, after);

    if (before.tag != ChessInteractionPhase::PieceLifted &&
        after.tag == ChessInteractionPhase::PieceLifted) {
        ClearValidDestMask();
        logger::Trace("pickup: from=%u",
                      static_cast<unsigned>(after.selectedSquare));
    }
    if (before.tag == ChessInteractionPhase::PieceLifted &&
        after.tag == ChessInteractionPhase::Idle) {
        if (before.selectedSquare < 64U) {
            StartSettleAfterglow(m_renderState,
                                 before.selectedSquare,
                                 true,
                                 INVALID_SQUARE);
        }
        ClearValidDestMask();
        m_interactionStartedFromMismatch = false;
    }
    if (before.tag == ChessInteractionPhase::PieceLifted &&
        after.tag == ChessInteractionPhase::PieceLifted) {
        const uint64_t removed = changedMask & ~occupiedMask;
        const uint64_t sourceMask = before.selectedSquare < 64U
                                        ? (1ULL << before.selectedSquare)
                                        : 0ULL;
        const uint64_t unexpectedRemoval = removed & ~sourceMask & ~earlyDuckMask;
        if (unexpectedRemoval != 0ULL) {
            m_captureReturnMask |= unexpectedRemoval;
            RestartCaptureReturnFlashCadence();
        }
    }
    if (before.tag == ChessInteractionPhase::Idle &&
        after.tag == ChessInteractionPhase::Idle) {
        const uint64_t unexpectedPlacement =
            changedMask & occupiedMask & ~expectedMask;
        if (unexpectedPlacement != 0ULL) {
            m_humanIllegalMask |= unexpectedPlacement;
            RestartIllegalFlashCadence();
        }
    }

    const bool primaryHumanPlacementComplete =
        IsHumanPlan(after) && after.plan.relocationCount == 1U &&
        (after.plan.progress.relocationsPlacedMask & 0x01U) != 0U &&
        (after.tag == ChessInteractionPhase::ExecutingPlan ||
         after.tag == ChessInteractionPhase::Settling);
    const uint64_t removedInObservation = changedMask & ~occupiedMask;
    const uint64_t followupDifference =
        IsHumanPlan(after)
            ? occupiedMask ^ after.plan.afterOccupiedMask
            : 0ULL;
    const bool coalescedNextSidePickup =
        before.tag == ChessInteractionPhase::PieceLifted &&
        after.tag == ChessInteractionPhase::Settling &&
        followupDifference != 0ULL &&
        (followupDifference & (followupDifference - 1ULL)) == 0ULL &&
        followupDifference == removedInObservation &&
        (followupDifference &
         m_chessInteractionModel.acknowledged.opponentOccupiedMask) != 0ULL;
    const bool castlingPlacementComplete =
        IsHumanPlan(after) &&
        after.plan.kind == PhysicalTransitionKind::Castling &&
        after.plan.relocationCount == 2U &&
        (after.plan.progress.relocationsPlacedMask & 0x03U) == 0x03U &&
        (before.plan.progress.relocationsPlacedMask & 0x03U) != 0x03U &&
        (after.tag == ChessInteractionPhase::AwaitingAction ||
         after.tag == ChessInteractionPhase::Settling);
    const bool newHumanInteractionOwnsState =
        after.tag == ChessInteractionPhase::PieceLifted ||
        (before.tag == ChessInteractionPhase::Idle &&
         (after.tag == ChessInteractionPhase::CaptureStaged ||
          after.tag == ChessInteractionPhase::Correction || IsHumanPlan(after)));
    if (m_humanSettleActive && newHumanInteractionOwnsState) {
        const uint8_t settledSquare = m_renderState.settleSquare;
        if (settledSquare < 64U) {
            StartSettleAfterglow(m_renderState,
                                 settledSquare,
                                 true,
                                 INVALID_SQUARE);
        }
        m_settleTimer.Stop();
        m_activeSettleDurationMs = 0U;
        m_humanSettleActive = false;
        m_renderState.settleSquare = INVALID_SQUARE;
        ClearValidDestMask();
    }
    if (!m_humanSettleActive && primaryHumanPlacementComplete) {
        StartSettling(after.plan.moveTo);
        if (coalescedNextSidePickup) {
            if (m_activeHumanSettleRequiresO) {
                m_humanIllegalMask |= followupDifference;
                RestartIllegalFlashCadence();
            } else {
                FinalizeMove();
            }
        }
    } else if (!m_humanSettleActive && castlingPlacementComplete) {
        const uint64_t placedDestinations = changedMask & occupiedMask &
                                            ((1ULL << after.plan.relocations[0].to) |
                                             (1ULL << after.plan.relocations[1].to));
        const uint8_t placedSquare = LastSquareInMask(placedDestinations);
        StartSettling(placedSquare < 64U ? placedSquare : after.plan.moveTo);
    }

    if (after.plan.source == PhysicalTransitionSource::Human &&
        after.plan.kind == PhysicalTransitionKind::Castling &&
        IsActivePlanPhase(after.tag)) {
        if (after.tag == ChessInteractionPhase::AwaitingAction) {
            CommitCastlingComposite();
        } else {
            ClearValidDestMask();
            if (after.requiredAction == RequiredPhysicalAction::PlacePrimary) {
                SetValidDestMask(1ULL << after.plan.relocations[0].to);
            } else if (after.requiredAction == RequiredPhysicalAction::PlaceSecondary) {
                SetValidDestMask(1ULL << after.plan.relocations[1].to);
            }
        }
    }

    if (IsCpuDirectedTransition(after.plan) &&
        m_cpuExecution.queued) {
        (void)TryFinalizeCpuMove();
    }

    if (after.tag == ChessInteractionPhase::Settling &&
        before.plan.kind == PhysicalTransitionKind::EnPassant &&
        before.plan.removalRequired &&
        !before.plan.progress.removalComplete &&
        after.plan.progress.removalComplete &&
        !m_activeHumanSettleRequiresO) {
        FinalizeMove();
    }
    const uint64_t duckMask = GetKnownNeutralDuckMask(
        m_chessInteractionModel.acknowledged);
    if (m_humanSettleActive && !m_activeHumanSettleRequiresO &&
        after.tag == ChessInteractionPhase::Settling &&
        IsEarlyDuckLiftAllowed(after) && duckMask != 0ULL &&
        (occupiedMask & duckMask) == 0ULL &&
        ProjectHumanChessOccupancy(
            m_chessInteractionModel.acknowledged, after, occupiedMask) ==
            after.plan.afterOccupiedMask &&
        m_humanIllegalMask == 0ULL && m_captureReturnMask == 0ULL) {
        FinalizeMove();
    }
}

} // namespace runtime
