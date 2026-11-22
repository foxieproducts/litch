#include "chess_interaction_model_internal.hpp"

namespace runtime::chess_interaction_internal {

namespace {

bool TryInterruptIncompleteHumanEnPassant(
    ChessInteractionModel& model,
    const ChessInteractionEvent& event) {
    const PhysicalTransitionPlan plan = model.phase.plan;
    if (plan.source != PhysicalTransitionSource::Human ||
        plan.kind != PhysicalTransitionKind::EnPassant ||
        !plan.removalRequired || plan.removalSquare >= 64U ||
        plan.progress.removalComplete) {
        return false;
    }
    const uint64_t destinationMask = 1ULL << plan.moveTo;
    if (event.changedMask == destinationMask &&
        (event.occupiedMask & destinationMask) == 0ULL) {
        model.phase = TaggedInteractionState{};
        model.phase.tag = ChessInteractionPhase::PieceLifted;
        model.phase.selectedSquare = plan.moveFrom;
        return true;
    }
    const uint64_t captureMask = 1ULL << plan.removalSquare;
    if ((event.changedMask & ~captureMask) == 0ULL) {
        return false;
    }
    EnterPlanCorrection(
        model, event.occupiedMask ^ model.acknowledged.occupiedMask);
    return true;
}

} // namespace

void UpdatePlanProgress(PhysicalTransitionPlan& plan,
                        const uint64_t occupiedMask) {
    if (plan.removalRequired && plan.removalSquare < 64U) {
        bool removalSharesDestination = false;
        bool matchingRelocationSourceAbsent = false;
        for (uint8_t i = 0U; i < plan.relocationCount; ++i) {
            if (plan.removalSquare == plan.relocations[i].to) {
                removalSharesDestination = true;
                matchingRelocationSourceAbsent =
                    (occupiedMask &
                     (1ULL << plan.relocations[i].from)) == 0ULL;
            }
        }
        const bool removalAbsent =
            (occupiedMask & (1ULL << plan.removalSquare)) == 0ULL;
        if (removalSharesDestination) {
            plan.progress.removalComplete = removalAbsent ||
                (plan.progress.removalComplete &&
                 matchingRelocationSourceAbsent);
        } else {
            plan.progress.removalComplete = removalAbsent;
        }
    }
    if (plan.restorationRequired && plan.restorationSquare < 64U) {
        const bool restorationOccupied =
            (occupiedMask & (1ULL << plan.restorationSquare)) != 0ULL;
        bool sharesRelocationSource = false;
        bool sharedSourceWasLifted = false;
        for (uint8_t i = 0U; i < plan.relocationCount; ++i) {
            if (plan.restorationSquare == plan.relocations[i].from) {
                sharesRelocationSource = true;
                sharedSourceWasLifted =
                    (plan.progress.relocationsLiftedMask &
                     static_cast<uint8_t>(1U << i)) != 0U;
            }
        }
        plan.progress.restorationComplete = restorationOccupied &&
            (!sharesRelocationSource || sharedSourceWasLifted);
    }
    for (uint8_t i = 0; i < plan.relocationCount; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1U << i);
        const PieceRelocation& relocation = plan.relocations[i];
        const bool sourceAbsent =
            (occupiedMask & (1ULL << relocation.from)) == 0ULL;
        const bool restoredPieceSharesSource =
            plan.source == PhysicalTransitionSource::Undo &&
            plan.restorationRequired &&
            plan.progress.restorationComplete &&
            plan.restorationSquare == relocation.from;
        if (sourceAbsent || restoredPieceSharesSource) {
            plan.progress.relocationsLiftedMask |= bit;
        } else {
            plan.progress.relocationsLiftedMask &=
                static_cast<uint8_t>(~bit);
        }
        const bool destinationCapturePending =
            plan.removalRequired && relocation.to == plan.removalSquare &&
            !plan.progress.removalComplete;
        if ((occupiedMask & (1ULL << relocation.to)) != 0ULL &&
            !destinationCapturePending) {
            plan.progress.relocationsPlacedMask |= bit;
        } else {
            plan.progress.relocationsPlacedMask &= static_cast<uint8_t>(~bit);
        }
    }
}

bool ReopenHumanPlanForCurrentOccupancy(
    ChessInteractionModel& model,
    const uint64_t occupiedMask) {
    if (model.phase.plan.source != PhysicalTransitionSource::Human ||
        model.phase.plan.kind == PhysicalTransitionKind::None ||
        occupiedMask == model.phase.plan.afterOccupiedMask) {
        return false;
    }

    UpdatePlanProgress(model.phase.plan, occupiedMask);
    if (occupiedMask == model.phase.plan.beforeOccupiedMask) {
        ClearPhase(model.phase);
        return true;
    }
    if (model.phase.plan.relocationCount == 1U &&
        !model.phase.plan.removalRequired) {
        const uint8_t source = model.phase.plan.moveFrom;
        ClearPhase(model.phase);
        model.phase.tag = ChessInteractionPhase::PieceLifted;
        model.phase.selectedSquare = source;
        return true;
    }
    const RequiredPhysicalAction next = GetNextPlanAction(model.phase.plan);
    if (next == RequiredPhysicalAction::None) {
        EnterPlanCorrection(
            model, occupiedMask ^ model.acknowledged.occupiedMask);
        return true;
    }
    model.phase.tag = ChessInteractionPhase::ExecutingPlan;
    model.phase.requiredAction = next;
    model.phase.pendingActionToken = 0U;
    return true;
}

RequiredPhysicalAction GetNextPlanAction(
    const PhysicalTransitionPlan& plan) {
    if ((plan.progress.relocationsLiftedMask & 0x01U) == 0U) {
        return RequiredPhysicalAction::LiftPrimary;
    }
    if (plan.removalRequired && !plan.progress.removalComplete) {
        return RequiredPhysicalAction::RemoveCapture;
    }
    if ((plan.progress.relocationsPlacedMask & 0x01U) == 0U) {
        return RequiredPhysicalAction::PlacePrimary;
    }
    if (plan.relocationCount > 1U &&
        (plan.progress.relocationsLiftedMask & 0x02U) == 0U) {
        return RequiredPhysicalAction::LiftSecondary;
    }
    if (plan.relocationCount > 1U &&
        (plan.progress.relocationsPlacedMask & 0x02U) == 0U) {
        return RequiredPhysicalAction::PlaceSecondary;
    }
    if (plan.restorationRequired && !plan.progress.restorationComplete) {
        return RequiredPhysicalAction::RestoreCapture;
    }
    return RequiredPhysicalAction::None;
}

void ReducePlanObservation(ChessInteractionModel& model,
                           const ChessInteractionEvent& event) {
    const bool human = model.phase.plan.source ==
                       PhysicalTransitionSource::Human;
    if (human &&
        (event.changedMask & ~GetLegalProgressPhysicalMask(model.phase)) !=
            0ULL) {
        EnterPlanCorrection(
            model, event.occupiedMask ^ model.acknowledged.occupiedMask);
        return;
    }
    if (human && !model.phase.plan.progress.actionAcknowledged &&
        event.occupiedMask == model.phase.plan.beforeOccupiedMask) {
        ClearPhase(model.phase);
        return;
    }
    if (TryInterruptIncompleteHumanEnPassant(model, event)) {
        return;
    }
    UpdatePlanProgress(model.phase.plan, event.occupiedMask);
    const RequiredPhysicalAction next = GetNextPlanAction(model.phase.plan);
    if (event.occupiedMask == model.phase.plan.afterOccupiedMask &&
        next == RequiredPhysicalAction::None) {
        if (model.phase.plan.progress.actionAcknowledged) {
            ClearPhase(model.phase);
        } else {
            EnterPlanComplete(model);
        }
        return;
    }
    if (!model.phase.plan.progress.actionAcknowledged &&
        next == RequiredPhysicalAction::None) {
        EnterPlanCorrection(
            model, event.occupiedMask ^ model.phase.plan.afterOccupiedMask);
        return;
    }
    model.phase.requiredAction = next;
}

void EnterPlanCorrection(ChessInteractionModel& model,
                         const uint64_t correctionMask) {
    if (correctionMask == 0ULL) {
        ClearPhase(model.phase);
        return;
    }
    model.phase.tag = ChessInteractionPhase::Correction;
    model.phase.correctionMask = correctionMask;
    model.phase.requiredAction = RequiredPhysicalAction::CorrectBoard;
    model.phase.pendingActionToken = 0U;
    ++model.feedbackEpoch;
}

} // namespace runtime::chess_interaction_internal
