#pragma once

#include <cstddef>
#include <cstdint>

#include "foxie_embedded/logger.hpp"

namespace fxe
{

struct HexDumpOptions
{
    size_t bytesPerLine = 16;
    size_t maxBytes = 128;
    bool showAscii = true;
    bool showAddress = true;
    uint8_t addressBytes = 4;
    size_t startOffset = 0;
    fxe::Level level = fxe::Level::DEBUG;
};

// \brief Log a hex dump using the foxie_embedded logger.
void LogHexDump(const void* data,
                size_t len,
                const char* label = nullptr,
                const HexDumpOptions& options = {});

} // namespace fxe
