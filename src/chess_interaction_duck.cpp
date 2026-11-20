#include "runtime/chess_interaction_model.hpp"

namespace runtime {
namespace {

bool IsSingleSquareMask(const uint64_t mask) {
    return mask != 0ULL && (mask & (mask - 1ULL)) == 0ULL;
}

uint8_t SquareFromMask(const uint64_t mask) {
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((mask & (1ULL << square)) != 0ULL) {
            return square;
        }
    }
    return CHESS_INTERACTION_INVALID_SQUARE;
}

uint64_t LiftedOccupancy(const PhysicalTransitionPlan& plan) {
    return plan.beforeOccupiedMask & ~(1ULL << plan.moveFrom);
}

uint8_t RequiredDestination(const TaggedInteractionState& phase) {
    return phase.selectedSquare;
}

bool IsAcknowledgedHumanChessBridge(
    const ChessInteractionModel& model,
    const uint64_t duckSourceMask) {
    const TaggedInteractionState& phase = model.phase;
    const uint64_t lifted = model.acknowledged.occupiedMask & ~duckSourceMask;
    const uint64_t extra = model.board.occupiedMask & ~lifted;
    const bool placementReady =
        (model.board.occupiedMask & model.acknowledged.occupiedMask) == lifted &&
        (extra == 0ULL || IsSingleSquareMask(extra));
    return phase.tag == ChessInteractionPhase::ExecutingPlan &&
           phase.plan.source == PhysicalTransitionSource::Human &&
           phase.plan.kind != PhysicalTransitionKind::None &&
           phase.plan.kind != PhysicalTransitionKind::DuckRelocation &&
           phase.plan.progress.actionAcknowledged &&
           phase.requiredAction == RequiredPhysicalAction::None &&
           phase.pendingActionToken == 0U && model.board.valid &&
           placementReady;
}

void ResetDestination(TaggedInteractionState& phase) {
    const uint8_t requiredDestination = RequiredDestination(phase);
    phase.plan.moveTo = requiredDestination;
    phase.plan.relocations[0].to = requiredDestination;
    phase.plan.afterOccupiedMask = phase.plan.beforeOccupiedMask;
}

void RequireDuckCorrection(ChessInteractionModel& model,
                           const uint64_t expected,
                           const uint64_t physical) {
    model.phase.tag = ChessInteractionPhase::DuckRelocation;
    model.phase.pendingActionToken = 0U;
    model.phase.correctionMask = expected ^ physical;
    model.phase.requiredAction = RequiredPhysicalAction::CorrectBoard;
    ++model.feedbackEpoch;
}

void RequireSettledDuckCorrection(ChessInteractionModel& model,
                                  const uint64_t physical) {
    model.phase.tag = ChessInteractionPhase::Correction;
    model.phase.pendingActionToken = 0U;
    model.phase.correctionMask =
        model.phase.plan.afterOccupiedMask ^ physical;
    model.phase.requiredAction = RequiredPhysicalAction::CorrectBoard;
    ++model.feedbackEpoch;
}

void ResetDuckLift(ChessInteractionModel& model) {
    model.phase.tag = ChessInteractionPhase::DuckRelocation;
    model.phase.pendingActionToken = 0U;
    ResetDestination(model.phase);
    PhysicalTransitionPlan& plan = model.phase.plan;
    plan.progress.relocationsLiftedMask = 0U;
    plan.progress.relocationsPlacedMask = 0U;
    plan.progress.actionAcknowledged = false;
    model.phase.correctionMask = 0ULL;
    model.phase.requiredAction = RequiredPhysicalAction::LiftPrimary;
}

void ReopenDuckPlacement(ChessInteractionModel& model) {
    model.phase.tag = ChessInteractionPhase::DuckRelocation;
    model.phase.pendingActionToken = 0U;
    ResetDestination(model.phase);
    PhysicalTransitionPlan& plan = model.phase.plan;
    plan.progress.relocationsPlacedMask = 0U;
    plan.progress.actionAcknowledged = false;
    model.phase.correctionMask = 0ULL;
}

void CompleteDuckPlacement(ChessInteractionModel& model,
                           const uint8_t destination) {
    PhysicalTransitionPlan& plan = model.phase.plan;
    plan.moveTo = destination;
    plan.relocations[0].to = destination;
    plan.afterOccupiedMask = LiftedOccupancy(plan) |
                             (1ULL << destination);
    plan.progress.relocationsLiftedMask = 1U;
    plan.progress.relocationsPlacedMask = 1U;
    const bool humanSelection = plan.source == PhysicalTransitionSource::Human &&
        RequiredDestination(model.phase) == CHESS_INTERACTION_INVALID_SQUARE;
    // A selectable human placement uses the same confirmation owner as the
    // chess half. Fixed CPU/history destinations remain acknowledgements.
    model.phase.tag = humanSelection ? ChessInteractionPhase::Settling
                                    : ChessInteractionPhase::AwaitingAction;
    model.phase.correctionMask = 0ULL;
    model.phase.requiredAction = humanSelection ? RequiredPhysicalAction::None
        : RequiredPhysicalAction::AwaitAcknowledgement;
    model.phase.pendingActionToken = humanSelection ? 0U : model.nextActionToken++;
}

void ReduceDuckPlacement(ChessInteractionModel& model,
                         const uint64_t occupiedMask) {
    PhysicalTransitionPlan& plan = model.phase.plan;
    const uint64_t expectedLifted = LiftedOccupancy(plan);
    const uint64_t missing = expectedLifted & ~occupiedMask;
    const uint64_t placed = occupiedMask & ~expectedLifted;
    if (missing != 0ULL || (!IsSingleSquareMask(placed) && placed != 0ULL)) {
        RequireDuckCorrection(model, expectedLifted, occupiedMask);
        return;
    }
    if (placed == 0ULL) {
        model.phase.correctionMask = 0ULL;
        model.phase.requiredAction = RequiredPhysicalAction::PlacePrimary;
        return;
    }

    const uint8_t destination = SquareFromMask(placed);
    const uint8_t requiredDestination = RequiredDestination(model.phase);
    if (requiredDestination < 64U && destination != requiredDestination) {
        RequireDuckCorrection(model, expectedLifted, occupiedMask);
        return;
    }
    CompleteDuckPlacement(model, destination);
}

} // namespace

bool BeginDuckRelocationPlan(ChessInteractionModel& model,
                             const uint8_t fromSquare,
                             const uint8_t requiredDestination,
                             const PhysicalTransitionSource source,
                             const DirectedMoveOrigin directedOrigin) {
    const bool destinationSelectable =
        requiredDestination == CHESS_INTERACTION_INVALID_SQUARE;
    const uint64_t sourceMask = fromSquare < 64U
                                    ? (1ULL << fromSquare)
                                    : 0ULL;
    const uint64_t typedChessMask =
        model.acknowledged.kingMask |
        model.acknowledged.rookMask |
        model.acknowledged.pawnMask |
        model.acknowledged.moverOccupiedMask |
        model.acknowledged.opponentOccupiedMask;
    const bool idleBoardReady =
        model.phase.tag == ChessInteractionPhase::Idle &&
        (!model.board.valid ||
         model.board.occupiedMask == model.acknowledged.occupiedMask);
    const bool earlyHumanLift =
        source == PhysicalTransitionSource::Human &&
        IsAcknowledgedHumanChessBridge(model, sourceMask);
    if ((!idleBoardReady && !earlyHumanLift) ||
        fromSquare >= 64U ||
        (model.acknowledged.occupiedMask & sourceMask) == 0ULL ||
        (typedChessMask & sourceMask) != 0ULL ||
        (!destinationSelectable &&
         (requiredDestination >= 64U || requiredDestination == fromSquare ||
          (model.acknowledged.occupiedMask &
           (1ULL << requiredDestination)) != 0ULL))) {
        ++model.inputFaultEpoch;
        return false;
    }

    TaggedInteractionState phase;
    phase.tag = ChessInteractionPhase::DuckRelocation;
    // selectedSquare retains the fixed destination across correction and an
    // AwaitingAction regression. INVALID means the destination is selectable.
    phase.selectedSquare = requiredDestination;
    phase.requiredAction = RequiredPhysicalAction::LiftPrimary;
    phase.plan.kind = PhysicalTransitionKind::DuckRelocation;
    phase.plan.source = source;
    phase.plan.directedOrigin = directedOrigin;
    phase.plan.beforeOccupiedMask = model.acknowledged.occupiedMask;
    phase.plan.afterOccupiedMask = model.acknowledged.occupiedMask;
    phase.plan.moveFrom = fromSquare;
    phase.plan.moveTo = requiredDestination;
    phase.plan.relocationCount = 1U;
    phase.plan.relocations[0] = {fromSquare, requiredDestination};
    if (earlyHumanLift) {
        phase.plan.progress.relocationsLiftedMask = 1U;
        phase.requiredAction = RequiredPhysicalAction::PlacePrimary;
    }
    model.phase = phase;
    if (earlyHumanLift &&
        model.board.occupiedMask != LiftedOccupancy(phase.plan)) {
        const ChessInteractionEvent observation =
            ChessInteractionEvent::MakeBoardObservation(
                model.board.timestampMs, model.board.sequence,
                model.board.changedMask, model.board.occupiedMask);
        ApplyDuckRelocationBoardObservation(model, observation);
    }
    return true;
}

void ApplyDuckRelocationBoardObservation(
    ChessInteractionModel& model,
    const ChessInteractionEvent& event) {
    if (model.phase.plan.kind != PhysicalTransitionKind::DuckRelocation ||
        (model.phase.tag != ChessInteractionPhase::DuckRelocation &&
         model.phase.tag != ChessInteractionPhase::Settling &&
         model.phase.tag != ChessInteractionPhase::Correction &&
         model.phase.tag != ChessInteractionPhase::AwaitingAction)) {
        ++model.inputFaultEpoch;
        return;
    }

    if (model.phase.tag == ChessInteractionPhase::AwaitingAction &&
        model.phase.plan.source == PhysicalTransitionSource::Human &&
        model.phase.pendingActionToken != 0U &&
        RequiredDestination(model.phase) ==
            CHESS_INTERACTION_INVALID_SQUARE &&
        model.phase.plan.moveTo < 64U &&
        (event.occupiedMask & (1ULL << model.phase.plan.moveTo)) != 0ULL) {
        // A non-Duck observation can confirm a human Duck placement before
        // the coordinator has acknowledged the compound turn. The shared
        // action-result bridge retains this board snapshot and replays it
        // against the next side's authoritative position. Reinterpreting it
        // as more Duck placement progress here would instead create false
        // correction for a legitimate early next-side pickup.
        return;
    }

    if (model.phase.tag == ChessInteractionPhase::Correction) {
        PhysicalTransitionPlan& plan = model.phase.plan;
        if (event.occupiedMask == plan.afterOccupiedMask) {
            model.phase.tag = ChessInteractionPhase::Settling;
            model.phase.requiredAction = RequiredPhysicalAction::None;
            model.phase.correctionMask = 0ULL;
            ++model.feedbackEpoch;
            return;
        }
        const uint64_t sourceMask = 1ULL << plan.moveFrom;
        if ((event.occupiedMask & sourceMask) != 0ULL) {
            const uint64_t before = plan.beforeOccupiedMask;
            ResetDuckLift(model);
            if (event.occupiedMask != before) {
                RequireDuckCorrection(model, before, event.occupiedMask);
            }
            return;
        }
        if (event.occupiedMask == LiftedOccupancy(plan)) {
            ReopenDuckPlacement(model);
            ReduceDuckPlacement(model, event.occupiedMask);
            return;
        }
        model.phase.correctionMask =
            plan.afterOccupiedMask ^ event.occupiedMask;
        model.phase.requiredAction = RequiredPhysicalAction::CorrectBoard;
        ++model.feedbackEpoch;
        return;
    }

    if (model.phase.requiredAction == RequiredPhysicalAction::CorrectBoard &&
        model.phase.plan.source == PhysicalTransitionSource::Human &&
        RequiredDestination(model.phase) ==
            CHESS_INTERACTION_INVALID_SQUARE &&
        model.phase.plan.progress.relocationsLiftedMask == 0U &&
        model.phase.plan.progress.relocationsPlacedMask == 0U) {
        PhysicalTransitionPlan& plan = model.phase.plan;
        if (event.occupiedMask == plan.beforeOccupiedMask) {
            ResetDuckLift(model);
        } else {
            model.phase.correctionMask =
                plan.beforeOccupiedMask ^ event.occupiedMask;
            ++model.feedbackEpoch;
        }
        return;
    }

    if (model.phase.tag == ChessInteractionPhase::AwaitingAction ||
        model.phase.tag == ChessInteractionPhase::Settling) {
        if (event.occupiedMask == model.phase.plan.afterOccupiedMask) {
            return;
        }
        if (model.phase.tag == ChessInteractionPhase::Settling) {
            const uint64_t destinationMask =
                1ULL << model.phase.plan.moveTo;
            const uint64_t removed =
                model.phase.plan.afterOccupiedMask & ~event.occupiedMask;
            const uint64_t added =
                event.occupiedMask & ~model.phase.plan.afterOccupiedMask;
            const bool destinationOnlyRelift =
                removed == destinationMask && added == 0ULL;
            if (!destinationOnlyRelift) {
                RequireSettledDuckCorrection(model, event.occupiedMask);
                return;
            }
        }
        ReopenDuckPlacement(model);
    }

    PhysicalTransitionPlan& plan = model.phase.plan;
    const uint64_t sourceMask = 1ULL << plan.moveFrom;
    const bool sourceOccupied = (event.occupiedMask & sourceMask) != 0ULL;
    if (sourceOccupied) {
        if (event.occupiedMask == plan.beforeOccupiedMask) {
            if (plan.progress.relocationsLiftedMask != 0U) {
                ++model.feedbackEpoch;
            }
            ResetDuckLift(model);
        } else {
            RequireDuckCorrection(
                model, plan.beforeOccupiedMask, event.occupiedMask);
        }
        return;
    }

    plan.progress.relocationsLiftedMask = 1U;
    ReduceDuckPlacement(model, event.occupiedMask);
}

} // namespace runtime
