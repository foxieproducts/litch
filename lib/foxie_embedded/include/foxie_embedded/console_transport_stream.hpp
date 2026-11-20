#pragma once

#include "foxie_embedded/console_transport.hpp"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace fxe {

class StreamConsoleTransport final : public IConsoleTransport {
  public:
#ifdef ARDUINO
    explicit StreamConsoleTransport(Stream& s) : m_stream(s) {}

    bool TryReadChar(char& outChar) override {
        if (m_stream.available() <= 0) {
            return false;
        }
        outChar = static_cast<char>(m_stream.read());
        return true;
    }

    void Write(const char* data, const size_t len) override {
        if (!data || len == 0) {
            return;
        }
        m_stream.write(reinterpret_cast<const uint8_t*>(data), len);
    }

    void Flush() override { m_stream.flush(); }

  private:
    Stream& m_stream;
#else
    explicit StreamConsoleTransport(int) {}

    bool TryReadChar(char&) override { return false; }
    void Write(const char*, size_t) override {}
    void Flush() override {}
#endif
};

} // namespace fxe
