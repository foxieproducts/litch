#include "chess_interaction_capture.hpp"

#include "chess_interaction_model_internal.hpp"

namespace runtime::chess_interaction_capture {
namespace {

bool IsSingle(const uint64_t mask) {
    return mask != 0ULL && (mask & (mask - 1ULL)) == 0ULL;
}

uint8_t ToSquare(const uint64_t mask) {
    for (uint8_t square = 0; square < 64U; ++square) {
        if ((mask & (1ULL << square)) != 0ULL) {
            return square;
        }
    }
    return CHESS_INTERACTION_INVALID_SQUARE;
}

bool IsLegalTargetFirstRemoval(const ChessInteractionModel& model,
                               const uint8_t square) {
    if (model.acknowledged.legalCaptureTargetsValid &&
        (model.acknowledged.legalCaptureTargetsMask &
         (1ULL << square)) != 0ULL) {
        return true;
    }
    return model.acknowledged.enPassantTargetSquare < 64U &&
           model.acknowledged.enPassantCaptureSquare == square;
}

void EnterSettling(ChessInteractionModel& model) {
    model.phase.tag = ChessInteractionPhase::Settling;
    model.phase.requiredAction = RequiredPhysicalAction::None;
    model.phase.pendingActionToken = 0U;
}

void UpdateRequiredAction(ChessInteractionModel& model,
                          const uint64_t occupied) {
    PhysicalTransitionPlan& plan = model.phase.plan;
    if ((plan.progress.relocationsLiftedMask & 0x01U) == 0U) {
        model.phase.requiredAction = RequiredPhysicalAction::LiftPrimary;
    } else if (!plan.progress.removalComplete) {
        model.phase.requiredAction = RequiredPhysicalAction::RemoveCapture;
    } else if ((plan.progress.relocationsPlacedMask & 0x01U) == 0U) {
        model.phase.requiredAction = RequiredPhysicalAction::PlacePrimary;
    } else if (occupied != plan.afterOccupiedMask) {
        EnterChessInteractionCorrection(
            model, occupied ^ plan.afterOccupiedMask);
    } else {
        EnterSettling(model);
    }
}

bool KnownInvalidLanding(const CaptureLegality& legality,
                        const uint8_t from, const uint8_t to) {
    return from < 64U && to < 64U &&
        (legality.knownSources & (1ULL << from)) != 0ULL &&
        (legality.landingsBySource[from] & (1ULL << to)) == 0ULL;
}

bool BuildPlan(ChessInteractionModel& model,
               const uint8_t from,
               const uint8_t captured,
               const uint64_t occupied) {
    const bool enPassant = captured == model.acknowledged.enPassantCaptureSquare &&
                           IsEnPassantSource(model.acknowledged, from);
    const uint8_t to = enPassant
                           ? model.acknowledged.enPassantTargetSquare
                           : captured;
    if (KnownInvalidLanding(model.acknowledgedCaptureLegality, from, to)) return false;

    const bool promotion =
        !enPassant &&
        (model.acknowledged.pawnMask & (1ULL << from)) != 0ULL &&
        (to / 8U == 0U || to / 8U == 7U);
    TaggedInteractionState phase;
    phase.tag = ChessInteractionPhase::ExecutingPlan;
    phase.plan.kind = enPassant
                          ? PhysicalTransitionKind::EnPassant
                          : (promotion ? PhysicalTransitionKind::Promotion
                                       : PhysicalTransitionKind::Capture);
    phase.plan.beforeOccupiedMask = model.acknowledged.occupiedMask;
    phase.plan.afterOccupiedMask =
        (model.acknowledged.occupiedMask & ~(1ULL << from) & ~(1ULL << captured)) |
        (1ULL << to);
    phase.plan.moveFrom = from;
    phase.plan.moveTo = to;
    phase.plan.relocationCount = 1U;
    phase.plan.relocations[0] = {from, to};
    phase.plan.removalRequired = true;
    phase.plan.removalSquare = captured;
    phase.plan.promotionRequired = promotion;
    // CaptureStaged already observed the victim absent. Preserve that evidence
    // when the same later scan lifts the capturer and fills its destination.
    phase.plan.progress.removalComplete =
        model.phase.tag == ChessInteractionPhase::CaptureStaged &&
        model.phase.selectedSquare == captured;
    model.phase = phase;
    chess_interaction_internal::UpdatePlanProgress(
        model.phase.plan, occupied);
    model.phase.requiredAction =
        chess_interaction_internal::GetNextPlanAction(model.phase.plan);
    return true;
}

bool BeginPlan(ChessInteractionModel& model,
               const uint8_t from,
               const uint8_t captured,
               const uint64_t occupied) {
    if (!BuildPlan(model, from, captured, occupied)) return false;
    UpdateRequiredAction(model, occupied);
    return true;
}

} // namespace

bool IsKnownInvalidHumanCapture(const CaptureLegality& legality, const PhysicalTransitionPlan& plan) {
    return plan.source == PhysicalTransitionSource::Human && plan.removalRequired &&
        (plan.kind == PhysicalTransitionKind::Capture || plan.kind == PhysicalTransitionKind::EnPassant ||
         plan.kind == PhysicalTransitionKind::Promotion) &&
        KnownInvalidLanding(legality, plan.moveFrom, plan.moveTo);
}

bool IsEnPassantSource(const ChessPositionState& position, const uint8_t from) {
    const uint8_t target = position.enPassantTargetSquare;
    const uint8_t victim = position.enPassantCaptureSquare;
    if (from >= 64U || target >= 64U || victim >= 64U ||
        (position.pawnMask & position.moverOccupiedMask & (1ULL << from)) == 0ULL ||
        (position.occupiedMask & (1ULL << target)) != 0ULL) {
        return false;
    }
    const int fromRank = from / 8U;
    const int targetRank = target / 8U;
    const int fileDelta = int(from % 8U) - int(victim % 8U);
    const bool forward = position.sideToMoveValid
        ? targetRank == fromRank + (position.sideToMoveBlack ? 1 : -1)
        : targetRank == fromRank - 1 || targetRank == fromRank + 1;
    // EP metadata describes an opportunity for a neighboring pawn. Other
    // pieces, including pawns one rank behind, can capture its victim normally.
    return fromRank == victim / 8U && target % 8U == victim % 8U &&
           (fileDelta == -1 || fileDelta == 1) && forward;
}

void RecordProgressDuringCorrection(ChessInteractionModel& model,
                                    const ChessInteractionEvent& event) {
    if (model.phase.tag == ChessInteractionPhase::PieceLifted) {
        const uint8_t from = model.phase.selectedSquare;
        const uint64_t opponentRemoved = event.changedMask &
            ~event.occupiedMask & model.acknowledged.opponentOccupiedMask;
        if (from >= 64U || (event.occupiedMask & (1ULL << from)) != 0ULL ||
            !IsSingle(opponentRemoved)) {
            return;
        }
        const uint8_t captured = ToSquare(opponentRemoved);
        if (captured == model.acknowledged.enPassantCaptureSquare &&
            IsEnPassantSource(model.acknowledged, from)) {
            // Incomplete human en passant requires canonical restoration
            // after unrelated input, rather than progress behind correction.
            return;
        }
        // The mover was selected before correction. Only a newly observed
        // victim vacancy can begin its capture; an already-empty square is
        // not proof for a new attempt.
        BuildPlan(model, from, captured, event.occupiedMask);
        return;
    }

    if (model.phase.tag != ChessInteractionPhase::ExecutingPlan) {
        // In particular, an existing human settle retains its lifecycle and
        // cancellation owner. Snapshot-only modes never reach this helper.
        return;
    }
    const PhysicalTransitionPlan& plan = model.phase.plan;
    if (IsCpuDirectedTransition(plan)) {
        // A fixed CPU plan survives correction, but its progress must include
        // returns as well as removals so stale proof cannot authorize a move.
        chess_interaction_internal::UpdatePlanProgress(
            model.phase.plan, event.occupiedMask);
        model.phase.requiredAction =
            chess_interaction_internal::GetNextPlanAction(model.phase.plan);
        return;
    }
    if (plan.source != PhysicalTransitionSource::Human ||
        !plan.removalRequired || plan.removalSquare != plan.moveTo ||
        plan.moveFrom >= 64U) {
        return;
    }
    if ((event.occupiedMask & (1ULL << plan.moveFrom)) != 0ULL) {
        // Returning the selected source cancels this capture attempt, even
        // while an unrelated correction hides the normal cancellation path.
        // Keep source selection until physical restoration is reconciled;
        // a later pickup must obtain fresh victim-removal evidence.
        const uint8_t from = plan.moveFrom;
        model.phase = TaggedInteractionState{};
        model.phase.tag = ChessInteractionPhase::PieceLifted;
        model.phase.selectedSquare = from;
        return;
    }
    chess_interaction_internal::UpdatePlanProgress(
        model.phase.plan, event.occupiedMask);
    model.phase.requiredAction =
        chess_interaction_internal::GetNextPlanAction(model.phase.plan);
}

bool ReduceIdleObservation(ChessInteractionModel& model,
                           const ChessInteractionEvent& event) {
    const uint64_t removed = event.changedMask & ~event.occupiedMask;
    const uint64_t moverRemoved = removed & model.acknowledged.moverOccupiedMask;
    const uint64_t opponentRemoved = removed & model.acknowledged.opponentOccupiedMask;
    if (IsSingle(moverRemoved) && IsSingle(opponentRemoved)) {
        if (!BeginPlan(model, ToSquare(moverRemoved), ToSquare(opponentRemoved),
                       event.occupiedMask)) {
            EnterChessInteractionCorrection(model, event.occupiedMask ^ model.acknowledged.occupiedMask);
        }
        return true;
    }
    if (moverRemoved == 0ULL && IsSingle(opponentRemoved)) {
        const uint8_t target = ToSquare(opponentRemoved);
        if (!IsLegalTargetFirstRemoval(model, target)) {
            EnterChessInteractionCorrection(
                model,
                event.occupiedMask ^ model.acknowledged.occupiedMask);
            return true;
        }
        model.phase = TaggedInteractionState{};
        model.phase.tag = ChessInteractionPhase::CaptureStaged;
        model.phase.selectedSquare = target;
        model.phase.requiredAction = RequiredPhysicalAction::LiftPrimary;
        return true;
    }
    return false;
}

bool ReducePieceLiftedObservation(ChessInteractionModel& model,
                                  const ChessInteractionEvent& event) {
    const uint64_t removed = event.changedMask & ~event.occupiedMask;
    const uint64_t opponentRemoved = removed & model.acknowledged.opponentOccupiedMask;
    if (IsSingle(opponentRemoved)) {
        // A different opponent piece does not replace the selected mover's
        // pickup with a capture. Existing pickup recovery owns its return.
        (void)BeginPlan(model, model.phase.selectedSquare, ToSquare(opponentRemoved),
                        event.occupiedMask);
        return true;
    }

    const uint8_t epTarget = model.acknowledged.enPassantTargetSquare;
    if (IsEnPassantSource(model.acknowledged, model.phase.selectedSquare) &&
        (event.changedMask & event.occupiedMask & (1ULL << epTarget)) != 0ULL) {
        if (!BeginPlan(model, model.phase.selectedSquare, model.acknowledged.enPassantCaptureSquare,
                       event.occupiedMask)) {
            EnterChessInteractionCorrection(model, event.occupiedMask ^ model.acknowledged.occupiedMask);
        }
        return true;
    }
    return false;
}

void ReduceCaptureStagedObservation(ChessInteractionModel& model,
                                    const ChessInteractionEvent& event) {
    const uint64_t stagedMask = 1ULL << model.phase.selectedSquare;
    const uint64_t moverRemoved =
        event.changedMask & ~event.occupiedMask &
        model.acknowledged.moverOccupiedMask;
    if ((event.changedMask & event.occupiedMask & stagedMask) != 0ULL) {
        if (moverRemoved != 0ULL &&
            (event.changedMask & ~(stagedMask | moverRemoved)) != 0ULL) {
            EnterChessInteractionCorrection(
                model, event.occupiedMask ^ model.acknowledged.occupiedMask);
            return;
        }
        if (IsSingle(moverRemoved) &&
            !(model.phase.selectedSquare == model.acknowledged.enPassantCaptureSquare &&
              IsEnPassantSource(model.acknowledged, ToSquare(moverRemoved))) &&
            BeginPlan(model, ToSquare(moverRemoved), model.phase.selectedSquare, event.occupiedMask)) {
            return;
        }
        chess_interaction_internal::ReplayIdleAfterConsumedChanges(
            model, event, stagedMask);
        return;
    }
    if (IsSingle(moverRemoved)) {
        if (!BeginPlan(model, ToSquare(moverRemoved), model.phase.selectedSquare,
                       event.occupiedMask)) {
            EnterChessInteractionCorrection(
                model, event.occupiedMask ^ model.acknowledged.occupiedMask);
        }
    } else if (moverRemoved != 0ULL) {
        EnterChessInteractionCorrection(
            model,
            event.occupiedMask ^ model.acknowledged.occupiedMask);
    }
}

} // namespace runtime::chess_interaction_capture
