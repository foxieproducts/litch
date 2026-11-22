#pragma once

#include <array>
#include <cstdint>

#include "hardware/led_layout.hpp"
#include "modules/edge_led_constants.hpp"
#include "modules/rgb.hpp"

namespace modules::status_led_frame_adapter {

inline constexpr std::array<uint8_t, 2> USB_PORT_PHYSICAL_INDICES = {
    27U,
    28U,
};

struct FixedPhysicalStatusOverlay {
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> pixels{};
    uint32_t mask = 0U;
    // Default opaque keeps existing producers unchanged. Pairing alone uses
    // this to release its addressed perimeter pixels into the ordinary frame.
    uint8_t perimeterAlpha255 = 255U;
    // Logical player banks, O/Up/Right/Down/Left/X. Final mapping owns topology.
    std::array<modules::Rgb, 12> controlPixels{};
    uint16_t controlMask = 0U;
    // Same feedback sampled onto V3's middle four home LEDs, left-to-right.
    std::array<modules::Rgb, 8> emulatedControlPixels{};
    uint8_t emulatedControlMask = 0U;
    // Per logical bank so a thinking fade cannot dim the other palette preview.
    std::array<uint8_t, 2> controlAlpha255{255U, 255U};
    // A short upper accent blends over the complete lower control result.
    // Empty by default; pairing alone currently requests this layer.
    std::array<modules::Rgb, 12> controlAccentPixels{};
    uint16_t controlAccentMask = 0U;
    uint8_t controlAccentAlpha255 = 0U;
    // True only when the final semantic perimeter carries ordinary Turn.
    // Required/menu/fault output defaults to retaining its existing priority.
    bool ordinaryStatus = false;

    void Set(const uint8_t physicalIndex, const modules::Rgb color) {
        if (physicalIndex >= modules::EDGE_LED_COUNT) return;
        pixels[physicalIndex] = color;
        mask |= 1UL << physicalIndex;
        ordinaryStatus = false;
    }
};

struct VirtualStatusTarget {
    uint8_t square = 0U;
    hardware::SquareLedSlot slot = hardware::SquareLedSlot::Square0;
};

constexpr uint8_t PhysicalIndexForSemanticIndex(
    const uint8_t semanticIndex,
    const bool boardRotate) {
    if (!boardRotate) {
        return semanticIndex;
    }
    return static_cast<uint8_t>(
        (semanticIndex + modules::EDGE_LED_COUNT / 2U) %
        modules::EDGE_LED_COUNT);
}

// Retained for adapters that need the inverse ring lookup. Fixed hardware cues
// are now declared explicitly to the final output mapper instead.
constexpr uint8_t SemanticIndexForFixedPhysicalIndex(
    const uint8_t physicalIndex,
    const bool boardRotate) {
    return PhysicalIndexForSemanticIndex(physicalIndex, boardRotate);
}

constexpr VirtualStatusTarget VirtualTargetForPhysicalIndex(
    const uint8_t physicalIndex) {
    // Match the dedicated ring: top left-to-right, right top-to-bottom,
    // bottom right-to-left, then left bottom-to-top (0=a8, 63=h1).
    if (physicalIndex < 8U) {
        return VirtualStatusTarget{
            physicalIndex,
            hardware::SquareLedSlot::Square0,
        };
    }
    if (physicalIndex < 16U) {
        return VirtualStatusTarget{
            static_cast<uint8_t>(7U + (physicalIndex - 8U) * 8U),
            hardware::SquareLedSlot::Square2,
        };
    }
    if (physicalIndex < 24U) {
        return VirtualStatusTarget{
            static_cast<uint8_t>(63U - (physicalIndex - 16U)),
            hardware::SquareLedSlot::Square0,
        };
    }
    return VirtualStatusTarget{
        static_cast<uint8_t>(56U - (physicalIndex - 24U) * 8U),
        hardware::SquareLedSlot::Square2,
    };
}

constexpr uint16_t VirtualLedIndexForPhysicalStatusIndex(
    const uint8_t physicalIndex) {
    const VirtualStatusTarget target =
        VirtualTargetForPhysicalIndex(physicalIndex);
    return hardware::LedIndexForSquare(target.square, target.slot);
}

constexpr bool HasCollisionFreeVirtualTargets() {
    for (uint8_t lhs = 0U; lhs < modules::EDGE_LED_COUNT; ++lhs) {
        for (uint8_t rhs = static_cast<uint8_t>(lhs + 1U);
             rhs < modules::EDGE_LED_COUNT;
             ++rhs) {
            if (VirtualLedIndexForPhysicalStatusIndex(lhs) ==
                VirtualLedIndexForPhysicalStatusIndex(rhs)) {
                return false;
            }
        }
    }
    return true;
}

static_assert(modules::EDGE_LED_COUNT == 32U,
              "status frame adapter expects 32 semantic LEDs");
static_assert(hardware::EDGE_LED_COUNT == 0U ||
                  hardware::EDGE_LED_COUNT == modules::EDGE_LED_COUNT,
              "hardware must expose either no dedicated status chain or the "
              "complete semantic frame");
static_assert(HasCollisionFreeVirtualTargets(),
              "virtual status LEDs must not share board LED slots");

inline modules::Rgb BlendVirtualStatus(const modules::Rgb base,
                                       const modules::Rgb accent) {
    static constexpr uint8_t EDGE_ALPHA_MAX = 160U;
    const uint8_t intensity =
        (accent.r > accent.g)
            ? ((accent.r > accent.b) ? accent.r : accent.b)
            : ((accent.g > accent.b) ? accent.g : accent.b);
    const uint8_t alpha = static_cast<uint8_t>(
        (static_cast<uint16_t>(EDGE_ALPHA_MAX) * intensity + 127U) /
        255U);
    return modules::AlphaBlendRgb(base, accent, alpha);
}

inline void ApplyToDedicatedChain(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& semanticFrame,
    const bool boardRotate,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& physicalFrame) {
    if constexpr (hardware::EDGE_LED_COUNT == 0U) {
        return;
    } else {
        for (uint8_t semanticIndex = 0U;
             semanticIndex < modules::EDGE_LED_COUNT;
             ++semanticIndex) {
            const uint8_t physicalIndex = PhysicalIndexForSemanticIndex(
                semanticIndex, boardRotate);
            physicalFrame[static_cast<uint16_t>(
                hardware::FirstEdgeLedIndex() + physicalIndex)] =
                semanticFrame[semanticIndex];
        }
    }
}

// The mask names composer-output squares, before final square rotation.
// Only C3's shared side slots yield to this explicit board-cue ownership.
constexpr bool IsVirtualStatusTargetProtected(
    const uint8_t physicalStatusIndex,
    const uint64_t sidePrioritySquareMask,
    const bool squareRotateForOutput) {
    const uint8_t physicalSquare = VirtualTargetForPhysicalIndex(physicalStatusIndex).square;
    const uint8_t semanticSquare = squareRotateForOutput
        ? static_cast<uint8_t>(63U - physicalSquare) : physicalSquare;
    return (sidePrioritySquareMask & (1ULL << semanticSquare)) != 0ULL;
}

inline void ApplyToVirtualPerimeter(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& semanticFrame,
    const bool boardRotate,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& physicalFrame,
    const uint64_t sidePrioritySquareMask = 0ULL,
    const bool squareRotateForOutput = false) {
    for (uint8_t semanticIndex = 0U;
         semanticIndex < modules::EDGE_LED_COUNT;
         ++semanticIndex) {
        const uint8_t physicalIndex = PhysicalIndexForSemanticIndex(
            semanticIndex, boardRotate);
        if (IsVirtualStatusTargetProtected(physicalIndex, sidePrioritySquareMask,
                                           squareRotateForOutput)) {
            continue;
        }
        const uint16_t physicalLed =
            VirtualLedIndexForPhysicalStatusIndex(physicalIndex);
        physicalFrame[physicalLed] = BlendVirtualStatus(
            physicalFrame[physicalLed], semanticFrame[semanticIndex]);
    }
}

} // namespace modules::status_led_frame_adapter
