#include <cstddef>
#include <cstdint>
#include <cstring>

#include "modules/game_persistence_coordinator.hpp"

#include "foxie_embedded/elapsed_time.hpp"
#include "logger.hpp"
#include "modules/draw_rules.hpp"
#include "modules/game_persistence_coordinator_update_internal.hpp"
#include "modules/game_persistence_engine_helpers.hpp"
#include "modules/persisted_side.hpp"
#include "modules/promotion_mode_setting.hpp"
#include "modules/vs_cpu_rng.hpp"
#include "trace/trace_recorder.hpp"

namespace modules {

namespace {

constexpr bool IsGameOver(const EngineDerivedState& d) {
    return d.inCheckmate || d.inDraw || d.inStalemate;
}

bool SafeGetFen(IEngine& engine, FenString& outFen) {
    return game_persistence_engine_helpers::SafeGetFen(
        engine, outFen, true);
}

} // namespace

void GamePersistenceCoordinator::ConsumeCommittedMoves() {
    runtime::PhysicalTransitionPlan pendingPlan;
    bool havePendingPlan =
        !m_promotionPending &&
        m_stateMachine.TryClaimPendingInteractionMove(pendingPlan);
    const PendingUndoCommand undoCommand = m_pendingUndoCommand;
    m_pendingUndoCommand = PendingUndoCommand{};
    bool haveUndoCommand = undoCommand.kind != UndoCommandKind::None;
    while (havePendingPlan || haveUndoCommand) {
        const bool interactionMove = havePendingPlan;
        if (interactionMove && pendingPlan.kind != runtime::PhysicalTransitionKind::Promotion) {
            m_restoredPromotionRequiresChoice = false;
        }
        havePendingPlan = false;
        haveUndoCommand = false;
        const bool isUndo = !interactionMove &&
                            undoCommand.kind == UndoCommandKind::Undo;
        const bool isCancelUndo = !interactionMove &&
                                  undoCommand.kind == UndoCommandKind::CancelUndo;
        const bool isGraceTakebackCommand =
            !interactionMove &&
            undoCommand.origin == UndoCommandOrigin::GraceTakeback;
        const uint8_t moveFrom = interactionMove ? pendingPlan.moveFrom : undoCommand.from;
        const uint8_t moveTo = interactionMove ? pendingPlan.moveTo : undoCommand.to;
        const bool captureSquareValid =
            interactionMove && pendingPlan.removalRequired;
        const uint8_t captureSquare = captureSquareValid
                                          ? pendingPlan.removalSquare
                                          : 0U;

        if (interactionMove &&
            pendingPlan.kind ==
                runtime::PhysicalTransitionKind::DuckRelocation) {
            if (m_duckUndoRelocationPending) {
                HandleDuckUndoRelocationAction(pendingPlan);
                continue;
            }
            if (m_duckRedoRelocationPending) {
                HandleDuckRedoRelocationAction(pendingPlan);
                continue;
            }
            (void)CommitDuckRelocation(pendingPlan);
            continue;
        }

        // A combined Hall lift/placement can reach commitment before pickup
        // guidance. A finished game must reject that move here as well.
        if (interactionMove && IsGameOverVariantAware()) {
            DispatchPendingInteractionActionResult(false);
            ClearLegalDestinationsMask();
            ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
            continue;
        }

        if (isUndo) {
            CaptureHintForCancelUndoRestore();
        }

        const bool preserveDuckHintDisplay =
            interactionMove && IsDuckGame() &&
            pendingPlan.source == runtime::PhysicalTransitionSource::Human &&
            m_stateMachine.IsHintDisplayRequested();
        if (preserveDuckHintDisplay) CaptureDisplayedDuckHintTurn();
        // Best-move hint: do not cancel on pickup/placement; only cancel when a
        // move/undo is actually being committed (engine mutation is imminent).
        if (m_hintThinking) {
            QuiesceAllThinking();
            ClearCachedLegalDestinations();
        } else if (m_stateMachine.IsHintThinkingActive()) {
            // Defensive: keep UI state consistent if an async completion races.
            m_stateMachine.SetHintThinkingActive(false);
            ClearCachedLegalDestinations();
        }
        m_stateMachine.SetHintDisplayRequested(preserveDuckHintDisplay);
        if (!preserveDuckHintDisplay) {
            m_stateMachine.ClearHintMove();
        }

        // Emit a human-readable move line with piece + capture details (best effort).
        // Use cached FEN as the authoritative pre-move state.
        if (interactionMove) {
            const bool isPendingCpuMove =
                m_pendingCpuMoveUciValid &&
                m_pendingCpuMoveFrom == moveFrom &&
                m_pendingCpuMoveTo == moveTo;
            LogMoveSummary(isPendingCpuMove ? "cpu" : "human",
                           m_cachedFen.CStr(), moveFrom, moveTo);
        } else if (isUndo) {
            logger::Trace("move: undo");
        } else if (isCancelUndo) {
            logger::Trace("move: cancel_undo");
        }

        if (interactionMove) {
            // Any finalized move invalidates cancel-undo (state machine enforces this too).
            m_cancelUndoHistoryEntryValid = false;
            std::memset(m_cancelUndoHistoryEntry, 0, sizeof(m_cancelUndoHistoryEntry));

            const bool isPendingCpuMove =
                m_pendingCpuMoveUciValid &&
                m_pendingCpuMoveFrom == moveFrom &&
                m_pendingCpuMoveTo == moveTo;

            // Any finalized move that doesn't match the queued CPU move should
            // invalidate the pending CPU UCI (e.g., external mutation or
            // cancellation flows).
            if (m_pendingCpuMoveUciValid && !isPendingCpuMove) {
                ClearPendingCpuMoveSelection();
            }

            if (isPendingCpuMove) {
                // CPU move: apply engine-selected UCI directly (including
                // promotions) and do not enter the human promotion modal.
                logger::Trace("cpu: apply %s", m_pendingCpuMoveUci.CStr());

                const uint8_t promoSuffix =
                    game_persistence_update_internal::
                        TryGetPromotionSuffixFromUci(
                            m_pendingCpuMoveUci.CStr());
                if (IsDuckGame()) {
                    // A Duck CPU choice is a compound turn. The ordinary
                    // physical chess plan is only its first choreography;
                    // stage that action and expose the engine-selected fixed
                    // duck relocation before committing engine/history/clock.
                    const bool staged = StageDuckChessMove(
                        pendingPlan, static_cast<char>(promoSuffix));
                    if (!staged) {
                        ClearPendingCpuMoveSelection();
                        (void)SafeGetFen(m_engine, m_cachedFen);
                        ApplyEngineFenToStateMachineExpected(
                            m_cachedFen.CStr());
                        m_engineDerived.RefreshFromEngine(m_engine);
                        RefreshDrawStateFromTracking();
                    }
                    continue;
                }

                const uint32_t nextHalfmoveClock = modules::NextHalfmoveClockPlies(
                    m_cachedFen.CStr(), m_halfmoveClockPlies,
                    moveFrom, moveTo, captureSquareValid, captureSquare);

                // Append undo persistence entry (including promotion suffix if present).
                uint8_t fields[UNDO_HISTORY_FIELDS_PER_MOVE] = {};
                game_persistence_update_internal::BuildUndoPersistFieldsFromFen(
                    m_cachedFen.CStr(),
                    m_engine.GetSideToMove(),
                    moveFrom,
                    moveTo,
                    captureSquareValid,
                    captureSquare,
                    promoSuffix,
                    fields);
                if (!m_engine.ApplyUciMove(m_pendingCpuMoveUci.CStr())) {
                    logger::Warn("engine: ApplyUciMove failed (%s) for queued CPU move",
                                 m_pendingCpuMoveUci.CStr());
                    ClearPendingCpuMoveSelection();
                    DispatchPendingInteractionActionResult(false);
                    (void)SafeGetFen(m_engine, m_cachedFen);
                    ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
                    m_engineDerived.RefreshFromEngine(m_engine);
                    RefreshDrawStateFromTracking();
                    continue;
                }

                (void)AppendUndoHistoryEntry(fields[0], fields[1], fields[2],
                                             fields[3], fields[4], fields[5],
                                             fields[6]);
                m_halfmoveClockPlies = nextHalfmoveClock;
                m_vsCpuRngSeed = NextVsCpuSeedAfterMove(m_vsCpuRngSeed, m_pendingCpuMoveUci.CStr());
                ClearPendingCpuMoveSelection();
                DispatchPendingInteractionActionResult(true);
            } else {
                // Detect pawn promotions from the current engine FEN (before applying move).
                // This keeps promotion choice engine-driven and deterministic.
                char fromPiece = '\0';
                if (TryGetFenPieceAtSquare(m_cachedFen.CStr(), moveFrom, fromPiece)) {
                    const uint8_t toRank = static_cast<uint8_t>(moveTo / 8);
                    const bool isWhitePawnPromo = (fromPiece == 'P') && (toRank == 0);
                    const bool isBlackPawnPromo = (fromPiece == 'p') && (toRank == 7);
                    if (isWhitePawnPromo || isBlackPawnPromo) {
                        const bool choose = m_restoredPromotionRequiresChoice ||
                            promotion_mode_setting::RequiresChoice(m_settings.ReadValue(
                                promotion_mode_setting::KEY,
                                promotion_mode_setting::DEFAULT_VALUE));
                        if (!choose) {
                            (void)CommitPromotionMove(
                                pendingPlan, PromotionPiece::Queen, /*automatic=*/true);
                            return;
                        }
                        if (m_stateMachine.StartPromotion()) {
                            m_restoredPromotionRequiresChoice = false;
                            m_promotionPending = true;
                            (void)PersistSnapshotDeferredWithRetries(
                                "promotion_pending", false);
                            return;
                        }

                        // Promotion modal failed to start; no durable move state
                        // has changed yet.
                        m_lastUndoEntryPromotionPending = false;
                    }
                }

                if (!ApplyHumanMoveTransactional(pendingPlan)) {
                    if (preserveDuckHintDisplay) m_stateMachine.ClearHintMove();
                    (void)SafeGetFen(m_engine, m_cachedFen);
                    ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
                    m_engineDerived.RefreshFromEngine(m_engine);
                    RefreshDrawStateFromTracking();
                    continue;
                }
                if (IsDuckGame()) {
                    // Duck staging owns the full combined-turn transaction.
                    // The engine, history, clock, and persistence commit only
                    // after the mandatory relocation (or king capture).
                    continue;
                }
            }

            (void)SafeGetFen(m_engine, m_cachedFen);
            (void)PushFenHistory(m_cachedFen.CStr(), m_halfmoveClockPlies);
            ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());

            // Update render-facing last-move pulse state from the authoritative
            // undo history we just appended.
            UpdateLastMovePulseFromUndoHistory();

            logger::Trace("fen: %s", m_cachedFen.CStr());

            if (m_traceRecorder && m_traceRecorder->IsActive()) {
                m_traceRecorder->RecordCheckpointEngine(m_cachedFen.CStr());
            }
            RecordVariantTraceCheckpoint();

            m_engineDerived.RefreshFromEngine(m_engine);
            RefreshDrawStateFromTracking();

            // If the move ended the game, stop any queued/active VS-CPU work.
            if (IsGameOver(m_engineDerived)) {
                ClearPendingCpuMoveSelection();
                if (m_vsCpuThinking) {
                    QuiesceAllThinking();
                }
                m_stateMachine.CancelQueuedCpuMove();
            }
            CommitGameClockMove(ToClockSide(m_engineDerived.sideToMove));
            ArmPreparedTakebackGrace();
            (void)PersistSnapshotDeferredWithRetries("move_finalized", false);
            continue;
        }

        if (!interactionMove && (isUndo || isCancelUndo)) {
            const bool wasGameOver = IsGameOverVariantAware();
            // Undo/cancel-undo cancels any pending CPU move.
            ClearPendingCpuMoveSelection();

            // Undo/cancel-undo cancels any pending promotion.
            m_promotionPending = false;

            // Maintain coordinator-owned undo persistence fields.
            if (isUndo) {
                m_cancelUndoVariantOutcome = m_variantOutcome;
                m_cancelUndoHistoryEntryValid = PopUndoHistoryEntry(m_cancelUndoHistoryEntry);
                m_lastUndoEntryPromotionPending = false;

            } else {
                if (m_cancelUndoHistoryEntryValid) {
                    (void)AppendUndoHistoryEntry(m_cancelUndoHistoryEntry[0],
                                                 m_cancelUndoHistoryEntry[1],
                                                 m_cancelUndoHistoryEntry[2],
                                                 m_cancelUndoHistoryEntry[3],
                                                 m_cancelUndoHistoryEntry[4],
                                                 m_cancelUndoHistoryEntry[5],
                                                 m_cancelUndoHistoryEntry[6]);
                    if (m_cancelUndoDuckValid &&
                        m_duckUndoMoveCount != 0U) {
                        const uint16_t lastDuck = static_cast<uint16_t>(
                            m_duckUndoMoveCount - 1U);
                        m_duckUndoFrom[lastDuck] = m_cancelUndoDuckFrom;
                        m_duckUndoTo[lastDuck] = m_cancelUndoDuckTo;
                    }
                    m_cancelUndoHistoryEntryValid = false;
                    std::memset(m_cancelUndoHistoryEntry, 0, sizeof(m_cancelUndoHistoryEntry));
                }
            }

            // Preserve pre-undo FEN to support cancel-undo even if we must rebuild.
            if (isUndo) {
                m_cancelUndoFen = m_cachedFen;
                m_cancelUndoFenValid = !m_cancelUndoFen.Empty();
            }

            bool ok = false;
            if (isUndo) {
                ok = TryUndoUsingFenHistory();
                if (!ok) {
                    ok = RebuildEngineFromUndoState();
                }
            } else {
                ok = TryCancelUndoUsingFenHistory();
                if (!ok && m_cancelUndoFenValid && !m_cancelUndoFen.Empty()) {
                    ok = m_engine.SetFen(m_cancelUndoFen.CStr());
                    if (ok) {
                        m_cachedFen = m_cancelUndoFen;
                    }
                }
                if (!ok) {
                    ok = RebuildEngineFromUndoState();
                }
                m_cancelUndoFenValid = false;
                m_cancelUndoFen.Clear();
            }

            if (!ok) {
                if (isUndo) {
                    DispatchPendingInteractionActionResult(false);
                } else {
                    DispatchInteractionAdapterResult(
                        runtime::AppEventType::RestoreResult,
                        "cancel_undo",
                        false);
                }
                logger::Warn("engine: undo/cancel-undo failed to apply (history + rebuild)");

                if (isUndo) {
                    if (m_cancelUndoHistoryEntryValid) {
                        (void)AppendUndoHistoryEntry(m_cancelUndoHistoryEntry[0],
                                                     m_cancelUndoHistoryEntry[1],
                                                     m_cancelUndoHistoryEntry[2],
                                                     m_cancelUndoHistoryEntry[3],
                                                     m_cancelUndoHistoryEntry[4],
                                                     m_cancelUndoHistoryEntry[5],
                                                     m_cancelUndoHistoryEntry[6]);
                        if (m_cancelUndoDuckValid &&
                            m_duckUndoMoveCount != 0U) {
                            const uint16_t lastDuck = static_cast<uint16_t>(
                                m_duckUndoMoveCount - 1U);
                            m_duckUndoFrom[lastDuck] =
                                m_cancelUndoDuckFrom;
                            m_duckUndoTo[lastDuck] =
                                m_cancelUndoDuckTo;
                        }
                        m_cancelUndoHistoryEntryValid = false;
                        std::memset(m_cancelUndoHistoryEntry, 0, sizeof(m_cancelUndoHistoryEntry));
                    }

                    if (m_cancelUndoFenValid && !m_cancelUndoFen.Empty()) {
                        if (m_engine.SetFen(m_cancelUndoFen.CStr())) {
                            m_cachedFen = m_cancelUndoFen;
                        }
                    }
                }

                m_cancelUndoFenValid = false;
                m_cancelUndoFen.Clear();

                m_stateMachine.ResetForExternalEngineMutation();
                m_stateMachine.ClearMovedSquares();
                SeedFenHistoryFromCurrentFen();
                UpdateLastMovePulseFromUndoHistory();
                ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
                m_engineDerived.RefreshFromEngine(m_engine);
                RefreshDrawStateFromTracking();
                m_gameClock.SwitchActiveSideWithoutIncrement(ToClockSide(m_engineDerived.sideToMove));
                if (isGraceTakebackCommand) {
                    ClearTakebackGrace();
                }
                continue;
            }

            if (isUndo && (IsDuckGame() || HasResignationOutcome())) {
                if (m_cancelUndoDuckValid) {
                    m_duckSquare = m_cancelUndoDuckFrom;
                }
                m_variantOutcome = VariantOutcome{};
                m_duckUndoRequestedCount = 0U;
            }
            if (isCancelUndo &&
                (IsDuckGame() || m_cancelUndoVariantOutcome.reason == VariantOutcomeReason::Resigned) &&
                !m_cancelUndoDuckValid) {
                m_variantOutcome = m_cancelUndoVariantOutcome;
            }

            if (isUndo) {
                DispatchPendingInteractionActionResult(true);
            } else {
                DispatchInteractionAdapterResult(
                    runtime::AppEventType::RestoreResult,
                    "cancel_undo",
                    true);
            }
            (void)SafeGetFen(m_engine, m_cachedFen);
            ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());

            UpdateLastMovePulseFromUndoHistory();

            logger::Trace("fen: %s", m_cachedFen.CStr());

            if (m_traceRecorder && m_traceRecorder->IsActive()) {
                m_traceRecorder->RecordCheckpointEngine(m_cachedFen.CStr());
            }
            m_engineDerived.RefreshFromEngine(m_engine);
            // Undo/cancel-undo rewinds or reapplies coordinator-owned history.
            // Rebuild the ply-boundary ring so subsequent traversal, including
            // after reboot, follows the same authoritative history.
            if (!RebuildFenHistoryFromUndoState()) {
                m_halfmoveClockPlies = ComputeHalfmoveClockPliesFromUndoHistory();
                SeedFenHistoryFromCurrentFen();
            }
            RefreshDrawStateFromTracking();

            // Undo/cancel-undo may exit a game-over state; also ensure VS-CPU is idle.
            ClearPendingCpuMoveSelection();
            if (m_vsCpuThinking) {
                QuiesceAllThinking();
            }
            m_stateMachine.CancelQueuedCpuMove();
            // A grace inverse is an exact transaction rollback, including the
            // increment and any opponent time consumed before the takeback.
            // Manual Undo keeps its established active-side-only semantics.
            if (isGraceTakebackCommand) {
                RestoreTakebackGraceSnapshots(isCancelUndo);
            } else {
                m_gameClock.SwitchActiveSideWithoutIncrement(
                    ToClockSide(m_engineDerived.sideToMove));
                if (isUndo && wasGameOver) ResumeGameClockAfterTerminalUndo();
            }
            if (isCancelUndo && IsGameOverVariantAware()) {
                m_gameClock.Stop();
            }
            if (isCancelUndo && IsDuckGame() &&
                BeginDuckCancelUndoRedoRelocation()) {
                continue;
            }
            if (isCancelUndo) {
                m_stateMachine.ClearMoveRenderTransition();
            }
            if (isUndo && m_stateMachine.IsUndoPromptActive()) {
                ScheduleSnapshotRetry("undo_prompt_completion", false);
            } else {
                (void)PersistSnapshotDeferredWithRetries(
                    isUndo ? "undo" : "cancel_undo",
                    false);
            }
            RecordVariantTraceCheckpoint();
            continue;
        }
    }
}

} // namespace modules
