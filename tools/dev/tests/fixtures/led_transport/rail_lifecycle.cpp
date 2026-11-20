// Compile the actual command, rail owner and transport together. Console argument
// parsing/output transport and SDK GPIO/RMT are adapters; command policy is real.
#undef ARDUINO
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"
#define ARDUINO 1
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include "firmware/power_rail.hpp"
#include "driver/ledc.h"
#include "hardware/neopixel_led_renderer.hpp"
#include "firmware/led_runtime_admission.hpp"
#include "firmware/recovery_led_frame.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "foxie_embedded/console_transport.hpp"
#include "rmt_fixture.hpp"

class Console {
 public:
    fxe::ConsoleInputSource source = fxe::ConsoleInputSource::Local;
    fxe::ConsoleInputSource CommandInputSource() const { return source; }
    struct Args {
        unsigned argc = 2U;
        std::string_view mode;
        std::string_view Get(unsigned) const { return mode; }
    };
    std::string output;
    void PrintLn(const char* text) { output += text; output += '\n'; }
};
namespace firmware { void RecordUsbBootPrecharge(uint32_t, uint32_t) {} }
namespace rail_fixture {
using PhysicalLedFrame = hardware::PhysicalLedFrame;

struct FirmwareSystem {
    firmware::PowerRail powerRail;
    hardware::NeoPixelLedRenderer leds;
    struct {
        hardware::BoardHardwareProfile resolved =
            hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3S3);
    } runtimeHardware;
    // This fixture exercises the inactive-owner transport path. It does not
    // simulate a running animation task or establish asynchronous Sleep safety.
    struct {
        bool IsStarted() const { return false; }
        bool Quiesce() { std::abort(); }
        void Resume() {}
    } ledAnimation;
    struct { bool IsActive() const { return false; } } comptest;
    firmware::RecoveryPolicy recoveryPolicy;
    bool recoveryFrameInitialized = false;
    firmware::RecoveryLedFrame recoveryLastFrame{};
    FirmwareSystem(bool split, bool full = false)
        : leds(full ? 256U : 4U, 47U, split ? (full ? 32U : 2U) : 0U, 33U, true) {}
    hardware::NeoPixelLedRenderer& Leds() { return leds; }
};
FirmwareSystem* system;
FirmwareSystem& GetSystem() { return *system; }
bool ClearLedOutput(FirmwareSystem& sys) {
    sys.leds.Clear();
    sys.leds.Show();
    return sys.leds.LastShowCompleted();
}
bool LedOutputCompleted(const FirmwareSystem& sys) {
    return sys.leds.LastShowCompleted();
}
void RenderPhysicalFrameWithRecoveryPowerBudget(
    FirmwareSystem& sys,
    const PhysicalLedFrame& frame,
    uint16_t scale) {
    modules::PowerBudgetPolicy policy({});
    modules::BudgetAwareLedPipeline pipeline(sys.leds, policy);
    pipeline.SetBrightnessScale01(scale / 256.0f);
    pipeline.RenderFrame(frame.data(), sys.leds.GetLedCount());
}
#define firmware_internal rail_fixture
#include "recovery_surface.inc"
}
#include "../../../../../src/firmware_led_rail_quiesce.cpp"
#include "../../../../../src/firmware_console_rail.cpp"
#undef firmware_internal

namespace {
// The fallback exists only to run this same behavioral fixture against the
// archived baseline (which has no binding API), rather than a parallel model.
template<class Renderer, class Rail>
auto BindSupply(Renderer& renderer, Rail& rail, int) -> decltype(
    renderer.SetPowerEnabledState(rail.LedOutputEnabledState()), void()) {
    renderer.SetPowerEnabledState(rail.LedOutputEnabledState());
}
template<class Renderer, class Rail>
void BindSupply(Renderer&, Rail&, long) {}
}

unsigned RailLifecycleChecks() {
    unsigned failures = 0U;
    const auto check = [&](bool ok, const char* why) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", why); }
    };
    for (bool split : {false, true}) {
        fixture::pwm::Reset();
        rail_fixture::FirmwareSystem sys(split); rail_fixture::system = &sys;
        auto profile = hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3);
        if (!split) profile.statusLedPin = profile.boardLedPin;
        sys.powerRail.Configure(profile);
        BindSupply(sys.leds, sys.powerRail, 0);
        hardware::RmtFlashArbiter arbiter; sys.leds.SetRmtFlashArbiter(&arbiter);
        fixture::arbiter = &arbiter;
        const auto beforeBoot = fixture::starts;
        sys.leds.Begin();
        check(fixture::starts == beforeBoot && !sys.leds.LastShowCompleted(),
              "configured disabled supply suppresses even Begin black transmission");
        sys.powerRail.Begin(); sys.leds.RestoreOutputPinRouting(); sys.leds.Show();
        check(fixture::pwm::state.badAttach == 0U && fixture::pwm::state.badConfig == 0U &&
                  fixture::pwm::state.paused && fixture::pwm::state.stopped && fixture::pwm::state.idle == 1U,
              "PWM starts at settled zero on the reserved clock/channel and ends paused HIGH");
        const auto& pwmHistory = fixture::pwm::state.history;
        check(pwmHistory.size() == 16U, "PWM enable uses only the bounded configured SDK sequence");
        if (pwmHistory.size() == 16U) {
            const std::array<unsigned, 4> duties{1U, 2U, 4U, 8U};
            for (size_t step = 0U; step < duties.size(); ++step) {
                const auto index = 6U + 2U * step;
                check(pwmHistory[index].api == fixture::pwm::Api::Duty &&
                          pwmHistory[index].value == duties[step] &&
                          pwmHistory[index + 1U].api == fixture::pwm::Api::Update,
                      "hardware PWM uses the independently requested tiny-duty stages");
                const auto next = step + 1U < duties.size() ? index + 2U : 14U;
                check(pwmHistory[next].atUs - pwmHistory[index + 1U].atUs >= 1250U,
                      "each PWM duty is retained for at least1250us before advancing or stopping HIGH");
            }
            check(pwmHistory[14U].api == fixture::pwm::Api::Stop && pwmHistory[14U].value == 1U,
                  "PWM stops at continuous HIGH only after the complete ramp");
        }
        check(sys.leds.LastShowCompleted(), "first powered frame completes");
        Console console;
        const auto command = [&](std::string_view mode) {
            console.output.clear(); return rail_fixture::console_commands::FiveVEnCommand(console, {2U, mode});
        };
        const auto beforeRepeatedOn = fixture::events.size();
        const auto beforeRepeatedPwm = fixture::pwm::state.calls;
        const auto beforeRepeatedFrame = fixture::starts;
        sys.recoveryFrameInitialized = true;
        for (const auto source : {fxe::ConsoleInputSource::Remote,
                                  fxe::ConsoleInputSource::Unknown,
                                  fxe::ConsoleInputSource::Local}) {
            console.source = source;
            check(command("on") && sys.powerRail.Is5vEnabled() &&
                      fixture::events.size() == beforeRepeatedOn &&
                      fixture::pwm::state.calls == beforeRepeatedPwm &&
                      fixture::starts == beforeRepeatedFrame && sys.recoveryFrameInitialized,
                  "repeated On accepts every Console source without GPIO/PWM/frame/cache changes");
        }
        check(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()), "fixture takes flash lease");
        check(!command("off") && sys.powerRail.Is5vEnabled() && console.output.find("OK") == std::string::npos,
              "flash-denied pre-off clear never disables or acknowledges success");
        arbiter.EndFlash();
        fixture::timeoutAfterSubmission = true;
        check(!command("off") && sys.powerRail.Is5vEnabled() && console.output.find("OK") == std::string::npos,
              "unconfirmed pre-off transfer never disables or acknowledges success");
        fixture::timeoutAfterSubmission = fixture::forceTimeout = false;
        fixture::events.clear();
        check(command("off") && !sys.powerRail.Is5vEnabled() &&
                  !sys.leds.LastShowCompleted() && console.output == "OK\n",
              "successful off acknowledges completed clear then disables");
        size_t lastSubmit=0U, dataLow=0U, enLow=0U;
        bool boardLow=false, statusLow=false;
        for (size_t i=0U; i<fixture::events.size(); ++i) {
            const auto e=fixture::events[i];
            if (e.kind==fixture::EventKind::Submit) lastSubmit=i;
            if (e.kind==fixture::EventKind::DataLow &&
                (e.pin==profile.boardLedPin || e.pin==profile.statusLedPin)) {
                dataLow=i;
                boardLow |= e.pin==profile.boardLedPin;
                statusLow |= e.pin==profile.statusLedPin;
            }
            if (e.kind==fixture::EventKind::Low && e.pin==profile.fiveVoltEnable) enLow=i;
        }
        check(boardLow && statusLow && lastSubmit < dataLow && dataLow < enLow,
              "both single/split chain routes become GPIO-low after drain and before EN-low");
        const auto offEvents=fixture::events.size();
        sys.leds.RestoreOutputPinRouting();
        check(fixture::events.size()==offEvents,
              "routing restore cannot reconnect an off supply");
        const auto offStarts=fixture::starts;
        const auto generation=arbiter.CompletedRenderGeneration();
        modules::PowerBudgetPolicy policy({});
        modules::BudgetAwareLedPipeline pipeline(sys.leds, policy);
        std::array<modules::Rgb, 6U> frame{};
        for (unsigned tick=0U; tick<50U; ++tick) {
            frame[0] = tick % 2U ? modules::Rgb{8U, 9U, 10U} : modules::Rgb{};
            pipeline.RenderFrame(frame.data(), sys.leds.GetLedCount());
            sys.leds.SetLedRgbwRaw(0U, 2U, 3U, 4U, 5U); sys.leds.Show();
        }
        check(fixture::starts==offStarts && !sys.leds.LastShowCompleted() &&
                  arbiter.CompletedRenderGeneration()==generation,
              "black/nonzero budget and direct raw Shows stay silent without physical or flash-generation acknowledgement");
        check(command("off") && fixture::starts==offStarts,
              "already-off command is idempotent without a new clear transmission");
        fixture::events.clear();
        check(command("on"), "on reopens the existing rail lifecycle");
        check(fixture::pwm::state.badAttach == 0U && fixture::pwm::state.badConfig == 0U,
              "warm enable explicitly resumes the paused PWM timer before zero-settle and attachment");
        pipeline.RenderFrame(frame.data(),sys.leds.GetLedCount());
        size_t enHigh=0U, route=0U, submit=0U;
        for (size_t i=0U; i<fixture::events.size(); ++i) {
            const auto e=fixture::events[i];
            if(e.kind==fixture::EventKind::High && e.pin==profile.fiveVoltEnable) enHigh=i;
            if(e.kind==fixture::EventKind::Route) route=i;
            if(e.kind==fixture::EventKind::Submit) submit=i;
        }
        check(enHigh<route && route<submit && fixture::starts>offStarts && sys.leds.LastShowCompleted(),
              "precharge/stabilization precede routing restore and fresh powered frame");
        sys.powerRail.PrepareForSleep();
        sys.leds.Clear();sys.leds.Show();
        check(!sys.leds.LastShowCompleted(), "sleep rail-off uses the same persistent guard");
        fixture::arbiter=nullptr;
        // Each failure is injected at the real owner's existing SDK boundary.
        // This checks final safe state and acknowledgement, not analogue behavior.
        for (unsigned failedCall = 1U; failedCall <= 16U; ++failedCall) {
            rail_fixture::FirmwareSystem failedSys(split); rail_fixture::system = &failedSys;
            failedSys.powerRail.Configure(profile);
            BindSupply(failedSys.leds, failedSys.powerRail, 0);
            fixture::pwm::Reset(failedCall);
            fixture::events.clear();
            const auto startsBeforeFailure = fixture::starts;
            check(!rail_fixture::EnableLedRail(failedSys) && !failedSys.powerRail.Is5vEnabled() &&
                      !failedSys.powerRail.Is5vEnableRequested() && fixture::starts == startsBeforeFailure,
                  "PWM setup/run/stop failure remains gated and never resumes LED delivery");
            check(fixture::pwm::state.stopped && fixture::pwm::state.idle == 0U,
                  "PWM failure leaves the peripheral stopped at LOW");
            bool endedLow = false;
            for (const auto event : fixture::events) {
                if (event.pin == profile.fiveVoltEnable && event.kind == fixture::EventKind::Low) endedLow = true;
                if (event.pin == profile.fiveVoltEnable && event.kind == fixture::EventKind::High) endedLow = false;
            }
            check(endedLow, "PWM failure forces the GPIO LOW before returning failure");
        }
        fixture::pwm::Reset();
    }
    firmware::LedRuntimeAdmission admission;
    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Denied, 0U);
    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::NotAttempted, 0U);
    check(!admission.Update(false, false, 1U) && admission.ConstrainNextWakeMs(20U, 1U)==20U,
          "rail-off NotAttempted retires denied retry without a 1ms wake loop");
    for (const auto cause : {firmware::RecoveryCause::UpdateFailed,
                             firmware::RecoveryCause::PersistedStateCorrupt}) {
        rail_fixture::FirmwareSystem sys(true, true); rail_fixture::system = &sys;
        sys.powerRail.Configure(hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3));
        BindSupply(sys.leds, sys.powerRail, 0);
        hardware::RmtFlashArbiter arbiter; sys.leds.SetRmtFlashArbiter(&arbiter);
        fixture::arbiter = &arbiter;
        sys.powerRail.Begin(); sys.leds.Begin();
        sys.recoveryPolicy.Begin(cause, firmware::RecoveryBeginMode::ArmImmediately, 0U);
        rail_fixture::RenderRecoverySurface(sys, 0U, false);
        check(sys.recoveryFrameInitialized && sys.leds.LastShowCompleted(),
              "static recovery frame is confirmed before rail cycling");
        Console console;
        check(rail_fixture::console_commands::FiveVEnCommand(console, {2U, "off"}),
              "static recovery accepts confirmed off");
        // No recovery render between off/on: queued commands can share a loop.
        check(rail_fixture::console_commands::FiveVEnCommand(console, {2U, "on"}),
              "static recovery accepts on");
        const auto beforeResume = fixture::starts;
        check(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()), "resume fixture takes flash lease");
        rail_fixture::RenderRecoverySurface(sys, 20U, false);
        check(!sys.recoveryFrameInitialized && fixture::starts == beforeResume,
              "denied first recovery redraw is not cached as displayed");
        arbiter.EndFlash();
        fixture::pixelWrites.clear();
        rail_fixture::RenderRecoverySurface(sys, 40U, false);
        bool lit = false;
        for (const auto& pixel : fixture::pixelWrites) lit |= pixel.r || pixel.g || pixel.b || pixel.w;
        check(fixture::starts > beforeResume && lit && sys.recoveryFrameInitialized,
              "unchanged static recovery status returns after off/on and denied first redraw");
        fixture::arbiter = nullptr;
    }
    rail_fixture::FirmwareSystem unsupported(false);
    auto profile=hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3);
    profile.hasFiveVoltEnable=false;unsupported.powerRail.Configure(profile);
    BindSupply(unsupported.leds,unsupported.powerRail,0);
    const auto before=fixture::starts;
    unsupported.leds.Begin();unsupported.powerRail.Disable5v();unsupported.leds.Clear();unsupported.leds.Show();
    check(fixture::starts>before && unsupported.leds.LastShowCompleted(),
          "profile without rail control keeps output despite false Is5vEnabled");
    return failures;
}
