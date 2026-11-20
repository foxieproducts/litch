#include "firmware/sleep_controller.hpp"
#include "modules/support_runtime_stats.hpp"

#ifdef ARDUINO

#include <Arduino.h>

#include "firmware/sleep_controller_hardware_internal.hpp"
#include "firmware/p1x_deep_wake.hpp"
#include "firmware/sleep_power_policy.hpp"
#include "logger.hpp"

#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <driver/gpio.h>
#include <soc/soc_caps.h>

#if SOC_PM_SUPPORT_EXT_WAKEUP
#include <driver/rtc_io.h>
#endif

namespace firmware {

namespace {

bool IsButtonWakeCause(const esp_sleep_wakeup_cause_t cause) {
    return (cause == ESP_SLEEP_WAKEUP_GPIO) || (cause == ESP_SLEEP_WAKEUP_EXT0) ||
           (cause == ESP_SLEEP_WAKEUP_EXT1) || (cause == ESP_SLEEP_WAKEUP_ULP);
}

RTC_DATA_ATTR uint32_t g_lightSleepRestartCookie = 0;
static constexpr uint32_t LIGHT_SLEEP_RESTART_COOKIE_VALUE = 0x4C495443U;

void ConfigureWakeInputs(const SleepWakePinSet& pins) {
    for (uint8_t index = 0U; index < pins.count; ++index) {
        pinMode(pins.pins[index], INPUT_PULLUP);
    }
}

void LogWakeSourceLevels(const SleepWakePinSet& pins) {
    for (uint8_t index = 0U; index < pins.count; ++index) {
        const uint8_t pin = pins.pins[index];
        logger::Trace("sleep: wake pin gpio%u level=%u",
                     static_cast<unsigned>(pin),
                     static_cast<unsigned>(digitalRead(pin) == LOW ? 0U : 1U));
    }
}

bool CanUseDeepSleepWake(const SleepWakePinSet& pins) {
#if defined(LITCH_ESP32S3) && SOC_PM_SUPPORT_EXT_WAKEUP
    unsigned rtcCount = 0;
    for (uint8_t index = 0U; index < pins.count; ++index) {
        const uint8_t pin = pins.pins[index];
        if (rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(pin))) {
            ++rtcCount;
        }
    }

    const bool possible = pins.count > 0U && rtcCount == pins.count;
    if (!possible) {
        logger::Warn("sleep: S3 deep sleep needs all wake pins RTC-capable (rtc_ok=%u/%u); using light sleep",
                     static_cast<unsigned>(rtcCount),
                     static_cast<unsigned>(pins.count));
    }
    return possible;
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    uint64_t configuredMask = 0;
    uint64_t validMask = 0;
    for (uint8_t index = 0U; index < pins.count; ++index) {
        const uint8_t pin = pins.pins[index];
        configuredMask |= (1ULL << pin);
        if (esp_sleep_is_valid_wakeup_gpio(static_cast<gpio_num_t>(pin))) {
            validMask |= (1ULL << pin);
        }
    }
    return (configuredMask != 0) && (validMask == configuredMask);
#else
    (void)pins;
    return false;
#endif
}

bool ConfigureLightSleepWakeSources(const SleepWakePinSet& pins) {
    bool configuredAny = false;
    for (uint8_t index = 0U; index < pins.count; ++index) {
        const uint8_t pin = pins.pins[index];
        const esp_err_t err = gpio_wakeup_enable(static_cast<gpio_num_t>(pin), GPIO_INTR_LOW_LEVEL);
        if (err != ESP_OK) {
            logger::Error("sleep: light sleep wake setup failed for gpio %u (%d)",
                          static_cast<unsigned>(pin),
                          static_cast<int>(err));
            return false;
        }
        configuredAny = true;
    }

    if (!configuredAny) {
        logger::Error("sleep: no light sleep wake buttons configured");
        return false;
    }

    const esp_err_t err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) {
        logger::Error("sleep: light sleep gpio wake enable failed (%d)", static_cast<int>(err));
        return false;
    }
    return true;
}

} // namespace

void SleepController::Begin() {
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const esp_reset_reason_t resetReason = esp_reset_reason();
    m_wakeAfterMs = 0U;
    m_wokeFromTimer = m_hardwareProfile.isS3 &&
        resetReason == ESP_RST_DEEPSLEEP && cause == ESP_SLEEP_WAKEUP_TIMER;

    if (ShouldRestoreRtcWakePinsForRuntime(
            resetReason == ESP_RST_DEEPSLEEP,
            cause == ESP_SLEEP_WAKEUP_EXT1 ||
                cause == ESP_SLEEP_WAKEUP_ULP)) {
        RestoreWakeButtonsForRuntimeOrRestart();
    }

    const bool cookieWake = (g_lightSleepRestartCookie == LIGHT_SLEEP_RESTART_COOKIE_VALUE);
    if (cookieWake) {
        g_lightSleepRestartCookie = 0;
    }

    m_bootKind = (cookieWake || IsButtonWakeCause(cause) || m_wokeFromTimer)
                     ? BootKind::DeepSleepWake
                     : BootKind::ColdBoot;

    logger::Trace("sleep: boot reset_reason=%d wake_cause=%d cookie=%d kind=%s",
                 static_cast<int>(resetReason),
                 static_cast<int>(cause),
                 cookieWake ? 1 : 0,
                 (m_bootKind == BootKind::DeepSleepWake) ? "wake" : "cold");
#if SOC_PM_SUPPORT_EXT_WAKEUP
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        logger::Trace("sleep: ext1 wake status=0x%llx",
                     static_cast<unsigned long long>(
                         esp_sleep_get_ext1_wakeup_status()));
    }
#endif

    if (resetReason == ESP_RST_DEEPSLEEP) {
        logger::Info("sleep: deep sleep confirmed (reset_reason=deepsleep)");
    }

    m_ignoreButtonsActive = (m_bootKind == BootKind::DeepSleepWake);
    m_allReleasedElapsed = fxe::ElapsedTime();
    m_idleActivityElapsed = fxe::ElapsedTime::StartNow();
}

void SleepController::MarkWokeFromSleep() {
    m_bootKind = BootKind::DeepSleepWake;
    m_ignoreButtonsActive = true;
    m_allReleasedElapsed = fxe::ElapsedTime();
    m_idleActivityElapsed = fxe::ElapsedTime::StartNow();
}

bool SleepController::AnyWakeButtonsDown() const {
    for (uint8_t index = 0U; index < m_buttonWakePins.count; ++index) {
        const uint8_t pin = m_buttonWakePins.pins[index];
        if (digitalRead(pin) == LOW) {
            return true;
        }
    }
    return false;
}

bool SleepController::AnyActiveWakeSourcesDown() const {
    for (uint8_t index = 0U; index < m_activeWakePins.count; ++index) {
        if (digitalRead(m_activeWakePins.pins[index]) == LOW) {
            return true;
        }
    }
    return false;
}

bool SleepController::CanBeginSleepTeardown() {
    ConfigureWakeInputs(m_buttonWakePins);
    return !AnyWakeButtonsDown();
}

bool SleepController::PrepareWakeSourceForSleep() {
    m_wakeSourcePrepared = false;
    m_activeWakePins = m_buttonWakePins;
    if (!CanBeginSleepTeardown()) {
        return false;
    }

    if (m_prepareOptionalWakeSource != nullptr &&
        !m_prepareOptionalWakeSource(m_optionalWakeSourceContext)) {
        logger::Error("sleep: optional wake source refused preparation");
        return false;
    }
    if (m_isOptionalWakeSourceArmed != nullptr &&
        m_isOptionalWakeSourceArmed(m_optionalWakeSourceContext)) {
        if (!m_activeWakePins.Add(m_optionalWakePin)) {
            logger::Error("sleep: optional wake source exceeds pin capacity");
            return false;
        }
        ConfigureWakeInputs(m_activeWakePins);
    }
    if (IsP1XDeepWakeEnabled()) {
        const uint8_t p1x = m_hardwareProfile.p1.x;
        pinMode(p1x, INPUT_PULLUP);
        if (digitalRead(p1x) == LOW) {
            logger::Error("sleep: P1-X remains asserted before teardown");
            return false;
        }
    }
    if (AnyActiveWakeSourcesDown()) {
        logger::Error("sleep: active-low wake source remains asserted");
        return false;
    }

    const esp_err_t disableErr =
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if ((disableErr != ESP_OK) && (disableErr != ESP_ERR_INVALID_STATE)) {
        logger::Error("sleep: could not clear stale wake sources (%d)",
                      static_cast<int>(disableErr));
        return false;
    }

    m_preparedForDeepSleep = CanUseDeepSleepWake(m_activeWakePins);
    if ((IsP1XDeepWakeEnabled() || m_wakeAfterMs != 0U) &&
        (!m_hardwareProfile.isS3 || !m_preparedForDeepSleep)) {
        logger::Error("sleep: requested wake requires verified S3 deep-sleep path");
        p1x_deep_wake::ConfigureWakeStub(false);
        return false;
    }
    if (!p1x_deep_wake::ConfigureWakeStub(m_hardwareProfile.isS3)) {
        logger::Error("sleep: P1-X ULP wake preparation failed");
        RestoreWakeButtonsAfterFailedPreparation();
        return false;
    }
    bool configured = m_preparedForDeepSleep
                                ? ConfigureWakeOnButtons()
                                : ConfigureLightSleepWakeSources(m_activeWakePins);
    if (configured && m_wakeAfterMs != 0U) {
        const esp_err_t timerErr = esp_sleep_enable_timer_wakeup(
            static_cast<uint64_t>(m_wakeAfterMs) * 1000ULL);
        if (timerErr != ESP_OK) {
            logger::Error("sleep: RTC timer setup failed (%d)", static_cast<int>(timerErr));
            configured = false;
        }
    }
    const bool sourcesReleased = !AnyActiveWakeSourcesDown();
    m_wakeSourcePrepared =
        IsSleepEntrySafeToTearDown(sourcesReleased, configured);
    if (!m_wakeSourcePrepared) {
        RestoreWakeButtonsAfterFailedPreparation();
        m_preparedForDeepSleep = false;
    }
    return m_wakeSourcePrepared;
}

void SleepController::UpdateIgnoreGate() {
    if (!m_ignoreButtonsActive) {
        return;
    }

    if (AnyWakeButtonsDown()) {
        m_allReleasedElapsed = fxe::ElapsedTime();
        return;
    }

    if (!m_allReleasedElapsed.Started()) {
        m_allReleasedElapsed.Reset();
        return;
    }

    if (m_allReleasedElapsed.HasElapsed(200U)) {
        m_ignoreButtonsActive = false;
    }
}

bool SleepController::EnterSleepNow() {
    logger::Info("sleep: entering sleep");

    const bool sourcesReleased = !AnyActiveWakeSourcesDown();
    LogWakeSourceLevels(m_activeWakePins);
    if (!IsSleepEntrySafeToTearDown(sourcesReleased,
                                    m_wakeSourcePrepared)) {
        logger::Error("sleep: refusing teardown (sources_released=%u wake_source=%u)",
                      static_cast<unsigned>(sourcesReleased ? 1U : 0U),
                      static_cast<unsigned>(m_wakeSourcePrepared ? 1U : 0U));
        return false;
    }
    const bool deepSleepPossible = m_preparedForDeepSleep;
    m_wakeSourcePrepared = false;

    if (m_preSleepCallback != nullptr) {
        logger::Trace("sleep: running pre-sleep callback");
        if (!m_preSleepCallback(m_preSleepCallbackContext)) {
            logger::Error("sleep: pre-sleep teardown incomplete; refusing sleep entry");
            return false;
        }
    }

    if (AnyActiveWakeSourcesDown()) {
        logger::Warn("sleep: wake source asserted during callback; aborting teardown");
        return false;
    }

    if (!sleep_controller_internal::StopI2cForSleep(m_hardwareProfile)) {
        logger::Error("sleep: refusing to sleep; i2c lines not high");
        return false;
    }

    if (AnyActiveWakeSourcesDown()) {
        logger::Warn("sleep: wake source asserted during i2c shutdown; aborting before USB detach");
        return false;
    }

    SleepWakePinSet preservedInputPins = m_activeWakePins;
    if (IsP1XDeepWakeEnabled() &&
        !preservedInputPins.Add(m_hardwareProfile.p1.x)) {
        logger::Error("sleep: could not preserve P1-X for ULP sampling");
        return false;
    }
    sleep_controller_internal::ApplyGpioLeakagePolicyForSleep(
        m_hardwareProfile, m_runtimeButtonPins, preservedInputPins);
    const bool detachedUsb =
        sleep_controller_internal::TryDetachUsbForSleep();

    const auto kind = GetRequestOrigin() == SleepRequestOrigin::Idle
        ? modules::SupportSleepKind::Automatic
        : GetRequestOrigin() == SleepRequestOrigin::Menu ||
          GetRequestOrigin() == SleepRequestOrigin::App
        ? modules::SupportSleepKind::Manual : modules::SupportSleepKind::Other;
    if (deepSleepPossible) {
        logger::Info("sleep: entering deep sleep");
        using HardwareTransition = void (*)();
        HardwareTransition volatile enterDeepSleep = &esp_deep_sleep_start;
        modules::SupportRuntimeStats::ArmSleepConfirmation(kind);
        enterDeepSleep();
        modules::SupportRuntimeStats::CancelSleepConfirmation();
        logger::Error("sleep: deep sleep entry returned unexpectedly");
        return false;
    }

    logger::Info("sleep: entering light sleep");

    const bool enableTimerWake = ShouldEnableLightSleepTimerWake(detachedUsb);
    if (enableTimerWake) {
        logger::Trace("sleep: light sleep enables %llu us timer wake because USB remained attached",
                     static_cast<unsigned long long>(LightSleepTimerWakeIntervalUs()));
        const esp_err_t timerErr = esp_sleep_enable_timer_wakeup(LightSleepTimerWakeIntervalUs());
        if (timerErr != ESP_OK) {
            logger::Warn("sleep: light sleep timer wake setup failed (%d)", static_cast<int>(timerErr));
        }
    } else if (!detachedUsb) {
        logger::Trace("sleep: release profile keeps light sleep on pure GPIO wake with USB still attached");
    }

    delay(20);

    while (true) {
        modules::SupportRuntimeStats::ArmSleepConfirmation(kind);
        const esp_err_t sleepErr = esp_light_sleep_start();
        if (sleepErr != ESP_OK) {
            modules::SupportRuntimeStats::CancelSleepConfirmation();
            logger::Error("sleep: light sleep entry failed (%d)", static_cast<int>(sleepErr));
            return false;
        }

        const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        if (cause == ESP_SLEEP_WAKEUP_GPIO || AnyActiveWakeSourcesDown()) {
            break;
        }
    }

    if (enableTimerWake) {
        (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }

    modules::SupportRuntimeStats::ConfirmLightSleep();
    g_lightSleepRestartCookie = LIGHT_SLEEP_RESTART_COOKIE_VALUE;
    delay(20);
    using HardwareTransition = void (*)();
    HardwareTransition volatile restart = &esp_restart;
    restart();
    logger::Error("sleep: restart returned unexpectedly");
    return false;
}

} // namespace firmware

#else

namespace firmware {

void SleepController::Begin() {
    m_wakeAfterMs = 0U;
    m_wokeFromTimer = false;
}

void SleepController::UpdateIgnoreGate() {}

bool SleepController::CanBeginSleepTeardown() {
    return true;
}

bool SleepController::PrepareWakeSourceForSleep() {
    return false;
}

bool SleepController::EnterSleepNow() {
    return false;
}

} // namespace firmware

#endif
