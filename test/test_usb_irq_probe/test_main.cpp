#include <cstdio>
#include "foxie_embedded/console.hpp"

#define ARDUINO 1
#define LITCH_HW_VARIANT_S3 1
// Attribute substitution is local to this production-source observation.
// The aggregate's existing SDK shim remains the shared esp_attr.h owner.
#define IRAM_ATTR
#define DRAM_ATTR
#include "../../src/usb_irq_probe.cpp"
#undef DRAM_ATTR
#undef IRAM_ATTR
#undef LITCH_HW_VARIANT_S3
#undef ARDUINO

usb_dev_t USB0;
namespace {
unsigned g_failures = 0U;
unsigned g_allocCalls = 0U;
unsigned g_enableCalls = 0U;
unsigned g_handlerCalls = 0U;
int g_source = 38;
int g_flags = 14;
int g_return = 0;
bool g_cleanup = false;
bool g_normalizedReset = false;
intr_handler_t g_allocated = nullptr;
void* g_receivedArg = nullptr;
void* g_expectedArg = nullptr;
intr_handle_t* g_receivedHandle = nullptr;
intr_handle_t g_handle = nullptr;

void Check(const bool condition, const char* const message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message);
    }
}

void ObservedDcdHandler(void* const arg) {
    ++g_handlerCalls;
    Check(arg == g_expectedArg, "original IRQ argument preserved");
    if (g_cleanup) {
        // Controlled equivalent of compiled bus_reset's paired writes. No
        // normalized reset is emitted: this is the escaped trace blind spot.
        USB0.dcfg = USB0.dcfg & ~0x7F0U;
        USB0.daintmsk = 0x00010001U;
        USB0.gintsts = USB0.gintsts & ~0x1000U;
    }
}

void OtherHandler(void*) {}

void Configure() {
    USB0.gintsts = 0x00041000U;
    USB0.gintmsk = 0x208C3C16U;
    USB0.gahbcfg = 1U;
    USB0.dcfg = 0x08100017U;
    USB0.daintmsk = 0x00090031U;
    USB0.in_ep_reg[4].diepint = 0x8080U;
    USB0.in_ep_reg[6].diepint = 0x8000U;
}
} // namespace

extern "C" esp_err_t __real_esp_intr_alloc(
    const int source, const int flags, const intr_handler_t handler,
    void* const arg, intr_handle_t* const retHandle) {
    ++g_allocCalls;
    Check(source == g_source && flags == g_flags, "allocation source and flags preserved");
    g_allocated = handler;
    g_receivedArg = arg;
    g_receivedHandle = retHandle;
    return g_return;
}

extern "C" void __real_dcd_int_enable(const uint8_t rhport) {
    ++g_enableCalls;
    Check(rhport <= 1U, "rhport forwarded unchanged");
    const int result = __wrap_esp_intr_alloc(
        g_source, g_flags, ObservedDcdHandler, g_expectedArg, &g_handle);
    Check(result == g_return, "allocation result forwarded unchanged");
}

int main() {
    int token = 0;
    g_expectedArg = &token;
    Check(__wrap_esp_intr_alloc(38, 14, OtherHandler, &token, &g_handle) == 0,
          "other registration result preserved");
    Check(g_allocated == OtherHandler, "outside DCD registration remains untouched");
    __wrap_dcd_int_enable(1U);
    Check(g_allocated == ObservedDcdHandler, "other rhport remains untouched");
    g_source = 39;
    __wrap_dcd_int_enable(0U);
    Check(g_allocated == ObservedDcdHandler, "other interrupt source remains untouched");
    g_source = 38;
    g_flags = 15;
    __wrap_dcd_int_enable(0U);
    Check(g_allocated == ObservedDcdHandler, "other SDK flags remain untouched");
    g_flags = 14;
    g_return = -7;
    __wrap_dcd_int_enable(0U);
    Check(g_allocated == ObserveUsbIrq && g_usbIrqRegistered.load() == 0U,
          "failed registration does not claim a successful observer binding");
    g_return = 0;
    __wrap_dcd_int_enable(0U);
    Check(g_receivedArg == &token && g_receivedHandle == &g_handle,
          "argument and handle pointer preserved at SDK boundary");
    Check(g_usbIrqRegistered.load() == 1U, "successful binding counted once");
    Configure();
    g_allocated(g_receivedArg);
    Check(g_handlerCalls == 1U && g_usbIrqEntered.load() == 1U &&
              g_usbIrqExited.load() == 1U && g_usbIrqFaultReady.load() == 0U,
          "ordinary IRQ forwarded exactly once without a false fault");
    g_cleanup = true;
    g_allocated(g_receivedArg);
    Check(g_handlerCalls == 2U && g_usbIrqLossSeen.load() == 1U &&
              g_usbIrqFaultReady.load() == 1U && !g_normalizedReset,
          "paired cleanup captured despite absence of normalized reset");
    Check(g_usbIrqFault[0] == 0x41000U && g_usbIrqFault[1] == 0x208C3C16U &&
              g_usbIrqFault[2] == 1U && g_usbIrqFault[3] == 0x08100017U &&
              g_usbIrqFault[4] == 0x90031U && g_usbIrqFault[6] == 0x8080U &&
              g_usbIrqFault[8] == 0x50U && g_usbIrqFault[9] == 0x08100007U &&
              g_usbIrqFault[10] == 0x10001U && g_usbIrqFault[11] == 0x40000U,
          "raw cause/endpoint state and paired post-handler state retained");
    Configure();
    USB0.gintsts = 0x00800000U;
    g_allocated(g_receivedArg);
    Check(g_usbIrqLossSeen.load() == 2U && g_usbIrqFault[0] == 0x41000U,
          "later cleanup cannot overwrite first published fault");
    __wrap_dcd_int_enable(0U);
    Check(g_usbIrqRegistered.load() == 2U, "same-handler re-registration supported");
    __wrap_esp_intr_alloc(38, 14, OtherHandler, nullptr, nullptr);
    Check(g_allocated == OtherHandler, "later unrelated user still forwarded");
    Check(g_allocCalls == 8U && g_enableCalls == 6U,
          "original SDK calls occur exactly once for every boundary invocation");
    std::printf("[USB_IRQ_PROBE] failures=%u\n", g_failures);
    return g_failures == 0U ? 0 : 1;
}
