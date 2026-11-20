#include "foxie_embedded/console_transport.hpp"

#include <cstring>

void fxe::IConsoleTransport::WriteString(const char* s) {
    if (!s) {
        return;
    }
    Write(s, std::strlen(s));
}
