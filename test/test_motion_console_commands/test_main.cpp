#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "console.hpp"
#include "firmware/motion_wake_controller.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/motion_console_commands.hpp"
#include "modules/settings_store.hpp"

namespace {

class FakeLis2dw12Bus final {
  public:
    hardware::Lis2dw12Transport Transport() {
        return hardware::Lis2dw12Transport{
            this,
            &Read,
            &Write,
            &Delay,
        };
    }

    void SetSample(const int16_t x, const int16_t y, const int16_t z) {
        sample = {x, y, z};
    }

    static bool Read(void* context,
                     const uint8_t,
                     const uint8_t reg,
                     uint8_t* data,
                     const size_t length) {
        auto& self = *static_cast<FakeLis2dw12Bus*>(context);
        if (reg == hardware::lis2dw12_register::WHO_AM_I && length == 1U) {
            data[0] = hardware::Lis2dw12::EXPECTED_DEVICE_ID;
            return true;
        }
        if (reg == hardware::lis2dw12_register::OUT_X_L && length == 6U) {
            for (size_t axis = 0; axis < self.sample.size(); ++axis) {
                const uint16_t raw = static_cast<uint16_t>(self.sample[axis]);
                data[axis * 2U] = static_cast<uint8_t>(raw & 0xFFU);
                data[axis * 2U + 1U] = static_cast<uint8_t>(raw >> 8U);
            }
            return true;
        }
        data[0] = self.registers[reg];
        return true;
    }

    static bool Write(void* context,
                      const uint8_t,
                      const uint8_t reg,
                      const uint8_t* data,
                      const size_t length) {
        auto& self = *static_cast<FakeLis2dw12Bus*>(context);
        if (length != 1U) {
            return false;
        }
        self.registers[reg] = data[0];
        ++self.writeCount;
        return true;
    }

    static void Delay(void*, uint32_t) {}

    std::array<uint8_t, 256> registers{};
    std::array<int16_t, 3> sample{};
    uint32_t writeCount = 0U;
};

struct FakeMotionPlatform {
    static void Configure(void*, uint8_t) {}
    static bool ReadHigh(void* context, uint8_t) {
        return static_cast<FakeMotionPlatform*>(context)->interruptHigh;
    }

    firmware::MotionWakePlatform Platform() {
        return firmware::MotionWakePlatform{this, &Configure, &ReadHigh};
    }

    bool interruptHigh = true;
};

void NoDelay(void*, uint32_t) {}

bool Contains(const std::string& text, const char* expected) {
    return text.find(expected) != std::string::npos;
}

} // namespace

int main() {
    fxe::test_support::TestResults results("MOTION_CONSOLE_COMMANDS");
    results.Start();

    modules::SettingsStoreConfig config;
    config.path = "build/test_motion_console_commands_settings.json";
    config.allowSynchronousDeferredSaves = false;
    modules::SettingsStore settings(config);

    FakeLis2dw12Bus bus;
    FakeMotionPlatform platform;
    firmware::MotionWakeController controller;
    results.Check(controller.Begin(hardware::BoardRevision::V3S3,
                                   bus.Transport(),
                                   platform.Platform(),
                                   settings),
                  "V3 motion controller begins");

    MemoryConsoleTransport transport;
    Console console(transport);
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);

    modules::MotionConsoleCommandContext context;
    context.delay = &NoDelay;
    modules::RegisterMotionConsoleCommands(
        console, settings, controller, context);

    fxe::test_support::RunCommand(console, transport, "motion status");
    results.Check(Contains(output.Str(), "supported=1 available=1"),
                  "status reports V3 availability");
    results.Check(Contains(output.Str(), "pin=21 id=0x44"),
                  "status reports verified interrupt pin and device ID");
    results.Check(Contains(output.Str(), "motion: requested enabled=1 mode=tap ") &&
                      Contains(output.Str(), "motion: applied enabled=1 mode=tap "),
                  "fresh settings report Tap as both requested and applied default");

    output.Clear();
    bus.SetSample(1600, -800, 400);
    fxe::test_support::RunCommand(console, transport, "motion sample");
    results.Check(Contains(output.Str(), "raw=1600,-800,400"),
                  "sample prints raw axes");
    results.Check(Contains(output.Str(), "mg="),
                  "sample prints converted milli-g axes");

    output.Clear();
    platform.interruptHigh = false;
    bus.registers[hardware::lis2dw12_register::WAKE_UP_SRC] = 0x0CU;
    bus.registers[hardware::lis2dw12_register::ALL_INT_SRC] = 0x20U;
    fxe::test_support::RunCommand(console, transport, "motion events");
    results.Check(Contains(output.Str(), "wake=1"),
                  "event command decodes wake source");
    results.Check(Contains(output.Str(), "events=1"),
                  "event command reports accumulated count");
    platform.interruptHigh = true;

    output.Clear();
    settings.Doc()["MOTION_WAKE_MODE"] = "motion";
    fxe::test_support::RunCommand(console, transport, "motion status");
    results.Check(Contains(output.Str(), "motion: requested enabled=1 mode=motion ") &&
                      Contains(output.Str(), "motion: applied enabled=1 mode=tap "),
                  "explicit Motion selection is requested before the sensor is reapplied");
    output.Clear();
    const uint32_t writesBeforeApply = bus.writeCount;
    fxe::test_support::RunCommand(console, transport, "motion apply");
    results.Check(bus.writeCount > writesBeforeApply,
                  "apply forces current runtime settings into sensor");
    results.Check(Contains(output.Str(), "OK"),
                  "apply reports success");
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "motion status");
    results.Check(Contains(output.Str(), "motion: requested enabled=1 mode=motion ") &&
                      Contains(output.Str(), "motion: applied enabled=1 mode=motion "),
                  "apply installs the explicitly requested Motion configuration");

    output.Clear();
    bus.SetSample(0, 0, 0);
    fxe::test_support::RunCommand(console, transport, "motion calibrate 4");
    results.Check(Contains(output.Str(), "samples=4"),
                  "calibration honors bounded sample count");
    results.Check(Contains(output.Str(), "recommended_threshold_mg=125"),
                  "calibration prints conservative recommendation");

    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "motion s");
    transport.PushChar('\t');
    console.Update();
    results.Check(Contains(output.Str(), "status\r\n") &&
                      Contains(output.Str(), "sample\r\n"),
                  "completion exposes motion status and sample");

    return results.Finish();
}
