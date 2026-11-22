#pragma once

#include <array>
#include <cstdint>

#include "hardware/board_revision.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/led_mapping.hpp"
#include "hardware/status_led_chain_profile.hpp"
#include "runtime/square_mapping.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "modules/board_led_frame.hpp"
#include "modules/grid_foreground_layer.hpp"

namespace hardware {

constexpr uint16_t MAX_PHYSICAL_BOARD_LED_COUNT = 256U;
constexpr uint16_t MAX_PHYSICAL_STATUS_LED_COUNT = 44U;
constexpr uint16_t MAX_PHYSICAL_LED_COUNT =
    MAX_PHYSICAL_BOARD_LED_COUNT + MAX_PHYSICAL_STATUS_LED_COUNT;

enum class V3SquareLedPosition : uint8_t {
    Left = 0U,
    Top = 1U,
    Right = 2U,
    Bottom = 3U,
};

struct BoardLedTopologyProfile {
    uint8_t physicalLedsPerSquare = 3U;
    uint16_t physicalBoardLedCount = 192U;
    uint16_t physicalStatusLedCount = 32U;
};

struct PhysicalLedTargets {
    std::array<uint16_t, 2> indices{};
    uint8_t count = 0U;
};

constexpr BoardLedTopologyProfile BoardLedTopologyFor(
    const BoardRevision revision) {
    switch (ResolveBoardRevision(revision)) {
        case BoardRevision::V1C3:
            return BoardLedTopologyProfile{3U, 192U, 0U};
        case BoardRevision::V3S3:
            return BoardLedTopologyProfile{4U, 256U, 32U};
        case BoardRevision::V3_3S3:
            return BoardLedTopologyProfile{4U, 256U, 44U};
        case BoardRevision::V2S3:
        case BoardRevision::Unknown:
        default:
            return BoardLedTopologyProfile{3U, 192U, 32U};
    }
}

constexpr PhysicalLedTargets MapV1V2SquareLedTargets(
    const uint8_t square,
    const SquareLedSlot semanticSlot,
    const bool rotate180) {
    const uint8_t mappedSquare = runtime::MapSquareIndex(square, rotate180);
    const SquareLedSlot mappedSlot = rotate180
                                         ? RotateSquareLedSlot180(semanticSlot)
                                         : semanticSlot;
    return PhysicalLedTargets{
        std::array<uint16_t, 2>{
            static_cast<uint16_t>(mappedSquare * 3U +
                                  static_cast<uint8_t>(mappedSlot)),
            0U,
        },
        1U,
    };
}

constexpr PhysicalLedTargets MapV3SquareLedTargets(
    const uint8_t square,
    const SquareLedSlot semanticSlot,
    const bool rotate180) {
    const uint8_t mappedSquare = runtime::MapSquareIndex(square, rotate180);
    const uint16_t base = static_cast<uint16_t>(mappedSquare * 4U);
    const SquareLedSlot mappedSlot = rotate180
                                         ? RotateSquareLedSlot180(semanticSlot)
                                         : semanticSlot;

    if (mappedSlot == SquareLedSlot::Piece) {
        return PhysicalLedTargets{
            std::array<uint16_t, 2>{
                static_cast<uint16_t>(base +
                                      static_cast<uint8_t>(V3SquareLedPosition::Top)),
                static_cast<uint16_t>(base +
                                      static_cast<uint8_t>(V3SquareLedPosition::Bottom)),
            },
            2U,
        };
    }

    const V3SquareLedPosition position =
        (mappedSlot == SquareLedSlot::Square0)
            ? V3SquareLedPosition::Left
            : V3SquareLedPosition::Right;
    return PhysicalLedTargets{
        std::array<uint16_t, 2>{
            static_cast<uint16_t>(base + static_cast<uint8_t>(position)),
            0U,
        },
        1U,
    };
}

constexpr PhysicalLedTargets MapSemanticSquareLedTargets(
    const BoardRevision revision,
    const uint8_t square,
    const SquareLedSlot semanticSlot,
    const bool rotate180) {
    if (square >= SQUARE_COUNT) {
        return PhysicalLedTargets{};
    }

    if (BoardLedTopologyFor(revision).physicalLedsPerSquare == 4U) {
        return MapV3SquareLedTargets(square, semanticSlot, rotate180);
    }

    return MapV1V2SquareLedTargets(square, semanticSlot, rotate180);
}

// Full-frame expansion owns topology and orientation, before RGBW conversion
// and power estimation. Producers still own status composition and priorities.
using PhysicalLedFrame = std::array<modules::Rgb, MAX_PHYSICAL_LED_COUNT>;

// Stack-owned handoff from the completed production render to shutdown. It is
// deliberately not stored in FirmwareSystem: the LED driver's shadow buffers
// may contain a denied frame, while a persistent duplicate costs 864 bytes.
struct AcceptedPhysicalLedFrame {
    PhysicalLedFrame frame{};
    uint16_t brightnessScale256 = 0U;
    bool valid = false;
};

inline void RecordAcceptedPhysicalLedFrame(
    const bool showCompleted,
    const PhysicalLedFrame& frame,
    const uint16_t brightnessScale256,
    AcceptedPhysicalLedFrame* accepted) {
    if (!showCompleted || accepted == nullptr) return;
    accepted->frame = frame;
    accepted->brightnessScale256 = brightnessScale256;
    accepted->valid = true;
}

struct LedFrameMappingConfig {
    BoardRevision revision = BoardRevision::V2S3;
    bool squareRotate = false;
    bool statusRotate = false;
    uint64_t sidePrioritySquareMask = 0ULL;
};

inline PhysicalLedFrame MapSemanticBoardFrame(
    const LedFrameMappingConfig& config,
    const modules::BoardLedFrame& board) {
    PhysicalLedFrame physical{};
    for (uint8_t square = 0U; square < SQUARE_COUNT; ++square) {
        const auto& leds = board[square];
        const auto mapped = runtime::MapSquareIndex(square, config.squareRotate);
        if (BoardLedTopologyFor(config.revision).physicalLedsPerSquare == 4U) {
            const uint16_t base = mapped * 4U;
            physical[base] = config.squareRotate ? leds.right : leds.left;
            physical[base + 1U] = config.squareRotate ? leds.bottom : leds.top;
            physical[base + 2U] = config.squareRotate ? leds.left : leds.right;
            physical[base + 3U] = config.squareRotate ? leds.top : leds.bottom;
        } else {
            const uint16_t base = mapped * 3U;
            physical[base] = config.squareRotate ? leds.right : leds.left;
            physical[base + 1U] = modules::RoundedPieceAverage(leds);
            physical[base + 2U] = config.squareRotate ? leds.left : leds.right;
        }
    }
    return physical;
}

inline void ApplySemanticStatusFrame(
    const LedFrameMappingConfig& config,
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status,
    PhysicalLedFrame& physical) {
    const auto topology = BoardLedTopologyFor(config.revision);
    for (uint8_t semantic = 0U; semantic < modules::EDGE_LED_COUNT; ++semantic) {
        const uint8_t physicalStatus = modules::status_led_frame_adapter::
            PhysicalIndexForSemanticIndex(semantic, config.statusRotate);
        if (topology.physicalStatusLedCount != 0U) {
            const uint8_t chainIndex =
                config.revision == BoardRevision::V3_3S3
                    ? V3_3PhysicalIndexForSemanticStatus(physicalStatus)
                    : physicalStatus;
            physical[topology.physicalBoardLedCount + chainIndex] = status[semantic];
        } else {
            if (modules::status_led_frame_adapter::IsVirtualStatusTargetProtected(
                    physicalStatus, config.sidePrioritySquareMask, config.squareRotate)) {
                continue;
            }
            const auto target = modules::status_led_frame_adapter::
                VirtualLedIndexForPhysicalStatusIndex(physicalStatus);
            physical[target] = modules::status_led_frame_adapter::BlendVirtualStatus(
                physical[target], status[semantic]);
        }
    }
}

inline PhysicalLedFrame MapSemanticLedFrame(
    const LedFrameMappingConfig& config,
    const modules::BoardLedFrame& board,
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status) {
    PhysicalLedFrame physical = MapSemanticBoardFrame(config, board);
    ApplySemanticStatusFrame(config, status, physical);
    return physical;
}

inline modules::Rgb ScaleGridForegroundRgb(
    const modules::Rgb color,
    const uint16_t scale256) {
    const auto channel = [scale256](const uint8_t value) {
        const uint32_t scaled =
            static_cast<uint32_t>(value) * scale256 + 128U;
        return static_cast<uint8_t>(scaled >= 65280U
                                        ? 255U
                                        : (scaled >> 8U));
    };
    return modules::Rgb{channel(color.r), channel(color.g), channel(color.b)};
}

inline void ClipGridForegroundToOccupiedPreview(
    const uint64_t occupiedGameplayMask,
    const bool gameplayRotate,
    const bool foregroundRotate,
    modules::GridForegroundLayer& layer) {
    layer.sourceCoverageMask &= occupiedGameplayMask;
    for (uint8_t foregroundSquare = 0U; foregroundSquare < SQUARE_COUNT;
         ++foregroundSquare) {
        if ((layer.placementCoverageMask &
             (1ULL << foregroundSquare)) == 0ULL) {
            continue;
        }
        const uint8_t physicalTarget = runtime::MapSquareIndex(
            foregroundSquare, foregroundRotate);
        const uint8_t gameplaySquare = runtime::MapSquareIndex(
            physicalTarget, gameplayRotate);
        if ((occupiedGameplayMask & (1ULL << gameplaySquare)) == 0ULL) {
            layer.placementCoverageMask &= ~(1ULL << foregroundSquare);
        }
    }
}

// Relocates only explicitly rasterized grid foreground pixels. The completed
// board remains mapped at gameplay orientation; menu glyph/icon coverage is
// first removed there and then placed at the literal control orientation.
inline void RelocateGridForeground(
    const LedFrameMappingConfig& baseConfig,
    const bool overlayRotate,
    const modules::BoardLedFrame& board,
    const modules::GridForegroundLayer& layer,
    PhysicalLedFrame& physical) {
    const BoardRevision resolved = ResolveBoardRevision(baseConfig.revision);

    // Remove the copy produced by ordinary semantic mapping. Coverage is
    // authoritative even when foreground and background RGB happen to match.
    for (uint8_t square = 0U; square < SQUARE_COUNT; ++square) {
        if ((layer.sourceCoverageMask & (1ULL << square)) == 0ULL) {
            continue;
        }
        const uint8_t mappedSquare = runtime::MapSquareIndex(
            square, baseConfig.squareRotate);
        const modules::Rgb background = ScaleGridForegroundRgb(
            layer.background[square], layer.brightnessScale256);
        if (BoardLedTopologyFor(resolved).physicalLedsPerSquare == 4U) {
            const uint16_t base = static_cast<uint16_t>(mappedSquare * 4U);
            physical[base] = background;
            physical[base + 2U] = background;
            continue;
        }
        const uint16_t base = static_cast<uint16_t>(mappedSquare * 3U);
        physical[base] = background;
        physical[base + 2U] = background;
        modules::BoardSquareLeds restored = board[square];
        restored.left = background;
        restored.right = background;
        physical[base + 1U] = modules::RoundedPieceAverage(restored);
    }

    // Place the explicit foreground at the control owner's orientation.
    for (uint8_t square = 0U; square < SQUARE_COUNT; ++square) {
        if ((layer.placementCoverageMask & (1ULL << square)) == 0ULL) {
            continue;
        }
        const uint8_t mappedSquare =
            runtime::MapSquareIndex(square, overlayRotate);
        const uint8_t gameplaySquare = runtime::MapSquareIndex(
            mappedSquare, baseConfig.squareRotate);
        const modules::Rgb composed = ScaleGridForegroundRgb(
            modules::led_text_renderer::AlphaBlend(
                layer.background[gameplaySquare], layer.colors[square],
                layer.alpha255),
            layer.brightnessScale256);
        if (BoardLedTopologyFor(resolved).physicalLedsPerSquare == 4U) {
            const uint16_t base = static_cast<uint16_t>(mappedSquare * 4U);
            physical[base] = composed;
            physical[base + 2U] = composed;
            continue;
        }
        const uint16_t base = static_cast<uint16_t>(mappedSquare * 3U);
        physical[base] = composed;
        physical[base + 2U] = composed;
        modules::BoardSquareLeds placed = board[gameplaySquare];
        placed.left = physical[base];
        placed.right = physical[base + 2U];
        physical[base + 1U] = modules::RoundedPieceAverage(placed);
    }
}

inline PhysicalLedFrame MapSemanticLedFrameWithGridForeground(
    const LedFrameMappingConfig& baseConfig,
    const bool overlayRotate,
    const modules::BoardLedFrame& board,
    const modules::GridForegroundLayer& layer,
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status) {
    PhysicalLedFrame physical = MapSemanticBoardFrame(baseConfig, board);
    RelocateGridForeground(
        baseConfig, overlayRotate, board, layer, physical);
    // V1 status pixels share board LEDs and retain their established priority.
    ApplySemanticStatusFrame(baseConfig, status, physical);
    return physical;
}

inline PhysicalLedFrame MapRuntimeSemanticLedFrame(
    const BoardRevision revision,
    const bool boardRotate,
    const bool firstPlayerIsP2,
    const modules::BoardLedFrame& board,
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status,
    const uint64_t sidePrioritySquareMask = 0ULL) {
    const bool effectiveRotate = boardRotate != firstPlayerIsP2;
    return MapSemanticLedFrame(
        {revision, effectiveRotate, effectiveRotate,
         sidePrioritySquareMask},
        board, status);
}

inline void ApplyFixedPhysicalStatusPixel(
    const BoardRevision revision,
    const uint8_t physicalStatusIndex,
    const modules::Rgb color,
    PhysicalLedFrame& physical,
    const uint8_t alpha255 = 255U) {
    if (physicalStatusIndex >= modules::EDGE_LED_COUNT) {
        return;
    }
    const auto topology = BoardLedTopologyFor(revision);
    if (topology.physicalStatusLedCount != 0U) {
        const uint8_t chainIndex = revision == BoardRevision::V3_3S3
            ? V3_3PhysicalIndexForSemanticStatus(physicalStatusIndex)
            : physicalStatusIndex;
        auto& target = physical[topology.physicalBoardLedCount + chainIndex];
        target = alpha255 == 255U ? color : modules::AlphaBlendRgb(target, color, alpha255);
        return;
    }
    const uint16_t target = modules::status_led_frame_adapter::
        VirtualLedIndexForPhysicalStatusIndex(physicalStatusIndex);
    const auto replacement = modules::status_led_frame_adapter::BlendVirtualStatus(
        physical[target], color);
    physical[target] = alpha255 == 255U ? replacement
        : modules::AlphaBlendRgb(physical[target], replacement, alpha255);
}

inline void ApplyFixedPhysicalControlAccent(
    const BoardRevision revision,
    const modules::status_led_frame_adapter::FixedPhysicalStatusOverlay& overlay,
    PhysicalLedFrame& physical,
    const bool controlBanksSwapped) {
    const auto resolved = ResolveBoardRevision(revision);
    if (overlay.controlAccentMask == 0U || overlay.controlAccentAlpha255 == 0U ||
        (resolved != BoardRevision::V3S3 && resolved != BoardRevision::V3_3S3)) return;
    for (uint8_t index = 0U; index < 12U; ++index) {
        if ((overlay.controlAccentMask & (1U << index)) == 0U) continue;
        const auto role = static_cast<ControlLedRole>(index % 6U);
        uint8_t target = 0U;
        if (resolved == BoardRevision::V3_3S3) {
            target = V3_3PhysicalIndexForLogicalControl(index >= 6U, role, controlBanksSwapped);
        } else {
            // V3 has no dedicated directional controls; its existing X/O
            // projection uses the outside positions of each four-pixel bank.
            if (role != ControlLedRole::O && role != ControlLedRole::X) continue;
            const bool physicalP2 = (index >= 6U) != controlBanksSwapped;
            target = modules::PlayerHomeLedIndex(
                physicalP2 ? modules::UiOwner::P2 : modules::UiOwner::P1,
                role == ControlLedRole::O ? 2U : 5U);
        }
        auto& pixel = physical[256U + target];
        pixel = modules::AlphaBlendRgb(pixel, overlay.controlAccentPixels[index],
                                       overlay.controlAccentAlpha255);
    }
}

inline void ApplyFixedPhysicalStatusOverlay(
    const BoardRevision revision,
    const modules::status_led_frame_adapter::FixedPhysicalStatusOverlay& overlay,
    PhysicalLedFrame& physical,
    const bool controlBanksSwapped = false) {
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        if ((overlay.mask & (1UL << index)) == 0U) continue;
        ApplyFixedPhysicalStatusPixel(revision, index, overlay.pixels[index], physical,
                                      overlay.perimeterAlpha255);
    }
    if (ResolveBoardRevision(revision) == BoardRevision::V3S3) {
        for (uint8_t index = 0U; index < 8U; ++index) {
            if ((overlay.emulatedControlMask & (1U << index)) == 0U) continue;
            const bool physicalP2 = (index >= 4U) != controlBanksSwapped;
            const uint8_t target = modules::PlayerHomeLedIndex(
                physicalP2 ? modules::UiOwner::P2 : modules::UiOwner::P1,
                static_cast<uint8_t>(2U + index % 4U));
            physical[256U + target] = modules::AlphaBlendRgb(
                physical[256U + target], overlay.emulatedControlPixels[index],
                overlay.controlAlpha255[index / 4U]);
        }
        ApplyFixedPhysicalControlAccent(revision, overlay, physical, controlBanksSwapped);
        return;
    }
    if (ResolveBoardRevision(revision) != BoardRevision::V3_3S3) return;
    for (uint8_t index = 0U; index < 12U; ++index) {
        if ((overlay.controlMask & (1U << index)) == 0U) continue;
        const auto role = static_cast<ControlLedRole>(index % 6U);
        const uint8_t target = V3_3PhysicalIndexForLogicalControl(
            index >= 6U, role, controlBanksSwapped);
        physical[256U + target] = modules::AlphaBlendRgb(
            physical[256U + target], overlay.controlPixels[index],
            overlay.controlAlpha255[index / 6U]);
    }
    ApplyFixedPhysicalControlAccent(revision, overlay, physical, controlBanksSwapped);
}

// Compatibility producers request one piece color; expand equally at ingress.
inline PhysicalLedFrame MapSemanticLedFrame(
    const LedFrameMappingConfig& config,
    const std::array<modules::Rgb, TOTAL_LED_COUNT>& board,
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status) {
    return MapSemanticLedFrame(config, modules::ExpandLegacyBoardFrame(board), status);
}

// Remaining startup/sleep/power producers carry status in the legacy tail.
// Already-physical input uses zero rotation. Do not overlay status a second
// time on a revision without a dedicated chain.
inline PhysicalLedFrame MapLegacyLedFrame(
    const LedFrameMappingConfig& config,
    const std::array<modules::Rgb, TOTAL_LED_COUNT>& legacy) {
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    if constexpr (EDGE_LED_COUNT != 0U) {
        if (BoardLedTopologyFor(config.revision).physicalStatusLedCount != 0U) {
            for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
                status[index] = legacy[BOARD_LED_COUNT + index];
            }
        }
    }
    return MapSemanticLedFrame(config, legacy, status);
}

} // namespace hardware
