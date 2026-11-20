#include "modules/led_frame_composer_internal.hpp"
#include "modules/beginner_mode_policy.hpp"

namespace modules::led_frame_composer {
namespace internal {

namespace {

uint32_t EpisodeElapsedMs(const uint32_t nowMs,
                          const bool startValid,
                          const uint32_t startMs) {
    return startValid ? static_cast<uint32_t>(nowMs - startMs) : 0U;
}

} // namespace

RenderIntent BuildRenderIntent(const RenderIntentInputs& inputs) {
    RenderIntent intent;
    if (inputs.snapshot == nullptr) {
        return intent;
    }

    const auto& snapshot = *inputs.snapshot;
    const bool orthodoxOutcomeEnabled =
        snapshot.game.variant != modules::GameVariant::Duck;
    if (snapshot.game.clockWinner != modules::GameWinner::None) {
        intent.celebration = CelebrationIntent::ClockVictory;
        intent.celebrationElapsedMs = static_cast<uint32_t>(
            snapshot.system.nowMs - snapshot.game.clockTerminalStartMs);
    } else if (snapshot.game.variantOutcome.IsTerminal()) {
        intent.celebration = CelebrationIntent::VariantVictory;
        intent.celebrationElapsedMs = static_cast<uint32_t>(
            snapshot.system.nowMs - snapshot.game.variantTerminalStartMs);
    } else if (orthodoxOutcomeEnabled && snapshot.game.inCheckmate) {
        intent.celebration = CelebrationIntent::Checkmate;
        intent.celebrationElapsedMs = EpisodeElapsedMs(
            snapshot.system.nowMs,
            snapshot.game.checkmateStartValid,
            snapshot.game.checkmateStartMs);
    } else if (orthodoxOutcomeEnabled && snapshot.game.inDraw) {
        intent.celebration = CelebrationIntent::Draw;
        intent.celebrationElapsedMs = EpisodeElapsedMs(
            snapshot.system.nowMs,
            snapshot.game.drawStartValid,
            snapshot.game.drawStartMs);
    }

    // Explicit Quick Setup owns presentation until exit; the terminal result
    // and its episode clock remain authoritative in the immutable snapshot.
    if (snapshot.render.quickSettingsOverlayActive) {
        intent.celebration = CelebrationIntent::None;
    }

    const bool fatalGameOver = intent.HasCelebration();
    const bool placementCorrectionActive =
        snapshot.interaction.placementModeActive ||
        snapshot.interaction.kingQueenSwapPromptActive;
    const bool phaseHasCenterOverlay = !snapshot.game.kingResignationArmed &&
        (snapshot.interaction.undoPromptActive ||
        (!placementCorrectionActive &&
         (IsHumanPromotionActive(snapshot.interaction) ||
          IsCpuMovePromptActive(snapshot.interaction))));

    const bool menuRequested = inputs.menuVisualActive || inputs.menuRequested ||
                               snapshot.render.menuOverlayActive ||
                               snapshot.render.quickSettingsOverlayActive;
    intent.menuForegroundVisible = !fatalGameOver && menuRequested;
    intent.menuBackgroundDimVisible = intent.menuForegroundVisible;

    intent.centerTextOverlayRequested = snapshot.render.textOverlayActive || phaseHasCenterOverlay;
    intent.centerTextOverlayVisible = !fatalGameOver &&
                                      intent.centerTextOverlayRequested &&
                                      !intent.menuForegroundVisible;

    intent.hintPromptRequested = snapshot.render.hintDisplayRequested;
    const runtime::TaggedInteractionState& interaction =
        snapshot.interaction.taggedState;
    const bool humanDuckRelocation =
        snapshot.game.variant == modules::GameVariant::Duck &&
        interaction.tag == runtime::ChessInteractionPhase::DuckRelocation &&
        interaction.plan.kind ==
            runtime::PhysicalTransitionKind::DuckRelocation &&
        interaction.plan.source ==
            runtime::PhysicalTransitionSource::Human &&
        interaction.requiredAction !=
            runtime::RequiredPhysicalAction::CorrectBoard;
    intent.hintPromptVisible = !fatalGameOver && !snapshot.game.kingResignationArmed &&
                               !snapshot.game.initialVariantValidationActive &&
                               !placementCorrectionActive &&
                               !intent.menuForegroundVisible &&
                               !intent.centerTextOverlayVisible &&
                               (IsIdleInteraction(snapshot) ||
                                (snapshot.render.hintMoveValid &&
                                 IsMatchingHumanHintPickup(
                                     interaction, snapshot.render.hintMoveFromSquare)) ||
                                (snapshot.game.variant == modules::GameVariant::Duck &&
                                 IsMatchingDuckHintChessProgress(
                                     interaction, snapshot.render.hintMoveFromSquare,
                                     snapshot.render.hintMoveToSquare)) ||
                                humanDuckRelocation) &&
                               intent.hintPromptRequested;

    intent.coachingOverlayActive = !fatalGameOver && intent.hintPromptVisible;
    return intent;
}

} // namespace internal
} // namespace modules::led_frame_composer
