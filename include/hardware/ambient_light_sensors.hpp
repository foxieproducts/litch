#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"

#ifdef ARDUINO
#include <Wire.h>

#include <SparkFun_VEML6030_Ambient_Light_Sensor.h>
#endif

namespace hardware {

// Result of one call to UpdateAttempt(). A completed attempt is distinct from
// a captured lux sample: fitted hardware can complete a scheduled detection
// attempt with zero valid sensors.
struct AmbientLightUpdateResult {
    bool attemptCompleted = false;
    bool sampleCaptured = false;
    uint8_t validSensorCount = 0U;
};

class AmbientLightSensors final {
  public:
    void Begin();
#ifdef ARDUINO
    void Begin(TwoWire& wire);
#endif

    void PowerOn();

    void ShutDown();

    // Returns true if a new lux sample was captured.
    //
    // Kept for current callers. New health-aware callers should use
    // UpdateAttempt() so a due zero-sensor attempt is not mistaken for one
    // that was not due.
    bool Update();

    AmbientLightUpdateResult UpdateAttempt();

    bool IsInitialized() const { return m_initialized; }
    bool IsPoweredOn() const { return m_poweredOn; }
    bool Sensor0Ok() const { return m_sensor0Ok && m_sensor0ReadOk; }
    bool Sensor1Ok() const { return m_sensor1Ok && m_sensor1ReadOk; }
    uint32_t GetLastSampleAgeMs() const { return m_lastSampleElapsed.Started() ? m_lastSampleElapsed.ElapsedMs() : 0U; }

    bool HasSample() const { return m_hasSample; }
    bool HasUsableSampleForBrightness() const {
        return m_hasSample && m_lastValidCount != 0U;
    }
    float GetLastLux() const { return m_lastLux; }
    uint32_t GetLastLux0() const { return m_lastLux0; }
    uint32_t GetLastLux1() const { return m_lastLux1; }
    uint8_t GetLastValidCount() const { return m_lastValidCount; }

  private:
    bool m_initialized = false;
    bool m_sensor0Ok = false;
    bool m_sensor1Ok = false;
    bool m_sensor0ReadOk = true;
    bool m_sensor1ReadOk = true;
    bool m_poweredOn = false;

    fxe::ElapsedTime m_lastSampleElapsed;
    uint32_t m_sampleIntervalMs = 50;

    bool m_hasSample = false;
    float m_lastLux = 0.0f;
    uint32_t m_lastLux0 = 0;
    uint32_t m_lastLux1 = 0;
    uint8_t m_lastValidCount = 0;

    // Output smoothing:
    // - Median-of-recent to reject periodic flicker/noise.
    // - Hysteresis + IIR to keep output sticky while still reacting quickly to real changes.
    static constexpr uint8_t LUX_HISTORY_CAPACITY = 5;
    uint32_t m_recentAvgLux[LUX_HISTORY_CAPACITY] = {};
    uint8_t m_recentAvgLuxHead = 0;
    uint8_t m_recentAvgLuxCount = 0;
    bool m_filteredLuxValid = false;
    uint32_t m_filteredLux = 0;

#ifdef ARDUINO
    SparkFun_Ambient_Light m_sensor0 = SparkFun_Ambient_Light(altAddr);
    SparkFun_Ambient_Light m_sensor1 = SparkFun_Ambient_Light(defAddr);
#endif
};

} // namespace hardware
