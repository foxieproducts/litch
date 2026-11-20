#pragma once

#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3) && \
    defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)

#include <tusb.h>

#include <cstddef>
#include <cstdint>

namespace firmware_internal {

// Only the cooperative console owner writes CDC interface 0. TinyUSB provides
// FIFO synchronization; these calls never wait for the host to drain TX.
// Arduino still owns the RX callback and bootloader line-coding callbacks.
class S3UsbConsoleDevice final {
  public:
    bool Ready() const { return tud_ready(); }
    uint8_t LineState() const { return tud_cdc_n_get_line_state(0U); }
    size_t AvailableForWrite() const {
        const size_t capacity = tud_cdc_n_write_available(0U);
        if (capacity != 0U || !tud_ready()) return capacity;
        // A suspend can skip the last flush after FIFO admission. Resume
        // does not pump that FIFO, so service it before waiting for capacity.
        // One attempt reuses the SDK's existing endpoint-claim mutex; it
        // never loops waiting for the host or resets a busy endpoint.
        (void)tud_cdc_n_write_flush(0U);
        return tud_cdc_n_write_available(0U);
    }
    size_t Write(const uint8_t* data, const size_t size) {
        return tud_cdc_n_write(0U, data, static_cast<uint32_t>(size));
    }
    void Flush() { (void)tud_cdc_n_write_flush(0U); }
};

} // namespace firmware_internal

#endif
