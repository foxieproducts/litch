#pragma once

#include <cstdint>

namespace modules {

inline constexpr uint8_t OTA_SLOT_0_SUBTYPE = 0x10U;
inline constexpr uint8_t OTA_SLOT_1_SUBTYPE = 0x11U;

/** Customer OTA alternates these slots; the validated running image is rollback. */
constexpr bool IsRollbackPreservingOtaRoute(const uint8_t runningSubtype,
                                            const uint8_t targetSubtype) {
    return (runningSubtype == OTA_SLOT_0_SUBTYPE &&
            targetSubtype == OTA_SLOT_1_SUBTYPE) ||
           (runningSubtype == OTA_SLOT_1_SUBTYPE &&
            targetSubtype == OTA_SLOT_0_SUBTYPE);
}

} // namespace modules
