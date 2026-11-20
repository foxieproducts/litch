#include <cstdio>
#include <string>

#include <device/dcd.h>
#include <device/usbd_pvt.h>
#include <soc/usb_struct.h>
#include <tusb.h>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/console_transport_memory.hpp"
#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"

namespace {
usb_dev_t g_observedBootUsb;
bool ObservedBootBusy(uint8_t, uint8_t) { return false; }
uint32_t ObservedBootCapacity(uint8_t) { return 64U; }
unsigned g_failures = 0U;
void Check(const bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message);
    }
}
} // namespace

namespace firmware {
void ObservedBootIrqPrint(fxe::Console&) {}
}

// Actual retained record owner and console formatter. SDK registers and USB
// forwarding are inert boundaries; this does not simulate physical USB or LEDs.
#define ARDUINO 1
#define LITCH_HW_VARIANT_S3 1
#define IRAM_ATTR
#define DRAM_ATTR
#define USB0 g_observedBootUsb
#define usbd_edpt_busy ObservedBootBusy
#define tud_cdc_n_write_available ObservedBootCapacity
#define PrintUsbIrqProbe ObservedBootIrqPrint
#include "../../src/usb_boot_probe.cpp"
#undef PrintUsbIrqProbe
#undef tud_cdc_n_write_available
#undef usbd_edpt_busy
#undef USB0
#undef DRAM_ATTR
#undef IRAM_ATTR
#undef LITCH_HW_VARIANT_S3
#undef ARDUINO

extern "C" void __real_dcd_event_handler(const dcd_event_t*, bool) {}
extern "C" void __real_dcd_edpt_clear_stall(uint8_t, uint8_t) {}
extern "C" bool __real_dcd_edpt_xfer(uint8_t, uint8_t, uint8_t*, uint16_t) { return true; }
extern "C" void __real_dcd_init(uint8_t) {}
extern "C" void __real_dcd_connect(uint8_t) {}
extern "C" bool __real_tud_disconnect() { return true; }
extern "C" bool __real_tud_connect() { return true; }
extern "C" void __real_tud_mount_cb() {}

int main() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    firmware::RecordUsbBootPrecharge(0x01000000U, 134U);
    Check(g_usbProbeCount.load() == 0U, "out-of-range parameters cannot alias a valid record");
    fxe::ManualMillis::SetNowMs(376U);
    firmware::RecordUsbBootPrecharge(1000U, 134U);
    fxe::ManualMillis::SetNowMs(472U);
    firmware::RecordUsbBootFirstNonzeroRender();
    fxe::ManualMillis::SetNowMs(1091U);
    firmware::RecordUsbBootServicesReady();
    Check(g_usbProbeCount.load() == 5U &&
              g_usbProbePublished[0].load() == 0x1FU &&
              g_usbProbeSetup[0].load() == 0U,
          "actual startup record publication is complete and distinct from SETUP packets");
    Check(g_usbProbeWords[0].load() == 0xF70003E8U &&
              g_usbProbeWords[1].load() == 0xF6000086U &&
              g_usbProbeWords[2].load() == 0xFB000178U &&
              g_usbProbeWords[3].load() == 0xF50001D8U &&
              g_usbProbeWords[4].load() == 0xF4000443U,
          "selected parameters and actual return times retained");
    // The host attaches after all early logger lines have gone. Later control
    // traffic can fill the record, but cannot overwrite these startup words.
    fxe::ManualMillis::SetNowMs(6500U);
    for (unsigned i = 0U; i < 80U; ++i) {
        dcd_event_t event{};
        event.event_id = DCD_EVENT_SETUP_RECEIVED;
        event.setup_received = {0x80U, 6U, 0x0100U, 0U, 18U};
        __wrap_dcd_event_handler(&event, true);
    }
    Check(g_usbProbeCount.load() == 85U &&
              g_usbProbeWords[0].load() == 0xF70003E8U &&
              g_usbProbeWords[4].load() == 0xF4000443U,
          "late attachment and bounded tail drop preserve first startup witnesses");
    fxe::MemoryConsoleTransport transport;
    fxe::Console console(transport);
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);
    firmware::PrintUsbBootProbe(console);
    Check(output.Str().find("seen=85 retained=64") != std::string::npos &&
              output.Str().find("f70003e8 f6000086 fb000178 f50001d8 f4000443") != std::string::npos &&
              output.Str().find("debug_usb_b5f1: w=56 ") != std::string::npos,
          "real console readback remains complete after delayed attachment");
    std::printf("[USB_BOOT_PROBE_CONTROLLED_BEGIN]\n%s[USB_BOOT_PROBE_CONTROLLED_END]\n",
                output.Str().c_str());
    std::printf("[USB_BOOT_PROBE] failures=%u\n", g_failures);
    return g_failures == 0U ? 0 : 1;
}
