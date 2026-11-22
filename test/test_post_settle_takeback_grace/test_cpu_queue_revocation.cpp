#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

void TestCanceledCpuQueueDoesNotReviveOldGrace() {
    constexpr const char* path =
        "build/test_takeback_cpu_queue_revocation.json";
    (void)std::remove(path);
    fxe::ManualMillis::SetNowMs(80000U);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 10U;
    stateConfig.cpuIdleDelayMs = 20U;
    stateConfig.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(stateConfig);
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "CPU revocation: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    settings.Doc()["VS_CPU_LEVEL"] = 0U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(),
          "CPU revocation: coordinator load");

    stateMachine.DebugSetPhysicalOccupancyMask(
        stateMachine.GetExpectedOccupancyMask());
    stateMachine.HandleSensorChange(52U, false);
    coordinator.UpdateLegalDestinationsMask();
    stateMachine.HandleSensorChange(44U, true);
    fxe::ManualMillis::AdvanceMs(11U);
    stateMachine.Tick();
    coordinator.Update();
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "CPU revocation: human move arms one history transaction");

    settings.Doc()["VS_CPU_LEVEL"] = 1U;
    settings.NotifyDocChanged();
    fxe::ManualMillis::AdvanceMs(21U);
    stateMachine.Tick();
    coordinator.Update();
    Check(stateMachine.HasQueuedCpuMove(),
          "CPU revocation: live CPU response is queued");

    settings.Doc()["VS_CPU_LEVEL"] = 0U;
    settings.NotifyDocChanged();
    coordinator.Update();
    Check(!stateMachine.HasQueuedCpuMove(),
          "CPU revocation: ownership change cancels queued response");

    runtime::InteractionDispatcher dispatcher(
        stateMachine, coordinator);
    const uint64_t forward = stateMachine.GetExpectedOccupancyMask();
    dispatcher.DispatchBoardObservation(runtime::BoardObservationEvent{
        fxe::Millis(),
        stateMachine.GetLastBoardObservationSequence() + 1U,
        1ULL << 44U,
        forward & ~(1ULL << 44U)});
    Check(!stateMachine.IsUndoPromptActive() &&
              !coordinator.HasPendingUndoCommand(),
          "CPU revocation: canceled queue cannot revive prior grace");
}

} // namespace

int RunPostSettleTakebackCpuQueueTests() {
    TestCanceledCpuQueueDoesNotReviveOldGrace();
    return g_failures;
}
