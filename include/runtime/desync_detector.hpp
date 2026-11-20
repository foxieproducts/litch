#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"

namespace runtime {

struct OccupancyDiff {
    uint64_t missingMask = 0; // expected occupied, but physically empty
    uint64_t extraMask = 0;   // expected empty, but physically occupied

    bool IsMismatch() const { return (missingMask | extraMask) != 0; }
};

inline OccupancyDiff ComputeOccupancyDiff(const uint64_t expectedOccupiedMask,
                                          const uint64_t physicalOccupiedMask) {
    const uint64_t missing = expectedOccupiedMask & ~physicalOccupiedMask;
    const uint64_t extra = physicalOccupiedMask & ~expectedOccupiedMask;
    return OccupancyDiff{missing, extra};
}

enum class DesyncTransition : uint8_t {
    None = 0,
    EnteredRecovery = 1,
    ExitedRecovery = 2,
};

enum class SensorStability : uint8_t {
    Unstable = 0,
    Stable = 1,
};

struct DesyncStatus {
    bool recoveryActive = false;
    DesyncTransition transition = DesyncTransition::None;
    OccupancyDiff diff{};
};

struct DesyncDetectorConfig {
    // Require sensors to be stable for at least this long before entering recovery.
    uint32_t stableRequiredMs = 200;
};

// DesyncDetector compares a physical occupancy snapshot vs an expected occupancy
// snapshot and triggers a recovery entry once sensors are stable.
//
// This is a pure, heap-free helper. It does not talk to the engine; callers
// supply the expected and physical occupancy bitmasks.
class DesyncDetector {
  public:
    explicit DesyncDetector(const DesyncDetectorConfig& config = {})
        : m_config(config) {}

    void Reset() {
        m_recoveryActive = false;
        m_stableElapsed = fxe::ElapsedTime();
        m_wasStable = false;
    }

    DesyncStatus Evaluate(const SensorStability stability,
                          const uint64_t expectedOccupiedMask,
                          const uint64_t physicalOccupiedMask) {
        const OccupancyDiff diff = ComputeOccupancyDiff(expectedOccupiedMask, physicalOccupiedMask);

        const bool sensorsStable = (stability == SensorStability::Stable);
        if (sensorsStable && !m_wasStable) {
            m_stableElapsed.Reset();
        }

        m_wasStable = sensorsStable;

        if (!m_recoveryActive) {
            if (sensorsStable && m_stableElapsed.HasElapsed(m_config.stableRequiredMs) && diff.IsMismatch()) {
                m_recoveryActive = true;
                return DesyncStatus{true, DesyncTransition::EnteredRecovery, diff};
            }

            return DesyncStatus{false, DesyncTransition::None, diff};
        }

        // Recovery is active: exit once fully resolved while stable.
        if (sensorsStable && !diff.IsMismatch()) {
            m_recoveryActive = false;
            return DesyncStatus{false, DesyncTransition::ExitedRecovery, diff};
        }

        return DesyncStatus{true, DesyncTransition::None, diff};
    }

    bool IsRecoveryActive() const { return m_recoveryActive; }

  private:
    DesyncDetectorConfig m_config;

    bool m_recoveryActive = false;

    fxe::ElapsedTime m_stableElapsed;
    bool m_wasStable = false;
};

} // namespace runtime
