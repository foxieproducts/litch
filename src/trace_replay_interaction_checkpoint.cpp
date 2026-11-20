#include "trace/trace_replay_interaction_checkpoint.hpp"

#include <cstdio>
#include <string_view>

#include "modules/game_persistence_coordinator.hpp"

namespace trace {
namespace {

bool Fail(const uint32_t lineNumber,
          const char* message,
          ReplayResult& outResult) {
    outResult.ok = false;
    outResult.failureLine = lineNumber;
    std::snprintf(outResult.message, sizeof(outResult.message), "%s", message);
    return false;
}

bool IsHistoricalUncheckableDivergence(const std::string_view divergence) {
    return divergence == "PhysicalOccupancy" ||
           divergence == "ExpectedOccupancy" ||
           divergence == "Phase" || divergence == "PlanKind";
}

bool HasVariantCheckpoint(const JsonlEventRecord& record) {
    return record.hasGameVariant || record.hasDuckSquare ||
           record.hasDuckTurnPhase || record.hasPendingChessMove ||
           record.hasPendingMoveFrom || record.hasPendingMoveTo ||
           record.hasPendingPromotion || record.hasVariantOutcomeReason ||
           record.hasVariantWinner || record.hasVariantStateHash ||
           record.hasAppStateHash;
}

bool ValidateVariantCheckpoint(
    const JsonlEventRecord& record,
    const modules::GamePersistenceCoordinator* const gamePersistence,
    const uint64_t modelHash,
    const uint32_t lineNumber,
    ReplayResult& outResult) {
    if (!HasVariantCheckpoint(record)) {
        return true;
    }
    if (gamePersistence == nullptr) {
        return Fail(lineNumber, "checkpoint_variant_unavailable", outResult);
    }

    const modules::GameVariantStateSnapshot state =
        gamePersistence->GetGameVariantStateSnapshot();
    if ((record.hasGameVariant &&
         record.gameVariant != static_cast<uint8_t>(state.variant)) ||
        (record.hasDuckSquare && record.duckSquare != state.duckSquare) ||
        (record.hasDuckTurnPhase &&
         record.duckTurnPhase != static_cast<uint8_t>(state.duckTurnPhase))) {
        return Fail(lineNumber, "checkpoint_variant_state_mismatch", outResult);
    }
    if ((record.hasPendingChessMove &&
         record.pendingChessMove != state.pendingChessMove) ||
        (record.hasPendingMoveFrom &&
         record.pendingMoveFrom != state.pendingMoveFrom) ||
        (record.hasPendingMoveTo &&
         record.pendingMoveTo != state.pendingMoveTo) ||
        (record.hasPendingPromotion &&
         record.pendingPromotion !=
             static_cast<uint8_t>(state.pendingPromotionSuffix))) {
        return Fail(lineNumber, "checkpoint_variant_pending_mismatch", outResult);
    }
    if ((record.hasVariantOutcomeReason &&
         record.variantOutcomeReason !=
             static_cast<uint8_t>(state.outcome.reason)) ||
        (record.hasVariantWinner &&
         record.variantWinner != static_cast<uint8_t>(state.outcome.winner))) {
        return Fail(lineNumber, "checkpoint_variant_outcome_mismatch", outResult);
    }
    if (record.hasVariantStateHash &&
        record.variantStateHash !=
            modules::ComputeGameVariantStateHash(state)) {
        return Fail(lineNumber, "checkpoint_variant_hash_mismatch", outResult);
    }
    if (record.hasAppStateHash &&
        record.appStateHash != modules::ComputeAppStateHash(modelHash, state)) {
        return Fail(lineNumber, "checkpoint_app_hash_mismatch", outResult);
    }
    return true;
}

} // namespace

bool ValidateInteractionCheckpoint(const JsonlEventRecord& record,
                                   const runtime::GameStateMachine& stateMachine,
                                   modules::GamePersistenceCoordinator* const gamePersistence,
                                   const uint32_t lineNumber,
                                   ReplayResult& outResult) {
    const runtime::ChessInteractionModel& model =
        stateMachine.GetChessInteractionModel();
    const uint64_t modelHash =
        runtime::ComputeChessInteractionModelHash(model);
    if (record.type == JsonlEventType::CheckpointVariant) {
        return ValidateVariantCheckpoint(
            record, gamePersistence, modelHash, lineNumber, outResult);
    }
    if (record.interactionPhase.empty() ||
        record.interactionPhase != runtime::ChessInteractionPhaseToString(model.phase.tag)) {
        return Fail(lineNumber, "checkpoint_interaction_phase_mismatch", outResult);
    }
    if (!record.transitionKind.empty() &&
        record.transitionKind != runtime::PhysicalTransitionKindToString(model.phase.plan.kind)) {
        return Fail(lineNumber, "checkpoint_interaction_plan_mismatch", outResult);
    }
    if (!record.requiredAction.empty() &&
        record.requiredAction != runtime::RequiredPhysicalActionToString(model.phase.requiredAction)) {
        return Fail(lineNumber, "checkpoint_interaction_required_mismatch", outResult);
    }
    if (record.hasExpectedMask &&
        record.expectedMask != model.acknowledged.occupiedMask) {
        return Fail(lineNumber, "checkpoint_interaction_expected_mismatch", outResult);
    }
    if (record.hasPhysicalMask &&
        record.physicalMask != model.board.occupiedMask) {
        return Fail(lineNumber, "checkpoint_interaction_physical_mismatch", outResult);
    }
    if (record.hasRelocationsLifted &&
        record.relocationsLifted != model.phase.plan.progress.relocationsLiftedMask) {
        return Fail(lineNumber, "checkpoint_interaction_lifted_mismatch", outResult);
    }
    if (record.hasRelocationsPlaced &&
        record.relocationsPlaced != model.phase.plan.progress.relocationsPlacedMask) {
        return Fail(lineNumber, "checkpoint_interaction_placed_mismatch", outResult);
    }
    if (record.hasRemovalComplete &&
        record.removalComplete != model.phase.plan.progress.removalComplete) {
        return Fail(lineNumber, "checkpoint_interaction_removed_mismatch", outResult);
    }
    if (record.hasRestorationComplete &&
        record.restorationComplete !=
            model.phase.plan.progress.restorationComplete) {
        return Fail(lineNumber, "checkpoint_interaction_restored_mismatch", outResult);
    }
    if (record.hasActionAcknowledged &&
        record.actionAcknowledged != model.phase.plan.progress.actionAcknowledged) {
        return Fail(lineNumber, "checkpoint_interaction_acked_mismatch", outResult);
    }
    if (record.hasActionToken &&
        record.actionToken != model.phase.pendingActionToken) {
        return Fail(lineNumber, "checkpoint_interaction_token_mismatch", outResult);
    }
    if (record.hasModelHash && record.modelHash != modelHash) {
        return Fail(lineNumber, "checkpoint_interaction_hash_mismatch", outResult);
    }
    if (!record.shadowDivergence.empty() &&
        !IsHistoricalUncheckableDivergence(record.shadowDivergence) &&
        record.shadowDivergence != runtime::InteractionInvariantStatusCodeToString(
                                       stateMachine.GetInteractionInvariantStatus().code)) {
        return Fail(lineNumber, "checkpoint_interaction_divergence_mismatch", outResult);
    }
    if (!ValidateVariantCheckpoint(
            record, gamePersistence, modelHash, lineNumber, outResult)) {
        return false;
    }
    if (gamePersistence != nullptr &&
        !gamePersistence->ResumeExternalizedDuckActionAfterCheckpoint()) {
        return Fail(lineNumber, "checkpoint_duck_resume_failed", outResult);
    }
    return true;
}

} // namespace trace
