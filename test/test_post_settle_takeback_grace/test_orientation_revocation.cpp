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

runtime::GameStateMachineConfig MakeConfig() {
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    return config;
}

void DispatchObservation(runtime::InteractionDispatcher& dispatcher,
                         uint32_t& sequence,
                         const uint64_t before,
                         const uint64_t after) {
    dispatcher.DispatchBoardObservation(runtime::BoardObservationEvent{
        fxe::Millis(), sequence++, before ^ after, after});
}

void CommitOpeningMove(runtime::GameStateMachine& stateMachine,
                       modules::GamePersistenceCoordinator& coordinator,
                       runtime::InteractionDispatcher& dispatcher,
                       uint32_t& sequence) {
    const uint64_t start = stateMachine.GetExpectedOccupancyMask();
    stateMachine.ApplyPhysicalOccupancySnapshot(start);
    const uint64_t lifted = start & ~(1ULL << 52U);
    DispatchObservation(dispatcher, sequence, start, lifted);
    const uint64_t placed = lifted | (1ULL << 44U);
    DispatchObservation(dispatcher, sequence, lifted, placed);
    fxe::ManualMillis::AdvanceMs(11U);
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();
}

void RunSettingMutationCase(const char* const path,
                            const char* const key,
                            const uint32_t value,
                            const bool expectUndo,
                            const char* const message) {
    (void)std::remove(path);
    fxe::ManualMillis::SetNowMs(70000U);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "orientation: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(),
          "orientation: coordinator load");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence = 1U;

    CommitOpeningMove(
        stateMachine, coordinator, dispatcher, sequence);
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "orientation: forward move committed");

    settings.Doc()[key] = value;
    settings.NotifyDocChanged();
    const uint64_t forward = stateMachine.GetExpectedOccupancyMask();
    DispatchObservation(
        dispatcher,
        sequence,
        forward,
        forward & ~(1ULL << 44U));

    const bool undoBegan = stateMachine.IsUndoPromptActive() &&
                           coordinator.HasPendingUndoCommand();
    Check(undoBegan == expectUndo, message);
}

void TestOrientationSettingsRevokeOnlyTheirTransaction() {
    RunSettingMutationCase(
        "build/test_takeback_board_rotate_revocation.json",
        "BOARD_ROTATE",
        1U,
        false,
        "orientation: BOARD_ROTATE revokes the old grace transaction");
    RunSettingMutationCase(
        "build/test_takeback_first_player_revocation.json",
        "GAME_FIRST_PLAYER",
        2U,
        false,
        "orientation: GAME_FIRST_PLAYER revokes the old grace transaction");
    RunSettingMutationCase(
        "build/test_takeback_visual_setting_preserved.json",
        "BRIGHTNESS",
        6U,
        true,
        "orientation: unrelated visual settings preserve grace");
}

} // namespace

int RunPostSettleTakebackOrientationTests() {
    TestOrientationSettingsRevokeOnlyTheirTransaction();
    return g_failures;
}
