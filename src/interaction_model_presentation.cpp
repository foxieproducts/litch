#include "runtime/interaction_model_presentation.hpp"

#include "runtime/game_state_machine.hpp"

namespace runtime {
namespace {

HintPresentationTuple CurrentTuple(const RenderStateView& render) {
    return {render.hintMoveFromSquare, render.hintMoveToSquare,
        render.hintDuckMoveValid ? render.hintDuckMoveFromSquare : uint8_t{255U},
        render.hintDuckMoveValid ? render.hintDuckMoveToSquare : uint8_t{255U}};
}

void SampleHintRefinement(const InteractionModelPresentation& published,
                          const uint32_t nowMs,
                          RenderStateView& render) {
    auto refinement = published.hintRefinement;
    AdvanceHintRefinement(refinement, CurrentTuple(published.snapshot.render), nowMs);
    render.hintRefinementActive = refinement.elapsed.Started();
    if (!render.hintRefinementActive) return;
    render.hintRefinementElapsedMs = nowMs - refinement.elapsed.StartMs();
    render.hintRefinementDurationMs = refinement.durationMs;
    // The only queued handoff is the previously incoming tuple -> latest.
    // Capture-square evidence belongs to those same immutable legal tuples.
    if (refinement.elapsed.StartMs() != published.hintRefinement.elapsed.StartMs()) {
        render.previousHintCaptureSquare = render.incomingHintCaptureSquare;
        render.incomingHintCaptureSquare = render.hintCaptureSquare;
    }
    render.previousHintFromSquare = refinement.outgoing.fromSquare;
    render.previousHintToSquare = refinement.outgoing.toSquare;
    render.previousHintDuckFromSquare = refinement.outgoing.duckFromSquare;
    render.previousHintDuckToSquare = refinement.outgoing.duckToSquare;
    render.incomingHintFromSquare = refinement.incoming.fromSquare;
    render.incomingHintToSquare = refinement.incoming.toSquare;
    render.incomingHintDuckFromSquare = refinement.incoming.duckFromSquare;
    render.incomingHintDuckToSquare = refinement.incoming.duckToSquare;
}

} // namespace

InteractionModelPresentation CaptureInteractionModelPresentation(
    const GameStateMachine& stateMachine,
    const InteractionModelSnapshot& snapshot) {
    InteractionModelPresentation destination;
    CaptureInteractionModelPresentation(stateMachine, snapshot, destination);
    return destination;
}
void CaptureInteractionModelPresentation(const GameStateMachine& stateMachine,
    const InteractionModelSnapshot& snapshot, InteractionModelPresentation& destination) {
    const auto& render = stateMachine.GetRenderState();
    destination.snapshot = snapshot;
    destination.turnIndicator = render.turnIndicator;
    destination.thinkingControls = render.thinkingControls;
    destination.hintBrewing = render.hintBrewing;
    destination.hintRefinement = render.hintRefinement;
    destination.allPiecesFlash = render.allPiecesFlash;
    destination.captureReturnPeriodMs = stateMachine.GetCaptureReturnFlashPeriodMs();
    destination.settleFlashPeriodMs = stateMachine.GetSettleFlashPeriodMs();
}

InteractionModelSnapshot SampleInteractionModelPresentation(
    const InteractionModelPresentation& published,
    const uint32_t nowMs) {
    auto sampled = published.snapshot;
    const uint32_t elapsed = nowMs - published.snapshot.system.nowMs;
    sampled.system.nowMs = nowMs;
    auto& render = sampled.render;
    auto& interaction = sampled.interaction;
    render.turnScale256 = published.turnIndicator.Scale256(nowMs);
    render.turnElapsedMs = published.turnIndicator.motion.Started()
        ? nowMs - published.turnIndicator.motion.StartMs() : 0U;
    render.thinkingControlScale256 = published.thinkingControls.Scale256(nowMs);
    render.thinkingControlElapsedMs = published.thinkingControls.ElapsedMs(nowMs);
    render.hintBrewingScale256 = published.hintBrewing.Scale256(nowMs);
    render.hintBrewingElapsedMs = published.hintBrewing.ElapsedMs(nowMs);
    if (render.hintFeedbackTimingValid) render.hintFeedbackElapsedMs += elapsed;
    if (render.hintRemovalSquare < 64U) render.hintRemovalElapsedMs += elapsed;
    if (render.previousHintRemovalSquare < 64U) render.previousHintRemovalElapsedMs += elapsed;
    SampleHintRefinement(published, nowMs, render);
    render.allPiecesFlashActive = published.allPiecesFlash.IsActive(nowMs);
    render.allPiecesFlashOn = published.allPiecesFlash.IsOn(nowMs);
    if (render.initialKingPlacementPromptActive) render.initialKingPlacementPromptElapsedMs += elapsed;
    if (render.settleAfterglowActive) {
        render.settleAfterglowElapsedMs += elapsed;
        render.settleAfterglowActive = render.settleAfterglowElapsedMs < SETTLE_AFTERGLOW_MS;
    }
    render.settleVisualElapsedMs += elapsed;
    if (render.settleTimingValid) {
        const uint32_t remaining = render.settleDurationMs > render.settleElapsedMs
            ? render.settleDurationMs - render.settleElapsedMs : 0U;
        render.settleElapsedMs += elapsed < remaining ? elapsed : remaining;
        render.settleOn = SampleSettleFlashOn(render.settleElapsedMs,
            render.settleDurationMs, published.settleFlashPeriodMs);
    } else if (render.humanMoveSettlingActive || render.cpuMoveSettlingActive) {
        render.settleOn = ((nowMs / published.settleFlashPeriodMs) % 2U) == 0U;
    }
    interaction.undoPromptElapsedMs += elapsed;
    interaction.promotionElapsedMs += elapsed;
    interaction.cpuMovePromptElapsedMs += elapsed;
    interaction.illegalElapsedMs += elapsed;
    interaction.illegalOn = ((interaction.illegalElapsedMs / interaction.illegalPeriodMs) % 2U) == 0U;
    interaction.captureReturnElapsedMs += elapsed;
    interaction.captureReturnOn = ((interaction.captureReturnElapsedMs /
        published.captureReturnPeriodMs) % 2U) == 0U;
    interaction.lastInteractionElapsedMs += elapsed;
    sampled.game.kingResignationElapsedMs += elapsed;
    auto& game = sampled.game;
    if (game.clockEnabled && game.clockRunning && !game.clockPaused) {
        auto& remaining = game.clockActiveSide == ClockSide::White
            ? game.clockWhiteRemainingMs : game.clockBlackRemainingMs;
        remaining = remaining > elapsed ? remaining - elapsed : 0U;
    }
    return sampled;
}

} // namespace runtime
