#pragma once

#include "hardware/usb_cc_sense.hpp"

namespace hardware {

// One ADC conversion per service turn. Preserve the full filter's priming,
// eight samples and 2 ms settling intervals without sleeping on the input task.
class UsbCcSampler final {
  public:
    void Begin(const uint32_t nowMs) {
        m_active = true;
        m_step = 0U;
        m_lastMs = nowMs;
        m_result = {};
    }
    void Cancel() { m_active = false; }
    bool IsActive() const { return m_active; }

    template <typename Read>
    bool Update(const uint32_t nowMs, Read&& read, UsbCcReadings& out) {
        if (!m_active || (m_step != 0U && nowMs - m_lastMs < 2U)) return false;
        m_lastMs = nowMs;
        const uint8_t pin = m_step / 9U;
        const uint8_t sample = m_step % 9U;
        if (pin == 2U) {
            m_active = false;
            out = m_result;
            return true;
        }
        const uint16_t value = read(pin);
        if (sample == 0U) {
            if (pin == 0U) m_result.cc1Priming = value;
            else m_result.cc2Priming = value;
        } else {
            m_samples[sample - 1U] = value;
            if (sample == 8U) {
                uint16_t minimum = UINT16_MAX;
                uint16_t maximum = 0U;
                for (const uint16_t item : m_samples) {
                    if (item < minimum) minimum = item;
                    if (item > maximum) maximum = item;
                }
                const uint16_t filtered = CalculateUsbCcTrimmedMean(m_samples);
                if (pin == 0U) {
                    m_result.cc1 = filtered;
                    m_result.cc1Minimum = minimum;
                    m_result.cc1Maximum = maximum;
                } else {
                    m_result.cc2 = filtered;
                    m_result.cc2Minimum = minimum;
                    m_result.cc2Maximum = maximum;
                }
            }
        }
        ++m_step;
        return false;
    }

  private:
    UsbCcReadings m_result{};
    UsbCcSampleWindow m_samples{};
    uint32_t m_lastMs = 0U;
    uint8_t m_step = 0U;
    bool m_active = false;
};

} // namespace hardware
