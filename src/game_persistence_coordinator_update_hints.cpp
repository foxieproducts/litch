#include <cstdint>

#include "modules/game_persistence_coordinator.hpp"

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/coach_insight.hpp"
#include "modules/game_persistence_engine_helpers.hpp"
#include "modules/initial_fen.hpp"
#include "modules/vs_cpu_turn.hpp"
#include "trace/trace_recorder.hpp"

namespace modules {

namespace {

bool SafeGetFen(IEngine& engine, FenString& outFen) {
    return game_persistence_engine_helpers::SafeGetFen(
        engine, outFen, true);
}

} // namespace

void GamePersistenceCoordinator::SyncHintMoveForCurrentPosition(const bool hintEligible,
                                                                const bool isHumanTurn) {
    if (!hintEligible || !isHumanTurn) {
        m_hintPendingStart = false;
        ResetHintRetry();
        m_stateMachine.ClearHintMove();
        return;
    }

    uint8_t cachedFromSquare = 0;
    uint8_t cachedToSquare = 0;
    if (TryGetCachedHintMoveForCurrentPosition(cachedFromSquare, cachedToSquare)) {
        // Cached guidance is immediately useful. Refinements never retarget a
        // revealed move while its piece is held.
        if (!CanPublishHint(cachedFromSquare, cachedToSquare)) {
            m_hintPendingStart = false;
            return;
        }
        // The idempotent setter also consumes presentation-only suppression on
        // the first eligible resumed frame, even if the tuple did not change.
        m_stateMachine.SetHintMoveSquares(cachedFromSquare, cachedToSquare);
        m_hintPendingStart = !m_hintThinking && NextHintDifficulty() != 0U;
        return;
    }

    if (!m_hintThinking) {
        m_hintPendingStart = true;
    }

    uint8_t renderedFromSquare = 0;
    uint8_t renderedToSquare = 0;
    if (m_stateMachine.TryGetHintMoveSquares(renderedFromSquare, renderedToSquare)) {
        m_stateMachine.ClearHintMove();
    }
}

bool GamePersistenceCoordinator::UpdateHints(const uint32_t vsCpuLevel,
                                             const bool firstPlayerIsP2,
                                             const bool cpuPlayerIsP2,
                                             const bool cancelAllThinkingThisTick) {
    // One background search refines the actual best move throughout the
    // position's analysis window, retaining each completed candidate.
    // Initial-position analysis is useful only after the physical setup has
    // completed its stable Ready boundary. This also cancels a startup-race
    // search if the first Hall snapshot later reveals an incomplete board.
    const bool untouchedOpening = IsAnyInitialFen(m_cachedFen.CStr()) &&
                                  m_stateMachine.GetMovedSquaresMask() == 0ULL &&
                                  m_undoHistoryFieldCount == 0U;
    const auto& observedBoard =
        m_stateMachine.GetChessInteractionModel().board;
    if (untouchedOpening && observedBoard.valid) {
        if (observedBoard.occupiedMask ==
            m_stateMachine.GetExpectedOccupancyMask()) {
            m_hintInitialSetupObservedComplete = true;
        } else if (observedBoard.occupiedMask == 0ULL) {
            // Emptying an untouched board (including Litchcraft exit) starts
            // a new Setup journey; do not retain an earlier Hint-ready latch.
            m_hintInitialSetupObservedComplete = false;
        }
    }
    const bool observedIncompleteOpening =
        untouchedOpening && observedBoard.valid &&
        !m_hintInitialSetupObservedComplete;
    const bool hintEligible = !observedIncompleteOpening &&
                              !m_engineDerived.inCheckmate &&
                              !m_engineDerived.inDraw &&
                              !m_engineDerived.inStalemate;
    const bool isCpuTurn = modules::IsVsCpuTurn(
        m_engineDerived.sideToMove, vsCpuLevel, firstPlayerIsP2, cpuPlayerIsP2);
    const bool isHumanTurn = !isCpuTurn;
    // Retry backoff is still pending; the exhausted analysis window is not.
    m_stateMachine.SetHintFeedbackPending(hintEligible && isHumanTurn &&
        NextHintDifficulty() != 0U);
    if (!isHumanTurn) {
        // Hide human guidance before cancellation can wait for worker cleanup.
        // Retain the position cache and manual request latch.
        m_stateMachine.ClearHintMove();
    }
    const auto selectAndCacheHint = [&](
                                        const UciMove* engineBest,
                                        const ClassicThinkResult* classicResult) {
        litch_max::ParsedUciMove selection;
        if (engineBest == nullptr ||
            !litch_max::TryParseUciMove(engineBest->CStr(), selection) ||
            (m_cachedLegalComputedFromSquaresMask64 &
             (1ULL << selection.fromSquare)) == 0ULL ||
            (m_cachedLegalDestByFromSquare[selection.fromSquare] &
             (1ULL << selection.toSquare)) == 0ULL) {
            return false;
        }
        CacheHintMoveForCurrentPosition(selection.fromSquare, selection.toSquare);
        if (CanPublishHint(selection.fromSquare, selection.toSquare)) {
            m_stateMachine.SetHintMoveSquares(selection.fromSquare, selection.toSquare);
            RecordAcceptedHintDiagnostic();
        }
        if (classicResult != nullptr && classicResult->source != ThinkCandidateSource::Seed &&
            classicResult->candidateCount > 0U) {
            litch_max::ParsedUciMove parsedBest;
            const ClassicThinkCandidate& analyzed =
                classicResult->candidates[0];
            if (litch_max::TryParseUciMove(engineBest->CStr(), parsedBest) &&
                parsedBest.fromSquare == analyzed.fromSquare &&
                parsedBest.toSquare == analyzed.toSquare &&
                (parsedBest.hasPromotion
                     ? parsedBest.promotionSuffixLower
                     : '\0') == analyzed.promotion) {
                CoachInsight insight;
                if (TryBuildCoachInsight(
                        m_cachedFen.CStr(), m_engineDerived.sideToMove,
                        m_engineDerived.inCheck, *classicResult, insight)) {
                    m_currentCoachInsight = insight;
                    m_currentCoachInsightValid = true;
                }
            }
        }
        return true;
    };
    const bool sideChanged = !m_hintLastSideToMoveValid || (m_hintLastSideToMove != m_engineDerived.sideToMove);
    if (sideChanged) {
        m_hintLastSideToMoveValid = true;
        m_hintLastSideToMove = m_engineDerived.sideToMove;
        m_hintPendingStart = hintEligible && isHumanTurn;
        ResetHintRetry();

        ClearCachedLegalDestinations();

        // New turn: clear stale hint output and restart the pipeline.
        m_stateMachine.ClearHintMove();

        if (m_hintThinking) {
            if (!QuiesceAllThinking()) {
                m_cancelAllThinkingRequested = true;
                return true;
            }
        }
    }

    // Cancel hint analysis if it is no longer applicable.
    if (m_hintThinking && (!hintEligible || !isHumanTurn || m_promotionPending)) {
        if (!QuiesceAllThinking()) {
            m_cancelAllThinkingRequested = true;
            return true;
        }
        ClearCachedLegalDestinations();
        ResetHintRetry();
    }

    // Take completed refinements before Sync can reveal a cached move.
    // Once revealed, the existing physical target remains frozen until Idle.
    ConsumeLiveHintCandidate();
    SyncHintMoveForCurrentPosition(hintEligible, isHumanTurn);

    // Continue an owned search below in any maintained phase, but a physical
    // plan never starts a new hint search or rebuilds its engine legality cache.
    const bool hintStateOk = IsHintSearchStableIdle();

    if (m_hintPendingStart && NextHintDifficulty() != 0U && IsHintRetryReady() && !m_hintThinking && !m_vsCpuThinking && !m_promotionPending &&
        hintStateOk && !m_stateMachine.IsPlacementModeActive() && !m_stateMachine.IsTextOverlayActive() &&
        !m_stateMachine.IsKingQueenSwapPromptActive()) {
        if (cancelAllThinkingThisTick) {
            return true;
        }
        if (UsesAsyncThinkWorker() && ThinkWorker().IsActive()) {
            ScheduleHintRetry("worker_cancel_pending");
            return true;
        }
        // If the engine was mutated externally, allow the normal external-mutation
        // detection below to run before starting any hint analysis.
        FenString nowFen;
        const bool fenOk = SafeGetFen(m_engine, nowFen);
        const bool externalMutationPending = fenOk && !m_cachedFen.Equals(nowFen.View());
        if (!externalMutationPending) {
            // Anchor the position window on its first admitted attempt. A
            // failed legality preflight must not leave requested feedback and
            // retries running forever; BeginHintSearch preserves the anchor
            // on same-position retries and retains completed cached guidance.
            ThinkConfig cfg = MakeHintThinkConfig();
            BeginHintSearch(cfg);
            PrepareImmediateDecision(cfg, ThinkRequestKind::Hint);
            // Precompute per-piece legality while the engine is idle so
            // destination highlights remain responsive during hint analysis.
            if (!BuildCachedLegalDestinationsForCurrentPosition()) {
                ThinkWorker().RecordStartFailure();
                ScheduleHintRetry("legal_cache");
                return true;
            }
            // Preparation spends the same position window. Re-read the
            // remaining allowance immediately before mutable search starts.
            if (NextHintDifficulty() == 0U) {
                ThinkWorker().RecordStartFailure();
                m_hintPendingStart = false;
                m_stateMachine.SetHintFeedbackPending(false);
                return true;
            }
            cfg.maxThinkMs = MakeHintThinkConfig().maxThinkMs;
            ThinkWorker().RecordPreflight();
            if (m_engine.StartThink(cfg)) {
                bool asyncOk = true;
                if (UsesAsyncThinkWorker()) {
                    asyncOk = ThinkWorker().Start(m_engine);
                }
                if (!asyncOk) {
                    (void)RecoverEngineAfterFailedThinkStart();
                    ScheduleHintRetry("worker_start");
                    ClearCachedLegalDestinations();
                    return true;
                } else {
                    m_hintThinking = true;
                    // Hidden background analysis stays quiet. An explicit Hint
                    // request owns the control cue through search completion.
                    m_stateMachine.SetHintThinkingActive(true);
                    m_hintPendingStart = false;
                }
            } else {
                ThinkWorker().RecordStartFailure();
                (void)RecoverEngineAfterFailedThinkStart();
                ScheduleHintRetry("engine_start");
                ClearCachedLegalDestinations();
                return true;
            }
        }
    }

    if (m_hintThinking) {
        ConsumeLiveHintCandidate();
        if (HasCachedHintForCurrentPosition() && !hintStateOk) {
            // The single worker retains its completed mailbox. Do not replace
            // the selected compound suggestion during pickup/physical progress.
            // A committing action can still quiesce and discard this refinement.
            return true;
        }
        uint8_t progress = 0;
        UciMove best;
        uint32_t elapsedMs = 0;
        uint32_t nodeCount = 0;
        ThinkState st = ThinkState::Thinking;
        if (UsesAsyncThinkWorker()) {
            if (ThinkWorker().IsDone() &&
                !ThinkWorker().IsActive()) {
                st = ThinkWorker().GetResult(
                    progress, best, elapsedMs, nodeCount);
            }
        } else {
            const fxe::ElapsedTime pollElapsed =
                fxe::ElapsedTime::StartNow();
            ThinkWorker().RecordSynchronousPollStarted();
            st = m_engine.PollThinkProgress(progress, best);
            // Native synchronous searches publish into the same immutable
            // mailbox during Poll. Consume that completed work before an
            // aborted/deadline result can end coordinator ownership.
            ConsumeLiveHintCandidate();
            elapsedMs = pollElapsed.ElapsedMs();

            ThinkStats stats;
            const bool statsValid = m_engine.GetLastThinkStats(stats);
            if (statsValid) {
                nodeCount = stats.nodeCount;
                if (stats.calculationMsValid) elapsedMs = stats.calculationMs;
            }
            ThinkWorker().RecordSynchronousResult(st, nodeCount, statsValid);
        }
        RefreshThinkDiagnostics();

        const bool completed = st == ThinkState::Done;
        const bool aborted = st == ThinkState::Canceled || st == ThinkState::Error || st == ThinkState::Idle;
        if (completed || aborted) {
            DispatchInteractionAdapterResult(
                runtime::AppEventType::SearchResult,
                "search",
                completed);
        }
        if (completed || aborted) {
            RecordHintSearchStats(elapsedMs, nodeCount);
            ClassicThinkResult classicResult;
            const bool classicResultValid =
                completed &&
                m_engine.GetLastClassicThinkResult(classicResult);
            m_engine.CancelThink();

            if (completed) {
                m_lastHintThinkStatsValid = true;
                m_lastHintThinkElapsedMs = elapsedMs;
                m_lastHintThinkNodeCount = nodeCount;
                m_lastHintThinkDifficulty = m_hintThinkDifficulty;
                m_lastHintThinkBestUci = best;
            } else {
                logger::Warn("hint: search stopped without final result (state=%u)",
                             static_cast<unsigned>(st));
            }

            // A terminal result may improve the cached move; failure leaves
            // any useful live publication intact. A merely legal move is not
            // evidence that the engine calculated it as best.
            // Only the calculation owner's qualified mailbox may establish the
            // first result. A terminal legal fallback cannot bypass its budget.
            const bool selected = completed && HasCachedHintForCurrentPosition() && selectAndCacheHint(
                &best, classicResultValid ? &classicResult : nullptr);
            if (completed && (selected || HasCachedHintForCurrentPosition())) {
                m_hintCompletedDifficulty = m_hintThinkDifficulty;
                ResetHintRetry();
            } else {
                ScheduleHintRetry("no_completed_hint_result");
            }

            m_hintThinking = false;
            m_stateMachine.SetHintThinkingActive(false);
            // Legal moves belong to this position, not the search lifetime.
            // Capture-first assistance still needs the immutable map after
            // completion or the analysis deadline. Position changes own its
            // existing invalidation boundaries.
            return true;
        }

        if (UsesAsyncThinkWorker()) {
            // Async think still running; avoid touching the engine until done.
            return true;
        }
    }

    return false;
}

void GamePersistenceCoordinator::PollExternalEngineMutationAndApply() {
    // While thinking is active, avoid expensive polling for external engine
    // mutations (and avoid calling GetFen while the engine may be busy).
    //
    // Important: use both coordinator-owned and state-machine flags. The state
    // machine can briefly outlive coordinator state during async completion/
    // cancellation races.
    const bool thinkingActive = m_vsCpuThinking || m_hintThinking || m_ponderThinking ||
                                m_stateMachine.IsCpuThinkingActive() ||
                                m_stateMachine.IsHintThinkingActive() ||
                                (UsesAsyncThinkWorker() &&
                                 ThinkWorker().IsActive());
    if (thinkingActive) {
        return;
    }

    FenString nowFen;
    if (!SafeGetFen(m_engine, nowFen)) {
        return;
    }

    if (IsCurrentEngineFenEquivalent(nowFen)) {
        return;
    }

    // External engine mutation detected.
    const bool replacesClockTerminal = HasClockTerminalOutcome();
    m_cachedFen = nowFen;
    m_gameVariant = GameVariant::Classic;
    m_duckSquare = INVALID_DUCK_SQUARE;
    m_duckTurnPhase = DuckTurnPhase::ChessMove;
    m_variantOutcome = VariantOutcome{};
    m_pendingDuckTurn = PendingDuckChessTurn{};
    m_stagedDuckChessUndoPending = false;
    m_externalizedDuckChessActionPending = false;
    m_externalizedDuckRelocationActionPending = false;
    m_externalizedDuckUndoActionPending = false;
    m_externalizedDuckRedoActionPending = false;
    m_externalizedDuckFixedActionValid = false;
    m_externalizedDuckRedoStartPending = false;
    m_externalizedDuckFrom = INVALID_DUCK_SQUARE;
    m_externalizedDuckTo = INVALID_DUCK_SQUARE;
    m_duckUndoRelocationPending = false;
    m_duckRedoRelocationPending = false;
    m_duckUndoRequestedCount = 0U;
    m_variantDetectionArmed = false;
    ClearCachedHintResults();

    // External mutation invalidates any queued CPU move and cancels thinking.
    ClearPendingCpuMoveSelection();
    if (m_vsCpuThinking) {
        if (!QuiesceAllThinking()) {
            m_cancelAllThinkingRequested = true;
            return;
        }
    }

    m_stateMachine.ResetForExternalEngineMutation();
    m_stateMachine.ClearMovedSquares();

    ClearUndoHistory();
    ClearFenHistory();
    (void)BeginNewGameHistory(true);

    // External mutation is treated as a fresh position; reset draw tracking.
    m_halfmoveClockPlies = 0;

    m_undoBaseFen = m_cachedFen;
    m_undoBaseMovedSquaresMask = 0;

    SeedFenHistoryFromCurrentFen();

    ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());

    if (m_traceRecorder && m_traceRecorder->IsActive()) {
        m_traceRecorder->RecordCheckpointEngine(m_cachedFen.CStr());
    }
    (void)m_stateMachine.TryEnterPlacementMode();

    m_engineDerived.RefreshFromEngine(m_engine);
    RefreshDrawStateFromTracking();

    // External mutation normally preserves remaining time and applies no
    // increment. A timed-out game is a completed game, so replacing it must
    // deliberately reset the terminal clock just like LoadExternalFen().
    if (replacesClockTerminal) {
        ClearClockTerminalOutcome();
        m_cachedTimerSetting = ReadTimerSetting();
        m_gameClock.ResetFromTimerSetting(
            m_cachedTimerSetting, ToClockSide(m_engineDerived.sideToMove));
    } else {
        m_gameClock.SwitchActiveSideWithoutIncrement(
            ToClockSide(m_engineDerived.sideToMove));
    }

    (void)PersistSnapshotDeferredWithRetries("external_fen_change", true);
    RecordVariantTraceCheckpoint();
}

} // namespace modules
