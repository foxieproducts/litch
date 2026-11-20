#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3)

#include <esp_err.h>

#include "esp32-hal-tinyusb.h"

#include "firmware/usb_device_power_descriptor.hpp"
#include "firmware/boot_platform_timing.hpp"
#include "foxie_embedded/millis.hpp"

extern "C" esp_err_t __real_tinyusb_init(tinyusb_device_config_t* config);

extern "C" esp_err_t __wrap_tinyusb_init(tinyusb_device_config_t* config) {
    firmware::PlatformBootTiming().usbBeginMs = fxe::Millis();
    if (config != nullptr) {
        config->usb_attributes =
            firmware::S3_USB_DEVICE_POWER_DESCRIPTOR.attributes;
        config->usb_power_ma =
            firmware::S3_USB_DEVICE_POWER_DESCRIPTOR.configuredMaxPowerMa;
    }
    const esp_err_t result = __real_tinyusb_init(config);
    firmware::PlatformBootTiming().usbEndMs = fxe::Millis();
    return result;
}

#endif
