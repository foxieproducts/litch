#if defined(ARDUINO) && defined(LITCH_ESP32S3) && defined(LITCH_PANIC_DIAGNOSTIC)
#include <esp_attr.h>
#include <esp_private/panic_internal.h>
#include <xtensa/xtensa_context.h>
#if defined(__XTENSA__) && defined(LITCH_BOOT_ENTRY_DIAGNOSTIC)
#include <soc/rtc_cntl_reg.h>
static_assert(RTC_CNTL_RESET_STATE_REG == 0x60008038U &&
              RTC_CNTL_STORE6_REG == 0x600080c8U &&
              RTC_CNTL_BROWN_OUT_REG == 0x600080e8U,
              "private entry assembly register addresses must match the S3 SDK");
static_assert(RTC_CNTL_RESET_CAUSE_PROCPU_S == 0U &&
              RTC_CNTL_RESET_CAUSE_APPCPU_S == 6U &&
              RTC_CNTL_RESET_CAUSE_PROCPU_V == 0x3fU &&
              RTC_CNTL_RESET_CAUSE_APPCPU_V == 0x3fU,
              "private entry reset decoding must match the S3 SDK");
#endif
#include "runtime/panic_capture_internal.hpp"

namespace runtime::panic_diagnostic {
// Fixed NOLOAD gap: does not move the normal RTC layout or overlap the raw ULP
// owner at word 2016. tools/pio/panic_diagnostic.ld enforces both boundaries.
#if defined(__XTENSA__)
volatile Capsule g_capsule __attribute__((section(".rtc.panic_diagnostic"), used));
volatile StackProbe g_stackProbe __attribute__((section(".rtc.panic_stack_probe"), used));
#else
volatile Capsule g_capsule;
volatile StackProbe g_stackProbe;
#endif
#if defined(LITCH_BOOT_ENTRY_DIAGNOSTIC)
extern "C" {
#if defined(__XTENSA__)
volatile EntryWitness litch_boot_entry_witness
    __attribute__((section(".rtc.boot_entry_witness"), used));
#else
volatile EntryWitness litch_boot_entry_witness;
#endif
}
#endif
namespace { uint32_t g_entered; }
static_assert(PANIC_EXCEPTION_DEBUG == 0, "pinned debug category ABI");
static_assert(PANIC_EXCEPTION_ABORT == 3, "pinned abort category ABI");
static_assert(offsetof(XtExcFrame, pc) == offsetof(ExceptionFrame, pc), "SDK PC ABI");
static_assert(offsetof(XtExcFrame, ps) == offsetof(ExceptionFrame, ps), "SDK PS ABI");
static_assert(offsetof(XtExcFrame, a0) == offsetof(ExceptionFrame, a0), "SDK A0 ABI");
static_assert(offsetof(XtExcFrame, a1) == offsetof(ExceptionFrame, a1), "SDK SP ABI");
#define LITCH_CHECK_FRAME_A(n) static_assert(offsetof(XtExcFrame, a##n) == \
    offsetof(ExceptionFrame, a) + ((n) - 2U) * 4U, "SDK A" #n " ABI")
LITCH_CHECK_FRAME_A(2); LITCH_CHECK_FRAME_A(3); LITCH_CHECK_FRAME_A(4); LITCH_CHECK_FRAME_A(5);
LITCH_CHECK_FRAME_A(6); LITCH_CHECK_FRAME_A(7); LITCH_CHECK_FRAME_A(8); LITCH_CHECK_FRAME_A(9);
LITCH_CHECK_FRAME_A(10); LITCH_CHECK_FRAME_A(11); LITCH_CHECK_FRAME_A(12); LITCH_CHECK_FRAME_A(13);
LITCH_CHECK_FRAME_A(14); LITCH_CHECK_FRAME_A(15);
#undef LITCH_CHECK_FRAME_A
static_assert(offsetof(XtExcFrame, sar) == offsetof(ExceptionFrame, sar), "SDK SAR ABI");
static_assert(offsetof(XtExcFrame, exccause) == offsetof(ExceptionFrame, cause), "SDK cause ABI");
static_assert(offsetof(XtExcFrame, excvaddr) == offsetof(ExceptionFrame, faultAddress), "SDK address ABI");
} // namespace runtime::panic_diagnostic

extern "C" void* volatile pxCurrentTCB[2];
extern "C" [[noreturn]] void __real_esp_panic_handler(panic_info_t*);
extern "C" [[noreturn]] void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t* info) {
    using namespace runtime::panic_diagnostic;
    // Single compare/exchange instruction, including nested or dual-core entry.
    // Native fixture uses the equivalent compiler atomic.
    uint32_t previous = 1U;
#if defined(__XTENSA__)
    const uint32_t expected = 0U;
    asm volatile("wsr %2, scompare1\n\ts32c1i %0, %1, 0"
                 : "+a"(previous) : "a"(&g_entered), "a"(expected) : "memory");
#else
    previous = __atomic_exchange_n(&g_entered, 1U, __ATOMIC_RELAXED);
#endif
    if (previous == 0U && ShouldCapture(g_capsule)) {
        FaultRegisters registers{};
#if defined(__XTENSA__)
        asm volatile("rsr %0, debugcause\n\trsr %1, dbreaka1\n\trsr %2, dbreakc1"
                     : "=a"(registers.debugCause), "=a"(registers.breakAddress),
                       "=a"(registers.breakControl) : : "memory");
#endif
        Capture(g_capsule, info, pxCurrentTCB, registers, g_panic_abort);
        CaptureStackProbe(g_stackProbe, g_capsule);
    }
    __real_esp_panic_handler(info);
}
#endif
