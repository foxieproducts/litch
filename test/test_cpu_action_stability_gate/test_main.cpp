#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"

#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/chess_interaction_persistence.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

bool ReadFen(modules::IEngine& engine, char (&out)[128]) {
    return engine.GetFen(out, sizeof(out));
}

void RemoveSettingsFiles(const char* path) {
    (void)std::remove(path);
    char sibling[192] = {};
    (void)std::snprintf(sibling, sizeof(sibling), "%s.bak", path);
    (void)std::remove(sibling);
    (void)std::snprintf(sibling, sizeof(sibling), "%s.tmp", path);
    (void)std::remove(sibling);
}

void TestDefaultStabilityDelaysCpuAction() {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t TO = 36U;
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_cpu_default_stability.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    RemoveSettingsFiles(settingsConfig.path);

    runtime::GameStateMachine stateMachine;
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "default stability settings load");
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "default stability game load");
    const uint64_t before = stateMachine.GetExpectedOccupancyMask();
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    Check(stateMachine.QueueCpuMove(FROM, TO, false, 0U, '\0'),
          "queue default-stability CPU move");

    uint64_t physical = before & ~(1ULL << FROM);
    stateMachine.HandleBoardObservation(1ULL << FROM, physical, 1000U, 1U);
    coordinator.Update();
    physical |= 1ULL << TO;
    stateMachine.HandleBoardObservation(1ULL << TO, physical, 1001U, 2U);

    char beforeFen[128] = {};
    Check(ReadFen(engine, beforeFen), "read FEN before stable CPU action");
    coordinator.Update();
    char immediateFen[128] = {};
    Check(ReadFen(engine, immediateFen) &&
              std::strcmp(beforeFen, immediateFen) == 0,
          "coordinator cannot claim CPU action immediately after placement");
    Check(stateMachine.HasQueuedCpuMove() &&
              stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan,
          "CPU plan remains queued and unclaimable during stability window");

    fxe::ManualMillis::SetNowMs(1201U);
    stateMachine.Tick();
    coordinator.Update();
    char appliedFen[128] = {};
    Check(ReadFen(engine, appliedFen) &&
              std::strcmp(beforeFen, appliedFen) != 0,
          "stable CPU action applies once after the default 200ms gate");
    Check(!stateMachine.HasQueuedCpuMove(),
          "stable CPU action clears its queue");
    coordinator.Update();
    char stableFen[128] = {};
    Check(ReadFen(engine, stableFen) &&
              std::strcmp(appliedFen, stableFen) == 0,
          "stable CPU action is not applied twice");
}

void TestCpuCaptureOrdering(const bool targetFirst) {
    constexpr const char* FEN =
        "7k/8/8/3p4/4P3/8/8/7K b - - 0 1";
    constexpr uint8_t FROM = 27U;
    constexpr uint8_t TO = 36U;
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 0U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = targetFirst
                              ? "build/test_cpu_capture_target_first.json"
                              : "build/test_cpu_capture_mover_first.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    RemoveSettingsFiles(settingsConfig.path);
    runtime::GameStateMachine stateMachine(config);
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "capture ordering settings load");
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "capture ordering game load");
    Check(coordinator.LoadExternalFen(FEN), "capture ordering FEN load");
    const uint64_t before = stateMachine.GetExpectedOccupancyMask();
    stateMachine.ApplyPhysicalOccupancySnapshot(before);
    stateMachine.Tick();
    Check(!stateMachine.IsPlacementModeActive(),
          "matching capture board exits placement before CPU prompt");
    Check(stateMachine.QueueCpuMove(FROM, TO, true, TO, '\0'),
          "queue CPU capture ordering move");

    char beforeFen[128] = {};
    Check(ReadFen(engine, beforeFen), "read CPU capture FEN before steps");
    uint64_t physical = before;
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;
    const uint8_t first = targetFirst ? TO : FROM;
    const uint8_t second = targetFirst ? FROM : TO;
    physical &= ~(1ULL << first);
    stateMachine.HandleBoardObservation(
        1ULL << first, physical, 2000U, sequence++);
    stateMachine.Tick();
    coordinator.Update();
    char intermediateFen[128] = {};
    Check(ReadFen(engine, intermediateFen) &&
              std::strcmp(beforeFen, intermediateFen) == 0 &&
              stateMachine.HasQueuedCpuMove(),
          "first CPU capture step cannot claim action");

    physical &= ~(1ULL << second);
    stateMachine.HandleBoardObservation(
        1ULL << second, physical, 2001U, sequence++);
    stateMachine.Tick();
    coordinator.Update();
    Check(ReadFen(engine, intermediateFen) &&
              std::strcmp(beforeFen, intermediateFen) == 0 &&
              stateMachine.HasQueuedCpuMove(),
          "second CPU capture step cannot claim action");

    physical |= 1ULL << TO;
    stateMachine.HandleBoardObservation(
        1ULL << TO, physical, 2002U, sequence++);
    stateMachine.Tick();
    coordinator.Update();
    char appliedFen[128] = {};
    Check(ReadFen(engine, appliedFen) &&
              std::strcmp(beforeFen, appliedFen) != 0,
          "complete CPU capture applies after all physical steps");
    Check(!stateMachine.HasQueuedCpuMove(),
          "complete CPU capture clears queue");
}

void TestCpuMixedIllegalObservationRecoversPlan() {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t TO = 36U;
    constexpr uint8_t THIRD = 51U;
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 0U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_cpu_mixed_illegal_recovery.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    RemoveSettingsFiles(settingsConfig.path);
    runtime::GameStateMachine stateMachine(config);
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "mixed CPU settings load");
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "mixed CPU game load");
    const uint64_t before = stateMachine.GetExpectedOccupancyMask();
    stateMachine.ApplyPhysicalOccupancySnapshot(before);
    Check(stateMachine.QueueCpuMove(FROM, TO, false, 0U, '\0'),
          "queue mixed-observation CPU move");

    const uint64_t from = 1ULL << FROM;
    const uint64_t to = 1ULL << TO;
    const uint64_t third = 1ULL << THIRD;
    const uint64_t mixed = (before & ~from & ~third) | to;
    uint32_t sequence = stateMachine.GetLastBoardObservationSequence() + 1U;
    stateMachine.HandleBoardObservation(
        from | to | third, mixed, 3000U, sequence++);
    Check(stateMachine.HasCpuIllegal() && stateMachine.HasQueuedCpuMove(),
          "mixed CPU completion latches only the unrelated change");
    stateMachine.HandleBoardObservation(
        third, mixed | third, 3001U, sequence++);
    Check(!stateMachine.HasCpuIllegal() &&
              !stateMachine.HasQueuedCpuMove() &&
              stateMachine.HasCommittedMovePending(),
          "correcting unrelated square reconciles and exposes CPU action");
    coordinator.Update();
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "reconciled CPU plan commits exactly once");
    coordinator.Update();
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "reconciled CPU plan cannot commit twice");
}

void TestCpuCaptureTargetRestoreProgress() {
    constexpr uint8_t FROM = 12U;
    constexpr uint8_t TO = 28U;
    constexpr uint64_t BEFORE = (1ULL << FROM) | (1ULL << TO);
    runtime::GameStateMachineConfig config;
    config.placementStableRequiredMs = 200U;
    config.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine machine(config);
    machine.HandleInteractionPositionAcknowledged(
        BEFORE, 0ULL, 0ULL, 0ULL, BEFORE);
    machine.ApplyPhysicalOccupancySnapshot(BEFORE);
    Check(machine.QueueCpuMove(FROM, TO, true, TO, '\0'),
          "queue CPU capture target-restore fixture");
    uint32_t sequence = machine.GetLastBoardObservationSequence() + 1U;
    machine.HandleBoardObservation(
        1ULL << TO, 1ULL << FROM, 4000U, sequence++);
    runtime::ChessInteractionDurableState removedDurable;
    Check(runtime::ExportChessInteractionDurableState(
              machine.GetChessInteractionModel(), removedDurable),
          "export CPU capture after target removal");
    machine.HandleBoardObservation(
        1ULL << TO, BEFORE, 4001U, sequence++);
    machine.HandleBoardObservation(
        1ULL << FROM, 1ULL << TO, 4002U, sequence++);
    Check(machine.GetChessInteractionModel().phase.requiredAction ==
              runtime::RequiredPhysicalAction::RemoveCapture,
          "restored CPU target must be removed after mover lift");

    runtime::GameStateMachine restored(config);
    restored.HandleInteractionPositionAcknowledged(
        BEFORE, 0ULL, 0ULL, 0ULL, BEFORE);
    Check(restored.RestoreInteractionDurableState(removedDurable),
          "restore CPU capture target-removal state");
    Check(restored.QueueCpuMove(FROM, TO, true, TO, '\0'),
          "reconnect durable CPU capture queue");
    restored.ApplyPhysicalOccupancySnapshot(BEFORE);
    restored.HandleSensorChange(FROM, false);
    Check(restored.GetChessInteractionModel().phase.requiredAction ==
              runtime::RequiredPhysicalAction::RemoveCapture,
          "boot scan clears stale CPU target-removal progress");
}

void TestDurableCpuCaptureAfterPlacementStability() {
    constexpr uint8_t FROM = 12U;
    constexpr uint8_t TO = 28U;
    constexpr uint64_t BEFORE = (1ULL << FROM) | (1ULL << TO);
    constexpr uint64_t AFTER = 1ULL << TO;
    runtime::GameStateMachineConfig config;
    config.placementStableRequiredMs = 200U;
    config.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine machine(config);
    machine.HandleInteractionPositionAcknowledged(
        BEFORE, 0ULL, 0ULL, 0ULL, BEFORE);
    machine.ApplyPhysicalOccupancySnapshot(BEFORE);
    Check(machine.QueueCpuMove(FROM, TO, true, TO, '\0'),
          "queue durable post-placement CPU capture");
    uint32_t sequence = machine.GetLastBoardObservationSequence() + 1U;
    machine.HandleBoardObservation(1ULL << TO,
                                   1ULL << FROM,
                                   5000U,
                                   sequence++);
    machine.HandleBoardObservation(1ULL << FROM,
                                   0ULL,
                                   5001U,
                                   sequence++);
    machine.HandleBoardObservation(1ULL << TO,
                                   AFTER,
                                   5002U,
                                   sequence++);
    Check(machine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::ExecutingPlan &&
              machine.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::None,
          "completed CPU capture remains unclaimable during stability");
    runtime::ChessInteractionDurableState durable;
    Check(runtime::ExportChessInteractionDurableState(
              machine.GetChessInteractionModel(), durable),
          "export CPU capture in post-placement stability window");

    fxe::ManualMillis::SetNowMs(6000U);
    runtime::GameStateMachine restored(config);
    restored.HandleInteractionPositionAcknowledged(
        BEFORE, 0ULL, 0ULL, 0ULL, BEFORE);
    Check(restored.RestoreInteractionDurableState(durable),
          "restore CPU capture post-placement window");
    Check(restored.QueueCpuMove(FROM, TO, true, TO, '\0'),
          "reconnect restored post-placement CPU queue");
    restored.ApplyPhysicalOccupancySnapshot(AFTER);
    Check(restored.GetChessInteractionModel().phase.requiredAction ==
              runtime::RequiredPhysicalAction::None &&
              restored.HasQueuedCpuMove(),
          "authoritative boot scan reconstructs completed CPU placement");
    restored.Tick();
    Check(!restored.HasCommittedMovePending(),
          "restored CPU capture still waits for stability");
    fxe::ManualMillis::AdvanceMs(200U);
    restored.Tick();
    runtime::PhysicalTransitionPlan action;
    Check(restored.TryClaimPendingInteractionMove(action) &&
              !restored.TryClaimPendingInteractionMove(action),
          "restored stable CPU capture exposes exactly one action");
}

} // namespace

int main() {
    std::puts("[CPU_ACTION_STABILITY_GATE] start");
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    TestDefaultStabilityDelaysCpuAction();
    TestCpuCaptureOrdering(true);
    TestCpuCaptureOrdering(false);
    TestCpuMixedIllegalObservationRecoversPlan();
    TestCpuCaptureTargetRestoreProgress();
    TestDurableCpuCaptureAfterPlacementStability();
    if (g_failures == 0) {
        std::puts("[CPU_ACTION_STABILITY_GATE] ok");
        return 0;
    }
    std::printf("[CPU_ACTION_STABILITY_GATE] failures=%d\n", g_failures);
    return 1;
}
