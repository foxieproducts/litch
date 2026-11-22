#include "runtime/chess_interaction_model.hpp"

#include "chess_interaction_capture.hpp"
#include "chess_interaction_model_internal.hpp"
#include "runtime/chess_castling_eligibility.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"

namespace runtime {

void EnterChessInteractionCorrection(ChessInteractionModel& model,
                                     const uint64_t correctionMask) {
    if (correctionMask == 0ULL) {
        model.phase = TaggedInteractionState{};
        return;
    }
    model.phase = TaggedInteractionState{};
    model.phase.tag = ChessInteractionPhase::Correction;
    model.phase.correctionMask = correctionMask;
    model.phase.requiredAction = RequiredPhysicalAction::CorrectBoard;
    ++model.feedbackEpoch;
}

namespace chess_interaction_internal {
namespace {

bool IsEnPassantLanding(const ChessPositionState& position,
                       const ChessInteractionEvent& event,
                       const uint8_t from) {
    return chess_interaction_capture::IsEnPassantSource(position, from) &&
           (event.changedMask & event.occupiedMask &
            (1ULL << position.enPassantTargetSquare)) != 0ULL;
}

bool RejectMixedEnPassantLanding(ChessInteractionModel& model,
                                const ChessInteractionEvent& event,
                                const uint8_t from) {
    if (!IsEnPassantLanding(model.acknowledged, event, from)) {
        return false;
    }
    const uint64_t moveMask = (1ULL << from) |
        (1ULL << model.acknowledged.enPassantTargetSquare) |
        (1ULL << model.acknowledged.enPassantCaptureSquare);
    if ((event.changedMask & ~moveMask) == 0ULL) {
        return false;
    }
    // The first EP landing has the same strict choreography as an already
    // active EP plan. Another pickup cannot stand in for its captured pawn.
    EnterChessInteractionCorrection(
        model, event.occupiedMask ^ model.acknowledged.occupiedMask);
    return true;
}

} // namespace

void ClearPhase(TaggedInteractionState& phase) { phase = TaggedInteractionState{}; }
bool IsExpectedCastlePair(const ChessInteractionModel& model,
                          const litch_max::CastlingPattern& pattern) {
    return IsCastlingPatternEligible(model.acknowledged, pattern);
}
const litch_max::CastlingPattern* FindCastlePair(const ChessInteractionModel& model,
                                     const uint8_t first, const uint8_t second) {
    for (const auto& pattern : litch_max::CHESS_CASTLING_PATTERNS) {
        const bool matches = (first == pattern.kingFrom && second == pattern.rookFrom) ||
                             (first == pattern.rookFrom && second == pattern.kingFrom);
        if (matches && IsExpectedCastlePair(model, pattern)) {
            return &pattern;
        }
    }
    return nullptr;
}
const litch_max::CastlingPattern* FindAtomicCastlePair(const ChessInteractionModel& model,
                                            const uint64_t removedMask) {
    for (const auto& pattern : litch_max::CHESS_CASTLING_PATTERNS) {
        const uint64_t pairMask = (1ULL << pattern.kingFrom) |
                                  (1ULL << pattern.rookFrom);
        if (removedMask == pairMask &&
            IsExpectedCastlePair(model, pattern)) {
            return &pattern;
        }
    }
    return nullptr;
}

void BeginCastlePlan(ChessInteractionModel& model, const litch_max::CastlingPattern& pattern) {
    TaggedInteractionState phase;
    phase.tag = ChessInteractionPhase::ExecutingPlan;
    phase.requiredAction = RequiredPhysicalAction::PlacePrimary;
    phase.plan.kind = PhysicalTransitionKind::Castling;
    phase.plan.beforeOccupiedMask = model.acknowledged.occupiedMask;
    phase.plan.afterOccupiedMask = model.acknowledged.occupiedMask;
    phase.plan.afterOccupiedMask &= ~(1ULL << pattern.kingFrom);
    phase.plan.afterOccupiedMask &= ~(1ULL << pattern.rookFrom);
    phase.plan.afterOccupiedMask |= (1ULL << pattern.kingTo);
    phase.plan.afterOccupiedMask |= (1ULL << pattern.rookTo);
    phase.plan.moveFrom = pattern.kingFrom;
    phase.plan.moveTo = pattern.kingTo;
    phase.plan.relocationCount = 2;
    phase.plan.relocations[0] = {pattern.kingFrom, pattern.kingTo};
    phase.plan.relocations[1] = {pattern.rookFrom, pattern.rookTo};
    model.phase = phase;
}

void BeginQuietPlan(ChessInteractionModel& model, const uint8_t from, const uint8_t to) {
    TaggedInteractionState phase;
    phase.tag = ChessInteractionPhase::ExecutingPlan;
    phase.requiredAction = RequiredPhysicalAction::PlacePrimary;
    const bool pawnMove = (model.acknowledged.pawnMask & (1ULL << from)) != 0ULL;
    const uint8_t toRank = static_cast<uint8_t>(to / 8U);
    const bool promotion = pawnMove && (toRank == 0U || toRank == 7U);
    phase.plan.kind = promotion ? PhysicalTransitionKind::Promotion
                                : PhysicalTransitionKind::QuietMove;
    phase.plan.promotionRequired = promotion;
    phase.plan.beforeOccupiedMask = model.acknowledged.occupiedMask;
    phase.plan.afterOccupiedMask =
        (model.acknowledged.occupiedMask & ~(1ULL << from)) |
        (1ULL << to);
    phase.plan.moveFrom = from;
    phase.plan.moveTo = to;
    phase.plan.relocationCount = 1;
    phase.plan.relocations[0] = {from, to};
    model.phase = phase;
}

void EnterAwaitingAction(ChessInteractionModel& model) {
    model.phase.tag = ChessInteractionPhase::AwaitingAction;
    model.phase.requiredAction = RequiredPhysicalAction::AwaitAcknowledgement;
    model.phase.pendingActionToken = model.nextActionToken++;
}

void EnterPlanComplete(ChessInteractionModel& model) {
    const PhysicalTransitionPlan& plan = model.phase.plan;
    const uint64_t duckMask = GetKnownNeutralDuckMask(model.acknowledged);
    const bool holdingDuck = duckMask != 0ULL &&
                             (model.board.occupiedMask & duckMask) == 0ULL;
    if (plan.source == PhysicalTransitionSource::Human &&
        (plan.kind != PhysicalTransitionKind::Castling || holdingDuck)) {
        model.phase.tag = ChessInteractionPhase::Settling;
        model.phase.requiredAction = RequiredPhysicalAction::None;
        model.phase.pendingActionToken = 0U;
        return;
    }
    if (IsCpuDirectedTransition(plan)) {
        model.phase.tag = ChessInteractionPhase::ExecutingPlan;
        model.phase.requiredAction = RequiredPhysicalAction::None;
        model.phase.pendingActionToken = 0U;
        return;
    }
    EnterAwaitingAction(model);
}

bool IsSingleSquareMask(const uint64_t mask) {
    return mask != 0ULL && (mask & (mask - 1ULL)) == 0ULL;
}

uint8_t SquareFromSingleMask(const uint64_t mask) {
    for (uint8_t square = 0; square < 64U; ++square) {
        if ((mask & (1ULL << square)) != 0ULL) {
            return square;
        }
    }
    return CHESS_INTERACTION_INVALID_SQUARE;
}

bool IsMoverSource(const ChessInteractionModel& model,
                   const uint64_t sourceMask) {
    const bool sideModelAvailable =
        (model.acknowledged.moverOccupiedMask |
         model.acknowledged.opponentOccupiedMask) != 0ULL;
    return !sideModelAvailable ||
           (model.acknowledged.moverOccupiedMask & sourceMask) != 0ULL;
}

bool TryBeginQuietPlan(ChessInteractionModel& model,
                       const uint64_t placedMask,
                       const uint64_t occupiedMask,
                       const uint64_t replayableFollowupMask = 0ULL) {
    if (!IsSingleSquareMask(placedMask)) {
        return false;
    }
    const uint8_t to = SquareFromSingleMask(placedMask);
    if ((model.acknowledged.occupiedMask & placedMask) != 0ULL ||
        model.phase.selectedSquare >= 64U) {
        return false;
    }
    BeginQuietPlan(model, model.phase.selectedSquare, to);
    UpdatePlanProgress(model.phase.plan, occupiedMask);
    const uint64_t positionDifference =
        occupiedMask ^ model.phase.plan.afterOccupiedMask;
    if (positionDifference == 0ULL ||
        positionDifference == replayableFollowupMask) {
        EnterPlanComplete(model);
    } else {
        EnterPlanCorrection(
            model, occupiedMask ^ model.phase.plan.afterOccupiedMask);
    }
    return true;
}

bool TryBeginCastleFromKingPlacement(ChessInteractionModel& model,
                                     const uint64_t placedMask,
                                     const uint64_t occupiedMask) {
    for (const auto& pattern : litch_max::CHESS_CASTLING_PATTERNS) {
        if (model.phase.selectedSquare != pattern.kingFrom ||
            (placedMask & (1ULL << pattern.kingTo)) == 0ULL ||
            !IsExpectedCastlePair(model, pattern)) {
            continue;
        }

        BeginCastlePlan(model, pattern);
        UpdatePlanProgress(model.phase.plan, occupiedMask);
        model.phase.requiredAction = GetNextPlanAction(model.phase.plan);
        if (occupiedMask == model.phase.plan.afterOccupiedMask) {
            EnterPlanComplete(model);
        }
        return true;
    }
    return false;
}

void ReduceIdleObservation(ChessInteractionModel& model,
                           const ChessInteractionEvent& event) {
    if ((GetKnownNeutralDuckMask(model.acknowledged) &
         ~event.occupiedMask) != 0ULL) {
        EnterChessInteractionCorrection(
            model, event.occupiedMask ^ model.acknowledged.occupiedMask);
        return;
    }
    const uint64_t removedMask = event.changedMask & ~event.occupiedMask;
    const uint64_t moverRemoved = removedMask & model.acknowledged.moverOccupiedMask;
    if (IsSingleSquareMask(moverRemoved) &&
        RejectMixedEnPassantLanding(
            model, event, SquareFromSingleMask(moverRemoved))) {
        return;
    }
    if (chess_interaction_capture::ReduceIdleObservation(
            model, event)) {
        return;
    }
    const litch_max::CastlingPattern* atomicCastle = FindAtomicCastlePair(model, removedMask);
    if (atomicCastle != nullptr) {
        BeginCastlePlan(model, *atomicCastle);
        UpdatePlanProgress(model.phase.plan, event.occupiedMask);
        model.phase.requiredAction = GetNextPlanAction(model.phase.plan);
        if (event.occupiedMask == model.phase.plan.afterOccupiedMask) {
            EnterPlanComplete(model);
        }
        return;
    }

    if (!IsSingleSquareMask(removedMask)) {
        if (removedMask != 0ULL) {
            EnterChessInteractionCorrection(
                model,
                event.occupiedMask ^ model.acknowledged.occupiedMask);
        }
        return;
    }
    if (!IsMoverSource(model, removedMask)) {
        return;
    }

    model.phase.tag = ChessInteractionPhase::PieceLifted;
    model.phase.selectedSquare = SquareFromSingleMask(removedMask);
    if (chess_interaction_capture::ReducePieceLiftedObservation(
            model, event)) {
        return;
    }
    const uint64_t placedMask = event.changedMask & event.occupiedMask;
    if (TryBeginCastleFromKingPlacement(
            model, placedMask, event.occupiedMask)) {
        return;
    }
    if (TryBeginQuietPlan(model, placedMask, event.occupiedMask)) {
        return;
    }
}

void ReplayIdleAfterConsumedChanges(ChessInteractionModel& model,
                                    const ChessInteractionEvent& event,
                                    const uint64_t consumedMask) {
    ClearPhase(model.phase);
    const uint64_t remaining = event.changedMask & ~consumedMask;
    const uint64_t physical = model.board.occupiedMask;
    if (remaining == 0ULL &&
        (GetKnownNeutralDuckMask(model.acknowledged) & ~physical) == 0ULL) {
        return;
    }
    ChessInteractionEvent replay = event;
    // A new mover must establish ownership while the Duck is on the board.
    // The preceding piece's chess-only projection cannot grant that ownership.
    replay.occupiedMask = physical;
    replay.changedMask = remaining;
    ReduceIdleObservation(model, replay);
    if (model.phase.tag == ChessInteractionPhase::Idle) {
        const uint64_t correction =
            physical ^ model.acknowledged.occupiedMask;
        if (correction != 0ULL) {
            EnterChessInteractionCorrection(model, correction);
        }
    }
}

void ReducePieceLiftedObservation(ChessInteractionModel& model,
                                  const ChessInteractionEvent& event) {
    if (RejectMixedEnPassantLanding(model, event, model.phase.selectedSquare)) {
        return;
    }
    const uint64_t placedMask = event.changedMask & event.occupiedMask;
    if (model.phase.selectedSquare < 64U &&
        (placedMask & (1ULL << model.phase.selectedSquare)) != 0ULL) {
        // Returning the selected piece retires its ownership before a fresh
        // victim removal or pickup in this observation is interpreted.
        ReplayIdleAfterConsumedChanges(
            model, event, 1ULL << model.phase.selectedSquare);
        return;
    }
    const uint64_t removedMask = event.changedMask & ~event.occupiedMask;
    const uint64_t opponentRemoved =
        removedMask & model.acknowledged.opponentOccupiedMask;
    const bool enPassantLanding =
        IsEnPassantLanding(model.acknowledged, event, model.phase.selectedSquare);
    const bool placedMoveAndNextPickup =
        IsSingleSquareMask(placedMask) &&
        IsSingleSquareMask(opponentRemoved) &&
        placedMask != opponentRemoved && !enPassantLanding;
    if (placedMoveAndNextPickup &&
        TryBeginQuietPlan(
            model, placedMask, event.occupiedMask, opponentRemoved)) {
        return;
    }
    if (chess_interaction_capture::ReducePieceLiftedObservation(
            model, event)) {
        return;
    }
    for (uint8_t second = 0; second < 64U; ++second) {
        if ((removedMask & (1ULL << second)) == 0ULL) {
            continue;
        }
        const litch_max::CastlingPattern* pattern = FindCastlePair(
            model,
            model.phase.selectedSquare,
            second);
        if (pattern == nullptr) {
            continue;
        }
        BeginCastlePlan(model, *pattern);
        UpdatePlanProgress(model.phase.plan, event.occupiedMask);
        if (event.occupiedMask == model.phase.plan.afterOccupiedMask) {
            EnterPlanComplete(model);
        } else {
            model.phase.requiredAction =
                GetNextPlanAction(model.phase.plan);
        }
        return;
    }

    if (TryBeginCastleFromKingPlacement(
            model, placedMask, event.occupiedMask)) {
        return;
    }

    (void)TryBeginQuietPlan(model, placedMask, event.occupiedMask);
}

bool TryReduceSettlingProgressRegression(
    ChessInteractionModel& model,
    const ChessInteractionEvent& event) {
    const PhysicalTransitionPlan plan = model.phase.plan;
    const uint64_t removedMask = event.changedMask & ~event.occupiedMask;
    uint64_t destinationMask = 0ULL;
    for (uint8_t i = 0U; i < plan.relocationCount; ++i) {
        if (plan.relocations[i].to < 64U) {
            destinationMask |= 1ULL << plan.relocations[i].to;
        }
    }
    const bool destinationOnlyRelift =
        IsSingleSquareMask(event.changedMask) &&
        event.changedMask == removedMask &&
        (removedMask & destinationMask) != 0ULL;
    const uint64_t captureMask =
        plan.removalSquare < 64U ? (1ULL << plan.removalSquare) : 0ULL;
    const bool enPassantCaptureRestored =
        plan.kind == PhysicalTransitionKind::EnPassant &&
        plan.removalRequired && plan.progress.removalComplete &&
        event.changedMask == captureMask &&
        (event.occupiedMask & captureMask) != 0ULL;
    const bool captureFullyRestored =
        plan.source == PhysicalTransitionSource::Human &&
        !plan.progress.actionAcknowledged && plan.removalRequired &&
        plan.removalSquare == plan.moveTo &&
        event.occupiedMask == plan.beforeOccupiedMask;
    if (destinationOnlyRelift || enPassantCaptureRestored || captureFullyRestored) {
        (void)ReopenHumanPlanForCurrentOccupancy(
            model, event.occupiedMask);
        return true;
    }

    const bool incompleteEnPassant =
        plan.source == PhysicalTransitionSource::Human &&
        plan.kind == PhysicalTransitionKind::EnPassant &&
        plan.removalRequired && !plan.progress.removalComplete;
    const bool requiredCaptureOnly =
        incompleteEnPassant && event.changedMask == captureMask &&
        (event.occupiedMask & captureMask) == 0ULL;
    if (incompleteEnPassant && !requiredCaptureOnly) {
        EnterPlanCorrection(
            model,
            event.occupiedMask ^ model.acknowledged.occupiedMask);
        return true;
    }

    if ((removedMask & destinationMask) == 0ULL) {
        return false;
    }

    EnterPlanCorrection(
        model,
        event.occupiedMask ^ model.acknowledged.occupiedMask);
    return true;
}

} // namespace chess_interaction_internal

using chess_interaction_internal::EnterAwaitingAction;
using chess_interaction_internal::EnterPlanComplete;
using chess_interaction_internal::GetNextPlanAction;
using chess_interaction_internal::UpdatePlanProgress;

void ApplyChessInteractionPlanRequested(
    ChessInteractionModel& model,
    const ChessInteractionEvent& event) {
    const bool replacesPendingPlan =
        event.requestedPlan.source == PhysicalTransitionSource::Undo &&
        ((model.phase.tag == ChessInteractionPhase::ExecutingPlan &&
          IsCpuDirectedTransition(model.phase.plan)) ||
         (model.phase.tag == ChessInteractionPhase::AwaitingAction &&
          model.phase.plan.source == PhysicalTransitionSource::Human &&
          model.phase.plan.kind == PhysicalTransitionKind::Promotion));
    if ((!replacesPendingPlan && model.phase.tag != ChessInteractionPhase::Idle) ||
        event.requestedPlan.kind == PhysicalTransitionKind::None ||
        event.requestedPlan.relocationCount == 0U) {
        ++model.inputFaultEpoch;
        return;
    }
    model.phase = TaggedInteractionState{};
    model.phase.tag = ChessInteractionPhase::ExecutingPlan;
    model.phase.plan = event.requestedPlan;
    // A newly requested engine/undo plan starts from the acknowledged logical
    // position. Durable restore supplies explicit progress separately; stale
    // prior observations must not auto-complete a new plan.
    const uint64_t occupied = model.acknowledged.occupiedMask;
    UpdatePlanProgress(model.phase.plan, occupied);
    model.phase.requiredAction = GetNextPlanAction(model.phase.plan);
    if (event.requestedPlan.source == PhysicalTransitionSource::Undo) {
        EnterAwaitingAction(model);
    } else if (occupied == model.phase.plan.afterOccupiedMask) {
        EnterPlanComplete(model);
    }
}

} // namespace runtime
