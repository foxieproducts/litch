#pragma once

#include <cstddef>
#include <cstdint>

#include "runtime/app_event.hpp"

namespace runtime {

// Fixed-size, heap-free event queue.
// Ordering is deterministic: events are popped in insertion order (seq).
template <size_t CAPACITY>
class EventBus final {
  public:
    static_assert(CAPACITY > 0U, "EventBus requires at least one slot");

    static constexpr size_t Capacity() { return CAPACITY; }

    void Reset() {
        m_head = 0;
        m_tail = 0;
        m_count = 0;
        m_nextSeq = 1;
    }

    size_t Count() const { return m_count; }

    // Reserve every sequence through an externally recorded event. Returns
    // false only when no strictly later uint32_t sequence can be represented.
    bool AdvanceSequencePast(const uint32_t sequence) {
        if (sequence == UINT32_MAX || m_nextSeq == 0U) {
            return false;
        }
        if (sequence >= m_nextSeq) {
            m_nextSeq = sequence + 1U;
        }
        return true;
    }

    bool Push(const AppEvent& evt, uint32_t* outSequence = nullptr) {
        if (m_count >= CAPACITY || m_nextSeq == 0U) {
            return false;
        }

        m_buf[m_tail] = evt;
        m_buf[m_tail].seq = m_nextSeq;
        m_nextSeq = (m_nextSeq == UINT32_MAX) ? 0U : m_nextSeq + 1U;
        if (outSequence != nullptr) {
            *outSequence = m_buf[m_tail].seq;
        }
        m_tail = (m_tail + 1) % CAPACITY;
        ++m_count;
        return true;
    }

    bool TryPop(AppEvent& outEvt) {
        if (m_count == 0) {
            return false;
        }

        outEvt = m_buf[m_head];
        m_head = (m_head + 1) % CAPACITY;
        --m_count;
        return true;
    }

  private:
    AppEvent m_buf[CAPACITY] = {};
    size_t m_head = 0;
    size_t m_tail = 0;
    size_t m_count = 0;
    uint32_t m_nextSeq = 1;
};

// Production ingress drains each event before invoking its handler, so one
// slot preserves global sequencing and explicit overflow semantics.
constexpr size_t APP_EVENT_QUEUE_CAPACITY = 1U;
using AppEventQueue = EventBus<APP_EVENT_QUEUE_CAPACITY>;

} // namespace runtime
