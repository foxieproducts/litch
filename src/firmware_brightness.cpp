#ifdef ARDUINO

#include "firmware/firmware_internal.hpp"

#include "firmware/ambient_runtime_health.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/brightness_scale_policy.hpp"

namespace firmware_internal {

uint8_t GetBrightnessLevel0To8(const modules::SettingsStore& settings) {
    uint32_t raw = settings.ReadValue("BRIGHTNESS", static_cast<uint32_t>(5U));

    if (raw > 8U) {
        raw = 8U;
    }

    return static_cast<uint8_t>(raw);
}

uint16_t ComputeEffectiveBrightnessScale256(const modules::SettingsStore& settings,
                                            const modules::BootFadeIn& bootFade) {
    const uint8_t level0To8 = GetBrightnessLevel0To8(settings);
    float ambientFactor01 = 0.0f;
    auto& sys = firmware_internal::GetSystem();
    const bool useAmbientBrightness = firmware::ShouldUseAmbientBrightness(
        sys.runtimeHardware.resolved.hasLightSensors,
        sys.ambientLights.HasUsableSampleForBrightness());
    // Keep the smoothing clock anchored to the dark fallback before the first
    // usable sample so ambient acquisition cannot cause a whole-board step.
    const float smoothedAmbient =
        sys.ambientLux.ComputeSmoothedAmbientFactor01(fxe::Millis());
    if (useAmbientBrightness) ambientFactor01 = smoothedAmbient;
    return modules::brightness_scale_policy::ComputeEffectiveScale256(
        level0To8,
        useAmbientBrightness,
        ambientFactor01,
        bootFade.GetFadeScale256(),
        settings.ReadValue("AUTO_BRIGHTNESS", true));
}

} // namespace firmware_internal

#endif
