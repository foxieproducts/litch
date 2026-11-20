#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "firmware/motion_wake_controller.hpp"
#include "modules/control_mutation_service.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

struct FakeDevice {
    std::array<uint8_t, 256> registers{};
    bool interruptHigh = true;
    uint32_t reads = 0U;
    uint32_t writes = 0U;
    uint32_t delayMs = 0U;
    bool failNextWrite = false;
    uint8_t configuredPin = hardware::NO_GPIO;

    static bool Read(void* context,
                     uint8_t,
                     uint8_t reg,
                     uint8_t* data,
                     size_t length) {
        auto& self = *static_cast<FakeDevice*>(context);
        ++self.reads;
        for (size_t index = 0; index < length; ++index) {
            data[index] = self.registers[static_cast<uint8_t>(reg + index)];
        }
        return true;
    }

    static bool Write(void* context,
                      uint8_t,
                      uint8_t reg,
                      const uint8_t* data,
                      size_t length) {
        auto& self = *static_cast<FakeDevice*>(context);
        ++self.writes;
        if (self.failNextWrite) {
            self.failNextWrite = false;
            return false;
        }
        for (size_t index = 0; index < length; ++index) {
            self.registers[static_cast<uint8_t>(reg + index)] = data[index];
        }
        return true;
    }

    static void Delay(void* context, uint32_t milliseconds) {
        static_cast<FakeDevice*>(context)->delayMs += milliseconds;
    }

    static void ConfigureInput(void* context, uint8_t pin) {
        static_cast<FakeDevice*>(context)->configuredPin = pin;
    }

    static bool ReadInterruptHigh(void* context, uint8_t) {
        return static_cast<FakeDevice*>(context)->interruptHigh;
    }

    hardware::Lis2dw12Transport Transport() {
        return {this, &Read, &Write, &Delay};
    }

    firmware::MotionWakePlatform Platform() {
        return {this, &ConfigureInput, &ReadInterruptHigh};
    }
};

void CheckRevisionGateAndLiveSettings() {
    FakeDevice v2Device;
    fxe::SettingsStoreConfig v2Config;
    v2Config.path = "build/test_motion_wake_v2.json";
    fxe::SettingsStore v2Settings(v2Config);
    firmware::MotionWakeController v2;
    Check(v2.Begin(hardware::BoardRevision::V2S3,
                   v2Device.Transport(),
                   v2Device.Platform(),
                   v2Settings),
          "controller: unsupported V2 initializes as a harmless no-op");
    Check(!v2.IsSupported() && v2Device.reads == 0U && v2Device.writes == 0U,
          "controller: V2 never probes the V3-only I2C address");

    FakeDevice v3Device;
    v3Device.registers[hardware::lis2dw12_register::WHO_AM_I] =
        hardware::Lis2dw12::EXPECTED_DEVICE_ID;
    fxe::SettingsStoreConfig v3Config;
    v3Config.path = "build/test_motion_wake_v3.json";
    fxe::SettingsStore v3Settings(v3Config);
    v3Settings.Doc()["MOTION_WAKE_MODE"] = "motion";
    firmware::MotionWakeController v3;
    Check(v3.Begin(hardware::BoardRevision::V3S3,
                   v3Device.Transport(),
                   v3Device.Platform(),
                   v3Settings),
          "controller: V3 probes and configures the accelerometer");
    Check(v3.IsSupported() && v3.IsAvailable() &&
              v3.GetInterruptPin() == 21U && v3Device.configuredPin == 21U,
          "controller: V3 owns only its authoritative GPIO21 interrupt");

    v3Settings.Doc()["MOTION_WAKE_MODE"] = "tap";
    v3Settings.Doc()["TAP_WAKE_THRESHOLD_MG"] = 750U;
    v3Settings.MarkDirty();
    Check(v3.Update(v3Settings),
          "controller: a live settings revision reapplies sensor tuning");
    Check(v3.GetSensor().GetAppliedConfig().tapExperimental &&
              v3.GetSensor().GetAppliedConfig().odrTenthsHz == 4000U &&
              v3Device.registers[hardware::lis2dw12_register::CTRL1] == 0x74U &&
              v3Device.registers[hardware::lis2dw12_register::TAP_THS_X] == 12U,
          "controller: tap tuning applies without firmware rebuild");
    const auto tapWrites = v3Device.writes;
    Check(v3.SetInteractiveSampling(true) && v3.SetInteractiveSampling(false) &&
              v3.GetSensor().GetAppliedConfig().odrTenthsHz == 4000U &&
              v3Device.writes == tapWrites,
          "controller: handling leaves the 400 Hz tap-only profile untouched");
}

void CheckSleepArmingIsFailSafe() {
    FakeDevice device;
    device.registers[hardware::lis2dw12_register::WHO_AM_I] =
        hardware::Lis2dw12::EXPECTED_DEVICE_ID;
    fxe::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_motion_wake_sleep.json";
    fxe::SettingsStore settings(settingsConfig);
    firmware::MotionWakeController controller;
    Check(controller.Begin(hardware::BoardRevision::V3S3,
                           device.Transport(),
                           device.Platform(),
                           settings),
          "sleep: controller begins");

    device.interruptHigh = false;
    Check(!controller.PrepareForSleep(settings),
          "sleep: stuck-low interrupt aborts arming");
    Check(controller.GetDiagnostics().stuckLowCount == 1U &&
              !controller.IsWakeReady(),
          "sleep: stuck-low reason is visible and never claims wake readiness");

    device.interruptHigh = true;
    Check(controller.PrepareForSleep(settings),
          "sleep: cleared high interrupt arms motion wake");
    Check(controller.IsWakeReady(),
          "sleep: ready requires configured sensor and released INT");

    settings.Doc()["MOTION_WAKE_ENABLED"] = false;
    settings.MarkDirty();
    Check(controller.PrepareForSleep(settings),
          "sleep: user-disabled motion is a valid button-only sleep state");
    Check(!controller.IsWakeReady(),
          "sleep: disabled motion is omitted rather than treated as an error");
}

void CheckDisabledMissingSensorDoesNotBlockButtons() {
    FakeDevice device;
    device.registers[hardware::lis2dw12_register::WHO_AM_I] = 0U;
    fxe::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_motion_wake_missing.json";
    fxe::SettingsStore settings(settingsConfig);
    settings.Doc()["MOTION_WAKE_ENABLED"] = false;

    firmware::MotionWakeController controller;
    Check(!controller.Begin(hardware::BoardRevision::V3S3,
                            device.Transport(),
                            device.Platform(),
                            settings),
          "missing V3 sensor remains diagnostically unavailable");
    Check(controller.PrepareForSleep(settings),
          "disabled missing sensor never blocks button-only sleep");
    Check(!controller.IsWakeReady(),
          "disabled missing sensor is never added as a wake source");
}

void CheckCalibrationMath() {
    firmware::MotionCalibrationAccumulator calibration;
    calibration.Add({0, 0, 0, 10, -5, 1000});
    calibration.Add({0, 0, 0, 20, 5, 1010});
    calibration.Add({0, 0, 0, -10, 0, 990});
    const auto result = calibration.GetResult();
    Check(result.sampleCount == 3U && result.peakToPeakMg == 30U,
          "calibration: deterministic peak-to-peak noise is reported");
    Check(result.recommendedMotionThresholdMg == 125U,
          "calibration: recommendation never falls below product-safe default");

    calibration.Reset();
    calibration.Add({0, 0, 0, -200, 0, 1000});
    calibration.Add({0, 0, 0, 200, 0, 1000});
    Check(calibration.GetResult().recommendedMotionThresholdMg == 800U,
          "calibration: noisy installation recommends twice peak-to-peak margin");
}

void CheckTransientSamplingPreservesSettings() {
    FakeDevice device;
    device.registers[hardware::lis2dw12_register::WHO_AM_I] = 0x44U;
    fxe::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_motion_interactive.json";
    fxe::SettingsStore settings(settingsConfig);
    settings.Doc()["MOTION_WAKE_MODE"] = "motion";
    settings.Doc()["MOTION_WAKE_THRESHOLD_MG"] = 250U;
    settings.Doc()["MOTION_WAKE_DURATION_MS"] = 120U;
    firmware::MotionWakeController controller;
    Check(controller.Begin(hardware::BoardRevision::V3S3, device.Transport(),
                           device.Platform(), settings), "transient sampling begins");
    const auto revision = settings.GetRevision();
    const auto motionRegisters = device.registers;
    const auto motionWrites = device.writes;
    Check(controller.SetInteractiveSampling(true) &&
              controller.GetSensor().GetAppliedConfig().odrTenthsHz == 250U &&
              controller.GetSensor().GetAppliedConfig().motionDurationMs == 120U &&
              device.writes == motionWrites && device.registers == motionRegisters,
          "controller admits 25 Hz handling without rewriting rate or custom wake duration");
    Check(controller.SetInteractiveSampling(false) &&
              controller.SetInteractiveSampling(true) &&
              device.writes == motionWrites && device.registers == motionRegisters,
          "controller exit and reentry preserve the exact already matching wake profile");
    Check(settings.GetRevision() == revision &&
              settings.ReadValue("MOTION_WAKE_THRESHOLD_MG", 0U) == 250U &&
              settings.ReadValue("MOTION_WAKE_DURATION_MS", 0U) == 120U &&
              !settings.Doc().containsKey("WATER_MODE"),
          "handling never persists a mode or rewrites wake tuning");
    settings.Doc()["MOTION_WAKE_THRESHOLD_MG"] = 375U;
    settings.MarkDirty();
    Check(controller.Update(settings) &&
              controller.GetSensor().GetAppliedConfig().odrTenthsHz == 250U &&
              controller.GetSensor().GetAppliedConfig().motionDurationMs == 120U &&
              controller.GetSensor().GetAppliedConfig().motionThresholdMg == 375U,
          "live settings change preserves current interactive acquisition");
    const auto sleepWrites = device.writes;
    device.registers[hardware::lis2dw12_register::CTRL1] = 0U;
    device.registers[hardware::lis2dw12_register::WAKE_UP_THS] = 0U;
    Check(controller.PrepareForSleep(settings) && controller.IsWakeReady() &&
              controller.GetSensor().GetAppliedConfig().odrTenthsHz == 250U &&
              controller.GetSensor().GetAppliedConfig().motionDurationMs == 120U &&
              device.writes > sleepWrites &&
              device.registers[hardware::lis2dw12_register::CTRL1] == 0x30U &&
              device.registers[hardware::lis2dw12_register::WAKE_UP_THS] == 12U,
          "controller fully reapplies authoritative 25 Hz wake settings before sleep readiness");
    firmware::MotionWakeController unsupported;
    FakeDevice v2Device;
    Check(unsupported.Begin(hardware::BoardRevision::V2S3, v2Device.Transport(),
                            v2Device.Platform(), settings) &&
              !unsupported.SetInteractiveSampling(true) && v2Device.writes == 0U,
          "unsupported board never gains an accelerometer access path");
}

void CheckUnrelatedSettingsDoNotStallMotion() {
    FakeDevice device;
    device.registers[hardware::lis2dw12_register::WHO_AM_I] =
        hardware::Lis2dw12::EXPECTED_DEVICE_ID;
    fxe::SettingsStoreConfig config;
    config.path = "build/test_motion_unrelated_setting.json";
    fxe::SettingsStore settings(config);
    settings.Doc()["MOTION_WAKE_MODE"] = "motion";
    settings.Doc()["SETTLE_ANIM_STYLE"] = "ripple";
    modules::ControlMutationService mutations;
    mutations.Configure(settings, {});
    firmware::MotionWakeController controller;
    Check(controller.Begin(hardware::BoardRevision::V3S3, device.Transport(),
                           device.Platform(), settings),
          "setting cadence: motion begins before animation preference changes");
    Check(controller.SetInteractiveSampling(
              firmware::InteractiveSamplingOwner::Litchcraft, true),
          "setting cadence: current handling owner remains active");

    const auto writes = device.writes;
    const auto delay = device.delayMs;
    const auto configurations = controller.GetDiagnostics().configurationCount;
    StaticJsonDocument<64> value;
    for (const char* style : {"landing_flash", "ripple"}) {
        value.set(style);
        const auto revision = settings.GetRevision();
        const auto outcome = mutations.MutateSetting(
            "SETTLE_ANIM_STYLE", value.as<JsonVariantConst>());
        Check(outcome.status == modules::ControlMutationStatus::Applied &&
                  settings.GetRevision() != revision,
              "setting cadence: ordinary animation setting crosses real mutation owner");
        const auto reads = device.reads;
        device.interruptHigh = false;
        device.registers[hardware::lis2dw12_register::WAKE_UP_SRC] = 0x08U;
        Check(controller.Update(settings),
              "setting cadence: next production sensor update succeeds");
        Check(device.writes == writes && device.delayMs == delay &&
                  controller.GetDiagnostics().configurationCount == configurations,
              "setting cadence: unrelated revision performs no sensor writes or 80ms delay");
        Check(device.reads > reads && controller.GetDiagnostics().lastEvent.wakeUp,
              "setting cadence: unchanged configuration still polls real event registers");
        device.interruptHigh = true;
        const auto observedWrites = device.writes;
        const auto observedDelay = device.delayMs;
        Check(controller.Update(settings) && device.writes == observedWrites &&
                  device.delayMs == observedDelay,
              "setting cadence: observed unrelated revision cannot retry configuration");
    }

    const auto tunedWrites = device.writes;
    const auto tunedDelay = device.delayMs;
    value.set(375U);
    const auto tuned = mutations.MutateSetting(
        "MOTION_WAKE_THRESHOLD_MG", value.as<JsonVariantConst>());
    Check(tuned.status == modules::ControlMutationStatus::Applied &&
              controller.Update(settings) && device.writes > tunedWrites &&
              device.delayMs == tunedDelay + 80U &&
              controller.GetSensor().GetAppliedConfig().motionThresholdMg == 375U &&
              controller.GetSensor().GetAppliedConfig().odrTenthsHz == 250U,
          "setting cadence: real motion tuning still applies and preserves handling");
    const auto forcedDelay = device.delayMs;
    Check(controller.ApplyNow(settings) &&
              device.delayMs == forcedDelay + 80U,
          "setting cadence: explicit ApplyNow remains forceful for identical settings");
    const auto sleepDelay = device.delayMs;
    Check(controller.PrepareForSleep(settings) && controller.IsWakeReady() &&
              device.delayMs == sleepDelay + 80U,
          "setting cadence: sleep still rearms the hardware for identical settings");
}

void CheckEveryMotionFieldAndFailedConfiguration() {
    constexpr const char* changes[] = {
        "{\"MOTION_WAKE_ENABLED\":false}",
        "{\"MOTION_WAKE_MODE\":\"tap\"}",
        "{\"MOTION_WAKE_THRESHOLD_MG\":375}",
        "{\"MOTION_WAKE_DURATION_MS\":120}",
        "{\"TAP_WAKE_THRESHOLD_MG\":750}",
        "{\"TAP_WAKE_SHOCK_MS\":60}",
    };
    for (const auto* change : changes) {
        FakeDevice device;
        device.registers[hardware::lis2dw12_register::WHO_AM_I] =
            hardware::Lis2dw12::EXPECTED_DEVICE_ID;
        fxe::SettingsStoreConfig config;
        config.path = "build/test_motion_each_field.json";
        fxe::SettingsStore settings(config);
        settings.Doc()["MOTION_WAKE_MODE"] = "motion";
        modules::ControlMutationService mutations;
        mutations.Configure(settings, {});
        firmware::MotionWakeController controller;
        Check(controller.Begin(hardware::BoardRevision::V3S3, device.Transport(),
                               device.Platform(), settings),
              "setting fields: fresh sensor begins");
        StaticJsonDocument<128> value;
        Check(!deserializeJson(value, change), "setting fields: bounded input decodes");
        const auto setting = *value.as<JsonObjectConst>().begin();
        const auto writes = device.writes;
        const auto applied = mutations.MutateSetting(setting.key().c_str(), setting.value());
        Check(applied.status == modules::ControlMutationStatus::Applied &&
                  controller.Update(settings) && device.writes > writes &&
                  controller.GetDiagnostics().configurationCount == 2U,
              "setting fields: each real configuration field still reaches hardware");
    }

    FakeDevice device;
    device.registers[hardware::lis2dw12_register::WHO_AM_I] =
        hardware::Lis2dw12::EXPECTED_DEVICE_ID;
    fxe::SettingsStoreConfig config;
    config.path = "build/test_motion_setting_failure.json";
    fxe::SettingsStore settings(config);
    firmware::MotionWakeController controller;
    Check(controller.Begin(hardware::BoardRevision::V3S3, device.Transport(),
                           device.Platform(), settings), "setting failure: sensor begins");
    settings.Doc()["MOTION_WAKE_THRESHOLD_MG"] = 375U;
    settings.MarkDirty();
    device.failNextWrite = true;
    Check(!controller.Update(settings) && !controller.IsAvailable() &&
              controller.GetDiagnostics().configurationCount == 1U,
          "setting failure: failed programming never publishes success");
    settings.Doc().remove("MOTION_WAKE_THRESHOLD_MG");
    settings.MarkDirty();
    const auto writes = device.writes;
    Check(!controller.Update(settings) && device.writes == writes,
          "setting failure: reverted preference cannot bypass unavailable sensor");
    Check(controller.Begin(hardware::BoardRevision::V3S3, device.Transport(),
                           device.Platform(), settings) && controller.Update(settings),
          "setting failure: successful explicit reinitialization restores operation");
}

} // namespace

int main() {
    std::puts("[MOTION_WAKE_CONTROLLER] start");
    CheckRevisionGateAndLiveSettings();
    CheckSleepArmingIsFailSafe();
    CheckDisabledMissingSensorDoesNotBlockButtons();
    CheckCalibrationMath();
    CheckTransientSamplingPreservesSettings();
    CheckUnrelatedSettingsDoNotStallMotion();
    CheckEveryMotionFieldAndFailedConfiguration();

    if (g_failures == 0) {
        std::puts("[MOTION_WAKE_CONTROLLER] ok");
        return 0;
    }
    std::printf("[MOTION_WAKE_CONTROLLER] failures=%d\n", g_failures);
    return 1;
}
