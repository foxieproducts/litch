#include "runtime/chess_interaction_model.hpp"

namespace runtime {
namespace {

uint64_t AddByte(uint64_t hash, const uint8_t value) {
    return (hash ^ value) * 1099511628211ULL;
}

uint64_t AddU64(uint64_t hash, const uint64_t value) {
    for (uint8_t shift = 0; shift < 64U; shift += 8U) {
        hash = AddByte(hash, static_cast<uint8_t>(value >> shift));
    }
    return hash;
}

uint64_t AddPlan(uint64_t hash, const PhysicalTransitionPlan& plan) {
    hash = AddByte(hash, static_cast<uint8_t>(plan.kind));
    hash = AddByte(hash, static_cast<uint8_t>(plan.source));
    // Trace schema v1 hashed the retired pickup-order field here. Preserve the
    // byte layout by deriving its only non-default value from authoritative
    // plan identity rather than retaining compatibility state.
    const uint8_t legacyPickupOrder =
        plan.source == PhysicalTransitionSource::Human &&
                plan.kind == PhysicalTransitionKind::Castling
            ? 2U
            : 0U;
    hash = AddByte(hash, legacyPickupOrder);
    hash = AddU64(hash, plan.beforeOccupiedMask);
    hash = AddU64(hash, plan.afterOccupiedMask);
    hash = AddByte(hash, plan.moveFrom);
    hash = AddByte(hash, plan.moveTo);
    hash = AddByte(hash, plan.relocationCount);
    for (const auto& relocation : plan.relocations) {
        hash = AddByte(hash, relocation.from);
        hash = AddByte(hash, relocation.to);
    }
    hash = AddByte(hash, plan.removalRequired ? 1U : 0U);
    hash = AddByte(hash, plan.removalSquare);
    hash = AddByte(hash, plan.promotionRequired ? 1U : 0U);
    hash = AddByte(hash, plan.restorationRequired ? 1U : 0U);
    hash = AddByte(hash, plan.restorationSquare);
    hash = AddByte(hash, static_cast<uint8_t>(plan.restorationPieceFen));
    hash = AddByte(hash, plan.progress.relocationsLiftedMask);
    hash = AddByte(hash, plan.progress.relocationsPlacedMask);
    hash = AddByte(hash, plan.progress.removalComplete ? 1U : 0U);
    hash = AddByte(hash, plan.progress.restorationComplete ? 1U : 0U);
    return AddByte(hash, plan.progress.actionAcknowledged ? 1U : 0U);
}

} // namespace

uint64_t ComputeChessInteractionModelHash(const ChessInteractionModel& model) {
    uint64_t hash = 1469598103934665603ULL;
    hash = AddU64(hash, model.acknowledged.occupiedMask);
    hash = AddU64(hash, model.acknowledged.kingMask);
    hash = AddU64(hash, model.acknowledged.rookMask);
    hash = AddU64(hash, model.acknowledged.pawnMask);
    hash = AddU64(hash, model.acknowledged.moverOccupiedMask);
    hash = AddU64(hash, model.acknowledged.opponentOccupiedMask);
    hash = AddByte(hash, model.acknowledged.enPassantTargetSquare);
    hash = AddByte(hash, model.acknowledged.enPassantCaptureSquare);
    hash = AddByte(hash, model.acknowledged.sideToMoveValid ? 1U : 0U);
    hash = AddByte(hash, model.acknowledged.sideToMoveBlack ? 1U : 0U);
    hash = AddByte(hash,
                   model.acknowledged.legalCaptureTargetsValid ? 1U : 0U);
    hash = AddU64(hash, model.acknowledged.legalCaptureTargetsMask);
    // Old traces predate engine-derived castling eligibility. Preserve their
    // exact hash stream when the optional extension is absent/invalid.
    if (model.acknowledged.legalCastlingTargetsValid) {
        hash = AddByte(hash, 0xC4U);
        hash = AddU64(
            hash,
            model.acknowledged.legalCastlingKingTargetsMask);
    }
    const auto& capture = model.acknowledgedCaptureLegality;
    if (capture.knownSources != 0ULL) {
        hash = AddByte(hash, 0xC5U);
        hash = AddU64(hash, capture.knownSources);
        for (uint8_t source = 0U; source < 64U; ++source) {
            if ((capture.knownSources & (1ULL << source)) != 0ULL) {
                hash = AddU64(hash, capture.landingsBySource[source]);
            }
        }
    }
    hash = AddU64(hash, model.acknowledged.revision);
    hash = AddByte(hash, model.board.valid ? 1U : 0U);
    hash = AddU64(hash, model.board.changedMask);
    hash = AddU64(hash, model.board.occupiedMask);
    hash = AddU64(hash, model.board.timestampMs);
    hash = AddU64(hash, model.board.stableSinceMs);
    hash = AddU64(hash, model.board.sequence);
    hash = AddByte(hash, static_cast<uint8_t>(model.phase.tag));
    hash = AddByte(hash, model.phase.selectedSquare);
    hash = AddU64(hash, model.phase.correctionMask);
    hash = AddByte(hash, static_cast<uint8_t>(model.phase.requiredAction));
    hash = AddU64(hash, model.phase.pendingActionToken);
    hash = AddPlan(hash, model.phase.plan);
    hash = AddU64(hash, model.nextActionToken);
    hash = AddU64(hash, model.feedbackEpoch);
    return AddU64(hash, model.inputFaultEpoch);
}

} // namespace runtime
