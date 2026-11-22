#include "foxie_embedded/hex_dump.hpp"

#include <algorithm>
#include <cctype>

#include "foxie_embedded/fixed_string.hpp"
#include "foxie_embedded/format.hpp"
#include "foxie_embedded/logger.hpp"

namespace {
void LogLineAtLevel(const fxe::Level level, const char* line) {
    if (!line) {
        return;
    }

    switch (level) {
        case fxe::Level::TRACE:
            fxe::Trace("%s", line);
            break;
        case fxe::Level::DEBUG:
            fxe::Debug("%s", line);
            break;
        case fxe::Level::INFO:
            fxe::Info("%s", line);
            break;
        case fxe::Level::WARN:
            fxe::Warn("%s", line);
            break;
        case fxe::Level::ERROR:
            fxe::Error("%s", line);
            break;
    }
}

size_t ClampBytesPerLine(const size_t value) {
    return value == 0 ? 16 : value;
}

size_t ResolveMaxBytes(const size_t len, const size_t maxBytes) {
    if (maxBytes == 0) {
        return len;
    }
    return std::min(len, maxBytes);
}
} // namespace

namespace fxe {

void LogHexDump(const void* data,
                const size_t len,
                const char* label,
                const HexDumpOptions& options) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    const size_t bytesPerLine = ClampBytesPerLine(options.bytesPerLine);
    const size_t shown = ResolveMaxBytes(len, options.maxBytes);
    const bool truncated = len > shown;

    fxe::FixedString<160> summary;
    if (label && label[0] != '\0') {
        if (truncated) {
            (void)summary.Format("HEX DUMP %s: size=%zu shown=%zu (truncated)", label, len, shown);
        } else {
            (void)summary.Format("HEX DUMP %s: size=%zu shown=%zu", label, len, shown);
        }
    } else {
        if (truncated) {
            (void)summary.Format("HEX DUMP: size=%zu shown=%zu (truncated)", len, shown);
        } else {
            (void)summary.Format("HEX DUMP: size=%zu shown=%zu", len, shown);
        }
    }

    LogLineAtLevel(options.level, summary.CStr());

    if (!bytes || shown == 0) {
        return;
    }

    const uint8_t addressBytes = options.addressBytes == 0 ? 4 : options.addressBytes;
    const int addressWidth = static_cast<int>(addressBytes * 2);

    for (size_t offset = 0; offset < shown; offset += bytesPerLine) {
        const size_t lineCount = std::min(bytesPerLine, shown - offset);
        fxe::FixedString<512> line;

        if (options.showAddress) {
            const size_t address = options.startOffset + offset;
            (void)line.Format("%0*lX: ", addressWidth, static_cast<unsigned long>(address));
        }

        for (size_t i = 0; i < bytesPerLine; ++i) {
            if (i < lineCount) {
                char buf[4] = {};
                size_t pos = 0;
                bool truncated = false;
                fxe::format::AppendUnsigned(buf,
                                            sizeof(buf),
                                            pos,
                                            bytes[offset + i],
                                            16U,
                                            true,
                                            2,
                                            false,
                                            true,
                                            truncated);
                (void)line.Append(buf);
                (void)line.PushBack(' ');
            } else {
                (void)line.Append("   ");
            }
        }

        if (options.showAscii) {
            (void)line.Append("|");
            for (size_t i = 0; i < bytesPerLine; ++i) {
                if (i < lineCount) {
                    const unsigned char c = bytes[offset + i];
                    (void)line.PushBack(std::isprint(c) ? static_cast<char>(c) : '.');
                } else {
                    (void)line.PushBack(' ');
                }
            }
            (void)line.Append("|");
        }

        LogLineAtLevel(options.level, line.CStr());
    }
}

} // namespace fxe
