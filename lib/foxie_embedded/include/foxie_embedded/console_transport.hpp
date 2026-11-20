#pragma once

#include <cstddef>
#include <cstdint>

namespace fxe {

enum class ConsoleInputSource { Unknown, Local, Remote, Mixed };

class IConsoleTransport {
  public:
    virtual ~IConsoleTransport() = default;

    // \brief Non-blocking attempt to read one byte.
    virtual bool TryReadChar(char& outChar) = 0;

    // Source of the last successfully read byte. Unidentified transports fail
    // closed for commands that require a local terminal.
    virtual ConsoleInputSource LastInputSource() const {
        return ConsoleInputSource::Unknown;
    }

    // Existing remote admission/session lifetime; zero is unavailable. No new
    // session is created here. The editor cannot retain remote input across it.
    virtual uint64_t RemoteInputGeneration() const { return 0U; }

    virtual void Write(const char* data, size_t len) = 0;

    virtual void Flush() = 0;

    void WriteString(const char* s);
};

} // namespace fxe
