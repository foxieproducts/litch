#pragma once

#include <cstring>
#include <string>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/console_transport_memory.hpp"

namespace fxe::test_support {

inline void FeedConsole(Console& console, MemoryConsoleTransport& transport, const char* bytes) {
    if (!bytes) {
        return;
    }

    transport.PushBytes(bytes, std::strlen(bytes));
    console.Update();
}

inline void PressEnter(Console& console, MemoryConsoleTransport& transport) {
    transport.PushChar('\r');
    console.Update();
}

inline void RunCommand(Console& console, MemoryConsoleTransport& transport, const char* cmd) {
    FeedConsole(console, transport, cmd);
    PressEnter(console, transport);
}

class ConsoleOutputCapture final {
  public:
    void Attach(MemoryConsoleTransport& transport) {
        transport.SetOutputCallback([this](const char* data, const size_t len) {
            if (!data || len == 0) {
                return;
            }

            m_output.append(data, len);
        });
    }

    void Clear() { m_output.clear(); }

    const std::string& Str() const { return m_output; }

  private:
    std::string m_output;
};

} // namespace fxe::test_support
