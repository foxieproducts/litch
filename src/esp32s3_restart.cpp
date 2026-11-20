#if defined(ARDUINO) && defined(LITCH_ESP32S3)
#include <esp_attr.h>
#include <hal/cpu_hal.h>
#include <regi2c_ctrl.h>
#include <soc/rtc_cntl_reg.h>

namespace {
bool IRAM_ATTR OtherCoreIsStalled() {
    const auto stall = cpu_hal_get_core_id() == 0
        ? (REG_GET_FIELD(RTC_CNTL_SW_CPU_STALL_REG, RTC_CNTL_SW_STALL_APPCPU_C1) << 2U) |
              REG_GET_FIELD(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_STALL_APPCPU_C0)
        : (REG_GET_FIELD(RTC_CNTL_SW_CPU_STALL_REG, RTC_CNTL_SW_STALL_PROCPU_C1) << 2U) |
              REG_GET_FIELD(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_STALL_PROCPU_C0);
    return stall == 0x86U;
}
} // namespace

extern "C" [[noreturn]] void __real_esp_restart_noos();

extern "C" [[noreturn]] void IRAM_ATTR __wrap_esp_restart_noos() {
    // The pinned SDK stops the peer before its XTAL switch takes the regi2c
    // lock. Own that recursive lock first, while the peer can still release it.
    // Shutdown callbacks have already completed at this boundary. Panic may
    // have stalled the peer earlier; leave that existing SDK path unchanged.
    if (!OtherCoreIsStalled()) {
        regi2c_enter_critical();
    }
    __real_esp_restart_noos();
}
#endif
