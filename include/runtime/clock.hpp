#pragma once

#include <cstdint>

#include "foxie_embedded/millis.hpp"

namespace runtime {

class ManualClock final {
  public:
    ManualClock() = default;
    explicit ManualClock(const uint32_t initialNowMs) : m_nowMs(initialNowMs) {}

    uint32_t NowMs() const { return m_nowMs; }

    void SetNowMs(const uint32_t nowMs) {
        m_nowMs = nowMs;
        fxe::ManualMillis::SetNowMs(nowMs);
    }

    void AdvanceMs(const uint32_t deltaMs) {
        m_nowMs += deltaMs;
        fxe::ManualMillis::AdvanceMs(deltaMs);
    }

  private:
    uint32_t m_nowMs = 0;
};

} // namespace runtime
