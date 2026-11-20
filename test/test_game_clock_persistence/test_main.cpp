#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstdlib>

#include <ArduinoJson.h>

#include "foxie_embedded/test_support/manual_millis_guard.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "modules/volatile_persistence_session.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/runtime_health.hpp"

int RunSecondClockEvaluationTimeoutRegression();

namespace {

constexpr const char* PLAYABLE_FEN =
    "8/8/8/8/8/8/4P3/4K2k w - - 0 1";
constexpr uint64_t PLAYABLE_OCCUPANCY =
    (1ULL << 52U) | (1ULL << 60U) | (1ULL << 63U);

uint32_t g_persistenceBoundaryMillisCalls = 0U;

uint32_t PersistenceBoundaryMillisProvider() {
    return g_persistenceBoundaryMillisCalls++ < 2U ? 60760U : 60761U;
}

void StartOpeningClockAtE2(
    runtime::ManualClock& clock,
    runtime::GameStateMachine& stateMachine,
    modules::GamePersistenceCoordinator& coordinator) {
    clock.AdvanceMs(750U);
    coordinator.Update();
    stateMachine.HandleSensorChange(52U, false);
    coordinator.Update();
}

void RemoveSettingsFiles(const char* path) {
    (void)std::remove(path);

    char derived[128] = {};
    (void)std::snprintf(derived, sizeof(derived), "%s.bak", path);
    (void)std::remove(derived);
    (void)std::snprintf(derived, sizeof(derived), "%s.tmp", path);
    (void)std::remove(derived);
}

modules::SettingsStoreConfig SettingsConfig(const char* path) {
    modules::SettingsStoreConfig cfg;
    cfg.path = path;
    cfg.deferredSaveDelayMs = 1000U;
    cfg.mustSaveDelayMs = 100U;
    return cfg;
}

runtime::GameStateMachineConfig StateMachineConfig() {
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 10U;
    cfg.cpuIdleDelayMs = 999999U;
    cfg.placementStableRequiredMs = 0U;
    return cfg;
}

runtime::RuntimeHealthSnapshot HealthyRuntime() {
    return runtime::RuntimeHealthSnapshot{
        .state = runtime::RuntimeHealthState::Healthy,
        .causes = 0U,
        .revision = 1U,
        .advice = runtime::RuntimeHealthAdvice{
            .boardInputAllowed = true,
            .moveAcceptanceAllowed = true,
            .clocksAllowed = true,
        },
    };
}

runtime::RuntimeHealthSnapshot PausedRuntime() {
    return runtime::RuntimeHealthSnapshot{
        .state = runtime::RuntimeHealthState::InputPaused,
        .causes = runtime::RuntimeHealthCauseMask(
            runtime::RuntimeHealthCause::BoardScanUncertain),
        .revision = 2U,
        .advice = runtime::RuntimeHealthAdvice{
            .boardInputAllowed = false,
            .moveAcceptanceAllowed = false,
            .clocksAllowed = false,
        },
    };
}

runtime::RuntimeHealthSnapshot DegradedRuntime() {
    return runtime::RuntimeHealthSnapshot{
        .state = runtime::RuntimeHealthState::DegradedUsable,
        .causes = runtime::RuntimeHealthCauseMask(
            runtime::RuntimeHealthCause::BrownoutLatched),
        .revision = 3U,
        .advice = runtime::RuntimeHealthAdvice{
            .boardInputAllowed = true,
            .moveAcceptanceAllowed = true,
            .clocksAllowed = true,
        },
    };
}


void DisabledClockBlackToMoveSurvivesLifecycleReboots(
    fxe::test_support::TestResults& results) {
    // Exact last-captured V3.3 position before the firmware update. Its Home
    // record already showed a disabled White clock while Black was to move.
    constexpr const char* fen =
        "2r5/1p2b1k1/pB1p1nbp/1p1Pp1p1/1Pr1P1P1/P1PQ1P1P/2R3K1/4R3 b - - 0 1";
    constexpr const char* path = "build/t_clock_off_black.json";
    RemoveSettingsFiles(path);
    const auto config = SettingsConfig(path);
    unsigned identities = 0U;
    const auto identity = [](void* context, modules::game_history::GameId& id) {
        id = {};
        id.bytes[0U] = static_cast<uint8_t>(++*static_cast<unsigned*>(context));
        return true;
    };
    modules::GamePersistenceState original;
    {
        modules::SettingsStore settings(config);
        results.Check(settings.Load(), "untimed seed settings load");
        settings.Doc()["TIMER"] = 0U;
        settings.Doc()["BRIGHTNESS"] = 3U;
        runtime::GameStateMachine sm(StateMachineConfig());
        sm.Reset();
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator game(settings, engine, sm);
        game.SetGameHistoryIdentityGenerator(identity, &identities);
        results.Check(game.LoadOrInitialize() && game.LoadExternalFen(fen) &&
                          game.FlushToSettingsNow(),
                      "real engine and lifecycle flush seed the captured black-to-move game");
        results.Check(modules::ReadGamePersistenceState(settings.Doc(), original) &&
                          original.clockStatePresent && !original.clockState.clock.enabled &&
                          original.clockState.clock.activeSide == runtime::ClockSide::Black &&
                          original.gameHistory.present && original.hasAcceptedMove,
                      "initial untimed record is valid Black with retained game identity");
    }
    for (unsigned cycle = 1U; cycle <= 2U; ++cycle) {
        modules::SettingsStore settings(config);
        results.Check(settings.Load(), "untimed lifecycle reloads actual saved JSON");
        runtime::GameStateMachine sm(StateMachineConfig());
        sm.Reset();
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator game(settings, engine, sm);
        game.SetGameHistoryIdentityGenerator(identity, &identities);
        auto degradation = modules::PersistenceDegradation::None;
        const bool loaded = modules::LoadGameForPersistenceSession(settings, game, degradation);
        const bool sameFen = std::strcmp(game.GetAuthoritativeFen(), fen) == 0;
        results.Check(loaded && sameFen &&
                          game.GetGameVariant() == modules::GameVariant::Classic &&
                          degradation == modules::PersistenceDegradation::None,
                      "untimed Black game survives consecutive lifecycle reboots without quarantine");
        const auto liveSide = game.GetGameClock().GetActiveSide();
        results.Check(game.FlushToSettingsNow(), "untimed lifecycle snapshot durably saves");
        modules::SettingsStore verified(config);
        modules::GamePersistenceState saved;
        const bool readback = verified.Load() &&
            modules::ReadGamePersistenceState(verified.Doc(), saved);
        results.Check(readback && saved.fen.Equals(fen) && saved.hasAcceptedMove &&
                          saved.gameHistory.gameId == original.gameHistory.gameId &&
                          saved.gameHistory.lineRevision == original.gameHistory.lineRevision &&
                          saved.gameHistory.currentPly == original.gameHistory.currentPly &&
                          !saved.clockState.clock.enabled &&
                          saved.clockState.sessionId == original.clockState.sessionId &&
                          saved.clockState.durableEpoch > original.clockState.durableEpoch &&
                          verified.ReadValue("BRIGHTNESS", 0U) == 3U,
                      "untimed durable readback retains position, provenance, identity and preferences");
        std::printf("[UNTIMED_BLACK_REBOOT] cycle=%u restored_fen=%u live_clock=%c "
                    "saved_fen=%u saved_clock=%c quarantine=%u identity_same=%u\n",
                    cycle, unsigned(sameFen), liveSide == runtime::ClockSide::Black ? 'b' : 'w',
                    unsigned(readback && saved.fen.Equals(fen)),
                    saved.clockState.clock.activeSide == runtime::ClockSide::Black ? 'b' : 'w',
                    unsigned(degradation != modules::PersistenceDegradation::None),
                    unsigned(saved.gameHistory.gameId == original.gameHistory.gameId));
    }
    // Enabled clocks still bind to the engine's side, and malformed disabled
    // payloads still fail the ordinary clock decoder before restoration.
    modules::SettingsStore negative(config);
    results.Check(negative.Load(), "clock side guard settings load");
    auto enabled = original;
    enabled.clockState.timerSetting = 1U;
    enabled.clockState.baseMs = 60000U;
    enabled.clockState.clock.enabled = true;
    enabled.clockState.clock.activeSide = runtime::ClockSide::White;
    enabled.clockState.clock.remainingMsWhite = 60000U;
    enabled.clockState.clock.remainingMsBlack = 60000U;
    negative.Doc()["TIMER"] = 1U;
    results.Check(modules::WriteGamePersistenceState(negative.Doc(), enabled),
                  "enabled side mismatch remains a structurally valid clock fixture");
    runtime::GameStateMachine sm(StateMachineConfig());
    sm.Reset();
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator game(negative, engine, sm);
    results.Check(!game.LoadOrInitialize(), "enabled clock side mismatch still rejects restore");
    auto malformed = original;
    malformed.clockState.started = true;
    results.Check(!modules::WriteGamePersistenceState(negative.Doc(), malformed),
                  "disabled clock with started state remains invalid");
    RemoveSettingsFiles(path);
}

} // namespace

int main() {
    fxe::test_support::TestResults results("GAME_CLOCK_PERSISTENCE");
    results.Start();
    fxe::test_support::ManualMillisGuard millisGuard;
    runtime::ManualClock clock(1000U);
    DisabledClockBlackToMoveSurvivesLifecycleReboots(results);

    // The fixed-size V1 record round-trips every clock authority field.
    {
        StaticJsonDocument<2048> doc;
        modules::GamePersistenceState state;
        results.Check(state.fen.Assign(PLAYABLE_FEN), "schema fixture FEN fits");
        state.clockStatePresent = true;
        state.clockState.sessionId = 17U;
        state.clockState.durableEpoch = 29U;
        state.clockState.timerSetting = 2U;
        state.clockState.baseMs = 180000U;
        state.clockState.started = true;
        state.clockState.clock = runtime::GameClockState{
            .enabled = true,
            .running = true,
            .paused = false,
            .timedOut = false,
            .activeSide = runtime::ClockSide::Black,
            .timedOutSide = runtime::ClockSide::White,
            .remainingMsWhite = 172345U,
            .remainingMsBlack = 179876U,
            .incrementMs = 2000U,
        };

        results.Check(modules::WriteGamePersistenceState(doc, state),
                      "GAME_CLOCK_V1 writes with game snapshot");
        const JsonVariantConst encoded = doc[modules::KEY_GAME_CLOCK_V1];
        results.Check(encoded.is<JsonArrayConst>() &&
                          encoded.as<JsonArrayConst>().size() == 11U,
                      "GAME_CLOCK_V1 is a bounded 11-field record");

        modules::GamePersistenceState restored;
        results.Check(modules::ReadGamePersistenceState(doc, restored),
                      "GAME_CLOCK_V1 validates on read");
        results.Check(restored.clockStatePresent &&
                          restored.clockState.sessionId == 17U &&
                          restored.clockState.durableEpoch == 29U &&
                          restored.clockState.timerSetting == 2U &&
                          restored.clockState.baseMs == 180000U &&
                          restored.clockState.started &&
                          restored.clockState.clock.running &&
                          !restored.clockState.clock.paused &&
                          restored.clockState.clock.activeSide ==
                              runtime::ClockSide::Black &&
                          restored.clockState.clock.remainingMsWhite == 172345U &&
                          restored.clockState.clock.remainingMsBlack == 179876U &&
                          restored.clockState.clock.incrementMs == 2000U,
                      "GAME_CLOCK_V1 round-trips exact state");

        doc[modules::KEY_GAME_CLOCK_V1][0] = 2U;
        results.Check(!modules::ReadGamePersistenceState(doc, restored),
                      "unsupported present clock version fails closed");

        results.Check(modules::WriteGamePersistenceState(doc, state),
                      "valid clock record rewrites after version test");
        doc[modules::KEY_GAME_CLOCK_V1][4] = 60000U;
        results.Check(!modules::ReadGamePersistenceState(doc, restored),
                      "timer/base configuration mismatch fails closed");

        results.Check(modules::WriteGamePersistenceState(doc, state),
                      "valid clock record rewrites after config test");
        doc[modules::KEY_GAME_CLOCK_V1][8] = "bad";
        results.Check(!modules::ReadGamePersistenceState(doc, restored),
                      "malformed remaining-time field fails closed");
    }

    // A legacy game with no clock record uses the selected preset, paused,
    // and cannot resume from cached occupancy or unhealthy runtime evidence.
    {
        constexpr const char* path = "build/t_clock_legacy.json";
        RemoveSettingsFiles(path);
        const modules::SettingsStoreConfig cfg = SettingsConfig(path);
        {
            modules::SettingsStore settings(cfg);
            results.Check(settings.Load(), "legacy settings load");
            settings.Doc()["TIMER"] = 2U;
            settings.Doc()[modules::KEY_GAME_FEN] = PLAYABLE_FEN;
            results.Check(settings.SaveNow(), "legacy fixture save");
        }

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "legacy settings reload");
        runtime::GameStateMachine sm(StateMachineConfig());
        sm.Reset();
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
        results.Check(coordinator.LoadOrInitialize(),
                      "legacy game without clock record restores");
        results.Check(coordinator.GetGameClock().IsRunning() &&
                          coordinator.GetGameClock().IsPaused() &&
                          coordinator.GetGameClock().GetRemainingMs(
                              runtime::ClockSide::White) == 180000U &&
                          coordinator.IsRestoredClockResumeAuthorizationPending(),
                      "legacy preset restores live but paused");

        sm.DebugSetPhysicalOccupancyMask(PLAYABLE_OCCUPANCY);
        sm.Tick();
        coordinator.Update();
        results.Check(coordinator.GetGameClock().IsPaused(),
                      "cached occupancy cannot resume restored clock");
        results.Check(!coordinator.AuthorizeRestoredClockResumeAfterBoot(
                          false, HealthyRuntime()),
                      "healthy runtime without a fresh full scan is rejected");
        results.Check(!coordinator.AuthorizeRestoredClockResumeAfterBoot(
                          true, PausedRuntime()),
                      "fresh scan with unhealthy runtime is rejected");
        results.Check(coordinator.AuthorizeRestoredClockResumeAfterBoot(
                          true, DegradedRuntime()),
                      "fresh full scan plus usable power limiting authorizes resume");
        coordinator.Update();
        results.Check(!coordinator.GetGameClock().IsPaused(),
                      "authorized playable clock resumes");
    }

    // Planned flush captures exact live time; reboot restores it paused with
    // the same session/epoch identity until fresh boot evidence arrives.
    {
        constexpr const char* path = "build/t_clock_flush.json";
        RemoveSettingsFiles(path);
        const modules::SettingsStoreConfig cfg = SettingsConfig(path);
        uint32_t persistedRemaining = 0U;
        uint32_t persistedSession = 0U;
        uint32_t persistedEpoch = 0U;
        {
            modules::SettingsStore settings(cfg);
            results.Check(settings.Load(), "flush settings load");
            settings.Doc()["TIMER"] = 1U;
            runtime::GameStateMachine sm(StateMachineConfig());
            sm.Reset();
            test_support::BoundedHintEngine engine;
            engine.Reset();
            modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
            results.Check(coordinator.LoadOrInitialize(), "flush coordinator init");
            results.Check(coordinator.LoadExternalFen(PLAYABLE_FEN),
                          "flush playable FEN loads");
            modules::GamePersistenceState beforeStart;
            results.Check(modules::ReadGamePersistenceState(
                              settings.Doc(), beforeStart) &&
                              !beforeStart.clockState.started,
                          "new clock session persists stopped before play");
            const uint32_t epochBeforeStart =
                beforeStart.clockState.durableEpoch;
            const uint32_t savesBeforeStart = settings.GetSaveCount();
            sm.DebugSetPhysicalOccupancyMask(PLAYABLE_OCCUPANCY);
            sm.Tick();
            coordinator.Update();
            results.Check(!coordinator.GetGameClock().IsRunning(),
                          "fresh clock waits for first legal pickup");
            StartOpeningClockAtE2(clock, sm, coordinator);
            modules::GamePersistenceState duringPickup;
            results.Check(
                coordinator.GetGameClock().IsRunning() &&
                    modules::ReadGamePersistenceState(
                        settings.Doc(), duringPickup) &&
                    duringPickup.clockState.started &&
                    duringPickup.clockState.clock.running &&
                    !duringPickup.interactionState.Empty(),
                "first pickup persists the running clock with interaction state");
            sm.HandleSensorChange(44U, true);
            coordinator.Update();
            clock.AdvanceMs(11U);
            sm.Tick();
            coordinator.Update();
            results.Check(coordinator.GetGameClock().IsRunning(),
                          "clock remains running after first move settles");
            modules::GamePersistenceState afterStart;
            results.Check(modules::ReadGamePersistenceState(
                              settings.Doc(), afterStart) &&
                              afterStart.clockState.started &&
                              afterStart.clockState.clock.running &&
                              afterStart.clockState.durableEpoch !=
                                  epochBeforeStart,
                          "first pickup advances the durable clock boundary");
            results.Check(settings.GetSaveCount() == savesBeforeStart,
                          "playable start queues worker save without SaveNow");

            clock.AdvanceMs(1234U);
            results.Check(coordinator.FlushToSettingsNow(),
                          "planned reboot flush succeeds");
            persistedRemaining = coordinator.GetGameClock().GetRemainingMs(
                runtime::ClockSide::Black);

            modules::GamePersistenceState persisted;
            results.Check(modules::ReadGamePersistenceState(
                              settings.Doc(), persisted) &&
                              persisted.clockStatePresent,
                          "planned flush writes clock record");
            persistedSession = persisted.clockState.sessionId;
            persistedEpoch = persisted.clockState.durableEpoch;
            results.Check(persisted.clockState.started &&
                              persisted.clockState.clock.running &&
                              persisted.clockState.clock.remainingMsBlack ==
                                  persistedRemaining,
                          "planned flush stores exact running value");
        }

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "flushed settings reload");
        runtime::GameStateMachine sm(StateMachineConfig());
        sm.Reset();
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
        results.Check(coordinator.LoadOrInitialize(),
                      "flushed clock restores after reboot");
        results.Check(coordinator.GetGameClock().IsRunning() &&
                          coordinator.GetGameClock().IsPaused() &&
                          coordinator.GetGameClock().GetRemainingMs(
                              runtime::ClockSide::Black) == persistedRemaining,
                      "reboot restores exact live clock paused");

        modules::GamePersistenceState restored;
        results.Check(modules::ReadGamePersistenceState(
                          settings.Doc(), restored) &&
                          restored.clockState.sessionId == persistedSession &&
                          restored.clockState.durableEpoch == persistedEpoch,
                      "reboot preserves durable clock identity");

        coordinator.ResetEngine();
        modules::GamePersistenceState reset;
        results.Check(modules::ReadGamePersistenceState(
                          settings.Doc(), reset) &&
                          reset.clockState.sessionId != persistedSession &&
                          reset.clockState.durableEpoch != persistedEpoch,
                      "new game advances session and durable epoch");
    }

    // Timeout is captured immediately and remains terminal across reboot.
    // This case makes the persistence capture itself consume the final
    // millisecond, after FlushToSettingsNow's ordinary clock update.
    {
        constexpr const char* path = "build/t_clock_capture_timeout.json";
        RemoveSettingsFiles(path);
        fxe::ManualMillis::SetNowMs(0U);
        const modules::SettingsStoreConfig cfg = SettingsConfig(path);
        {
            modules::SettingsStore settings(cfg);
            results.Check(settings.Load(), "capture-timeout settings load");
            settings.Doc()["TIMER"] = 1U;
            runtime::GameStateMachine sm(StateMachineConfig());
            sm.Reset();
            test_support::BoundedHintEngine engine;
            engine.Reset();
            modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
            results.Check(coordinator.LoadOrInitialize(),
                          "capture-timeout coordinator init");
            results.Check(coordinator.LoadExternalFen(PLAYABLE_FEN),
                          "capture-timeout playable FEN loads");
            sm.DebugSetPhysicalOccupancyMask(PLAYABLE_OCCUPANCY);
            sm.Tick();
            coordinator.Update();
            StartOpeningClockAtE2(clock, sm, coordinator);
            sm.HandleSensorChange(44U, true);
            coordinator.Update();
            clock.AdvanceMs(11U);
            sm.Tick();
            coordinator.Update();
            results.Check(coordinator.GetGameClock().IsRunning(),
                          "capture-timeout clock starts on first pickup");

            g_persistenceBoundaryMillisCalls = 0U;
            {
                fxe::ScopedMillisProviderOverride boundaryMillis(
                    &PersistenceBoundaryMillisProvider);
                results.Check(coordinator.FlushToSettingsNow(),
                              "capture-timeout planned flush succeeds");
            }
            results.Check(coordinator.HasClockTerminalOutcome() &&
                              coordinator.GetClockTerminalWinner() ==
                                  modules::GameWinner::White &&
                              coordinator.GetGameClock().HasTimedOut(),
                          "capture-timeout is terminal in the live coordinator");

            modules::GamePersistenceState persisted;
            results.Check(modules::ReadGamePersistenceState(
                              settings.Doc(), persisted) &&
                              persisted.clockState.clock.timedOut &&
                              persisted.clockState.clock.timedOutSide ==
                                  runtime::ClockSide::Black,
                          "capture-timeout serializes the same terminal state");
        }

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "capture-timeout settings reload");
        runtime::GameStateMachine sm(StateMachineConfig());
        sm.Reset();
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
        results.Check(coordinator.LoadOrInitialize() &&
                          coordinator.HasClockTerminalOutcome() &&
                          coordinator.GetClockTerminalWinner() ==
                              modules::GameWinner::White,
                      "capture-timeout live and reboot terminal state agree");
    }

    // Timeout is captured immediately and remains terminal across reboot.
    {
        constexpr const char* path = "build/t_clock_timeout.json";
        RemoveSettingsFiles(path);
        const modules::SettingsStoreConfig cfg = SettingsConfig(path);
        {
            modules::SettingsStore settings(cfg);
            results.Check(settings.Load(), "timeout settings load");
            settings.Doc()["TIMER"] = 1U;
            runtime::GameStateMachine sm(StateMachineConfig());
            sm.Reset();
            test_support::BoundedHintEngine engine;
            engine.Reset();
            modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
            results.Check(coordinator.LoadOrInitialize(), "timeout coordinator init");
            results.Check(coordinator.LoadExternalFen(PLAYABLE_FEN),
                          "timeout playable FEN loads");
            sm.DebugSetPhysicalOccupancyMask(PLAYABLE_OCCUPANCY);
            sm.Tick();
            coordinator.Update();
            StartOpeningClockAtE2(clock, sm, coordinator);
            sm.HandleSensorChange(44U, true);
            coordinator.Update();
            clock.AdvanceMs(11U);
            sm.Tick();
            coordinator.Update();
            const uint32_t savesBeforeTimeout = settings.GetSaveCount();
            clock.AdvanceMs(60000U);
            coordinator.Update();

            modules::GamePersistenceState timedOut;
            results.Check(modules::ReadGamePersistenceState(
                              settings.Doc(), timedOut) &&
                              timedOut.clockStatePresent &&
                              timedOut.clockState.clock.timedOut &&
                              timedOut.clockState.clock.timedOutSide ==
                                  runtime::ClockSide::Black &&
                              timedOut.clockState.clock.remainingMsBlack == 0U,
                          "timeout latch updates durable record immediately");
            results.Check(settings.GetSaveCount() == savesBeforeTimeout,
                          "timeout queues worker save without SaveNow");
            results.Check(coordinator.FlushToSettingsNow(),
                          "terminal clock flushes for reboot");
        }

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "timeout settings reload");
        runtime::GameStateMachine sm(StateMachineConfig());
        sm.Reset();
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
        results.Check(coordinator.LoadOrInitialize(), "timed-out game restores");
        results.Check(coordinator.HasClockTerminalOutcome() &&
                          coordinator.GetClockTerminalWinner() ==
                              modules::GameWinner::White &&
                          coordinator.GetGameClock().HasTimedOut() &&
                          !coordinator.GetGameClock().IsRunning() &&
                          !coordinator.IsRestoredClockResumeAuthorizationPending(),
                      "reboot preserves timeout terminal without resume gate");
        results.Check(!coordinator.AuthorizeRestoredClockResumeAfterBoot(
                          true, HealthyRuntime()),
                      "timeout cannot be authorized to resume");
        results.Check(!coordinator.RequestUndo(1U),
                      "undo cannot resurrect restored timeout");
    }

    // Present malformed data rejects the whole restore rather than refilling.
    {
        constexpr const char* path = "build/t_clock_corrupt.json";
        RemoveSettingsFiles(path);
        const modules::SettingsStoreConfig cfg = SettingsConfig(path);
        {
            modules::SettingsStore settings(cfg);
            results.Check(settings.Load(), "corrupt settings load");
            settings.Doc()["TIMER"] = 1U;
            settings.Doc()[modules::KEY_GAME_FEN] = PLAYABLE_FEN;
            JsonArray record =
                settings.Doc()[modules::KEY_GAME_CLOCK_V1].to<JsonArray>();
            for (uint8_t i = 0U; i < 11U; ++i) {
                (void)record.add(0U);
            }
            record[0] = 99U;
            results.Check(settings.SaveNow(), "corrupt fixture save");
        }

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "corrupt settings reload");
        runtime::GameStateMachine sm(StateMachineConfig());
        sm.Reset();
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
        results.Check(!coordinator.LoadOrInitialize(),
                      "present malformed clock record fails closed");
        results.Check(!coordinator.GetGameClock().IsEnabled(),
                      "failed restore does not refill from TIMER preset");
    }

    results.Check(RunSecondClockEvaluationTimeoutRegression() == 0,
                  "second clock evaluation cannot restart Hint after timeout");
    return results.Finish();
}
