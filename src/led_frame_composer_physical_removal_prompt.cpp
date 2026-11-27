#include "modules/led_frame_composer_internal.hpp"
#include "modules/led_render_policy.hpp"

namespace modules::led_frame_composer::internal {
namespace {

constexpr uint32_t TAP_MS = 120U;
constexpr uint32_t REMOVAL_CYCLE_MS = 1200U;
constexpr uint8_t REMOVAL_PEAK = static_cast<uint8_t>(
    255U * 256U / led_render_policy::COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256);
constexpr modules::Rgb REMOVAL_AMBER{REMOVAL_PEAK, REMOVAL_PEAK * 2U / 5U, 0U};
constexpr modules::Rgb REMOVAL_ORANGE{REMOVAL_PEAK, REMOVAL_PEAK / 3U, 0U};

bool RemovalMayOwnBoard(const ComposeContext& ctx) {
    const auto& snapshot = ctx.Snapshot();
    return !ctx.renderIntent.HasCelebration() && !ctx.in->menuActive &&
        !ctx.renderIntent.menuForegroundVisible && !snapshot.render.textOverlayActive &&
        !snapshot.interaction.placementModeActive && !ctx.kingQueenSwapPromptActive &&
        !ctx.undoPromptActive && !ctx.invalidBoard.IsActive() &&
        ctx.humanIllegalMask == 0ULL && ctx.cpuIllegalMask == 0ULL &&
        ctx.cpuTentativeCorrectionMask == 0ULL &&
        ctx.captureReturnMask == 0ULL &&
        !runtime::IsRuntimeHealthPlayPaused(ctx.in->runtimeHealth) &&
        !ctx.in->higherPriorityStatusOverlayActive && !snapshot.system.safeModeActive &&
        !snapshot.system.hardwareTestActive && !snapshot.system.amptestActive && !snapshot.system.otaInProgress;
}

uint8_t RemovalAlpha(const ComposeContext& ctx, const uint8_t square) {
    if (square >= 64U || !RemovalMayOwnBoard(ctx) ||
        (ctx.physicalOcc64 & (UINT64_C(1) << square)) == 0ULL) return 0U;
    const auto& plan = ctx.Snapshot().interaction.taggedState.plan;
    const bool directed = (ctx.cpuTargetOccValid || ctx.partialCastlingTargetOccValid) &&
        plan.removalRequired && !plan.progress.removalComplete && plan.removalSquare == square;
    const bool pendingEnPassant = ctx.pendingCaptureSquareValid && ctx.pendingCaptureSquare == square;
    if (directed || pendingEnPassant) return 255U;
    if (!IsHintCaptureRemovalSquare(ctx, square)) return 0U;
    const auto& render = ctx.Snapshot().render;
    // A different suggested attacker is not a different physical victim.
    // Keep its cue continuous while the move endpoints crossfade.
    if (render.hintRefinementActive && render.previousHintCaptureSquare == square &&
        render.incomingHintCaptureSquare == square) return 255U;
    return ctx.hintRefinementAlpha255;
}

bool RemovalTapIsBright(const ComposeContext& ctx, const uint8_t square) {
    const auto& snapshot = ctx.Snapshot();
    uint32_t elapsed = snapshot.interaction.lastInteractionElapsedMs;
    if (ctx.cpuTargetOccValid) {
        elapsed = snapshot.interaction.cpuMovePromptElapsedMs;
    } else if (IsHintCaptureRemovalSquare(ctx, square)) {
        const auto& render = snapshot.render;
        if (render.hintRemovalSquare == square) {
            elapsed = render.hintRemovalElapsedMs > render.hintRemovalDelayMs
                ? render.hintRemovalElapsedMs - render.hintRemovalDelayMs : 0U;
        } else if (render.previousHintRemovalSquare == square) {
            elapsed = render.previousHintRemovalElapsedMs > render.previousHintRemovalDelayMs
                ? render.previousHintRemovalElapsedMs - render.previousHintRemovalDelayMs : 0U;
        } else if (!(ctx.pendingCaptureSquareValid && ctx.pendingCaptureSquare == square) &&
                   render.hintFeedbackTimingValid) {
            elapsed = render.hintFeedbackElapsedMs;
        }
    }
    const uint32_t phase = elapsed % REMOVAL_CYCLE_MS;
    return phase < TAP_MS || (phase >= 2U * TAP_MS && phase < 3U * TAP_MS);
}

} // namespace

bool IsHintCaptureRemovalSquare(const ComposeContext& ctx, const uint8_t logicalSq) {
    const auto& plan = ctx.Snapshot().interaction.taggedState.plan;
    // Occupancy after a completed capture belongs to the seated mover. Duck
    // keeps the compound Hint visible through chess settling; that cache must
    // not turn the already removed victim back into a physical action.
    if (plan.removalRequired && plan.removalSquare == logicalSq &&
        plan.progress.removalComplete) return false;
    return logicalSq < 64U && !ctx.invalidBoard.IsActive() &&
           ctx.renderIntent.hintPromptVisible && ctx.hintMoveValid &&
           ctx.hintCaptureSquare == logicalSq &&
           (ctx.physicalOcc64 & (1ULL << logicalSq)) != 0ULL;
}

uint64_t PhysicalRemovalSquareMask(const ComposeContext& ctx) {
    const auto& plan = ctx.Snapshot().interaction.taggedState.plan;
    uint64_t mask = 0ULL;
    for (const uint8_t square : {plan.removalSquare, ctx.pendingCaptureSquare, ctx.hintCaptureSquare})
        if (RemovalAlpha(ctx, square) != 0U) mask |= UINT64_C(1) << square;
    return mask;
}

uint64_t ApplyPhysicalRemovalBrackets(const ComposeContext& ctx, modules::BoardLedFrame& board) {
    uint64_t brightTaps = 0ULL;
    const auto& plan = ctx.Snapshot().interaction.taggedState.plan;
    for (const uint8_t square : {plan.removalSquare, ctx.pendingCaptureSquare, ctx.hintCaptureSquare}) {
        const uint8_t alpha = RemovalAlpha(ctx, square);
        if (alpha == 0U) continue;
        modules::Rgb base = REMOVAL_ORANGE;
        if (!ctx.minimalAnimations) {
            if (RemovalTapIsBright(ctx, square)) {
                base = REMOVAL_AMBER;
                brightTaps |= UINT64_C(1) << square;
            } else {
                base = ScaleRgbSaturating(REMOVAL_AMBER, 64U);
            }
        }
        // Only an optional suggestion fades during a Hint refinement.
        const auto color = ScaleRgbSaturating(base,
            static_cast<uint16_t>(alpha) * 256U / 255U);
        auto& leds = board[square];
        leds.left = color;
        leds.right = color;
        modules::SetPiecePair(leds, modules::RGB_BLACK);
    }
    return brightTaps;
}

} // namespace modules::led_frame_composer::internal
