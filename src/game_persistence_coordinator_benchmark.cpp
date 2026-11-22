#include "modules/game_persistence_coordinator.hpp"
#include "modules/vs_cpu_difficulty.hpp"
namespace modules {
bool GamePersistenceCoordinator::IsEngineLaneActive() const {
    return m_benchmarkActive || m_ponderThinking || (UsesAsyncThinkWorker() && ThinkWorker().IsActive());
}
bool GamePersistenceCoordinator::IsBenchmarkAdmissionStable() const {
    return m_benchmarkControlsIdle != nullptr &&
        m_benchmarkControlsIdle(m_benchmarkControlsContext) &&
        m_gameVariant == GameVariant::Classic && IsGameOverVariantAware() &&
        !m_gameClock.IsRunning() && IsSearchStableIdle() && !m_boardSettingsActive &&
        !m_litchcraft.IsActive() && !m_litchcraft.IsEntryHoldActive() &&
        !m_stateMachine.HasCommittedMovePending() && !HasPendingUndoCommand() &&
        !m_snapshotRetryPending && !m_promotionPending && !m_initialVariantValidationPending &&
        !m_pendingCpuMoveUciValid && !m_hintPendingStart && !m_vsCpuThinking && !m_ponderThinking &&
        !m_hintThinking && !m_cancelCpuWorkRequested && !m_cancelHintWorkRequested && !m_cancelAllThinkingRequested;
}
bool GamePersistenceCoordinator::StartBenchmark(const uint8_t caseId, const uint8_t level) {
    if (GetEngineBenchmarkCase(caseId) == nullptr || level < 1U || level > 11U ||
        m_benchmarkActive || ThinkWorker().IsActive() || !IsBenchmarkAdmissionStable()) return false;
    // Reap only an already quiescent mailbox; never join an active worker here.
    RefreshThinkDiagnostics();
    if (!ThinkWorker().CancelAndJoin(m_engine)) return false;
    CancelPonder(true);
    auto config = MakeThinkConfigForVsCpuLevel(level, 5U);
    config.reversibleHalfmoves = GetEngineBenchmarkCase(caseId)->reversibleHalfmoves;
    m_diagnosticRequestId = m_diagnosticRequestId == UINT32_MAX ? 1U : m_diagnosticRequestId + 1U;
    m_benchmarkStatus = {};
    m_benchmarkStatus.caseId = caseId;
    m_benchmarkStatus.level = level;
    const auto& root = GetEngineBenchmarkCase(caseId)->root;
    ThinkWorker().PrepareDiagnostics(m_diagnosticRequestId, ThinkRequestKind::Benchmark,
        config, root.side == MCUMAX_BOARD_WHITE ? SideToMove::White : SideToMove::Black, false, 255U);
    config.progressObserver = ThinkWorker().PrepareProgressObserver();
    config.candidateObserver = ThinkWorker().PrepareCandidateObserver();
    if (!m_engine.StartBenchmark(caseId, config)) {
        ThinkWorker().RecordStartFailure();
        RefreshThinkDiagnostics();
        return false;
    }
    ThinkWorker().RecordPreflight();
    m_benchmarkActive = true;
    if (!ThinkWorker().Start(m_engine)) {
        m_engine.CancelThink();
        uint8_t ignoredProgress = 0U;
        UciMove ignoredMove;
        (void)m_engine.PollThinkProgress(ignoredProgress, ignoredMove);
        ThinkWorker().RecordStartFailure();
        m_benchmarkActive = false;
        RefreshThinkDiagnostics();
        return false;
    }
    RefreshThinkDiagnostics();
    return true;
}
void GamePersistenceCoordinator::StopBenchmark() {
    if (!m_benchmarkActive) return;
    m_benchmarkStatus.cancelRequested = true;
    m_engine.CancelThink();
}
bool GamePersistenceCoordinator::TryGetBenchmarkStatus(EngineBenchmarkStatus& out) const {
    out = m_benchmarkStatus;
    if (out.caseId == 0U) return false;
    if (!m_benchmarkActive && out.diagnostics.quiescentValid && !out.diagnostics.active) return true;
    ThinkDiagnostics live;
    if (!ThinkWorker().TryGetDiagnostics(live) || live.kind != ThinkRequestKind::Benchmark ||
        live.requestId != out.diagnostics.requestId) {
        out = {};
        return false;
    }
    out.diagnostics = live;
    return out.diagnostics.requestId != 0U;
}
bool GamePersistenceCoordinator::UpdateBenchmark() {
    if (!m_benchmarkActive) return false;
    if (!IsBenchmarkAdmissionStable()) StopBenchmark();
    RefreshThinkDiagnostics();
    if (ThinkWorker().IsActive()) return true;
    // Final publication is diagnostic only, copied after all shared cleanup.
    RefreshThinkDiagnostics();
    if (!m_benchmarkStatus.cancelRequested && ThinkWorker().IsDone()) {
        uint8_t progress = 0U;
        uint32_t elapsed = 0U, nodes = 0U;
        UciMove best;
        (void)ThinkWorker().GetResult(progress, best, elapsed, nodes);
        m_benchmarkStatus.finalMove = best;
        m_benchmarkStatus.finalMoveValid = !best.Empty();
    }
    (void)ThinkWorker().CancelAndJoin(m_engine);
    m_benchmarkActive = false;
    return false;
}
} // namespace modules
