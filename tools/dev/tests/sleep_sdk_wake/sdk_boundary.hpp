#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 0x103,
                    ESP_ERR_INVALID_ARG = 0x102;
constexpr unsigned LOW = 0U, HIGH = 1U, INPUT = 1U, INPUT_PULLUP = 2U,
                   OUTPUT = 3U, OUTPUT_OPEN_DRAIN = 4U;
constexpr unsigned SDA = 3U, SCL = 4U;
enum esp_reset_reason_t { ESP_RST_POWERON, ESP_RST_DEEPSLEEP };
#define CONFIG_IDF_TARGET_ESP32S3 1
#define SOC_PM_SUPPORT_EXT_WAKEUP 1
#define SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP 0
#define SOC_USB_SERIAL_JTAG_SUPPORTED 0
#define SOC_ULP_SUPPORTED 1
#define SOC_TOUCH_SENSOR_NUM 0
#define SOC_GPIO_PIN_COUNT 49
#define RTC_DATA_ATTR
#define ESP_STATIC_ASSERT static_assert
#include <soc/soc.h>
#include <hal/gpio_types.h>
#include <hal/rtc_io_types.h>
#include <esp_sleep.h>
#include <soc/rtc_io_reg.h>
#include <soc/rtc_cntl_reg.h>

namespace sleep_sdk {
struct EnteredSleep {};
struct Restarted {};
struct BlockedBleStop {};
struct Pin {
    bool high = true, rtc = false, pullup = false;
    bool pulldown = true, held = true, inputOnly = false;
};
struct State {
    std::array<Pin, 49U> pins{};
    std::array<uint32_t, 2048U> slowMemory{};
    std::vector<std::string> calls;
    uint64_t requestedExt1 = 0U, appliedExt1 = 0U;
    uint32_t ulpEntry = 0U, ulpPeriod = 0U;
    uint32_t ulpElapsedMs = 0U, acceptedUlpWakes = 0U, ignoredUlpWakes = 0U;
    bool ulpRequested = false, ulpRunning = false, rtcPowerOn = false;
    bool pulseP1DuringTeardown = false, holdP1ThroughEntry = false, readyForUlpWake = false;
    bool deepEntered = false, usbDetached = false, railOff = false;
    bool failExt1 = false, failUlp = false, failRtc = false;
    bool blockBleStop = false, assertDuringBleStop = false;
    esp_sleep_wakeup_cause_t wakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
    esp_reset_reason_t resetReason = ESP_RST_POWERON;
};
inline State state;
inline void Note(const std::string& call) { state.calls.push_back(call); }
}

void delay(unsigned long milliseconds);
void delayMicroseconds(unsigned int microseconds);
void pinMode(unsigned pin, unsigned mode);
int digitalRead(unsigned pin);
void digitalWrite(unsigned pin, unsigned level);
esp_err_t gpio_pullup_dis(gpio_num_t pin);
esp_err_t gpio_pulldown_dis(gpio_num_t pin);
esp_err_t gpio_hold_dis(gpio_num_t pin);
esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t mode);
esp_err_t gpio_wakeup_disable(gpio_num_t pin);
bool rtc_gpio_is_valid_gpio(gpio_num_t pin);
esp_err_t rtc_gpio_init(gpio_num_t pin);
esp_err_t rtc_gpio_deinit(gpio_num_t pin);
esp_err_t rtc_gpio_pullup_en(gpio_num_t pin);
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin);
esp_err_t rtc_gpio_hold_dis(gpio_num_t pin);
esp_err_t rtc_gpio_set_direction(gpio_num_t pin, rtc_gpio_mode_t mode);
esp_reset_reason_t esp_reset_reason();
void esp_restart();
bool tud_disconnect();
struct SerialBoundary { void flush(); void end(); };
inline SerialBoundary Serial;
struct WireBoundary { void end(); };
inline WireBoundary Wire;
