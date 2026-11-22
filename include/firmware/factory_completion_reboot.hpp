#pragma once

#include <cstdint>

#include "foxie_embedded/millis.hpp"

namespace firmware {

// The factory receipt owner alone begins completion. Rendering acknowledges
// it after the actual transfer; the reboot owner independently checks time and
// live storage/OTA guards. No loop-entry timestamp crosses this boundary.
class FactoryCompletionReboot final {
  public:
    static constexpr uint32_t ACKNOWLEDGED_RESULT_MS = 1000U;
    void Begin() { m_pending = true; m_acknowledged = false; m_acknowledgedAtMs = 0U; }
    void Cancel() { m_pending = m_acknowledged = false; m_acknowledgedAtMs = 0U; }
    bool IsPending() const { return m_pending; }
    bool IsAcknowledged() const { return m_acknowledged; }
    uint32_t AcknowledgedAtMs() const { return m_acknowledgedAtMs; }
    void NoteFrameAcknowledged(const bool completed) {
        if (m_pending && completed && !m_acknowledged) {
            m_acknowledgedAtMs = fxe::Millis();
            m_acknowledged = true;
        }
    }
    bool IsReady(const bool operationBusy, const bool otaHealthy) const {
        return m_pending && m_acknowledged && !operationBusy && otaHealthy &&
            static_cast<uint32_t>(fxe::Millis() - m_acknowledgedAtMs) >= ACKNOWLEDGED_RESULT_MS;
    }

  private:
    bool m_pending = false;
    bool m_acknowledged = false;
    uint32_t m_acknowledgedAtMs = 0U;
};

} // namespace firmware
