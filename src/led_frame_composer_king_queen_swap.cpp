#include "modules/led_frame_composer_internal.hpp"

namespace modules::led_frame_composer::internal {
namespace {

constexpr uint32_t SHUTTLE_TRAVEL_MS = 480U;
constexpr uint32_t SHUTTLE_HOLD_MS = 120U;
constexpr uint32_t SHUTTLE_PASS_MS = SHUTTLE_TRAVEL_MS + SHUTTLE_HOLD_MS;

uint32_t ShuttleProgressQ8(const uint32_t elapsed, const uint8_t steps) {
    const uint32_t phase = elapsed % SHUTTLE_PASS_MS;
    return phase < SHUTTLE_TRAVEL_MS
        ? phase * steps * 256U / SHUTTLE_TRAVEL_MS : steps * 256U;
}

modules::Rgb ScaleKqCue(const modules::Rgb color, const uint16_t scale256) {
    return ScaleRgbSaturating(color, scale256);
}

void SetKqRailSlot(modules::BoardSquareLeds& leds,
                   const hardware::SquareLedSlot slot,
                   const modules::Rgb color) {
    switch (slot) {
        case hardware::SquareLedSlot::Square0:
            leds.left = color;
            return;
        case hardware::SquareLedSlot::Square2:
            leds.right = color;
            return;
        case hardware::SquareLedSlot::Piece:
            modules::SetPiecePair(leds, color);
            return;
    }
}

} // namespace

void ApplyKingQueenSwapPairShuttle(const ComposeContext& ctx,
                                   const uint8_t logicalSq,
                                   const bool occupied,
                                   modules::BoardSquareLeds& leds) {
    if (!ctx.kingQueenSwapPromptActive || logicalSq >= 64U) {
        return;
    }
    const uint64_t bit = UINT64_C(1) << logicalSq;
    if ((ctx.kingQueenSwapPromptFlashMask & bit) == 0ULL) {
        return;
    }

    const uint8_t file = static_cast<uint8_t>(logicalSq % 8U);
    if (file != 3U && file != 4U) {
        return;
    }
    const uint8_t dSquare = file == 3U
                                ? logicalSq
                                : static_cast<uint8_t>(logicalSq - 1U);
    const uint8_t eSquare = static_cast<uint8_t>(dSquare + 1U);
    const uint64_t dBit = UINT64_C(1) << dSquare;
    const uint64_t eBit = UINT64_C(1) << eSquare;
    const bool dPending = (ctx.kingQueenSwapPromptFlashMask & dBit) != 0ULL;
    const bool ePending = (ctx.kingQueenSwapPromptFlashMask & eBit) != 0ULL;
    const bool dOccupied = (ctx.physicalOcc64 & dBit) != 0ULL;
    const bool eOccupied = (ctx.physicalOcc64 & eBit) != 0ULL;

    const bool whiteBackRank = logicalSq >= 56U;
    const modules::UiOwner owner = OwnerForSideToMove(
        whiteBackRank ? modules::SideToMove::White
                      : modules::SideToMove::Black,
        ctx.firstPlayerIsP2);
    const modules::Rgb accent = owner == modules::UiOwner::P2
                                    ? ctx.p2AccentRgb
                                    : ctx.p1AccentRgb;

    const bool sourceOn = ctx.minimalAnimations || modules::animation::IsBlinkOn(
        {ctx.Snapshot().interaction.lastInteractionElapsedMs},
        modules::animation::LED_FRAME_TOKENS.moverPieceBlink);
    const modules::Rgb source = sourceOn
        ? PieceRgbForOwner(ctx, owner) : modules::RGB_BLACK;

    // Once either square has been lifted, replace the reciprocal animation
    // with a steady, explicit socket/source relationship. The hardware knows
    // occupancy only; this deliberately does not claim to verify identities.
    if (!dOccupied || !eOccupied || !dPending || !ePending) {
        if (!occupied) {
            leds.left = accent;
            leds.right = accent;
            modules::SetPiecePair(leds, modules::RGB_BLACK);
            return;
        }

        const hardware::SquareLedSlot inwardSlot =
            file == 3U ? hardware::SquareLedSlot::Square2
                       : hardware::SquareLedSlot::Square0;
        const hardware::SquareLedSlot outwardSlot =
            file == 3U ? hardware::SquareLedSlot::Square0
                       : hardware::SquareLedSlot::Square2;
        SetKqRailSlot(leds, outwardSlot, ScaleKqCue(accent, 112U));
        SetKqRailSlot(leds, inwardSlot, accent);
        modules::SetPiecePair(leds, source);
        return;
    }

    // Full pending pair: keep all four rail pixels visible as a bracket while
    // a strong head travels D-outer -> D/E seam -> E-outer and reverses.
    const modules::Rgb rail = ScaleKqCue(accent, 96U);
    leds.left = rail;
    leds.right = rail;
    modules::SetPiecePair(leds, source);

    if (ctx.minimalAnimations) return;

    const uint32_t elapsed =
        ctx.Snapshot().interaction.lastInteractionElapsedMs;
    const uint32_t progressQ8 =
        (elapsed / SHUTTLE_PASS_MS) % 2U == 0U
            ? ShuttleProgressQ8(elapsed, 3U)
            : 3U * 256U - ShuttleProgressQ8(elapsed, 3U);

    const uint8_t headIndex = static_cast<uint8_t>(progressQ8 / 256U);
    const uint8_t nextIndex = headIndex < 3U
                                  ? static_cast<uint8_t>(headIndex + 1U)
                                  : headIndex;
    const uint16_t fraction = static_cast<uint16_t>(progressQ8 % 256U);
    const auto ApplyHead = [&](const uint8_t index, const uint16_t scale) {
        const bool thisIsD = index < 2U;
        if ((thisIsD && logicalSq != dSquare) ||
            (!thisIsD && logicalSq != eSquare)) {
            return;
        }
        const hardware::SquareLedSlot slot =
            (index == 0U || index == 2U)
                ? hardware::SquareLedSlot::Square0
                : hardware::SquareLedSlot::Square2;
        SetKqRailSlot(leds, slot, ScaleKqCue(accent, scale));
    };
    ApplyHead(headIndex, static_cast<uint16_t>(256U - fraction / 2U));
    if (nextIndex != headIndex) {
        ApplyHead(nextIndex, static_cast<uint16_t>(128U + fraction / 2U));
    }
}

} // namespace modules::led_frame_composer::internal
