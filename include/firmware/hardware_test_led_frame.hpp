#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "firmware/hardware_test_mode.hpp"
#include "hardware/led_layout.hpp"
#include "modules/edge_led_constants.hpp"
#include "modules/rgb.hpp"

namespace firmware {

inline constexpr size_t HARDWARE_TEST_BUTTON_COUNT =
    static_cast<size_t>(modules::UserButton::P2O) + 1U;

/**
 * \brief Build a warm near-neutral RGB stimulus for the S3 white-die test.
 * \note The normal renderer maps this stimulus predominantly onto the W die.
 */
modules::Rgb WhiteChannelTestRgb(uint8_t intensity);

using HardwareTestLedFrame =
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>;
using HardwareTestStatusFrame =
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>;

struct HardwareTestLedFrameInputs {
    HardwareTestMode::Step step = HardwareTestMode::Step::LedRainbow;
    bool boardRotate = false;
    uint32_t nowMs = 0U;
    uint64_t occupiedMask = 0ULL;
    std::array<bool, HARDWARE_TEST_BUTTON_COUNT> pressedButtons{};

    bool IsButtonPressed(const modules::UserButton button) const {
        const size_t index = static_cast<size_t>(button);
        return index < pressedButtons.size() && pressedButtons[index];
    }
};

/**
 * \brief Compose one physical-order frame for boot hardware-test mode.
 */
HardwareTestLedFrame ComposeHardwareTestLedFrame(
    const HardwareTestLedFrameInputs& inputs);
HardwareTestStatusFrame ComposeHardwareTestStatusFrame(
    const HardwareTestLedFrameInputs& inputs);

} // namespace firmware
