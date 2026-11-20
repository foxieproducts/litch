#include "modules/game_persistence_coordinator.hpp"

#include <cstring>

#include "modules/initial_fen.hpp"
#include "modules/assistance_preferences.hpp"
#include "modules/persisted_side.hpp"
#include "trace/trace_recorder.hpp"

namespace modules {

ControlCoordinatorResult
GamePersistenceCoordinator::CheckControlStartOnlyGate(
    const char* const canonicalKey) const {
    if (m_litchcraft.IsEntryHoldActive()) {
        return ControlCoordinatorResult::Busy;
    }
    const bool orientationMutation =
        canonicalKey != nullptr &&
        (std::strcmp(canonicalKey, "BOARD_ROTATE") == 0 ||
         std::strcmp(canonicalKey, "GAME_FIRST_PLAYER") == 0);
    if (m_litchcraft.IsActive()) {
        // Reinterpreting a resting brush through the new square mapping would
        // look like a lift plus a new touch on the opposite square. Require an
        // empty observation before changing orientation; once lifted, retain
        // the logical canvas and remap app input and physical output together.
        if (!orientationMutation || m_litchcraft.Occupancy() != 0U) {
            return ControlCoordinatorResult::Busy;
        }
    }
    const bool timerMutation = canonicalKey != nullptr &&
                               std::strcmp(canonicalKey, "TIMER") == 0;
    if (HasClockTerminalOutcome() ||
        !modules::IsAnyInitialFen(m_cachedFen.CStr()) ||
        m_stateMachine.GetMovedSquaresMask() != 0ULL ||
        HasUndoHistory() ||
        (timerMutation && (m_clockSessionStarted || m_gameClock.IsRunning()))) {
        return ControlCoordinatorResult::NotAtStart;
    }

    if (timerMutation) {
        // TIMER does not read or reorient the engine. Fresh setup, its K/Q
        // prompt and background Hint work must leave the clock selectable.
        // A real move/Undo still owns its physical and clock boundaries.
        const auto phase = m_stateMachine.GetChessInteractionModel().phase.tag;
        const bool setup = m_stateMachine.IsPlacementModeActive() ||
                           m_stateMachine.IsKingQueenSwapPromptActive();
        if ((phase != runtime::ChessInteractionPhase::Idle && !setup) ||
            m_pendingDuckTurn.valid || m_pendingCpuMoveUciValid ||
            m_vsCpuThinking || m_stateMachine.HasQueuedCpuMove() ||
            m_stateMachine.IsMoveSettlingActive() ||
            m_stateMachine.IsPromotionActive() ||
            m_stateMachine.IsUndoPromptActive()) {
            return ControlCoordinatorResult::Busy;
        }
        return ControlCoordinatorResult::Applied;
    }

    // Owned background Hint work must not make fresh-start controls busy.
    // Persistence does not touch the engine; the later first-player projection
    // quiesces that work before resetting and retains the intent until safe.
    if (m_vsCpuThinking || m_stateMachine.IsCpuThinkingActive() ||
        (UsesAsyncThinkWorker() && ThinkWorker().IsActive() && !m_hintThinking)) {
        return ControlCoordinatorResult::Busy;
    }

    const runtime::ChessInteractionPhase phase =
        m_stateMachine.GetChessInteractionModel().phase.tag;
    // Ordinary Fresh Setup correction is still the untouched opening boundary:
    // player-end and literal rotation changes are useful while arranging it.
    const bool ordinarySetupCorrection =
        phase == runtime::ChessInteractionPhase::Correction &&
        m_stateMachine.IsPlacementModeActive();
    const bool cancelableOrientationPrompt =
        orientationMutation &&
        m_stateMachine.IsKingQueenSwapPromptCancelable();
    if ((phase != runtime::ChessInteractionPhase::Idle &&
         !ordinarySetupCorrection) ||
        m_stateMachine.IsMoveSettlingActive() ||
        m_stateMachine.HasQueuedCpuMove() ||
        m_stateMachine.IsPromotionActive() ||
        (m_stateMachine.IsKingQueenSwapPromptActive() &&
         !cancelableOrientationPrompt) ||
        m_stateMachine.IsUndoPromptActive() ||
        m_stateMachine.IsTextOverlayActive()) {
        return ControlCoordinatorResult::Busy;
    }
    return ControlCoordinatorResult::Applied;
}

bool GamePersistenceCoordinator::RequestUndo(const uint8_t moveCount) {
    StopBenchmark();
    if (m_litchcraft.IsActive()) return false;
    ControlDispatchContext context{*this};
    (void)m_eventIngress.Dispatch(
        runtime::AppEvent::MakeUndoRequested(fxe::Millis(), moveCount),
        &context,
        &GamePersistenceCoordinator::DispatchControlEvent);
    return context.result;
}

bool GamePersistenceCoordinator::HasUndoHistory() const {
    return m_undoHistoryFieldCount != 0U;
}

uint8_t GamePersistenceCoordinator::GetUndoHistoryMoveCount() const {
    return static_cast<uint8_t>(
        m_undoHistoryFieldCount /
        UNDO_HISTORY_FIELDS_PER_MOVE);
}

bool GamePersistenceCoordinator::IsCancelUndoAvailable() const {
    return m_stateMachine.IsUndoPromptActive() &&
           (m_pendingUndoCommand.kind == UndoCommandKind::Undo ||
            m_cancelUndoHistoryEntryValid);
}

bool GamePersistenceCoordinator::IsUndoActionPending() const {
    return HasPendingUndoCommand() ||
           m_chainedUndoRemaining != 0U ||
           IsCancelUndoAvailable() ||
           m_duckUndoRelocationPending ||
           m_duckRedoRelocationPending ||
           m_stagedDuckChessUndoPending;
}

bool GamePersistenceCoordinator::HasPendingUndoCommand() const {
    return m_pendingUndoCommand.kind != UndoCommandKind::None;
}

bool GamePersistenceCoordinator::BeginNextUndo(
    const UndoCommandOrigin origin) {
    constexpr uint16_t stride =
        UNDO_HISTORY_FIELDS_PER_MOVE;
    if (m_pendingUndoCommand.kind != UndoCommandKind::None ||
        m_undoHistoryFieldCount < stride) {
        return false;
    }

    const uint16_t base =
        static_cast<uint16_t>(m_undoHistoryFieldCount - stride);
    const uint8_t from = m_undoHistoryFields[base + 5U];
    const uint8_t to = m_undoHistoryFields[base + 6U];
    if (!m_stateMachine.BeginUndoPrompt(
            from,
            to,
            PersistedSideFromU8(m_undoHistoryFields[base + 0U]),
            static_cast<char>(m_undoHistoryFields[base + 1U]),
            static_cast<char>(m_undoHistoryFields[base + 2U]),
            m_undoHistoryFields[base + 3U])) {
        return false;
    }

    m_pendingUndoCommand = PendingUndoCommand{
        UndoCommandKind::Undo, from, to, origin};
    return true;
}

void GamePersistenceCoordinator::CloseCompletedUndoWindow() {
    if (m_stateMachine.IsUndoPromptActive() ||
        m_pendingUndoCommand.kind != UndoCommandKind::None ||
        m_duckUndoRelocationPending ||
        m_duckRedoRelocationPending) {
        return;
    }

    if (m_stagedDuckChessUndoPending) {
        m_stagedDuckChessUndoPending = false;
        UpdateLastMovePulseFromUndoHistory();
        if (m_undoHistoryFieldCount == 0U &&
            std::strcmp(m_cachedFen.CStr(), INITIAL_FEN) == 0) {
            RearmFreshGameAfterCompletedUndo();
        } else {
            ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
            RecordVariantTraceCheckpoint();
            (void)PersistSnapshotDeferredWithRetries(
                "duck_staged_chess_undo_complete", false);
        }
        if (m_chainedUndoRemaining == 0U) {
            return;
        }
    }

    const bool completedFinalUndo =
        m_cancelUndoHistoryEntryValid &&
        m_undoHistoryFieldCount == 0U &&
        m_chainedUndoRemaining == 0U;

    m_cancelUndoHistoryEntryValid = false;
    std::memset(m_cancelUndoHistoryEntry, 0,
                sizeof(m_cancelUndoHistoryEntry));
    m_cancelUndoFenValid = false;
    m_cancelUndoFen.Clear();
    m_cancelUndoHalfmoveValid = false;
    m_cancelUndoHalfmovePlies = 0U;
    m_cancelUndoHintMoveValid = false;
    m_cancelUndoHintFen.Clear();
    m_cancelUndoHintFromSquare = 0U;
    m_cancelUndoHintToSquare = 0U;
    m_cancelUndoVariantOutcome = VariantOutcome{};

    if (m_takebackGrace.undoInProgress) {
        ClearTakebackGrace();
    }

    if (completedFinalUndo) {
        RearmFreshGameAfterCompletedUndo();
    }

    if (m_chainedUndoRemaining == 0U) {
        return;
    }

    // A pre-ack CPU move may need its physical placement corrected before the
    // preceding human move can own an undo plan. Preserve the chained request
    // until that correction returns the reducer to Idle.
    if (m_stateMachine.GetChessInteractionModel().phase.tag !=
            runtime::ChessInteractionPhase::Idle ||
        m_stateMachine.IsMoveSettlingActive()) {
        return;
    }
    // An uncommitted CPU chess half returns physically before the preceding
    // human turn can be undone. Its Duck return must still precede the engine
    // rewind, just as it does for an ordinary whole-turn Duck Undo.
    uint8_t duckFrom = INVALID_DUCK_SQUARE;
    uint8_t duckTo = INVALID_DUCK_SQUARE;
    if (IsDuckGame() && TryPeekLastDuckUndo(duckFrom, duckTo)) {
        if (BeginDuckUndoIfNeeded(m_chainedUndoRemaining)) {
            m_chainedUndoRemaining = 0U;
        }
    } else if (BeginNextUndo()) {
        --m_chainedUndoRemaining;
    } else {
        m_chainedUndoRemaining = 0U;
    }
}

bool GamePersistenceCoordinator::ApplyUndoRequest(const uint8_t moveCount) {
    if (m_litchcraft.IsActive()) return false;
    if (m_gameClock.HasTimedOut() ||
        m_stateMachine.IsPlacementModeActive() ||
        m_stateMachine.IsKingQueenSwapPromptActive() ||
        (m_stagedDuckChessUndoPending &&
         m_stateMachine.IsUndoPromptActive())) {
        return false;
    }

    if (!m_takebackGrace.undoInProgress) {
        ClearTakebackGrace();
    }

    RequestCancelAllThinking();

    // A committed Duck turn begins Undo with a fixed Duck return before the
    // engine rewind. While that return is still untouched, a second canonical
    // Undo is Cancel Undo just like it is during the following chess inverse.
    if (m_duckUndoRelocationPending) {
        if (!m_stateMachine.CancelDuckRelocationForUndo()) {
            return false;
        }
        m_duckUndoRelocationPending = false;
        m_duckUndoRequestedCount = 0U;
        if (m_takebackGrace.undoInProgress) {
            RestoreTakebackGraceSnapshots(true);
            ClearTakebackGrace();
        }
        return true;
    }

    if (IsDuckGame() && m_pendingDuckTurn.valid) {
        return moveCount != 0U && BeginStagedDuckChessUndo();
    }

    CloseCompletedUndoWindow();
    m_chainedUndoRemaining = 0U;

    if (IsCancelUndoAvailable()) {
        if (m_pendingUndoCommand.kind == UndoCommandKind::Undo) {
            // The inverse engine action has not run yet. Collapse undo +
            // cancel-undo to no command and leave history untouched.
            const bool graceTakeback =
                m_pendingUndoCommand.origin ==
                UndoCommandOrigin::GraceTakeback;
            m_pendingUndoCommand = PendingUndoCommand{};
            m_stateMachine.CancelUndoPrompt(true);
            m_stateMachine.ClearMoveRenderTransition();
            if (graceTakeback) {
                RestoreTakebackGraceSnapshots(true);
                ClearTakebackGrace();
            }
            uint8_t duckFrom = INVALID_DUCK_SQUARE;
            uint8_t duckTo = INVALID_DUCK_SQUARE;
            if (IsDuckGame() && TryPeekLastDuckUndo(duckFrom, duckTo)) {
                // The Duck already returned physically, but history still
                // owns the forward turn. Restore it through the same fixed
                // redo owner used after an acknowledged engine rewind.
                m_cancelUndoDuckFrom = duckFrom;
                m_cancelUndoDuckTo = duckTo;
                m_cancelUndoDuckValid = true;
                m_cancelUndoVariantOutcome = m_variantOutcome;
                m_duckSquare = duckFrom;
                DispatchInteractionPositionAcknowledged(
                    m_stateMachine.GetChessInteractionModel());
                return BeginDuckCancelUndoRedoRelocation();
            }
            return true;
        }

        if (!m_cancelUndoHistoryEntryValid) {
            return false;
        }
        m_pendingUndoCommand = PendingUndoCommand{
            UndoCommandKind::CancelUndo,
            m_cancelUndoHistoryEntry[5],
            m_cancelUndoHistoryEntry[6],
            m_takebackGrace.undoInProgress
                ? UndoCommandOrigin::GraceTakeback
                : UndoCommandOrigin::Manual};
        m_stateMachine.CancelUndoPrompt(true);
        return true;
    }

    if (m_stateMachine.IsUndoPromptActive()) {
        return false;
    }

    if (m_stateMachine.HasQueuedCpuMove()) {
        m_stateMachine.CancelQueuedCpuMove();
        ClearPendingCpuMoveSelection();
        if (!HasUndoHistory()) {
            return true;
        }
    } else if (m_stateMachine.IsPromotionActive()) {
        return m_stateMachine.BeginPendingPromotionUndo();
    }

    if (m_stateMachine.IsMoveActionAwaitingResult()) {
        const bool canceledCpuAction = runtime::IsCpuDirectedTransition(
            m_stateMachine.GetChessInteractionModel().phase.plan);
        const bool canceled =
            m_stateMachine.CancelUnclaimedMoveActionForUndo();
        if (canceled) {
            ClearPendingCpuMoveSelection();
            if (canceledCpuAction && moveCount != 0U) {
                const uint8_t available = GetUndoHistoryMoveCount();
                m_chainedUndoRemaining =
                    moveCount > available ? available : moveCount;
            }
        }
        return canceled;
    }

    // Once the reducer is Idle, the lingering CPU/composite settle is visual
    // only. End it and continue into history undo in the same request.
    (void)m_stateMachine.CancelCompletedSettleForUndo();
    if (m_stateMachine.IsHumanMoveInProgress() ||
        m_stateMachine.IsPlacementModeActive() ||
        m_stateMachine.IsMoveSettlingActive()) {
        m_stateMachine.CancelMovePreviewForUndo();
        return true;
    }

    const uint8_t available = GetUndoHistoryMoveCount();
    if (available == 0U || moveCount == 0U) {
        return false;
    }
    const uint8_t requestedRaw =
        moveCount > available ? available : moveCount;
    const uint8_t requested = requestedRaw;
    if (IsDuckGame()) {
        uint8_t duckFrom = INVALID_DUCK_SQUARE;
        uint8_t duckTo = INVALID_DUCK_SQUARE;
        if (TryPeekLastDuckUndo(duckFrom, duckTo)) {
            return BeginDuckUndoIfNeeded(requested);
        }
    }
    if (!BeginNextUndo()) {
        return false;
    }
    m_chainedUndoRemaining = static_cast<uint8_t>(requested - 1U);
    return true;
}

void GamePersistenceCoordinator::DispatchInteractionPositionAcknowledged(
    const runtime::ChessInteractionModel& model) {
    const auto& acknowledged = model.acknowledged;
    runtime::AppEvent position = runtime::AppEvent::MakePositionAcknowledged(
        0U,
        acknowledged.occupiedMask,
        acknowledged.kingMask,
        acknowledged.rookMask,
        acknowledged.moverOccupiedMask,
        acknowledged.opponentOccupiedMask,
        acknowledged.pawnMask,
        acknowledged.enPassantTargetSquare,
        acknowledged.enPassantCaptureSquare,
        acknowledged.sideToMoveValid,
        acknowledged.sideToMoveBlack,
        acknowledged.legalCaptureTargetsValid,
        acknowledged.legalCaptureTargetsMask,
        acknowledged.legalCastlingTargetsValid,
        acknowledged.legalCastlingKingTargetsMask);
    // A Duck re-acknowledgement starts from live model state. Stabilize this
    // payload through callbacks so tracing sees the rows that were dispatched.
    const runtime::CaptureLegality captureLegality = model.acknowledgedCaptureLegality;
    DispatchInteractionPositionAcknowledged(position, captureLegality);
}

void GamePersistenceCoordinator::DispatchInteractionPositionAcknowledged(
    runtime::AppEvent& position,
    const runtime::CaptureLegality& captureLegality) {
    if (m_interactionEventsExternalized) {
        return;
    }

    position.tMs = fxe::Millis();
    const uint32_t eventSequence = m_eventIngress.DispatchPositionAcknowledged(position, captureLegality);
    if (m_traceRecorder != nullptr && m_traceRecorder->IsActive()) {
        m_traceRecorder->RecordPositionAcknowledged(
            eventSequence,
            position.occupiedMask,
            position.kingMask,
            position.rookMask,
            position.moverMask,
            position.opponentMask,
            position.pawnMask,
            position.enPassantTargetSquare,
            position.enPassantCaptureSquare,
            position.sideToMoveValid,
            position.sideToMoveBlack,
            position.legalCaptureTargetsValid,
            position.legalCaptureTargetsMask,
            position.legalCastlingTargetsValid,
            position.legalCastlingKingTargetsMask,
            captureLegality);
    }
}

void GamePersistenceCoordinator::DispatchPendingInteractionActionResult(
    const bool succeeded) {
    if (m_interactionEventsExternalized) {
        return;
    }

    const runtime::ChessInteractionModel& model =
        m_stateMachine.GetChessInteractionModel();
    if (model.phase.tag != runtime::ChessInteractionPhase::AwaitingAction ||
        model.phase.pendingActionToken == 0U) {
        return;
    }

    const uint32_t token = model.phase.pendingActionToken;
    const uint32_t eventSequence = m_eventIngress.Dispatch(
        runtime::AppEvent::MakeActionResult(
            fxe::Millis(),
            token,
            succeeded));
    if (m_traceRecorder != nullptr && m_traceRecorder->IsActive()) {
        m_traceRecorder->RecordActionResult(eventSequence, token, succeeded);
        m_traceRecorder->RecordCheckpointInteraction(
            m_stateMachine.GetChessInteractionModel(),
            m_stateMachine.GetInteractionInvariantStatus());
    }
}

void GamePersistenceCoordinator::DispatchInteractionAdapterResult(
    const runtime::AppEventType type,
    const char* adapter,
    const bool succeeded) {
    if (m_interactionEventsExternalized) {
        return;
    }

    const uint32_t eventSequence = m_eventIngress.Dispatch(
        runtime::AppEvent::MakeAdapterResult(
            type,
            fxe::Millis(),
            0,
            succeeded));
    if (m_traceRecorder != nullptr && m_traceRecorder->IsActive()) {
        m_traceRecorder->RecordAdapterResult(
            eventSequence,
            adapter,
            succeeded);
    }
}

} // namespace modules
