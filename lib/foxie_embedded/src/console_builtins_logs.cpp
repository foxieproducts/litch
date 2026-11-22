#include "foxie_embedded/console.hpp"

#include "foxie_embedded/logger.hpp"

namespace fxe {

void Console::InstallBuiltinLogs() {
    auto logsCmd = [this](const Args&) -> bool {
        fxe::DumpBufferedLines([this](const char* line) { PrintLn("%s", line); });
        return true;
    };
    RegisterCommand("logs", "Show buffered logs", logsCmd);
}

} // namespace fxe
