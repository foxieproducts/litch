#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/fixed_function.hpp"

namespace fxe {

class Button final {
  public:
    enum class Event : uint8_t {
        Press = 0,
        Repeat = 1,
        Release = 2,
        LongHold = 3,
    };

    using DetectionFn = fxe::FixedFunction<bool(), 32>;

    struct Config {
        bool canRepeat = true;

        // Milliseconds.
        uint32_t repeatRateMs = 50;
        uint32_t beforeRepeatMs = 400;
        uint32_t beforePressMs = 0;
        uint32_t debounceMs = 20;
        uint32_t holdoffMs = 0;
        uint32_t longHoldMs = 0;

        // Heap-free callback (supports lambdas with captures).
        fxe::FixedFunction<void(Event), 32> onEvent;
        // Captured-edge input only: accept the active edge, rearm after a
        // stable inactive debounce window. Polling keeps symmetric debounce.
        bool immediatePress = false;
        fxe::FixedFunction<void(Event, uint32_t), 32> onTimedEvent;
    };

    Button() = default;
    Button(uint8_t pin, bool pressedState = false);

    void Update();

    // The owner advances all buttons chronologically before feeding an edge.
    void FeedEdge(bool rawDown, uint32_t atMs);
    bool NextDeadline(uint32_t& atMs) const;
    void AdvanceOneAt(uint32_t atMs);
    bool PendingReleaseAt(uint32_t& atMs) const;

    void SetDetectionFn(const DetectionFn& fn) { m_detectionFn = fn; }
    void SeedRawDown(bool rawDown);

    bool IsPressed() const { return m_enabled && m_isPressed; }

    uint32_t MsInState() const;

    void SetEnabled(bool enabled);

    void Reset();

    void SetConfig(const Config& cfg) { m_config = cfg; }
    const Config& GetConfig() const { return m_config; }

  private:
    void SendEvent(Event evt);
    void SendEventAt(Event evt, uint32_t atMs);

    bool MustDelayBeforePress() const;
    bool ShouldSendDelayedPress() const;
    bool ShouldSendLongHold() const;
    bool ShouldRepeat();

    bool IsHoldoffActive();

    Config m_config;

    bool m_enabled = true;

    // Raw input tracking.
    bool m_rawDown = false;

    DetectionFn m_detectionFn;
    bool m_ignoreUntilRelease = false;

    // Debounce state: when raw differs from stable, wait until it remains
    // unchanged for debounceMs, then commit stable.
    bool m_debouncing = false;
    bool m_debounceTargetDown = false;
    fxe::ElapsedTime m_debounceTime;

    // Stable state.
    bool m_isPressed = false;
    fxe::ElapsedTime m_stateTime;

    // Holdoff state.
    bool m_holdoffActive = false;
    fxe::ElapsedTime m_holdoffTime;

    // Event bookkeeping.
    bool m_pressEventSent = false;
    bool m_longHoldEventSent = false;

    fxe::ElapsedTime m_repeatTime;
};

} // namespace fxe
