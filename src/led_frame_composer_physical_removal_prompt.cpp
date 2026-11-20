#include "modules/led_frame_composer_internal.hpp"
#include "modules/led_render_policy.hpp"

namespace modules::led_frame_composer::internal {
namespace {

constexpr uint32_t REMOVAL_HANDOFF_MS = 300U;
constexpr uint32_t REMOVAL_CYCLE_MS = REMOVAL_HANDOFF_MS * 2U;
constexpr uint32_t REMOVAL_ENDPOINT_DWELL_MS = 60U;
constexpr uint32_t REMOVAL_FADE_MS = REMOVAL_HANDOFF_MS - REMOVAL_ENDPOINT_DWELL_MS;
constexpr uint8_t REMOVAL_WHITE_PEAK = static_cast<uint8_t>(
    modules::RGB_WHITE.r * 256U / led_render_policy::COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256);
constexpr modules::Rgb REMOVAL_WHITE{REMOVAL_WHITE_PEAK, REMOVAL_WHITE_PEAK, REMOVAL_WHITE_PEAK};
enum class RemovalSide : uint8_t { Left, Right };

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
    return IsHintCaptureRemovalSquare(ctx, square) ? ctx.hintRefinementAlpha255 : 0U;
}

modules::Rgb RemovalWhite(const ComposeContext& ctx, const BoardComposeRuntime& runtime,
                          const RemovalSide side = RemovalSide::Left) {
    // Compute time progress directly: the generic 8-bit triangle quantizes
    // below the exact full endpoint required by this removal presentation.
    const uint32_t phase = runtime.illegalAnim.elapsedMs % REMOVAL_CYCLE_MS;
    const uint32_t towardRight = phase <= REMOVAL_HANDOFF_MS ? phase : REMOVAL_CYCLE_MS - phase;
    // A centered endpoint dwell survives arbitrary phase at the normal20ms
    // physical cadence; a single mathematical endpoint could be missed forever.
    constexpr uint32_t HALF_DWELL_MS = REMOVAL_ENDPOINT_DWELL_MS / 2U;
    const uint16_t rightScale = towardRight <= HALF_DWELL_MS ? 0U :
        towardRight >= REMOVAL_HANDOFF_MS - HALF_DWELL_MS ? 256U :
        static_cast<uint16_t>(((towardRight - HALF_DWELL_MS) * 256U + REMOVAL_FADE_MS / 2U) / REMOVAL_FADE_MS);
    const uint16_t scale = ctx.minimalAnimations ? 256U :
        side == RemovalSide::Right ? rightScale : static_cast<uint16_t>(256U - rightScale);
    return ScaleRgbSaturating(REMOVAL_WHITE, scale);
}

} // namespace

bool IsHintCaptureRemovalSquare(const ComposeContext& ctx, const uint8_t logicalSq) {
    return logicalSq < 64U && !ctx.invalidBoard.IsActive() &&
           ctx.renderIntent.hintPromptVisible && ctx.hintMoveValid &&
           ctx.hintCaptureSquare == logicalSq &&
           (ctx.physicalOcc64 & (1ULL << logicalSq)) != 0ULL;
}

void ApplyPhysicalRemovalPrompt(const ComposeContext& ctx,
                                const BoardComposeRuntime& runtime,
                                const bool forceOff,
                                const uint8_t logicalSq,
                                modules::Rgb& ioFinalSquare) {
    const uint8_t alpha = forceOff ? 0U : RemovalAlpha(ctx, logicalSq);
    if (alpha != 0U) {
        // Refine optional intensity against black, never an ordinary player
        // color that could relight the zero end of the local fade.
        ioFinalSquare = ScaleRgbSaturating(RemovalWhite(ctx, runtime),
            static_cast<uint16_t>(alpha) * 256U / 255U);
    }
}

uint64_t PhysicalRemovalSquareMask(const ComposeContext& ctx) {
    const auto& plan = ctx.Snapshot().interaction.taggedState.plan;
    uint64_t mask = 0ULL;
    for (const uint8_t square : {plan.removalSquare, ctx.pendingCaptureSquare, ctx.hintCaptureSquare})
        if (RemovalAlpha(ctx, square) != 0U) mask |= UINT64_C(1) << square;
    return mask;
}

void ApplyPhysicalRemovalBrackets(const ComposeContext& ctx,
                                  const BoardComposeRuntime& runtime,
                                  modules::BoardLedFrame& board) {
    const auto& plan = ctx.Snapshot().interaction.taggedState.plan;
    const std::array<uint8_t, 3U> victims{plan.removalSquare, ctx.pendingCaptureSquare, ctx.hintCaptureSquare};
    for (uint8_t i = 0U; i < victims.size(); ++i) {
        const uint8_t victim = victims[i];
        bool duplicate = false;
        for (uint8_t prior = 0U; prior < i; ++prior) duplicate = duplicate || victims[prior] == victim;
        const uint8_t alpha = duplicate ? 0U : RemovalAlpha(ctx, victim);
        if (alpha == 0U) continue;
        auto& leds = board[victim];
        ApplyPhysicalRemovalPrompt(ctx, runtime, false, victim, leds.left);
        leds.right = ScaleRgbSaturating(RemovalWhite(ctx, runtime, RemovalSide::Right),
            static_cast<uint16_t>(alpha) * 256U / 255U);
        modules::SetPiecePair(leds, modules::RGB_BLACK);

    }
}

} // namespace modules::led_frame_composer::internal
