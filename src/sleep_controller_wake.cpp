#include "firmware/sleep_controller.hpp"

#ifdef ARDUINO

#include <Arduino.h>
#include <esp_err.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <driver/gpio.h>
#include <soc/soc_caps.h>

#if SOC_PM_SUPPORT_EXT_WAKEUP
#include <driver/rtc_io.h>
#endif

#include "logger.hpp"

namespace firmware {

namespace {

#if SOC_PM_SUPPORT_EXT_WAKEUP
bool ConfigureRtcPullupsForWakePins(const SleepWakePinSet& pins) {
    bool configuredAny = false;
    for (uint8_t index = 0U; index < pins.count; ++index) {
        const uint8_t pin = pins.pins[index];
        const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
        if (!rtc_gpio_is_valid_gpio(gpio)) {
            logger::Warn("sleep: gpio %u is not an RTC IO", static_cast<unsigned>(pin));
            return false;
        }

        const bool configured = (rtc_gpio_init(gpio) == ESP_OK) &&
                                (rtc_gpio_pullup_en(gpio) == ESP_OK) &&
                                (rtc_gpio_pulldown_dis(gpio) == ESP_OK) &&
                                (rtc_gpio_hold_dis(gpio) == ESP_OK) &&
                                (rtc_gpio_set_direction(gpio, RTC_GPIO_MODE_INPUT_ONLY) == ESP_OK);
        if (!configured) {
            logger::Warn("sleep: RTC wake pin setup failed for gpio %u", static_cast<unsigned>(pin));
            return false;
        }
        configuredAny = true;
    }
    return configuredAny;
}
#endif

bool RestoreWakePinsForDigitalRuntime(const SleepWakePinSet& pins) {
    bool restored = true;
    for (uint8_t index = 0U; index < pins.count; ++index) {
        const uint8_t pin = pins.pins[index];
#if SOC_PM_SUPPORT_EXT_WAKEUP
        // EXT1 wake leaves its pads routed through RTC IO; gpio_config/pinMode
        // alone does not return them to the digital GPIO matrix.
        const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
        if (rtc_gpio_is_valid_gpio(gpio)) {
            (void)rtc_gpio_hold_dis(gpio);
            const esp_err_t err = rtc_gpio_deinit(gpio);
            if (err != ESP_OK) {
                logger::Error("sleep: RTC wake pin restore failed for gpio %u (%d)",
                              static_cast<unsigned>(pin),
                              static_cast<int>(err));
                restored = false;
            }
        }
#endif
        pinMode(pin, INPUT_PULLUP);
    }
    return restored;
}

} // namespace

bool SleepController::ConfigureWakeOnButtons() {
    const uint64_t mask = m_activeWakePins.Mask();

    if (mask == 0) {
        logger::Warn("sleep: no button wake pins configured");
        return false;
    }

#if defined(LITCH_ESP32S3) && SOC_PM_SUPPORT_EXT_WAKEUP
    (void)mask;

    if (!ConfigureRtcPullupsForWakePins(m_activeWakePins)) {
        return false;
    }
    const esp_err_t powerDomainErr = esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    if (powerDomainErr != ESP_OK) {
        logger::Warn("sleep: RTC peripheral power config failed (%d)", static_cast<int>(powerDomainErr));
        return false;
    }

    uint64_t rtcMask = 0;
    for (uint8_t index = 0U; index < m_activeWakePins.count; ++index) {
        const uint8_t pin = m_activeWakePins.pins[index];
        const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
        if (!rtc_gpio_is_valid_gpio(gpio)) {
            logger::Warn("sleep: gpio %u is not an RTC IO; skipping for ext wake", static_cast<unsigned>(pin));
            continue;
        }
        rtcMask |= (1ULL << pin);
    }

    if (rtcMask != mask) {
        logger::Warn("sleep: ext wake does not cover every configured button");
        return false;
    }

    const esp_err_t err = esp_sleep_enable_ext1_wakeup(rtcMask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        logger::Warn("sleep: ext wake config failed (%d)", static_cast<int>(err));
        return false;
    }
    logger::Trace("sleep: ext wake configured (mask=0x%llx)", static_cast<unsigned long long>(rtcMask));
    return true;
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    uint64_t validMask = 0;
    for (uint8_t index = 0U; index < m_activeWakePins.count; ++index) {
        const uint8_t pin = m_activeWakePins.pins[index];
        if (!esp_sleep_is_valid_wakeup_gpio(static_cast<gpio_num_t>(pin))) {
            logger::Warn("sleep: gpio %u is not deep-sleep wake-capable; skipping", static_cast<unsigned>(pin));
            continue;
        }
        validMask |= (1ULL << pin);
    }

    if (validMask != mask) {
        logger::Warn("sleep: gpio wake does not cover every configured button");
        return false;
    }

    const esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(validMask, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        logger::Warn("sleep: gpio wake config failed (%d)", static_cast<int>(err));
        return false;
    }
    logger::Trace("sleep: gpio wake configured (mask=0x%llx)", static_cast<unsigned long long>(validMask));
    return true;
#elif SOC_PM_SUPPORT_EXT_WAKEUP
    uint64_t rtcMask = 0;
    for (uint8_t index = 0U; index < m_activeWakePins.count; ++index) {
        const uint8_t pin = m_activeWakePins.pins[index];
        if (!rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(pin))) {
            logger::Warn("sleep: gpio %u is not an RTC IO; skipping for ext wake", static_cast<unsigned>(pin));
            continue;
        }
        rtcMask |= (1ULL << pin);
    }

    if (rtcMask != mask) {
        logger::Warn("sleep: ext wake does not cover every configured button");
        return false;
    }

    const esp_err_t err = esp_sleep_enable_ext1_wakeup(rtcMask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        logger::Warn("sleep: ext wake config failed (%d)", static_cast<int>(err));
        return false;
    }
    logger::Trace("sleep: ext wake configured (mask=0x%llx)", static_cast<unsigned long long>(rtcMask));
    return true;
#else
    (void)mask;
    logger::Warn("sleep: wake config unsupported on this target");
    return false;
#endif
}

void SleepController::RestoreWakeButtonsForRuntimeOrRestart() {
    if (RestoreWakePinsForDigitalRuntime(m_restoreWakePins)) {
        return;
    }

    logger::Error("sleep: wake pins unavailable after wake; restarting");
    using HardwareTransition = void (*)();
    HardwareTransition volatile restart = &esp_restart;
    restart();
    while (true) {
        delay(1000);
    }
}

void SleepController::RestoreWakeButtonsAfterFailedPreparation() {
    bool restored = true;
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    for (uint8_t index = 0U; index < m_activeWakePins.count; ++index) {
        const uint8_t pin = m_activeWakePins.pins[index];
        const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
        const esp_err_t gpioWakeErr = gpio_wakeup_disable(gpio);
        restored = (gpioWakeErr == ESP_OK) && restored;
    }

    restored = RestoreWakePinsForDigitalRuntime(m_restoreWakePins) && restored;

#if SOC_PM_SUPPORT_EXT_WAKEUP
    restored = (esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,
                                    ESP_PD_OPTION_AUTO) == ESP_OK) &&
               restored;
#endif

    if (!restored) {
        logger::Error("sleep: failed wake-pin rollback; restarting");
        using HardwareTransition = void (*)();
        HardwareTransition volatile restart = &esp_restart;
        restart();
        while (true) {
            delay(1000);
        }
    }
}

} // namespace firmware

#else

namespace firmware {

bool SleepController::ConfigureWakeOnButtons() {
    return false;
}

void SleepController::RestoreWakeButtonsForRuntimeOrRestart() {}

void SleepController::RestoreWakeButtonsAfterFailedPreparation() {}

} // namespace firmware

#endif
