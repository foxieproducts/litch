#include "firmware/hardware_test_led_frame.hpp"

#include "hardware/led_mapping.hpp"
#include "modules/ui_color_wheel.hpp"

namespace firmware {

namespace {

void FillRainbowFrame(const HardwareTestLedFrameInputs& inputs,
                      const uint8_t divisor,
                      HardwareTestLedFrame& frame) {
    const uint8_t phase = static_cast<uint8_t>((inputs.nowMs / divisor) & 0xFFU);
    for (uint16_t logical = 0U; logical < hardware::BOARD_LED_COUNT; ++logical) {
        const uint8_t hue = static_cast<uint8_t>(phase + logical);
        const uint16_t physical = hardware::MapLedIndexForBoardRotate(
            logical, inputs.boardRotate);
        frame[physical] = modules::ui_color_wheel::WheelToRgb(hue);
    }
}

void FillButtonFrame(const HardwareTestLedFrameInputs& inputs,
                     HardwareTestLedFrame& frame) {
    const uint8_t phase = static_cast<uint8_t>((inputs.nowMs / 16U) & 0xFFU);
    for (uint16_t logical = 0U; logical < hardware::BOARD_LED_COUNT; ++logical) {
        const uint8_t hue = static_cast<uint8_t>(phase + logical * 2U);
        const modules::Rgb color = modules::ui_color_wheel::WheelToRgb(hue);
        const uint16_t physical = hardware::MapLedIndexForBoardRotate(
            logical, inputs.boardRotate);
        frame[physical] = modules::ScaleRgbSaturatingBy256(color, 64U);
    }

}

void FillStatusButtonFrame(const HardwareTestLedFrameInputs& inputs,
                           HardwareTestStatusFrame& frame) {
    const uint8_t phase = static_cast<uint8_t>((inputs.nowMs / 16U) & 0xFFU);
    for (uint8_t logical = 0U; logical < modules::EDGE_LED_COUNT; ++logical) {
        const uint8_t hue = static_cast<uint8_t>(
            phase + (hardware::BOARD_LED_COUNT + logical) * 2U);
        frame[logical] = modules::ScaleRgbSaturatingBy256(
            modules::ui_color_wheel::WheelToRgb(hue), 64U);
    }

    constexpr std::array<modules::UserButton, 8> BUTTONS = {
        modules::UserButton::Up,
        modules::UserButton::Down,
        modules::UserButton::Left,
        modules::UserButton::Right,
        modules::UserButton::P1X,
        modules::UserButton::P1O,
        modules::UserButton::P2X,
        modules::UserButton::P2O,
    };
    constexpr std::array<uint8_t, 8> HUES = {
        0U, 32U, 64U, 96U, 160U, 192U, 224U, 16U};
    for (uint8_t offset = 0U; offset < BUTTONS.size(); ++offset) {
        if (!inputs.IsButtonPressed(BUTTONS[offset]) ||
            offset >= modules::EDGE_LED_COUNT) {
            continue;
        }
        frame[offset] = modules::ui_color_wheel::WheelToRgb(HUES[offset]);
    }
}

void FillStatusRainbowFrame(const HardwareTestLedFrameInputs& inputs,
                            const uint8_t divisor,
                            HardwareTestStatusFrame& frame) {
    const uint8_t phase = static_cast<uint8_t>((inputs.nowMs / divisor) & 0xFFU);
    for (uint8_t logical = 0U; logical < modules::EDGE_LED_COUNT; ++logical) {
        frame[logical] = modules::ui_color_wheel::WheelToRgb(
            static_cast<uint8_t>(phase + hardware::BOARD_LED_COUNT + logical));
    }
}

void FillHallFrame(const HardwareTestLedFrameInputs& inputs,
                   HardwareTestLedFrame& frame) {
    for (uint8_t square = 0U; square < hardware::SQUARE_COUNT; ++square) {
        if (((inputs.occupiedMask >> square) & 1ULL) == 0ULL) {
            continue;
        }

        const uint16_t base = hardware::LedIndexForSquare(
            square, hardware::SquareLedSlot::Square0);
        for (uint8_t slot = 0U; slot < hardware::LEDS_PER_SQUARE; ++slot) {
            const uint16_t logical = static_cast<uint16_t>(base + slot);
            const uint16_t physical = hardware::MapLedIndexForBoardRotate(
                logical, inputs.boardRotate);
            frame[physical] = modules::RGB_WHITE;
        }
    }
}

} // namespace

modules::Rgb WhiteChannelTestRgb(const uint8_t intensity) {
    constexpr uint16_t WARM_BLUE_SCALE256 = 224U;
    return modules::Rgb{intensity, intensity,
                        modules::ScaleU8By256(intensity, WARM_BLUE_SCALE256)};
}

HardwareTestLedFrame ComposeHardwareTestLedFrame(
    const HardwareTestLedFrameInputs& inputs) {
    HardwareTestLedFrame frame{};
    if (inputs.step == HardwareTestMode::Step::LedRainbow) {
        FillRainbowFrame(inputs, 12U, frame);
    }
    if (inputs.step == HardwareTestMode::Step::LightSensors) {
        FillRainbowFrame(inputs, 24U, frame);
    }
    if (inputs.step == HardwareTestMode::Step::Buttons) {
        FillButtonFrame(inputs, frame);
    }
    if (inputs.step == HardwareTestMode::Step::HallSensors) {
        FillHallFrame(inputs, frame);
    }
    if (inputs.step == HardwareTestMode::Step::WhiteChannel) {
        const uint16_t phase = static_cast<uint16_t>((inputs.nowMs / 6U) & 0x1FFU);
        const uint8_t triangle = (phase < 256U)
                                     ? static_cast<uint8_t>(phase)
                                     : static_cast<uint8_t>(511U - phase);
        for (uint16_t led = 0U; led < hardware::BOARD_LED_COUNT; ++led) {
            frame[led] = WhiteChannelTestRgb(triangle);
        }
    }
    return frame;
}

HardwareTestStatusFrame ComposeHardwareTestStatusFrame(
    const HardwareTestLedFrameInputs& inputs) {
    HardwareTestStatusFrame frame{};
    if (inputs.step == HardwareTestMode::Step::LedRainbow) {
        FillStatusRainbowFrame(inputs, 12U, frame);
    }
    if (inputs.step == HardwareTestMode::Step::LightSensors) {
        FillStatusRainbowFrame(inputs, 24U, frame);
    }
    if (inputs.step == HardwareTestMode::Step::Buttons) {
        FillStatusButtonFrame(inputs, frame);
    }
    if (inputs.step == HardwareTestMode::Step::WhiteChannel) {
        const uint16_t phase = static_cast<uint16_t>((inputs.nowMs / 6U) & 0x1FFU);
        const uint8_t triangle = (phase < 256U)
                                     ? static_cast<uint8_t>(phase)
                                     : static_cast<uint8_t>(511U - phase);
        frame.fill(WhiteChannelTestRgb(triangle));
    }
    return frame;
}

} // namespace firmware
