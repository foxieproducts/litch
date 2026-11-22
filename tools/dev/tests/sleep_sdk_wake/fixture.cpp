#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "sdk_boundary.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/logger.hpp"
#include <esp32s3/ulp.h>

// Use the pinned SDK's actual instruction encodings with fixture-owned RAM.
#undef RTC_SLOW_MEM
#define RTC_SLOW_MEM sleep_sdk::state.slowMemory.data()
#define ARDUINO 1
#define ARDUINO_USB_MODE 0
#define LITCH_ESP32S3 1
#define LITCH_HW_VARIANT_S3 1
#include "../../../../src/sleep_transition.cpp"
#include "../../../../src/sleep_controller.cpp"
#include "../../../../src/p1x_deep_wake.cpp"
#include "../../../../src/sleep_controller_wake.cpp"
#include "../../../../src/sleep_controller_hardware.cpp"
#include "../../../../src/sleep_controller_enter_sleep.cpp"
#include "../../../../src/sleep_entry_orchestration.cpp"

namespace sleep_sdk {
unsigned failures = 0U;
void Check(bool okay, const char* message) {
    if (!okay) { ++failures; std::printf("FAIL: %s\n", message); }
}
size_t At(const char* call) {
    const auto found = std::find(state.calls.begin(), state.calls.end(), call);
    return static_cast<size_t>(found - state.calls.begin());
}
constexpr uint64_t BUTTON_MASK = 0x6f6e0ULL;
constexpr uint64_t MOTION_MASK = 1ULL << 21U;

// Execute the real loaded encodings; unsupported instructions fail instead of
// substituting a second implementation of the button/release policy.
void RunUlpSample() {
    uint16_t registers[4]{};
    unsigned pc = state.ulpEntry;
    for (unsigned steps = 0U; steps < 64U; ++steps) {
        ulp_insn_t instruction{};
        const uint32_t word = state.slowMemory.at(pc);
        std::memcpy(&instruction, &word, sizeof(word));
        unsigned next = pc + 1U;
        switch (word >> 28U) {
            case OPCODE_ALU:
                if (instruction.alu_imm.sub_opcode == SUB_OPCODE_ALU_IMM) {
                    const auto& op = instruction.alu_imm;
                    if (op.sel == ALU_SEL_MOV) registers[op.dreg] = op.imm;
                    else if (op.sel == ALU_SEL_ADD) registers[op.dreg] = registers[op.sreg] + op.imm;
                    else throw std::runtime_error("unsupported ULP immediate ALU");
                } else if (instruction.alu_reg.sub_opcode == SUB_OPCODE_ALU_REG &&
                           instruction.alu_reg.sel == ALU_SEL_MOV) {
                    registers[instruction.alu_reg.dreg] = registers[instruction.alu_reg.sreg];
                } else throw std::runtime_error("unsupported ULP register ALU");
                break;
            case OPCODE_LD:
                registers[instruction.ld.dreg] = state.slowMemory.at(
                    registers[instruction.ld.sreg] + instruction.ld.offset) & 0xffffU;
                break;
            case OPCODE_ST:
                state.slowMemory.at(registers[instruction.st.sreg] + instruction.st.offset) =
                    registers[instruction.st.dreg] | (pc << 16U);
                break;
            case OPCODE_RD_REG: {
                const auto& op = instruction.rd_reg;
                if (op.periph_sel == RD_REG_PERIPH_RTC_IO &&
                    op.addr == (RTC_GPIO_IN_REG & 0xffU) / sizeof(uint32_t) &&
                    op.low == RTC_GPIO_IN_NEXT_S && op.high == RTC_GPIO_IN_NEXT_S) {
                    registers[0] = state.pins[0U].high ? 1U : 0U;
                } else if (op.periph_sel == RD_REG_PERIPH_RTC_CNTL &&
                           op.addr == (RTC_CNTL_LOW_POWER_ST_REG & 0xffU) / sizeof(uint32_t) &&
                           op.low == RTC_CNTL_RDY_FOR_WAKEUP_S && op.high == RTC_CNTL_RDY_FOR_WAKEUP_S) {
                    registers[0] = state.readyForUlpWake ? 1U : 0U;
                } else throw std::runtime_error("unsupported ULP register read");
                break;
            }
            case OPCODE_BRANCH:
                if (instruction.b.sub_opcode != SUB_OPCODE_B || instruction.b.cmp != B_CMP_L)
                    throw std::runtime_error("unsupported ULP branch");
                if (registers[0] < instruction.b.imm)
                    next = instruction.b.sign ? pc - instruction.b.offset : pc + instruction.b.offset;
                break;
            case OPCODE_WR_REG: {
                const auto& op = instruction.wr_reg;
                if (op.periph_sel != RD_REG_PERIPH_RTC_CNTL ||
                    op.addr != (RTC_CNTL_ULP_CP_TIMER_REG & 0xffU) / sizeof(uint32_t) ||
                    op.low != RTC_CNTL_ULP_CP_SLP_TIMER_EN_S ||
                    op.high != RTC_CNTL_ULP_CP_SLP_TIMER_EN_S || op.data != 0U)
                    throw std::runtime_error("unsupported ULP peripheral write");
                state.ulpRunning = false;
                break;
            }
            case OPCODE_END:
                if (!instruction.end.wakeup) throw std::runtime_error("unsupported ULP END");
                if (state.readyForUlpWake && state.ulpRequested) {
                    ++state.acceptedUlpWakes;
                    state.readyForUlpWake = false;
                } else ++state.ignoredUlpWakes;
                break;
            case OPCODE_HALT: return;
            default: throw std::runtime_error("unsupported ULP opcode");
        }
        pc = next;
    }
    throw std::runtime_error("ULP sample exceeded instruction bound");
}

void AdvanceUlpTime(unsigned long milliseconds) {
    for (unsigned long index = 0U; index < milliseconds; ++index) {
        fxe::ManualMillis::AdvanceMs(1U);
        if (state.ulpPeriod == 0U) continue;
        ++state.ulpElapsedMs;
        if ((state.pulseP1DuringTeardown || state.holdP1ThroughEntry) && !state.deepEntered)
            state.pins[0U].high = !(state.ulpElapsedMs >= 20U &&
                (state.holdP1ThroughEntry || state.ulpElapsedMs < 65U));
        if (state.ulpRunning && state.ulpElapsedMs % (state.ulpPeriod / 1000U) == 0U)
            RunUlpSample();
    }
}

struct Journey {
    firmware::SleepController controller;
    fxe::SettingsStore settings;
    bool motion = false;

    explicit Journey(bool enableMotion = false) : motion(enableMotion) {
        state = {};
        fxe::ManualMillis::SetNowMs(0U);
        controller.ConfigureHardwareProfile(hardware::BoardHardwareProfileFor(
            hardware::BoardRevision::V3_3S3));
        settings.Doc()["P1_X_DEEP_WAKE_ENABLED"] = false;
        settings.Doc()["P1_X_WAKE_HOLD_MS"] = 5000U;
        controller.SetPreSleepCallback([](void*) {
            Note("ble-stop-begin");
            if (state.blockBleStop) throw BlockedBleStop{};
            if (state.assertDuringBleStop) state.pins[7U].high = false;
            Note("ble-stop-end");
            return true;
        }, this);
        controller.ConfigureOptionalActiveLowWakeSource(
            21U, [](void*) { Note("motion-prepare"); return true; },
            [](void* context) { return static_cast<Journey*>(context)->motion; }, this);
    }
    firmware::SleepPreparationHooks Preparation() {
        return {this, [](void*) { Note("persistence-begin"); return true; },
            [](void*) { Note("persistence-ready"); return firmware::SleepPersistenceStatus::Succeeded; },
            [](void* p) { return static_cast<Journey*>(p)->controller.CanBeginSleepTeardown(); },
            [](void* p) { return static_cast<Journey*>(p)->controller.PrepareWakeSourceForSleep(); }};
    }
    firmware::SleepEntryHooks Entry() {
        return {this, [](void*) { Note("leds-clear"); return true; },
            [](void*, uint32_t ms) { delay(ms); },
            [](void*) { Note("ambient-stop"); },
            [](void*) { state.railOff = true; Note("rail-off"); },
            [](void* p) { return static_cast<Journey*>(p)->controller.EnterSleepNow(); },
            [](void*, bool) { esp_restart(); }};
    }
    void PrepareAppSleep() {
        Check(controller.RequestSleep(firmware::SleepRequestOrigin::App, 0U, false), "manual Sleep starts its direct fade with activity cancellation disabled");
        controller.Update(settings, 0U, firmware::SleepSuppression::Allow);
        Check(!controller.NoteValidatedMotionOrTapActivity(), "motion does not cancel manual preparation");
        controller.Update(settings, 250U, firmware::SleepSuppression::Allow);
        Check(controller.GetState() == firmware::SleepState::Fading && controller.GetFadeScale256() == 128U &&
                  !controller.NoteValidatedMotionOrTapActivity(), "manual entry retains its half-fade despite motion");
        controller.Update(settings, 500U, firmware::SleepSuppression::Allow);
        Check(firmware::RunSleepLifecycleStep(controller, Preparation(), Entry()) ==
              firmware::SleepLifecycleResult::Pending, "persistence yields before wake setup");
    }
    void PrepareIdleTimeout() {
        settings.Doc()["SLEEP_TIMEOUT"] = "5m";
        controller.Begin();
        fxe::ManualMillis::SetNowMs(300000U);
        controller.Update(settings, 300000U, firmware::SleepSuppression::Allow);
        Check(controller.GetRequestOrigin() == firmware::SleepRequestOrigin::Idle &&
                  controller.GetState() == firmware::SleepState::Countdown,
              "actual five-minute inactivity owner requests Idle sleep");
        fxe::ManualMillis::SetNowMs(303000U);
        controller.Update(settings, 303000U, firmware::SleepSuppression::Allow);
        Check(controller.GetState() == firmware::SleepState::Fading,
              "idle timeout completes its real three-second count-up before fade");
        fxe::ManualMillis::SetNowMs(303500U);
        controller.Update(settings, 303500U, firmware::SleepSuppression::Allow);
        Check(firmware::RunSleepLifecycleStep(controller, Preparation(), Entry()) ==
                  firmware::SleepLifecycleResult::Pending,
              "idle fade reaches the same yielded persistence preflight");
    }
    void Enter() {
        (void)firmware::RunSleepLifecycleStep(controller, Preparation(), Entry());
    }
};

void CompareAppAndIdleEntry() {
    for (const bool motion : {false, true}) {
        Journey app(motion);
        app.PrepareAppSleep();
        try { app.Enter(); Check(false, "app entry must terminate at SDK"); }
        catch (const EnteredSleep&) {}
        const auto appEntry = state;
        Journey idle(motion);
        idle.PrepareIdleTimeout();
        try { idle.Enter(); Check(false, "idle entry must terminate at SDK"); }
        catch (const EnteredSleep&) {}
        Check(appEntry.requestedExt1 == state.requestedExt1 &&
                  appEntry.appliedExt1 == state.appliedExt1 &&
                  appEntry.ulpRequested == state.ulpRequested &&
                  appEntry.ulpRunning == state.ulpRunning &&
                  appEntry.ulpEntry == state.ulpEntry &&
                  appEntry.ulpPeriod == state.ulpPeriod &&
                  appEntry.rtcPowerOn == state.rtcPowerOn &&
                  appEntry.slowMemory == state.slowMemory,
              "App and actual idle timeout retain identical SDK wake requests, ULP code/latch and power policy");
        for (size_t pin = 0U; pin < state.pins.size(); ++pin) {
            const auto& a = appEntry.pins[pin];
            const auto& b = state.pins[pin];
            Check(a.high == b.high && a.rtc == b.rtc && a.pullup == b.pullup &&
                      a.pulldown == b.pulldown && a.held == b.held && a.inputOnly == b.inputOnly,
                  "App and idle timeout finish with identical GPIO mux/pull/hold/direction state");
        }
        Check(appEntry.calls == state.calls,
              "App and idle timeout preserve identical preparation/callback/teardown/wait order");
        std::printf("[SLEEP_SDK] app_vs_actual_idle motion=%u mask=0x%llx ULP/pads/power/order=identical\n",
                    motion, static_cast<unsigned long long>(state.requestedExt1));
    }
}

void SuccessfulAppSleep(bool motion) {
    Journey journey(motion);
    journey.PrepareAppSleep();
    try { journey.Enter(); Check(false, "SDK deep sleep must be terminal"); }
    catch (const EnteredSleep&) {}
    Check(state.deepEntered && state.usbDetached && state.railOff, "production path reaches final SDK deep sleep");
    Check(state.requestedExt1 == (BUTTON_MASK | (motion ? MOTION_MASK : 0U)), "all eleven fitted buttons remain armed independently of motion");
    Check((state.requestedExt1 & 1U) == 0U, "GPIO0 excluded from EXT1");
    Check(state.ulpRequested && state.ulpRunning && state.ulpPeriod == 40000U,
          "actual ULP setup requests 25 Hz GPIO0 wake");
    Check(state.slowMemory[2040U] == 0U && state.slowMemory[state.ulpEntry] != 0U,
          "actual ULP program loaded with cleared release latch");
    Check(At("ext1-request") < At("rail-off") && At("rail-off") < At("ble-stop-begin") &&
          At("ble-stop-end") < At("usb-detach") && At("usb-detach") < At("deep-entry"),
          "wake preparation precedes teardown, BLE stop, USB detach and SDK entry");
    Check(state.appliedExt1 == state.requestedExt1, "SDK applies retained request at entry");
    std::printf("[SLEEP_SDK] app motion=%u requested=0x%llx ulp=%u entry=pass\n", motion,
                static_cast<unsigned long long>(state.requestedExt1), state.ulpRequested);

    // The hardware wake event is injected; execute the real reset-time restore.
    state.wakeCause = motion ? ESP_SLEEP_WAKEUP_ULP : ESP_SLEEP_WAKEUP_EXT1;
    state.resetReason = ESP_RST_DEEPSLEEP;
    firmware::SleepController woke;
    woke.ConfigureHardwareProfile(hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3));
    state.pins[7U].high = false;
    woke.Begin();
    Check(woke.IsDeepSleepWake() && woke.ShouldIgnoreButtonEvents(),
          "actual boot restores EXT1/ULP wake with held-button guard");
    for (unsigned pin = 0U; pin < 22U; ++pin) {
        if ((state.requestedExt1 & (1ULL << pin)) || pin == 0U)
            Check(!state.pins[pin].rtc && state.pins[pin].pullup,
                  "actual boot returns RTC wake pads to digital pull-up inputs");
    }
    woke.UpdateIgnoreGate();
    delay(300U);
    Check(woke.ShouldIgnoreButtonEvents(), "held wake button cannot enter gameplay");
    state.pins[7U].high = true;
    woke.UpdateIgnoreGate();
    delay(199U);
    woke.UpdateIgnoreGate();
    Check(woke.ShouldIgnoreButtonEvents(), "release guard retains the full 200 ms interval");
    delay(1U);
    woke.UpdateIgnoreGate();
    Check(!woke.ShouldIgnoreButtonEvents(), "all released controls resume after the guard");
}

void FailedWakeSetup() {
    for (unsigned failure = 0U; failure < 3U; ++failure) {
        Journey journey;
        journey.PrepareAppSleep();
        state.failExt1 = failure == 0U;
        state.failUlp = failure == 1U;
        state.failRtc = failure == 2U;
        const auto result = firmware::RunSleepLifecycleStep(journey.controller, journey.Preparation(), journey.Entry());
        Check(result == firmware::SleepLifecycleResult::PreparationFailed &&
              journey.controller.GetState() == firmware::SleepState::Awake,
              "failed real wake setup returns awake without teardown");
        Check(!state.railOff && !state.usbDetached && !state.deepEntered,
              "failed wake setup preserves runtime outputs and USB");
        Check(state.requestedExt1 == 0U && !state.ulpRequested,
              "failed wake setup rolls back all requested wake sources");
        bool restored = true;
        for (unsigned pin = 0U; pin < 22U; ++pin)
            if ((BUTTON_MASK & (1ULL << pin)) || pin == 0U) restored &= !state.pins[pin].rtc;
        Check(restored, "failed wake setup restores every RTC button pad to runtime");
        std::printf("[SLEEP_SDK] rejected=%s awake=pass teardown=none\n",
                    failure == 0U ? "ext1" : failure == 1U ? "ulp" : "rtc");
    }
}

void P1PulseDuringTeardownThenFreshWake() {
    enum class BeforeEntry { Released, Pulse, Held };
    for (const auto input : {BeforeEntry::Released, BeforeEntry::Pulse, BeforeEntry::Held}) {
        Journey journey;
        journey.PrepareAppSleep();
        state.pulseP1DuringTeardown = input == BeforeEntry::Pulse;
        state.holdP1ThroughEntry = input == BeforeEntry::Held;
        try { journey.Enter(); Check(false, "SDK deep entry must be terminal"); }
        catch (const EnteredSleep&) {}
        Check(state.deepEntered && state.ulpRunning && state.ignoredUlpWakes == 0U &&
                  (state.slowMemory[2040U] & 0xffffU) == 0U,
              "awake teardown cannot consume P1-X consent or stop its deep-wake timer");
        Check(state.appliedExt1 == BUTTON_MASK && (state.appliedExt1 & (1ULL << 12U)),
              "P2-X remains independently armed on the unchanged EXT1 mask");
        Check(state.pins[0U].high == (input != BeforeEntry::Held),
              "the selected early press is released or still held across actual SDK entry");

        state.pins[0U].high = false; delay(800U);
        Check(state.acceptedUlpWakes == 0U && state.ulpRunning,
              "held GPIO0 cannot wake into ROM before release");
        state.pins[0U].high = true; delay(160U);
        Check(state.acceptedUlpWakes == 0U, "four released samples cannot wake");
        state.pins[0U].high = false; delay(40U);
        state.pins[0U].high = true; delay(160U);
        Check(state.acceptedUlpWakes == 0U, "release bounce resets all five required samples");
        delay(40U);
        Check(state.acceptedUlpWakes == 1U && !state.ulpRunning && state.ignoredUlpWakes == 0U,
              "fresh post-entry P1 press and five stable release samples wake exactly once");
        constexpr auto words = sizeof(firmware::p1x_deep_wake::ULP_PROGRAM) / sizeof(ulp_insn_t);
        Check(state.ulpEntry + words < 2040U, "loaded ULP instructions end before the reserved state word");
        std::printf("[SLEEP_SDK] pre_entry_P1=%s actual_ULP_words=%zu program=[%u,%zu) state=2040 "
                    "ignored_awake_wakes=%u fresh_P1_wakes=%u P2_EXT1=armed\n",
                    input == BeforeEntry::Released ? "released" :
                        input == BeforeEntry::Pulse ? "pulse" : "held",
                    words, state.ulpEntry, state.ulpEntry + words,
                    state.ignoredUlpWakes, state.acceptedUlpWakes);
    }
}

void TeardownAbortAndBlockedStop() {
    Journey aborted;
    aborted.PrepareAppSleep();
    state.assertDuringBleStop = true;
    try { aborted.Enter(); Check(false, "post-teardown abort must restart"); }
    catch (const Restarted&) {}
    Check(!state.deepEntered && !state.usbDetached && At("restart") < state.calls.size(),
          "asserted wake source during BLE callback restarts after teardown");

    Journey blocked;
    blocked.PrepareAppSleep();
    state.blockBleStop = true;
    try { blocked.Enter(); Check(false, "controlled blocked BLE boundary must stop progress"); }
    catch (const BlockedBleStop&) {}
    Check(state.railOff && !state.deepEntered && !state.usbDetached &&
          state.requestedExt1 == BUTTON_MASK,
          "blocked pre-sleep callback is dark with sources requested but CPU not asleep");
    std::puts("[SLEEP_SDK] controlled blocked-callback dark=1 sdk-entry=0; physical cause remains unproven");
}
} // namespace sleep_sdk

void delay(unsigned long ms) {
    sleep_sdk::Note("wait-" + std::to_string(ms));
    sleep_sdk::AdvanceUlpTime(ms);
}
void delayMicroseconds(unsigned int) {}
void pinMode(unsigned pin, unsigned mode) {
    sleep_sdk::state.pins[pin].inputOnly = mode == INPUT || mode == INPUT_PULLUP;
    if (mode == INPUT_PULLUP) sleep_sdk::state.pins[pin].pullup = true;
}
int digitalRead(unsigned pin) { return sleep_sdk::state.pins[pin].high ? HIGH : LOW; }
void digitalWrite(unsigned pin, unsigned level) { sleep_sdk::state.pins[pin].high = level != LOW; }
esp_err_t gpio_pullup_dis(gpio_num_t pin) { sleep_sdk::state.pins[pin].pullup = false; return ESP_OK; }
esp_err_t gpio_pulldown_dis(gpio_num_t pin) { sleep_sdk::state.pins[pin].pulldown = false; return ESP_OK; }
esp_err_t gpio_hold_dis(gpio_num_t pin) { sleep_sdk::state.pins[pin].held = false; return ESP_OK; }
esp_err_t gpio_wakeup_enable(gpio_num_t, gpio_int_type_t) { return ESP_OK; }
esp_err_t gpio_wakeup_disable(gpio_num_t) { return ESP_OK; }
bool rtc_gpio_is_valid_gpio(gpio_num_t pin) { return pin <= 21U; }
esp_err_t rtc_gpio_init(gpio_num_t pin) {
    if (sleep_sdk::state.failRtc) return ESP_FAIL;
    sleep_sdk::state.pins[pin].rtc = true; return ESP_OK;
}
esp_err_t rtc_gpio_deinit(gpio_num_t pin) { sleep_sdk::state.pins[pin].rtc = false; return ESP_OK; }
esp_err_t rtc_gpio_pullup_en(gpio_num_t pin) { sleep_sdk::state.pins[pin].pullup = true; return ESP_OK; }
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin) { sleep_sdk::state.pins[pin].pulldown = false; return ESP_OK; }
esp_err_t rtc_gpio_hold_dis(gpio_num_t pin) { sleep_sdk::state.pins[pin].held = false; return ESP_OK; }
esp_err_t rtc_gpio_set_direction(gpio_num_t pin, rtc_gpio_mode_t mode) {
    sleep_sdk::state.pins[pin].inputOnly = mode == RTC_GPIO_MODE_INPUT_ONLY; return ESP_OK;
}
esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_wakeup_cause_t) {
    sleep_sdk::state.requestedExt1 = 0U; sleep_sdk::state.ulpRequested = false;
    sleep_sdk::Note("wake-disable"); return ESP_OK;
}
esp_err_t esp_sleep_enable_gpio_wakeup() { return ESP_OK; }
esp_err_t esp_sleep_enable_ulp_wakeup() {
    if (sleep_sdk::state.failUlp) return ESP_FAIL;
    sleep_sdk::state.ulpRequested = true; sleep_sdk::Note("ulp-request"); return ESP_OK;
}
esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t mask, esp_sleep_ext1_wakeup_mode_t mode) {
    sleep_sdk::Check(mode == ESP_EXT1_WAKEUP_ANY_LOW, "active-low EXT1 selection");
    sleep_sdk::Check(sleep_sdk::state.appliedExt1 == 0U, "no hardware mask expected before SDK entry");
    if (sleep_sdk::state.failExt1) return ESP_FAIL;
    sleep_sdk::state.requestedExt1 = mask; sleep_sdk::Note("ext1-request"); return ESP_OK;
}
esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t, esp_sleep_pd_option_t option) { sleep_sdk::state.rtcPowerOn = option == ESP_PD_OPTION_ON; return ESP_OK; }
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t) { return ESP_OK; }
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return sleep_sdk::state.wakeCause; }
esp_reset_reason_t esp_reset_reason() { return sleep_sdk::state.resetReason; }
uint64_t esp_sleep_get_ext1_wakeup_status() { return sleep_sdk::state.appliedExt1; }
void esp_deep_sleep_start() {
    using namespace sleep_sdk;
    Note("deep-entry");
    Check(state.requestedExt1 != 0U && state.ulpRequested && state.rtcPowerOn,
          "SDK entry retains EXT1 and ULP requests plus RTC power");
    for (unsigned pin = 0U; pin < 22U; ++pin) {
        if ((state.requestedExt1 & (1ULL << pin)) || pin == 0U)
            Check(state.pins[pin].rtc && state.pins[pin].pullup && !state.pins[pin].pulldown &&
                  !state.pins[pin].held && state.pins[pin].inputOnly,
                  "final leakage policy preserves each requested RTC wake pad");
    }
    state.appliedExt1 = state.requestedExt1;
    state.deepEntered = true;
    state.readyForUlpWake = true;
    throw EnteredSleep{};
}
esp_err_t esp_light_sleep_start() { throw std::runtime_error("unexpected light sleep"); }
void esp_restart() { sleep_sdk::Note("restart"); throw sleep_sdk::Restarted{}; }
bool tud_disconnect() { sleep_sdk::state.usbDetached = true; sleep_sdk::Note("usb-detach"); return true; }
void SerialBoundary::flush() {}
void SerialBoundary::end() { sleep_sdk::Note("serial-end"); }
void WireBoundary::end() { sleep_sdk::Note("i2c-end"); }
extern "C" esp_err_t ulp_set_wakeup_period(size_t, uint32_t period) { sleep_sdk::state.ulpPeriod = period; return ESP_OK; }
extern "C" esp_err_t ulp_run(uint32_t entry) {
    sleep_sdk::state.ulpEntry = entry;
    sleep_sdk::state.ulpElapsedMs = 0U;
    sleep_sdk::state.ulpRunning = true;
    return ESP_OK;
}
extern "C" void ulp_timer_stop() { sleep_sdk::state.ulpRunning = false; }

int main() {
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::SetMinLevel(fxe::Level::ERROR);
    sleep_sdk::SuccessfulAppSleep(false);
    sleep_sdk::SuccessfulAppSleep(true);
    sleep_sdk::CompareAppAndIdleEntry();
    sleep_sdk::FailedWakeSetup();
    sleep_sdk::P1PulseDuringTeardownThenFreshWake();
    sleep_sdk::TeardownAbortAndBlockedStop();
    return sleep_sdk::failures == 0U ? 0 : 1;
}
