#include <array>
#include <cstdio>

#include "hardware/led_layout.hpp"
#include "modules/status_led_frame_adapter.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

bool RgbEqual(const modules::Rgb lhs, const modules::Rgb rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

} // namespace

int main() {
    std::puts("[STATUS_LED_FRAME_ADAPTER] start");

    std::array<modules::Rgb, modules::EDGE_LED_COUNT> semantic{};
    for (uint8_t index = 0U; index < semantic.size(); ++index) {
        semantic[index] = modules::Rgb{
            static_cast<uint8_t>(index + 1U),
            static_cast<uint8_t>(255U - index),
            static_cast<uint8_t>(index * 3U),
        };
    }

    if constexpr (hardware::EDGE_LED_COUNT == modules::EDGE_LED_COUNT) {
        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> dedicated{};
        modules::status_led_frame_adapter::ApplyToDedicatedChain(
            semantic, false, dedicated);
        for (uint8_t index = 0U; index < semantic.size(); ++index) {
            const uint16_t physical = static_cast<uint16_t>(
                hardware::FirstEdgeLedIndex() + index);
            Check(RgbEqual(dedicated[physical], semantic[index]),
                  "unrotated dedicated chain preserves semantic indices");
        }

        dedicated.fill(modules::RGB_BLACK);
        modules::status_led_frame_adapter::ApplyToDedicatedChain(
            semantic, true, dedicated);
        for (uint8_t index = 0U; index < semantic.size(); ++index) {
            const uint8_t rotated = static_cast<uint8_t>(
                (index + modules::EDGE_LED_COUNT / 2U) %
                modules::EDGE_LED_COUNT);
            const uint16_t physical = static_cast<uint16_t>(
                hardware::FirstEdgeLedIndex() + rotated);
            Check(RgbEqual(dedicated[physical], semantic[index]),
                  "BOARD_ROTATE rotates the dedicated chain by half a ring");
        }
    }

    std::array<bool, hardware::BOARD_LED_COUNT> claimed{};
    for (uint8_t index = 0U; index < semantic.size(); ++index) {
        const auto target =
            modules::status_led_frame_adapter::VirtualTargetForPhysicalIndex(
                index);
        const uint16_t led = hardware::LedIndexForSquare(
            target.square, target.slot);
        Check(led < claimed.size(), "virtual target stays on the board chain");
        Check(!claimed[led], "all 32 virtual status targets are collision-free");
        claimed[led] = true;
    }

    const auto northH8 =
        modules::status_led_frame_adapter::VirtualTargetForPhysicalIndex(7U);
    const auto eastH8 =
        modules::status_led_frame_adapter::VirtualTargetForPhysicalIndex(8U);
    const auto southH1 =
        modules::status_led_frame_adapter::VirtualTargetForPhysicalIndex(16U);
    const auto eastH1 =
        modules::status_led_frame_adapter::VirtualTargetForPhysicalIndex(15U);
    const auto southA1 =
        modules::status_led_frame_adapter::VirtualTargetForPhysicalIndex(23U);
    const auto westA1 =
        modules::status_led_frame_adapter::VirtualTargetForPhysicalIndex(24U);
    const auto westA8 =
        modules::status_led_frame_adapter::VirtualTargetForPhysicalIndex(31U);
    const auto northA8 =
        modules::status_led_frame_adapter::VirtualTargetForPhysicalIndex(0U);
    Check(northH8.square == 7U && eastH8.square == 7U &&
              northH8.slot != eastH8.slot,
          "h8 corner uses two distinct square LED slots");
    Check(southH1.square == 63U && eastH1.square == 63U &&
              southH1.slot != eastH1.slot,
          "h1 corner uses two distinct square LED slots");
    Check(southA1.square == 56U && westA1.square == 56U &&
              southA1.slot != westA1.slot,
          "a1 corner uses two distinct square LED slots");
    Check(westA8.square == 0U && northA8.square == 0U &&
              westA8.slot != northA8.slot,
          "a8 corner uses two distinct square LED slots");

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> virtualFrame{};
    virtualFrame.fill(modules::RGB_BLACK);
    modules::status_led_frame_adapter::ApplyToVirtualPerimeter(
        semantic, true, virtualFrame);
    for (uint8_t semanticIndex = 0U;
         semanticIndex < semantic.size();
         ++semanticIndex) {
        const uint8_t physicalIndex = static_cast<uint8_t>(
            (semanticIndex + modules::EDGE_LED_COUNT / 2U) %
            modules::EDGE_LED_COUNT);
        const auto target =
            modules::status_led_frame_adapter::VirtualTargetForPhysicalIndex(
                physicalIndex);
        const uint16_t led = hardware::LedIndexForSquare(
            target.square, target.slot);
        Check(!RgbEqual(virtualFrame[led], modules::RGB_BLACK),
              "rotated virtual status intent reaches its physical target");
    }

    const modules::Rgb preservedBase{80U, 10U, 0U};
    Check(RgbEqual(
              modules::status_led_frame_adapter::BlendVirtualStatus(
                  preservedBase, modules::RGB_BLACK),
              preservedBase),
          "an unlit virtual status pixel preserves the square cue");
    const modules::Rgb blended =
        modules::status_led_frame_adapter::BlendVirtualStatus(
            preservedBase, modules::Rgb{0U, 0U, 255U});
    Check(blended.r != 0U && blended.b != 0U,
          "virtual status blend remains visible without erasing square semantics");

    if (g_failures == 0) {
        std::puts("[STATUS_LED_FRAME_ADAPTER] ok");
        return 0;
    }

    std::printf("[STATUS_LED_FRAME_ADAPTER] failures=%d\n", g_failures);
    return 1;
}
