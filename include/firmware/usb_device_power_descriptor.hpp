#pragma once

#include <cstdint>

namespace firmware {

struct UsbDevicePowerDescriptorPolicy {
    uint8_t attributes = 0U;
    uint16_t configuredMaxPowerMa = 0U;
};

// The S3 is powered by VBUS. The 500 mA value is the USB 2.0 configured
// high-power request encoded in bMaxPower; it is not permission to draw that
// current before configuration or while the bus is suspended.
inline constexpr UsbDevicePowerDescriptorPolicy S3_USB_DEVICE_POWER_DESCRIPTOR{
    0U,
    500U,
};

} // namespace firmware
