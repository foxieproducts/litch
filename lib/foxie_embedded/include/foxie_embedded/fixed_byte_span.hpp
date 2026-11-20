#pragma once

#include <cstddef>
#include <cstdint>

#include "foxie_embedded/span.hpp"

namespace fxe {

template <size_t N>
class FixedByteSpan final {
  public:
    FixedByteSpan() = default;

    ByteSpan Span() { return ByteSpan(m_data, m_size); }
    ConstByteSpan Span() const { return ConstByteSpan(m_data, m_size); }

    size_t Size() const { return m_size; }
    size_t Capacity() const { return N; }
    bool IsEmpty() const { return m_size == 0; }

    void Clear() { m_size = 0; }

    bool PushBack(const uint8_t value) {
        if (m_size >= N) {
            return false;
        }
        m_data[m_size++] = value;
        return true;
    }

    uint8_t* Data() { return m_data; }
    const uint8_t* Data() const { return m_data; }

  private:
    uint8_t m_data[N] = {};
    size_t m_size = 0;
};

} // namespace fxe
