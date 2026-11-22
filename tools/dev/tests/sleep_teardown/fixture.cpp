#include <algorithm>
#include <type_traits>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <esp_rom_sys.h>
#include "sdk_boundary.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/logger.hpp"
#include "firmware/p1x_deep_wake.hpp"
#include "firmware/sleep_transition.hpp"
#include "firmware/sleep_wake_pin_policy.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "firmware/sleep_controller.hpp"
#include "modules/control_mutation_service.hpp"
#include "runtime/runtime_health.hpp"
#include <esp32s3/ulp.h>
#undef RTC_SLOW_MEM
#define RTC_SLOW_MEM sleep_sdk::state.slowMemory.data()
#define ARDUINO 1
#define ARDUINO_USB_MODE 0
#define LITCH_ESP32S3 1
#define LITCH_HW_VARIANT_S3 1
#include LITCH_SOURCE_SLEEP_TRANSITION
#include LITCH_SOURCE_SLEEP_CONTROLLER
#include LITCH_SOURCE_P1X_DEEP_WAKE
#include LITCH_SOURCE_SLEEP_CONTROLLER_WAKE
#include LITCH_SOURCE_SLEEP_CONTROLLER_HARDWARE
#include LITCH_SOURCE_SLEEP_CONTROLLER_ENTER_SLEEP
#include LITCH_SOURCE_SLEEP_ENTRY_ORCHESTRATION

namespace sleep_sdk {
unsigned failures=0U;
void Check(bool okay,const char* message){if(!okay){++failures;std::printf("FAIL %s\n",message);}}
template<typename Result> Result CompletedStop(void*) {
    Note("ble-stop");
    if constexpr (!std::is_void_v<Result>) return true;
}
struct Journey {
    firmware::SleepController controller;
    fxe::SettingsStore settings;
    Journey(){state={};fxe::ManualMillis::SetNowMs(0U);controller.ConfigureHardwareProfile(hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3));controller.Begin();
        controller.SetPreSleepCallback(&CompletedStop<std::invoke_result_t<firmware::SleepController::PreSleepCallback, void*>>,this);
        controller.ConfigureOptionalActiveLowWakeSource(21U,[](void*){Note("motion-prepare");return true;},[](void*){return true;},this);}
    firmware::SleepPreparationHooks Preparation(){return{this,[](void*){Note("save-begin");return true;},[](void*){return firmware::SleepPersistenceStatus::Succeeded;},[](void* p){return static_cast<Journey*>(p)->controller.CanBeginSleepTeardown();},[](void* p){return static_cast<Journey*>(p)->controller.PrepareWakeSourceForSleep();}};}
    firmware::SleepEntryHooks Entry(){return{this,[](void*){Note("black");return true;},[](void*,uint32_t ms){delay(ms);},[](void*){Note("ambient-stop");},[](void*){state.railOff=true;},[](void* p){return static_cast<Journey*>(p)->controller.EnterSleepNow();},[](void*,bool){esp_restart();}};}
    void Step(){(void)firmware::RunSleepLifecycleStep(controller,Preparation(),Entry());}
};
}
void delay(unsigned long ms) { fxe::ManualMillis::AdvanceMs(static_cast<uint32_t>(ms)); }
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
    sleep_sdk::state.timerInterval = 0U;
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
esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t domain, esp_sleep_pd_option_t option) {
    if (domain == ESP_PD_DOMAIN_RTC_SLOW_MEM) ++sleep_sdk::state.slowPowerConfigCalls;
    sleep_sdk::state.rtcPowerOn = option == ESP_PD_OPTION_ON;
    return ESP_OK;
}
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t interval) {
    sleep_sdk::Note("timer-request");
    if (sleep_sdk::state.failTimer) return ESP_FAIL;
    sleep_sdk::state.timerInterval = interval;
    return ESP_OK;
}
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return sleep_sdk::state.wakeCause; }
esp_reset_reason_t esp_reset_reason() { return sleep_sdk::state.resetReason; }
extern "C" soc_reset_reason_t esp_rom_get_reset_reason(int cpu) {
    sleep_sdk::Check(cpu == 0, "status reads the specified CPU0 ROM reset cause");
    return static_cast<soc_reset_reason_t>(sleep_sdk::state.romResetReason);
}
uint64_t esp_sleep_get_ext1_wakeup_status() {
    return sleep_sdk::state.wakeCause == ESP_SLEEP_WAKEUP_EXT1
        ? sleep_sdk::state.appliedExt1 : 0U;
}
void esp_deep_sleep_start() {
    using namespace sleep_sdk;
    Check(state.requestedExt1 == (0x6f6e0ULL | (1ULL << 21U)) && state.ulpRequested && state.ulpRunning,
          "all origins preserve fitted EXT1 buttons, motion and GPIO0 ULP at the real SDK handoff");
    for (unsigned pin = 0U; pin < 22U; ++pin) {
        if ((state.requestedExt1 & (1ULL << pin)) || pin == 0U)
            Check(state.pins[pin].rtc && state.pins[pin].pullup && !state.pins[pin].pulldown &&
                  !state.pins[pin].held && state.pins[pin].inputOnly,
                  "handoff preserves every configured RTC wake input");
    }
    state.deepEntered=true;state.appliedExt1=state.requestedExt1;Note("deep-entry");
    throw EnteredSleep{};
}
esp_err_t esp_light_sleep_start() { throw std::runtime_error("unexpected light sleep"); }
void esp_restart() { sleep_sdk::Note("restart"); throw sleep_sdk::Restarted{}; }
bool tud_disconnect() { sleep_sdk::state.usbDetached=true;sleep_sdk::Note("usb-detach");return true; }
void SerialBoundary::flush() {}
void SerialBoundary::end() { sleep_sdk::Note("serial-end"); }
void WireBoundary::end() { sleep_sdk::Note("i2c-end"); }
extern "C" esp_err_t ulp_set_wakeup_period(size_t, uint32_t period) { sleep_sdk::state.ulpPeriod = period; return ESP_OK; }
extern "C" esp_err_t ulp_run(uint32_t entry) { sleep_sdk::state.ulpEntry = entry; sleep_sdk::state.ulpRunning = true; return ESP_OK; }
extern "C" void ulp_timer_stop() { sleep_sdk::state.ulpRunning = false; }


// The native-stop leaf is controlled. Actual Stop and firmware callback bodies
// are included below; the aggregate exercises real ShutdownStack failure/pending.
namespace modules {
using StopResult = std::invoke_result_t<firmware::SleepController::PreSleepCallback, void*>;
class BleService {
 public:
    bool refuse = false;
    bool nativeRunning = true;
    bool m_enabled = true;
    template<typename Result> Result CompleteNativeStop() {
        nativeRunning = refuse;
        if constexpr (!std::is_void_v<Result>) return !refuse;
    }
    StopResult ShutdownStack() { return CompleteNativeStop<StopResult>(); }
    StopResult Stop();
};
#define LITCH_BLE 1
#include "ble_stop.inc"
}
#include "sleep_adapter.inc"

// Root wiring is controlled; request admission and Loop's suppression/update
// block are extracted unchanged from their actual firmware owners.
namespace firmware_internal {
struct FirmwareSystem {
    firmware::SleepController sleep;
    fxe::SettingsStore settings;
    runtime::RuntimeHealthReducer runtimeHealth;
    struct Active { bool active = false; bool IsActive() const { return active; } };
    Active hardwareTest, amptest, amptestVerify, comptest;
    struct Ble {
        bool ota = false, accepted = false;
        bool IsOtaInProgress() const { return ota; }
        bool IsOtaRuntimeAccepted() const { return accepted; }
    } ble;
    struct BootDisplay {
        bool cancelled = false;
        void Cancel() { cancelled = true; }
    } bootDisplay;
    bool safeModeActive = false, rebootPersistencePending = false;
    bool otaBootAwaitingHealth = false;
};
// Presentation cancellation is a controlled boundary in this Sleep teardown fixture.
void CancelBootPresentation(FirmwareSystem& sys) { sys.bootDisplay.Cancel(); }
#include "sleep_request.inc"
void UpdateSleep(FirmwareSystem& sys, const bool otaHealthCommitFinished) {
    const bool otaInProgress = sys.ble.IsOtaInProgress();
#include "sleep_suppression.inc"
}
} // namespace firmware_internal

int main() {
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::SetMinLevel(fxe::Level::ERROR);
    for (const bool pendingImage : {false, true}) {
        firmware_internal::FirmwareSystem sys;
        sys.otaBootAwaitingHealth = pendingImage;
        fxe::ManualMillis::SetNowMs(100U);
        sleep_sdk::Check(firmware_internal::RequestControlSleep(&sys) ==
                            modules::ControlCoordinatorResult::Applied,
                        "actual Sleep adapter retains a request during startup confirmation");
        sleep_sdk::Check(sys.bootDisplay.cancelled,
                        "actual Sleep request cancels the optional boot presentation");
        firmware_internal::UpdateSleep(sys, false);
        const bool retained = sys.sleep.GetState() == firmware::SleepState::Fading;
        sleep_sdk::Check(retained, "actual Loop block preserves the early directed request");
        fxe::ManualMillis::SetNowMs(350U);
        firmware_internal::UpdateSleep(sys, false);
        const uint16_t fade = sys.sleep.GetFadeScale256();
        sleep_sdk::Check(sys.sleep.GetState() == firmware::SleepState::Fading && fade == 128U,
                        "actual Loop block preserves the half-fade while startup is pending");
        fxe::ManualMillis::SetNowMs(600U);
        firmware_internal::UpdateSleep(sys, false);
        sleep_sdk::Check(sys.sleep.GetState() == firmware::SleepState::Preparing,
                        "early request reaches cooperative preparation without a second request");
        sys.ble.ota = true;
        firmware_internal::UpdateSleep(sys, false);
        sleep_sdk::Check(sys.sleep.GetState() == firmware::SleepState::Awake,
                        "actual Loop still cancels for an active OTA transfer");
        std::printf("EARLY_SLEEP_ADAPTER pending_image=%u retained=%u fade=%u safety_cancelled=%u\n",
                    pendingImage, retained, fade, sys.sleep.GetState() == firmware::SleepState::Awake);
    }
    for (const bool refuse : {false, true}) {
        for (const bool idle : {false, true}) {
            sleep_sdk::Journey journey;
            modules::BleService ble;
            ble.refuse = refuse;
            journey.controller.SetPreSleepCallback(&StopBleBeforeSleep, &ble);
            journey.controller.RequestSleep(idle ? firmware::SleepRequestOrigin::Idle
                                                 : firmware::SleepRequestOrigin::App, 0U, idle);
            fxe::ManualMillis::SetNowMs(3500U);
            journey.controller.Update(journey.settings, 3500U, firmware::SleepSuppression::Allow);
            journey.Step();
            bool restarted = false;
            try { journey.Step(); }
            catch (const sleep_sdk::EnteredSleep&) {}
            catch (const sleep_sdk::Restarted&) { restarted = true; }
            sleep_sdk::Check(!ble.m_enabled && ble.nativeRunning == refuse,
                             "controlled native shutdown retains the failure truth");
            sleep_sdk::Check(sleep_sdk::state.deepEntered == !refuse &&
                             sleep_sdk::state.usbDetached == !refuse && restarted == refuse,
                             "incomplete BLE teardown recovers without USB detach or SDK sleep");
            std::printf("SLEEP_TEARDOWN idle=%u native_refusal=%u native_running=%u usb_detached=%u sdk_entry=%u restart=%u\n",
                         idle, refuse, ble.nativeRunning, sleep_sdk::state.usbDetached,
                         sleep_sdk::state.deepEntered, restarted);
        }
    }
    return sleep_sdk::failures == 0U ? 0 : 1;
}
