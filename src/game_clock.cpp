#include "runtime/game_clock.hpp"

namespace {

constexpr uint32_t SecondsToMs(const uint32_t s) {
    return s * 1000U;
}

} // namespace

namespace runtime {

TimerPreset TimerPresetForTimerSetting(const uint32_t timerSetting) {
    // docs/spec/timer_semantics.md
    switch (timerSetting) {
        case 0:
            return TimerPreset{0, 0};
        case 1:
            return TimerPreset{60, 0};
        case 2:
            return TimerPreset{180, 2};
        case 3:
            return TimerPreset{300, 0};
        case 4:
            return TimerPreset{600, 0};
        case 5:
            return TimerPreset{900, 10};
        case 6:
            return TimerPreset{1800, 0};
        default:
            return TimerPreset{0, 0};
    }
}

bool GameClock::IsValidSide(const ClockSide side) {
    return side == ClockSide::White || side == ClockSide::Black;
}

ClockSide GameClock::NormalizeSide(const ClockSide side) {
    return side == ClockSide::Black ? ClockSide::Black : ClockSide::White;
}

uint32_t GameClock::GetRemainingMs(const ClockSide side) const {
    return (side == ClockSide::Black) ? m_remainingMsBlack : m_remainingMsWhite;
}

void GameClock::ResetTickBaseline() {
    m_hasTickBaseline = false;
    m_tickElapsed = fxe::ElapsedTime();
}

void GameClock::ResetToDisabled() {
    m_enabled = false;
    m_running = false;
    m_paused = false;
    m_timedOut = false;
    m_timeoutEventPending = false;
    m_activeSide = ClockSide::White;
    m_timedOutSide = ClockSide::White;
    m_remainingMsWhite = 0;
    m_remainingMsBlack = 0;
    m_incrementMs = 0;
    ResetTickBaseline();
}

void GameClock::LatchTimeout(const ClockSide losingSide, const bool emitEvent) {
    m_timedOut = true;
    m_timeoutEventPending = emitEvent;
    m_timedOutSide = NormalizeSide(losingSide);
    m_activeSide = m_timedOutSide;
    m_running = false;
    m_paused = false;

    uint32_t* const remaining = (m_timedOutSide == ClockSide::Black)
                                    ? &m_remainingMsBlack
                                    : &m_remainingMsWhite;
    *remaining = 0;
    ResetTickBaseline();
}

void GameClock::ResetFromTimerSetting(const uint32_t timerSetting,
                                      const ClockSide startSide) {
    const TimerPreset preset = TimerPresetForTimerSetting(timerSetting);

    m_enabled = preset.baseSeconds != 0U;
    m_running = false;
    m_paused = false;
    m_timedOut = false;
    m_timeoutEventPending = false;

    m_activeSide = NormalizeSide(startSide);
    m_timedOutSide = ClockSide::White;

    m_remainingMsWhite = SecondsToMs(preset.baseSeconds);
    m_remainingMsBlack = SecondsToMs(preset.baseSeconds);

    m_incrementMs = SecondsToMs(preset.incrementSeconds);

    ResetTickBaseline();
}

void GameClock::Start() {
    StartAt(fxe::Millis());
}

void GameClock::StartAt(const uint32_t startedAtMs) {
    if (!m_enabled || m_timedOut) {
        return;
    }

    m_running = true;
    m_paused = false;
    m_tickElapsed = fxe::ElapsedTime(startedAtMs);
    m_hasTickBaseline = true;
}

void GameClock::Stop() {
    if (m_timedOut) {
        return;
    }

    if (!m_running) {
        return;
    }

    Tick();
    m_running = false;
    m_paused = false;
    ResetTickBaseline();
}

void GameClock::SetPaused(const bool paused) {
    if (m_timedOut) {
        return;
    }

    if (!m_running) {
        return;
    }

    if (paused == m_paused) {
        return;
    }

    Tick();
    if (m_timedOut) {
        return;
    }

    m_paused = paused;
    m_tickElapsed.Reset();
    m_hasTickBaseline = true;
}

void GameClock::ConsumeElapsedMs() {
    if (!m_running || m_paused || !m_enabled || m_timedOut) {
        ResetTickBaseline();
        return;
    }

    if (!m_hasTickBaseline) {
        m_tickElapsed.Reset();
        m_hasTickBaseline = true;
        return;
    }

    const uint32_t elapsed = m_tickElapsed.ElapsedMs();
    m_tickElapsed.Reset();

    if (elapsed == 0U) {
        return;
    }

    uint32_t* remaining = (m_activeSide == ClockSide::Black) ? &m_remainingMsBlack : &m_remainingMsWhite;
    if (*remaining <= elapsed) {
        *remaining = 0;
        LatchTimeout(m_activeSide, true);
        return;
    }

    *remaining -= elapsed;
}

void GameClock::Tick() {
    ConsumeElapsedMs();
}

void GameClock::CommitMoveAndSwitch(const ClockSide newActiveSide) {
    if (m_timedOut) {
        return;
    }

    const ClockSide normalizedNewActiveSide = NormalizeSide(newActiveSide);
    if (!m_running) {
        m_activeSide = normalizedNewActiveSide;
        ResetTickBaseline();
        return;
    }

    Tick();

    if (m_timedOut) {
        return;
    }

    // Apply increment to the side that just moved (current active side before switch).
    if (m_enabled && m_incrementMs != 0U) {
        uint32_t* remaining = (m_activeSide == ClockSide::Black) ? &m_remainingMsBlack : &m_remainingMsWhite;
        const uint32_t before = *remaining;
        const uint32_t after = before + m_incrementMs;
        *remaining = (after < before) ? UINT32_MAX : after;
    }

    m_activeSide = normalizedNewActiveSide;
    m_tickElapsed.Reset();
    m_hasTickBaseline = true;
}

void GameClock::SwitchActiveSideWithoutIncrement(const ClockSide newActiveSide) {
    if (m_timedOut) {
        return;
    }

    const ClockSide normalizedNewActiveSide = NormalizeSide(newActiveSide);
    if (!m_running) {
        m_activeSide = normalizedNewActiveSide;
        ResetTickBaseline();
        return;
    }

    Tick();
    if (m_timedOut) {
        return;
    }

    m_activeSide = normalizedNewActiveSide;
    m_tickElapsed.Reset();
    m_hasTickBaseline = true;
}

GameClockState GameClock::CaptureState() {
    Tick();
    return GameClockState{
        .enabled = m_enabled,
        .running = m_running,
        .paused = m_paused,
        .timedOut = m_timedOut,
        .activeSide = m_activeSide,
        .timedOutSide = m_timedOutSide,
        .remainingMsWhite = m_remainingMsWhite,
        .remainingMsBlack = m_remainingMsBlack,
        .incrementMs = m_incrementMs,
    };
}

void GameClock::RestoreState(const GameClockState& state) {
    if (m_timedOut) {
        return;
    }

    if (!IsValidSide(state.activeSide) || !IsValidSide(state.timedOutSide) ||
        !state.enabled) {
        ResetToDisabled();
        return;
    }

    m_enabled = true;
    m_running = state.running;
    m_paused = state.running && state.paused;
    m_timedOut = false;
    m_timeoutEventPending = false;
    m_activeSide = state.activeSide;
    m_timedOutSide = state.timedOutSide;
    m_remainingMsWhite = state.remainingMsWhite;
    m_remainingMsBlack = state.remainingMsBlack;
    m_incrementMs = state.incrementMs;

    ResetTickBaseline();
    if (state.timedOut) {
        LatchTimeout(state.timedOutSide, false);
        return;
    }

    if (m_remainingMsWhite == 0U || m_remainingMsBlack == 0U) {
        const ClockSide losingSide = (m_remainingMsWhite == 0U &&
                                      m_remainingMsBlack != 0U)
                                         ? ClockSide::White
                                     : (m_remainingMsBlack == 0U && m_remainingMsWhite != 0U)
                                         ? ClockSide::Black
                                         : m_activeSide;
        LatchTimeout(losingSide, false);
        return;
    }

    if (m_running) {
        m_tickElapsed.Reset();
        m_hasTickBaseline = true;
    }
}

bool GameClock::ConsumeTimeoutEvent() {
    if (!m_timeoutEventPending) {
        return false;
    }

    m_timeoutEventPending = false;
    return true;
}

} // namespace runtime
