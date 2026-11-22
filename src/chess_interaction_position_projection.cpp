#include "runtime/chess_interaction_model.hpp"

namespace runtime {

namespace {

uint64_t PlanControlledPhysicalMask(
    const PhysicalTransitionPlan& plan) {
    uint64_t mask = 0ULL;
    for (uint8_t i = 0; i < plan.relocationCount; ++i) {
        const PieceRelocation& relocation = plan.relocations[i];
        if (relocation.from < 64U) {
            mask |= (1ULL << relocation.from);
        }
        if (relocation.to < 64U) {
            mask |= (1ULL << relocation.to);
        }
    }
    if (plan.removalRequired && plan.removalSquare < 64U) {
        mask |= (1ULL << plan.removalSquare);
    }
    if (plan.restorationRequired && plan.restorationSquare < 64U) {
        mask |= (1ULL << plan.restorationSquare);
    }
    return mask;
}

} // namespace

uint64_t GetLegalProgressPhysicalMask(
    const TaggedInteractionState& state) {
    switch (state.tag) {
        case ChessInteractionPhase::PieceLifted:
        case ChessInteractionPhase::CaptureStaged:
            return state.selectedSquare < 64U
                       ? (1ULL << state.selectedSquare)
                       : 0ULL;
        case ChessInteractionPhase::ExecutingPlan:
        case ChessInteractionPhase::AwaitingAction:
        case ChessInteractionPhase::Settling:
        case ChessInteractionPhase::DuckRelocation:
            return PlanControlledPhysicalMask(state.plan);
        case ChessInteractionPhase::Idle:
        case ChessInteractionPhase::Correction:
        default:
            return 0ULL;
    }
}

uint64_t ProjectPhysicalOccupancyForInteractionProgress(
    const ChessPositionState& acknowledged,
    const TaggedInteractionState& state) {
    if (state.tag == ChessInteractionPhase::PieceLifted ||
        state.tag == ChessInteractionPhase::CaptureStaged) {
        return state.selectedSquare < 64U
                   ? acknowledged.occupiedMask &
                         ~(1ULL << state.selectedSquare)
                   : acknowledged.occupiedMask;
    }
    if (state.tag == ChessInteractionPhase::Correction) {
        return acknowledged.occupiedMask ^ state.correctionMask;
    }
    if (state.tag == ChessInteractionPhase::Idle) {
        return acknowledged.occupiedMask;
    }

    uint64_t occupied = state.plan.beforeOccupiedMask;
    for (uint8_t i = 0U; i < state.plan.relocationCount; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1U << i);
        if ((state.plan.progress.relocationsLiftedMask & bit) != 0U &&
            state.plan.relocations[i].from < 64U) {
            occupied &= ~(1ULL << state.plan.relocations[i].from);
        }
    }
    if (state.plan.removalRequired &&
        state.plan.progress.removalComplete) {
        occupied &= ~(1ULL << state.plan.removalSquare);
    }
    if (state.plan.restorationRequired &&
        state.plan.progress.restorationComplete) {
        occupied |= 1ULL << state.plan.restorationSquare;
    }
    for (uint8_t i = 0U; i < state.plan.relocationCount; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1U << i);
        if ((state.plan.progress.relocationsPlacedMask & bit) != 0U &&
            state.plan.relocations[i].to < 64U) {
            occupied |= 1ULL << state.plan.relocations[i].to;
        }
    }
    return occupied;
}

ChessPositionState ProjectChessPositionAfterPlan(
    const ChessPositionState& acknowledged,
    const PhysicalTransitionPlan& plan) {
    ChessPositionState projected = acknowledged;
    if (plan.kind == PhysicalTransitionKind::DuckRelocation) {
        projected.occupiedMask = plan.afterOccupiedMask;
        return projected;
    }
    const bool undo = plan.source == PhysicalTransitionSource::Undo;
    uint64_t movedSideMask = undo ? acknowledged.opponentOccupiedMask
                                  : acknowledged.moverOccupiedMask;
    uint64_t waitingSideMask = undo ? acknowledged.moverOccupiedMask
                                    : acknowledged.opponentOccupiedMask;
    projected.occupiedMask = plan.afterOccupiedMask;

    for (uint8_t i = 0; i < plan.relocationCount; ++i) {
        const PieceRelocation& relocation = plan.relocations[i];
        const uint64_t fromMask = (1ULL << relocation.from);
        const uint64_t toMask = (1ULL << relocation.to);
        if ((projected.kingMask & fromMask) != 0ULL) {
            projected.kingMask = (projected.kingMask & ~fromMask) | toMask;
        }
        if ((projected.rookMask & fromMask) != 0ULL) {
            projected.rookMask = (projected.rookMask & ~fromMask) | toMask;
        }
        if ((projected.pawnMask & fromMask) != 0ULL) {
            projected.pawnMask &= ~fromMask;
            if (!plan.promotionRequired) {
                projected.pawnMask |= toMask;
            }
        }
        if ((movedSideMask & fromMask) != 0ULL) {
            movedSideMask = (movedSideMask & ~fromMask) | toMask;
        }
        waitingSideMask &= ~toMask;
    }

    if (plan.removalRequired) {
        const uint64_t removalMask = (1ULL << plan.removalSquare);
        waitingSideMask &= ~removalMask;
        projected.kingMask &= ~removalMask;
        projected.rookMask &= ~removalMask;
        projected.pawnMask &= ~removalMask;
    }
    if (plan.restorationRequired) {
        const uint64_t restoreMask = (1ULL << plan.restorationSquare);
        waitingSideMask |= restoreMask;
        if (plan.restorationPieceFen == 'P' ||
            plan.restorationPieceFen == 'p') {
            projected.pawnMask |= restoreMask;
        } else if (plan.restorationPieceFen == 'R' ||
                   plan.restorationPieceFen == 'r') {
            projected.rookMask |= restoreMask;
        }
    }

    projected.moverOccupiedMask = undo ? movedSideMask : waitingSideMask;
    projected.opponentOccupiedMask = undo ? waitingSideMask : movedSideMask;
    projected.enPassantTargetSquare = CHESS_INTERACTION_INVALID_SQUARE;
    projected.enPassantCaptureSquare = CHESS_INTERACTION_INVALID_SQUARE;
    projected.legalCaptureTargetsValid = false;
    projected.legalCaptureTargetsMask = 0ULL;
    projected.legalCastlingTargetsValid = false;
    projected.legalCastlingKingTargetsMask = 0ULL;
    return projected;
}

} // namespace runtime
