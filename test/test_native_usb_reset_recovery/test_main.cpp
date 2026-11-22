#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include <esp_system.h>
#include <tusb.h>

#include "firmware/native_usb_reset_recovery.hpp"
#include "foxie_embedded/millis.hpp"
#define USB0 ObservedNativeUsbResetUSB0
#include <soc/usb_struct.h>
#include <esp_intr_alloc.h>
#include <freertos/queue.h>
#include <device/dcd.h>

namespace {

unsigned g_resetReason = 1U;
unsigned g_detaches = 0U;
unsigned g_attaches = 0U;
uint32_t g_attachedAtMs = 0U;
unsigned g_failures = 0U;

unsigned ObservedUsbResetReason() { return g_resetReason; }
bool ObservedUsbDisconnect() { ++g_detaches; return true; }
bool ObservedUsbConnect() {
    ++g_attaches;
    g_attachedAtMs = fxe::Millis();
    return true;
}

void Check(const bool condition, const char* label) {
    if (!condition) {
        if (g_failures < 8U) std::printf("[FAIL] usb_reset: %s\n", label);
        ++g_failures;
    }
}

} // namespace

// Compile the actual SDK-bound owner. Only reset reason, clock and USB calls
// are observed; this cannot emulate a macOS/TinyUSB bulk endpoint stall.
// Values are the pinned ESP-IDF esp_reset_reason_t enum, not RTC state.
#define ESP_RST_SW 3U
#define ESP_RST_DEEPSLEEP 8U
#define ARDUINO 1
#define LITCH_HW_VARIANT_S3 1
#define ARDUINO_USB_MODE 0
#define esp_reset_reason ObservedUsbResetReason
#define tud_disconnect ObservedUsbDisconnect
#define tud_connect ObservedUsbConnect
#define BeginNativeUsbResetRecovery ObservedBeginNativeUsbResetRecovery
#define PollNativeUsbResetRecovery ObservedPollNativeUsbResetRecovery
#define CaptureNativeUsbDcdInterrupt ObservedCaptureNativeUsbDcdInterrupt
#define NativeUsbDcdInterruptBegin ObservedNativeUsbDcdInterruptBegin
#define NativeUsbDcdInterruptEnd ObservedNativeUsbDcdInterruptEnd
#define NativeUsbRecoveryBlocksConnect ObservedNativeUsbRecoveryBlocksConnect
#define BeforeNativeUsbEvent ObservedBeforeNativeUsbEvent
#define AfterNativeUsbEvent ObservedAfterNativeUsbEvent
#include "../../src/native_usb_reset_recovery.cpp"
#undef CaptureNativeUsbDcdInterrupt
#undef NativeUsbDcdInterruptBegin
#undef NativeUsbDcdInterruptEnd
#undef NativeUsbRecoveryBlocksConnect
#undef BeforeNativeUsbEvent
#undef AfterNativeUsbEvent

#undef BeginNativeUsbResetRecovery
#undef PollNativeUsbResetRecovery
#undef tud_disconnect
#undef tud_connect
#undef esp_reset_reason
#undef ARDUINO_USB_MODE
#undef LITCH_HW_VARIANT_S3
#undef ARDUINO
#undef ESP_RST_DEEPSLEEP
#undef ESP_RST_SW

namespace {

void CheckRecovery(const unsigned reason, const uint32_t start) {
    const unsigned beforeDetach = g_detaches;
    const unsigned beforeAttach = g_attaches;
    g_resetReason = reason;
    fxe::ManualMillis::SetNowMs(start);
    firmware_internal::ObservedBeginNativeUsbResetRecovery();
    Check(g_detaches == beforeDetach + 1U, "warm reset gives host a new detach boundary");
    Check(g_attaches == beforeAttach, "setup returns with host still detached");
    for (unsigned ms = 0U; ms < 500U; ++ms) {
        fxe::ManualMillis::SetNowMs(start + ms);
        firmware_internal::ObservedPollNativeUsbResetRecovery();
        Check(g_attaches == beforeAttach, "loop remains detached during recovery dwell");
    }
    fxe::ManualMillis::SetNowMs(start + 500U);
    firmware_internal::ObservedPollNativeUsbResetRecovery();
    Check(g_attaches == beforeAttach + 1U && g_attachedAtMs == start + 500U,
          "loop reconnects once at recovery deadline, including clock rollover");
    for (unsigned ms = 501U; ms < 510U; ++ms) {
        fxe::ManualMillis::SetNowMs(start + ms);
        firmware_internal::ObservedPollNativeUsbResetRecovery();
    }
    Check(g_attaches == beforeAttach + 1U, "normal loop cannot repeat the attachment");
}

} // namespace

int main() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    g_resetReason = 1U;
    firmware_internal::ObservedBeginNativeUsbResetRecovery();
    firmware_internal::ObservedPollNativeUsbResetRecovery();
    Check(g_detaches == 0U && g_attaches == 0U, "ordinary cold boot keeps initial USB attachment");
    for (const unsigned reason : {0U, 2U, 4U, 5U, 6U, 7U, 9U, 10U}) {
        g_resetReason = reason;
        firmware_internal::ObservedBeginNativeUsbResetRecovery();
        firmware_internal::ObservedPollNativeUsbResetRecovery();
    }
    Check(g_detaches == 0U && g_attaches == 0U, "unrelated reset reasons retain existing attachment policy");
    CheckRecovery(8U, 1000U);
    CheckRecovery(3U, 2000U);
    CheckRecovery(3U, UINT32_MAX - 123U);
    for (unsigned reset = 0U; reset < 500U; ++reset) CheckRecovery(3U, reset * 1000U);
    std::printf("[USB_RESET] failures=%u detaches=%u attaches=%u\n",
                g_failures, g_detaches, g_attaches);
    return g_failures == 0U ? 0 : 1;
}

usb_dev_t USB0;
extern "C" esp_err_t esp_intr_disable(intr_handle_t) { return ESP_OK; }
extern "C" esp_err_t esp_intr_enable(intr_handle_t) { return ESP_OK; }
extern "C" void dcd_init(uint8_t) {}
extern "C" void dcd_event_handler(const dcd_event_t*, bool) {}
extern "C" BaseType_t __real_xQueueGenericSend(QueueHandle_t, const void*, TickType_t, BaseType_t) { return pdFALSE; }
extern "C" BaseType_t __real_xQueueGenericSendFromISR(QueueHandle_t, const void*, BaseType_t*, BaseType_t) { return pdFALSE; }
#undef USB0
