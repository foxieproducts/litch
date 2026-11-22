#include "modules/led_frame_composer_internal.hpp"

#include "modules/animation_timing.hpp"
#include "modules/draw_rules.hpp"

namespace modules::led_frame_composer {
namespace internal {

void ApplyDrawCelebration(const ComposeContext& ctx,
                          modules::BoardLedFrame& outLedFrame,
                          std::array<modules::Rgb, 64>& outGridFrame) {
    const auto& game = ctx.Snapshot().game;

    if (ctx.renderIntent.celebration != CelebrationIntent::Draw ||
        !game.inDraw || game.inCheckmate) {
        return;
    }

    const uint32_t elapsedMs = ctx.renderIntent.celebrationElapsedMs;

    // Draw UX:
    // - Less bright than normal gameplay.
    // - Calm and uniform instead of celebratory.
    // - Use both players' piece colors to read as "neither side wins".
    const bool stalemate = (game.drawReason == modules::DrawReason::Stalemate);

    static constexpr uint32_t FADE_MS = 900U;
    const uint32_t clamped = (elapsedMs > FADE_MS) ? FADE_MS : elapsedMs;
    const uint8_t fadeAlpha = static_cast<uint8_t>(255U - (clamped * 255U) / (FADE_MS == 0 ? 1U : FADE_MS));
    const uint16_t baseDim256 = stalemate ? 136U : 152U;
    const uint16_t fadeScale256 = ctx.minimalAnimations
                                      ? baseDim256
                                      : static_cast<uint16_t>(
                                            (static_cast<uint32_t>(fadeAlpha) *
                                                 baseDim256 +
                                             127U) /
                                            255U);

    const modules::Rgb both = modules::AlphaBlendRgb(ctx.p1PieceRgb, ctx.p2PieceRgb, 128U);
    const auto& shimmerToken = stalemate
                                   ? modules::animation::LED_FRAME_TOKENS.stalemateShimmer
                                   : modules::animation::LED_FRAME_TOKENS.drawShimmer;
    const uint8_t overlayAlpha = ctx.minimalAnimations
                                     ? static_cast<uint8_t>(72U)
                                     : modules::animation::SampleValue(
                                           modules::animation::AnimationContext{
                                               elapsedMs},
                                           shimmerToken);

    for (uint8_t physicalSq = 0; physicalSq < 64; ++physicalSq) {

        outGridFrame[physicalSq] = ScaleRgbSaturating(outGridFrame[physicalSq], fadeScale256);
        outLedFrame[physicalSq].left = ScaleRgbSaturating(outLedFrame[physicalSq].left, fadeScale256);
        outLedFrame[physicalSq].right = ScaleRgbSaturating(outLedFrame[physicalSq].right, fadeScale256);
        outLedFrame[physicalSq].top = ScaleRgbSaturating(outLedFrame[physicalSq].top, fadeScale256);
        outLedFrame[physicalSq].bottom = ScaleRgbSaturating(outLedFrame[physicalSq].bottom, fadeScale256);

        const modules::Rgb mixed = modules::AlphaBlendRgb(outGridFrame[physicalSq], both, overlayAlpha);

        outGridFrame[physicalSq] = mixed;
        outLedFrame[physicalSq].left = mixed;
        outLedFrame[physicalSq].right = mixed;
    }
}

} // namespace internal
} // namespace modules::led_frame_composer
