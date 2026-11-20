// Build the pinned SDK device core with the S3 application's USB class profile.
// CDC, runtime DFU detach and the ROM download handoff retain their SDK owners.
#if defined(LITCH_ESP32S3)
#include <sdkconfig.h>

#if ARDUINO_USB_MSC_ON_BOOT || ARDUINO_USB_DFU_ON_BOOT
#error "The Litch S3 application USB profile requires CDC-only boot registration"
#endif

// The pinned Arduino SDK ships all classes in its precompiled driver table.
// Override these two Kconfig switches locally before TinyUSB reads sdkconfig.
#undef CONFIG_TINYUSB_MSC_ENABLED
#define CONFIG_TINYUSB_MSC_ENABLED 0
#undef CONFIG_TINYUSB_DFU_ENABLED
#define CONFIG_TINYUSB_DFU_ENABLED 0

#include "../tools/pio/tinyusb/usbd.c"
#endif
