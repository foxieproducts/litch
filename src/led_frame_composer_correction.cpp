#include "modules/led_frame_composer_internal.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"

namespace modules::led_frame_composer::internal {

void ConfigureCorrectionFeedback(ComposeContext& ctx) {
    if (ctx.Snapshot().game.kingResignationArmed) return;
    // Grade physical mistakes, not legality: masks still own/cancel conflicting
    // prompts immediately. Self-check and guided Undo retain their direct cues.
    const bool physicalCorrectionActive = ctx.invalidBoard.IsActive() ||
        (ctx.humanIllegalMask | ctx.cpuIllegalMask) != 0ULL;
    if (!physicalCorrectionActive || ctx.undoPromptActive ||
        ctx.kingSafetyPromptActive) return;

    const auto& snapshot = ctx.Snapshot();
    if (snapshot.game.variant == modules::GameVariant::Duck &&
        snapshot.game.duckSquare < 64U &&
        ctx.cpuIllegalMask == 0ULL && ctx.captureReturnMask == 0ULL &&
        runtime::GetDuckLandingRemovalMask(
            1ULL << snapshot.game.duckSquare, snapshot.interaction.taggedState,
            ctx.expectedOcc64, ctx.physicalOcc64, ctx.humanIllegalMask) != 0ULL) {
        // Both expected pieces are held or on ambiguous landings. Indicate
        // only the placed extras to lift; no global fault is needed.
        ctx.correctionFeedback = modules::CorrectionFeedback::Gentle;
        return;
    }

    ctx.correctionFeedback = modules::CorrectionFeedbackForElapsed(
        ctx.invalidBoard.elapsedMs);
    if (ctx.correctionFeedback == modules::CorrectionFeedback::Strong) {
        // Start strong guidance on-phase, rather than inheriting an arbitrary
        // blink phase from the preceding quiet/gentle interval.
        ctx.invalidBoard.elapsedMs -= modules::CORRECTION_STRONG_MS;
        const uint32_t periodMs = ctx.illegalPeriodMs == 0U
            ? 1U : ctx.illegalPeriodMs;
        ctx.illegalOn = ((ctx.invalidBoard.elapsedMs / periodMs) % 2U) == 0U;
    }
}

void ApplyCorrectionSquarePrompt(const ComposeContext& ctx,
                                 const BoardComposeRuntime& runtime,
                                 const bool occupied, const uint8_t logicalSq,
                                 modules::Rgb& ioFinalSquare,
                                 modules::Rgb& ioPieceRgb,
                                 BoardSquarePromptState& ioPromptState) {
    if (ctx.Snapshot().game.kingResignationArmed &&
        logicalSq == ctx.Snapshot().game.kingResignationSquare) {
        ioFinalSquare = ScaleRgbSaturating(modules::RGB_YELLOW, 96U);
        return;
    }
    if (ctx.correctionFeedback == modules::CorrectionFeedback::Quiet) return;
    if (ctx.correctionFeedback == modules::CorrectionFeedback::Gentle) {
        // The bracket locates the mismatch without pulsing the occupied piece
        // or making the whole status edge announce a fault.
        ioFinalSquare = ScaleRgbSaturating(modules::RGB_ORANGE, 160U);
        return;
    }
    ioPromptState.useMismatchRainbow = true;
    ioPromptState.mismatchRainbowLeds = BuildMismatchRainbowLeds(runtime, logicalSq);
    ioFinalSquare = modules::RepresentativeSquareColor(ioPromptState.mismatchRainbowLeds);
    if (occupied) ioPieceRgb = ioPromptState.mismatchRainbowLeds.top;
}

} // namespace modules::led_frame_composer::internal
