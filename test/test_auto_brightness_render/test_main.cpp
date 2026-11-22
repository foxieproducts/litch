#include <array>
#include <cstdio>
#include <filesystem>

#include "firmware/firmware_internal.hpp"
#include "firmware/ambient_runtime_health.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/ambient_lux_averager.hpp"
#include "modules/boot_fade_in.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/settings_store.hpp"

namespace brightness_fixture {
struct SensorValidity {
    bool usable = true;
    bool HasUsableSampleForBrightness() const { return usable; }
};
struct Profile { bool hasLightSensors = true; };
struct Hardware { Profile resolved; };
struct System {
    Hardware runtimeHardware;
    SensorValidity ambientLights;
    modules::AmbientLuxAverager ambientLux;
} system;
System& GetSystem() { return system; }
} // namespace brightness_fixture

// Compile the exact renderer-called production function. Only the hardware
// aggregate/profile and fresh-sample leaf are controlled; settings, ambient
// smoothing, boot fade and final RGB/current pipeline are real owners.
#define ARDUINO 1
#define firmware_internal brightness_fixture
#include "../../src/firmware_brightness.cpp"
#undef firmware_internal
#undef ARDUINO

namespace {
int failures = 0;
void Check(bool ok, const char* why) {
    if (!ok) { ++failures; std::printf("[FAIL] %s\n", why); }
}
class CaptureRenderer final : public modules::ILedRenderer {
  public:
    uint16_t GetLedCount() const override { return pixels.size(); }
    void Clear() override { pixels.fill({}); }
    void SetLedRgb(uint16_t i, uint8_t r, uint8_t g, uint8_t b) override {
        pixels[i] = {r, g, b};
    }
    void Show() override {}
    std::array<modules::Rgb, 1> pixels{};
};
uint8_t Render(fxe::SettingsStore& settings) {
    modules::BootFadeIn fade;
    const auto scale = brightness_fixture::ComputeEffectiveBrightnessScale256(settings, fade);
    modules::PowerBudgetPolicyConfig power;
    power.maxLedCurrentMa = 450U;
    power.maxTotalCurrentMa = 450U;
    modules::PowerBudgetPolicy policy(power);
    CaptureRenderer renderer;
    modules::BudgetAwareLedPipelineConfig config;
    config.outputBrightnessScale256 = 128U; // Retail V3.3 hardware gain.
    modules::BudgetAwareLedPipeline pipeline(renderer, policy, config);
    pipeline.SetBrightnessScale01(static_cast<float>(scale) / 256.0f);
    const modules::Rgb cue{255U, 0U, 0U};
    pipeline.RenderFrame(&cue, 1U);
    Check(renderer.pixels[0].r <= 115U, "normal V3.3 output never exceeds 45 percent");
    Check(pipeline.GetLastStats().appliedLedCurrentMa <= 450U, "current protection remains active");
    return renderer.pixels[0].r;
}
void Sample(float lux) {
    brightness_fixture::system.ambientLux.Reset();
    brightness_fixture::system.ambientLux.OnLuxSample(fxe::Millis(), lux);
}
} // namespace

int main() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    constexpr const char* path = "build/test_auto_brightness_render.json";
    std::filesystem::remove(path);
    modules::SettingsStoreConfig config;
    config.path = path;
    modules::SettingsStore settings(config);
    settings.Doc()["AUTO_BRIGHTNESS"] = true;
    Sample(10000.0f);
    settings.Doc()["BRIGHTNESS"] = 8U;
    const auto brightMaximum = Render(settings);
    settings.Doc()["BRIGHTNESS"] = 4U;
    const auto brightLower = Render(settings);
    Check(brightLower < brightMaximum, "bright room: lowering slider visibly lowers production RGB output");
    settings.Doc()["AUTO_BRIGHTNESS"] = false;
    Check(Render(settings) == brightLower, "bright auto reaches chosen manual ceiling, never a common maximum");
    Sample(0.0f);
    const auto manual = Render(settings);
    settings.Doc()["AUTO_BRIGHTNESS"] = true;
    Check(Render(settings) < manual, "dark room: Auto scales down within the user ceiling");
    Check(settings.ReadValue("BRIGHTNESS", 0U) == 4U, "Auto changes preserve the saved preference");
    brightness_fixture::system.ambientLights.usable = false;
    Check(Render(settings) < manual, "missing sample retains conservative dark fallback within ceiling");
    brightness_fixture::system.ambientLights.usable = true;
    for (const auto level : {0U, 1U}) {
        settings.Doc()["BRIGHTNESS"] = level;
        Sample(10000.0f);
        const auto automatic = Render(settings);
        settings.Doc()["AUTO_BRIGHTNESS"] = false;
        Check(automatic <= Render(settings), "near-dark and lowest Auto level never exceed their manual ceiling");
        settings.Doc()["AUTO_BRIGHTNESS"] = true;
    }
    settings.Doc()["BRIGHTNESS"] = 3U;
    settings.SaveNow();
    modules::SettingsStore rebooted(config);
    Check(rebooted.Load(), "saved preference reloads at reboot");
    Check(rebooted.ReadValue("BRIGHTNESS", 0U) == 3U &&
              rebooted.ReadValue("AUTO_BRIGHTNESS", false), "mode and selected maximum persist together");
    Sample(10000.0f);
    const auto rebootAuto = Render(rebooted);
    rebooted.Doc()["AUTO_BRIGHTNESS"] = false;
    Check(Render(rebooted) == rebootAuto, "rebooted bright-room Auto still respects the same preference");
    std::filesystem::remove(path);
    std::printf("[AUTO_BRIGHTNESS_RENDER] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
