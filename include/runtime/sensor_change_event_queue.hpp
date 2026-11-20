#pragma once

#include <array>
#include <cstdint>

#include "runtime/interaction_domain_types.hpp"

namespace runtime {

class SensorChangeEventQueue {
  public:
    static constexpr uint8_t CAPACITY = 64;

    bool Enqueue(const SensorChangeEvent& event) {
        if (m_size >= CAPACITY) {
            // Overflow policy: drop the newest event and preserve the existing
            // FIFO contents so already-captured deltas drain deterministically.
            return false;
        }

        m_events[m_tail] = event;
        m_tail = NextIndex(m_tail);
        ++m_size;
        return true;
    }

    bool TryDequeue(SensorChangeEvent& outEvent) {
        if (m_size == 0) {
            return false;
        }

        outEvent = m_events[m_head];
        m_head = NextIndex(m_head);
        --m_size;
        return true;
    }

    void Clear() {
        m_head = 0;
        m_tail = 0;
        m_size = 0;
    }

    uint8_t Size() const {
        return m_size;
    }

  private:
    static constexpr uint8_t NextIndex(const uint8_t index) {
        return static_cast<uint8_t>((index + 1U) % CAPACITY);
    }

    std::array<SensorChangeEvent, CAPACITY> m_events{};
    uint8_t m_head = 0;
    uint8_t m_tail = 0;
    uint8_t m_size = 0;
};

} // namespace runtime