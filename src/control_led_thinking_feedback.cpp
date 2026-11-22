#include "modules/control_led_thinking_feedback.hpp"

#include "hardware/status_led_chain_profile.hpp"
#include "modules/ui_color_wheel.hpp"
#include "modules/animation_timing.hpp"

namespace modules {
namespace {
using Role = hardware::ControlLedRole;
constexpr uint32_t STEP_MS = 200U;
constexpr uint32_t BEACHBALL_CYCLE_MS = 2000U;
constexpr uint16_t BEACHBALL_FLOOR_256 = 32U;
constexpr Rgb FOXIE_ORANGE{255U, 128U, 0U};
constexpr Rgb FOXIE_PURPLE{128U, 0U, 255U};
constexpr Role SWEEP[]{Role::X, Role::Left, Role::Right, Role::O,
                       Role::Right, Role::Left};
constexpr Role CLOCKWISE[]{Role::Up, Role::Right, Role::Down, Role::Left};
constexpr uint8_t EMULATED_SWEEP[]{0U, 1U, 2U, 3U, 2U, 1U};
Rgb FoxieColor(const uint32_t step) {
    // Invert each full pass so a fitted LED is not tied to one color.
    return (step + step / 6U) % 2U == 0U ? FOXIE_PURPLE : FOXIE_ORANGE;
}
void OwnEmulatedBank(status_led_frame_adapter::FixedPhysicalStatusOverlay& out, UiOwner owner) {
    out.emulatedControlMask |= owner == UiOwner::P2 ? 0xF0U : 0x0FU;
}
void SetEmulated(status_led_frame_adapter::FixedPhysicalStatusOverlay& out,
                 UiOwner owner, uint8_t position, Rgb color) {
    out.emulatedControlPixels[(owner == UiOwner::P2 ? 4U : 0U) + position] = color;
}
void Set(status_led_frame_adapter::FixedPhysicalStatusOverlay& out,
         UiOwner owner, Role role, Rgb color) {
    const uint8_t bank = owner == UiOwner::P2 ? 6U : 0U;
    const uint8_t index = bank + static_cast<uint8_t>(role);
    out.controlPixels[index] = color;
    out.controlMask |= static_cast<uint16_t>(1U << index);
}
} // namespace

void ComposeControlLedColorPreview(const UiOwner owner, const Rgb color,
    status_led_frame_adapter::FixedPhysicalStatusOverlay& output) {
    if (owner == UiOwner::None) return;
    for (uint8_t role = 0U; role < 6U; ++role) {
        Set(output, owner, static_cast<Role>(role), color);
    }
    output.controlAlpha255[owner == UiOwner::P2 ? 1U : 0U] = 255U;
    OwnEmulatedBank(output, owner);
    for (uint8_t position = 0U; position < 4U; ++position)
        SetEmulated(output, owner, position, color);
}

void ComposeControlLedThinkingFeedback(
    const ControlLedThinkingFeedback& feedback,
    status_led_frame_adapter::FixedPhysicalStatusOverlay& output) {
    if (feedback.owner == UiOwner::None ||
        feedback.style == hint_thinking_style_setting::Style::None) return;
    if (feedback.style == hint_thinking_style_setting::Style::CauldronBrewing) {
        const auto& token = animation::LED_FRAME_TOKENS.hintCauldronBreath;
        const uint16_t value = feedback.minimalAnimations ? token.curve.minValue
            : animation::SampleValue({feedback.elapsedMs}, token);
        const uint16_t scale = static_cast<uint16_t>((value * 256U + 127U) / 255U);
        Set(output, feedback.owner, Role::O,
            ScaleRgbSaturatingBy256(feedback.playerColor, scale));
        output.controlAlpha255[feedback.owner == UiOwner::P2 ? 1U : 0U] =
            static_cast<uint8_t>((feedback.scale256 * 255U + 128U) / 256U);
        return;
    }
    output.controlAlpha255[feedback.owner == UiOwner::P2 ? 1U : 0U] =
        static_cast<uint8_t>((feedback.scale256 * 255U + 128U) / 256U);
    OwnEmulatedBank(output, feedback.owner);
    if (feedback.style == hint_thinking_style_setting::Style::Beachball) {
        const uint32_t elapsed = feedback.elapsedMs % BEACHBALL_CYCLE_MS;
        const uint8_t head = static_cast<uint8_t>(elapsed / (BEACHBALL_CYCLE_MS / 4U));
        const auto hue = static_cast<uint8_t>(elapsed * 256U / BEACHBALL_CYCLE_MS);
        const uint16_t fraction = static_cast<uint16_t>((elapsed % (BEACHBALL_CYCLE_MS / 4U)) * (256U - BEACHBALL_FLOOR_256) /
                                                        (BEACHBALL_CYCLE_MS / 4U));
        for (uint8_t position = 0U; position < 4U; ++position) {
            const auto shown = feedback.minimalAnimations
                ? ScaleRgbSaturatingBy256(ui_color_wheel::WheelToRgb(
                      static_cast<uint8_t>(position * 64U)), 128U)
                : ScaleRgbSaturatingBy256(ui_color_wheel::WheelToRgb(
                      static_cast<uint8_t>(position * 64U + hue)),
                      position == head ? 256U - fraction :
                      position == (head + 1U) % 4U ? BEACHBALL_FLOOR_256 + fraction : BEACHBALL_FLOOR_256);
            Set(output, feedback.owner, CLOCKWISE[position], shown);
            SetEmulated(output, feedback.owner, position, shown);
        }
        return;
    }
    const bool clockwise = feedback.style ==
        hint_thinking_style_setting::Style::PlayerClockwise;
    const uint32_t count = clockwise ? 4U : 6U;
    const uint32_t step = feedback.elapsedMs / STEP_MS;
    const uint32_t index = step % count;
    const Rgb color = clockwise ? feedback.playerColor : FoxieColor(step);
    if (feedback.minimalAnimations) {
        if (clockwise) {
            for (const auto role : CLOCKWISE) Set(output, feedback.owner, role,
                ScaleRgbSaturatingBy256(color, 128U));
        } else {
            Set(output, feedback.owner, Role::X, FOXIE_PURPLE);
            Set(output, feedback.owner, Role::Left, FOXIE_ORANGE);
            Set(output, feedback.owner, Role::Right, FOXIE_PURPLE);
            Set(output, feedback.owner, Role::O, FOXIE_ORANGE);
        }
        for (uint8_t position = 0U; position < 4U; ++position)
            SetEmulated(output, feedback.owner, position, clockwise
                ? ScaleRgbSaturatingBy256(color, 128U)
                : (position % 2U == 0U ? FOXIE_PURPLE : FOXIE_ORANGE));
        return;
    }
    // Cross-fade adjacent positions; no frame depends on render frequency.
    const uint16_t nextScale = static_cast<uint16_t>(
        (feedback.elapsedMs % STEP_MS) * 256U / STEP_MS);
    const auto* roles = clockwise ? CLOCKWISE : SWEEP;
    const Rgb nextColor = clockwise ? color : FoxieColor(step + 1U);
    Set(output, feedback.owner, roles[index],
        ScaleRgbSaturatingBy256(color, 256U - nextScale));
    Set(output, feedback.owner, roles[(index + 1U) % count],
        ScaleRgbSaturatingBy256(nextColor, nextScale));
    SetEmulated(output, feedback.owner, clockwise ? index : EMULATED_SWEEP[index],
        ScaleRgbSaturatingBy256(color, 256U - nextScale));
    const uint32_t next = (index + 1U) % count;
    SetEmulated(output, feedback.owner, clockwise ? next : EMULATED_SWEEP[next],
        ScaleRgbSaturatingBy256(nextColor, nextScale));
}
} // namespace modules
