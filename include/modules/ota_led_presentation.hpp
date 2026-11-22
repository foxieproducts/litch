#pragma once

#include <array>
#include <cstdint>

#include "hardware/board_led_output_mapping.hpp"
#include "modules/rgb.hpp"

namespace modules {

// A projection of the BLE transaction owner, not another update lifecycle.
struct OtaLedPresentation {
    bool active = false;
    uint8_t percent = 0U;
};

inline constexpr uint32_t OTA_LED_REFRESH_MS = 200U;
inline constexpr uint16_t OTA_LED_MINIMUM_SCALE256 = 32U;

constexpr uint16_t OtaLedBrightnessScale(const uint16_t requestedScale) {
    return requestedScale < OTA_LED_MINIMUM_SCALE256
               ? OTA_LED_MINIMUM_SCALE256 : requestedScale;
}

constexpr OtaLedPresentation ProjectOtaLedPresentation(
    const bool transactionActive, const bool simulated,
    const bool rebootPending, const uint8_t percent) {
    return {!simulated && (transactionActive || rebootPending),
            static_cast<uint8_t>(percent > 100U ? 100U : percent)};
}

inline constexpr uint32_t OTA_LED_FINALIZATION_REFRESH_MS = 20U;
inline constexpr uint32_t BULK_LED_REFRESH_MS = 100U;

constexpr uint32_t OtaLedRefreshMs(const OtaLedPresentation presentation) {
    // The settings worker yields each flash access to a completed LED frame.
    // Once image bytes are complete, the faster cadence lets its durable
    // snapshot finish inside OTA's bounded persistence gate.
    return presentation.active && presentation.percent >= 100U
        ? OTA_LED_FINALIZATION_REFRESH_MS : OTA_LED_REFRESH_MS;
}

constexpr uint32_t RuntimeLedRefreshMs(const OtaLedPresentation ota,
    const bool bulk, const uint32_t normalMs, const bool otaInProgress = false) {
    return ota.active ? OtaLedRefreshMs(ota)
        : bulk ? BULK_LED_REFRESH_MS
        : otaInProgress ? OtaLedRefreshMs(ota) : normalMs;
}

// Mandatory update feedback ignores cosmetic animation and orientation
// settings. Callers map the semantic board once, add fixed-physical status
// progress, and apply the conservative power/RMT exclusion policy.
void ComposeOtaLedPresentation(
    OtaLedPresentation presentation, BoardLedFrame& board);

void ApplyOtaPhysicalStatusPresentation(
    OtaLedPresentation presentation, hardware::BoardRevision revision,
    hardware::PhysicalLedFrame& physical);

} // namespace modules
