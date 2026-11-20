#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
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

struct Fixture {
    static constexpr uint8_t FROM = 52U;
    static constexpr uint8_t TO = 36U;

    modules::SettingsStoreConfig settingsConfig;
    runtime::GameStateMachine machine;
    modules::SettingsStore settings;
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator;
    uint32_t sequence = 1U;
    uint64_t before = 0ULL;
    char fenBefore[128] = {};

    Fixture(const char* path, const uint32_t settleDurationMs)
        : settingsConfig(MakeSettingsConfig(path)),
          machine(MakeMachineConfig(settleDurationMs)),
          settings(settingsConfig),
          coordinator(settings, engine, machine) {
        (void)std::remove(path);
        Check(settings.Load(), "pre-ack human undo settings load");
        Check(coordinator.LoadOrInitialize(),
              "pre-ack human undo game load");
        Check(engine.GetFen(fenBefore, sizeof(fenBefore)),
              "read FEN before pre-ack human undo");
        before = machine.GetExpectedOccupancyMask();
        machine.ApplyPhysicalOccupancySnapshot(before);
        sequence = machine.GetLastBoardObservationSequence() + 1U;
        machine.SetValidDestMask(1ULL << TO);
        const uint64_t lifted = before & ~(1ULL << FROM);
        machine.HandleBoardObservation(
            1ULL << FROM, lifted, 1U, sequence++);
        machine.HandleBoardObservation(
            1ULL << TO, lifted | (1ULL << TO), 2U, sequence++);
        fxe::ManualMillis::SetNowMs(settleDurationMs + 2U);
        machine.Tick();
        Check(machine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction,
              "completed human plan waits at the pre-ack boundary");
    }

    static modules::SettingsStoreConfig MakeSettingsConfig(
        const char* path) {
        modules::SettingsStoreConfig config;
        config.path = path;
        config.deferredSaveDelayMs = 999999U;
        return config;
    }

    static runtime::GameStateMachineConfig MakeMachineConfig(
        const uint32_t settleDurationMs) {
        runtime::GameStateMachineConfig config;
        config.settleDurationMs = settleDurationMs;
        config.placementStableRequiredMs = 0U;
        config.cpuIdleDelayMs = 999999U;
        return config;
    }
};

void TestUnclaimedHumanActionIsCanceled() {
    Fixture fixture("build/preack_human_undo.json", 10U);
    Check(fixture.coordinator.RequestUndo(1U),
          "undo cancels an unclaimed human action");
    Check(fixture.machine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction &&
              !fixture.machine.HasCommittedMovePending() &&
              !fixture.machine.IsMoveSettlingActive(),
          "pre-ack human undo enters correction without a stale token");
    fixture.coordinator.Update();
    char fenAfter[128] = {};
    Check(fixture.engine.GetFen(fenAfter, sizeof(fenAfter)) &&
              std::strcmp(fixture.fenBefore, fenAfter) == 0,
          "canceled pre-ack human action never reaches the engine");
}

void TestClaimedHumanActionCannotBeCanceled() {
    Fixture fixture("build/preack_human_undo_claimed.json", 0U);
    runtime::PhysicalTransitionPlan action;
    Check(fixture.machine.TryClaimPendingInteractionMove(action),
          "claim human action before undo safety check");
    Check(!fixture.coordinator.RequestUndo(1U) &&
              fixture.machine.IsMoveActionAwaitingResult(),
          "undo cannot stale an already claimed human action");
}

} // namespace

int main() {
    std::puts("[PREACK_HUMAN_UNDO] start");
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    TestUnclaimedHumanActionIsCanceled();
    fxe::ManualMillis::SetNowMs(0U);
    TestClaimedHumanActionCannotBeCanceled();
    if (gFailures == 0) {
        std::puts("[PREACK_HUMAN_UNDO] ok");
        return 0;
    }
    std::printf("[PREACK_HUMAN_UNDO] failures=%d\n", gFailures);
    return 1;
}
