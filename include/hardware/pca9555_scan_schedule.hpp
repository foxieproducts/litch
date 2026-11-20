#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "hardware/pca9555_occupancy_decode.hpp"

namespace hardware {

class Pca9555ScanSchedule final {
  public:
    struct InterruptSnapshot {
        uint32_t epoch[PCA9555_EXPANDER_COUNT] = {};
    };

    static constexpr uint32_t RECONCILIATION_INTERVAL_MS = 50U;

    void NotifyAnyInterrupt() {
        for (uint8_t i = 0U; i < PCA9555_EXPANDER_COUNT; ++i) {
            ++m_interruptEpoch[i];
        }
    }

    void NotifyExpanderInterrupt(const uint8_t expanderIndex) {
        if (expanderIndex < PCA9555_EXPANDER_COUNT) {
            ++m_interruptEpoch[expanderIndex];
        }
    }

    void RequestFullRead() { m_forceFullRead = true; }

    bool ShouldScan() const {
        return m_forceFullRead || HasUnacknowledgedInterrupt() ||
               (m_sinceLastScan.Started() &&
                m_sinceLastScan.HasElapsed(RECONCILIATION_INTERVAL_MS));
    }

    InterruptSnapshot CaptureInterrupts() const {
        InterruptSnapshot snapshot;
        for (uint8_t i = 0U; i < PCA9555_EXPANDER_COUNT; ++i) {
            snapshot.epoch[i] = m_interruptEpoch[i];
        }
        return snapshot;
    }

    void OnScanSucceeded(const InterruptSnapshot& snapshot) {
        for (uint8_t i = 0U; i < PCA9555_EXPANDER_COUNT; ++i) {
            m_acknowledgedEpoch[i] = snapshot.epoch[i];
        }
        m_forceFullRead = false;
        m_sinceLastScan.Reset();
    }

    void OnScanFailed() {}

  private:
    bool HasUnacknowledgedInterrupt() const {
        for (uint8_t i = 0U; i < PCA9555_EXPANDER_COUNT; ++i) {
            if (m_interruptEpoch[i] != m_acknowledgedEpoch[i]) {
                return true;
            }
        }
        return false;
    }

    volatile uint32_t m_interruptEpoch[PCA9555_EXPANDER_COUNT] = {};
    uint32_t m_acknowledgedEpoch[PCA9555_EXPANDER_COUNT] = {};
    bool m_forceFullRead = true;
    fxe::ElapsedTime m_sinceLastScan;
};

} // namespace hardware
