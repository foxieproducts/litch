#ifdef ARDUINO
#include "firmware/firmware_internal.hpp"
#include "firmware/usb_boot_probe.hpp"
#include "logger.hpp"
namespace firmware_internal {
void RecordBootRenderReturn(FirmwareSystem& sys, const fxe::ElapsedTime& render,
    const uint16_t scale256) {
    if (sys.bootFirstNonzeroRenderLogged || !sys.startupLeds.Fade().IsStarted() ||
        sys.ledPowerStats.appliedLedCurrentMa == 0U ||
        !(sys.ledAnimation.IsStarted() ? sys.ledAnimation.Receipt().completed : LedOutputCompleted(sys))) return;
    sys.bootFirstNonzeroRenderLogged = true;
#if defined(LITCH_HW_VARIANT_S3)
    firmware::RecordUsbBootFirstNonzeroRender();
#endif
    logger::Trace("boot: first_nonzero_render_return_ms=%lu render_start_ms=%lu effective_scale_256=%u estimated_current_ma=%u",
        static_cast<unsigned long>(fxe::Millis()), static_cast<unsigned long>(render.StartMs()),
        static_cast<unsigned>(scale256), static_cast<unsigned>(sys.ledPowerStats.appliedLedCurrentMa));
}
} // namespace firmware_internal
#endif
