#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include "modules/side_helpers.hpp"
#include "modules/vs_cpu_turn.hpp"
#include "runtime/runtime_health.hpp"

namespace modules {

namespace {

constexpr uint32_t OPENING_CLOCK_ARMING_WINDOW_MS = 750U;

bool IsAuthorizedOpeningHumanProgress(
    const runtime::TaggedInteractionState& interaction) {
    switch (interaction.tag) {
        case runtime::ChessInteractionPhase::PieceLifted:
        case runtime::ChessInteractionPhase::CaptureStaged:
            return true;
        case runtime::ChessInteractionPhase::ExecutingPlan:
        case runtime::ChessInteractionPhase::AwaitingAction:
        case runtime::ChessInteractionPhase::Settling:
        case runtime::ChessInteractionPhase::DuckRelocation:
            return interaction.plan.source ==
                       runtime::PhysicalTransitionSource::Human &&
                   interaction.plan.kind !=
                       runtime::PhysicalTransitionKind::None &&
                   interaction.plan.kind !=
                       runtime::PhysicalTransitionKind::Undo &&
                   interaction.requiredAction !=
                       runtime::RequiredPhysicalAction::CorrectBoard;
        case runtime::ChessInteractionPhase::Idle:
        case runtime::ChessInteractionPhase::Correction:
        default:
            return false;
    }
}

uint32_t LaterRecentTimestamp(const uint32_t first,
                              const uint32_t second) {
    const uint32_t now = fxe::Millis();
    return now - first <= now - second ? first : second;
}

} // namespace

runtime::ClockSide GamePersistenceCoordinator::ToClockSide(const SideToMove side) {
    return ClockSideFromSideToMove(side);
}

bool GamePersistenceCoordinator::RestoreGameClockFromPersistence(
    const GamePersistenceState& persisted,
    const bool initializedNewGame,
    const bool untouchedInitialSetup) {
    const uint32_t timerSetting = ReadTimerSetting();
    const runtime::ClockSide engineSide =
        ToClockSide(m_engineDerived.sideToMove);

    m_gameClock.ResetFromTimerSetting(0U, engineSide);
    m_cachedTimerSetting = timerSetting;
    m_clockSessionId = 1U;
    m_clockDurableEpoch = 0U;
    m_clockSessionStarted = false;
    m_pregameClockGraceArmed = false;
    m_pregameClockProgressSeen = false;
    m_pregameClockProgressStartedAtMs = 0U;
    m_pregameClockGraceElapsed.Reset();
    m_restoredClockResumeAuthorizationPending = false;
    ClearClockTerminalOutcome();

    if (!persisted.clockStatePresent) {
        m_gameClock.ResetFromTimerSetting(timerSetting, engineSide);
        if (!initializedNewGame && m_gameClock.IsEnabled() &&
            !IsGameOverVariantAware()) {
            const runtime::TimerPreset preset =
                runtime::TimerPresetForTimerSetting(timerSetting);
            m_gameClock.RestoreState(runtime::GameClockState{
                .enabled = true,
                .running = true,
                .paused = true,
                .timedOut = false,
                .activeSide = engineSide,
                .timedOutSide = runtime::ClockSide::White,
                .remainingMsWhite = preset.baseSeconds * 1000U,
                .remainingMsBlack = preset.baseSeconds * 1000U,
                .incrementMs = preset.incrementSeconds * 1000U,
            });
            m_clockSessionStarted = true;
            m_restoredClockResumeAuthorizationPending = true;
        }
        return true;
    }

    const GameClockPersistenceState& durable = persisted.clockState;
    const bool staleUnusedClockAtUntouchedStart =
        !initializedNewGame && untouchedInitialSetup &&
        !m_pendingDuckTurn.valid &&
        durable.timerSetting != timerSetting &&
        !durable.clock.timedOut &&
        durable.clock.remainingMsWhite == durable.baseMs &&
        durable.clock.remainingMsBlack == durable.baseMs;
    if (staleUnusedClockAtUntouchedStart &&
        durable.clock.activeSide == engineSide) {
        // Some pre-release builds saved TIMER separately from an unused
        // opening snapshot. Only a record with no clock progress may yield to
        // that later preference; a terminal result remains authoritative.
        (void)m_settings.Doc().remove(KEY_GAME_CLOCK_V1);
        m_settings.NotifyDocChanged();
        m_gameClock.ResetFromTimerSetting(timerSetting, engineSide);
        logger::Warn(
            "clock: retired stale unused record at untouched initial setup "
            "(saved_timer=%u current_timer=%u)",
            static_cast<unsigned>(durable.timerSetting),
            static_cast<unsigned>(timerSetting));
        return true;
    }
    // Disabled clocks normalize to White in RAM and carry no turn authority.
    // Only an enabled clock must bind its active side to the engine position.
    if (initializedNewGame ||
        (!durable.clock.timedOut && !HasResignationOutcome() &&
         durable.timerSetting != timerSetting) ||
        (durable.clock.enabled && durable.clock.activeSide != engineSide)) {
        return false;
    }

    const bool gameAlreadyTerminal = IsGameOverVariantAware();
    if ((durable.clock.running && gameAlreadyTerminal) ||
        (durable.clock.timedOut && gameAlreadyTerminal) ||
        (durable.started && !durable.clock.running &&
         !durable.clock.timedOut && !gameAlreadyTerminal)) {
        return false;
    }
    if (durable.clock.timedOut &&
        (persisted.cpuMoveValid || !persisted.interactionState.Empty() ||
         m_pendingDuckTurn.valid)) {
        return false;
    }

    runtime::GameClockState restored = durable.clock;
    if (restored.running) {
        // Cold-off time is unknowable. A live record resumes only after the
        // explicit fresh-scan/runtime-health authorization gate below.
        restored.paused = true;
    }
    // The saved preset belongs to this terminal episode. Preserve a newer
    // separately saved TIMER preference for explicit New Game, independently
    // of whether optional terminal presentation can be restored.
    m_cachedTimerSetting = (durable.clock.timedOut || HasResignationOutcome()) ? durable.timerSetting
                                                : timerSetting;
    m_gameClock.ResetFromTimerSetting(m_cachedTimerSetting, engineSide);
    m_gameClock.RestoreState(restored);
    m_clockSessionId = durable.sessionId;
    m_clockDurableEpoch = durable.durableEpoch;
    m_clockSessionStarted = durable.started;
    m_restoredClockResumeAuthorizationPending =
        restored.running && !restored.timedOut;
    if (restored.timedOut) {
        m_clockTerminalWinner =
            restored.timedOutSide == runtime::ClockSide::White
                ? GameWinner::Black
                : GameWinner::White;
        m_clockTerminalStartMs = fxe::Millis();
        if (!RestoreClockTerminalPresentation(
                durable.terminalDuckChessMove.View())) {
            // Presentation is optional; its failure cannot discard the valid
            // stopped clock/result or resume a partially completed turn.
            logger::Warn("clock: ignored invalid terminal presentation");
        }
        ApplyEngineFenToStateMachineExpected(GetPresentationFen());
    }
    return true;
}

bool GamePersistenceCoordinator::AuthorizeRestoredClockResumeAfterBoot(
    const bool freshSuccessfulFullBoardScan,
    const runtime::RuntimeHealthSnapshot& runtimeHealth) {
    if (!m_restoredClockResumeAuthorizationPending ||
        HasClockTerminalOutcome() || m_gameClock.HasTimedOut() ||
        !freshSuccessfulFullBoardScan ||
        !runtimeHealth.advice.boardInputAllowed ||
        !runtimeHealth.advice.moveAcceptanceAllowed ||
        !runtimeHealth.advice.clocksAllowed) {
        return false;
    }

    m_restoredClockResumeAuthorizationPending = false;
    return true;
}

void GamePersistenceCoordinator::SetRuntimeHealthSnapshot(
    const runtime::RuntimeHealthSnapshot& runtimeHealth) {
    m_runtimeHealthClocksAllowed = runtimeHealth.advice.clocksAllowed;
    if (!m_runtimeHealthClocksAllowed && m_gameClock.IsRunning()) {
        m_gameClock.SetPaused(true);
    }
}

void GamePersistenceCoordinator::SetBoardSettingsActive(const bool active) {
    if (active) StopBenchmark();
    if (active && !m_boardSettingsActive) {
        m_stateMachine.CancelHintRefinement();
    }
    m_boardSettingsActive = active;
    if (active && m_gameClock.IsRunning()) {
        m_gameClock.SetPaused(true);
    }
}

GamePersistenceCoordinator::ClockPauseReasons
GamePersistenceCoordinator::ComputeClockPauseReasons() const {
    const runtime::TaggedInteractionState& interaction =
        m_stateMachine.GetChessInteractionModel().phase;
    const bool correction =
        interaction.tag == runtime::ChessInteractionPhase::Correction ||
        interaction.requiredAction ==
            runtime::RequiredPhysicalAction::CorrectBoard;
    const bool undo =
        m_stateMachine.IsUndoPromptActive() ||
        (interaction.tag != runtime::ChessInteractionPhase::Idle &&
         interaction.plan.source ==
             runtime::PhysicalTransitionSource::Undo);
    return ClockPauseReasons{
        .placement = m_stateMachine.IsPlacementModeActive(),
        .correction = correction,
        .undo = undo,
        .humanIllegal = m_stateMachine.HasHumanIllegal(),
        .cpuIllegal = m_stateMachine.HasCpuIllegal(),
        .cpuMoveQueued = m_stateMachine.HasQueuedCpuMove(),
        .occupancy =
            !m_stateMachine.IsPhysicalOccupancyConsistentWithInteraction(),
        .runtimeHealth = !m_runtimeHealthClocksAllowed,
        .boardSettings = m_boardSettingsActive,
    };
}

uint32_t GamePersistenceCoordinator::ReadTimerSetting() const {
    return m_settings.ReadValue("TIMER", static_cast<uint32_t>(0U));
}

uint32_t GamePersistenceCoordinator::ReadVsCpuLevelSetting() const {
    const uint32_t v = m_settings.ReadValue("VS_CPU_LEVEL", static_cast<uint32_t>(0U));
    return (v > 11U) ? 11U : v;
}

void GamePersistenceCoordinator::UpdateGameClock() {
    if (HasClockTerminalOutcome() || HasResignationOutcome()) {
        return;
    }
    const uint32_t timerSetting = ReadTimerSetting();
    if (timerSetting != m_cachedTimerSetting) {
        m_cachedTimerSetting = timerSetting;
        m_gameClock.ResetFromTimerSetting(timerSetting, ToClockSide(m_engineDerived.sideToMove));
        m_clockSessionStarted = false;
        m_pregameClockGraceArmed = false;
        m_pregameClockProgressSeen = false;
        m_pregameClockProgressStartedAtMs = 0U;
        m_pregameClockGraceElapsed.Reset();
        m_restoredClockResumeAuthorizationPending = false;
        ClearClockTerminalOutcome();
    }

    if (!m_gameClock.IsEnabled()) {
        if (m_gameClock.IsRunning()) {
            m_gameClock.Stop();
        }
        return;
    }

    if (IsGameOverVariantAware()) {
        (void)ConsumePendingClockTimeout();
        m_gameClock.Stop();
        return;
    }

    const ClockPauseReasons pauseReasons = ComputeClockPauseReasons();
    const bool shouldPause =
        pauseReasons.Any() || m_restoredClockResumeAuthorizationPending;

    if (!m_clockSessionStarted) {
        const runtime::TaggedInteractionState& interaction =
            m_stateMachine.GetChessInteractionModel().phase;
        const bool noCommittedMove =
            m_undoHistoryFieldCount == 0U && m_duckUndoMoveCount == 0U;
        const bool firstMoveCommitted = !noCommittedMove;
        const bool humanProgress =
            noCommittedMove &&
            IsAuthorizedOpeningHumanProgress(interaction) &&
            m_stateMachine.IsPhysicalOccupancyConsistentWithInteraction();
        const bool firstPlayerIsP2 =
            m_settings.ReadValue(
                "GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U;
        const bool cpuPlayerIsP2 =
            m_settings.ReadValue(
                "VS_CPU_PLAYER", static_cast<uint32_t>(2U)) == 2U;
        const bool cpuOwnsOpeningTurn =
            modules::IsVsCpuTurn(
                m_engineDerived.sideToMove,
                ReadVsCpuLevelSetting(),
                firstPlayerIsP2,
                cpuPlayerIsP2);
        const bool cpuProgress =
            noCommittedMove && cpuOwnsOpeningTurn &&
            (m_vsCpuThinking || m_pendingCpuMoveUciValid ||
             m_stateMachine.HasQueuedCpuMove());
        const bool openingProgress = humanProgress || cpuProgress;
        const bool playReady =
            noCommittedMove &&
            interaction.tag == runtime::ChessInteractionPhase::Idle &&
            m_stateMachine.IsExpectedOccupancyMatched() &&
            !m_stateMachine.IsPlacementModeActive() &&
            !m_stateMachine.IsKingQueenSwapPromptActive() &&
            !m_stateMachine.IsUndoPromptActive() &&
            !m_stateMachine.IsPromotionActive() &&
            !m_stateMachine.HasHumanIllegal() &&
            !m_stateMachine.HasCpuIllegal() &&
            !shouldPause;

        if (openingProgress && m_pregameClockGraceArmed) {
            if (!m_pregameClockProgressSeen) {
                m_pregameClockProgressSeen = true;
                m_pregameClockProgressStartedAtMs =
                    humanProgress
                        ? m_stateMachine.GetLastBoardObservationTimestampMs()
                        : fxe::Millis();
            }
        } else if (playReady) {
            if (!m_pregameClockGraceArmed ||
                m_pregameClockProgressSeen) {
                m_pregameClockGraceArmed = true;
                m_pregameClockProgressSeen = false;
                m_pregameClockProgressStartedAtMs = 0U;
                m_pregameClockGraceElapsed.Reset();
            }
        } else if (!(firstMoveCommitted &&
                     m_pregameClockProgressSeen)) {
            m_pregameClockGraceArmed = false;
            m_pregameClockProgressSeen = false;
            m_pregameClockProgressStartedAtMs = 0U;
        }

        const bool progressRemainsValid =
            openingProgress ||
            (firstMoveCommitted && m_pregameClockProgressSeen);
        const bool startBlocked =
            cpuProgress ? pauseReasons.AnyExceptCpuMoveQueued()
                        : shouldPause;
        if (progressRemainsValid && m_pregameClockGraceArmed &&
            m_pregameClockProgressSeen &&
            m_pregameClockGraceElapsed.HasElapsed(
                OPENING_CLOCK_ARMING_WINDOW_MS) &&
            !startBlocked && !m_gameClock.HasTimedOut()) {
            const uint32_t armingDeadlineMs =
                m_pregameClockGraceElapsed.StartMs() +
                OPENING_CLOCK_ARMING_WINDOW_MS;
            const uint32_t startMs = LaterRecentTimestamp(
                armingDeadlineMs,
                m_pregameClockProgressStartedAtMs);
            m_gameClock.StartAt(startMs);
            if (m_gameClock.IsRunning()) {
                m_clockSessionStarted = true;
                m_gameClock.SetPaused(shouldPause);
                (void)PersistSnapshotDeferredWithRetries(
                    "clock_started_on_opening_progress", false);
            }
        }
    }

    if (ConsumePendingClockTimeout()) {
        if (HasClockTerminalOutcome()) {
            (void)PersistSnapshotDeferredWithRetries(
                "clock_timeout", false);
        }
        return;
    }

    if (!m_gameClock.IsRunning()) {
        return;
    }

    m_gameClock.SetPaused(shouldPause);
    if (ConsumePendingClockTimeout()) {
        if (HasClockTerminalOutcome()) {
            (void)PersistSnapshotDeferredWithRetries(
                "clock_timeout", false);
        }
        return;
    }
    m_gameClock.Tick();
    if (ConsumePendingClockTimeout() && HasClockTerminalOutcome()) {
        (void)PersistSnapshotDeferredWithRetries("clock_timeout", false);
    }
}

void GamePersistenceCoordinator::ResumeGameClockAfterTerminalUndo() {
    if (!m_clockSessionStarted || IsGameOverVariantAware() ||
        !m_gameClock.IsEnabled() || m_gameClock.HasTimedOut()) return;
    m_gameClock.Start();
    m_gameClock.SetPaused(ComputeClockPauseReasons().Any() ||
        m_restoredClockResumeAuthorizationPending);
}

void GamePersistenceCoordinator::CommitGameClockMove(
    const runtime::ClockSide newActiveSide) {
    m_gameClock.CommitMoveAndSwitch(newActiveSide);
}

} // namespace modules
