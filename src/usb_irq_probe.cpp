#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3)

#include <atomic>
#include <cstdint>

#include <esp_attr.h>
#include <esp_intr_alloc.h>
#include <soc/periph_defs.h>
#include <soc/usb_struct.h>

#include "firmware/usb_boot_probe.hpp"
#include "firmware/native_usb_reset_recovery.hpp"
#include "foxie_embedded/console.hpp"

namespace {

// [DEBUG-b5f1] Observe the pinned DCD's registration, not other SDK IRQ users.
constexpr int USB_DCD_IRQ_FLAGS = ESP_INTR_FLAG_LEVEL1 |
                                  ESP_INTR_FLAG_LEVEL2 | ESP_INTR_FLAG_LEVEL3;
static_assert(ETS_USB_INTR_SOURCE == 38);
static_assert(USB_DCD_IRQ_FLAGS == 14);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<intr_handler_t>::is_always_lock_free);
DRAM_ATTR std::atomic<uint32_t> g_usbDcdRegistering{0U};
DRAM_ATTR std::atomic<intr_handler_t> g_usbOriginalIrq{nullptr};
DRAM_ATTR std::atomic<uint32_t> g_usbIrqEntered{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbIrqExited{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbIrqRegistered{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbIrqLossSeen{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbIrqFaultReady{0U};
// Immutable after publication: raw flags/mask/AHB/address/endpoint mask,
// IN0/4/5 status, all-IN bit15 mask, then address/mask/raw flags after handler.
DRAM_ATTR uint32_t g_usbIrqFault[12]{};

void IRAM_ATTR ObserveUsbIrq(void* const arg) {
    const uint32_t raw = USB0.gintsts;
    const uint32_t mask = USB0.gintmsk;
    if (!firmware_internal::NativeUsbDcdInterruptBegin(raw, mask)) return;
    g_usbIrqEntered.fetch_add(1U, std::memory_order_relaxed);
    const uint32_t ahb = USB0.gahbcfg;
    const uint32_t address = USB0.dcfg;
    const uint32_t endpoints = USB0.daintmsk;
    const uint32_t ep0 = USB0.in_ep_reg[0].diepint;
    const uint32_t ep4 = USB0.in_ep_reg[4].diepint;
    const uint32_t ep5 = USB0.in_ep_reg[5].diepint;
    uint32_t unknown = 0U;
    // Actual SDK ISR visits seven IN endpoints, including unconfigured ones.
    for (unsigned ep = 0U; ep < 7U; ++ep) {
        if ((USB0.in_ep_reg[ep].diepint & 0x8000U) != 0U) {
            unknown |= uint32_t{1U} << ep;
        }
    }
    const auto handler = g_usbOriginalIrq.load(std::memory_order_acquire);
    handler(arg);
    const uint32_t afterAddress = USB0.dcfg;
    const uint32_t afterEndpoints = USB0.daintmsk;
    const uint32_t afterRaw = USB0.gintsts;
    g_usbIrqExited.fetch_add(1U, std::memory_order_relaxed);
    const bool configuredLoss = (address & 0x7F0U) != 0U && (afterAddress & 0x7F0U) == 0U &&
        endpoints != 0x00010001U && afterEndpoints == 0x00010001U;
    firmware_internal::NativeUsbDcdInterruptEnd(configuredLoss);
    if (configuredLoss) {
        g_usbIrqLossSeen.fetch_add(1U, std::memory_order_relaxed);
        uint32_t unpublished = 0U;
        if (g_usbIrqFaultReady.compare_exchange_strong(
                unpublished, 2U, std::memory_order_acquire)) {
            g_usbIrqFault[0] = raw;
            g_usbIrqFault[1] = mask;
            g_usbIrqFault[2] = ahb;
            g_usbIrqFault[3] = address;
            g_usbIrqFault[4] = endpoints;
            g_usbIrqFault[5] = ep0;
            g_usbIrqFault[6] = ep4;
            g_usbIrqFault[7] = ep5;
            g_usbIrqFault[8] = unknown;
            g_usbIrqFault[9] = afterAddress;
            g_usbIrqFault[10] = afterEndpoints;
            g_usbIrqFault[11] = afterRaw;
            g_usbIrqFaultReady.store(1U, std::memory_order_release);
        }
    }
}

} // namespace

extern "C" void __real_dcd_int_enable(uint8_t);
extern "C" void __wrap_dcd_int_enable(const uint8_t rhport) {
    if (rhport == 0U) g_usbDcdRegistering.store(1U, std::memory_order_relaxed);
    __real_dcd_int_enable(rhport);
    if (rhport == 0U) g_usbDcdRegistering.store(0U, std::memory_order_relaxed);
}

extern "C" esp_err_t __real_esp_intr_alloc(
    int, int, intr_handler_t, void*, intr_handle_t*);
extern "C" esp_err_t __wrap_esp_intr_alloc(
    const int source, const int flags, const intr_handler_t handler,
    void* const arg, intr_handle_t* const retHandle) {
    if (g_usbDcdRegistering.load(std::memory_order_relaxed) != 0U &&
        source == ETS_USB_INTR_SOURCE && flags == USB_DCD_IRQ_FLAGS && handler) {
        auto original = g_usbOriginalIrq.load(std::memory_order_acquire);
        if (!original) {
            g_usbOriginalIrq.compare_exchange_strong(
                original, handler, std::memory_order_acq_rel);
            original = g_usbOriginalIrq.load(std::memory_order_acquire);
        }
        if (original == handler) {
            // Preserve the SDK flags, argument, handle and result exactly.
            const auto result = __real_esp_intr_alloc(
                source, flags, ObserveUsbIrq, arg, retHandle);
            if (result == ESP_OK && retHandle != nullptr) {
                firmware_internal::CaptureNativeUsbDcdInterrupt(*retHandle);
                g_usbIrqRegistered.fetch_add(1U, std::memory_order_relaxed);
            }
            return result;
        }
    }
    return __real_esp_intr_alloc(source, flags, handler, arg, retHandle);
}

namespace firmware {

void PrintUsbIrqProbe(fxe::Console& console) {
    const auto ready = g_usbIrqFaultReady.load(std::memory_order_acquire);
    console.PrintLn("debug_usb_b5f1: irq=%lu/%lu/%lu/%lu/%u",
                    static_cast<unsigned long>(g_usbIrqEntered.load(std::memory_order_relaxed)),
                    static_cast<unsigned long>(g_usbIrqExited.load(std::memory_order_relaxed)),
                    static_cast<unsigned long>(g_usbIrqRegistered.load(std::memory_order_relaxed)),
                    static_cast<unsigned long>(g_usbIrqLossSeen.load(std::memory_order_relaxed)),
                    ready == 1U ? 1U : 0U);
    // Never read a plain field while the ISR could still be writing it.
    if (ready != 1U) return;
    console.PrintLn(
        "debug_usb_b5f1: fault=%08lx/%08lx/%08lx/%08lx/%08lx/%08lx/%08lx/%08lx/%08lx/%08lx/%08lx/%08lx",
        static_cast<unsigned long>(g_usbIrqFault[0]), static_cast<unsigned long>(g_usbIrqFault[1]),
        static_cast<unsigned long>(g_usbIrqFault[2]), static_cast<unsigned long>(g_usbIrqFault[3]),
        static_cast<unsigned long>(g_usbIrqFault[4]), static_cast<unsigned long>(g_usbIrqFault[5]),
        static_cast<unsigned long>(g_usbIrqFault[6]), static_cast<unsigned long>(g_usbIrqFault[7]),
        static_cast<unsigned long>(g_usbIrqFault[8]), static_cast<unsigned long>(g_usbIrqFault[9]),
        static_cast<unsigned long>(g_usbIrqFault[10]), static_cast<unsigned long>(g_usbIrqFault[11]));
}

} // namespace firmware

#endif
