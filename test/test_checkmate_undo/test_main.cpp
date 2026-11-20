#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "modules/vs_cpu_turn.hpp"
#include "runtime/app_event.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

bool GetFen(const modules::IEngine& engine, char* out, const size_t outSize) {
    if (out == nullptr || outSize == 0U) {
        return false;
    }
    out[0] = '\0';
    return engine.GetFen(out, outSize);
}

void Sensor(runtime::ManualClock& clock,
            runtime::GameStateMachine& stateMachine,
            const uint8_t square,
            const bool occupied) {
    runtime::DispatchAppEventToStateMachine(
        stateMachine,
        runtime::AppEvent::MakeSensorDelta(clock.NowMs(), square, occupied));
}

void DoMove(runtime::ManualClock& clock,
            runtime::GameStateMachine& stateMachine,
            const uint8_t from,
            const uint8_t to,
            const uint32_t settleMs) {
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    stateMachine.ApplyPhysicalOccupancySnapshot(
        stateMachine.GetExpectedOccupancyMask());
    Sensor(clock, stateMachine, from, false);
    Sensor(clock, stateMachine, to, true);
    clock.AdvanceMs(settleMs + 1U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    runtime::DispatchAppEventToStateMachine(
        stateMachine, runtime::AppEvent::MakeTick(clock.NowMs()));
}

runtime::GameStateMachineConfig MakeStateMachineConfig() {
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    return config;
}

void VerifySinglePlyUndoFromCheckmate(runtime::ManualClock& clock) {
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_checkmate_undo_single.json";
    settingsConfig.deferredSaveDelayMs = 100000U;
    (void)std::remove(settingsConfig.path);
    (void)std::remove("build/test_checkmate_undo_single.json.bak");
    (void)std::remove("build/test_checkmate_undo_single.json.tmp");

    const runtime::GameStateMachineConfig stateConfig = MakeStateMachineConfig();
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "single-ply settings load");
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "single-ply coordinator initialize");

    DoMove(clock, stateMachine, 53U, 45U, stateConfig.settleDurationMs); // f2 -> f3
    coordinator.Update();
    DoMove(clock, stateMachine, 12U, 28U, stateConfig.settleDurationMs); // e7 -> e5
    coordinator.Update();
    DoMove(clock, stateMachine, 54U, 38U, stateConfig.settleDurationMs); // g2 -> g4
    coordinator.Update();

    char fenBeforeMate[160] = {};
    Check(GetFen(engine, fenBeforeMate, sizeof(fenBeforeMate)), "FEN before mate");
    DoMove(clock, stateMachine, 3U, 39U, stateConfig.settleDurationMs); // Qd8 -> h4#
    coordinator.Update();
    Check(coordinator.GetEngineDerivedState().inCheckmate, "Fool's Mate detected");

    Check(coordinator.RequestUndo(1U), "single-ply undo starts from checkmate");
    coordinator.Update();
    char fenAfterUndo[160] = {};
    Check(GetFen(engine, fenAfterUndo, sizeof(fenAfterUndo)), "FEN after mate undo");
    Check(std::strcmp(fenBeforeMate, fenAfterUndo) == 0,
          "single-ply undo restores the pre-mate FEN");
    Check(!coordinator.GetEngineDerivedState().inCheckmate,
          "single-ply undo clears checkmate state");
}

void VerifyHumanVsCpuMateStopsAfterOnePly(runtime::ManualClock& clock) {
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_checkmate_undo_vs_cpu.json";
    settingsConfig.deferredSaveDelayMs = 100000U;
    (void)std::remove(settingsConfig.path);
    (void)std::remove("build/test_checkmate_undo_vs_cpu.json.bak");
    (void)std::remove("build/test_checkmate_undo_vs_cpu.json.tmp");

    const runtime::GameStateMachineConfig stateConfig = MakeStateMachineConfig();
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "VS-CPU settings load");
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "VS-CPU coordinator initialize");

    DoMove(clock, stateMachine, 52U, 36U, stateConfig.settleDurationMs); // e2 -> e4
    coordinator.Update();
    DoMove(clock, stateMachine, 13U, 21U, stateConfig.settleDurationMs); // f7 -> f6
    coordinator.Update();
    DoMove(clock, stateMachine, 51U, 35U, stateConfig.settleDurationMs); // d2 -> d4
    coordinator.Update();
    DoMove(clock, stateMachine, 14U, 30U, stateConfig.settleDurationMs); // g7 -> g5
    coordinator.Update();
    DoMove(clock, stateMachine, 59U, 31U, stateConfig.settleDurationMs); // Qd1 -> h5#
    coordinator.Update();
    Check(coordinator.GetEngineDerivedState().inCheckmate, "human checkmate detected");

    const uint8_t undoCount = modules::GetVsCpuUndoPlyCount(
        coordinator.GetEngineDerivedState().sideToMove, 1U, false);
    Check(undoCount == 1U, "human checkmate selects one-ply VS-CPU undo");
    Check(coordinator.RequestUndo(undoCount), "VS-CPU checkmate undo starts");
    coordinator.Update();
    Sensor(clock, stateMachine, 31U, false);
    Sensor(clock, stateMachine, 59U, true);
    Check(!stateMachine.IsUndoPromptActive(),
          "human checkmate undo stops after the mating move");
}

} // namespace

int main() {
    std::puts("[CHECKMATE_UNDO] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    runtime::ManualClock clock(1000U);

    VerifySinglePlyUndoFromCheckmate(clock);
    VerifyHumanVsCpuMateStopsAfterOnePly(clock);

    if (g_failures == 0) {
        std::puts("[CHECKMATE_UNDO] ok");
        return 0;
    }
    std::printf("[CHECKMATE_UNDO] failures=%d\n", g_failures);
    return 1;
}
