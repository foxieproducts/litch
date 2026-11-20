#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3)

#include <atomic>
#include <cstdint>

#include <esp_attr.h>
#include <soc/usb_struct.h>
#include <tusb.h>
#include <device/dcd.h>
#include <device/usbd_pvt.h>

#include "firmware/usb_boot_probe.hpp"
#include "firmware/native_usb_reset_recovery.hpp"
#include "foxie_embedded/console.hpp"
#include "foxie_embedded/millis.hpp"

namespace {

// [DEBUG-b5f1] First 64 control/lifecycle/startup events of one diagnostic boot.
// ISR records contain no time calls, logging, allocation or waits. Task markers
// use fxe::Millis(), truncated to 24 bits; trials must finish within 4 hours.
constexpr unsigned USB_PROBE_WORDS = 64U;
static_assert(std::atomic<uint32_t>::is_always_lock_free);
DRAM_ATTR std::atomic<uint32_t> g_usbProbeCount{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbProbeWords[USB_PROBE_WORDS]{};
DRAM_ATTR std::atomic<uint32_t> g_usbProbePublished[2]{};
DRAM_ATTR std::atomic<uint32_t> g_usbProbeSetup[2]{};
DRAM_ATTR std::atomic<uint32_t> g_usbProbeClearSeen{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbProbeClearIndex{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbProbeClearApplied{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbProbeInArmed{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbProbeInRejected{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbProbeInArmLength{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbProbeInCompleted{0U};
DRAM_ATTR std::atomic<uint32_t> g_usbProbeInComplete{0U};

void IRAM_ATTR RecordUsbProbe(const uint32_t word, const bool isSetup = false) {
    const auto index = g_usbProbeCount.fetch_add(1U, std::memory_order_relaxed);
    if (index < USB_PROBE_WORDS) {
        const auto mask = uint32_t{1U} << (index % 32U);
        if (isSetup) {
            g_usbProbeSetup[index / 32U].fetch_or(mask, std::memory_order_relaxed);
        }
        g_usbProbeWords[index].store(word, std::memory_order_release);
        g_usbProbePublished[index / 32U].fetch_or(mask, std::memory_order_release);
    }
}

void RecordUsbTaskMarker(const uint8_t marker) {
    RecordUsbProbe((static_cast<uint32_t>(marker) << 24U) |
                   (fxe::Millis() & 0x00FFFFFFU));
}

} // namespace

extern "C" void __real_dcd_event_handler(const dcd_event_t*, bool);
extern "C" void IRAM_ATTR __wrap_dcd_event_handler(
    const dcd_event_t* event, const bool inIsr) {
    if (event->event_id == DCD_EVENT_SETUP_RECEIVED) {
        const auto& request = event->setup_received;
        if (request.bmRequestType == 0x02U &&
            request.bRequest == TUSB_REQ_CLEAR_FEATURE &&
            request.wValue == TUSB_REQ_FEATURE_EDPT_HALT && request.wLength == 0U) {
            g_usbProbeClearIndex.store(request.wIndex, std::memory_order_relaxed);
            g_usbProbeClearSeen.fetch_add(1U, std::memory_order_relaxed);
        }
        RecordUsbProbe((static_cast<uint32_t>(request.bmRequestType) << 24U) |
                       (static_cast<uint32_t>(request.bRequest) << 16U) |
                       request.wValue, true);
    } else if (event->event_id == DCD_EVENT_XFER_COMPLETE &&
               event->rhport == 0U && event->xfer_complete.ep_addr == 0x84U) {
        // Diagnostic snapshot only: not a transactional pairing with an arm.
        g_usbProbeInComplete.store(
            (static_cast<uint32_t>(event->xfer_complete.result) << 24U) |
                (event->xfer_complete.len & 0x00FFFFFFU),
            std::memory_order_relaxed);
        g_usbProbeInCompleted.fetch_add(1U, std::memory_order_relaxed);
    } else if (event->event_id == DCD_EVENT_BUS_RESET ||
               event->event_id == DCD_EVENT_UNPLUGGED ||
               event->event_id == DCD_EVENT_SUSPEND ||
               event->event_id == DCD_EVENT_RESUME) {
        RecordUsbProbe(0xF0000000U | (inIsr ? 0x00010000U : 0U) | event->event_id);
    }
    if (firmware_internal::BeforeNativeUsbEvent(event, inIsr)) {
        __real_dcd_event_handler(event, inIsr);
        firmware_internal::AfterNativeUsbEvent(event);
    }
}

extern "C" void __real_dcd_edpt_clear_stall(uint8_t, uint8_t);
extern "C" void __wrap_dcd_edpt_clear_stall(const uint8_t rhport, const uint8_t epAddr) {
    if (rhport == 0U && epAddr == 0x84U) {
        g_usbProbeClearApplied.fetch_add(1U, std::memory_order_relaxed);
    }
    __real_dcd_edpt_clear_stall(rhport, epAddr);
}

extern "C" bool __real_dcd_edpt_xfer(uint8_t, uint8_t, uint8_t*, uint16_t);
extern "C" bool __wrap_dcd_edpt_xfer(
    const uint8_t rhport, const uint8_t epAddr, uint8_t* buffer, const uint16_t length) {
    const bool accepted = __real_dcd_edpt_xfer(rhport, epAddr, buffer, length);
    if (rhport == 0U && epAddr == 0x84U) {
        g_usbProbeInArmLength.store(length, std::memory_order_relaxed);
        (accepted ? g_usbProbeInArmed : g_usbProbeInRejected)
            .fetch_add(1U, std::memory_order_relaxed);
    }
    return accepted;
}

extern "C" void __real_dcd_init(uint8_t);
extern "C" void __wrap_dcd_init(const uint8_t rhport) {
    RecordUsbTaskMarker(0xF8U);
    __real_dcd_init(rhport);
    RecordUsbTaskMarker(0xF9U);
}

extern "C" void __real_dcd_connect(uint8_t);
extern "C" void __wrap_dcd_connect(const uint8_t rhport) {
    RecordUsbTaskMarker(0xFAU);
    if (!firmware_internal::NativeUsbRecoveryBlocksConnect()) __real_dcd_connect(rhport);
}

extern "C" bool __real_tud_disconnect();
extern "C" bool __wrap_tud_disconnect() {
    RecordUsbTaskMarker(0xFFU);
    return __real_tud_disconnect();
}

extern "C" bool __real_tud_connect();
extern "C" bool __wrap_tud_connect() {
    RecordUsbTaskMarker(0xFEU);
    return __real_tud_connect();
}

extern "C" void __real_tud_mount_cb();
extern "C" void __wrap_tud_mount_cb() {
    RecordUsbTaskMarker(0xFDU);
    __real_tud_mount_cb();
}

namespace firmware {

void RecordUsbBootPrecharge(const uint32_t nominalUs, const uint32_t pulses) {
    // Parameters precede the completed-loop timestamp. Publication bits remain
    // authoritative; a partial or out-of-range record cannot qualify a boot.
    if (nominalUs > 0x00FFFFFFU || pulses > 0x00FFFFFFU) {
        return;
    }
    RecordUsbProbe(0xF7000000U | nominalUs);
    RecordUsbProbe(0xF6000000U | pulses);
    RecordUsbTaskMarker(0xFBU);
}

void RecordUsbBootFirstNonzeroRender() {
    RecordUsbTaskMarker(0xF5U);
}

void RecordUsbBootServicesReady() {
    RecordUsbTaskMarker(0xF4U);
}

void PrintUsbBootProbe(fxe::Console& console) {
    const auto count = g_usbProbeCount.load(std::memory_order_acquire);
    const auto retained = count < USB_PROBE_WORDS ? count : USB_PROBE_WORDS;
    const auto published0 = g_usbProbePublished[0].load(std::memory_order_acquire);
    const auto published1 = g_usbProbePublished[1].load(std::memory_order_acquire);
    console.PrintLn("debug_usb_b5f1: published=%08lx/%08lx setup=%08lx/%08lx",
                    static_cast<unsigned long>(published0), static_cast<unsigned long>(published1),
                    static_cast<unsigned long>(g_usbProbeSetup[0].load(std::memory_order_acquire)),
                    static_cast<unsigned long>(g_usbProbeSetup[1].load(std::memory_order_acquire)));
    console.PrintLn(
        "debug_usb_b5f1: seen=%lu retained=%lu dcfg=%08lx dctl=%08lx gintsts=%08lx gintmsk=%08lx",
        static_cast<unsigned long>(count), static_cast<unsigned long>(retained),
        static_cast<unsigned long>(USB0.dcfg),
        static_cast<unsigned long>(USB0.dctl),
        static_cast<unsigned long>(USB0.gintsts),
        static_cast<unsigned long>(USB0.gintmsk));
    // These reads neither pop a FIFO nor acknowledge an interrupt. Values may
    // change between loads; counters/flags/registers are not one atomic state.
    console.PrintLn(
        "debug_usb_b5f1: in84=%u/%lu/%lu/%lu/%u/%lu/%08lx/%lu/%08lx/%lu",
        usbd_edpt_busy(0U, 0x84U) ? 1U : 0U,
        static_cast<unsigned long>(tud_cdc_n_write_available(0U)),
        static_cast<unsigned long>(g_usbProbeInArmed.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_usbProbeInRejected.load(std::memory_order_relaxed)),
        static_cast<unsigned>(static_cast<uint16_t>(g_usbProbeInArmLength.load(std::memory_order_relaxed))),
        static_cast<unsigned long>(g_usbProbeInCompleted.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_usbProbeInComplete.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_usbProbeClearSeen.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_usbProbeClearIndex.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_usbProbeClearApplied.load(std::memory_order_relaxed)));
    console.PrintLn(
        "debug_usb_b5f1: ep84=%08lx/%08lx/%08lx/%08lx/%08lx/%08lx/%08lx/%08lx/%08lx",
        static_cast<unsigned long>(USB0.in_ep_reg[4].diepctl),
        static_cast<unsigned long>(USB0.in_ep_reg[4].diepint),
        static_cast<unsigned long>(USB0.in_ep_reg[4].dieptsiz),
        static_cast<unsigned long>(USB0.in_ep_reg[4].dtxfsts),
        static_cast<unsigned long>(USB0.diepmsk),
        static_cast<unsigned long>(USB0.daint),
        static_cast<unsigned long>(USB0.daintmsk),
        static_cast<unsigned long>(USB0.dtknqr4_fifoemptymsk),
        static_cast<unsigned long>(USB0.gahbcfg));
    PrintUsbIrqProbe(console);
    for (unsigned index = 0U; index < retained; index += 8U) {
        unsigned long words[8]{};
        for (unsigned offset = 0U; offset < 8U && index + offset < retained; ++offset) {
            words[offset] = g_usbProbeWords[index + offset].load(std::memory_order_acquire);
        }
        console.PrintLn("debug_usb_b5f1: w=%u %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                        index, words[0], words[1], words[2], words[3],
                        words[4], words[5], words[6], words[7]);
    }
}

} // namespace firmware

#endif
