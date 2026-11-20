#include <cstdio>
#include <cstring>

#include "foxie_embedded/settings_store.hpp"
#include "hardware/lis2dw12_settings.hpp"
#include "modules/settings_registry.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

void CheckDescriptors() {
    struct Expected {
        const char* key;
        fxe::SettingType type;
    };
    static constexpr Expected EXPECTED[] = {
        {"MOTION_WAKE_ENABLED", fxe::SettingType::Bool},
        {"MOTION_WAKE_MODE", fxe::SettingType::Enum},
        {"MOTION_WAKE_THRESHOLD_MG", fxe::SettingType::UInt32},
        {"MOTION_WAKE_DURATION_MS", fxe::SettingType::UInt32},
        {"TAP_WAKE_THRESHOLD_MG", fxe::SettingType::UInt32},
        {"TAP_WAKE_SHOCK_MS", fxe::SettingType::UInt32},
        {"P1_X_DEEP_WAKE_ENABLED", fxe::SettingType::Bool},
        {"P1_X_WAKE_HOLD_MS", fxe::SettingType::UInt32},
    };

    for (const auto& expected : EXPECTED) {
        const auto* descriptor =
            modules::settings_registry::LookupByKey(expected.key);
        Check(descriptor != nullptr, expected.key);
        if (descriptor != nullptr) {
            Check(descriptor->type == expected.type,
                  "settings: descriptor type matches contract");
            Check(descriptor->category == fxe::SettingCategory::Hardware,
                  "settings: motion tuning is hardware category");
            Check(descriptor->IsVisibleToApp(),
                  "settings: motion tuning is app-visible and BLE-enumerated");
        }
    }

    const auto* mode =
        modules::settings_registry::LookupByKey("MOTION_WAKE_MODE");
    Check(mode != nullptr && mode->allowedCount == 3U &&
              std::strcmp(mode->defaultStr, "tap") == 0,
          "settings: Tap is default and all three mode tokens remain available");
}

void CheckReadDefaultsAndMutations() {
    fxe::SettingsStoreConfig storeConfig;
    storeConfig.path = "build/test_lis2dw12_settings.json";
    fxe::SettingsStore settings(storeConfig);

    const auto defaults = hardware::lis2dw12_settings::Read(settings);
    Check(defaults.enabled &&
              defaults.mode == hardware::Lis2dw12WakeMode::Tap,
          "settings: default is enabled Tap wake");
    Check(defaults.motionThresholdMg == 125U &&
              defaults.motionDurationMs == 80U,
          "settings: default motion tuning is responsive and low power");
    Check(defaults.tapThresholdMg == 438U &&
              defaults.tapShockMs == 40U,
          "settings: Tap defaults to the V3.3 trial tuning");

    settings.Doc()["MOTION_WAKE_ENABLED"] = false;
    settings.Doc()["MOTION_WAKE_MODE"] = "motion_and_tap";
    settings.Doc()["MOTION_WAKE_THRESHOLD_MG"] = 250U;
    settings.Doc()["MOTION_WAKE_DURATION_MS"] = 160U;
    settings.Doc()["TAP_WAKE_THRESHOLD_MG"] = 750U;
    settings.Doc()["TAP_WAKE_SHOCK_MS"] = 20U;

    const auto changed = hardware::lis2dw12_settings::Read(settings);
    Check(!changed.enabled &&
              changed.mode == hardware::Lis2dw12WakeMode::MotionAndTap,
          "settings: enable and combined experimental mode apply live");
    Check(changed.motionThresholdMg == 250U &&
              changed.motionDurationMs == 160U &&
              changed.tapThresholdMg == 750U &&
              changed.tapShockMs == 20U,
          "settings: all threshold and duration values read without recompiling");

    settings.Doc()["MOTION_WAKE_MODE"] = "not-a-mode";
    Check(hardware::lis2dw12_settings::Read(settings).mode ==
              hardware::Lis2dw12WakeMode::Motion,
          "settings: corrupt mode fails safe to low-power motion");
}

} // namespace

int main() {
    std::puts("[LIS2DW12_SETTINGS] start");
    CheckDescriptors();
    CheckReadDefaultsAndMutations();

    if (g_failures == 0) {
        std::puts("[LIS2DW12_SETTINGS] ok");
        return 0;
    }
    std::printf("[LIS2DW12_SETTINGS] failures=%d\n", g_failures);
    return 1;
}
