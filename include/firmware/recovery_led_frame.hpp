#pragma once

#include <array>
#include <cstdint>

#include "firmware/recovery_policy.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/edge_led_constants.hpp"
#include "modules/rgb.hpp"

namespace firmware {

using RecoveryLedFrame =
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>;

// Recovery deliberately remains below both the S3 conservative 216 mA LED
// headroom and this tighter surface-specific ceiling.
inline constexpr uint16_t RECOVERY_LED_CURRENT_CAP_MA = 120U;

/**
 * \brief Compose the canonical 32-LED semantic recovery status frame.
 * \note The caller maps this frame to a dedicated chain or virtual perimeter.
 */
RecoveryLedFrame ComposeRecoveryLedFrame(
    const RecoverySnapshot& snapshot,
    uint32_t nowMs);

/** \brief Project the cold-boot face-button cue onto the fitted LED layout. */
hardware::PhysicalLedFrame ComposeFactoryResetControlFrame(
    hardware::BoardRevision revision, bool hasP2Controls, modules::Rgb color);

} // namespace firmware
