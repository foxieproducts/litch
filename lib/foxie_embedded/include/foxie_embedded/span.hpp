#pragma once

#include <cstddef>
#include <cstdint>

namespace fxe {

template <typename T>
class Span final {
  public:
    Span() = default;
    Span(T* data, const size_t size)
        : m_data(data), m_size(size) {}

    template <size_t N>
    Span(T (&data)[N])
        : m_data(data), m_size(N) {}

    T* Data() { return m_data; }
    const T* Data() const { return m_data; }
    size_t Size() const { return m_size; }
    bool IsEmpty() const { return m_size == 0; }

    T& operator[](const size_t index) { return m_data[index]; }
    const T& operator[](const size_t index) const { return m_data[index]; }

    T* begin() { return m_data; }
    T* end() { return m_data + m_size; }
    const T* begin() const { return m_data; }
    const T* end() const { return m_data + m_size; }

  private:
    T* m_data = nullptr;
    size_t m_size = 0;
};

using ByteSpan = Span<uint8_t>;
using ConstByteSpan = Span<const uint8_t>;

} // namespace fxe
