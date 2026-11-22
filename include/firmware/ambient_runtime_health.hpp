#pragma once

#include <cstdint>

#include "hardware/ambient_light_sensors.hpp"
#include "runtime/runtime_health.hpp"

namespace firmware {

// The ambient factor is meaningful only after a fitted profile has a fresh
// nonzero-sensor sample. This selects the existing brightness policy's
// deterministic non-ambient path; it does not introduce another policy.
constexpr bool ShouldUseAmbientBrightness(const bool hasFittedSensors,
                                          const bool hasUsableSample) {
    return hasFittedSensors && hasUsableSample;
}

// Publishes only evidence from a completed attempt on a fitted profile.
// Startup and not-due results deliberately leave the reducer untouched.
inline bool ObserveAmbientRuntimeHealth(
    runtime::RuntimeHealthReducer& reducer,
    const bool hasFittedSensors,
    const uint32_t nowMs,
    const hardware::AmbientLightUpdateResult& attempt) {
    if (!hasFittedSensors || !attempt.attemptCompleted) {
        return false;
    }

    runtime::RuntimeHealthFact fact;
    fact.tMs = nowMs;
    fact.kind = runtime::RuntimeHealthFactKind::AmbientSensorCount;
    fact.ambientSensorCount = attempt.validSensorCount;
    return reducer.Apply(fact);
}

} // namespace firmware
