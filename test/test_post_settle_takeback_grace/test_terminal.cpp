#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

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

modules::SettingsStoreConfig MakeSettingsConfig(const char* const path) {
    modules::SettingsStoreConfig config;
    config.path = path;
    config.deferredSaveDelayMs = 999999U;
    return config;
}

struct Fixture {
    explicit Fixture(const char* const path)
        : settingsConfig(MakeSettingsConfig(path)),
          stateConfig(MakeConfig()),
          settings(settingsConfig),
          stateMachine(stateConfig),
          coordinator(settings, engine, stateMachine),
          dispatcher(stateMachine, coordinator) {
        (void)std::remove(path);
        stateMachine.Reset();
    }

    bool Load(const char* const fen) {
        if (!settings.Load()) {
            return false;
        }
        settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
        if (!coordinator.LoadOrInitialize() ||
            !coordinator.LoadExternalFen(fen)) {
            return false;
        }
        stateMachine.ApplyPhysicalOccupancySnapshot(
            stateMachine.GetExpectedOccupancyMask());
        stateMachine.Tick();
        sequence = stateMachine.GetLastBoardObservationSequence() + 1U;
        return stateMachine.IsExpectedOccupancyMatched();
    }

    modules::SettingsStoreConfig settingsConfig;
    runtime::GameStateMachineConfig stateConfig;
    modules::SettingsStore settings;
    runtime::GameStateMachine stateMachine;
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator;
    runtime::InteractionDispatcher dispatcher;
    uint32_t sequence = 1U;
};

void Observe(Fixture& fixture,
             const uint64_t before,
             const uint64_t after) {
    fixture.dispatcher.DispatchBoardObservation(
        runtime::BoardObservationEvent{
            fxe::Millis(), fixture.sequence++, before ^ after, after});
}

void SettleMove(Fixture& fixture,
                const uint8_t from,
                const uint8_t to) {
    const uint64_t start =
        fixture.stateMachine.GetExpectedOccupancyMask();
    const uint64_t lifted = start & ~(1ULL << from);
    Observe(fixture, start, lifted);
    Observe(fixture, lifted, lifted | (1ULL << to));
    fxe::ManualMillis::AdvanceMs(11U);
    fixture.dispatcher.DispatchTick(fxe::Millis());
    fixture.coordinator.Update();
}

void BeginGraceUndo(Fixture& fixture, const uint8_t destination) {
    const uint64_t forward =
        fixture.stateMachine.GetExpectedOccupancyMask();
    Observe(fixture, forward, forward & ~(1ULL << destination));
    Check(fixture.stateMachine.IsUndoPromptActive() &&
              fixture.coordinator.HasPendingUndoCommand(),
          "terminal: immediate inverse enters positive Undo guidance");
    fixture.coordinator.Update();
}

void TestClassicMateCanBeTakenBack() {
    fxe::ManualMillis::SetNowMs(70000U);
    Fixture fixture("build/test_takeback_terminal_mate.json");
    static constexpr char FEN[] =
        "7k/5Qpp/8/8/8/8/8/K7 w - - 0 1";
    Check(fixture.Load(FEN), "terminal mate: fixture loads");
    char before[160] = {};
    Check(fixture.engine.GetFen(before, sizeof(before)),
          "terminal mate: pre-move FEN reads");

    SettleMove(fixture, 13U, 5U);
    Check(fixture.coordinator.GetEngineDerivedState().inCheckmate,
          "terminal mate: quiet queen move produces checkmate");

    BeginGraceUndo(fixture, 5U);
    Check(!fixture.coordinator.GetEngineDerivedState().inCheckmate,
          "terminal mate: inverse command clears terminal presentation");
    const uint64_t lifted =
        fixture.stateMachine.GetPhysicalOccupancyMask();
    Observe(fixture, lifted, lifted | (1ULL << 13U));
    fixture.coordinator.Update();

    char after[160] = {};
    Check(fixture.engine.GetFen(after, sizeof(after)) &&
              std::strcmp(after, before) == 0 &&
              fixture.coordinator.GetUndoHistoryMoveCount() == 0U,
          "terminal mate: guided inverse restores pre-mate position");
}

void TestPromotionMateCancelRedoRestoresTerminalState() {
    fxe::ManualMillis::SetNowMs(80000U);
    Fixture fixture("build/test_takeback_terminal_promotion.json");
    static constexpr char FEN[] =
        "7k/5Ppp/8/8/8/8/8/K7 w - - 0 1";
    Check(fixture.Load(FEN), "promotion mate: fixture loads");
    fixture.settings.Doc()["PROMOTION_MODE"] = "choose";

    SettleMove(fixture, 13U, 5U);
    Check(fixture.stateMachine.IsPromotionActive(),
          "promotion mate: choice is requested before commit");
    fixture.stateMachine.HandleButtonEvent(
        modules::UserButton::P1O, fxe::Button::Event::Press);
    fixture.coordinator.Update();
    Check(fixture.coordinator.GetEngineDerivedState().inCheckmate,
          "promotion mate: default queen produces checkmate");
    char terminalFen[160] = {};
    Check(fixture.engine.GetFen(terminalFen, sizeof(terminalFen)),
          "promotion mate: terminal FEN reads");

    BeginGraceUndo(fixture, 5U);
    Check(!fixture.coordinator.GetEngineDerivedState().inCheckmate &&
              fixture.coordinator.IsCancelUndoAvailable(),
          "promotion mate: inverse clears mate and keeps X cancel available");
    Check(fixture.coordinator.RequestUndo(1U),
          "promotion mate: X cancel requests forward redo");
    fixture.coordinator.Update();

    char redoneFen[160] = {};
    Check(fixture.engine.GetFen(redoneFen, sizeof(redoneFen)) &&
              std::strcmp(redoneFen, terminalFen) == 0 &&
              fixture.coordinator.GetEngineDerivedState().inCheckmate &&
              fixture.coordinator.GetUndoHistoryMoveCount() == 1U,
          "promotion mate: cancel-redo restores promotion and terminal state");
}

} // namespace

int RunPostSettleTakebackTerminalTests() {
    TestClassicMateCanBeTakenBack();
    TestPromotionMateCancelRedoRestoresTerminalState();
    return g_failures;
}
