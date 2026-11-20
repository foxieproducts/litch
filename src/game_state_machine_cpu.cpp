#include "runtime/game_state_machine.hpp"

#include "logger.hpp"
#include "runtime/game_state_machine_castling.hpp"

namespace runtime {

bool GameStateMachine::BuildCpuTransitionPlan(
    const uint8_t from,
    const uint8_t to,
    const bool captureSquareValid,
    const uint8_t captureSquare,
    const char promotionSuffixLower,
    PhysicalTransitionPlan& outPlan) const {
    if (from >= 64 || to >= 64) {
        return false;
    }

    outPlan = PhysicalTransitionPlan{};
    outPlan.source = PhysicalTransitionSource::Directed;
    outPlan.directedOrigin = DirectedMoveOrigin::Cpu;
    outPlan.beforeOccupiedMask =
        m_chessInteractionModel.acknowledged.occupiedMask;
    outPlan.afterOccupiedMask = GetExpectedOccupancyMask();
    outPlan.moveFrom = from;
    outPlan.moveTo = to;
    outPlan.relocationCount = 1U;
    outPlan.relocations[0] = {from, to};

    // Apply the primary move.
    const uint64_t fromMask = (1ULL << from);
    const uint64_t toMask = (1ULL << to);
    outPlan.afterOccupiedMask &= ~fromMask;
    outPlan.afterOccupiedMask |= toMask;

    // Apply capture removal (en-passant or normal capture).
    if (captureSquareValid && captureSquare < 64) {
        outPlan.afterOccupiedMask &= ~(1ULL << captureSquare);
        outPlan.afterOccupiedMask |= toMask;
    }

    // Castling: only treat king moves as composite (king+rook).
    litch_max::CastlingPattern p;
    bool isKingMove = false;

    const ChessPositionState expected = GetInteractionExpectedPosition();
    const uint64_t expectedKings = expected.kingMask;
    const uint64_t expectedRooks = expected.rookMask;
    const bool typeModelAvailable = expectedKings != 0U || expectedRooks != 0U;
    if (TryMatchCastlingPattern(from, to, p, isKingMove) && isKingMove) {
        if (typeModelAvailable) {
            const uint64_t kingFromMask = (1ULL << p.kingFrom);
            const uint64_t rookFromMask = (1ULL << p.rookFrom);
            if (((expectedKings & kingFromMask) == 0U) ||
                ((expectedRooks & rookFromMask) == 0U)) {
                isKingMove = false;
            }
        }
    }
    if (isKingMove) {
        outPlan.kind = PhysicalTransitionKind::Castling;
        outPlan.relocationCount = 2U;
        outPlan.relocations[1] = {p.rookFrom, p.rookTo};
        outPlan.afterOccupiedMask &= ~(1ULL << p.rookFrom);
        outPlan.afterOccupiedMask |= (1ULL << p.rookTo);
    } else if (promotionSuffixLower != '\0') {
        outPlan.kind = PhysicalTransitionKind::Promotion;
        outPlan.promotionRequired = true;
    } else if (captureSquareValid) {
        outPlan.kind = captureSquare == to
                           ? PhysicalTransitionKind::Capture
                           : PhysicalTransitionKind::EnPassant;
    } else {
        outPlan.kind = PhysicalTransitionKind::QuietMove;
    }
    if (!isKingMove && captureSquareValid) {
        outPlan.removalRequired = true;
        outPlan.removalSquare = captureSquare;
    }
    return true;
}

void GameStateMachine::ClearCpuMove(const bool preservePendingAction) {
    if (!preservePendingAction &&
        IsCpuDirectedTransition(m_chessInteractionModel.phase.plan)) {
        ReduceInteraction(ChessInteractionEvent::MakePlanCanceled());
    }
    m_cpuExecution.queued = false;
    m_cpuExecution.promotionSuffixLower = '\0';
    if (!m_cpuExecution.moveSettleActive) {
        m_cpuExecution.settleDestinationSuppressed = false;
    }
    ClearCpuTentativeCorrection();
}

void GameStateMachine::CancelQueuedCpuMove() {
    if (!m_cpuExecution.queued && !m_cpuExecution.moveSettleActive) {
        return;
    }

    const bool clearedCpuIllegal = m_cpuExecution.illegalMask != 0ULL;
    CancelCurrentMove();
    ClearCpuMove();
    m_cpuExecution.illegalMask = 0;
    if (clearedCpuIllegal) {
        RestartIllegalFlashCadence();
    }
}

bool GameStateMachine::QueueCpuMove(const uint8_t fromSquare,
                                    const uint8_t toSquare,
                                    const bool captureSquareValid,
                                    const uint8_t captureSquare,
                                    const char promotionSuffixLower) {
    if (IsPromotionActive()) {
        return false;
    }

    PhysicalTransitionPlan plan;
    if (!BuildCpuTransitionPlan(fromSquare,
                                toSquare,
                                captureSquareValid,
                                captureSquare,
                                promotionSuffixLower,
                                plan)) {
        return false;
    }
    const PhysicalTransitionPlan& restored =
        m_chessInteractionModel.phase.plan;
    const bool matchingRestoredPlan =
        m_chessInteractionModel.phase.tag ==
            ChessInteractionPhase::ExecutingPlan &&
        IsCpuDirectedTransition(restored) &&
        restored.kind == plan.kind &&
        restored.beforeOccupiedMask == plan.beforeOccupiedMask &&
        restored.afterOccupiedMask == plan.afterOccupiedMask &&
        restored.moveFrom == plan.moveFrom &&
        restored.moveTo == plan.moveTo &&
        restored.relocationCount == plan.relocationCount &&
        restored.removalRequired == plan.removalRequired &&
        restored.removalSquare == plan.removalSquare &&
        restored.promotionRequired == plan.promotionRequired;
    if (!matchingRestoredPlan) {
        ReduceInteraction(ChessInteractionEvent::MakePlanRequested(plan));
    }
    if (m_chessInteractionModel.phase.tag !=
            ChessInteractionPhase::ExecutingPlan ||
        !IsCpuDirectedTransition(m_chessInteractionModel.phase.plan)) {
        logger::Warn("cpu: interaction plan rejected phase=%s faults=%lu",
                     ChessInteractionPhaseToString(
                         m_chessInteractionModel.phase.tag),
                     static_cast<unsigned long>(
                         m_chessInteractionModel.inputFaultEpoch));
        return false;
    }

    m_cpuExecution.queued = true;
    m_cpuExecution.promotionSuffixLower = promotionSuffixLower;

    // Clear any stale CPU-illegal markers from prior prompts. Prompt
    // bookkeeping alone must not phase-shift another correction cue.
    const bool clearedCpuIllegal = m_cpuExecution.illegalMask != 0ULL;
    m_cpuExecution.illegalMask = 0;
    if (clearedCpuIllegal) {
        RestartIllegalFlashCadence();
    }
    ClearCpuTentativeCorrection();

    if (!m_placementModeActive) {
        m_cpuExecution.promptElapsed.Reset();
        const uint32_t nowMs = fxe::Millis();
        const uint32_t stableReq = m_config.placementStableRequiredMs;
        m_lastInteractionElapsed.Reset();
        if (stableReq != 0) {
            m_lastInteractionElapsed = fxe::ElapsedTime(nowMs - stableReq);
        }
    }

    return true;
}

bool GameStateMachine::TryFinalizeCpuMove() {
    if (!m_cpuExecution.queued) {
        return false;
    }

    // If settle is disabled, preserve legacy immediate-finalize behavior.
    const uint32_t settleDurationMs = m_config.settleDurationMs;

    // For destination captures, don't allow the move to finalize until the
    // destination has been observed empty at least once.
    const PhysicalTransitionPlan& pendingPlan =
        m_chessInteractionModel.phase.plan;
    if (!IsCpuDirectedTransition(pendingPlan)) {
        return false;
    }
    if (pendingPlan.removalRequired &&
        pendingPlan.removalSquare == pendingPlan.moveTo &&
        !pendingPlan.progress.removalComplete) {
        return false;
    }

    if (m_chessInteractionModel.board.occupiedMask !=
        pendingPlan.afterOccupiedMask) {
        return false;
    }

    if (!IsSensorsStableForPlacement()) {
        return false;
    }

    if (m_chessInteractionModel.phase.tag ==
            ChessInteractionPhase::ExecutingPlan &&
        IsCpuDirectedTransition(m_chessInteractionModel.phase.plan)) {
        ReduceInteraction(ChessInteractionEvent::MakeSettleConfirmed());
    }
    const bool reducerOwned =
        m_chessInteractionModel.phase.tag ==
            ChessInteractionPhase::AwaitingAction &&
        IsCpuDirectedTransition(m_chessInteractionModel.phase.plan);
    if (!reducerOwned) {
        return false;
    }

    const uint8_t committedTo = pendingPlan.moveTo;
    uint8_t settleSquare = committedTo;
    // Correction may finish with an unrelated piece being restored. Only a
    // destination in this move may inherit the CPU's settle presentation.
    for (uint8_t i = 0U; i < pendingPlan.relocationCount; ++i) {
        if (pendingPlan.relocations[i].to == m_lastSensorSquare) {
            settleSquare = m_lastSensorSquare;
        }
    }

    m_checkCacheRefreshRequested = true;
    m_visualActiveSideOverridePersist = modules::PersistedSide::Unknown;
    m_statusEdgeActiveSideOverridePersist = modules::PersistedSide::Unknown;
    CancelInitialKingPlacementPrompt();

    m_cpuExecution.illegalMask = 0;
    ClearCpuMove(true);

    if (settleDurationMs != 0U) {
        StartCpuMoveSettling(settleSquare);
    }

    return true;
}

bool GameStateMachine::ConsumeCpuIdleTimeoutFired() {
    const bool fired = m_cpuExecution.idleTimeoutFired;
    m_cpuExecution.idleTimeoutFired = false;
    return fired;
}

} // namespace runtime
