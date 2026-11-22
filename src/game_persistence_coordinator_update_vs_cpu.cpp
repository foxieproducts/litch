#include <cstdint>
#include <cstring>

#include "foxie_embedded/elapsed_time.hpp"
#include "logger.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/game_persistence_coordinator.hpp"
#include "modules/game_persistence_coordinator_update_internal.hpp"
#include "modules/game_persistence_coordinator_vs_cpu_internal.hpp"
#include "modules/vs_cpu_turn.hpp"

namespace modules {

namespace vs_cpu = game_persistence_vs_cpu_internal;


bool GamePersistenceCoordinator::UpdateVsCpu(const uint32_t vsCpuLevel,
                                             const bool firstPlayerIsP2,
                                             const bool cpuPlayerIsP2,
                                             const bool allowVsCpuThinkStart,
                                             const bool vsCpuThinkMaintainOk,
                                             const bool cancelAllThinkingThisTick) {
    // VS-CPU relies on the physical occupancy matching the engine position
    // model. If sensors weren't yet stable at the moment a new game/external
    // FEN was applied, the one-shot placement-mode entry can fail and the
    // mismatch only becomes obvious later (often on the CPU turn).
    //
    // Retry entering placement mode here once sensors are stable.
    if ((vsCpuLevel != 0U) &&
        !m_stateMachine.IsUndoPromptActive() &&
        !m_stateMachine.IsExpectedOccupancyMatched() &&
        !m_stateMachine.IsPlacementModeActive()) {
        // Capture-first sequencing intentionally creates a temporary
        // expected/physical mismatch. Do not treat that as a resync condition.
        if (!m_stateMachine.HasCaptureFirstStaging() && !m_stateMachine.HasCaptureReturn()) {
            if (m_stateMachine.TryEnterPlacementMode()) {
                logger::Trace("placement: enter (expected/physical mismatch)");
                return true;
            }
        }
    }

    const bool isVsCpuTurn = modules::IsVsCpuTurn(
        m_engineDerived.sideToMove, vsCpuLevel, firstPlayerIsP2, cpuPlayerIsP2);
    if (isVsCpuTurn) {
        // A completed human Hint has no worker to retire. Hide it before the
        // CPU search/prompt paths return early, retaining its position cache
        // and the existing manual request semantics.
        m_stateMachine.SetHintFeedbackPending(false);
        m_stateMachine.ClearHintMove();
    }
    if (!isVsCpuTurn && (m_stateMachine.HasQueuedCpuMove() || m_pendingCpuMoveUciValid)) {
        logger::Trace("cpu: discard queued move (turn changed)");
        m_stateMachine.CancelQueuedCpuMove();
        ClearPendingCpuMoveSelection();
        (void)PersistSnapshotDeferredWithRetries("cpu_move_discarded_turn_change", false);
        return true;
    }

    // VsCPU: after idle delay, start thinking for the CPU side and queue the
    // resulting move as a physical CpuMoving prompt.
    if (!isVsCpuTurn) {
        m_cpuRetryElapsed = fxe::ElapsedTime{};
        return false;
    }

    // If we already have a completed move, queue it as soon as the board is stable again.
    if (allowVsCpuThinkStart && !m_stateMachine.HasQueuedCpuMove() && m_pendingCpuMoveUciValid) {
        uint8_t captureSquare = m_pendingCpuMoveTo;
        bool isCapture = false;
        if (!game_persistence_update_internal::TryComputeCpuCaptureSquareAndIsCapture(m_cachedFen.CStr(),
                                                                                      m_pendingCpuMoveFrom,
                                                                                      m_pendingCpuMoveTo,
                                                                                      captureSquare,
                                                                                      isCapture)) {
            captureSquare = m_pendingCpuMoveTo;
            isCapture = false;
        }

        const uint8_t promoSuffix = game_persistence_update_internal::TryGetPromotionSuffixFromUci(m_pendingCpuMoveUci.CStr());
        const char promoSuffixChar = promoSuffix ? static_cast<char>(promoSuffix) : '\0';

        if (QueueLiveCpuMove(m_pendingCpuMoveFrom,
                             m_pendingCpuMoveTo,
                             isCapture,
                             captureSquare,
                             promoSuffixChar)) {
            (void)PersistSnapshotDeferredWithRetries("cpu_move_queued", false);
        }
        return true;
    }

    if (allowVsCpuThinkStart && !m_stateMachine.HasQueuedCpuMove()) {
        if (!m_vsCpuThinking) {
            // A settings change can give the current human turn to the CPU.
            // Let UpdateHints retire its now-ineligible owner first; do not
            // consume the CPU idle token or touch engine legality/search yet.
            if (m_hintThinking ||
                (UsesAsyncThinkWorker() && ThinkWorker().IsActive())) {
                return false;
            }
            if (cancelAllThinkingThisTick) {
                return true;
            }
            // The idle token refires while idle; it cannot throttle a failed
            // search. Keep failure recovery bounded independently of board input.
            if (!IsCpuRetryReady()) {
                return true;
            }
            if (m_stateMachine.ConsumeCpuIdleTimeoutFired()) {
                m_cpuRetryElapsed = fxe::ElapsedTime{};
                ThinkConfig cfg = MakeCpuThinkConfig(vsCpuLevel);
                if (TryUsePonder(vsCpuLevel, cfg)) return true;
                BeginThinkDiagnostics(cfg, ThinkRequestKind::Cpu);
                cfg.candidateObserver = ThinkWorker().PrepareCandidateObserver();
                if (UsesAsyncThinkWorker()) {
                    cfg.progressObserver = ThinkWorker().PrepareProgressObserver();
                }
                PrepareImmediateDecision(cfg, ThinkRequestKind::Cpu);
                ThinkWorker().RecordPreflight();

                logger::Trace("cpu: think cfg (lvl=%u maxNodes=%u maxDepth=%u seed=%u)",
                              static_cast<unsigned>(cfg.difficulty),
                              static_cast<unsigned>(cfg.maxNodes),
                              static_cast<unsigned>(cfg.maxDepth),
                              static_cast<unsigned>(cfg.rngSeed));

                if (m_engine.StartThink(cfg)) {
                    bool asyncOk = true;
                    if (UsesAsyncThinkWorker()) {
                        asyncOk = ThinkWorker().Start(m_engine);
                    }
                    if (!asyncOk) {
                        (void)RecoverEngineAfterFailedThinkStart();
                        logger::Warn("cpu: think worker start failed");
                        (void)RetainImmediateCpuDecision();
                        m_cpuRetryElapsed.Reset();
                    } else {
                        logger::Trace("cpu: think start (level=%u)",
                                     static_cast<unsigned>(vsCpuLevel));
                        BeginCpuThinking(cfg);
                    }
                } else {
                    ThinkWorker().RecordStartFailure();
                    (void)RecoverEngineAfterFailedThinkStart();
                    logger::Warn("cpu: think start failed");
                    (void)RetainImmediateCpuDecision();
                    m_cpuRetryElapsed.Reset();
                }
            }
        }
    }

    // Poll an in-flight think regardless of transient mismatch/illegal.
    if (m_vsCpuThinking && !m_stateMachine.HasQueuedCpuMove() && vsCpuThinkMaintainOk) {
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
            elapsedMs = pollElapsed.ElapsedMs();

            ThinkStats stats;
            const bool statsValid = m_engine.GetLastThinkStats(stats);
            if (statsValid) nodeCount = stats.nodeCount;
            ThinkWorker().RecordSynchronousResult(st, nodeCount, statsValid);
        }
        RefreshThinkDiagnostics();
        if (st == ThinkState::Done) {
            m_cpuThinkingEstimate.Clear();
            const uint32_t nps = (elapsedMs == 0U) ? 0U
                                                   : static_cast<uint32_t>((static_cast<uint64_t>(nodeCount) * 1000U) /
                                                                           static_cast<uint64_t>(elapsedMs));
            logger::Trace("cpu: think done (best=%s ms=%u nodes=%u nps=%u)",
                         best.CStr(),
                         static_cast<unsigned>(elapsedMs),
                         static_cast<unsigned>(nodeCount),
                         static_cast<unsigned>(nps));

            litch_max::ParsedUciMove chosenParsed;
            UciMove chosenUci;

            const bool haveChosen = vs_cpu::TryChooseClassicCpuMove(
                m_engine, best, chosenParsed, chosenUci);
            if (haveChosen && !chosenUci.Equals(best.View())) {
                logger::Trace("cpu: blunder (level=%u best=%s chosen=%s)",
                             static_cast<unsigned>(vsCpuLevel), best.CStr(), chosenUci.CStr());
            }

            if (!haveChosen) {
                (void)RetainImmediateCpuDecision();
                logger::Warn("cpu: no valid searched move; retain immediate decision");
                m_cpuRetryElapsed.Reset();
            }

            if (haveChosen) {
                uint8_t captureSquare = chosenParsed.toSquare;
                bool isCapture = false;
                if (!game_persistence_update_internal::TryComputeCpuCaptureSquareAndIsCapture(m_cachedFen.CStr(),
                                                                                              chosenParsed.fromSquare,
                                                                                              chosenParsed.toSquare,
                                                                                              captureSquare,
                                                                                              isCapture)) {
                    captureSquare = chosenParsed.toSquare;
                    isCapture = false;
                }

                m_pendingCpuMoveUciValid = true;
                m_pendingCpuMoveFrom = chosenParsed.fromSquare;
                m_pendingCpuMoveTo = chosenParsed.toSquare;
                m_pendingCpuMoveCaptureSquareValid = isCapture;
                m_pendingCpuMoveCaptureSquare = isCapture ? captureSquare : 0U;
                m_pendingCpuMoveUci = chosenUci;

                if (allowVsCpuThinkStart) {
                    const char promoSuffixChar = chosenParsed.hasPromotion ? chosenParsed.promotionSuffixLower : '\0';
                    if (QueueLiveCpuMove(chosenParsed.fromSquare,
                                         chosenParsed.toSquare,
                                         isCapture,
                                         captureSquare,
                                         promoSuffixChar)) {
                        (void)PersistSnapshotDeferredWithRetries("cpu_move_queued", false);
                    }
                } else {
                    (void)PersistSnapshotDeferredWithRetries("cpu_move_selected", false);
                }
            }

            // Ensure the engine exits think mode cleanly before the next tick.
            m_engine.CancelThink();
            m_vsCpuThinking = false;
            m_stateMachine.SetCpuThinkingActive(false);
            return true;
        }

        if (st == ThinkState::Canceled || st == ThinkState::Error || st == ThinkState::Idle) {
            m_cpuThinkingEstimate.Clear();
            logger::Warn("cpu: think aborted (state=%u)", static_cast<unsigned>(st));
            m_cpuRetryElapsed.Reset();
            m_engine.CancelThink();
            if (st != ThinkState::Canceled && !m_cancelCpuWorkRequested && !m_cancelAllThinkingRequested) {
                (void)RetainImmediateCpuDecision();
            }
            m_vsCpuThinking = false;
            m_stateMachine.SetCpuThinkingActive(false);
            return true;
        }

        if (UsesAsyncThinkWorker()) {
            // Async think still running; avoid touching the engine until done.
            return true;
        }
    }

    return false;
}
} // namespace modules
