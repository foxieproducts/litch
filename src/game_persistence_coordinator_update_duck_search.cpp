#include <initializer_list>

#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/chess_strings.hpp"
#include <litch_max/duck_chess_rules.hpp>
#include "modules/vs_cpu_turn.hpp"

namespace modules {
namespace {

bool IsDuckDestinationValid(const litch_max::DuckChessMoveResult& applied,
                            const uint8_t duckFrom,
                            const uint8_t duckTo) {
    if (applied.capturedKing) {
        return duckTo == litch_max::DUCK_CHESS_INVALID_SQUARE;
    }
    char piece = '\0';
    return duckTo < 64U && duckTo != duckFrom &&
           !TryGetFenPieceAtSquare(
               applied.resultingFen.CStr(), duckTo, piece);
}

bool ValidateDuckResult(const char* fen,
                        const uint8_t duckSquare,
                        const DuckThinkResult& result,
                        litch_max::ParsedUciMove& outMove,
                        litch_max::DuckChessMoveResult& outApplied) {
    return litch_max::TryParseUciMove(result.chessMove.CStr(), outMove) &&
           litch_max::TryApplyDuckMove(fen, duckSquare,
                            result.chessMove.CStr(), outApplied) &&
           outApplied.capturedKing == result.capturedKing &&
           IsDuckDestinationValid(
               outApplied, duckSquare, result.duckDestination);
}

} // namespace

bool GamePersistenceCoordinator::PollDuckSearch(
    const bool stableIdle) {
    if (m_hintThinking) {
        ConsumeLiveHintCandidate();
        // The cached legal compound decision is immediately available. Held
        // guidance freezes its tuple until physical ownership returns to Idle.
        (void)ShowCachedDuckHint();
    }
    if (m_hintThinking && HasCachedHintForCurrentPosition() && !stableIdle) {
        // Keep the last compound suggestion stable during physical handling.
        // A completed refinement remains in the existing worker/engine mailbox
        // until Idle or an invalidating action safely discards it.
        return true;
    }
    uint8_t progress = 0U;
    UciMove best;
    uint32_t elapsedMs = 0U;
    uint32_t nodeCount = 0U;
    ThinkState state = ThinkState::Thinking;
    if (UsesAsyncThinkWorker()) {
        if (ThinkWorker().IsDone() &&
            !ThinkWorker().IsActive()) {
            state = ThinkWorker().GetResult(
                progress, best, elapsedMs, nodeCount);
        }
    } else {
        const auto pollElapsed = fxe::ElapsedTime::StartNow();
        ThinkWorker().RecordSynchronousPollStarted();
        state = m_engine.PollThinkProgress(progress, best);
        elapsedMs = pollElapsed.ElapsedMs();
        // Synchronous native searches publish while Poll owns the engine.
        // Preserve their actual candidate before processing a terminal state.
        if (m_hintThinking) { ConsumeLiveHintCandidate(); }
    }
    if (state == ThinkState::Thinking) {
        return true;
    }

    if (!UsesAsyncThinkWorker()) {
        ThinkStats stats;
        const bool statsValid = m_engine.GetLastThinkStats(stats);
        if (statsValid) {
            nodeCount = stats.nodeCount;
            if (stats.calculationMsValid) elapsedMs = stats.calculationMs;
        }
        ThinkWorker().RecordSynchronousResult(state, nodeCount, statsValid);
    }
    RefreshThinkDiagnostics();
    const bool wasCpu = m_vsCpuThinking;
    if (!wasCpu) {
        if (!UsesAsyncThinkWorker()) {
            ThinkStats stats;
            if (m_engine.GetLastThinkStats(stats)) { nodeCount = stats.nodeCount; }
        }
        RecordHintSearchStats(elapsedMs, nodeCount);
        if (state == ThinkState::Done) {
            m_lastHintThinkStatsValid = true;
            m_lastHintThinkElapsedMs = elapsedMs;
            m_lastHintThinkNodeCount = nodeCount;
            m_lastHintThinkBestUci = best;
            m_lastHintThinkDifficulty = m_hintThinkDifficulty;
        }
    }
    m_cpuThinkingEstimate.Clear();
    m_vsCpuThinking = false;
    m_hintThinking = false;
    m_stateMachine.SetCpuThinkingActive(false);
    m_stateMachine.SetHintThinkingActive(false);
    if (state != ThinkState::Done) {
        logger::Warn("duck search: aborted state=%u",
                     static_cast<unsigned>(state));
        DispatchInteractionAdapterResult(
            runtime::AppEventType::SearchResult, "search", false);
        if (wasCpu) {
            if (state != ThinkState::Canceled && !m_cancelCpuWorkRequested && !m_cancelAllThinkingRequested) {
                (void)RetainImmediateCpuDecision();
            }
            m_cpuRetryElapsed.Reset();
        } else {
            ScheduleHintRetry("duck_hint_search_aborted");
        }
        return true;
    }
    if (!ApplyCompletedDuckSearchResult(wasCpu, stableIdle)) {
        if (wasCpu) {
            (void)RetainImmediateCpuDecision();
            m_cpuRetryElapsed.Reset();
        } else if (HasCachedHintForCurrentPosition()) {
            m_hintCompletedDifficulty = m_hintThinkDifficulty;
            ResetHintRetry();
        } else {
            ScheduleHintRetry("duck_hint_no_completed_result");
        }
    }
    return true;
}

void GamePersistenceCoordinator::CaptureDisplayedDuckHintTurn() {
    if (m_duckHintTurnValid && m_duckHintFen.Equals(m_cachedFen.View()) &&
        m_duckHintSquare == m_duckSquare && m_stateMachine.IsHintDisplayRequested() &&
        m_stateMachine.GetRenderState().hintMoveValid) {
        const auto shown = m_stateMachine.GetVisibleHintPresentation();
        for (const auto* candidate : {&m_duckHintTurn, &m_displayedDuckHintTurn}) {
            litch_max::ParsedUciMove parsed;
            if (litch_max::TryParseUciMove(candidate->chessMove.CStr(), parsed) &&
                shown.fromSquare == parsed.fromSquare && shown.toSquare == parsed.toSquare &&
                shown.duckFromSquare == m_duckSquare && shown.duckToSquare == candidate->duckDestination) {
                m_displayedDuckHintTurn = *candidate;
                return;
            }
        }
    }
    m_displayedDuckHintTurn = DuckThinkResult{};
}

bool GamePersistenceCoordinator::CacheLiveDuckHint(const ThinkCandidate& candidate) {
    DuckThinkResult result;
    if (!litch_max::TryFormatUciMove(candidate.fromSquare, candidate.toSquare,
                         candidate.promotion, result.chessMove)) {
        return false;
    }
    result.duckDestination = candidate.duckDestination;
    result.capturedKing = candidate.capturedKing;
    result.score = candidate.score;
    litch_max::ParsedUciMove parsed;
    litch_max::DuckChessMoveResult applied;
    if (!ValidateDuckResult(m_cachedFen.CStr(), m_duckSquare,
                            result, parsed, applied)) {
        return false;
    }
    CaptureDisplayedDuckHintTurn();
    m_duckHintTurnValid = true;
    m_duckHintFen = m_cachedFen;
    m_duckHintSquare = m_duckSquare;
    m_duckHintTurn = result;
    ResetHintRetry();
    (void)ShowCachedDuckHint();
    return true;
}

bool GamePersistenceCoordinator::ApplyCompletedDuckSearchResult(
    const bool wasCpu,
    const bool stableIdle) {
    DuckThinkResult result;
    litch_max::ParsedUciMove parsed;
    litch_max::DuckChessMoveResult applied;
    if (!m_engine.GetLastDuckThinkResult(result) ||
        !ValidateDuckResult(m_cachedFen.CStr(), m_duckSquare,
                            result, parsed, applied)) {
        logger::Warn("duck search: invalid compound result");
        DispatchInteractionAdapterResult(
            runtime::AppEventType::SearchResult, "search", false);
        return false;
    }
    DispatchInteractionAdapterResult(
        runtime::AppEventType::SearchResult, "search", true);
    if (!wasCpu) {
        if (!HasCachedHintForCurrentPosition()) return false;
        ResetHintRetry();
        ThinkCandidate completed;
        completed.fromSquare = parsed.fromSquare;
        completed.toSquare = parsed.toSquare;
        completed.promotion = parsed.hasPromotion ? parsed.promotionSuffixLower : '\0';
        completed.isDuck = true;
        completed.duckDestination = result.duckDestination;
        completed.capturedKing = result.capturedKing;
        completed.score = result.score;
        (void)CacheLiveDuckHint(completed);
        m_hintCompletedDifficulty = m_hintThinkDifficulty;
        return true;
    }

    m_pendingCpuMoveUciValid = true;
    m_pendingCpuMoveFrom = parsed.fromSquare;
    m_pendingCpuMoveTo = parsed.toSquare;
    m_pendingCpuMoveCaptureSquareValid = applied.captureSquareValid;
    m_pendingCpuMoveCaptureSquare =
        applied.captureSquareValid ? applied.captureSquare : 0U;
    m_pendingCpuMoveUci = result.chessMove;
    m_pendingCpuDuckDestinationValid = !result.capturedKing;
    m_pendingCpuDuckDestination = result.duckDestination;
    QueuePendingDuckCpuMoveIfStable(stableIdle);
    (void)PersistSnapshotDeferredWithRetries(
        "duck_cpu_move_selected", false);
    return true;
}

void GamePersistenceCoordinator::QueuePendingDuckCpuMoveIfStable(
    const bool stableIdle) {
    if (!stableIdle || m_stateMachine.HasQueuedCpuMove()) {
        return;
    }
    litch_max::ParsedUciMove parsed;
    litch_max::DuckChessMoveResult applied;
    DuckThinkResult pending;
    pending.chessMove = m_pendingCpuMoveUci;
    pending.duckDestination = m_pendingCpuDuckDestination;
    pending.capturedKing = !m_pendingCpuDuckDestinationValid;
    if (!ValidateDuckResult(
            m_cachedFen.CStr(), m_duckSquare, pending,
            parsed, applied)) {
        return;
    }
    const char promotion = parsed.hasPromotion
                               ? parsed.promotionSuffixLower
                               : '\0';
    (void)QueueLiveCpuMove(
        parsed.fromSquare, parsed.toSquare,
        applied.captureSquareValid,
        applied.captureSquare, promotion);
}

bool GamePersistenceCoordinator::ShowCachedDuckHint() {
    if (!m_duckHintTurnValid ||
        !m_duckHintFen.Equals(m_cachedFen.View()) ||
        m_duckHintSquare != m_duckSquare) {
        return false;
    }
    litch_max::ParsedUciMove parsed;
    if (litch_max::TryParseUciMove(
            m_duckHintTurn.chessMove.CStr(), parsed) &&
        CanPublishHint(parsed.fromSquare, parsed.toSquare)) {
        m_stateMachine.SetHintMoveSquares(
            parsed.fromSquare, parsed.toSquare);
        if (!m_duckHintTurn.capturedKing &&
            m_duckHintTurn.duckDestination < 64U) {
            m_stateMachine.SetHintDuckMoveSquares(
                m_duckSquare, m_duckHintTurn.duckDestination);
        } else {
            m_stateMachine.ClearHintDuckMove();
        }
        RecordAcceptedHintDiagnostic();
    }
    return true;
}

bool GamePersistenceCoordinator::StartDuckSearchIfReady(
    const bool isCpuTurn,
    const uint32_t vsCpuLevel,
    const bool stableIdle,
    const bool cancelAllThinkingThisTick) {
    if (isCpuTurn && !IsCpuRetryReady()) {
        return false;
    }
    const bool shouldStart = stableIdle && !cancelAllThinkingThisTick &&
                             (isCpuTurn ? m_stateMachine.ConsumeCpuIdleTimeoutFired() : true);
    if (!shouldStart) {
        return false;
    }
    if (!isCpuTurn && !IsHintRetryReady()) {
        return false;
    }
    if (!isCpuTurn && NextHintDifficulty() == 0U) {
        return false;
    }
    if (UsesAsyncThinkWorker() && ThinkWorker().IsActive()) {
        return false;
    }

    ThinkConfig config = isCpuTurn
        ? MakeCpuThinkConfig(vsCpuLevel)
        : MakeHintThinkConfig();
    if (isCpuTurn) {
        BeginThinkDiagnostics(config, ThinkRequestKind::Cpu);
        config.candidateObserver = ThinkWorker().PrepareCandidateObserver();
        if (UsesAsyncThinkWorker()) config.progressObserver = ThinkWorker().PrepareProgressObserver();
    }
    if (!isCpuTurn) {
        BeginHintSearch(config);
        PrepareImmediateDecision(config, ThinkRequestKind::Hint);
        // Use the same position cache as Classic assistance. Duck legality is
        // a pure query, prepared before search admission rather than rendering.
        if (!m_cachedLegalDestMapValid ||
            !m_cachedLegalDestFen.Equals(m_cachedFen.View())) {
            if (!BuildCachedLegalDestinationsForCurrentPosition()) {
                ThinkWorker().RecordStartFailure();
                ScheduleHintRetry("duck_legal_cache");
                return false;
            }
        }
        // Pure legality preparation is part of the same bounded attempt.
        if (NextHintDifficulty() == 0U) {
            ThinkWorker().RecordStartFailure();
            m_hintPendingStart = false;
            m_stateMachine.SetHintFeedbackPending(false);
            return false;
        }
        config.maxThinkMs = MakeHintThinkConfig().maxThinkMs;
    }
    if (isCpuTurn) {
        PrepareImmediateDecision(config, ThinkRequestKind::Cpu);
        m_cpuRetryElapsed = fxe::ElapsedTime{};
    }
    ThinkWorker().RecordPreflight();
    if (!m_engine.StartDuckThink(
            m_cachedFen.CStr(), m_duckSquare, config)) {
        ThinkWorker().RecordStartFailure();
        logger::Warn("duck search: failed to start");
        (void)RecoverEngineAfterFailedThinkStart();
        if (isCpuTurn) {
            (void)RetainImmediateCpuDecision();
            m_cpuRetryElapsed.Reset();
        } else {
            ScheduleHintRetry("duck_hint_start");
        }
        return false;
    }
    if (UsesAsyncThinkWorker() && !ThinkWorker().Start(m_engine)) {
        (void)RecoverEngineAfterFailedThinkStart();
        if (isCpuTurn) {
            (void)RetainImmediateCpuDecision();
            m_cpuRetryElapsed.Reset();
        } else {
            ScheduleHintRetry("duck_hint_worker_start");
        }
        return false;
    }
    if (isCpuTurn) {
        BeginCpuThinking(config);
    } else {
        m_hintThinking = true;
        m_stateMachine.SetHintThinkingActive(true);
    }
    return true;
}

bool GamePersistenceCoordinator::UpdateDuckSearch(
    const uint32_t vsCpuLevel,
    const bool firstPlayerIsP2,
    const bool cpuPlayerIsP2,
    const bool cancelAllThinkingThisTick) {
    if (m_variantOutcome.IsTerminal()) {
        m_stateMachine.SetHintThinkingActive(false);
        m_stateMachine.SetHintFeedbackPending(false);
        m_stateMachine.SetHintDisplayRequested(false);
        m_stateMachine.ClearHintMove();
        return false;
    }

    const bool isCpuTurn = IsVsCpuTurn(
        m_engineDerived.sideToMove,
        vsCpuLevel,
        firstPlayerIsP2,
        cpuPlayerIsP2);
    m_stateMachine.SetHintFeedbackPending(!isCpuTurn &&
        NextHintDifficulty() != 0U);
    if (!isCpuTurn) { m_cpuRetryElapsed = fxe::ElapsedTime{}; }
    if (isCpuTurn) {
        m_stateMachine.SetHintDisplayRequested(false);
        m_stateMachine.ClearHintMove();
        // Keep the analyzed compound turn for this position: CPU eligibility
        // only hides it; a committed position change invalidates the cache.
    }
    if (isCpuTurn && m_hintThinking) {
        // Seat/level changes can hand this position to the CPU. Retire the
        // human search before consuming its idle token or entering the engine.
        if (!QuiesceAllThinking()) {
            m_cancelAllThinkingRequested = true;
            return true;
        }
    }
    const bool stableIdle = isCpuTurn ? IsSearchStableIdle()
                                     : IsHintSearchStableIdle();
    if (!isCpuTurn && (m_pendingCpuMoveUciValid ||
                       m_stateMachine.HasQueuedCpuMove())) {
        m_stateMachine.CancelQueuedCpuMove();
        ClearPendingCpuMoveSelection();
    }
    if (m_vsCpuThinking || m_hintThinking) {
        return PollDuckSearch(stableIdle);
    }
    if (isCpuTurn && m_pendingCpuMoveUciValid) {
        QueuePendingDuckCpuMoveIfStable(stableIdle);
        return true;
    }
    if (!isCpuTurn) {
        (void)ShowCachedDuckHint();
    }
    return StartDuckSearchIfReady(
        isCpuTurn, vsCpuLevel, stableIdle,
        cancelAllThinkingThisTick);
}

} // namespace modules
