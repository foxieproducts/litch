#pragma once

#include <cstdint>

namespace modules {

// Pure sustained-gesture classifier. Hardware acquisition remains owned by the
// existing motion controller; this type sees only its bounded 25 Hz samples.
class LitchcraftShakeDetector final {
  public:
    static constexpr uint32_t SAMPLE_PERIOD_MS = 40U;
    static constexpr uint32_t MAX_SAMPLE_GAP_MS = 80U;
    static constexpr uint32_t SUSTAINED_MS = 1000U;
    static constexpr uint32_t MAX_ENERGETIC_GAP_MS = 160U;
    static constexpr uint32_t REARM_QUIET_MS = 600U;
    static constexpr int32_t AXIS_DELTA_THRESHOLD_MG = 250;
    static constexpr int32_t TOTAL_DELTA_THRESHOLD_MG = 450;
    static constexpr int64_t REVERSAL_DOT_THRESHOLD = -45000;
    static constexpr uint8_t MIN_ENERGETIC_SAMPLES = 10U;
    static constexpr uint8_t MIN_REVERSALS = 3U;

    void Reset();
    bool Observe(int32_t xMg, int32_t yMg, int32_t zMg, uint32_t nowMs,
                 bool eligible);
    bool IsCoolingDown() const { return m_coolingDown; }

  private:
    void ResetWindow();

    int32_t m_previousX = 0;
    int32_t m_previousY = 0;
    int32_t m_previousZ = 0;
    int32_t m_lastDeltaX = 0;
    int32_t m_lastDeltaY = 0;
    int32_t m_lastDeltaZ = 0;
    uint32_t m_lastSampleAtMs = 0U;
    uint32_t m_windowStartedAtMs = 0U;
    uint32_t m_lastEnergeticAtMs = 0U;
    uint32_t m_quietStartedAtMs = 0U;
    uint8_t m_energeticSamples = 0U;
    uint8_t m_reversals = 0U;
    bool m_hasPrevious = false;
    bool m_windowActive = false;
    bool m_hasEnergeticDelta = false;
    bool m_coolingDown = false;
};

} // namespace modules
