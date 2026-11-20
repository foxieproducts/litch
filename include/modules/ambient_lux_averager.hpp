#pragma once

#include <math.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace modules {

struct AmbientLuxAveragerConfig {
    // Sliding window length for averaging lux.
    // 0 keeps only the newest sample.
    uint32_t windowMs = 0U;

    // Smoothing time constant for applying the ambient factor.
    // Higher is smoother/slower.
    uint32_t smoothMs = 650U;

    // Smoothing time constant when the ambient factor is decreasing.
    // Dimming should feel responsive; brightening can remain more conservative.
    uint32_t smoothDownMs = 200U;

    // Hysteresis threshold for the ambient factor (0..1).
    // Changes smaller than this are treated as "inside the deadband" and are
    // applied using the drift time constant.
    float hysteresis01 = 0.03f;

    // When rawFactor01 remains inside the hysteresis band, we still want the
    // effective brightness to eventually drift toward the true value.
    // This is the time constant (ms) for that slow drift.
    uint32_t driftMs = 1200U;

    // Drift time constant when the ambient factor is decreasing.
    // This controls how quickly we converge to the floor when ambient light
    // drops but remains inside the deadband.
    uint32_t driftDownMs = 450U;

    // avgLux >= fullLux maps to ambient factor 1.0.
    uint16_t fullLux = 200U;

    // Lux deadzone offset applied before mapping to a 0..1 factor.
    // This helps avoid low-light baselines (e.g., ~1 lux noise floor) from
    // permanently lifting brightness above the user-selected minimum.
    float luxOffset = 1.0f;

    // When true, maps lux to the 0..1 range using a log-style curve:
    //   factor = log(1 + avgLux) / log(1 + fullLux)
    // This makes low lux values contribute more than a linear mapping, which
    // generally matches human perception better.
    bool useLogCurve = true;

    // Optional gamma shaping applied after the primary mapping.
    // Values > 1 reduce the contribution of very low lux while preserving
    // responsiveness (temporal smoothing is handled separately).
    float curveGamma = 1.8f;

    // Endpoint handling for smoothed output.
    // If the raw factor is within endpointRawEpsilon01 of 0 or 1 for sustained
    // samples, and the smoothed factor reaches endpointSnapEpsilon01 distance of
    // that endpoint, snap exactly to 0 or 1.
    // This keeps stickiness while guaranteeing eventual convergence to true min/max.
    float endpointRawEpsilon01 = 0.001f;
    float endpointSnapEpsilon01 = 0.01f;
};

// Fixed-size, deterministic lux sliding-window averager.
//
// Intended for mapping ambient light sensors into a smooth 0..1 brightness factor
// without heap allocations.
class AmbientLuxAverager final {
  public:
    struct SmoothingCheckpoint final {
        float factor01 = 0.0f;
        uint32_t lastUpdateMs = 0U;
        bool lastUpdateMsValid = false;
    };

    SmoothingCheckpoint CaptureSmoothing() const {
        return {m_smoothedFactor01, m_lastUpdateMs, m_lastUpdateMsValid};
    }

    void RestoreSmoothing(const SmoothingCheckpoint& checkpoint) {
        m_smoothedFactor01 = checkpoint.factor01;
        m_lastUpdateMs = checkpoint.lastUpdateMs;
        m_lastUpdateMsValid = checkpoint.lastUpdateMsValid;
    }
    explicit AmbientLuxAverager(const AmbientLuxAveragerConfig& config = {})
        : m_config(config), m_fullLuxLog1p(ComputeFullLuxLog1p(config)) {}

    void Reset() {
        m_head = 0U;
        m_count = 0U;
        m_sumLux = 0U;

        m_lastUpdateMsValid = false;
        m_lastUpdateMs = 0U;
        m_smoothedFactor01 = 0.0f;
    }

    void OnLuxSample(const uint32_t nowMs, const float lux, const bool valid = true) {
        if (!valid) {
            return;
        }

        uint16_t luxU16 = 0U;
        if (lux > 0.0f) {
            const float capped = (lux > 65535.0f) ? 65535.0f : lux;
            const uint32_t rounded = static_cast<uint32_t>(capped + 0.5f);
            luxU16 = (rounded > 65535U) ? static_cast<uint16_t>(65535U) : static_cast<uint16_t>(rounded);
        }

        PushSample(nowMs, luxU16);
        TrimOldSamples(nowMs);
    }

    bool TryGetAverageLux(const uint32_t nowMs, float& outAvgLux) {
        TrimOldSamples(nowMs);
        if (m_count == 0U) {
            outAvgLux = 0.0f;
            return false;
        }

        outAvgLux = static_cast<float>(m_sumLux) / static_cast<float>(m_count);
        return true;
    }

    float ComputeAmbientFactor01(const uint32_t nowMs) {
        float avgLux = 0.0f;
        if (!TryGetAverageLux(nowMs, avgLux)) {
            return 0.0f;
        }

        const uint16_t fullLux = m_config.fullLux;
        if (fullLux == 0U) {
            return 1.0f;
        }

        const float luxNonNegative = (avgLux < 0.0f) ? 0.0f : avgLux;
        const float offset = (m_config.luxOffset < 0.0f) ? 0.0f : m_config.luxOffset;
        const float adjustedLux = (luxNonNegative > offset) ? (luxNonNegative - offset) : 0.0f;

        float factor = 0.0f;
        if (m_config.useLogCurve) {
            const float denom = m_fullLuxLog1p;
            if (denom <= 0.0f) {
                factor = 1.0f;
            } else {
                const float numer = ::logf(1.0f + adjustedLux);
                factor = numer / denom;
            }
        } else {
            factor = adjustedLux / static_cast<float>(fullLux);
        }

        const float gamma = (m_config.curveGamma <= 0.0f) ? 1.0f : m_config.curveGamma;
        if (gamma != 1.0f && factor > 0.0f) {
            factor = ::powf(factor, gamma);
        }

        if (factor <= 0.0f) {
            return 0.0f;
        }
        if (factor >= 1.0f) {
            return 1.0f;
        }
        return factor;
    }

    const AmbientLuxAveragerConfig& Config() const { return m_config; }

    float ComputeSmoothedAmbientFactor01(const uint32_t nowMs) {
        return ComputeSmoothedAmbientFactor01(nowMs, ComputeAmbientFactor01(nowMs));
    }

    // Rendering receives the sensor owner's already-resolved raw factor. The
    // existing smoothing policy advances independently without reading sensors.
    float ComputeSmoothedAmbientFactor01(const uint32_t nowMs,
                                         const float rawFactor01) {
        auto state = CaptureSmoothing();
        const float value = SampleSmoothing(m_config, state, nowMs, rawFactor01);
        RestoreSmoothing(state);
        return value;
    }
    static float SampleSmoothing(const AmbientLuxAveragerConfig& config,
        SmoothingCheckpoint& state, const uint32_t nowMs, const float rawFactor01) {

        if (!state.lastUpdateMsValid) {
            state.lastUpdateMsValid = true;
            state.lastUpdateMs = nowMs;
            state.factor01 = rawFactor01;
            return state.factor01;
        }

        const uint32_t elapsedMs = nowMs - state.lastUpdateMs;
        state.lastUpdateMs = nowMs;
        // A blocked output transfer must not bank a large brightness jump.
        // Normal frame cadence is 20/35 ms; 40 ms bounds catch-up after a
        // flash or scheduler stall while retaining ordinary smoothing.
        const uint32_t dtMs = elapsedMs < 40U ? elapsedMs : 40U;

        const float hysteresis01 = (config.hysteresis01 < 0.0f) ? 0.0f : config.hysteresis01;
        const float delta = rawFactor01 - state.factor01;
        const float absDelta = (delta < 0.0f) ? -delta : delta;

        uint32_t tauMs = 0U;
        if (absDelta <= hysteresis01) {
            tauMs = (delta < 0.0f) ? config.driftDownMs : config.driftMs;
        } else {
            tauMs = (delta < 0.0f) ? config.smoothDownMs : config.smoothMs;
        }

        if (dtMs == 0U) {
            return state.factor01;
        }
        if (tauMs == 0U) {
            state.factor01 = rawFactor01;
            return state.factor01;
        }

        const float dt = static_cast<float>(dtMs);
        const float tau = static_cast<float>(tauMs);
        const float alpha = dt / (tau + dt);
        state.factor01 = state.factor01 + alpha * (rawFactor01 - state.factor01);

        const float endpointRawEpsilon01 = (config.endpointRawEpsilon01 < 0.0f) ? 0.0f : config.endpointRawEpsilon01;
        const float endpointSnapEpsilon01 = (config.endpointSnapEpsilon01 < 0.0f) ? 0.0f : config.endpointSnapEpsilon01;

        const bool rawNearMin = rawFactor01 <= endpointRawEpsilon01;
        const bool rawNearMax = rawFactor01 >= (1.0f - endpointRawEpsilon01);
        if (rawNearMin && state.factor01 <= endpointSnapEpsilon01) {
            state.factor01 = 0.0f;
        } else if (rawNearMax && state.factor01 >= (1.0f - endpointSnapEpsilon01)) {
            state.factor01 = 1.0f;
        }

        if (state.factor01 < 0.0f) {
            state.factor01 = 0.0f;
        }
        if (state.factor01 > 1.0f) {
            state.factor01 = 1.0f;
        }
        return state.factor01;
    }

  private:
    struct Sample {
        uint32_t ms = 0U;
        uint16_t lux = 0U;
    };

    static constexpr size_t MAX_SAMPLES = 32;

    void PushSample(const uint32_t nowMs, const uint16_t luxU16) {
        if (m_count < MAX_SAMPLES) {
            const size_t idx = (m_head + m_count) % MAX_SAMPLES;
            m_samples[idx].ms = nowMs;
            m_samples[idx].lux = luxU16;
            m_sumLux += static_cast<uint32_t>(luxU16);
            ++m_count;
            return;
        }

        // Overwrite the oldest sample.
        m_sumLux -= static_cast<uint32_t>(m_samples[m_head].lux);
        m_samples[m_head].ms = nowMs;
        m_samples[m_head].lux = luxU16;
        m_sumLux += static_cast<uint32_t>(luxU16);
        m_head = (m_head + 1U) % MAX_SAMPLES;
    }

    void TrimOldSamples(const uint32_t nowMs) {
        const uint32_t windowMs = m_config.windowMs;
        if (windowMs == 0U) {
            // Keep only the newest sample.
            while (m_count > 1U) {
                PopOldest();
            }
            return;
        }

        while (m_count > 0U) {
            const Sample& s = m_samples[m_head];
            const uint32_t ageMs = nowMs - s.ms;
            if (ageMs <= windowMs) {
                return;
            }
            PopOldest();
        }
    }

    void PopOldest() {
        if (m_count == 0U) {
            return;
        }

        m_sumLux -= static_cast<uint32_t>(m_samples[m_head].lux);
        m_head = (m_head + 1U) % MAX_SAMPLES;
        --m_count;
    }

    static float ComputeFullLuxLog1p(const AmbientLuxAveragerConfig& config) {
        const float fullLux = static_cast<float>(config.fullLux);
        if (fullLux <= 0.0f) {
            return 0.0f;
        }

        // Use logf(1+x) instead of log1pf for portability.
        return ::logf(1.0f + fullLux);
    }

    AmbientLuxAveragerConfig m_config;

    float m_fullLuxLog1p = 0.0f;

    std::array<Sample, MAX_SAMPLES> m_samples{};
    size_t m_head = 0U;
    size_t m_count = 0U;
    uint32_t m_sumLux = 0U;

    bool m_lastUpdateMsValid = false;
    uint32_t m_lastUpdateMs = 0U;
    float m_smoothedFactor01 = 0.0f;
};

} // namespace modules
