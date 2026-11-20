#include <cstdio>

#include "modules/led_frame_composer_internal.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    runtime::InteractionModelSnapshot snapshot{};

    {
        snapshot.render.hintDisplayRequested = true;

        const auto intent = modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &snapshot,
             .menuRequested = false,
             .menuVisualActive = false,
             .hintMoveValid = true});

        Check(intent.hintPromptVisible, "idle hint request becomes visible intent");
        Check(intent.coachingOverlayActive, "visible hint counts as coaching overlay");
    }

    {
        snapshot.game.hintThinking = true;

        const auto intent = modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &snapshot,
             .menuRequested = false,
             .menuVisualActive = false,
             .hintMoveValid = false});

        Check(intent.hintPromptRequested, "early Hint request retains visibility intent while awaiting a result");
    }

    {
        snapshot.render.textOverlayActive = true;
        snapshot.render.menuOverlayActive = true;
        snapshot.game.inCheckmate = true;
        snapshot.game.inDraw = true;

        const auto intent = modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &snapshot,
             .menuRequested = true,
             .menuVisualActive = false,
             .hintMoveValid = false});

        Check(!intent.menuForegroundVisible, "checkmate suppresses lower-priority menu foreground");
        Check(!intent.menuBackgroundDimVisible, "checkmate suppresses lower-priority menu dimming");
        Check(!intent.centerTextOverlayVisible, "fatal intent blocks center text visibility");
        Check(intent.celebration == modules::led_frame_composer::internal::CelebrationIntent::Checkmate,
              "checkmate outranks draw in celebration intent");
        Check(intent.celebrationElapsedMs == 0U,
              "missing episode start produces deterministic zero elapsed time");
    }

    {
        snapshot.render.menuOverlayActive = false;
        snapshot.render.textOverlayActive = false;
        snapshot.system.nowMs = 5400U;
        snapshot.game.checkmateStartValid = true;
        snapshot.game.checkmateStartMs = 5000U;

        const auto intent = modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &snapshot,
             .menuRequested = false,
             .menuVisualActive = false,
             .hintMoveValid = false});

        Check(intent.celebration == modules::led_frame_composer::internal::CelebrationIntent::Checkmate,
              "checkmate celebration becomes visible once menu intent clears");
        Check(intent.celebrationElapsedMs == 400U,
              "checkmate intent exposes episode-relative elapsed time");
    }

    {
        snapshot.game.inCheckmate = false;
        snapshot.game.inDraw = false;
        snapshot.interaction.taggedState.tag =
            runtime::ChessInteractionPhase::ExecutingPlan;
        snapshot.interaction.taggedState.plan.source =
            runtime::PhysicalTransitionSource::Cpu;
        snapshot.interaction.placementModeActive = true;

        const auto intent = modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &snapshot});

        Check(!intent.centerTextOverlayRequested,
              "placement correction suppresses the CPU center overlay");
    }

    {
        snapshot.interaction.placementModeActive = false;
        snapshot.interaction.undoPromptActive = true;

        const auto intent = modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &snapshot});

        Check(intent.centerTextOverlayRequested,
              "undo explicitly requests its center overlay");
    }

    if (g_failures == 0) {
        std::puts("[LED_RENDER_INTENT] ok");
        return 0;
    }

    std::printf("[LED_RENDER_INTENT] failures=%d\n", g_failures);
    return 1;
}
