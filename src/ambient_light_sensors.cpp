#include "hardware/ambient_light_sensors.hpp"

#ifdef ARDUINO

#include "logger.hpp"

namespace hardware {

namespace {

constexpr uint16_t VEML6030_INTEG_TIME_MS = 50U;
constexpr float VEML6030_GAIN = 0.25f;

uint32_t ClampU32(const uint32_t v) {
    return v;
}

uint32_t AbsDiffU32(const uint32_t a, const uint32_t b) {
    return (a >= b) ? (a - b) : (b - a);
}

uint32_t MedianU32(uint32_t* values, const uint8_t count) {
    if (values == nullptr || count == 0U) {
        return 0U;
    }

    // Insertion sort (count <= 5).
    for (uint8_t i = 1; i < count; ++i) {
        const uint32_t key = values[i];
        int8_t j = static_cast<int8_t>(i - 1U);
        while (j >= 0 && values[static_cast<uint8_t>(j)] > key) {
            values[static_cast<uint8_t>(j + 1)] = values[static_cast<uint8_t>(j)];
            --j;
        }
        values[static_cast<uint8_t>(j + 1)] = key;
    }

    const uint8_t mid = static_cast<uint8_t>(count / 2U);
    if ((count % 2U) == 1U) {
        return values[mid];
    }

    // Even: average the two middle samples.
    const uint32_t a = values[static_cast<uint8_t>(mid - 1U)];
    const uint32_t b = values[mid];
    return (a + b + 1U) / 2U;
}

} // namespace

void AmbientLightSensors::Begin() {
    Begin(Wire);
}

void AmbientLightSensors::Begin(TwoWire& wire) {
    if (m_initialized) {
        return;
    }

    m_sensor0Ok = m_sensor0.begin(wire);
    m_sensor1Ok = m_sensor1.begin(wire);

    if (m_sensor0Ok) {
        m_sensor0.setIntegTime(VEML6030_INTEG_TIME_MS);
        m_sensor0.setGain(VEML6030_GAIN);
    }

    if (m_sensor1Ok) {
        m_sensor1.setIntegTime(VEML6030_INTEG_TIME_MS);
        m_sensor1.setGain(VEML6030_GAIN);
    }

    if (!m_sensor0Ok && !m_sensor1Ok) {
        logger::Warn("light sensors: no VEML6030 sensors detected");
    } else {
        const uint16_t integMs = m_sensor0Ok ? m_sensor0.readIntegTime()
                                             : (m_sensor1Ok ? m_sensor1.readIntegTime() : 0U);
        const float gain = m_sensor0Ok ? m_sensor0.readGain() : (m_sensor1Ok ? m_sensor1.readGain() : 0.0f);
        logger::Trace("light sensors: VEML6030 sensors ok (s0=%u s1=%u integ=%ums gain=%.3f)",
                     static_cast<unsigned>(m_sensor0Ok ? 1U : 0U),
                     static_cast<unsigned>(m_sensor1Ok ? 1U : 0U),
                     static_cast<unsigned>(integMs),
                     static_cast<double>(gain));
    }

    m_lastSampleElapsed.Reset();
    m_initialized = true;

    // Reset filter state on init.
    m_recentAvgLuxHead = 0;
    m_recentAvgLuxCount = 0;
    m_filteredLuxValid = false;
    m_filteredLux = 0;
    m_lastLux = 0.0f;
    m_lastValidCount = 0U;
    m_hasSample = false;

    PowerOn();
}

void AmbientLightSensors::PowerOn() {
    if (!m_initialized) {
        return;
    }

    m_poweredOn = true;

    // Reset filter state on power transitions to avoid stale/unstable output.
    m_recentAvgLuxHead = 0;
    m_recentAvgLuxCount = 0;
    m_filteredLuxValid = false;
    m_filteredLux = 0;
    m_lastLux = 0.0f;
    m_hasSample = false;
    m_lastValidCount = 0U;

    logger::Trace("light sensors: power on (s0_ok=%u s1_ok=%u)",
                 static_cast<unsigned>(m_sensor0Ok ? 1U : 0U),
                 static_cast<unsigned>(m_sensor1Ok ? 1U : 0U));

    if (m_sensor0Ok) {
        m_sensor0.powerOn();
    }

    if (m_sensor1Ok) {
        m_sensor1.powerOn();
    }
}

void AmbientLightSensors::ShutDown() {
    if (!m_initialized) {
        return;
    }

    m_poweredOn = false;
    m_hasSample = false;
    m_lastValidCount = 0U;

    logger::Trace("light sensors: shut down (s0_ok=%u s1_ok=%u)",
                 static_cast<unsigned>(m_sensor0Ok ? 1U : 0U),
                 static_cast<unsigned>(m_sensor1Ok ? 1U : 0U));

    if (m_sensor0Ok) {
        m_sensor0.shutDown();
    }

    if (m_sensor1Ok) {
        m_sensor1.shutDown();
    }
}

bool AmbientLightSensors::Update() {
    return UpdateAttempt().sampleCaptured;
}

AmbientLightUpdateResult AmbientLightSensors::UpdateAttempt() {
    if (!m_initialized) {
        return {};
    }

    if (!m_poweredOn) {
        return {};
    }

    if (m_lastSampleElapsed.Started() && !m_lastSampleElapsed.HasElapsed(m_sampleIntervalMs)) {
        return {};
    }

    if (!m_lastSampleElapsed.Started()) {
        m_lastSampleElapsed.Reset();
    }

    uint8_t validCount = 0;
    uint32_t lux0 = 0;
    uint32_t lux1 = 0;

    // begin() detects address presence; each lux read must also communicate.
    // Do not interpret a failed transaction's fabricated register bytes as lux.
    if (m_sensor0Ok) {
        lux0 = ClampU32(m_sensor0.readLight());
        m_sensor0ReadOk = m_sensor0.lastReadOk();
        if (m_sensor0ReadOk) ++validCount; else lux0 = 0U;
    }
    if (m_sensor1Ok) {
        lux1 = ClampU32(m_sensor1.readLight());
        m_sensor1ReadOk = m_sensor1.lastReadOk();
        if (m_sensor1ReadOk) ++validCount; else lux1 = 0U;
    }

    if (validCount == 0) {
        m_lastValidCount = 0;
        m_hasSample = false;
        m_lastSampleElapsed.Reset();
        return AmbientLightUpdateResult{true, false, 0U};
    }

    const uint32_t sum = lux0 + lux1;
    const uint32_t avg = (validCount == 1) ? sum : ((sum + 1U) / 2U);

    // Push into bounded history.
    m_recentAvgLux[m_recentAvgLuxHead] = avg;
    m_recentAvgLuxHead = static_cast<uint8_t>((m_recentAvgLuxHead + 1U) % LUX_HISTORY_CAPACITY);
    if (m_recentAvgLuxCount < LUX_HISTORY_CAPACITY) {
        ++m_recentAvgLuxCount;
    }

    // Median-of-recent samples to reject sensor flicker/noise.
    uint32_t tmp[LUX_HISTORY_CAPACITY] = {};
    for (uint8_t i = 0; i < m_recentAvgLuxCount; ++i) {
        tmp[i] = m_recentAvgLux[i];
    }
    const uint32_t median = MedianU32(tmp, m_recentAvgLuxCount);

    // Hysteresis + IIR smoothing:
    // - Small changes within a few lux / a few percent are ignored.
    // - Larger deltas are applied quickly.
    if (!m_filteredLuxValid) {
        m_filteredLux = median;
        m_filteredLuxValid = true;
    } else {
        const uint32_t cur = m_filteredLux;
        const uint32_t delta = AbsDiffU32(median, cur);

        // Deadband: max(2 lux, 3% of current).
        uint32_t deadband = (cur * 3U + 99U) / 100U;
        if (deadband < 2U) {
            deadband = 2U;
        }

        if (delta > deadband) {
            // Fast-track substantial changes: max(20 lux, 12% of current).
            uint32_t fast = (cur * 12U + 99U) / 100U;
            if (fast < 20U) {
                fast = 20U;
            }

            const uint16_t alpha256 = (delta >= fast) ? 192U : 64U; // 0.75 or 0.25
            const uint16_t inv256 = static_cast<uint16_t>(256U - alpha256);

            const uint32_t next =
                (static_cast<uint32_t>(cur) * inv256 + static_cast<uint32_t>(median) * alpha256 + 128U) / 256U;
            m_filteredLux = next;
        }
    }

    m_lastLux0 = lux0;
    m_lastLux1 = lux1;
    m_lastValidCount = validCount;
    m_lastLux = static_cast<float>(m_filteredLux);
    m_hasSample = true;

    m_lastSampleElapsed.Reset();
    return AmbientLightUpdateResult{true, true, validCount};
}

} // namespace hardware

#else

namespace hardware {

void AmbientLightSensors::Begin() {
    m_initialized = true;
    m_poweredOn = false;
}

void AmbientLightSensors::PowerOn() {
    if (!m_initialized) {
        return;
    }
    m_poweredOn = true;
    m_hasSample = false;
    m_lastValidCount = 0U;
}

void AmbientLightSensors::ShutDown() {
    if (!m_initialized) {
        return;
    }
    m_poweredOn = false;
    m_hasSample = false;
    m_lastValidCount = 0U;
}

bool AmbientLightSensors::Update() {
    return UpdateAttempt().sampleCaptured;
}

AmbientLightUpdateResult AmbientLightSensors::UpdateAttempt() {
    return {};
}

} // namespace hardware

#endif
