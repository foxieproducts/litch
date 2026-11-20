#include <cstdio>

#include "runtime/app_event.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void AdvanceAndTick(runtime::ManualClock& clock, runtime::GameStateMachine& sm, const uint32_t deltaMs) {
    clock.AdvanceMs(deltaMs);
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeTick(clock.NowMs()));
}

void Sensor(runtime::ManualClock& clock, runtime::GameStateMachine& sm, const uint8_t square, const bool occ) {
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeSensorDelta(clock.NowMs(), square, occ));
}

void PressP1(runtime::ManualClock& clock, runtime::GameStateMachine& sm) {
    runtime::DispatchAppEventToStateMachine(
        sm, runtime::AppEvent::MakeButton(clock.NowMs(), modules::UserButton::P1X, fxe::Button::Event::Press));
}

} // namespace

int main() {
    std::puts("[GAME_STATE_MACHINE_CPU_THINKING] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    runtime::ManualClock clock;

    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 20;
    cfg.settleFlashMs = 10;
    cfg.cpuIdleDelayMs = 100000;
    cfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    // Seed a simple expected/physical occupancy with a piece at 12.
    sm.SetExpectedOccupancyMask(1ULL << 12);
    sm.DebugSetPhysicalOccupancyMask(1ULL << 12);

    sm.SetCpuThinkingActive(true);
    Check(sm.IsCpuThinkingActive(), "thinking flag is set");

    // While thinking, observations update correction state without starting a
    // human move.
    Sensor(clock, sm, 12, false);
    Sensor(clock, sm, 28, true);
    for (uint16_t i = 0; i < 129U; ++i) {
        Sensor(clock, sm, 31, (i % 2U) == 0U);
    }
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "thinking-time sensor deltas remain an explicit correction");
    Check((sm.GetChessInteractionModel().board.occupiedMask & (1ULL << 31)) != 0ULL,
          "thinking retains the latest observation without a bounded replay queue");

    // Interacting during CPU thinking should provide invalid feedback.
    Check(sm.HasHumanIllegal(), "interaction during thinking marks illegal");

    // Ending thinking does not replay old deltas into move transitions.
    sm.SetCpuThinkingActive(false);
    Check(!sm.IsCpuThinkingActive(), "thinking flag clears");
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "ending thinking retains the correction without replaying a move");

    AdvanceAndTick(clock, sm, 25);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "tick retains the correction");

    // Clear the illegal markers by correcting the occupancy.
    Sensor(clock, sm, 28, false);
    Sensor(clock, sm, 31, false);
    Sensor(clock, sm, 12, true);
    Check(!sm.HasHumanIllegal(), "corrections clear illegal markers");

    // Perform a normal move (not during CPU thinking) so undo/cancel-undo is meaningful.
    Sensor(clock, sm, 12, false);
    Sensor(clock, sm, 28, true);
    AdvanceAndTick(clock, sm, 25);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "normal move settle exposes its action");

    // While undo prompt is active, cancel-undo should remain available even if
    // a sensor delta arrives during thinking.
    sm.Reset();
    sm.SetExpectedOccupancyMask(1ULL << 28U);
    sm.DebugSetPhysicalOccupancyMask(1ULL << 28U);
    Check(sm.BeginUndoPrompt(12U, 28U,
                             modules::PersistedSide::Unknown,
                             '\0', '\0', 0U),
          "explicit undo prompt starts");

    sm.SetCpuThinkingActive(true);
    Sensor(clock, sm, 7, true);
    Check(sm.IsUndoPromptActive(),
          "thinking-time sensor delta keeps reducer-owned undo prompt");

    if (g_failures == 0) {
        std::puts("[GAME_STATE_MACHINE_CPU_THINKING] ok");
        return 0;
    }

    std::printf("[GAME_STATE_MACHINE_CPU_THINKING] failures=%d\n", g_failures);
    return 1;
}
