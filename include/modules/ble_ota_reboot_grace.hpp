#pragma once

#include <cstdint>

namespace modules {

inline constexpr uint32_t BLE_OTA_REBOOT_NOTIFY_GRACE_MS = 500U;
inline constexpr uint32_t BLE_OTA_REBOOT_DISCONNECT_GRACE_MS = 250U;

inline bool IsBleOtaRebootGraceElapsed(const uint32_t nowMs,
                                       const uint32_t armedAtMs) {
    return static_cast<uint32_t>(nowMs - armedAtMs) >=
           BLE_OTA_REBOOT_NOTIFY_GRACE_MS;
}

} // namespace modules
