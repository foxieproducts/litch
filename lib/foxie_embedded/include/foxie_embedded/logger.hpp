#pragma once

#include <cstdint>

#include "foxie_embedded/fixed_function.hpp"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace fxe {

enum class Level : uint8_t {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
};

void SetMinLevel(Level level);

Level GetMinLevel();

using OutputLineCallback = fxe::FixedFunction<void(const char* line), 64>;

// \brief Redirect formatted log lines to a caller-owned sink.
// \note Intended for the console so prompt redraw stays stable.
void SetOutputLineCallback(OutputLineCallback cb);

// \brief Enumerate buffered log lines from oldest to newest.
void DumpBufferedLines(OutputLineCallback cb);

void ClearBufferedLines();

void Trace(const char* fmt, ...);
void Debug(const char* fmt, ...);
void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);
void Error(const char* fmt, ...);

// \brief Writes a line to the underlying transport with no prefixes.
// \note Intended for trace JSONL streaming.
void RawLine(const char* line);

} // namespace fxe
