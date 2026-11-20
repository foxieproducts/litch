#pragma once

#include <cstdint>

#include "foxie_embedded/millis.hpp"

namespace fxe {

// Simple helper for measuring elapsed time in milliseconds.
//
// Design goals:
// - Callers do not need to thread `nowMs` through callsites.
// - Time can be overridden for unit tests and deterministic replays.
// - Safe for typical wraparound usage when intervals are reasonably small.
// - Tiny and header-only.
class ElapsedTime final {
  public:
    ElapsedTime() = default;

    explicit ElapsedTime(const uint32_t startMs) : m_startMs(startMs), m_started(true) {}

    static ElapsedTime StartNow() {
        ElapsedTime t;
        t.Reset();
        return t;
    }

    void Reset() {
        m_startMs = Millis();
        m_started = true;
    }

    bool Started() const { return m_started; }

    uint32_t StartMs() const { return m_startMs; }

    uint32_t ElapsedMs() const {
        if (!m_started) {
            return 0;
        }

        // Unsigned subtraction is wrap-safe for short durations.
        return Millis() - m_startMs;
    }

    bool HasElapsed(const uint32_t durationMs) const {
        return ElapsedMs() >= durationMs;
    }

    uint32_t RemainingMs(const uint32_t durationMs) const {
        const uint32_t e = ElapsedMs();
        return (e >= durationMs) ? 0u : (durationMs - e);
    }

  private:
    uint32_t m_startMs = 0;
    bool m_started = false;
};

} // namespace fxe
