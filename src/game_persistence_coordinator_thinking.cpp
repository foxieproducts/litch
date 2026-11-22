#include "modules/game_persistence_coordinator.hpp"

#include "modules/beginner_mode_policy.hpp"
#include "modules/initial_fen.hpp"
#include "modules/vs_cpu_difficulty.hpp"

namespace modules {
namespace {

constexpr uint32_t CPU_FAILURE_RETRY_MS = 1000U;

} // namespace

ThinkConfig GamePersistenceCoordinator::MakeCpuThinkConfig(
    const uint32_t vsCpuLevel) const {
    // Only a retained line from the actual opening proves an early turn.
    // Imported positions and truncated/unknown prefixes use full strength.
    // Committed history survives reload and excludes queued physical moves;
    // Undo naturally restores the corresponding point in the ramp.
    const bool completeOpeningHistory = m_gameHistoryPersistence.valid &&
        m_gameHistoryPersistence.present && !m_gameHistoryPersistence.captureIncomplete &&
        m_undoBaseFen.Equals(INITIAL_FEN);
    const uint32_t completedTurns = completeOpeningHistory
        ? GetUndoHistoryMoveCount() / 2U : 2U;
    ThinkConfig config = MakeThinkConfigForVsCpuTurn(vsCpuLevel, completedTurns, m_vsCpuRngSeed,
        IsDuckGame() ? CpuSearchVariant::Duck : CpuSearchVariant::Classic);
    FillChessKnowledgeContext(config);
    return config;
}

bool GamePersistenceCoordinator::IsCpuRetryReady() const {
    return !m_cpuRetryElapsed.Started() ||
           m_cpuRetryElapsed.HasElapsed(CPU_FAILURE_RETRY_MS);
}

bool GamePersistenceCoordinator::UsesAsyncThinkWorker() const {
#ifdef ARDUINO
    return true;
#else
    return m_asyncThinkWorkerOverride != nullptr || m_benchmarkActive || m_ponderThinking;
#endif
}

AsyncThinkWorker& GamePersistenceCoordinator::ThinkWorker() {
    return m_asyncThinkWorkerOverride != nullptr
               ? *m_asyncThinkWorkerOverride
               : m_asyncThinkWorker;
}

const AsyncThinkWorker& GamePersistenceCoordinator::ThinkWorker() const {
    return m_asyncThinkWorkerOverride != nullptr
               ? *m_asyncThinkWorkerOverride
               : m_asyncThinkWorker;
}

bool GamePersistenceCoordinator::QuiesceAllThinking() {
    // Preserve qualified speculative work for an exact physical human commit.
    CancelPonder(false);
    if (m_ponderThinking) {
        if (ThinkWorker().IsActive()) return false;
        // Reap only after the final publication and worker statistics are copied.
        CollectPonderResult();
    }
    m_cpuRetryElapsed = fxe::ElapsedTime{};
    m_cpuThinkingEstimate.Clear();
    m_stateMachine.SetCpuThinkingActive(false);
    if (m_vsCpuThinking) RetireThinkDiagnostics(ThinkRequestKind::Cpu);
    if (m_hintThinking) RetireThinkDiagnostics(ThinkRequestKind::Hint);
    if (m_benchmarkActive) StopBenchmark();
    const bool coordinatorOwnsSearch = m_vsCpuThinking || m_hintThinking || m_benchmarkActive || m_ponderThinking;
    const bool workerActive = UsesAsyncThinkWorker() &&
                              ThinkWorker().IsActive();
    if (coordinatorOwnsSearch || workerActive) {
        if (UsesAsyncThinkWorker()) {
            if (!ThinkWorker().CancelAndJoin(m_engine)) {
                return false;
            }
        } else {
            // Default native tests poll the engine synchronously and have no
            // worker thread to reap.
            m_engine.CancelThink();
        }
    } else if (UsesAsyncThinkWorker()) {
        // Reap/discard a result which completed just before this call. This is
        // nonblocking because IsActive() is already false.
        if (!ThinkWorker().CancelAndJoin(m_engine)) {
            return false;
        }
    }

    RefreshThinkDiagnostics();
    m_benchmarkActive = false;
    m_ponderThinking = false;
    m_vsCpuThinking = false;
    m_hintThinking = false;
    m_stateMachine.SetHintThinkingActive(false);
    return true;
}

void GamePersistenceCoordinator::RequestCancelCpuWork() {
    CancelPonder(true);
    StopBenchmark();
    RetireThinkDiagnostics(ThinkRequestKind::Cpu);
    m_cpuRetryElapsed = fxe::ElapsedTime{};
    m_cpuThinkingEstimate.Clear();
    m_stateMachine.SetCpuThinkingActive(false);
    ClearPendingCpuMoveSelection();
    m_stateMachine.CancelQueuedCpuMove();
    m_cancelCpuWorkRequested = true;
    if (m_vsCpuThinking) {
        m_engine.CancelThink();
    }
}

void GamePersistenceCoordinator::RequestCancelHintWorkAndRestart() {
    CancelPonder(true);
    StopBenchmark();
    RetireThinkDiagnostics(ThinkRequestKind::Hint);
    m_stateMachine.SetHintThinkingActive(false);
    m_stateMachine.SetHintDisplayRequested(false);
    m_stateMachine.ClearHintMove();
    m_hintPendingStart = false;
    m_hintLastSideToMoveValid = false;
    ResetHintRetry();
    ClearCachedHintResults();
    m_cancelHintWorkRequested = true;
    if (m_hintThinking) {
        m_engine.CancelThink();
    }
}

bool GamePersistenceCoordinator::IsSearchStableIdle() const {
    const runtime::TaggedInteractionState& phase =
        m_stateMachine.GetChessInteractionModel().phase;
    return phase.tag == runtime::ChessInteractionPhase::Idle &&
           !m_stateMachine.IsMoveSettlingActive() &&
           !m_stateMachine.HasQueuedCpuMove() &&
           !m_stateMachine.IsPlacementModeActive() &&
           !m_stateMachine.IsTextOverlayActive() &&
           !m_stateMachine.IsKingQueenSwapPromptActive() &&
           !m_stateMachine.IsUndoPromptActive() &&
           !m_stateMachine.IsPromotionActive() &&
           m_stateMachine.IsExpectedOccupancyMatched() &&
           !m_stateMachine.HasHumanIllegal() &&
           !m_stateMachine.HasCpuIllegal();
}

bool GamePersistenceCoordinator::IsHintSearchStableIdle() const {
    return !m_boardSettingsActive && IsSearchStableIdle();
}

bool GamePersistenceCoordinator::CanPublishHint(
    const uint8_t fromSquare, const uint8_t toSquare) const {
    if (m_boardSettingsActive) {
        return false;
    }
    if (IsHintSearchStableIdle()) {
        return true;
    }
    if (!IsMatchingHumanHintPickup(
            m_stateMachine.GetChessInteractionModel().phase, fromSquare) ||
        toSquare >= 64U ||
        ((m_stateMachine.GetValidDestMask() >> toSquare) & 1ULL) == 0ULL ||
        m_stateMachine.IsPlacementModeActive() ||
        m_stateMachine.IsTextOverlayActive() ||
        m_stateMachine.IsKingQueenSwapPromptActive() ||
        m_stateMachine.IsUndoPromptActive() ||
        m_stateMachine.IsPromotionActive() ||
        m_stateMachine.IsMoveSettlingActive() ||
        m_stateMachine.HasQueuedCpuMove() ||
        m_stateMachine.HasHumanIllegal() || m_stateMachine.HasCpuIllegal()) {
        return false;
    }
    uint8_t renderedFrom = 64U;
    uint8_t renderedTo = 64U;
    // First publication may arrive during pickup. A visible suggestion is
    // immutable until Idle, even if a later result has naturally completed.
    return !m_stateMachine.IsHintDisplayRequested() ||
           !m_stateMachine.TryGetHintMoveSquares(renderedFrom, renderedTo) ||
           (renderedFrom == fromSquare && renderedTo == toSquare);
}

void GamePersistenceCoordinator::FillChessKnowledgeContext(ThinkConfig& config) const {
    if (IsDuckGame()) return;
    const uint8_t count = GetUndoHistoryMoveCount();
    const bool completeHistory = m_gameHistoryPersistence.valid &&
        m_gameHistoryPersistence.present && !m_gameHistoryPersistence.captureIncomplete &&
        m_gameHistoryPersistence.currentPly == count && m_undoBaseFen.Equals(INITIAL_FEN);
    bool knownClock = completeHistory;
    // Imported/truncated history may retain only part of a reversible run.
    // A retained real pawn move/capture establishes the actual count again.
    for (uint16_t field = 0U; field + UNDO_HISTORY_FIELDS_PER_MOVE <= m_undoHistoryFieldCount;
         field = static_cast<uint16_t>(field + UNDO_HISTORY_FIELDS_PER_MOVE)) {
        const uint8_t piece = m_undoHistoryFields[field + 1U];
        if (piece == 'p' || piece == 'P' || m_undoHistoryFields[field + 2U] != 0U) knownClock = true;
    }
    config.reversibleHalfmoves = knownClock ? m_halfmoveClockPlies : UINT16_MAX;
    if (!completeHistory || count > THINK_OPENING_MAX_PLIES) return;
    auto& prefix = config.openingPrefix;
    for (uint8_t ply = 0U; ply < count; ++ply) {
        const uint16_t field = static_cast<uint16_t>(ply) * UNDO_HISTORY_FIELDS_PER_MOVE;
        // This book has no promotion prefixes. Decline unsupported metadata
        // instead of reinterpreting raw undo storage into another authority.
        if (m_undoHistoryFields[field + 4U] != 0U) return;
        prefix.moves[ply].fromSquare = m_undoHistoryFields[field + 5U];
        prefix.moves[ply].toSquare = m_undoHistoryFields[field + 6U];
    }
    prefix.moveCount = count;
    prefix.fromCanonicalStart = true;
}

ThinkConfig GamePersistenceCoordinator::MakeHintThinkConfig() const {
    ThinkConfig config = MakeThinkConfigForVsCpuLevel(11U, 1U);
    constexpr uint32_t ANALYSIS_WINDOW_MS = 90000U;
    const uint32_t elapsed = IsCurrentHintAnalysisPosition() && m_hintInitialWindow.Started()
        ? m_hintInitialWindow.ElapsedMs() : 0U;
    config.maxThinkMs = elapsed < ANALYSIS_WINDOW_MS
        ? ANALYSIS_WINDOW_MS - elapsed : 1U;
    config.continuousAnalysis = true;
    config.minimumCalculationMs = 500U;
    config.maxNodes = 0U;
    config.maxDepth = 0U;
    FillChessKnowledgeContext(config);
    return config;
}

uint8_t GamePersistenceCoordinator::NextHintDifficulty() const {
    constexpr uint32_t ANALYSIS_WINDOW_MS = 90000U;
    if (!IsCurrentHintAnalysisPosition()) {
        return 11U;
    }
    if (m_hintCompletedDifficulty != 0U ||
        (m_hintInitialWindow.Started() &&
         m_hintInitialWindow.HasElapsed(ANALYSIS_WINDOW_MS))) {
        return 0U;
    }
    return 11U;
}

bool GamePersistenceCoordinator::IsCurrentHintAnalysisPosition() const {
    return m_hintAnalysisFen.Equals(m_cachedFen.View()) &&
           m_hintAnalysisDuckSquare == (IsDuckGame() ? m_duckSquare : INVALID_DUCK_SQUARE);
}

void GamePersistenceCoordinator::BeginHintSearch(ThinkConfig& config) {
    if (!IsCurrentHintAnalysisPosition()) {
        m_hintAnalysisFen = m_cachedFen;
        m_hintAnalysisDuckSquare = IsDuckGame() ? m_duckSquare : INVALID_DUCK_SQUARE;
        m_hintInitialWindow = fxe::ElapsedTime{};
        m_hintCompletedDifficulty = 0U;
        m_hintCalculatedMs = 0U;
        m_hintScannedNodes = 0U;
        m_lastHintThinkStatsValid = false;
        m_lastHintThinkBestUci.Clear();
    }
    if (!m_hintInitialWindow.Started()) {
        m_hintInitialWindow.Reset();
    }
    BeginThinkDiagnostics(config, ThinkRequestKind::Hint);
    m_hintSearchElapsed.Reset();
    m_hintThinkDifficulty = config.difficulty;
    m_hintThinkAutomatic = m_coachingHintActive;
    config.candidateObserver = ThinkWorker().PrepareCandidateObserver();
    config.progressObserver = ThinkWorker().PrepareProgressObserver(true);
}

bool GamePersistenceCoordinator::HasCachedHintForCurrentPosition() const {
    if (IsDuckGame()) {
        return m_duckHintTurnValid && m_duckHintFen.Equals(m_cachedFen.View()) &&
               m_duckHintSquare == m_duckSquare;
    }
    uint8_t from = 64U;
    uint8_t to = 64U;
    return TryGetCachedHintMoveForCurrentPosition(from, to);
}

void GamePersistenceCoordinator::SetCoachingHintActive(const bool active) {
    if (m_coachingHintActive == active) {
        return;
    }
    m_coachingHintActive = active;
    if (active) CancelPonder(true);
    const bool haveHint = HasCachedHintForCurrentPosition();
    if (!haveHint) {
        ResetHintRetry();
    }
    // Visibility changes reuse the same background analysis.
}

void GamePersistenceCoordinator::RequestCancelAllThinking() {
    CancelPonder(true);
    StopBenchmark();
    RetireThinkDiagnostics(ThinkRequestKind::Cpu);
    RetireThinkDiagnostics(ThinkRequestKind::Hint);
    m_cpuRetryElapsed = fxe::ElapsedTime{};
    m_cpuThinkingEstimate.Clear();
    m_stateMachine.SetCpuThinkingActive(false);
    m_stateMachine.SetHintThinkingActive(false);
    m_stateMachine.SetHintDisplayRequested(false);
    m_stateMachine.ClearHintMove();

    m_hintPendingStart = false;
    ResetHintRetry();

    m_cancelAllThinkingRequested = true;
    if (m_vsCpuThinking || m_hintThinking) {
        m_engine.CancelThink();
    }
}

void GamePersistenceCoordinator::BeginCpuThinking(const ThinkConfig& config) {
    m_cpuThinkingEstimate.Start(
        m_engineDerived.sideToMove == SideToMove::White ? 'w' : 'b',
        config.maxNodes, config.maxThinkMs);
    m_vsCpuThinking = true;
    m_stateMachine.SetCpuThinkingActive(true);
}

void GamePersistenceCoordinator::UpdateCpuThinkingPresentation() {
    if (m_vsCpuThinking) {
        m_cpuThinkingEstimate.Update(
            UsesAsyncThinkWorker() ? ThinkWorker().PublishedNodeCount() : 0U);
    }
}

CpuThinkingPresentation GamePersistenceCoordinator::GetCpuThinkingPresentation() const {
    if (!m_vsCpuThinking || !m_stateMachine.IsCpuThinkingActive() ||
        m_cancelCpuWorkRequested || m_cancelAllThinkingRequested ||
        HasClockTerminalOutcome() || m_variantOutcome.IsTerminal()) {
        return {};
    }
    CpuThinkingPresentation out = m_cpuThinkingEstimate.Snapshot();
    if (!UsesAsyncThinkWorker() || ThinkWorker().PublishedNodeCount() == 0U) {
        out.remainingSeconds = 0U;
    }
    return out;
}

} // namespace modules
