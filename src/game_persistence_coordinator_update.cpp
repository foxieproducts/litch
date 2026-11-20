#include <algorithm>
#include <cstdint>

#include "modules/game_persistence_coordinator.hpp"

#include "foxie_embedded/elapsed_time.hpp"
#include "logger.hpp"
#include "modules/initial_fen.hpp"
#include "modules/minimal_animations_setting.hpp"
#include "modules/orientation.hpp"
#include "modules/vs_cpu_turn.hpp"

namespace modules {

namespace {

bool GetFirstPlayerIsP2SettingLocal(const SettingsStore& settings) {
    return settings.ReadValue("GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U;
}

bool GetVsCpuPlayerIsP2SettingLocal(const SettingsStore& settings) {
    return settings.ReadValue("VS_CPU_PLAYER", static_cast<uint32_t>(2U)) == 2U;
}

bool GetBoardRotateSettingLocal(const SettingsStore& settings) {
    const auto& doc = settings.Doc();
    if (doc.containsKey("BOARD_ROTATE")) {
        return settings.ReadValue("BOARD_ROTATE", static_cast<uint32_t>(0U)) != 0U;
    }
    return false;
}

} // namespace

void GamePersistenceCoordinator::RetrySnapshotIfPending() {
    if (!m_snapshotRetryPending) {
        return;
    }

    if (!m_snapshotRetryClearUndo && m_stateMachine.IsUndoPromptActive()) {
        return;
    }

    if (m_settings.IsDisabled()) {
        logger::Warn("persist: snapshot retry suspended (settings disabled) reason=%s",
                     m_snapshotRetryReason.Empty() ? "?" : m_snapshotRetryReason.CStr());
        ClearSnapshotRetry();
        return;
    }

    // Retry with a small exponential backoff to reduce log spam/IO churn.
    static constexpr uint32_t MIN_RETRY_INTERVAL_MS = 250;
    static constexpr uint32_t MAX_RETRY_INTERVAL_MS = 5000;
    const uint8_t backoffPow =
        (m_snapshotRetryFailures > 4U) ? 4U : m_snapshotRetryFailures;
    const uint32_t retryIntervalMs =
        std::min<uint32_t>(MAX_RETRY_INTERVAL_MS,
                           MIN_RETRY_INTERVAL_MS * static_cast<uint32_t>(1U << backoffPow));
    if (!m_snapshotRetryTime.HasElapsed(retryIntervalMs)) {
        return;
    }
    m_snapshotRetryTime.Reset();

    if (PersistSnapshot(m_snapshotRetryClearUndo)) {
        logger::Info("persist: snapshot retry ok: failures=%u reason=%s",
                     static_cast<unsigned>(m_snapshotRetryFailures),
                     m_snapshotRetryReason.Empty() ? "?" : m_snapshotRetryReason.CStr());
        ClearSnapshotRetry();
        return;
    }

    if (m_snapshotRetryFailures != UINT8_MAX) {
        ++m_snapshotRetryFailures;
    }

    const modules::SettingsStoreError err = m_settings.GetLastError();

    if (err == modules::SettingsStoreError::Unsupported) {
        logger::Warn("persist: snapshot retry suspended (err=%s) reason=%s",
                     modules::SettingsStoreErrorToString(err),
                     m_snapshotRetryReason.Empty() ? "?" : m_snapshotRetryReason.CStr());
        ClearSnapshotRetry();
        return;
    }

    // Only log occasionally to avoid flooding the console.
    const bool shouldLog =
        (m_snapshotRetryFailures <= 3U) ||
        ((m_snapshotRetryFailures % 10U) == 0U);
    if (shouldLog) {
        logger::Warn("persist: snapshot retry failed: failures=%u err=%s reason=%s",
                     static_cast<unsigned>(m_snapshotRetryFailures),
                     modules::SettingsStoreErrorToString(err),
                     m_snapshotRetryReason.Empty() ? "?" : m_snapshotRetryReason.CStr());
    }
}

void GamePersistenceCoordinator::Update() {
    if (UpdateBenchmark()) return;
    // Clear after every path, including Duck's paired chess/Duck setters:
    // clearing only on entry can rearm an episode while Minimal is enabled.
    struct MinimalHintPresentationGuard {
        const SettingsStore& settings;
        runtime::GameStateMachine& stateMachine;
        ~MinimalHintPresentationGuard() {
            if (minimal_animations_setting::Read(settings)) {
                stateMachine.CancelHintRefinement();
            }
        }
    } minimalHintPresentationGuard{m_settings, m_stateMachine};
    UpdateLitchcraft();
    if (m_litchcraft.IsActive()) return;
    // A timeout is consumed before every continuation that could otherwise
    // finish a staged Duck turn, mutate the engine, or start/consume search.
    UpdateGameClock();
    UpdateKingResignation();
    UpdateCpuThinkingPresentation();
    if (HasClockTerminalOutcome() || HasResignationOutcome()) {
        if (m_terminalAbandonPending) {
            if (!AbandonStagedWorkForTerminalOutcome()) {
                return;
            }
            m_terminalAbandonPending = false;
            (void)PersistSnapshotDeferredWithRetries(
                "terminal_work_quiesced", false);
        }
        // A direct engine replacement is a full-game boundary. Keep this one
        // read-only detector reachable after timeout, while every ordinary
        // coordinator mutation remains gated below.
        PollExternalEngineMutationAndApply();
        if (HasClockTerminalOutcome() ||
            (HasResignationOutcome() && !IsUndoActionPending())) {
            RetrySnapshotIfPending();
            return;
        }
    }

    CloseCompletedUndoWindow();
    if (m_duckRedoRelocationPending && !m_externalizedDuckRedoStartPending) {
        (void)ResumeExternalizedDuckRedoAfterPositionAcknowledged();
    }

    UpdateGameVariantActivation();
    if (!UpdateEmptyBoardSetup()) return;

    // A failed Duck relocation action rolls the reducer back through
    // Correction to the last acknowledged duck square. Once that correction
    // reaches Idle, the staged chess half is still authoritative and the
    // mandatory relocation must resume rather than leaving a half-turn
    // stranded (including after durable restore).
    if (IsDuckGame() && m_pendingDuckTurn.valid &&
        m_duckTurnPhase == DuckTurnPhase::DuckRelocation &&
        m_stateMachine.GetChessInteractionModel().phase.tag ==
            runtime::ChessInteractionPhase::Idle &&
        !m_externalizedDuckChessActionPending &&
        !m_externalizedDuckRelocationActionPending) {
        (void)ContinueStagedDuckTurnAfterChessAction();
    }
    UpdateInitialDuckObstructionRelocation();

    if (m_stateMachine.ConsumeUndoCanceledQueuedCpuMove()) {
        ClearPendingCpuMoveSelection();
        (void)PersistSnapshotDeferredWithRetries("cpu_move_prompt_canceled_by_undo", false);
    }

    const bool cancelCpuWorkThisTick = m_cancelCpuWorkRequested;
    const bool cancelHintWorkThisTick = m_cancelHintWorkRequested;
    m_cancelCpuWorkRequested = false;
    m_cancelHintWorkRequested = false;
    if ((cancelCpuWorkThisTick && (m_vsCpuThinking || m_ponderThinking)) ||
        (cancelHintWorkThisTick && (m_hintThinking || m_ponderThinking))) {
        if (!QuiesceAllThinking()) {
            m_cancelCpuWorkRequested = cancelCpuWorkThisTick;
            m_cancelHintWorkRequested = cancelHintWorkThisTick;
            return;
        }
    }
    if (cancelCpuWorkThisTick) {
        m_vsCpuThinking = false;
        ClearPendingCpuMoveSelection();
        m_stateMachine.CancelQueuedCpuMove();
        m_stateMachine.SetCpuThinkingActive(false);
    }
    if (cancelHintWorkThisTick) {
        m_hintThinking = false;
        m_hintPendingStart = false;
        m_hintLastSideToMoveValid = false;
        ResetHintRetry();
        m_stateMachine.SetHintThinkingActive(false);
        m_stateMachine.SetHintDisplayRequested(false);
        m_stateMachine.ClearHintMove();
    }

    const bool cancelAllThinkingThisTick = m_cancelAllThinkingRequested;
    if (cancelAllThinkingThisTick) {
        // Clear the request early so nested calls can't re-enter cancellation.
        m_cancelAllThinkingRequested = false;

        // Ensure any in-flight think is fully quiesced before we touch the engine.
        if (!QuiesceAllThinking()) {
            m_cancelAllThinkingRequested = true;
            return;
        }

        // Cancel coordinator-owned think state.
        m_hintPendingStart = false;
        ResetHintRetry();
        ClearPendingCpuMoveSelection();

        // UI should already have been cleared by RequestCancelAllThinking(), but
        // keep this idempotent in case cancellation was requested elsewhere.
        m_stateMachine.SetHintDisplayRequested(false);
        m_stateMachine.ClearHintMove();
    }

    LogStateTransitions();

    // If snapshot encoding was temporarily unsafe, keep retrying until the
    // latest stable state is accepted by the shared debounce window.
    RetrySnapshotIfPending();

    // King/queen swap prompt. BOARD_ROTATE changes literal board orientation;
    // GAME_FIRST_PLAYER changes logical-P1 physical placement. Either can
    // change the effective square mapping and require a physical K/Q swap.
    const bool boardRotateEnabled = GetBoardRotateSettingLocal(m_settings);
    const bool firstPlayerIsP2 = GetFirstPlayerIsP2SettingLocal(m_settings);
    const bool cpuPlayerIsP2 = GetVsCpuPlayerIsP2SettingLocal(m_settings);
    const bool atEitherInitialFen = modules::IsAnyInitialFen(m_cachedFen.CStr());
    UpdateKingQueenSwapPrompt(
        boardRotateEnabled, firstPlayerIsP2, atEitherInitialFen);

    // VS-CPU thinking policy:
    // - Starting a think requires a fully stable board state (no mismatch/illegal).
    // - Once a think is in progress, transient physical mistakes should not cancel it;
    //   instead we keep thinking and delay the physical CpuMoving prompt until the
    //   board is corrected back to a stable state.
    // Until a physical first move completes, an untouched starting position
    // remains variant-selectable. Preserve normal CPU ownership during that
    // window; a Duck latch cancels stale orthodox work and later restarts the
    // compound search path.
    const uint32_t vsCpuLevel = ReadVsCpuLevelSetting();

    // Split the VS-CPU gating into:
    // - maintain/poll: keep an in-flight think alive through transient mismatch/illegal
    // - start/queue: only start thinking or enter CpuMoving when the board is stable
    const bool vsCpuThinkMaintainOk = (vsCpuLevel != 0U) &&
                                      !m_stateMachine.IsKingQueenSwapPromptActive() &&
                                      !m_stateMachine.IsUndoPromptActive() &&
                                      !m_stateMachine.IsTextOverlayActive() &&
                                      !m_promotionPending &&
                                      !IsGameOverVariantAware();
    const runtime::TaggedInteractionState& phaseBeforeActions =
        m_stateMachine.GetChessInteractionModel().phase;
    const bool allowVsCpuThinkStart = vsCpuThinkMaintainOk &&
                                      phaseBeforeActions.tag ==
                                          runtime::ChessInteractionPhase::Idle &&
                                      !m_stateMachine.IsMoveSettlingActive() &&
                                      !m_stateMachine.HasQueuedCpuMove() &&
                                      !m_stateMachine.IsPlacementModeActive() &&
                                      m_stateMachine.IsExpectedOccupancyMatched() &&
                                      !m_stateMachine.HasHumanIllegal() &&
                                      !m_stateMachine.HasCpuIllegal();

    if (m_vsCpuThinking &&
        (!vsCpuThinkMaintainOk ||
         !modules::IsVsCpuTurn(
             m_engineDerived.sideToMove, vsCpuLevel, firstPlayerIsP2, cpuPlayerIsP2))) {
        if (!QuiesceAllThinking()) {
            m_cancelAllThinkingRequested = true;
            return;
        }
        DispatchInteractionAdapterResult(
            runtime::AppEventType::SearchResult,
            "search",
            false);
    }

    // Claiming a physical move or dequeuing Undo is a one-shot boundary. Keep
    // both retryable until the shared worker has stopped touching the engine;
    // otherwise a cancellation timeout could be followed by concurrent
    // ApplyUciMove/SetFen access with no way to replay the claimed action.
    const bool engineMutationPending =
        (!m_promotionPending &&
         m_stateMachine.HasCommittedMovePending()) ||
        m_pendingUndoCommand.kind != UndoCommandKind::None;
    if (m_pendingUndoCommand.kind != UndoCommandKind::None) CancelPonder(true);
    if (engineMutationPending && !QuiesceAllThinking()) {
        m_cancelAllThinkingRequested = true;
        return;
    }

    UpdatePromotionChoiceIfPending();
    ConsumeCommittedMoves();
    // Action acknowledgement can replay the next player's pickup. Re-evaluate
    // the physical pause gates against that post-move interaction immediately,
    // including when the clock was already running before this handoff.
    UpdateGameClock();
    if (HasClockTerminalOutcome()) {
        return;
    }

    const bool castlingRookPromptActive = m_stateMachine.IsCastlingRookPromptActive();
    if (castlingRookPromptActive && !m_lastCastlingRookPromptActive) {
        (void)PersistSnapshotDeferredWithRetries("castling_rook_prompt", false);
    }
    m_lastCastlingRookPromptActive = castlingRookPromptActive;

    // External polling is only allowed from the reducer's tagged Idle phase.
    // Existing search ownership must still be maintained through physical
    // progress. In particular, a background hint can finish while castling
    // awaits its rook. UpdateHints cannot start new work outside Idle.
    const runtime::ChessInteractionPhase phaseAfterActions =
        m_stateMachine.GetChessInteractionModel().phase.tag;
    if (phaseAfterActions != runtime::ChessInteractionPhase::Idle ||
        m_stateMachine.IsMoveSettlingActive() ||
        m_stateMachine.HasQueuedCpuMove()) {
        if (!m_vsCpuThinking && !m_pendingCpuMoveUciValid && !m_hintThinking && !m_ponderThinking) {
            // An early completed result needs no engine work to become visible
            // at 500 ms while its source is still held. Keep all external
            // polling and new search admission behind the existing Idle gate.
            if (phaseAfterActions == runtime::ChessInteractionPhase::PieceLifted &&
                !m_engineDerived.inCheckmate && !m_engineDerived.inDraw &&
                !m_engineDerived.inStalemate && !m_variantOutcome.IsTerminal() &&
                !IsVsCpuTurn(m_engineDerived.sideToMove, vsCpuLevel,
                             firstPlayerIsP2, cpuPlayerIsP2)) {
                if (IsDuckGame()) {
                    (void)ShowCachedDuckHint();
                } else {
                    SyncHintMoveForCurrentPosition(true, true);
                }
            }
            return;
        }
    }
    // Placement mode normally blocks coordinator polling, but we must still
    // allow VS-CPU thinking to continue (and complete) through transient
    // mismatch/illegal states.
    if (m_stateMachine.IsPlacementModeActive() && !m_vsCpuThinking &&
        !m_pendingCpuMoveUciValid && !m_hintThinking && !m_ponderThinking) {
        return;
    }
    if (m_stateMachine.IsTextOverlayActive()) {
        return;
    }

    if (m_promotionPending) {
        // Promotion modal is active; do not poll for external mutations.
        return;
    }

    if (IsDuckGame()) {
        if (UpdateDuckSearch(vsCpuLevel,
                             firstPlayerIsP2,
                             cpuPlayerIsP2,
                             cancelAllThinkingThisTick)) {
            return;
        }
        PollExternalEngineMutationAndApply();
        return;
    }

    // Speculation owns the same worker lane but never the board's CPU cue.
    // Retire it before normal Hint/CPU admission or external engine polling.
    if (m_ponderThinking && UpdatePonder(vsCpuLevel, firstPlayerIsP2, cpuPlayerIsP2)) return;

    const bool wasVsCpuThinking = m_vsCpuThinking;
    if (UpdateVsCpu(vsCpuLevel,
                    firstPlayerIsP2,
                    cpuPlayerIsP2,
                    allowVsCpuThinkStart,
                    vsCpuThinkMaintainOk,
                    cancelAllThinkingThisTick)) {
        if (wasVsCpuThinking && !m_vsCpuThinking) {
            const bool searchSucceeded = m_pendingCpuMoveUciValid ||
                                         m_stateMachine.HasQueuedCpuMove();
            DispatchInteractionAdapterResult(
                runtime::AppEventType::SearchResult,
                "search",
                searchSucceeded);
        }
        return;
    }

    if (UpdateHints(vsCpuLevel, firstPlayerIsP2, cpuPlayerIsP2,
                    cancelAllThinkingThisTick)) {
        return;
    }

    if (UpdatePonder(vsCpuLevel, firstPlayerIsP2, cpuPlayerIsP2)) return;
    PollExternalEngineMutationAndApply();
}

void GamePersistenceCoordinator::LogStateTransitions() {
    const bool humanPieceInHand = m_stateMachine.IsHumanPieceInHand();
    if (humanPieceInHand) {
        uint8_t fromSq = 255U;
        if (!m_stateMachine.TryGetHumanPieceInHandSquare(fromSq)) {
            return;
        }

        const bool shouldLog = !m_lastLoggedHumanPieceInHand ||
                               m_lastLoggedPickupFromSquare != fromSq;
        if (shouldLog) {
            char piece = '\0';
            (void)TryGetFenPieceAtSquare(m_cachedFen.CStr(), fromSq, piece);
            if (piece == '\0') {
                piece = '-';
            }

            logger::Info("pickup: from=%u piece=%c",
                         static_cast<unsigned>(fromSq),
                         piece);
            m_lastLoggedPickupFromSquare = fromSq;
        }
    } else {
        m_lastLoggedPickupFromSquare = 255;
    }

    m_lastLoggedHumanPieceInHand = humanPieceInHand;
}

} // namespace modules
