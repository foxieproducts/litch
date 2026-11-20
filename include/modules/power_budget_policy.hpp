#pragma once

#include <cstdint>

namespace modules {

struct PowerBudgetPolicyConfig {
    // Hard caps; defaults mean "no cap" unless the caller configures them.
    uint16_t maxLedCurrentMa = UINT16_MAX;
    uint16_t maxTotalCurrentMa = UINT16_MAX;

    // Non-LED load that is always present while the controlled load is active.
    // This headroom is reserved before authorizing LED or optional USB current.
    uint16_t fixedSystemCurrentMa = 0;

    // Optional USB power decision support.
    bool allowUsbPower = false;
    uint16_t usbReserveCurrentMa = 0;

    // Shared hysteresis margin (mA) to avoid flapping at thresholds.
    uint16_t hysteresisMa = 0;
};

struct PowerBudgetDecision {
    // Guaranteed <= maxLedCurrentMa. For a finite total cap, the LED allowance
    // is zero if fixedSystemCurrentMa consumes the cap; otherwise their sum is
    // <= maxTotalCurrentMa.
    uint16_t allowedLedCurrentMa = 0;

    // 0..1. If estimatedLedCurrentMa==0, this is 1.
    float ledBrightnessScale01 = 1.0f;

    // Only meaningful if allowUsbPower=true; otherwise always false.
    bool usbPowerEnabled = false;
};

class PowerBudgetPolicy {
  public:
    explicit PowerBudgetPolicy(const PowerBudgetPolicyConfig& cfg) : m_cfg(cfg) {
        Reset();
    }

    void Reset() {
        m_estimatedLedCurrentMa = 0;
        m_ledLimiting = false;
        m_usbRequested = false;
        m_usbEnabled = false;
        m_changed = true;
    }

    void SetUsbPowerRequested(const bool requested) {
        m_usbRequested = requested;
        UpdateDecision();
    }

    void SetEstimatedLedCurrentMa(const uint16_t milliAmps) {
        m_estimatedLedCurrentMa = milliAmps;
        UpdateDecision();
    }

    uint16_t GetAllowedLedCurrentMa() const {
        return m_decision.allowedLedCurrentMa;
    }

    float GetLedBrightnessScale01() const {
        return m_decision.ledBrightnessScale01;
    }

    bool GetUsbPowerEnabled() const {
        return m_decision.usbPowerEnabled;
    }

    PowerBudgetDecision GetDecision() const {
        return m_decision;
    }

    bool ConsumeChanged() {
        const bool wasChanged = m_changed;
        m_changed = false;
        return wasChanged;
    }

  private:
    static float Clamp01(const float v) {
        if (v < 0.0f) {
            return 0.0f;
        }
        if (v > 1.0f) {
            return 1.0f;
        }
        return v;
    }

    uint16_t AvailableTotalCurrentForLedsMa() const {
        if (m_cfg.maxTotalCurrentMa == UINT16_MAX) {
            return UINT16_MAX;
        }
        if (m_cfg.fixedSystemCurrentMa >= m_cfg.maxTotalCurrentMa) {
            return 0U;
        }
        return static_cast<uint16_t>(m_cfg.maxTotalCurrentMa -
                                     m_cfg.fixedSystemCurrentMa);
    }

    void UpdateDecision() {
        const uint16_t totalLedHeadroomMa = AvailableTotalCurrentForLedsMa();
        const uint16_t absoluteLedMax =
            (m_cfg.maxLedCurrentMa < totalLedHeadroomMa)
                ? m_cfg.maxLedCurrentMa
                : totalLedHeadroomMa;

        const bool prevUsb = m_usbEnabled;
        const uint16_t prevAllowed = m_decision.allowedLedCurrentMa;

        // LED limiting hysteresis.
        if (!m_ledLimiting) {
            if (m_estimatedLedCurrentMa > absoluteLedMax) {
                m_ledLimiting = true;
            }
        } else {
            const uint16_t exitThreshold = (absoluteLedMax > m_cfg.hysteresisMa)
                                               ? static_cast<uint16_t>(absoluteLedMax - m_cfg.hysteresisMa)
                                               : 0;
            if (m_estimatedLedCurrentMa <= exitThreshold) {
                m_ledLimiting = false;
            }
        }

        uint16_t allowedLed = 0;
        if (m_ledLimiting) {
            allowedLed = absoluteLedMax;
        } else {
            allowedLed = (m_estimatedLedCurrentMa < absoluteLedMax) ? m_estimatedLedCurrentMa : absoluteLedMax;
        }

        // USB power enable/disable hysteresis.
        // Decision is based on what we intend to draw for LEDs (allowedLed) plus the reserved USB budget.
        if (!m_cfg.allowUsbPower || !m_usbRequested) {
            m_usbEnabled = false;
        } else if (!m_usbEnabled) {
            const uint16_t enableThreshold = (m_cfg.maxTotalCurrentMa > m_cfg.hysteresisMa)
                                                 ? static_cast<uint16_t>(m_cfg.maxTotalCurrentMa - m_cfg.hysteresisMa)
                                                 : 0;
            const uint32_t totalIfEnabled =
                static_cast<uint32_t>(m_cfg.fixedSystemCurrentMa) +
                static_cast<uint32_t>(allowedLed) + m_cfg.usbReserveCurrentMa;
            if (totalIfEnabled <= enableThreshold) {
                m_usbEnabled = true;
            }
        } else {
            const uint32_t totalIfEnabled =
                static_cast<uint32_t>(m_cfg.fixedSystemCurrentMa) +
                static_cast<uint32_t>(allowedLed) + m_cfg.usbReserveCurrentMa;
            if (totalIfEnabled > m_cfg.maxTotalCurrentMa) {
                m_usbEnabled = false;
            }
        }

        // Final decision payload.
        m_decision.allowedLedCurrentMa = allowedLed;
        m_decision.usbPowerEnabled = m_usbEnabled;

        if (m_estimatedLedCurrentMa == 0) {
            m_decision.ledBrightnessScale01 = 1.0f;
        } else {
            const float scale = static_cast<float>(allowedLed) / static_cast<float>(m_estimatedLedCurrentMa);
            m_decision.ledBrightnessScale01 = Clamp01(scale);
        }

        if ((prevUsb != m_usbEnabled) || (prevAllowed != m_decision.allowedLedCurrentMa)) {
            m_changed = true;
        }
    }

    const PowerBudgetPolicyConfig m_cfg;

    uint16_t m_estimatedLedCurrentMa = 0;
    bool m_ledLimiting = false;

    bool m_usbRequested = false;
    bool m_usbEnabled = false;

    PowerBudgetDecision m_decision;
    bool m_changed = false;
};

} // namespace modules
