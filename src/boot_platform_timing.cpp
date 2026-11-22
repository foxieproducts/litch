#include "firmware/boot_platform_timing.hpp"
#include "foxie_embedded/millis.hpp"
#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3)
#include <esp_timer.h>
#endif

namespace firmware {
namespace {
// Constant-initialized platform storage. Early SDK entries write scalar
// samples directly; no application owner or local-static construction runs.
BootPlatformTiming g_platformBootTiming{};
} // namespace

BootPlatformTiming& PlatformBootTiming() {
    // Written only by ordered SDK initialization hooks and the startup task.
    // No runtime worker mutates this platform-boundary record.
    return g_platformBootTiming;
}

} // namespace firmware

#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3)
// Both entries run after SDK/C++ startup. Observe their existing order without
// logging, constructing the firmware system, or changing USB/NVS/OTA policy.
extern "C" void __real_app_main();
extern "C" void __real_initArduino();

extern "C" void __wrap_app_main() {
    firmware::g_platformBootTiming.appMainBeginMs =
        static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    __real_app_main();
}

extern "C" void __wrap_initArduino() {
    firmware::g_platformBootTiming.arduinoBeginMs =
        static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    __real_initArduino();
    firmware::g_platformBootTiming.arduinoEndMs =
        static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}
#elif defined(ARDUINO)
// Arduino's weak variant hook runs after CPU/PSRAM/NVS initialization and
// before creating loopTask. Litch variants otherwise supply pin definitions.
extern "C" void initVariant() {
    firmware::PlatformBootTiming().arduinoEndMs = fxe::Millis();
}
#endif
