#include "firmware/recovery_policy.hpp"
#include "hardware/user_input_timing.hpp"

namespace firmware {

namespace {

bool ControlsAreNeutral(const RecoveryInput& input) {
    return input.controlsReleased &&
           !input.retryHeld &&
           !input.destructiveChordHeld &&
           input.owner == RecoveryOwner::None;
}

uint16_t ProgressPermille(const uint32_t elapsedMs,
                          const uint32_t durationMs) {
    if (elapsedMs >= durationMs) {
        return 1000U;
    }
    return static_cast<uint16_t>(
        (static_cast<uint64_t>(elapsedMs) * 1000U) / durationMs);
}

} // namespace

RecoveryCause SelectRecoveryCause(const RecoveryEvidence& evidence) {
    if (evidence.manualFactoryResetRequested) {
        return RecoveryCause::ManualFactoryReset;
    }
    if (evidence.persistedStateCorrupt) {
        return RecoveryCause::PersistedStateCorrupt;
    }
    if (evidence.updatePending) {
        return RecoveryCause::UpdatePending;
    }
    if (evidence.updateFailed) {
        return RecoveryCause::UpdateFailed;
    }
    if (evidence.earlyCrashDetected) {
        return RecoveryCause::EarlyCrash;
    }
    return RecoveryCause::None;
}

RecoveryInput NormalizeSafeRecoveryInput(
    const RecoveryControlLevels& controls) {
    const bool p1Any = controls.p1X || controls.p1O;
    const bool p2X = controls.hasP2Controls && controls.p2X;
    const bool p2O = controls.hasP2Controls && controls.p2O;
    const bool p2Any = p2X || p2O;

    RecoveryInput input;
    input.controlsReleased = !(p1Any || p2Any);
    if (p1Any == p2Any) {
        return input;
    }

    if (p1Any) {
        input.owner = RecoveryOwner::P1;
        input.destructiveChordHeld = controls.p1X && controls.p1O;
        input.retryHeld = controls.p1O && !controls.p1X;
    } else {
        input.owner = RecoveryOwner::P2;
        input.destructiveChordHeld = p2X && p2O;
        input.retryHeld = p2O && !p2X;
    }
    return input;
}

RecoveryOwner SelectFactoryResetEntryOwner(
    const RecoveryControlLevels& controls) {
    const bool p2Down = controls.hasP2Controls && controls.p2Down;
    if (controls.p1Down == p2Down) {
        return RecoveryOwner::None;
    }
    return controls.p1Down ? RecoveryOwner::P1 : RecoveryOwner::P2;
}

RecoveryInput NormalizeFactoryResetInput(
    const RecoveryControlLevels& controls,
    const RecoveryOwner owner) {
    const RecoveryInput face = NormalizeSafeRecoveryInput(controls);
    const bool down = controls.p1Down ||
        (controls.hasP2Controls && controls.p2Down);
    RecoveryInput input;
    input.controlsReleased = face.controlsReleased && !down;
    if (!down && face.destructiveChordHeld && face.owner == owner) {
        input.owner = owner;
        input.destructiveChordHeld = true;
    }
    return input;
}

void RecoveryPolicy::Begin(const RecoveryCause cause,
                           const RecoveryBeginMode mode,
                           const uint32_t nowMs) {
    m_snapshot = {};
    m_coldResetStartedMs.reset();
    m_coldStableInputSinceMs.reset();
    m_snapshot.cause = cause;
    m_snapshot.stateEnteredMs = nowMs;
    m_gestureStartedMs = nowMs;

    if (cause == RecoveryCause::None) {
        m_snapshot.state = RecoveryState::Inactive;
        return;
    }

    m_snapshot.state =
        (mode == RecoveryBeginMode::RequireNeutralRelease)
            ? RecoveryState::WaitForRelease
            : RecoveryState::Ready;
}

void RecoveryPolicy::BeginColdFactoryReset(const uint32_t nowMs) {
    Begin(RecoveryCause::ManualFactoryReset,
          RecoveryBeginMode::RequireNeutralRelease, nowMs);
    m_coldResetStartedMs = nowMs;
}

bool RecoveryPolicy::ColdInputStable(const bool valid, const uint32_t nowMs) {
    if (!valid) {
        m_coldStableInputSinceMs.reset();
        return false;
    }
    if (!m_coldStableInputSinceMs) m_coldStableInputSinceMs = nowMs;
    return nowMs - *m_coldStableInputSinceMs >=
        hardware::user_input_timing::BUTTON_DEBOUNCE_MS;
}

RecoveryUpdate RecoveryPolicy::Update(const RecoveryInput& input,
                                      const uint32_t nowMs) {
    RecoveryAction emittedAction = RecoveryAction::None;
    const bool coldReset = IsColdFactoryReset();
    if (coldReset && m_snapshot.state != RecoveryState::ActionPending &&
        nowMs - *m_coldResetStartedMs >= COLD_RESET_PROMPT_MS) {
        Begin(RecoveryCause::None, RecoveryBeginMode::ArmImmediately, nowMs);
        return RecoveryUpdate{m_snapshot, RecoveryAction::None};
    }
    const bool destructive =
        input.destructiveChordHeld && input.owner != RecoveryOwner::None;

    switch (m_snapshot.state) {
        case RecoveryState::Inactive:
            break;

        case RecoveryState::WaitForRelease:
            if (coldReset ? ColdInputStable(ControlsAreNeutral(input), nowMs)
                          : ControlsAreNeutral(input)) {
                m_snapshot.activeOwner = RecoveryOwner::None;
                EnterState(RecoveryState::Ready, nowMs);
            }
            break;

        case RecoveryState::Ready:
            if (coldReset ? ColdInputStable(destructive, nowMs) : destructive) {
                m_snapshot.activeOwner = input.owner;
                m_gestureStartedMs = nowMs;
                EnterState(RecoveryState::DestructiveHold, nowMs);
            } else if (!coldReset && input.retryHeld &&
                       input.owner != RecoveryOwner::None) {
                m_snapshot.activeOwner = input.owner;
                m_gestureStartedMs = nowMs;
                EnterState(RecoveryState::RetryHold, nowMs);
            }
            break;

        case RecoveryState::RetryHold:
            if (input.owner != m_snapshot.activeOwner) {
                CancelCurrentGesture(nowMs);
            } else if (input.destructiveChordHeld) {
                m_gestureStartedMs = nowMs;
                EnterState(RecoveryState::DestructiveHold, nowMs);
            } else if (!input.retryHeld) {
                CancelCurrentGesture(nowMs);
            } else if ((nowMs - m_gestureStartedMs) >= RETRY_HOLD_MS) {
                emittedAction = EmitAction(RecoveryAction::RetryBoot, nowMs);
            } else {
                UpdateProgress(nowMs);
            }
            break;

        case RecoveryState::DestructiveHold:
            if (input.owner != m_snapshot.activeOwner ||
                !input.destructiveChordHeld) {
                CancelCurrentGesture(nowMs);
            } else if (coldReset && nowMs - m_gestureStartedMs >=
                                      COLD_RESET_HOLD_MS) {
                emittedAction = EmitAction(RecoveryAction::FactoryReset, nowMs);
            } else if (!coldReset && (nowMs - m_gestureStartedMs) >=
                       DESTRUCTIVE_HOLD_MS) {
                EnterState(RecoveryState::CancelWindow, nowMs);
                UpdateProgress(nowMs);
            } else {
                UpdateProgress(nowMs);
            }
            break;

        case RecoveryState::CancelWindow:
            if (input.owner != m_snapshot.activeOwner ||
                !input.destructiveChordHeld) {
                CancelCurrentGesture(nowMs);
            } else if ((nowMs - m_snapshot.stateEnteredMs) >=
                       CANCEL_WINDOW_MS) {
                emittedAction = EmitAction(RecoveryAction::FactoryReset, nowMs);
            } else {
                UpdateProgress(nowMs);
            }
            break;

        case RecoveryState::Cancelled:
            if (coldReset) {
                if (ColdInputStable(ControlsAreNeutral(input), nowMs))
                    Begin(RecoveryCause::None,
                          RecoveryBeginMode::ArmImmediately, nowMs);
            } else if (ControlsAreNeutral(input)) {
                m_snapshot.activeOwner = RecoveryOwner::None;
                EnterState(RecoveryState::Ready, nowMs);
            }
            break;

        case RecoveryState::ActionPending:
            break;

        case RecoveryState::ActionFailed:
            if (ControlsAreNeutral(input)) {
                m_snapshot.pendingAction = RecoveryAction::None;
                m_snapshot.actionFailure = RecoveryActionFailure::None;
                m_snapshot.activeOwner = RecoveryOwner::None;
                EnterState(RecoveryState::Ready, nowMs);
            }
            break;
    }

    return RecoveryUpdate{m_snapshot, emittedAction};
}

void RecoveryPolicy::ReportActionFailure(
    const RecoveryActionFailure failure,
    const uint32_t nowMs) {
    if (m_snapshot.state != RecoveryState::ActionPending ||
        m_snapshot.pendingAction == RecoveryAction::None) {
        return;
    }
    // Failure hands back to ordinary recovery, which needs fresh input and
    // its longer consent sequence. The short cold-power prompt is spent.
    m_coldResetStartedMs.reset();
    m_snapshot.actionFailure = failure;
    EnterState(RecoveryState::ActionFailed, nowMs);
    m_snapshot.totalProgressPermille = 1000U;
}

void RecoveryPolicy::EnterState(const RecoveryState state,
                                const uint32_t nowMs) {
    m_snapshot.state = state;
    m_snapshot.stateEnteredMs = nowMs;
    m_coldStableInputSinceMs.reset();
    if (state == RecoveryState::Ready ||
        state == RecoveryState::WaitForRelease ||
        state == RecoveryState::Inactive ||
        state == RecoveryState::RetryHold ||
        state == RecoveryState::DestructiveHold) {
        m_snapshot.totalProgressPermille = 0U;
    }
}

void RecoveryPolicy::CancelCurrentGesture(const uint32_t nowMs) {
    UpdateProgress(nowMs);
    m_snapshot.pendingAction = RecoveryAction::None;
    EnterState(RecoveryState::Cancelled, nowMs);
}

RecoveryAction RecoveryPolicy::EmitAction(const RecoveryAction action,
                                          const uint32_t nowMs) {
    m_snapshot.pendingAction = action;
    m_snapshot.totalProgressPermille = 1000U;
    EnterState(RecoveryState::ActionPending, nowMs);
    return action;
}

void RecoveryPolicy::UpdateProgress(const uint32_t nowMs) {
    const uint32_t elapsedMs = nowMs - m_gestureStartedMs;
    uint16_t progress = m_snapshot.totalProgressPermille;
    if (m_snapshot.state == RecoveryState::RetryHold) {
        progress = ProgressPermille(elapsedMs, RETRY_HOLD_MS);
    } else if (m_snapshot.state == RecoveryState::DestructiveHold ||
               m_snapshot.state == RecoveryState::CancelWindow) {
        progress = ProgressPermille(
            elapsedMs, IsColdFactoryReset() ? COLD_RESET_HOLD_MS
                                            : DESTRUCTIVE_HOLD_MS);
    }

    if (progress > m_snapshot.totalProgressPermille) {
        m_snapshot.totalProgressPermille = progress;
    }
}

} // namespace firmware
