#pragma once

#include <cstddef>
#include <cstdint>

namespace fxe {

template <typename T, size_t CAPACITY>
class FixedRingBuffer final {
  public:
    static_assert(CAPACITY > 0, "FixedRingBuffer capacity must be > 0");

    void Clear() {
        m_head = 0;
        m_tail = 0;
        m_count = 0;
    }

    size_t Size() const { return m_count; }
    bool IsEmpty() const { return m_count == 0; }
    bool IsFull() const { return m_count >= CAPACITY; }

    bool Push(const T& value) {
        if (IsFull()) {
            return false;
        }

        m_items[m_tail] = value;
        m_tail = static_cast<size_t>((m_tail + 1U) % CAPACITY);
        ++m_count;
        return true;
    }

    bool TryPop(T& out) {
        if (IsEmpty()) {
            return false;
        }

        out = m_items[m_head];
        m_head = static_cast<size_t>((m_head + 1U) % CAPACITY);
        --m_count;
        return true;
    }

  private:
    T m_items[CAPACITY] = {};
    size_t m_head = 0;
    size_t m_tail = 0;
    size_t m_count = 0;
};

} // namespace fxe
