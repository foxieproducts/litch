#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"

namespace {

int gFailures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++gFailures;
    }
}

void RunUnclaimedCase(const uint32_t settleDurationMs) {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t TO = 36U;
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = settleDurationMs == 0U
                              ? "build/preack_cpu_undo_zero.json"
                              : "build/preack_cpu_undo_visual.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    modules::SettingsStore settings(settingsConfig);
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = settleDurationMs;
    config.placementStableRequiredMs = 0U;
    config.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine machine(config);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "pre-ack CPU undo settings load");
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, machine);
    Check(coordinator.LoadOrInitialize(), "pre-ack CPU undo game load");
    char fenBefore[128] = {};
    Check(engine.GetFen(fenBefore, sizeof(fenBefore)),
          "read FEN before pre-ack CPU undo");
    const uint64_t before = machine.GetExpectedOccupancyMask();
    machine.ApplyPhysicalOccupancySnapshot(before);
    Check(machine.QueueCpuMove(FROM, TO, false, 0U, '\0'),
          "queue pre-ack CPU undo fixture");
    uint32_t sequence = machine.GetLastBoardObservationSequence() + 1U;
    const uint64_t lifted = before & ~(1ULL << FROM);
    machine.HandleBoardObservation(
        1ULL << FROM, lifted, 1U, sequence++);
    machine.HandleBoardObservation(
        1ULL << TO, lifted | (1ULL << TO), 2U, sequence++);
    machine.Tick();
    Check(machine.IsMoveActionAwaitingResult(),
          "completed CPU plan waits at the pre-ack boundary");
    Check(coordinator.RequestUndo(1U),
          "undo cancels an unclaimed CPU action");
    Check(machine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction &&
              !machine.HasCommittedMovePending() &&
              !machine.IsMoveSettlingActive(),
          "pre-ack CPU undo enters correction without a stale token");
    coordinator.Update();
    char fenAfter[128] = {};
    Check(engine.GetFen(fenAfter, sizeof(fenAfter)) &&
              std::strcmp(fenBefore, fenAfter) == 0,
          "canceled pre-ack CPU action never reaches the engine");
}

void TestClaimedCase() {
    constexpr uint8_t FROM = 12U;
    constexpr uint8_t TO = 28U;
    constexpr uint64_t BEFORE = 1ULL << FROM;
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 0U;
    config.placementStableRequiredMs = 0U;
    runtime::GameStateMachine machine(config);
    machine.HandleInteractionPositionAcknowledged(
        BEFORE, 0ULL, 0ULL, 0ULL, BEFORE);
    machine.ApplyPhysicalOccupancySnapshot(BEFORE);
    Check(machine.QueueCpuMove(FROM, TO, false, 0U, '\0'),
          "queue claimed CPU undo fixture");
    uint32_t sequence = machine.GetLastBoardObservationSequence() + 1U;
    machine.HandleBoardObservation(1ULL << FROM, 0ULL, 1U, sequence++);
    machine.HandleBoardObservation(
        1ULL << TO, 1ULL << TO, 2U, sequence++);
    machine.Tick();
    runtime::PhysicalTransitionPlan action;
    Check(machine.TryClaimPendingInteractionMove(action),
          "claim CPU action before undo safety check");
    Check(!machine.CancelUnclaimedMoveActionForUndo() &&
              machine.IsMoveActionAwaitingResult(),
          "undo cannot stale an already claimed CPU action");
}

} // namespace

int main() {
    std::puts("[PREACK_CPU_UNDO] start");
    RunUnclaimedCase(100U);
    RunUnclaimedCase(0U);
    TestClaimedCase();
    if (gFailures == 0) {
        std::puts("[PREACK_CPU_UNDO] ok");
        return 0;
    }
    std::printf("[PREACK_CPU_UNDO] failures=%d\n", gFailures);
    return 1;
}
