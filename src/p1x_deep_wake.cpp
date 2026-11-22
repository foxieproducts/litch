#include "firmware/p1x_deep_wake.hpp"

#if defined(ARDUINO) && defined(LITCH_ESP32S3)
#include <cstring>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <esp32s3/ulp.h>
#include <ulp_common.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/rtc_io_reg.h>
#endif

namespace firmware::p1x_deep_wake {
namespace {

#if defined(ARDUINO) && defined(LITCH_ESP32S3)
// Arduino reserves no ULP bytes, while linker-owned RTC data grows upward from
// word zero. Keep this tiny hand-encoded FSM at the far end of the S3's
// 2048-word RTC slow memory.
constexpr uint16_t ULP_PROGRAM_WORD = 2016U;
constexpr uint16_t ULP_STATE_WORD = 2040U;

const ulp_insn_t ULP_PROGRAM[] = {
    // The timer already runs during teardown. Ignore awake button episodes so
    // they cannot stop it before sleep with an ineffective WAKE instruction.
    I_RD_REG(RTC_CNTL_LOW_POWER_ST_REG, RTC_CNTL_RDY_FOR_WAKEUP_S, RTC_CNTL_RDY_FOR_WAKEUP_S),
    I_BL(19, 1), // Not ready: halt this sample, retaining the timer.
    I_MOVI(R3, ULP_STATE_WORD),
    I_LD(R2, R3, 0),
    I_RD_REG(RTC_GPIO_IN_REG, RTC_GPIO_IN_NEXT_S, RTC_GPIO_IN_NEXT_S),
    I_BL(12, 1),
    I_MOVR(R0, R2),
    I_BL(13, 1),
    I_ADDI(R2, R2, 1),
    I_ST(R2, R3, 0),
    I_MOVR(R0, R2),
    I_BL(9, WAKE_STATE),
    I_MOVI(R2, 0),
    I_ST(R2, R3, 0),
    I_END(),
    I_WAKE(),
    I_HALT(),
    I_MOVI(R2, 1),
    I_ST(R2, R3, 0),
    I_HALT(),
    I_HALT(),
};

static_assert(ULP_PROGRAM_WORD +
                  (sizeof(ULP_PROGRAM) / sizeof(ULP_PROGRAM[0])) <
              ULP_STATE_WORD,
              "P1-X ULP program overlaps its state word");
#endif

} // namespace

bool ConfigureWakeStub(const bool isS3) {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    ulp_timer_stop();
    if (!isS3) {
        return true;
    }

    const gpio_num_t p1x = GPIO_NUM_0;
    if (rtc_gpio_init(p1x) != ESP_OK ||
        rtc_gpio_set_direction(p1x, RTC_GPIO_MODE_INPUT_ONLY) != ESP_OK ||
        rtc_gpio_pullup_en(p1x) != ESP_OK ||
        rtc_gpio_pulldown_dis(p1x) != ESP_OK ||
        rtc_gpio_hold_dis(p1x) != ESP_OK) {
        return false;
    }
    for (size_t index = 0U;
         index < sizeof(ULP_PROGRAM) / sizeof(ULP_PROGRAM[0]);
         ++index) {
        uint32_t encodedInstruction = 0U;
        static_assert(sizeof(encodedInstruction) == sizeof(ULP_PROGRAM[index]),
                      "ULP instruction must occupy one RTC slow-memory word");
        std::memcpy(&encodedInstruction,
                    &ULP_PROGRAM[index],
                    sizeof(encodedInstruction));
        RTC_SLOW_MEM[ULP_PROGRAM_WORD + index] = encodedInstruction;
    }
    RTC_SLOW_MEM[ULP_STATE_WORD] = 0U;
    if (ulp_set_wakeup_period(0U, POLL_INTERVAL_US) != ESP_OK ||
        ulp_run(ULP_PROGRAM_WORD) != ESP_OK ||
        esp_sleep_enable_ulp_wakeup() != ESP_OK) {
        ulp_timer_stop();
        return false;
    }
    return true;
#else
    (void)isS3;
    return true;
#endif
}

} // namespace firmware::p1x_deep_wake
