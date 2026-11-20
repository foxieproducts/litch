#pragma once

#include <cstddef>
#include <cstdint>

namespace fxe {

template <size_t CAPACITY>
class FixedByteBuffer final {
  public:
    static_assert(CAPACITY > 0, "FixedByteBuffer capacity must be > 0");

    void Clear() {
        m_head = 0;
        m_tail = 0;
        m_count = 0;
    }

    size_t Size() const { return m_count; }
    size_t Capacity() const { return CAPACITY; }
    bool IsEmpty() const { return m_count == 0; }
    bool IsFull() const { return m_count >= CAPACITY; }

    bool Push(const uint8_t value) {
        if (IsFull()) {
            return false;
        }

        m_items[m_tail] = value;
        m_tail = static_cast<size_t>((m_tail + 1U) % CAPACITY);
        ++m_count;
        return true;
    }

    bool TryPop(uint8_t& outValue) {
        if (IsEmpty()) {
            return false;
        }

        outValue = m_items[m_head];
        m_head = static_cast<size_t>((m_head + 1U) % CAPACITY);
        --m_count;
        return true;
    }

    size_t Write(const uint8_t* data, const size_t size) {
        if (!data || size == 0) {
            return 0;
        }

        size_t written = 0;
        for (size_t i = 0; i < size; ++i) {
            if (!Push(data[i])) {
                break;
            }
            ++written;
        }
        return written;
    }

    size_t Read(uint8_t* outData, const size_t size) {
        if (!outData || size == 0) {
            return 0;
        }

        size_t read = 0;
        for (size_t i = 0; i < size; ++i) {
            uint8_t value = 0;
            if (!TryPop(value)) {
                break;
            }
            outData[i] = value;
            ++read;
        }
        return read;
    }

  private:
    uint8_t m_items[CAPACITY] = {};
    size_t m_head = 0;
    size_t m_tail = 0;
    size_t m_count = 0;
};

} // namespace fxe
