#pragma once

#include <cstdint>

namespace modules {

// Tiny reusable transition envelope for compositing one presentation over
// another. Retargeting samples the current value first, so interrupted and
// reversed transitions remain continuous instead of restarting or popping.
class SmoothFade255 final {
  public:
    void Reset(const uint8_t value, const uint32_t nowMs) {
        m_startMs = nowMs;
        m_durationMs = 0U;
        m_start = value;
        m_value = value;
        m_target = value;
    }

    void SetTarget(const uint8_t target,
                   const uint32_t durationMs,
                   const uint32_t nowMs) {
        Update(nowMs);
        if (target == m_target) { return; }
        m_startMs = nowMs;
        m_durationMs = durationMs;
        m_start = m_value;
        m_target = target;
        if (durationMs == 0U) { m_value = target; }
    }

    uint8_t Update(const uint32_t nowMs) {
        if (m_value == m_target || m_durationMs == 0U) {
            m_value = m_target;
            return m_value;
        }
        const uint32_t elapsed = nowMs - m_startMs;
        if (elapsed >= m_durationMs) {
            m_value = m_target;
            return m_value;
        }
        const uint32_t progress256 = elapsed * 256U / m_durationMs;
        const uint32_t eased256 =
            (progress256 * progress256 * (768U - 2U * progress256) + 32768U) >> 16U;
        const int32_t delta = int32_t(m_target) - int32_t(m_start);
        m_value = static_cast<uint8_t>(int32_t(m_start) +
            (delta * int32_t(eased256) + (delta >= 0 ? 128 : -128)) / 256);
        return m_value;
    }

    uint8_t Value() const { return m_value; }
    uint8_t Target() const { return m_target; }
    bool IsSettled() const { return m_value == m_target; }

  private:
    uint32_t m_startMs = 0U;
    uint32_t m_durationMs = 0U;
    uint8_t m_start = 0U;
    uint8_t m_value = 0U;
    uint8_t m_target = 0U;
};

} // namespace modules
