#include "trace/trace_recorder.hpp"

#include <cstdio>

#include "modules/game_variant_state.hpp"

namespace trace {
namespace {

constexpr size_t TRACE_INTERACTION_LINE_MAX = 256U;
constexpr size_t TRACE_VARIANT_LINE_MAX = 256U;

void FormatLegacyCheckpoint(
    char* const line,
    const size_t lineSize,
    const uint32_t tMs,
    const runtime::ChessInteractionModel& model,
    const runtime::InteractionInvariantStatus& invariantStatus) {
    const runtime::PhysicalTransitionProgress& progress =
        model.phase.plan.progress;
    std::snprintf(
        line, lineSize,
        "{\"type\":\"checkpoint_interaction\",\"t_ms\":%lu,\"ph\":\"%s\",\"pl\":\"%s\",\"req\":\"%s\",\"exp\":\"0x%016llx\",\"l\":%u,\"p\":%u,\"r\":%u,\"s\":%u,\"a\":%u,\"tok\":%lu,\"h\":\"0x%016llx\",\"d\":\"%s\"}",
        static_cast<unsigned long>(tMs),
        runtime::ChessInteractionPhaseToString(model.phase.tag),
        runtime::PhysicalTransitionKindToString(model.phase.plan.kind),
        runtime::RequiredPhysicalActionToString(model.phase.requiredAction),
        static_cast<unsigned long long>(model.acknowledged.occupiedMask),
        static_cast<unsigned>(progress.relocationsLiftedMask),
        static_cast<unsigned>(progress.relocationsPlacedMask),
        progress.removalComplete ? 1U : 0U,
        progress.restorationComplete ? 1U : 0U,
        progress.actionAcknowledged ? 1U : 0U,
        static_cast<unsigned long>(model.phase.pendingActionToken),
        static_cast<unsigned long long>(
            runtime::ComputeChessInteractionModelHash(model)),
        runtime::InteractionInvariantStatusCodeToString(invariantStatus.code));
}

void FormatVariantCheckpoint(
    char* const line,
    const size_t lineSize,
    const uint32_t tMs,
    const uint64_t modelHash,
    const modules::GameVariantStateSnapshot& state) {
    const uint64_t variantHash =
        modules::ComputeGameVariantStateHash(state);
    const uint64_t appHash = modules::ComputeAppStateHash(modelHash, state);
    std::snprintf(
        line, lineSize,
        "{\"type\":\"checkpoint_variant\",\"t_ms\":%lu,\"v\":%u,\"ds\":%u,\"dp\":%u,\"pd\":%u,\"pf\":%u,\"pt\":%u,\"pp\":%u,\"or\":%u,\"win\":%u,\"vh\":\"0x%016llx\",\"ah\":\"0x%016llx\"}",
        static_cast<unsigned long>(tMs),
        static_cast<unsigned>(state.variant),
        static_cast<unsigned>(state.duckSquare),
        static_cast<unsigned>(state.duckTurnPhase),
        state.pendingChessMove ? 1U : 0U,
        static_cast<unsigned>(state.pendingMoveFrom),
        static_cast<unsigned>(state.pendingMoveTo),
        static_cast<unsigned>(
            static_cast<unsigned char>(state.pendingPromotionSuffix)),
        static_cast<unsigned>(state.outcome.reason),
        static_cast<unsigned>(state.outcome.winner),
        static_cast<unsigned long long>(variantHash),
        static_cast<unsigned long long>(appHash));
}

} // namespace

void TraceRecorder::RecordCheckpointInteraction(
    const runtime::ChessInteractionModel& model,
    const runtime::InteractionInvariantStatus& invariantStatus) {
    if (!m_active) {
        return;
    }

    char interactionLine[TRACE_INTERACTION_LINE_MAX] = {};
    FormatLegacyCheckpoint(
        interactionLine, sizeof(interactionLine), NowTraceMs(), model,
        invariantStatus);
    WriteJsonLine(interactionLine);
}

void TraceRecorder::RecordCheckpointVariant(
    const modules::GameVariantStateSnapshot& variantState,
    const uint64_t interactionModelHash) {
    if (!m_active) {
        return;
    }

    char variantLine[TRACE_VARIANT_LINE_MAX] = {};
    FormatVariantCheckpoint(
        variantLine, sizeof(variantLine), NowTraceMs(),
        interactionModelHash, variantState);
    WriteJsonLine(variantLine);
}

} // namespace trace
