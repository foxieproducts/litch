#include "runtime/game_state_machine.hpp"

namespace runtime {
namespace {

uint64_t CpuAllowedRemovalMask(const PhysicalTransitionPlan& plan) {
    uint64_t mask = 0ULL;
    for (uint8_t i = 0U; i < plan.relocationCount; ++i) {
        if (plan.relocations[i].from < 64U) {
            mask |= 1ULL << plan.relocations[i].from;
        }
    }
    if (plan.removalRequired && plan.removalSquare < 64U) {
        mask |= 1ULL << plan.removalSquare;
    }
    return mask;
}

uint64_t CpuUnexpectedChangeMask(
    const TaggedInteractionState& before,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    const uint64_t removed = changedMask & ~occupiedMask;
    const uint64_t placed = changedMask & occupiedMask;
    const uint64_t target = before.plan.afterOccupiedMask;
    const uint64_t unexpectedRemoval =
        removed & ~CpuAllowedRemovalMask(before.plan) & target;
    const uint64_t unexpectedPlacement = placed & ~target;
    return unexpectedRemoval | unexpectedPlacement;
}

bool HasCpuRelocationInHand(const PhysicalTransitionPlan& plan,
                            const uint64_t occupiedMask) {
    for (uint8_t i = 0U; i < plan.relocationCount; ++i) {
        const PieceRelocation& relocation = plan.relocations[i];
        if (relocation.from >= 64U || relocation.to >= 64U) {
            continue;
        }
        const bool sourceAbsent =
            (occupiedMask & (1ULL << relocation.from)) == 0ULL;
        const bool destinationOccupied =
            (occupiedMask & (1ULL << relocation.to)) != 0ULL;
        const bool destinationHoldsPendingCapture =
            plan.removalRequired &&
            plan.removalSquare == relocation.to &&
            !plan.progress.removalComplete;
        if (sourceAbsent &&
            (!destinationOccupied || destinationHoldsPendingCapture)) {
            return true;
        }
    }
    return false;
}

bool IsTentativeCpuMoverPlacement(const PhysicalTransitionPlan& plan,
                                  const uint64_t unexpected,
                                  const uint64_t occupiedMask) {
    const bool singleUnexpectedSquare =
        unexpected != 0ULL &&
        (unexpected & (unexpected - 1ULL)) == 0ULL;
    const bool unexpectedIsPlacement =
        (unexpected & occupiedMask) == unexpected;
    return singleUnexpectedSquare && unexpectedIsPlacement &&
           HasCpuRelocationInHand(plan, occupiedMask);
}

} // namespace

void GameStateMachine::ClearCpuTentativeCorrection() {
    m_cpuExecution.tentativeCorrectionMask = 0ULL;
}

bool GameStateMachine::HandleQueuedCpuCorrectionObservation(
    const TaggedInteractionState& before,
    const uint64_t changedMask,
    const uint64_t occupiedMask,
    const uint32_t tMs,
    const uint32_t sequence) {
    const uint64_t target = before.plan.afterOccupiedMask;
    const uint64_t mismatch = occupiedMask ^ target;
    const uint64_t unexpected = CpuUnexpectedChangeMask(
        before, changedMask, occupiedMask);
    const uint64_t legalProgress =
        changedMask & GetLegalProgressPhysicalMask(before) & ~unexpected;

    if (m_cpuExecution.illegalMask != 0ULL) {
        ReduceInteraction(
            ChessInteractionEvent::MakeCaptureProgressBoardObservation(
                tMs, sequence, changedMask, occupiedMask));
        const uint64_t previous = m_cpuExecution.illegalMask;
        m_cpuExecution.illegalMask &= mismatch;
        m_cpuExecution.illegalMask |= unexpected;
        if (m_cpuExecution.illegalMask != previous) {
            RestartIllegalFlashCadence();
        }
        ReconcileCpuPlanAfterModal();
        return true;
    }

    if (m_cpuExecution.tentativeCorrectionMask != 0ULL) {
        const uint64_t remainingTentative =
            m_cpuExecution.tentativeCorrectionMask & mismatch;
        const uint64_t newContradiction =
            unexpected & ~m_cpuExecution.tentativeCorrectionMask;
        ReduceInteraction(
            ChessInteractionEvent::MakeCaptureProgressBoardObservation(
                tMs, sequence, changedMask, occupiedMask));
        if (remainingTentative == 0ULL &&
            newContradiction == 0ULL) {
            ClearCpuTentativeCorrection();
            ReconcileCpuPlanAfterModal();
            return true;
        }

        // Any second contradictory change, or valid plan progress attempted
        // while the first mismatch remains, promotes only the actual mismatch
        // squares to the latched hard-correction state.
        if (newContradiction != 0ULL || legalProgress != 0ULL) {
            m_cpuExecution.illegalMask |=
                remainingTentative | newContradiction;
            ClearCpuTentativeCorrection();
            RestartIllegalFlashCadence();
        }
        return true;
    }

    if (unexpected == 0ULL) {
        return false;
    }

    ReduceInteraction(
        ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            tMs, sequence, changedMask, occupiedMask));
    if (IsTentativeCpuMoverPlacement(
            before.plan, unexpected, occupiedMask)) {
        m_cpuExecution.tentativeCorrectionMask = unexpected;
    } else {
        m_cpuExecution.illegalMask |= unexpected;
        RestartIllegalFlashCadence();
    }
    return true;
}

} // namespace runtime
