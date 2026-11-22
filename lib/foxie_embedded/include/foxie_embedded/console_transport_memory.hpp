#pragma once

#include <cstddef>
#include <cstdint>

#include "foxie_embedded/console_transport.hpp"
#include "foxie_embedded/fixed_function.hpp"

namespace fxe {

class MemoryConsoleTransport final : public IConsoleTransport {
  public:
    using OutputCallback = fxe::FixedFunction<void(const char* data, size_t len), 64>;

    static constexpr size_t INPUT_CAPACITY = 8192;

    void PushChar(const char c) { PushByte(static_cast<uint8_t>(c)); }

    void PushBytes(const char* data, const size_t len) {
        for (size_t i = 0; i < len; ++i) {
            PushByte(static_cast<uint8_t>(data[i]));
        }
    }

    void SetOutputCallback(OutputCallback cb) {
        m_outCb = std::move(cb);
    }

    bool TryReadChar(char& outChar) override {
        if (m_count == 0) {
            return false;
        }

        outChar = static_cast<char>(m_buf[m_head]);
        m_head = (m_head + 1) % INPUT_CAPACITY;
        --m_count;

        return true;
    }

    void Write(const char* data, const size_t len) override {
        if (!m_outCb || !data || len == 0) {
            return;
        }
        m_outCb(data, len);
    }

    void Flush() override {}

  private:
    void PushByte(const uint8_t b) {
        if (m_count >= INPUT_CAPACITY) {
            // Input overflow. Reset to avoid unbounded backlog.
            m_head = 0;
            m_tail = 0;
            m_count = 0;
        }

        m_buf[m_tail] = b;
        m_tail = (m_tail + 1) % INPUT_CAPACITY;
        ++m_count;
    }

    uint8_t m_buf[INPUT_CAPACITY] = {};
    size_t m_head = 0;
    size_t m_tail = 0;
    size_t m_count = 0;

    OutputCallback m_outCb;
};

} // namespace fxe
