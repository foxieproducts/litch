#include "modules/led_frame_composer_internal.hpp"

#include "runtime/directed_physical_prompt.hpp"

namespace modules::led_frame_composer::internal {
namespace {

constexpr uint64_t InitialSquaresMaskForFen(const char pieceFen) {
    switch (pieceFen) {
        case 'P': return 0x00FF000000000000ULL;
        case 'R': return 0x8100000000000000ULL;
        case 'N': return 0x4200000000000000ULL;
        case 'B': return 0x2400000000000000ULL;
        case 'Q': return 0x0800000000000000ULL;
        case 'K': return 0x1000000000000000ULL;
        case 'p': return 0x000000000000FF00ULL;
        case 'r': return 0x0000000000000081ULL;
        case 'n': return 0x0000000000000042ULL;
        case 'b': return 0x0000000000000024ULL;
        case 'q': return 0x0000000000000008ULL;
        case 'k': return 0x0000000000000010ULL;
        default: return 0ULL;
    }
}

modules::SideToMove ExpectedMasksSideToMove(const ComposeContext& ctx) {
    const auto& snapshot = ctx.Snapshot();
    return snapshot.game.sideToMove;
}

void ConfigureAnimationTokens(const ComposeContext& ctx,
                              BoardComposeRuntime& runtime) {
    runtime.globalAnim = modules::animation::AnimationContext{ctx.nowMs};
    runtime.undoAnim = modules::animation::AnimationContext{
        ctx.Snapshot().interaction.undoPromptElapsedMs};
    runtime.illegalAnim = modules::animation::AnimationContext{
        ctx.illegalElapsedMs};

    runtime.faultBlinkToken.phase.periodMs = ctx.illegalPeriodMs * 2U;
    runtime.faultBlinkToken.phase.playback =
        modules::animation::PlaybackMode::Loop;
    runtime.faultBlinkToken.curve.waveform =
        modules::animation::Waveform::Blink;
    runtime.faultBlinkToken.curve.minValue = 0U;
    runtime.faultBlinkToken.curve.maxValue = 255U;
    runtime.faultBlinkToken.curve.onWidth255 = 127U;

    runtime.lastMovePulseToken.phase.periodMs = 2200U;
    runtime.lastMovePulseToken.phase.playback =
        modules::animation::PlaybackMode::Loop;
    runtime.lastMovePulseToken.curve.waveform =
        modules::animation::Waveform::Triangle;
    runtime.lastMovePulseToken.curve.minValue = 0U;
    runtime.lastMovePulseToken.curve.maxValue = 255U;

    runtime.dangerPulseToken.phase.periodMs = 1200U;
    runtime.dangerPulseToken.phase.playback =
        modules::animation::PlaybackMode::Loop;
    runtime.dangerPulseToken.curve.waveform =
        modules::animation::Waveform::DoublePulse;
    runtime.dangerPulseToken.curve.minValue = 100U;
    runtime.dangerPulseToken.curve.maxValue = 255U;
    runtime.dangerPulseToken.curve.onWidth255 = 30U;
    runtime.dangerPulseToken.curve.gapWidth255 = 23U;

}

void ResolveUndoCandidateGuidance(const ComposeContext& ctx,
                                  const bool restoreExpectedMissing,
                                  BoardComposeRuntime& runtime) {
    const auto& snapshot = ctx.Snapshot();
    const bool allowed = ctx.undoPromptActive &&
                         (!ctx.invalidBoard.IsActive() ||
                          (ctx.undoRestorePhase && restoreExpectedMissing &&
                           ctx.undoRestoreValid));
    if (!allowed) {
        return;
    }

    char neededFenPiece = '\0';
    if (ctx.undoMoverPhase && runtime.undoMoverExpectedMissing &&
        ctx.undoMoverPieceFenKnown) {
        neededFenPiece = ctx.undoMoverPieceFen;
        runtime.undoCandidateBlinkRgb = ctx.undoMoverPieceRgb;
    } else if (ctx.undoRestorePhase && restoreExpectedMissing &&
               ctx.undoRestoreValid) {
        neededFenPiece = ctx.undoRestorePieceFen;
        runtime.undoCandidateBlinkRgb = ctx.undoRestorePieceRgb;
    }
    if (neededFenPiece == '\0' || !snapshot.board.enginePiecesValid) {
        return;
    }

    for (uint8_t square = 0; square < 64U; ++square) {
        if ((ctx.physicalOcc64 & (1ULL << square)) != 0ULL &&
            snapshot.board.enginePieces[square] == neededFenPiece) {
            runtime.undoCandidateBlinkMask |= (1ULL << square);
        }
    }
    runtime.undoCandidateBlinkActive = runtime.undoCandidateBlinkMask != 0ULL;
}

} // namespace

BoardComposeRuntime BuildBoardComposeRuntime(const ComposeContext& ctx) {
    const auto& snapshot = ctx.Snapshot();
    BoardComposeRuntime runtime;
    runtime.expectedMoverOwner = OwnerForSideToMove(
        ExpectedMasksSideToMove(ctx), ctx.firstPlayerIsP2);
    runtime.expectedOpponentOwner =
        runtime.expectedMoverOwner == modules::UiOwner::P2
            ? modules::UiOwner::P1
            : modules::UiOwner::P2;
    ConfigureAnimationTokens(ctx, runtime);

    const auto& tokens = modules::animation::LED_FRAME_TOKENS;
    runtime.moverPieceBlinkOn = ctx.minimalAnimations || modules::animation::IsBlinkOn(
        runtime.globalAnim, tokens.moverPieceBlink);
    runtime.undoMoverPieceBlinkOn = ctx.minimalAnimations || modules::animation::IsBlinkOn(
        runtime.undoAnim, tokens.moverPieceBlink);
    runtime.mismatchRainbowBaseWheel = modules::animation::SampleValue(
        modules::animation::AnimationContext{ctx.invalidBoard.elapsedMs},
        tokens.invalidBoardRainbow);

    runtime.undoMoveValid = ctx.undoPromptActive &&
                            ctx.undoFromSquare < 64U &&
                            ctx.undoToSquare < 64U;
    runtime::DirectedPhysicalPrompt directedUndoPrompt;
    const bool directedUndoPromptValid =
        runtime::TryGetDirectedPhysicalPrompt(
            snapshot.interaction.taggedState, directedUndoPrompt);
    const bool primaryUndoPrompt =
        !directedUndoPromptValid ||
        directedUndoPrompt.relocationIndex == 0U;
    runtime.undoMoverExpectedPresent = runtime.undoMoveValid &&
                                       ctx.undoMoverPhase &&
                                       primaryUndoPrompt &&
                                       (ctx.physicalOcc64 &
                                        (1ULL << ctx.undoToSquare)) != 0ULL;
    runtime.undoMoverExpectedMissing = runtime.undoMoveValid &&
                                       ctx.undoMoverPhase &&
                                       primaryUndoPrompt &&
                                       !runtime.undoMoverExpectedPresent;
    const bool restoreExpectedMissing =
        ctx.undoRestorePhase && ctx.undoRestoreValid &&
        ctx.undoRestoreSquare < 64U &&
        (ctx.physicalOcc64 & (1ULL << ctx.undoRestoreSquare)) == 0ULL;

    runtime.undoMoverInitialMask =
        runtime.undoMoverExpectedMissing && ctx.undoMoverPieceFenKnown &&
                !ctx.invalidBoard.IsActive()
            ? InitialSquaresMaskForFen(ctx.undoMoverPieceFen)
            : 0ULL;
    runtime.undoRestoreInitialMask =
        restoreExpectedMissing && !ctx.invalidBoard.IsActive()
            ? InitialSquaresMaskForFen(ctx.undoRestorePieceFen)
            : 0ULL;

    if (ctx.undoPromptActive) {
        runtime.undoCastlingRookMoveValid =
            snapshot.interaction.undoCastlingRookMoveValid;
        runtime.undoCastlingRookFrom =
            snapshot.interaction.undoCastlingRookFrom;
        runtime.undoCastlingRookTo = snapshot.interaction.undoCastlingRookTo;
    }
    ResolveUndoCandidateGuidance(ctx, restoreExpectedMissing, runtime);

    return runtime;
}

} // namespace modules::led_frame_composer::internal
