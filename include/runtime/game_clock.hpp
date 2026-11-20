#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"

namespace runtime {

enum class ClockSide : uint8_t {
    White = 0,
    Black = 1,
};

struct TimerPreset {
    uint32_t baseSeconds = 0;
    uint32_t incrementSeconds = 0;

    constexpr TimerPreset() = default;
    constexpr TimerPreset(const uint32_t baseSeconds_, const uint32_t incrementSeconds_)
        : baseSeconds(baseSeconds_), incrementSeconds(incrementSeconds_) {}
};

struct GameClockState {
    bool enabled = false;
    bool running = false;
    bool paused = false;
    bool timedOut = false;
    ClockSide activeSide = ClockSide::White;
    ClockSide timedOutSide = ClockSide::White;
    uint32_t remainingMsWhite = 0;
    uint32_t remainingMsBlack = 0;
    uint32_t incrementMs = 0;
};

// TIMER setting value mapping (docs/spec/timer_semantics.md).
// Invalid values map to OFF (0) for safety.
TimerPreset TimerPresetForTimerSetting(uint32_t timerSetting);

class GameClock final {
  public:
    GameClock() = default;

    void ResetFromTimerSetting(uint32_t timerSetting, ClockSide startSide);

    void Start();
    // Start from an already-observed boundary. This keeps cooperative-loop
    // latency from giving away time after an authoritative board event.
    void StartAt(uint32_t startedAtMs);
    void Stop();

    void SetPaused(bool paused);

    // Update accounting using the injected clock.
    void Tick();

    // Call at the moment a move is committed to the engine.
    // Applies increment to the side that just moved and switches active side.
    void CommitMoveAndSwitch(ClockSide newActiveSide);

    // Switches the active side without applying increment.
    // Intended for undo/cancel-undo and other non-move engine mutations.
    void SwitchActiveSideWithoutIncrement(ClockSide newActiveSide);

    GameClockState CaptureState();
    void RestoreState(const GameClockState& state);

    bool IsEnabled() const { return m_enabled; }
    bool IsRunning() const { return m_running; }
    bool IsPaused() const { return m_paused; }
    bool HasTimedOut() const { return m_timedOut; }

    ClockSide GetActiveSide() const { return m_activeSide; }
    ClockSide GetTimedOutSide() const { return m_timedOutSide; }

    // Returns true exactly once after this clock reaches zero.
    // The durable timeout state remains available through HasTimedOut().
    bool ConsumeTimeoutEvent();

    uint32_t GetRemainingMs(ClockSide side) const;

  private:
    static bool IsValidSide(ClockSide side);
    static ClockSide NormalizeSide(ClockSide side);

    void ResetTickBaseline();
    void ConsumeElapsedMs();
    void LatchTimeout(ClockSide losingSide, bool emitEvent);
    void ResetToDisabled();

    bool m_enabled = false;
    bool m_running = false;
    bool m_paused = false;
    bool m_timedOut = false;
    bool m_timeoutEventPending = false;

    ClockSide m_activeSide = ClockSide::White;
    ClockSide m_timedOutSide = ClockSide::White;

    uint32_t m_remainingMsWhite = 0;
    uint32_t m_remainingMsBlack = 0;

    uint32_t m_incrementMs = 0;

    bool m_hasTickBaseline = false;
    fxe::ElapsedTime m_tickElapsed;
};

} // namespace runtime
