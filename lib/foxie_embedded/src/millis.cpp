#include "foxie_embedded/millis.hpp"

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <chrono>
#include <mutex>
#endif

namespace fxe {

namespace {

uint32_t DefaultMillis() {
#ifdef ARDUINO
    return static_cast<uint32_t>(::millis());
#else
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point t0 = Clock::now();
    const auto dt = Clock::now() - t0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dt).count();
    return static_cast<uint32_t>(ms);
#endif
}

#ifdef ARDUINO
MillisProvider g_provider = &DefaultMillis;
#else
std::atomic<MillisProvider> g_provider{&DefaultMillis};
#endif

#ifndef ARDUINO
// Native replay code temporarily replaces the process-wide provider. Keep a
// complete override lifetime exclusive so concurrent VirtualLitch handles cannot
// observe one another's provider or ManualMillis value. Recursive locking is
// required because the production adapter can establish a nested time scope.
std::recursive_mutex g_providerOverrideMutex;
#endif

} // namespace

uint32_t Millis() {
#ifdef ARDUINO
    const auto provider = g_provider;
#else
    const auto provider = g_provider.load(std::memory_order_relaxed);
#endif
    return provider ? provider() : DefaultMillis();
}

uint32_t ManualMillisProvider() {
    return ManualMillis::NowMs();
}

void SetMillisProviderForTests(const MillisProvider provider) {
#ifdef ARDUINO
    g_provider = provider ? provider : &DefaultMillis;
#else
    g_provider.store(provider ? provider : &DefaultMillis, std::memory_order_relaxed);
#endif
}

void ResetMillisProviderForTests() {
    SetMillisProviderForTests(&DefaultMillis);
}

ScopedMillisProviderOverride::ScopedMillisProviderOverride(const MillisProvider provider) {
#ifndef ARDUINO
    g_providerOverrideMutex.lock();
#endif
#ifdef ARDUINO
    m_prev = g_provider;
#else
    m_prev = g_provider.load(std::memory_order_relaxed);
#endif
    SetMillisProviderForTests(provider);
}

ScopedMillisProviderOverride::~ScopedMillisProviderOverride() {
    SetMillisProviderForTests(m_prev);
#ifndef ARDUINO
    g_providerOverrideMutex.unlock();
#endif
}

} // namespace fxe
