#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "foxie_embedded/millis.hpp"

#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

runtime::GameStateMachineConfig MakeConfig() {
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 100U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    return config;
}

void TestCastleSettleDoesNotConsumeUndo() {
    constexpr const char* FEN =
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";
    constexpr uint64_t PICKUP = (1ULL << 60U) | (1ULL << 63U);
    constexpr uint64_t PLACEMENT = (1ULL << 61U) | (1ULL << 62U);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_post_ack_castle_undo.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "castle undo settings load");
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "castle undo game load");
    Check(coordinator.LoadExternalFen(FEN), "castle undo FEN load");
    const uint64_t before = stateMachine.GetExpectedOccupancyMask();
    stateMachine.ApplyPhysicalOccupancySnapshot(before);
    stateMachine.Tick();
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;
    const uint64_t lifted = before & ~PICKUP;
    stateMachine.HandleBoardObservation(
        PICKUP, lifted, 1000U, sequence++);
    stateMachine.HandleBoardObservation(
        PLACEMENT, lifted | PLACEMENT, 1001U, sequence++);
    coordinator.Update();
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle &&
              stateMachine.IsMoveSettlingActive(),
          "acknowledged castle retains only visual settle");
    Check(coordinator.RequestUndo(1U),
          "undo during acknowledged castle settle is accepted");
    Check(!stateMachine.IsMoveSettlingActive() &&
              stateMachine.IsUndoPromptActive() &&
              coordinator.HasPendingUndoCommand(),
          "castle settle ends and same request opens history undo");
}

void TestCpuSettleDoesNotConsumeUndo() {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t TO = 36U;
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_post_ack_cpu_undo.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "CPU undo settings load");
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "CPU undo game load");
    const uint64_t before = stateMachine.GetExpectedOccupancyMask();
    stateMachine.ApplyPhysicalOccupancySnapshot(before);
    Check(stateMachine.QueueCpuMove(FROM, TO, false, 0U, '\0'),
          "queue CPU undo fixture");
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;
    const uint64_t lifted = before & ~(1ULL << FROM);
    stateMachine.HandleBoardObservation(
        1ULL << FROM, lifted, 2000U, sequence++);
    stateMachine.HandleBoardObservation(
        1ULL << TO, lifted | (1ULL << TO), 2001U, sequence++);
    stateMachine.Tick();
    coordinator.Update();
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle &&
              stateMachine.IsCpuMoveSettlingActive(),
          "acknowledged CPU move retains only visual settle");
    Check(coordinator.RequestUndo(1U),
          "undo during acknowledged CPU settle is accepted");
    Check(!stateMachine.IsMoveSettlingActive() &&
              stateMachine.IsUndoPromptActive() &&
              coordinator.HasPendingUndoCommand(),
          "CPU settle ends and same request opens history undo");
}

} // namespace

int main() {
    std::puts("[POST_ACK_SETTLE_UNDO] start");
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    TestCastleSettleDoesNotConsumeUndo();
    TestCpuSettleDoesNotConsumeUndo();
    if (g_failures == 0) {
        std::puts("[POST_ACK_SETTLE_UNDO] ok");
        return 0;
    }
    std::printf("[POST_ACK_SETTLE_UNDO] failures=%d\n", g_failures);
    return 1;
}
