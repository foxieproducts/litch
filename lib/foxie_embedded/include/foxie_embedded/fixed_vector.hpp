#pragma once

#include <cstddef>
#include <cstdint>

namespace fxe {

template <typename T, size_t CAPACITY>
class FixedVector final {
  public:
    static_assert(CAPACITY > 0, "FixedVector capacity must be > 0");

    size_t Size() const { return m_size; }
    size_t Capacity() const { return CAPACITY; }
    bool IsEmpty() const { return m_size == 0; }
    bool IsFull() const { return m_size >= CAPACITY; }

    void Clear() { m_size = 0; }

    bool PushBack(const T& value) {
        if (IsFull()) {
            return false;
        }

        m_items[m_size] = value;
        ++m_size;
        return true;
    }

    bool TryPopBack(T& outValue) {
        if (IsEmpty()) {
            return false;
        }

        --m_size;
        outValue = m_items[m_size];
        return true;
    }

    T& operator[](const size_t index) { return m_items[index]; }
    const T& operator[](const size_t index) const { return m_items[index]; }

    T& Back() { return m_items[m_size - 1]; }
    const T& Back() const { return m_items[m_size - 1]; }

  private:
    T m_items[CAPACITY] = {};
    size_t m_size = 0;
};

} // namespace fxe
