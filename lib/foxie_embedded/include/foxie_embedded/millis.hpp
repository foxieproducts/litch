#pragma once

#include <cstdint>
#ifndef ARDUINO
#include <atomic>
#endif

namespace fxe {

using MillisProvider = uint32_t (*)();

// Standard monotonic time source.
//
// This intentionally avoids forcing `nowMs` to be threaded through every callsite.
// For tests and deterministic replays, override the provider.
uint32_t Millis();

// Test/replay hook: replace the global Millis provider.
//
// This is intentionally a simple function pointer (no heap, no std::function).
// Use `ScopedMillisProviderOverride` to restore automatically.
void SetMillisProviderForTests(MillisProvider provider);
void ResetMillisProviderForTests();

// Simple deterministic time source for tests/replays.
//
// Usage:
// - `fxe::ManualMillis::SetNowMs(123);`
// - `fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);`
class ManualMillis final {
  public:
    static uint32_t NowMs() {
#ifdef ARDUINO
        return s_nowMs;
#else
        return s_nowMs.load(std::memory_order_relaxed);
#endif
    }

    static void SetNowMs(const uint32_t nowMs) {
#ifdef ARDUINO
        s_nowMs = nowMs;
#else
        s_nowMs.store(nowMs, std::memory_order_relaxed);
#endif
    }

    static void AdvanceMs(const uint32_t deltaMs) {
#ifdef ARDUINO
        s_nowMs += deltaMs;
#else
        s_nowMs.fetch_add(deltaMs, std::memory_order_relaxed);
#endif
    }

  private:
#ifdef ARDUINO
    inline static uint32_t s_nowMs = 0;
#else
    inline static std::atomic<uint32_t> s_nowMs{0U};
#endif
};

uint32_t ManualMillisProvider();

class ScopedMillisProviderOverride final {
  public:
    explicit ScopedMillisProviderOverride(MillisProvider provider);
    ~ScopedMillisProviderOverride();

    ScopedMillisProviderOverride(const ScopedMillisProviderOverride&) = delete;
    ScopedMillisProviderOverride& operator=(const ScopedMillisProviderOverride&) = delete;

  private:
    MillisProvider m_prev = nullptr;
};

} // namespace fxe
