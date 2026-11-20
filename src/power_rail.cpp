#include "firmware/power_rail.hpp"

#ifdef ARDUINO

#include <Arduino.h>

#include <array>

#if defined(ARDUINO_ARCH_ESP32)
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <rom/gpio.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_struct.h>
#endif

#include "hardware/hardware.hpp"
#include "firmware/usb_boot_probe.hpp"
#include "logger.hpp"
#include "foxie_embedded/elapsed_time.hpp"

namespace firmware {

namespace {

// Only V3.3 has been physically exercised with the hardware PWM profile.
constexpr uint32_t FIVE_VOLT_PRE_ENABLE_SETTLE_MS = 5U;
constexpr uint32_t PWM_POST_ENABLE_DELAY_MS = 5U;
constexpr uint32_t PRECHARGE_PWM_HZ = 4882U;
constexpr uint32_t PRECHARGE_STEP_US = 1250U;
constexpr std::array<uint32_t, 4> PRECHARGE_DUTIES{1U, 2U, 4U, 8U};
constexpr uint32_t PRECHARGE_NOMINAL_US =
    PRECHARGE_STEP_US * PRECHARGE_DUTIES.size();
constexpr uint32_t LEGACY_PRECHARGE_PULSES = 10000U;
constexpr uint32_t LEGACY_PRECHARGE_NOMINAL_US = 75000U;
constexpr uint32_t LEGACY_PRECHARGE_ON_US = 0U;
constexpr uint32_t LEGACY_PRECHARGE_OFF_US =
    LEGACY_PRECHARGE_NOMINAL_US / LEGACY_PRECHARGE_PULSES - LEGACY_PRECHARGE_ON_US;
constexpr uint32_t LEGACY_POST_ENABLE_DELAY_MS = 10U;

void HoldLedDataPinsLowForEnable(const uint8_t boardLedPin,
                                 const uint8_t statusLedPin) {
    // Prevent random LED latch/current surge when 5V comes up.
    digitalWrite(boardLedPin, LOW);
    pinMode(boardLedPin, OUTPUT);
    digitalWrite(statusLedPin, LOW);
    pinMode(statusLedPin, OUTPUT);
    delay(1);
}

#if defined(ARDUINO_ARCH_ESP32)
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define LITCH_GPIO_OUT_W1TS_WRITE(mask) GPIO.out_w1ts.val = (mask)
#define LITCH_GPIO_OUT_W1TC_WRITE(mask) GPIO.out_w1tc.val = (mask)
#else
#define LITCH_GPIO_OUT_W1TS_WRITE(mask) GPIO.out_w1ts = (mask)
#define LITCH_GPIO_OUT_W1TC_WRITE(mask) GPIO.out_w1tc = (mask)
#endif
#endif

inline void FastGpioHighEsp32(const uint8_t pin) {
#if defined(ARDUINO_ARCH_ESP32)
    // Fast path for ESP32-family: direct register set.
    if (pin < 32U) {
        LITCH_GPIO_OUT_W1TS_WRITE(1UL << pin);
        return;
    }

    // Best-effort fallback for high GPIO indices.
    gpio_set_level(static_cast<gpio_num_t>(pin), 1);
#else
    digitalWrite(pin, HIGH);
#endif
}

inline void FastGpioLowEsp32(const uint8_t pin) {
#if defined(ARDUINO_ARCH_ESP32)
    if (pin < 32U) {
        LITCH_GPIO_OUT_W1TC_WRITE(1UL << pin);
        return;
    }

    gpio_set_level(static_cast<gpio_num_t>(pin), 0);
#else
    digitalWrite(pin, LOW);
#endif
}

void HoldLedDataPinsLowForDisable(const uint8_t boardLedPin,
                                  const uint8_t statusLedPin) {
    // Latch low before disconnecting RMT, including an unconfirmed transfer.
    // No subsequent Show can reopen the route while the rail is disabled.
    const uint8_t pins[] = {boardLedPin, statusLedPin};
    for (const uint8_t pin : pins) {
        digitalWrite(pin, LOW);
#if defined(ARDUINO_ARCH_ESP32)
        gpio_matrix_out(pin, SIG_GPIO_OUT_IDX, false, false);
#endif
        pinMode(pin, OUTPUT);
    }
}

void Drive5vEnLow(const uint8_t fiveVoltEnablePin) {
    // Prepare the LOW latch before enabling output, including after reset.
    FastGpioLowEsp32(fiveVoltEnablePin);
#if defined(ARDUINO_ARCH_ESP32)
    gpio_matrix_out(fiveVoltEnablePin, SIG_GPIO_OUT_IDX, false, false);
#endif
    pinMode(fiveVoltEnablePin, OUTPUT);
    delay(1);
}

void RunLegacyPrecharge(const uint8_t fiveVoltEnablePin) {
    // Preserve V2/V3's original instruction-length HIGH taps and 7 us LOW.
    for (uint32_t i = 0; i < LEGACY_PRECHARGE_PULSES; ++i) {
        FastGpioHighEsp32(fiveVoltEnablePin);
        if (LEGACY_PRECHARGE_ON_US > 0U) {
            delayMicroseconds(static_cast<unsigned int>(LEGACY_PRECHARGE_ON_US));
        }
        FastGpioLowEsp32(fiveVoltEnablePin);
        if (LEGACY_PRECHARGE_OFF_US > 0U) {
            delayMicroseconds(static_cast<unsigned int>(LEGACY_PRECHARGE_OFF_US));
        }
    }
}

#if defined(ARDUINO_ARCH_ESP32)
constexpr ledc_mode_t PRECHARGE_MODE = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t PRECHARGE_TIMER = LEDC_TIMER_3;
constexpr ledc_channel_t PRECHARGE_CHANNEL = static_cast<ledc_channel_t>(7U);

void StopPrechargePwmLow() {
    (void)ledc_stop(PRECHARGE_MODE, PRECHARGE_CHANNEL, 0U);
    (void)ledc_timer_pause(PRECHARGE_MODE, PRECHARGE_TIMER);
}

int PreparePrechargePwm() {
    // No fade service/task/ISR is installed. The pinned SDK retains one bounded
    // 8-byte LEDC object until reset; this owner reserves timer3/channel7.
    ledc_timer_config_t timer{};
    timer.speed_mode = PRECHARGE_MODE;
    timer.duty_resolution = LEDC_TIMER_14_BIT;
    timer.timer_num = PRECHARGE_TIMER;
    timer.freq_hz = PRECHARGE_PWM_HZ;
    timer.clk_cfg = LEDC_USE_APB_CLK;
    esp_err_t result = ledc_timer_config(&timer);
    if (result != ESP_OK) return result;
    result = ledc_stop(PRECHARGE_MODE, PRECHARGE_CHANNEL, 0U);
    if (result == ESP_OK) result = ledc_set_duty(PRECHARGE_MODE, PRECHARGE_CHANNEL, 0U);
    if (result == ESP_OK) result = ledc_update_duty(PRECHARGE_MODE, PRECHARGE_CHANNEL);
    if (result == ESP_OK) result = ledc_timer_resume(PRECHARGE_MODE, PRECHARGE_TIMER);
    if (result != ESP_OK) StopPrechargePwmLow();
    return result;
}

int RunPrechargePwm(const uint8_t pin) {
    // The zero duty has settled while the pin remained routed to GPIO LOW.
    if (ledc_get_freq(PRECHARGE_MODE, PRECHARGE_TIMER) != PRECHARGE_PWM_HZ ||
        ledc_get_duty(PRECHARGE_MODE, PRECHARGE_CHANNEL) != 0U) {
        StopPrechargePwmLow();
        return ESP_ERR_INVALID_STATE;
    }
    ledc_channel_config_t channel{};
    channel.gpio_num = pin;
    channel.speed_mode = PRECHARGE_MODE;
    channel.channel = PRECHARGE_CHANNEL;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.timer_sel = PRECHARGE_TIMER;
    channel.duty = 0U;
    channel.hpoint = 0;
    esp_err_t result = ledc_channel_config(&channel);
    for (const auto duty : PRECHARGE_DUTIES) {
        if (result == ESP_OK) result = ledc_set_duty(PRECHARGE_MODE, PRECHARGE_CHANNEL, duty);
        if (result == ESP_OK) result = ledc_update_duty(PRECHARGE_MODE, PRECHARGE_CHANNEL);
        if (result != ESP_OK) break;
        delayMicroseconds(PRECHARGE_STEP_US);
    }
    if (result == ESP_OK) result = ledc_stop(PRECHARGE_MODE, PRECHARGE_CHANNEL, 1U);
    if (result == ESP_OK) result = ledc_timer_pause(PRECHARGE_MODE, PRECHARGE_TIMER);
    if (result != ESP_OK) StopPrechargePwmLow();
    return result;
}
#endif

void Log5vEnabled(const bool usesPwm, const uint32_t elapsedMs) {
    if (!usesPwm) {
        logger::Trace(
            "power: 5V rail enabled (precharge=%luus/%lup on=%luus post=%ums elapsed_ms=%lu)",
            static_cast<unsigned long>(LEGACY_PRECHARGE_NOMINAL_US),
            static_cast<unsigned long>(LEGACY_PRECHARGE_PULSES),
            static_cast<unsigned long>(LEGACY_PRECHARGE_ON_US),
            static_cast<unsigned int>(LEGACY_POST_ENABLE_DELAY_MS),
            static_cast<unsigned long>(elapsedMs));
        return;
    }
    logger::Trace(
        "power: 5V rail enabled (pre_enable_ms=%lu pwm_hz=%lu pwm_bits=14 steps=4 step_us=%lu post_ms=%u elapsed_ms=%lu)",
        static_cast<unsigned long>(FIVE_VOLT_PRE_ENABLE_SETTLE_MS),
        static_cast<unsigned long>(PRECHARGE_PWM_HZ),
        static_cast<unsigned long>(PRECHARGE_STEP_US),
        static_cast<unsigned int>(PWM_POST_ENABLE_DELAY_MS),
        static_cast<unsigned long>(elapsedMs));
}

bool Enable5vRail(const uint8_t boardLedPin,
                  const uint8_t statusLedPin,
                  const uint8_t fiveVoltEnablePin,
                  const bool usesPwm) {
    HoldLedDataPinsLowForEnable(boardLedPin, statusLedPin);
    Drive5vEnLow(fiveVoltEnablePin);
    if (usesPwm) {
        int result = PreparePrechargePwm();
        if (result == 0) {
            delay(FIVE_VOLT_PRE_ENABLE_SETTLE_MS);
            result = RunPrechargePwm(fiveVoltEnablePin);
        }
        if (result != 0) {
            Drive5vEnLow(fiveVoltEnablePin);
            logger::Error("power: PWM precharge failed (%d); 5V rail held off", result);
            return false;
        }
    } else {
        RunLegacyPrecharge(fiveVoltEnablePin);
    }

    // PWM is stopped at HIGH; the legacy train is already GPIO-owned.
    // Preload HIGH before routing to GPIO, preserving the PWM level at handoff.
    FastGpioHighEsp32(fiveVoltEnablePin);
#if defined(ARDUINO_ARCH_ESP32)
    gpio_matrix_out(fiveVoltEnablePin, SIG_GPIO_OUT_IDX, false, false);
#endif
    delay(usesPwm ? PWM_POST_ENABLE_DELAY_MS : LEGACY_POST_ENABLE_DELAY_MS);
    return true;
}

} // namespace

void PowerRail::Begin() {
    if (!m_enabled || m_enableRequested.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    m_fiveVoltOn.store(false, std::memory_order_release);
    const auto elapsed = fxe::ElapsedTime::StartNow();
    if (!Enable5vRail(m_boardLedPin, m_statusLedPin, m_fiveVoltEnablePin,
                     m_usesPwmFiveVoltPrecharge)) {
        m_enableRequested.store(false, std::memory_order_release);
        return;
    }
    m_fiveVoltOn.store(true, std::memory_order_release);
#if defined(LITCH_HW_VARIANT_S3)
    RecordUsbBootPrecharge(
        m_usesPwmFiveVoltPrecharge ? PRECHARGE_NOMINAL_US : LEGACY_PRECHARGE_NOMINAL_US,
        m_usesPwmFiveVoltPrecharge ? 0U : LEGACY_PRECHARGE_PULSES);
#endif
    Log5vEnabled(m_usesPwmFiveVoltPrecharge, elapsed.ElapsedMs());
}

void PowerRail::Enable5v() {
    if (!m_enabled || m_enableRequested.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    m_fiveVoltOn.store(false, std::memory_order_release);
    const auto elapsed = fxe::ElapsedTime::StartNow();
    if (!Enable5vRail(m_boardLedPin, m_statusLedPin, m_fiveVoltEnablePin,
                     m_usesPwmFiveVoltPrecharge)) {
        m_enableRequested.store(false, std::memory_order_release);
        return;
    }
    m_fiveVoltOn.store(true, std::memory_order_release);
    Log5vEnabled(m_usesPwmFiveVoltPrecharge, elapsed.ElapsedMs());
}

void PowerRail::Disable5v() {
    if (!m_enabled || !m_enableRequested.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    m_fiveVoltOn.store(false, std::memory_order_release);
    HoldLedDataPinsLowForDisable(m_boardLedPin, m_statusLedPin);
    Drive5vEnLow(m_fiveVoltEnablePin);

    logger::Trace("power: 5V rail disabled");
}

void PowerRail::PrepareForSleep() {
    if (!m_enabled) {
        return;
    }

    m_enableRequested.store(false, std::memory_order_release);
    m_fiveVoltOn.store(false, std::memory_order_release);
    HoldLedDataPinsLowForDisable(m_boardLedPin, m_statusLedPin);
    Drive5vEnLow(m_fiveVoltEnablePin);

    logger::Trace("power: 5V rail disabled");
}

} // namespace firmware

#else

namespace firmware {

void PowerRail::Begin() {}

void PowerRail::PrepareForSleep() {}

void PowerRail::Enable5v() {}

void PowerRail::Disable5v() {}

} // namespace firmware

#endif
