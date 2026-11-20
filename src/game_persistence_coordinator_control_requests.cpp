#include "modules/game_persistence_coordinator.hpp"

#include "modules/assistance_preferences.hpp"
#include "trace/trace_recorder.hpp"

namespace modules {

bool GamePersistenceCoordinator::DispatchControlEvent(
    void* const context,
    runtime::GameStateMachine&,
    const runtime::AppEvent& event) {
    auto& dispatch = *static_cast<ControlDispatchContext*>(context);
    GamePersistenceCoordinator& self = dispatch.coordinator;

    const char* control = nullptr;
    switch (event.type) {
        case runtime::AppEventType::UndoRequested:
            control = "undo";
            break;
        case runtime::AppEventType::HintToggleRequested:
            control = "hint_toggle";
            break;
        case runtime::AppEventType::ResetRequested:
            control = "reset";
            break;
        default:
            return false;
    }

    if (self.m_traceRecorder != nullptr && self.m_traceRecorder->IsActive()) {
        self.m_traceRecorder->RecordControl(event.seq, control, event.count);
    }

    switch (event.type) {
        case runtime::AppEventType::UndoRequested:
            dispatch.result = self.ApplyUndoRequest(event.count);
            return true;
        case runtime::AppEventType::HintToggleRequested: {
            uint8_t shownFrom = 64U, shownTo = 64U;
            const bool shown = self.m_stateMachine.IsHintDisplayRequested() &&
                self.HasCachedHintForCurrentPosition() &&
                self.m_stateMachine.TryGetHintMoveSquares(shownFrom, shownTo);
            if (self.m_coachingHintActive) {
                self.m_stateMachine.ReplayHintMove();
            } else {
                self.m_stateMachine.HandleHintButtonTap();
            }
            self.m_stateMachine.NoteManualHintRequest();
            if (!shown && self.m_stateMachine.IsHintDisplayRequested()) {
                self.ConsumeLiveHintCandidate();
                if (self.IsDuckGame()) (void)self.ShowCachedDuckHint();
                else self.SyncHintMoveForCurrentPosition(true, true);
            }
            if (self.m_stateMachine.IsHintDisplayRequested()) {
                self.LogRequestedHint();
            }
            dispatch.result = true;
            return true;
        }
        case runtime::AppEventType::ResetRequested:
            dispatch.result = self.ResetEngineInternal(true);
            return true;
        default:
            return false;
    }
}

void GamePersistenceCoordinator::RequestHintToggle() {
    StopBenchmark();
    CancelPonder(true);
    if (m_litchcraft.IsActive()) return;
    ControlDispatchContext context{*this};
    (void)m_eventIngress.Dispatch(
        runtime::AppEvent::MakeHintToggleRequested(fxe::Millis()),
        &context,
        &GamePersistenceCoordinator::DispatchControlEvent);
}

ControlCoordinatorResult GamePersistenceCoordinator::RequestControlHint(
    const uint16_t requestId) {
    StopBenchmark();
    CancelPonder(true);
    if (m_litchcraft.IsActive()) return ControlCoordinatorResult::Busy;
    (void)requestId;
    if (IsGameOverVariantAware()) {
        return ControlCoordinatorResult::GameOver;
    }
    if (m_stateMachine.IsPlacementModeActive() ||
        m_stateMachine.IsKingQueenSwapPromptActive() ||
        m_stateMachine.IsUndoPromptActive() ||
        m_stateMachine.IsPromotionActive()) {
        return ControlCoordinatorResult::Busy;
    }
    const UiOwner turnOwner = GetEngineDerivedState().sideToMove == SideToMove::White
        ? UiOwner::P1 : UiOwner::P2;
    if (assistance_preferences::CpuOwns(m_settings, turnOwner)) {
        return ControlCoordinatorResult::EngineRejected;
    }

    ControlDispatchContext context{*this};
    const uint32_t sequence = m_eventIngress.Dispatch(
        runtime::AppEvent::MakeHintToggleRequested(fxe::Millis()),
        &context,
        &GamePersistenceCoordinator::DispatchControlEvent);
    if (sequence == 0U) {
        return ControlCoordinatorResult::QueueFull;
    }
    return context.result ? ControlCoordinatorResult::Applied
                          : ControlCoordinatorResult::EngineRejected;
}

ControlCoordinatorResult GamePersistenceCoordinator::RequestControlUndo(
    const uint16_t requestId,
    const uint8_t moveCount) {
    StopBenchmark();
    CancelPonder(true);
    if (m_litchcraft.IsActive()) return ControlCoordinatorResult::Busy;
    (void)requestId;
    if (m_gameClock.HasTimedOut()) {
        return ControlCoordinatorResult::GameOver;
    }
    if (moveCount == 0U) {
        return ControlCoordinatorResult::EngineRejected;
    }
    if (m_stateMachine.IsPlacementModeActive() ||
        m_stateMachine.IsKingQueenSwapPromptActive() ||
        (m_stateMachine.IsUndoPromptActive() &&
         !IsCancelUndoAvailable())) {
        return ControlCoordinatorResult::Busy;
    }
    const bool hasTransientUndo =
        IsCancelUndoAvailable() || IsDuckGame() && m_pendingDuckTurn.valid ||
        m_stateMachine.HasQueuedCpuMove() ||
        m_stateMachine.IsPromotionActive() ||
        m_stateMachine.IsMoveActionAwaitingResult() ||
        m_stateMachine.IsHumanMoveInProgress() ||
        m_stateMachine.IsMoveSettlingActive();
    if (!HasUndoHistory() && !hasTransientUndo) {
        return ControlCoordinatorResult::NoHistory;
    }

    ControlDispatchContext context{*this};
    const uint32_t sequence = m_eventIngress.Dispatch(
        runtime::AppEvent::MakeUndoRequested(fxe::Millis(), moveCount),
        &context,
        &GamePersistenceCoordinator::DispatchControlEvent);
    if (sequence == 0U) {
        return ControlCoordinatorResult::QueueFull;
    }
    if (context.result) {
        return ControlCoordinatorResult::Applied;
    }
    return IsUndoActionPending() ? ControlCoordinatorResult::Busy
                                 : ControlCoordinatorResult::EngineRejected;
}

ControlCoordinatorResult GamePersistenceCoordinator::RequestControlNewGame(
    const uint16_t requestId) {
    StopBenchmark();
    CancelPonder(true);
    if (m_litchcraft.IsActive()) return ControlCoordinatorResult::Busy;
    (void)requestId;
    const auto admission = CheckReplacementAdmission(ReplacementKind::NewGame);
    if (admission != ControlCoordinatorResult::Applied) return admission;
    // A physical hold is a single deliberate request. Async engine ownership
    // may need more than one firmware tick to drain, so retain the accepted
    // mutation and poll it instead of converting that temporary condition
    // into a one-shot rejection that requires another two-second hold.
    if (!QuiesceAllThinking()) {
        return ControlCoordinatorResult::Pending;
    }
    ControlDispatchContext context{*this};
    const uint32_t sequence = m_eventIngress.Dispatch(
        runtime::AppEvent::MakeResetRequested(fxe::Millis()),
        &context,
        &GamePersistenceCoordinator::DispatchControlEvent);
    if (sequence == 0U) {
        return ControlCoordinatorResult::QueueFull;
    }
    return context.result ? ControlCoordinatorResult::Applied
                          : ControlCoordinatorResult::EngineRejected;
}

ControlCoordinatorResult GamePersistenceCoordinator::PollControlAction(
    const uint16_t requestId) {
    if (m_litchcraft.IsActive()) return ControlCoordinatorResult::Busy;
    // New Game is currently the only coordinator action that returns Pending.
    // Re-enter through the same guarded request path so SetFen/storage failures
    // remain terminal while worker cancellation alone remains retryable.
    return RequestControlNewGame(requestId);
}

} // namespace modules
