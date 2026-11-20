#include <atomic>
#include <cstdarg>
#include <cstdio>

#ifndef ARDUINO
#include <mutex>
#endif

#include <utility>

#ifdef ARDUINO
#include <Arduino.h>
#if defined(LITCH_ESP32S3)
#include <esp_attr.h>
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#endif

#include "foxie_embedded/format.hpp"
#include "foxie_embedded/logger.hpp"
#include "foxie_embedded/millis.hpp"

namespace {
fxe::Level g_minLevel = fxe::Level::INFO;

fxe::OutputLineCallback g_outputCb;

constexpr size_t LOG_BUFFER_LINES = 16;
constexpr size_t LOG_LINE_LEN = 256;

#if defined(ARDUINO) && defined(LITCH_ESP32S3)
// These CPU-only strings may survive deep sleep. The indices in normal BSS
// start each boot empty, so only lines written during this boot can be dumped.
RTC_SLOW_ATTR
#endif
char g_logLines[LOG_BUFFER_LINES][LOG_LINE_LEN];
size_t g_logWriteIndex = 0;
size_t g_logCount = 0;

#ifdef ARDUINO
StaticSemaphore_t g_logMutexStorage{};
std::atomic<SemaphoreHandle_t> g_logMutex{nullptr};
portMUX_TYPE g_logMutexInitMux = portMUX_INITIALIZER_UNLOCKED;

SemaphoreHandle_t EnsureLogMutex() {
    auto mutex = g_logMutex.load(std::memory_order_acquire);
    if (mutex != nullptr) return mutex;

    portENTER_CRITICAL(&g_logMutexInitMux);
    mutex = g_logMutex.load(std::memory_order_relaxed);
    if (mutex == nullptr) {
        mutex = xSemaphoreCreateRecursiveMutexStatic(&g_logMutexStorage);
        g_logMutex.store(mutex, std::memory_order_release);
    }
    portEXIT_CRITICAL(&g_logMutexInitMux);
    return mutex;
}

void LogLock() {
    const auto mutex = EnsureLogMutex();
    if (mutex != nullptr) xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
}

void LogUnlock() {
    const auto mutex = g_logMutex.load(std::memory_order_acquire);
    if (mutex != nullptr) xSemaphoreGiveRecursive(mutex);
}
#else
std::recursive_mutex g_logMutex;

void LogLock() {
    g_logMutex.lock();
}

void LogUnlock() {
    g_logMutex.unlock();
}
#endif

class LogLockGuard {
  public:
    LogLockGuard() {
        LogLock();
    }

    ~LogLockGuard() {
        LogUnlock();
    }

    LogLockGuard(const LogLockGuard&) = delete;
    LogLockGuard& operator=(const LogLockGuard&) = delete;
};

void AppendBufferedLine(const char* line) {
    if (!line) {
        return;
    }

    // Stop writing RTC memory at the terminator even for a reused slot.
    size_t length = 0;
    bool truncated = false;
    fxe::format::AppendStr(g_logLines[g_logWriteIndex], LOG_LINE_LEN, length, line, truncated);

    g_logWriteIndex = (g_logWriteIndex + 1) % LOG_BUFFER_LINES;
    if (g_logCount < LOG_BUFFER_LINES) {
        ++g_logCount;
    }
}

const char* LevelToString(const fxe::Level level) {
    switch (level) {
        case fxe::Level::TRACE:
            return "TRACE";
        case fxe::Level::DEBUG:
            return "DEBUG";
        case fxe::Level::INFO:
            return "INFO";
        case fxe::Level::WARN:
            return "WARN";
        case fxe::Level::ERROR:
            return "ERROR";
    }

    return "?";
}

void AppendTimestampSecMs(char* line, size_t& pos, bool& truncated) {
    const uint32_t nowMs = fxe::Millis();
    const uint32_t sec = nowMs / 1000U;
    const uint32_t ms = nowMs % 1000U;
    fxe::format::AppendUnsigned(line, LOG_LINE_LEN, pos, sec, 10U, false, 0, false, false, truncated);
    fxe::format::AppendChar(line, LOG_LINE_LEN, pos, '.', truncated);
    fxe::format::AppendUnsigned(line, LOG_LINE_LEN, pos, ms, 10U, false, 3, false, true, truncated);
}

void LogV(const fxe::Level level, const char* fmt, va_list args) {
    LogLockGuard lock;

    if (static_cast<uint8_t>(level) < static_cast<uint8_t>(g_minLevel)) {
        return;
    }

    char line[LOG_LINE_LEN] = {};
    size_t pos = 0;
    bool truncated = false;
    AppendTimestampSecMs(line, pos, truncated);
    fxe::format::AppendChar(line, LOG_LINE_LEN, pos, ' ', truncated);
    fxe::format::AppendChar(line, LOG_LINE_LEN, pos, '[', truncated);
    fxe::format::AppendStr(line, LOG_LINE_LEN, pos, LevelToString(level), truncated);
    fxe::format::AppendChar(line, LOG_LINE_LEN, pos, ']', truncated);
    fxe::format::AppendChar(line, LOG_LINE_LEN, pos, ' ', truncated);
    if (fmt) {
        va_list argsCopy;
        va_copy(argsCopy, args);
        (void)fxe::format::FormatAppendV(line, LOG_LINE_LEN, pos, fmt, argsCopy);
        va_end(argsCopy);
    }

    AppendBufferedLine(line);

    if (g_outputCb) {
        g_outputCb(line);
        return;
    }

#ifdef ARDUINO
    if (Serial) {
        Serial.printf("%s\n", line);
    }
#else
    std::printf("%s\n", line);
#endif
}

void RawLineImpl(const char* line) {
    if (!line) {
        return;
    }

    LogLockGuard lock;

    if (static_cast<uint8_t>(fxe::Level::TRACE) < static_cast<uint8_t>(g_minLevel)) {
        return;
    }

    AppendBufferedLine(line);

    if (g_outputCb) {
        g_outputCb(line);
        return;
    }

#ifdef ARDUINO
    if (Serial) {
        Serial.println(line);
    }
#else
    std::puts(line);
#endif
}
} // namespace

namespace fxe {

void SetMinLevel(const Level level) {
    LogLockGuard lock;
    g_minLevel = level;
}

Level GetMinLevel() {
    LogLockGuard lock;
    return g_minLevel;
}

void SetOutputLineCallback(OutputLineCallback cb) {
    LogLockGuard lock;
    g_outputCb = std::move(cb);
}

void DumpBufferedLines(OutputLineCallback cb) {
    if (!cb) {
        return;
    }

    LogLockGuard lock;

    const size_t start = (g_logCount < LOG_BUFFER_LINES) ? 0 : g_logWriteIndex;
    for (size_t i = 0; i < g_logCount; ++i) {
        const size_t idx = (start + i) % LOG_BUFFER_LINES;
        cb(g_logLines[idx]);
    }
}

void ClearBufferedLines() {
    LogLockGuard lock;

    g_logWriteIndex = 0;
    g_logCount = 0;
    for (size_t i = 0; i < LOG_BUFFER_LINES; ++i) {
        g_logLines[i][0] = '\0';
    }
}

void Trace(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV(Level::TRACE, fmt, args);
    va_end(args);
}

void Debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV(Level::DEBUG, fmt, args);
    va_end(args);
}

void Info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV(Level::INFO, fmt, args);
    va_end(args);
}

void Warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV(Level::WARN, fmt, args);
    va_end(args);
}

void Error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV(Level::ERROR, fmt, args);
    va_end(args);
}

void RawLine(const char* line) {
    RawLineImpl(line);
}

} // namespace fxe
