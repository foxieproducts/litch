#pragma once

#include <cstdint>

namespace firmware {

// Startup SDK boundary samples in the firmware clock domain, not an external
// power-on timebase. An unobserved hook must not become a duration origin.
struct BootPlatformTiming {
    static constexpr uint32_t UNOBSERVED_MS = UINT32_MAX;
    uint32_t appMainBeginMs = UNOBSERVED_MS;
    uint32_t usbBeginMs = UNOBSERVED_MS;
    uint32_t usbEndMs = UNOBSERVED_MS;
    uint32_t arduinoBeginMs = UNOBSERVED_MS;
    uint32_t arduinoEndMs = UNOBSERVED_MS;
    uint32_t setupBeginMs = UNOBSERVED_MS;
    uint32_t watchdogEndMs = UNOBSERVED_MS;
};

BootPlatformTiming& PlatformBootTiming();

} // namespace firmware
