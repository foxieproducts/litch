#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/millis.hpp"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace fxe {

struct PerformanceTelemetrySnapshot {
    uint32_t loopCount = 0;

    uint32_t lastLoopDeltaMs = 0;
    uint32_t minLoopDeltaMs = 0;
    uint32_t maxLoopDeltaMs = 0;

    uint32_t droppedFrames = 0;
    uint32_t i2cErrors = 0;

    uint32_t minFreeHeapBytes = 0;
    uint32_t inputCompletionGapMs = UINT32_MAX;
    uint32_t bleCompletionGapMs = UINT32_MAX;
};

// Heap-free telemetry accumulator for basic performance counters.
class PerformanceTelemetry final {
  public:
    PerformanceTelemetry() = default;

    void Reset();

    // Call once per main loop.
    void OnLoop();

    // The first interval after reset began before the observation window.
    void ResetServiceGaps() { m_inputGapMs = m_bleGapMs = UINT32_MAX; }
    void OnInputCompletionGap(uint32_t elapsedMs) {
        m_inputGapMs = m_inputGapMs == UINT32_MAX ? 0U :
            (elapsedMs > m_inputGapMs ? elapsedMs : m_inputGapMs);
    }
    void OnBleCompletionGap(uint32_t elapsedMs) {
        m_bleGapMs = m_bleGapMs == UINT32_MAX ? 0U :
            (elapsedMs > m_bleGapMs ? elapsedMs : m_bleGapMs);
    }

    void OnFrameDropped() { ++m_droppedFrames; }
    void OnI2cError() { ++m_i2cErrors; }
    void OnI2cErrors(uint32_t count) { m_i2cErrors += count; }

    PerformanceTelemetrySnapshot GetSnapshot() const;

  private:
    void MaybeSampleHeap();

    uint32_t m_loopCount = 0;

    bool m_hasLast = false;
    ElapsedTime m_sinceLastLoop;

    uint32_t m_lastLoopDeltaMs = 0;
    uint32_t m_minLoopDeltaMs = 0;
    uint32_t m_maxLoopDeltaMs = 0;

    uint32_t m_droppedFrames = 0;
    uint32_t m_i2cErrors = 0;

    uint32_t m_minFreeHeapBytes = 0;
    uint32_t m_inputGapMs = UINT32_MAX;
    uint32_t m_bleGapMs = UINT32_MAX;
};

inline void PerformanceTelemetry::Reset() {
    ResetServiceGaps();
    m_loopCount = 0;
    m_hasLast = false;
    m_sinceLastLoop = ElapsedTime();

    m_lastLoopDeltaMs = 0;
    m_minLoopDeltaMs = 0;
    m_maxLoopDeltaMs = 0;

    m_droppedFrames = 0;
    m_i2cErrors = 0;

    m_minFreeHeapBytes = 0;
}

inline void PerformanceTelemetry::MaybeSampleHeap() {
#ifdef ARDUINO
    // ESP32 Arduino exposes a running minimum since boot.
    const uint32_t minFree = ESP.getMinFreeHeap();
    if (m_minFreeHeapBytes == 0 || minFree < m_minFreeHeapBytes) {
        m_minFreeHeapBytes = minFree;
    }
#else
    // No heap sampling in native builds.
#endif
}

inline void PerformanceTelemetry::OnLoop() {
    ++m_loopCount;

    if (m_hasLast) {
        const uint32_t delta = m_sinceLastLoop.ElapsedMs();
        m_lastLoopDeltaMs = delta;

        if (m_minLoopDeltaMs == 0 || delta < m_minLoopDeltaMs) {
            m_minLoopDeltaMs = delta;
        }
        if (delta > m_maxLoopDeltaMs) {
            m_maxLoopDeltaMs = delta;
        }
    }

    m_sinceLastLoop.Reset();
    m_hasLast = true;

    MaybeSampleHeap();
}

inline PerformanceTelemetrySnapshot PerformanceTelemetry::GetSnapshot() const {
    PerformanceTelemetrySnapshot out;
    out.loopCount = m_loopCount;
    out.lastLoopDeltaMs = m_lastLoopDeltaMs;
    out.minLoopDeltaMs = m_minLoopDeltaMs;
    out.maxLoopDeltaMs = m_maxLoopDeltaMs;
    out.droppedFrames = m_droppedFrames;
    out.i2cErrors = m_i2cErrors;
    out.minFreeHeapBytes = m_minFreeHeapBytes;
    out.inputCompletionGapMs = m_inputGapMs;
    out.bleCompletionGapMs = m_bleGapMs;
    return out;
}

} // namespace fxe
