#include <cstdint>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

#include <Arduino.h>

#include "foxie_embedded/millis.hpp"

namespace {

enum class Kind { Mode, Write, Milliseconds, Microseconds, PreparePwm, RunPwm };
struct Operation {
    Kind kind;
    uint32_t pin;
    uint32_t value;
};
std::vector<Operation> g_operations;
uint32_t g_recordedUs = 0U;
uint32_t g_recordedPulses = 0U;
unsigned g_recordCount = 0U;
unsigned g_failures = 0U;
std::function<void()> g_duringPrecharge;

void Check(const bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::printf("[FAIL] %s\n", message);
    }
}

void RecordedPinMode(const uint8_t pin, const int mode) {
    g_operations.push_back({Kind::Mode, pin, static_cast<uint32_t>(mode)});
}
void RecordedDigitalWrite(const uint8_t pin, const int value) {
    g_operations.push_back({Kind::Write, pin, static_cast<uint32_t>(value)});
}
void RecordedDelay(const unsigned long value) {
    g_operations.push_back({Kind::Milliseconds, 0U, static_cast<uint32_t>(value)});
}
void RecordedDelayMicroseconds(const unsigned int value) {
    g_operations.push_back({Kind::Microseconds, 0U, value});
    if (auto callback = std::exchange(g_duringPrecharge, {})) callback();
}
int RecordedPreparePrechargePwm() {
    g_operations.push_back({Kind::PreparePwm, 0U, 0U});
    return 0;
}
int RecordedRunPrechargePwm(const uint8_t pin) {
    g_operations.push_back({Kind::RunPwm, pin, 0U});
    if (auto callback = std::exchange(g_duringPrecharge, {})) callback();
    return 0;
}

} // namespace

namespace firmware {
void RecordedBootPrecharge(const uint32_t nominalUs, const uint32_t pulses) {
    g_recordedUs = nominalUs;
    g_recordedPulses = pulses;
    ++g_recordCount;
}
} // namespace firmware

// Compile the actual Arduino owner with recorded GPIO/delay boundaries and a
// distinct class name so the native no-hardware implementation remains linked.
// PWM backend calls are recorded here; the SDK/RMT fixture covers actual SDK
// sequencing. The legacy train records its actual GPIO taps and delays.
// Neither fixture simulates electrical waveforms or physical boot.
#define ARDUINO 1
#define LITCH_HW_VARIANT_S3 1
#define OUTPUT 1
#define HIGH 1
#define LOW 0
#define PowerRail RecordedPowerRail
#define pinMode RecordedPinMode
#define digitalWrite RecordedDigitalWrite
#define delay RecordedDelay
#define delayMicroseconds RecordedDelayMicroseconds
#define PreparePrechargePwm RecordedPreparePrechargePwm
#define RunPrechargePwm RecordedRunPrechargePwm
#define RecordUsbBootPrecharge RecordedBootPrecharge
#include "../../src/power_rail.cpp"
#undef RecordUsbBootPrecharge
#undef RunPrechargePwm
#undef PreparePrechargePwm
#undef delayMicroseconds
#undef delay
#undef digitalWrite
#undef pinMode
#undef PowerRail
#undef LOW
#undef HIGH
#undef OUTPUT
#undef LITCH_HW_VARIANT_S3
#undef ARDUINO

namespace {

void Expect(const size_t index, const Kind kind, const uint32_t pin,
            const uint32_t value) {
    Check(index < g_operations.size(), "completed enable has every required operation");
    if (index < g_operations.size()) {
        const auto& operation = g_operations[index];
        Check(operation.kind == kind && operation.pin == pin && operation.value == value,
              "actual rail enable preserves ordered GPIO and delay operations");
    }
}

void CheckEnableSequence(const hardware::BoardHardwareProfile& profile) {
    const bool usesPwm = profile.revision == hardware::BoardRevision::V3_3S3;
    Check(g_operations.size() == (usesPwm ? 13U : 30010U),
          "rail enable uses only its revision's precharge and handoff");
    Expect(0U, Kind::Write, profile.boardLedPin, 0U);
    Expect(1U, Kind::Mode, profile.boardLedPin, 1U);
    Expect(2U, Kind::Write, profile.statusLedPin, 0U);
    Expect(3U, Kind::Mode, profile.statusLedPin, 1U);
    Expect(4U, Kind::Milliseconds, 0U, 1U);
    Expect(5U, Kind::Write, profile.fiveVoltEnable, 0U);
    Expect(6U, Kind::Mode, profile.fiveVoltEnable, 1U);
    Expect(7U, Kind::Milliseconds, 0U, 1U);
    if (usesPwm) {
        Expect(8U, Kind::PreparePwm, 0U, 0U);
        Expect(9U, Kind::Milliseconds, 0U, 5U);
        Expect(10U, Kind::RunPwm, profile.fiveVoltEnable, 0U);
        Expect(11U, Kind::Write, profile.fiveVoltEnable, 1U);
        Expect(12U, Kind::Milliseconds, 0U, 5U);
    } else {
        for (size_t pulse = 0U; pulse < 10000U; ++pulse) {
            const size_t index = 8U + pulse * 3U;
            Expect(index, Kind::Write, profile.fiveVoltEnable, 1U);
            Expect(index + 1U, Kind::Write, profile.fiveVoltEnable, 0U);
            Expect(index + 2U, Kind::Microseconds, 0U, 7U);
        }
        Expect(30008U, Kind::Write, profile.fiveVoltEnable, 1U);
        Expect(30009U, Kind::Milliseconds, 0U, 10U);
    }
}

void CheckRevision(const hardware::BoardRevision revision,
                   const uint32_t nominalUs, const uint32_t pulses) {
    const auto profile = hardware::BoardHardwareProfileFor(revision);
    firmware::RecordedPowerRail rail;
    rail.Configure(profile);
    g_operations.clear();
    g_recordedUs = 0U;
    g_recordedPulses = 0U;
    g_recordCount = 0U;
    const auto* outputEnabled = rail.LedOutputEnabledState();
    Check(outputEnabled != nullptr && !outputEnabled->load(std::memory_order_acquire),
          "configured controlled supply starts with output gated");
    g_duringPrecharge = [&] {
        const auto beforeRepeat = g_operations.size();
        Check(rail.Is5vEnableRequested() && !rail.Is5vEnabled(),
              "requested On is authoritative during precharge before renderer readiness");
        rail.Enable5v();
        rail.Begin();
        Check(g_operations.size() == beforeRepeat,
              "repeated On during precharge does not drive pins or restart progress");
    };
    rail.Begin();
    CheckEnableSequence(profile);
    const auto enabledOperations = g_operations.size();
    rail.Enable5v();
    rail.Begin();
    Check(g_operations.size() == enabledOperations,
          "repeated On after startup does not restart either precharge backend");
    Check(rail.Is5vEnabled() && g_recordCount == 1U &&
              g_recordedUs == nominalUs && g_recordedPulses == pulses,
          "startup records the selected profile's nominal duration and pulse metadata");
    g_operations.clear();
    rail.Disable5v();
    Check(!rail.Is5vEnabled() && !outputEnabled->load(std::memory_order_acquire), "runtime disable closes borrowed output state");
    Expect(0U, Kind::Write, profile.boardLedPin, 0U);
    Expect(1U, Kind::Mode, profile.boardLedPin, 1U);
    Expect(2U, Kind::Write, profile.statusLedPin, 0U);
    Expect(3U, Kind::Mode, profile.statusLedPin, 1U);
    Expect(4U, Kind::Write, profile.fiveVoltEnable, 0U);
    Expect(5U, Kind::Mode, profile.fiveVoltEnable, 1U);
    Expect(6U, Kind::Milliseconds, 0U, 1U);
    Check(g_operations.size() == 7U, "disable retains its existing 1 ms hold");
    rail.Disable5v();
    Check(g_operations.size() == 7U && !rail.Is5vEnableRequested(),
          "repeated Off leaves an already-off rail untouched");
    g_operations.clear();
    rail.Enable5v();
    CheckEnableSequence(profile);
    Check(rail.Is5vEnabled() && *outputEnabled && g_recordCount == 1U,
          "runtime re-enable uses the same profile without overwriting boot evidence");
    std::printf("[POWER_RAIL] revision=%u nominal_us=%lu cycles=%lu begin+reenable checked\n",
                static_cast<unsigned>(revision), static_cast<unsigned long>(nominalUs),
                static_cast<unsigned long>(pulses));
}

} // namespace

int main() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    g_operations.reserve(30010U);
    // Expected timings are independent of the driver constants. Unknown revisions use V2 timings.
    CheckRevision(hardware::BoardRevision::V3_3S3, 5000U, 0U);
    CheckRevision(hardware::BoardRevision::V2S3, 75000U, 10000U);
    CheckRevision(hardware::BoardRevision::V3S3, 75000U, 10000U);
    CheckRevision(hardware::BoardRevision::Unknown, 75000U, 10000U);
    std::printf("[POWER_RAIL] failures=%u\n", g_failures);
    return g_failures == 0U ? 0 : 1;
}
