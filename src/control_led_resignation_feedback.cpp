#include "modules/control_led_resignation_feedback.hpp"

#include "hardware/status_led_chain_profile.hpp"

namespace modules {
namespace {
using Role = hardware::ControlLedRole;
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

void ComposeControlLedResignationFeedback(const UiOwner owner, const uint32_t elapsedMs,
    const Rgb playerColor, status_led_frame_adapter::FixedPhysicalStatusOverlay& output) {
    if (owner == UiOwner::None) return;
    constexpr uint32_t HOLD_MS = 5000U;
    const bool armed = elapsedMs >= HOLD_MS;
    const Rgb color = armed
        ? (((elapsedMs - HOLD_MS) / 500U) % 2U == 0U
            ? ScaleRgbSaturatingBy256(RGB_YELLOW, 160U) : Rgb{})
        : ScaleRgbSaturatingBy256(playerColor, elapsedMs * 128U / HOLD_MS);
    Set(output, owner, Role::O, color);
    output.controlAlpha255[owner == UiOwner::P2 ? 1U : 0U] = 255U;
    OwnEmulatedBank(output, owner);
    SetEmulated(output, owner, 3U, color);
}

} // namespace modules
