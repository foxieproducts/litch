#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include "console.hpp"
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"
#include "firmware/led_animation_presentation.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/hardware.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/user_input.hpp"
#include "owner_pairing_console_test_transport.hpp"
#include "firmware/sleep_transition.hpp"

// Preload native platform headers, then compile the actual firmware session
// owners below. Only SDK timing, wiring and the physical output sink are fake.
#define ARDUINO 1
#include "firmware/amptest_session.hpp"
#undef ARDUINO

namespace amptest_fixture {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::printf("[FAIL] %s\n", message); }
}
struct Pixel { uint8_t r = 0U, g = 0U, b = 0U, w = 0U; };
struct Leds {
    std::array<Pixel, hardware::MAX_PHYSICAL_LED_COUNT> pixels{};
    unsigned shows = 0U;
    bool transferCompletes = true;
    bool lastShowCompleted = false;
    uint16_t GetLedCount() const {
        constexpr auto topology = hardware::BoardLedTopologyFor(hardware::BoardRevision::V3S3);
        return topology.physicalBoardLedCount + topology.physicalStatusLedCount;
    }
    void Clear() { pixels.fill({}); }
    void SetLedRgb(uint16_t i, uint8_t r, uint8_t g, uint8_t b) { SetLedRgbwRaw(i, r, g, b, 0U); }
    void SetLedRgbwRaw(uint16_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
        Check(i < GetLedCount(), "raw render stays within the physical renderer");
        if (i < GetLedCount()) pixels[i] = {r, g, b, w};
    }
    void Show() { ++shows; lastShowCompleted = transferCompletes; }
    bool LastShowCompleted() const { return lastShowCompleted; }
    void RestoreOutputPinRouting() {}
};
// Complete the current output-owner commands synchronously at the existing
// controlled physical sink. FreeRTOS scheduling and RMT delivery are bypassed;
// production session/rail admission still depends on exact Show completion.
struct LedAnimation {
    Leds& output;
    bool quiescent = false;
    bool IsStarted() const { return true; }
    bool IsQuiescent() const { return quiescent; }
    bool Submit(const firmware::RawDieLedCommand& command) {
        if (quiescent) return false;
        output.Clear();
        for (uint16_t pixel = 0U; pixel < output.GetLedCount(); ++pixel) {
            output.SetLedRgbwRaw(pixel,
                command.channel == 0U ? command.value : 0U,
                command.channel == 1U ? command.value : 0U,
                command.channel == 2U ? command.value : 0U,
                command.channel == 3U ? command.value : 0U);
        }
        output.Show();
        return output.LastShowCompleted();
    }
    bool Quiesce() {
        if (quiescent) return true;
        output.Clear();
        output.Show();
        quiescent = output.LastShowCompleted();
        return quiescent;
    }
    void Resume() { quiescent = false; }
};
struct FirmwareSystem {
    bool factoryBootOnly = false, rebootPersistencePending = false;
    bool recoveryFrameInitialized = false;
    struct { bool ota=false; bool IsOtaInProgress() const { return ota; } } ble;
    struct { bool IsActive() const { return false; } } hardwareTest;
    struct { firmware::SleepState GetState() const { return firmware::SleepState::Awake; } } sleep;
    struct { bool on = true; void PowerOn() { on = true; } void ShutDown() { on = false; } } ambientLights;
    struct { bool on = true; bool Is5vEnableRequested() const { return on; } bool Is5vEnabled() const { return on; }
        void Enable5v() { on = true; } void Disable5v() { on = false; } } powerRail;
    struct { unsigned cancels = 0U;
        struct { bool running=false; bool IsRunning() const { return running; } } clock;
        const auto& GetGameClock() const { return clock; }
        bool IsEngineLaneActive() const { return false; }
        struct { bool IsActive() const { return false; } } litchcraft;
        auto& Litchcraft() { return litchcraft; }
        void RequestCancelAllThinking() { ++cancels; } } gamePersistence;
    struct { void Stop() {} } menuOverlay;
    bool quickSettingsOverlayActive = false, gameResetHoldActive = false, gameResetHoldTriggered = false;
    modules::UiOwner quickSettingsOverlayOwner = modules::UiOwner::None;
    uint16_t ledPowerLimitMa = 216U;
    struct { struct { hardware::BoardRevision revision = hardware::BoardRevision::V3S3; } resolved; } runtimeHardware;
    modules::BudgetAwareLedPipelineStats ledPowerStats{};
    amptest_fixture::Leds leds;
    amptest_fixture::LedAnimation ledAnimation{leds};
    bool ledLastCommandCompleted = false;
    amptest_fixture::Leds& Leds() { return leds; }
    struct { bool IsActive() const { return false; } } comptest;
    firmware::amptest::Session amptest;
    firmware::amptest::VerifySession amptestVerify;
};
FirmwareSystem* system = nullptr;
struct Context { fxe::Console* console = nullptr; bool* settings = nullptr; } context;
FirmwareSystem& GetSystem() { return *system; }
Context& GetSystemContext() { return context; }
bool GetFlipSidesSetting(const bool& rotate) { return rotate; }
bool ClearLedOutput(FirmwareSystem& sys) {
    if (sys.ledAnimation.IsQuiescent() && !sys.powerRail.Is5vEnabled()) return true;
    sys.ledLastCommandCompleted = sys.ledAnimation.Submit(firmware::RawDieLedCommand{});
    return sys.ledLastCommandCompleted;
}
void RefreshLedPowerSourceStatus(FirmwareSystem&, uint32_t, bool) {}
uint16_t GetActiveLedCurrentLimitMa(const FirmwareSystem& sys) { return sys.ledPowerLimitMa; }
uint16_t EstimatePhysicalFrameLedCurrentMa(FirmwareSystem&,
        const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>&, uint16_t) {
    Check(false, "verify budget loads are outside this input/render journey"); return 0U;
}
void RenderPhysicalFrameWithPowerBudget(FirmwareSystem&,
        const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>&, uint16_t) {
    Check(false, "verify budget loads are outside this input/render journey");
}
}

void AmptestFixtureDelay(unsigned long ms) { fxe::ManualMillis::AdvanceMs(ms); }
namespace hardware { constexpr bool AMPTEST_FIXTURE_IS_S3 = true; }
#define ARDUINO 1
#define firmware_internal amptest_fixture
#define delay AmptestFixtureDelay
#include "../../src/firmware_led_rail_quiesce.cpp"
#define SAMPLE_MODES RUNTIME_SAMPLE_MODES
#include "../../src/amptest_session_runtime.cpp"
#undef SAMPLE_MODES
#include "../../src/amptest_session_interaction.cpp"
#include "../../src/amptest_verify_session.cpp"
#define TurnOffRailWithLedDrain VerifyTurnOffRailWithLedDrain
#include "../../src/amptest_verify_session_hardware.cpp"
#undef TurnOffRailWithLedDrain
#define HW_IS_S3 AMPTEST_FIXTURE_IS_S3
#include "../../src/firmware_console_amptest.cpp"
#undef HW_IS_S3
#undef delay
#undef firmware_internal
#undef ARDUINO

namespace amptest_fixture {
struct Fixture {
    fxe::ScopedMillisProviderOverride clock{&fxe::ManualMillisProvider};
    FirmwareSystem sys;
    test_support::OwnerPairingConsoleTransport transport;
    Console console{transport.hybrid};
    std::string output;
    bool rotate = false;
    Fixture() {
        fxe::ManualMillis::SetNowMs(0U); system = &sys; context = {&console, &rotate};
        console.RegisterCommand("amptest", "production amptest", [this](const Console::Args& args) {
            return console_commands::AmptestCommand(console, args);
        });
        // Same entered-line priority as firmware_console.cpp. Startup admission
        // and BLE transport are bypassed; Console parsing and both owners run.
        console.SetEnteredLineCallback([this](const char* line) {
            return sys.amptest.HandleEnteredLine(console, line) ||
                   sys.amptestVerify.HandleEnteredLine(console, line);
        });
    }
    void Send(const char* line, bool remote = false) {
        transport.usb.output.clear();
        (remote ? transport.ble.input : transport.usb.input) += std::string(line) + '\n';
        console.Update(); output = transport.usb.output;
        unsigned boardLit = 0U, statusLit = 0U;
        for (size_t i = 0U; i < sys.leds.GetLedCount(); ++i) {
            const auto p = sys.leds.pixels[i];
            if (p.r || p.g || p.b || p.w) (i < 256U ? boardLit : statusLit)++;
        }
        const auto step = output.find("step ");
        const auto end = output.find('\n', step);
        const auto reported = step == std::string::npos ? std::string() : output.substr(step, end - step);
        std::printf("[AMPTEST_INPUT] '%s' raw=%u verify=%u rail=%u ambient=%u lit_board=%u lit_status=%u %s\n",
                    line, sys.amptest.IsActive(), sys.amptestVerify.IsActive(),
                    sys.powerRail.on, sys.ambientLights.on, boardLit, statusLit, reported.c_str());
    }
    bool Says(const char* text) const { return output.find(text) != std::string::npos; }
    void AllPixels(Pixel expected, const char* message) {
        Check(std::all_of(sys.leds.pixels.begin(),
                          sys.leds.pixels.begin() + sys.leds.GetLedCount(),
                          [expected](Pixel p) {
            return p.r == expected.r && p.g == expected.g && p.b == expected.b && p.w == expected.w;
        }), message);
    }
    void Step(const char* expected) { Check(Says(expected), expected); }
};
void RawRenderAndMeasurements() {
    Fixture f;
    f.Send("amptest"); f.Step("step 1/7 core_baseline");
    Check(!f.sys.powerRail.on && !f.sys.ambientLights.on && f.sys.gamePersistence.cancels == 1U &&
              f.sys.ledAnimation.IsQuiescent(),
          "start cancels search and confirms output-owner quiescence before core baseline");
    f.Send("100"); f.Step("step 2/7 ambient_baseline");
    f.Send("110"); f.Step("step 3/7 rail_baseline");
    f.Send("200"); f.Step("step 4/7 mode=red level=1/30 pwm=9");
    f.AllPixels({9U, 0U, 0U, 0U}, "red reaches all 256 board and 32 status LEDs at safe level 1");
    Check(f.sys.ledLastCommandCompleted && !f.sys.ledAnimation.IsQuiescent(),
          "raw command records confirmed completion after the output owner resumes");
    f.rotate = true; f.sys.amptest.Render();
    f.AllPixels({9U, 0U, 0U, 0U}, "uniform physical load includes last row and status after rotation");
    f.sys.amptest.HandleButtonEvent({modules::UserButton::Up, fxe::Button::Event::Press});
    f.AllPixels({17U, 0U, 0U, 0U}, "DPad adjusts the same complete physical frame");
    // level 2 => PWM17; delta288mA => full-scale4320mA => 15mA per physical LED.
    f.Send("488");
    Check(f.Says("per_led=15.00 mA per_channel=15.00 mA"),
          "sample normalization uses the same 288 LEDs that were driven");
    f.Send("amptest status");
    Check(f.Says("mode=red level=2/30") && f.Says("per_led=15.00 mA"),
          "explicit status preserves the step and reports physical-count calibration");
    f.Send("amptest start"); f.Step("mode=red level=2/30");
    f.Send(""); f.Step("step 5/7 mode=green level=1/30");
    f.AllPixels({0U, 9U, 0U, 0U}, "blank Enter advances and resets green to safe level 1");
    f.sys.amptest.HandleButtonEvent({modules::UserButton::Up, fxe::Button::Event::Press});
    f.Send("488");
    f.Send("next"); f.Step("step 6/7 mode=blue level=1/30");
    f.AllPixels({0U, 0U, 9U, 0U}, "next advances to complete blue frame");
    f.sys.amptest.HandleButtonEvent({modules::UserButton::Up, fxe::Button::Event::Press});
    f.Send("488");
    f.Send("skip"); f.Step("step 7/7 mode=white level=1/30");
    f.AllPixels({0U, 0U, 0U, 9U}, "white drives only the raw W die on every physical LED");
    f.sys.amptest.HandleButtonEvent({modules::UserButton::Up, fxe::Button::Event::Press});
    f.Send("488");
    f.Send("amptest");
    Check(f.Says("rgb_derived_fullscale=13160 mA delta_vs_rail=12960 mA per_led=45.00 mA per_channel=15.00 mA") &&
              f.Says("rgbw_derived_fullscale=17480 mA delta_vs_rail=17280 mA per_led=60.00 mA per_channel=15.00 mA"),
          "derived RGB and RGBW summaries normalize the measured physical load");
    Check(!f.sys.amptest.IsActive() && f.Says("amptest: complete"),
          "repeated bare command completes the active last color");
    Check(f.sys.powerRail.on && f.sys.ambientLights.on, "completion restores runtime power state");
    f.AllPixels({}, "completion clears every raw LED");
}
void RepeatedCommandAndVerify() {
    Fixture f;
    f.Send("amptest"); f.Send("amptest"); f.Step("step 2/7 ambient_baseline");
    f.Send("amptest restart"); f.Step("step 1/7 core_baseline");
    f.Send("amptest verify");
    Check(!f.sys.amptest.IsActive() && f.sys.amptestVerify.IsActive() && f.Says("step 1/8"),
          "explicit verify replaces raw mode at its core baseline");
    f.Send("amptest");
    Check(!f.sys.amptest.IsActive() && f.sys.amptestVerify.IsActive() && f.Says("step 2/8"),
          "bare amptest advances active verify without switching to raw characterization");
    f.Send("amptest verify status"); f.Step("step 2/8");
    f.Send("amptest verify restart"); f.Step("step 1/8");
    f.Send("amptest verify start"); f.Step("step 1/8");
    f.Send("amptest start");
    Check(f.sys.amptest.IsActive() && !f.sys.amptestVerify.IsActive() && f.Says("step 1/7"),
          "explicit raw start still selects raw characterization");
    f.Send("amptest verify restart");
    Check(!f.sys.amptest.IsActive() && f.sys.amptestVerify.IsActive() && f.Says("step 1/8"),
          "verify restart selects verification without leaving raw characterization active");
    f.Send("amptest restart");
    Check(f.sys.amptest.IsActive() && !f.sys.amptestVerify.IsActive() && f.Says("step 1/7"),
          "raw restart selects characterization without leaving verification active");
    f.Send("amptest stop");
    Check(!f.sys.amptest.IsActive() && f.sys.powerRail.on && f.sys.ambientLights.on,
          "explicit stop exits and restores runtime hardware");
}
void RgbOnlyProfileRejectsRgbwCalibration() {
    Fixture f;
    f.sys.runtimeHardware.resolved.revision = hardware::BoardRevision::V3_3S3;
    f.Send("amptest");
    Check(!f.sys.amptest.IsActive() &&
              f.Says("V3.3 RGB profile unsupported"),
          "RGB-only V3.3 cannot silently run a white-die calibration");
}
void BenchAuthorityAndConfirmedEntry() {
    Fixture f;
    f.sys.gamePersistence.clock.running=true;
    f.Send("amptest verify", true);
    Check(!f.sys.amptestVerify.IsActive() && f.sys.powerRail.on && f.sys.leds.shows == 0U,
          "governed diagnostic entry cannot interrupt an active game clock");
    f.sys.gamePersistence.clock.running=false;
    if (f.sys.amptestVerify.IsActive()) f.Send("amptest verify stop");
    f.Send("amptest", true);
    Check(f.sys.amptest.IsActive() && !f.sys.powerRail.on && f.sys.leds.shows != 0U,
          "Bluetooth raw entry uses the existing confirmed-clear hardware owner");
    if (f.sys.amptest.IsActive()) f.Send("amptest stop");
    f.sys.gamePersistence.cancels = 0U;
    f.sys.leds.transferCompletes = false;
    f.Send("amptest");
    Check(!f.sys.amptest.IsActive() && f.sys.powerRail.on && f.sys.gamePersistence.cancels == 0U,
          "unconfirmed raw entry leaves runtime rail and input ownership intact");
    if (f.sys.amptest.IsActive()) f.Send("amptest stop");
    f.Send("amptest verify", true);
    Check(!f.sys.amptestVerify.IsActive() && f.sys.powerRail.on,
          "governed Bluetooth verification also waits for a confirmed rail clear");
    if (f.sys.amptestVerify.IsActive()) f.Send("amptest verify stop");
    f.sys.leds.transferCompletes = true;
    f.Send("amptest");
    f.Send("amptest verify status", true);
    Check(f.Says("amptest verify: idle") && f.sys.amptest.IsActive(),
          "Bluetooth can inspect verification status without changing the active raw owner");
    f.Send("next", true);
    Check(f.sys.ambientLights.on,
          "Bluetooth next advances the existing raw baseline");
    f.Send("100", true);
    Check(f.Says("step 3/7"), "Bluetooth current reading advances one baseline step");
    f.Send("", true);
    Check(f.sys.amptest.IsActive() && f.Says("step 4/7"),
          "numeric and blank Bluetooth shortcuts use the existing raw session");
    f.Send("next");
    Check(f.Says("step 5/7"),
          "USB shortcut uses the same entered-line owner");
    f.Send("amptest stop");
    f.Send("amptest verify", true);
    Check(f.sys.amptestVerify.IsActive(),
          "governed verification remains available over Bluetooth");
    f.Send("next", true);
    Check(f.sys.ambientLights.on,
          "governed verification retains its Bluetooth interactive input");
    f.Send("amptest verify stop", true);
}
}

int RunAmpTestProductionJourney() {
    amptest_fixture::failures = 0;
    amptest_fixture::RawRenderAndMeasurements();
    amptest_fixture::RepeatedCommandAndVerify();
    amptest_fixture::RgbOnlyProfileRejectsRgbwCalibration();
    amptest_fixture::BenchAuthorityAndConfirmedEntry();
    return amptest_fixture::failures;
}
