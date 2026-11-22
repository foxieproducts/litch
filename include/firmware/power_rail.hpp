#pragma once

#include <atomic>
#include <cstdint>

#include "hardware/board_hardware_profile.hpp"
#include "hardware/hardware.hpp"

namespace firmware {

class PowerRail final {
  public:
    void Configure(const hardware::BoardHardwareProfile& profile) {
        m_enabled = profile.hasFiveVoltEnable;
        m_boardLedPin = profile.boardLedPin;
        m_statusLedPin = profile.statusLedPin;
        m_fiveVoltEnablePin = profile.fiveVoltEnable;
        m_usesPwmFiveVoltPrecharge = profile.revision == hardware::BoardRevision::V3_3S3;
    }

    void Begin();
    void PrepareForSleep();

    // Runtime control (S3 only).
    void Enable5v();
    void Disable5v();
    bool Is5vEnabled() const { return m_fiveVoltOn.load(std::memory_order_acquire); }
    // Set before any precharge GPIO/PWM work; renderer readiness remains separate.
    bool Is5vEnableRequested() const {
        return m_enableRequested.load(std::memory_order_acquire);
    }

    // Borrowed by the renderer for this owner's lifetime. Uncontrolled supplies
    // have no rail-off state and must keep their output available.
    const std::atomic<bool>* LedOutputEnabledState() const {
        return m_enabled ? &m_fiveVoltOn : nullptr;
    }

  private:
    std::atomic<bool> m_enableRequested{false};
    std::atomic<bool> m_fiveVoltOn{false};
    bool m_enabled = hardware::HW_HAS_5V_EN;
    bool m_usesPwmFiveVoltPrecharge = false;
    uint8_t m_boardLedPin = hardware::PIN_BOARD_LEDS;
    uint8_t m_statusLedPin = hardware::PIN_EDGE_LEDS;
    uint8_t m_fiveVoltEnablePin = hardware::PIN_5V_EN;
};

} // namespace firmware
