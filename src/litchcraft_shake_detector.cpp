#include "modules/litchcraft_shake_detector.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace modules {

void LitchcraftShakeDetector::ResetWindow() {
    m_windowStartedAtMs = 0U;
    m_lastEnergeticAtMs = 0U;
    m_energeticSamples = 0U;
    m_reversals = 0U;
    m_windowActive = false;
    m_hasEnergeticDelta = false;
}

void LitchcraftShakeDetector::Reset() {
    m_previousX = 0;
    m_previousY = 0;
    m_previousZ = 0;
    m_lastDeltaX = 0;
    m_lastDeltaY = 0;
    m_lastDeltaZ = 0;
    m_lastSampleAtMs = 0U;
    m_quietStartedAtMs = 0U;
    m_hasPrevious = false;
    m_coolingDown = false;
    ResetWindow();
}

bool LitchcraftShakeDetector::Observe(const int32_t xMg, const int32_t yMg,
                                      const int32_t zMg,
                                      const uint32_t nowMs,
                                      const bool eligible) {
    if (!eligible) {
        Reset();
        return false;
    }
    if (!m_hasPrevious || nowMs - m_lastSampleAtMs >= MAX_SAMPLE_GAP_MS) {
        ResetWindow();
        m_previousX = xMg;
        m_previousY = yMg;
        m_previousZ = zMg;
        m_lastSampleAtMs = nowMs;
        m_hasPrevious = true;
        return false;
    }
    if (nowMs - m_lastSampleAtMs < SAMPLE_PERIOD_MS) {
        return false;
    }

    const int32_t dx = xMg - m_previousX;
    const int32_t dy = yMg - m_previousY;
    const int32_t dz = zMg - m_previousZ;
    m_previousX = xMg;
    m_previousY = yMg;
    m_previousZ = zMg;
    m_lastSampleAtMs = nowMs;
    const int32_t maximum = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
    const int32_t total = std::abs(dx) + std::abs(dy) + std::abs(dz);
    const bool energetic = maximum >= AXIS_DELTA_THRESHOLD_MG &&
                           total >= TOTAL_DELTA_THRESHOLD_MG;

    if (m_coolingDown) {
        if (energetic) {
            m_quietStartedAtMs = 0U;
        } else if (m_quietStartedAtMs == 0U) {
            m_quietStartedAtMs = nowMs;
        } else if (nowMs - m_quietStartedAtMs >= REARM_QUIET_MS) {
            m_coolingDown = false;
            m_quietStartedAtMs = 0U;
            ResetWindow();
        }
        return false;
    }

    if (!energetic) {
        if (m_windowActive &&
            nowMs - m_lastEnergeticAtMs > MAX_ENERGETIC_GAP_MS) {
            ResetWindow();
        }
        return false;
    }

    if (!m_windowActive) {
        m_windowActive = true;
        m_windowStartedAtMs = nowMs;
    }
    if (m_hasEnergeticDelta) {
        const int64_t dot = static_cast<int64_t>(dx) * m_lastDeltaX +
                            static_cast<int64_t>(dy) * m_lastDeltaY +
                            static_cast<int64_t>(dz) * m_lastDeltaZ;
        if (dot <= REVERSAL_DOT_THRESHOLD &&
            m_reversals != std::numeric_limits<uint8_t>::max()) {
            ++m_reversals;
        }
    }
    m_lastDeltaX = dx;
    m_lastDeltaY = dy;
    m_lastDeltaZ = dz;
    m_hasEnergeticDelta = true;
    m_lastEnergeticAtMs = nowMs;
    if (m_energeticSamples != std::numeric_limits<uint8_t>::max()) {
        ++m_energeticSamples;
    }
    if (nowMs - m_windowStartedAtMs < SUSTAINED_MS ||
        m_energeticSamples < MIN_ENERGETIC_SAMPLES ||
        m_reversals < MIN_REVERSALS) {
        return false;
    }

    m_coolingDown = true;
    m_quietStartedAtMs = 0U;
    ResetWindow();
    return true;
}

} // namespace modules
