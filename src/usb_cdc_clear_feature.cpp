#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3) && \
    defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)

#include <tusb.h>
#include <device/dcd.h>

extern "C" bool __real_cdcd_control_xfer_cb(
    uint8_t, uint8_t, const tusb_control_request_t*);

extern "C" bool __wrap_cdcd_control_xfer_cb(
    const uint8_t rhport, const uint8_t stage,
    const tusb_control_request_t* request) {
    // Arduino 2.0.17 fixes CDC0 to OUT03, IN84 and notification IN85. USBD
    // routes these requests here after its standard endpoint handling. Its
    // clear-stall guard skips the DCD call when software halt is already clear,
    // although USB 2.0 section 9.4.5 still requires DATA0 initialization.
    // Run only in this existing control-task callback; preserve busy/claimed
    // ownership and queued data. The DCD clear is bounded register writes.
    if (rhport == 0U && stage == CONTROL_STAGE_SETUP && request != nullptr &&
        request->bmRequestType == 0x02U &&
        request->bRequest == TUSB_REQ_CLEAR_FEATURE &&
        request->wValue == TUSB_REQ_FEATURE_EDPT_HALT &&
        request->wLength == 0U && tud_mounted() &&
        (request->wIndex == 0x03U || request->wIndex == 0x84U ||
         request->wIndex == 0x85U)) {
        // A previously halted endpoint has already been cleared by USBD;
        // repeating DATA0 here is idempotent within the same SETUP handling.
        dcd_edpt_clear_stall(rhport, static_cast<uint8_t>(request->wIndex));
    }
    return __real_cdcd_control_xfer_cb(rhport, stage, request);
}

#endif
