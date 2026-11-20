#pragma once

#include <cstdint>

namespace modules {

// Presentation only: electrical reductions are immediate. Cap elapsed credit
// after a stalled frame so resuming cannot turn an upward ramp into a flash.
class UpwardOutputSlew final {
  public:
    void Reset(const uint16_t value, const uint32_t nowMs) {
        m_value1000 = uint32_t(value) * 1000U;
        m_lastMs = nowMs;
    }
    uint16_t Update(const uint16_t target, const uint32_t nowMs) {
        const uint32_t elapsed = nowMs - m_lastMs;
        m_lastMs = nowMs;
        const uint32_t wanted = uint32_t(target) * 1000U;
        if (wanted <= m_value1000) {
            m_value1000 = wanted;
        } else {
            // At most 40 mA per service turn, 1000 mA/s under normal cadence.
            const uint32_t step = (elapsed < 40U ? elapsed : 40U) * 1000U;
            m_value1000 += wanted - m_value1000 < step ? wanted - m_value1000 : step;
        }
        return static_cast<uint16_t>(m_value1000 / 1000U);
    }
  private:
    uint32_t m_value1000 = 0U;
    uint32_t m_lastMs = 0U;
};

} // namespace modules
