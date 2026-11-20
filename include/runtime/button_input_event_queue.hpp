#pragma once

#include <array>
#include <cstdint>

#include "runtime/interaction_domain_types.hpp"

namespace runtime {

class ButtonInputEventQueue {
  public:
    static constexpr uint8_t CAPACITY = 16;

    bool Enqueue(const ButtonInputEvent& event) {
        if (m_size >= CAPACITY) {
            // Overflow policy: drop the newest event and keep older queued
            // events in FIFO order so already-captured input dispatches first.
            return false;
        }

        m_events[m_tail] = event;
        m_tail = NextIndex(m_tail);
        ++m_size;
        return true;
    }

    bool EnqueueFront(const ButtonInputEvent& event) {
        if (m_size >= CAPACITY) {
            return false;
        }
        m_head = PreviousIndex(m_head);
        m_events[m_head] = event;
        ++m_size;
        return true;
    }

    bool TryPeek(ButtonInputEvent& outEvent) const {
        if (m_size == 0U) return false;
        outEvent = m_events[m_head];
        return true;
    }

    bool TryDequeue(ButtonInputEvent& outEvent) {
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

    static constexpr uint8_t PreviousIndex(const uint8_t index) {
        return static_cast<uint8_t>((index + CAPACITY - 1U) % CAPACITY);
    }

    std::array<ButtonInputEvent, CAPACITY> m_events{};
    uint8_t m_head = 0;
    uint8_t m_tail = 0;
    uint8_t m_size = 0;
};

} // namespace runtime
