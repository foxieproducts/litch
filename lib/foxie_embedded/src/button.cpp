#include "foxie_embedded/button.hpp"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace fxe {

Button::Button(const uint8_t pin, const bool pressedState) {
#ifdef ARDUINO
    SetDetectionFn([pin, pressedState]() { return digitalRead(pin) == pressedState; });
#else
    (void)pin;
    (void)pressedState;
#endif
}

void Button::Update() {
    if (!m_enabled) {
        return;
    }

    if (!m_detectionFn) {
        return;
    }

    m_rawDown = m_detectionFn();

    if (IsHoldoffActive()) {
        return;
    }

    // Debounce toward a new stable state.
    if (!m_debouncing) {
        if (m_rawDown != m_isPressed) {
            m_debouncing = true;
            m_debounceTargetDown = m_rawDown;
            m_debounceTime.Reset();
        }
    }

    if (m_debouncing) {
        // If input bounces during the window, restart the debounce toward the
        // most recent raw state.
        if (m_rawDown != m_debounceTargetDown) {
            m_debounceTargetDown = m_rawDown;
            m_debounceTime.Reset();
        }

        const uint32_t ageMs = m_debounceTime.ElapsedMs();
        if (ageMs >= m_config.debounceMs) {
            m_debouncing = false;

            const bool newPressed = m_debounceTargetDown;
            if (newPressed != m_isPressed) {
                m_isPressed = newPressed;
                m_stateTime.Reset();

                if (m_ignoreUntilRelease) {
                    if (!m_isPressed) {
                        m_ignoreUntilRelease = false;
                    }
                } else {
                    if (m_isPressed) {
                        m_longHoldEventSent = false;
                        if (!MustDelayBeforePress()) {
                            SendEvent(Event::Press);
                        }
                    } else {
                        SendEvent(Event::Release);
                        if (m_config.holdoffMs > 0) {
                            m_holdoffActive = true;
                            m_holdoffTime.Reset();
                        }
                    }
                }
            }
        } else {
            // No timed events while the stable state is still being decided.
            return;
        }
    }

    if (!m_ignoreUntilRelease && ShouldSendDelayedPress()) {
        SendEvent(Event::Press);
    }

    if (!m_ignoreUntilRelease && ShouldSendLongHold()) {
        SendEvent(Event::LongHold);
    }

    if (!m_ignoreUntilRelease && ShouldRepeat()) {
        SendEvent(Event::Repeat);
    }
}

void Button::FeedEdge(const bool rawDown, const uint32_t atMs) {
    m_rawDown = rawDown;
    if (!m_enabled || (m_holdoffActive && m_config.holdoffMs != 0U)) return;
    if (m_config.immediatePress) {
        if (rawDown) {
            // A reassertion inside release debounce is contact bounce, not a
            // second press. The original held interval remains authoritative.
            m_debouncing = false;
            if (!m_isPressed) {
                m_isPressed = true;
                m_stateTime = ElapsedTime(atMs);
                m_longHoldEventSent = false;
                if (!m_ignoreUntilRelease) SendEventAt(Event::Press, atMs);
            }
        } else if (m_isPressed && !m_debouncing) {
            m_debouncing = true;
            m_debounceTargetDown = false;
            m_debounceTime = ElapsedTime(atMs);
        }
        return;
    }
    if (!m_debouncing && rawDown != m_isPressed) {
        m_debouncing = true;
        m_debounceTargetDown = rawDown;
        m_debounceTime = ElapsedTime(atMs);
    } else if (m_debouncing && rawDown != m_debounceTargetDown) {
        m_debounceTargetDown = rawDown;
        m_debounceTime = ElapsedTime(atMs);
    }
}

bool Button::PendingReleaseAt(uint32_t& atMs) const {
    if (!m_enabled || !m_config.immediatePress || !m_isPressed ||
        !m_debouncing || m_debounceTargetDown) return false;
    atMs = m_debounceTime.StartMs();
    return true;
}

bool Button::NextDeadline(uint32_t& atMs) const {
    if (!m_enabled) return false;
    if (m_holdoffActive && m_config.holdoffMs != 0U) {
        atMs = m_holdoffTime.StartMs() + m_config.holdoffMs;
        return true;
    }
    if (m_debouncing) {
        atMs = m_debounceTime.StartMs() + m_config.debounceMs;
        return true;
    }
    if (!m_isPressed || m_ignoreUntilRelease) return false;
    bool found = false;
    const auto consider = [&](const uint32_t deadline) {
        if (!found || static_cast<int32_t>(deadline - atMs) < 0) atMs = deadline;
        found = true;
    };
    const uint32_t startedAt = m_stateTime.StartMs();
    if (!m_pressEventSent && m_config.beforePressMs != 0U) consider(startedAt + m_config.beforePressMs);
    if (!m_longHoldEventSent && m_config.longHoldMs != 0U) consider(startedAt + m_config.longHoldMs);
    if (m_pressEventSent && m_config.canRepeat && m_config.repeatRateMs != 0U) {
        const uint32_t fromLastEvent = m_repeatTime.StartMs() - startedAt + m_config.repeatRateMs;
        const uint32_t fromState = fromLastEvent > m_config.beforeRepeatMs
            ? fromLastEvent : m_config.beforeRepeatMs;
        consider(startedAt + fromState);
    }
    return found;
}

void Button::AdvanceOneAt(const uint32_t atMs) {
    if (!m_enabled) return;
    const auto reached = [atMs](const uint32_t deadline) {
        return static_cast<int32_t>(atMs - deadline) >= 0;
    };
    if (m_holdoffActive && m_config.holdoffMs != 0U) {
        if (!reached(m_holdoffTime.StartMs() + m_config.holdoffMs)) return;
        m_holdoffActive = false;
        FeedEdge(m_rawDown, atMs);
        return;
    }
    if (m_debouncing) {
        if (!reached(m_debounceTime.StartMs() + m_config.debounceMs)) return;
        const uint32_t effectiveAt = m_config.immediatePress && !m_debounceTargetDown
            ? m_debounceTime.StartMs() : atMs;
        m_debouncing = false;
        if (m_isPressed == m_debounceTargetDown) return;
        m_isPressed = m_debounceTargetDown;
        m_stateTime = ElapsedTime(effectiveAt);
        if (m_ignoreUntilRelease) {
            if (!m_isPressed) m_ignoreUntilRelease = false;
            return;
        }
        if (m_isPressed) {
            m_longHoldEventSent = false;
            if (!MustDelayBeforePress()) SendEventAt(Event::Press, effectiveAt);
        } else {
            SendEventAt(Event::Release, effectiveAt);
            if (m_config.holdoffMs != 0U) {
                m_holdoffActive = true;
                m_holdoffTime = ElapsedTime(atMs);
            }
        }
        return;
    }
    if (!m_isPressed || m_ignoreUntilRelease) return;
    const uint32_t startedAt = m_stateTime.StartMs();
    if (!m_pressEventSent && m_config.beforePressMs != 0U && reached(startedAt + m_config.beforePressMs)) {
        SendEventAt(Event::Press, atMs);
    } else if (!m_longHoldEventSent && m_config.longHoldMs != 0U && reached(startedAt + m_config.longHoldMs)) {
        SendEventAt(Event::LongHold, atMs);
    } else if (m_pressEventSent && m_config.canRepeat && m_config.repeatRateMs != 0U &&
               reached(startedAt + m_config.beforeRepeatMs) && reached(m_repeatTime.StartMs() + m_config.repeatRateMs)) {
        SendEventAt(Event::Repeat, atMs);
    }
}

uint32_t Button::MsInState() const {
    if (!m_enabled) {
        return 0;
    }
    if (!m_stateTime.Started()) {
        return 0;
    }
    return m_stateTime.ElapsedMs();
}

void Button::SetEnabled(const bool enabled) {
    m_enabled = enabled;
    Reset();
}

void Button::Reset() {
    m_rawDown = false;

    m_isPressed = false;
    m_stateTime.Reset();

    m_debouncing = false;
    m_debounceTargetDown = false;
    m_debounceTime.Reset();

    m_holdoffActive = false;
    m_holdoffTime.Reset();

    m_pressEventSent = false;
    m_longHoldEventSent = false;

    m_repeatTime.Reset();

    m_ignoreUntilRelease = false;
}

void Button::SeedRawDown(const bool rawDown) {
    m_rawDown = rawDown;
    m_isPressed = rawDown;
    m_stateTime.Reset();

    m_debouncing = false;
    m_debounceTargetDown = rawDown;
    m_debounceTime.Reset();

    m_holdoffActive = false;
    m_holdoffTime.Reset();

    m_pressEventSent = false;
    m_longHoldEventSent = false;
    m_repeatTime.Reset();

    m_ignoreUntilRelease = rawDown;
}

void Button::SendEvent(const Event evt) {
    SendEventAt(evt, Millis());
}

void Button::SendEventAt(const Event evt, const uint32_t atMs) {
    if (m_config.onEvent) {
        m_config.onEvent(evt);
    }
    if (m_config.onTimedEvent) m_config.onTimedEvent(evt, atMs);

    if (evt == Event::Press) {
        m_pressEventSent = true;
        m_repeatTime = ElapsedTime(atMs);
    }

    if (evt == Event::Release) {
        m_pressEventSent = false;
        m_longHoldEventSent = false;
        m_repeatTime = ElapsedTime(atMs);
    }

    if (evt == Event::LongHold) {
        m_longHoldEventSent = true;
        m_repeatTime = ElapsedTime(atMs);
    }

    if (evt == Event::Repeat) {
        m_repeatTime = ElapsedTime(atMs);
    }
}

bool Button::MustDelayBeforePress() const {
    return m_config.beforePressMs > 0;
}

bool Button::ShouldSendDelayedPress() const {
    return m_isPressed && !m_pressEventSent && MustDelayBeforePress() &&
           MsInState() >= m_config.beforePressMs;
}

bool Button::ShouldSendLongHold() const {
    return m_isPressed && !m_longHoldEventSent && m_config.longHoldMs > 0 &&
           MsInState() >= m_config.longHoldMs;
}

bool Button::ShouldRepeat() {
    if (!m_isPressed || !m_config.canRepeat || !m_pressEventSent) {
        return false;
    }

    if (m_config.repeatRateMs == 0) {
        return false;
    }

    if (MsInState() < m_config.beforeRepeatMs) {
        return false;
    }

    if (IsHoldoffActive() || m_debouncing) {
        return false;
    }

    return m_repeatTime.ElapsedMs() >= m_config.repeatRateMs;
}

bool Button::IsHoldoffActive() {
    if (!m_holdoffActive || m_config.holdoffMs == 0) {
        return false;
    }

    if (m_holdoffTime.ElapsedMs() >= m_config.holdoffMs) {
        m_holdoffActive = false;
        return false;
    }
    return true;
}

} // namespace fxe
