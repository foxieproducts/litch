#include <array>
#include <cstdint>
#include <cstdio>

#include "modules/led_frame_composer_internal.hpp"
#include "modules/settings_store.hpp"
#include "runtime/interaction_domain_types.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

runtime::PhysicalTransitionPlan MakeQuietPlan(const uint64_t before,
                                               const uint8_t from,
                                               const uint8_t to) {
    runtime::PhysicalTransitionPlan plan;
    plan.kind = runtime::PhysicalTransitionKind::QuietMove;
    plan.beforeOccupiedMask = before;
    plan.afterOccupiedMask = (before & ~(1ULL << from)) | (1ULL << to);
    plan.moveFrom = from;
    plan.moveTo = to;
    plan.relocationCount = 1U;
    plan.relocations[0] = {from, to};
    return plan;
}

runtime::InteractionModelSnapshot MakeSnapshot(const uint64_t expected,
                                                const uint64_t physical) {
    runtime::InteractionModelSnapshot snapshot;
    snapshot.board.expectedOccupiedMask = expected;
    snapshot.board.physicalOccupiedMask = physical;
    return snapshot;
}

modules::led_frame_composer::internal::InvalidBoardState Evaluate(
    modules::SettingsStore& settings,
    runtime::InteractionModelSnapshot& snapshot) {
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &settings;
    inputs.interactionSnapshot = &snapshot;
    modules::led_frame_composer::internal::ComposeContext context;
    Check(modules::led_frame_composer::internal::BuildComposeContext(inputs, context),
          "feedback policy builds compose context");
    return modules::led_frame_composer::internal::ComputeInvalidBoardState(context);
}

} // namespace

int main() {
    std::puts("[INTERACTION_FEEDBACK_POLICY] start");

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_interaction_feedback_policy.json";
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "feedback policy settings load");

    constexpr uint8_t FROM = 10U;
    constexpr uint8_t TO = 11U;
    constexpr uint8_t OTHER = 30U;
    constexpr uint64_t BEFORE = (1ULL << FROM) | (1ULL << 20U) |
                                (1ULL << OTHER);
    const runtime::PhysicalTransitionPlan quietPlan =
        MakeQuietPlan(BEFORE, FROM, TO);

    {
        runtime::InteractionModelSnapshot snapshot =
            MakeSnapshot(BEFORE, BEFORE & ~(1ULL << FROM));
        snapshot.interaction.taggedState.tag =
            runtime::ChessInteractionPhase::PieceLifted;
        snapshot.interaction.taggedState.selectedSquare = FROM;
        Check(!Evaluate(settings, snapshot).IsActive(),
              "piece-lifted source delta is legal guidance, not invalid feedback");
    }

    {
        runtime::InteractionModelSnapshot snapshot =
            MakeSnapshot(BEFORE, BEFORE & ~(1ULL << 20U));
        snapshot.interaction.taggedState.tag =
            runtime::ChessInteractionPhase::CaptureStaged;
        snapshot.interaction.taggedState.selectedSquare = 20U;
        Check(!Evaluate(settings, snapshot).IsActive(),
              "target-first capture staging is legal guidance, not invalid feedback");
    }

    constexpr std::array<runtime::ChessInteractionPhase, 3> PLAN_PHASES = {
        runtime::ChessInteractionPhase::ExecutingPlan,
        runtime::ChessInteractionPhase::AwaitingAction,
        runtime::ChessInteractionPhase::Settling,
    };
    for (const runtime::ChessInteractionPhase phase : PLAN_PHASES) {
        runtime::InteractionModelSnapshot snapshot =
            MakeSnapshot(BEFORE, quietPlan.afterOccupiedMask);
        snapshot.interaction.taggedState.tag = phase;
        snapshot.interaction.taggedState.plan = quietPlan;
        Check(!Evaluate(settings, snapshot).IsActive(),
              "plan-controlled delta is not invalid feedback in a legal plan phase");
    }

    {
        runtime::PhysicalTransitionPlan capturePlan = quietPlan;
        capturePlan.kind = runtime::PhysicalTransitionKind::Capture;
        capturePlan.relocations[0].to = 20U;
        capturePlan.moveTo = 20U;
        capturePlan.removalRequired = true;
        capturePlan.removalSquare = 20U;
        capturePlan.afterOccupiedMask = BEFORE & ~(1ULL << FROM);
        runtime::InteractionModelSnapshot snapshot =
            MakeSnapshot(BEFORE, BEFORE & ~(1ULL << 20U));
        snapshot.interaction.taggedState.tag =
            runtime::ChessInteractionPhase::ExecutingPlan;
        snapshot.interaction.taggedState.plan = capturePlan;
        Check(!Evaluate(settings, snapshot).IsActive(),
              "capture-removal progress is guidance, not invalid feedback");
    }

    {
        constexpr uint8_t KING_FROM = 60U;
        constexpr uint8_t KING_TO = 62U;
        constexpr uint8_t ROOK_FROM = 63U;
        constexpr uint8_t ROOK_TO = 61U;
        const uint64_t before = (1ULL << KING_FROM) | (1ULL << ROOK_FROM) |
                                (1ULL << OTHER);
        runtime::PhysicalTransitionPlan castlePlan;
        castlePlan.kind = runtime::PhysicalTransitionKind::Castling;
        castlePlan.beforeOccupiedMask = before;
        castlePlan.afterOccupiedMask =
            (before & ~((1ULL << KING_FROM) | (1ULL << ROOK_FROM))) |
            (1ULL << KING_TO) | (1ULL << ROOK_TO);
        castlePlan.relocationCount = 2U;
        castlePlan.relocations[0] = {KING_FROM, KING_TO};
        castlePlan.relocations[1] = {ROOK_FROM, ROOK_TO};
        runtime::InteractionModelSnapshot snapshot = MakeSnapshot(
            before,
            (before & ~(1ULL << KING_FROM)) | (1ULL << KING_TO));
        snapshot.interaction.taggedState.tag =
            runtime::ChessInteractionPhase::ExecutingPlan;
        snapshot.interaction.taggedState.plan = castlePlan;
        Check(!Evaluate(settings, snapshot).IsActive(),
              "king-first castle progress is guidance, not invalid feedback");
    }

    {
        runtime::InteractionModelSnapshot snapshot =
            MakeSnapshot(BEFORE, quietPlan.afterOccupiedMask & ~(1ULL << OTHER));
        snapshot.interaction.taggedState.tag =
            runtime::ChessInteractionPhase::ExecutingPlan;
        snapshot.interaction.taggedState.plan = quietPlan;
        const auto invalid = Evaluate(settings, snapshot);
        Check(invalid.IsActive() && (invalid.missingMask & (1ULL << OTHER)) != 0ULL,
              "unrelated mismatch remains invalid during legal plan progress");
        Check((invalid.missingMask & (1ULL << FROM)) == 0ULL &&
                  (invalid.extraMask & (1ULL << TO)) == 0ULL,
              "invalid feedback names only the unrelated mismatch");
    }

    {
        runtime::InteractionModelSnapshot snapshot =
            MakeSnapshot(BEFORE, quietPlan.afterOccupiedMask);
        snapshot.interaction.taggedState.tag =
            runtime::ChessInteractionPhase::Correction;
        snapshot.interaction.taggedState.correctionMask =
            (1ULL << FROM) | (1ULL << TO);
        Check(Evaluate(settings, snapshot).IsActive(),
              "authoritative correction phase always emits corrective feedback");
    }

    {
        runtime::InteractionModelSnapshot snapshot =
            MakeSnapshot(BEFORE, quietPlan.afterOccupiedMask);
        snapshot.interaction.taggedState.tag =
            runtime::ChessInteractionPhase::Idle;
        Check(Evaluate(settings, snapshot).IsActive(),
              "unexplained mismatch is invalid outside legal progress");
    }

    if (g_failures == 0) {
        std::puts("[INTERACTION_FEEDBACK_POLICY] ok");
        return 0;
    }
    std::printf("[INTERACTION_FEEDBACK_POLICY] failures=%d\n", g_failures);
    return 1;
}
