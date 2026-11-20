#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/quick_settings_overlay_controller.hpp"
#include "modules/settings_store.hpp"
#include "runtime/app_event.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/pending_undo_command_handoff.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

void Sensor(runtime::ManualClock& clock,
            runtime::GameStateMachine& stateMachine,
            const uint8_t square,
            const bool occupied) {
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    runtime::DispatchAppEventToStateMachine(
        stateMachine,
        runtime::AppEvent::MakeSensorDelta(
            clock.NowMs(), square, occupied));
}

void DoMove(runtime::ManualClock& clock,
            runtime::GameStateMachine& stateMachine,
            const uint8_t from,
            const uint8_t to,
            const uint32_t settleMs) {
    stateMachine.DebugSetPhysicalOccupancyMask(
        stateMachine.GetExpectedOccupancyMask());
    Sensor(clock, stateMachine, from, false);
    Sensor(clock, stateMachine, to, true);
    clock.AdvanceMs(settleMs + 1U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    runtime::DispatchAppEventToStateMachine(
        stateMachine,
        runtime::AppEvent::MakeTick(clock.NowMs()));
}

bool GetFen(const modules::IEngine& engine,
            char* const out,
            const size_t outSize) {
    return out != nullptr && outSize != 0U && engine.GetFen(out, outSize);
}

void RemoveSettingsFiles(const char* const path,
                         const char* const backupPath,
                         const char* const temporaryPath) {
    (void)std::remove(path);
    (void)std::remove(backupPath);
    (void)std::remove(temporaryPath);
}

runtime::GameStateMachineConfig MakeStateMachineConfig() {
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    return config;
}

void TestCancelUndoHandoffRendersBeforeBlockingCommand() {
    fxe::ManualMillis::SetNowMs(3000U);
    const runtime::PendingUndoCommandHandoffState state{
        true,
        runtime::MoveRenderTransitionKind::CancelUndo,
        false};
    uint8_t sequence = 0U;
    uint8_t renderSequence = 0U;
    uint8_t consumeSequence = 0U;
    uint32_t renderAtMs = 0U;

    runtime::RunPendingUndoCommandHandoff(
        state,
        [&]() {
            renderSequence = ++sequence;
            renderAtMs = fxe::Millis();
            return true;
        },
        [&]() {
            consumeSequence = ++sequence;
            fxe::ManualMillis::AdvanceMs(200U);
        });

    Check(renderSequence == 1U && consumeSequence == 2U,
          "cancel handoff: render precedes blocking command");
    Check(renderAtMs == 3000U,
          "cancel handoff: persistence latency cannot delay first frame");

    bool rendered = false;
    bool consumed = false;
    runtime::RunPendingUndoCommandHandoff(
        {true, runtime::MoveRenderTransitionKind::None, false},
        [&]() { rendered = true; return true; },
        [&]() { consumed = true; });
    Check(!rendered && consumed,
          "undo handoff: ordinary pending command consumes without extra frame");

    rendered = false;
    consumed = false;
    runtime::RunPendingUndoCommandHandoff(
        {true, runtime::MoveRenderTransitionKind::CancelUndo, true},
        [&]() { rendered = true; return true; },
        [&]() { consumed = true; });
    Check(!rendered && consumed,
          "cancel handoff: suppressed output skips frame but consumes command");

    rendered = false;
    consumed = false;
    const bool ready = runtime::RunPendingUndoCommandHandoff(
        state,
        [&]() { rendered = true; return false; },
        [&]() { consumed = true; });
    Check(rendered && !consumed && !ready,
          "cancel handoff: denied frame defers synchronous restoration");
}

void TestImmediateCancelDoesNotQueueWork() {
    constexpr const char* path =
        "build/test_undo_coordinator_immediate_cancel.json";
    RemoveSettingsFiles(
        path,
        "build/test_undo_coordinator_immediate_cancel.json.bak",
        "build/test_undo_coordinator_immediate_cancel.json.tmp");

    runtime::ManualClock clock(1000U);
    const runtime::GameStateMachineConfig stateConfig =
        MakeStateMachineConfig();
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 100000U;
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "immediate cancel: settings load");
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(),
          "immediate cancel: coordinator load");

    DoMove(clock, stateMachine, 52U, 44U, stateConfig.settleDurationMs);
    coordinator.Update();
    char forwardFen[160] = {};
    Check(GetFen(engine, forwardFen, sizeof(forwardFen)),
          "immediate cancel: forward fen");

    Check(coordinator.RequestUndo(1U),
          "immediate cancel: first request accepted");
    Check(coordinator.HasPendingUndoCommand(),
          "immediate cancel: first request schedules engine command");
    Check(stateMachine.IsUndoPromptActive(),
          "immediate cancel: prompt opened");
    Check(coordinator.RequestUndo(1U),
          "immediate cancel: second request accepted");
    Check(!coordinator.HasPendingUndoCommand(),
          "immediate cancel: collapsed request clears engine command");
    Check(!stateMachine.IsUndoPromptActive(),
          "immediate cancel: prompt closed");
    Check(stateMachine.GetRenderState().moveTransition.kind ==
              runtime::MoveRenderTransitionKind::None,
          "immediate cancel: no stale render transition");
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "immediate cancel: history untouched");

    coordinator.Update();
    char afterFen[160] = {};
    Check(GetFen(engine, afterFen, sizeof(afterFen)),
          "immediate cancel: final fen");
    Check(std::strcmp(afterFen, forwardFen) == 0,
          "immediate cancel: engine untouched");

    Check(coordinator.RequestUndo(1U),
          "applied cancel: undo request accepted");
    coordinator.Update();
    modules::QuickSettingsOverlayController quickSettings;
    modules::QuickSettingsAction cancelAction;
    const modules::ButtonEvent cancelPress{
        modules::UserButton::P1X,
        fxe::Button::Event::Press,
        modules::UiOwner::P1};
    Check(quickSettings.HandleButtonEvent(
              cancelPress,
              false,
              coordinator.IsCancelUndoAvailable(),
              cancelAction,
              clock.NowMs()),
          "applied cancel: X press is consumed");
    Check(cancelAction.type == modules::QuickSettingsActionType::UndoTap,
          "applied cancel: X press requests immediate cancel");
    Check(coordinator.RequestUndo(1U),
          "applied cancel: press-time cancel request accepted");
    Check(!stateMachine.IsUndoPromptActive(),
          "applied cancel: prompt closes before X release");
    Check(coordinator.HasPendingUndoCommand(),
          "applied cancel: engine command remains pending");
    coordinator.Update();
    Check(!coordinator.HasPendingUndoCommand(),
          "applied cancel: engine command is consumed");
    char restoredFen[160] = {};
    Check(GetFen(engine, restoredFen, sizeof(restoredFen)) &&
              std::strcmp(restoredFen, forwardFen) == 0,
          "applied cancel: forward engine position restored");
}

void TestUndoCancelsOnlyUncommittedSettle() {
    constexpr const char* path =
        "build/test_undo_coordinator_settle_cancel.json";
    RemoveSettingsFiles(
        path,
        "build/test_undo_coordinator_settle_cancel.json.bak",
        "build/test_undo_coordinator_settle_cancel.json.tmp");

    runtime::ManualClock clock(2000U);
    const runtime::GameStateMachineConfig stateConfig =
        MakeStateMachineConfig();
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 100000U;
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "settle cancel: settings load");
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "settle cancel: coordinator load");

    DoMove(clock, stateMachine, 52U, 44U, stateConfig.settleDurationMs);
    coordinator.Update();
    char forwardFen[160] = {};
    Check(GetFen(engine, forwardFen, sizeof(forwardFen)),
          "settle cancel: forward fen");

    Sensor(clock, stateMachine, 11U, false);
    Sensor(clock, stateMachine, 27U, true);
    Check(stateMachine.IsMoveSettlingActive(),
          "settle cancel: preview entered settle");
    Check(coordinator.RequestUndo(1U),
          "settle cancel: request accepted");
    Check((stateMachine.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle),
          "settle cancel: preview returned to idle");
    Check(!stateMachine.IsUndoPromptActive(),
          "settle cancel: no history undo prompt");
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "settle cancel: prior history preserved");

    coordinator.Update();
    char afterFen[160] = {};
    Check(GetFen(engine, afterFen, sizeof(afterFen)),
          "settle cancel: final fen");
    Check(std::strcmp(afterFen, forwardFen) == 0,
          "settle cancel: engine untouched");
}

} // namespace

int main() {
    std::puts("[UNDO_COORDINATOR_AUTHORITY] start");
    fxe::ScopedMillisProviderOverride millisGuard(
        &fxe::ManualMillisProvider);

    TestCancelUndoHandoffRendersBeforeBlockingCommand();
    TestImmediateCancelDoesNotQueueWork();
    TestUndoCancelsOnlyUncommittedSettle();

    if (g_failures == 0) {
        std::puts("[UNDO_COORDINATOR_AUTHORITY] ok");
        return 0;
    }
    std::printf("[UNDO_COORDINATOR_AUTHORITY] failures=%d\n", g_failures);
    return 1;
}
