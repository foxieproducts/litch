#pragma once

#include <cstddef>
#include <cstdint>

#include "foxie_embedded/deadline_timer.hpp"

namespace fxe {

// Fixed-capacity manager for DeadlineTimer instances.
//
// This is useful when a system wants to keep all timers updated from one place
// (e.g., a main loop) without threading `nowMs` everywhere.
template <size_t CAPACITY>
class DeadlineTimerManager final {
  public:
    using Timer = DeadlineTimer;
    using Callback = DeadlineTimer::Callback;

    void Reset() {
        for (size_t i = 0; i < CAPACITY; ++i) {
            m_used[i] = false;
            m_timers[i].Stop();
            m_timers[i].ClearCallback();
        }
    }

    // Starts a timer and returns its id, or -1 on failure.
    int Start(const uint32_t durationMs, Callback cb) {
        for (size_t i = 0; i < CAPACITY; ++i) {
            if (m_used[i]) {
                continue;
            }

            m_used[i] = true;
            m_timers[i].SetCallback(std::move(cb));
            m_timers[i].Start(durationMs);
            return static_cast<int>(i);
        }

        return -1;
    }

    void Cancel(const int id) {
        if (id < 0 || static_cast<size_t>(id) >= CAPACITY) {
            return;
        }

        const size_t idx = static_cast<size_t>(id);
        m_timers[idx].Stop();
        m_timers[idx].ClearCallback();
        m_used[idx] = false;
    }

    void Update() {
        for (size_t i = 0; i < CAPACITY; ++i) {
            if (!m_used[i]) {
                continue;
            }

            if (m_timers[i].Update()) {
                // One-shot: free the slot after firing.
                m_timers[i].ClearCallback();
                m_used[i] = false;
            }
        }
    }

  private:
    Timer m_timers[CAPACITY] = {};
    bool m_used[CAPACITY] = {};
};

} // namespace fxe
