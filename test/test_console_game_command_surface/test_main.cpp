#include <cstdio>
#include <cstring>
#include <string>

#include "console.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/game_console_commands.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
#include "bounded_hint_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

uint32_t ReadVsCpuLevel(const modules::SettingsStore& settings) {
    const auto& doc = settings.Doc();
    return doc["VS_CPU_LEVEL"] | 0U;
}

uint32_t ReadHintLevel(const modules::SettingsStore& settings) {
    const auto& doc = settings.Doc();
    uint32_t v = doc["HINT_LEVEL"] | 8U;

    if (v < 1U) {
        v = 1U;
    }
    if (v > 11U) {
        v = 11U;
    }
    return v;
}

modules::ControlMutationCallbacks MakeControlCallbacks(
    modules::GamePersistenceCoordinator& gamePersistence) {
    modules::ControlMutationCallbacks callbacks;
    callbacks.context = &gamePersistence;
    callbacks.checkStartOnlyGate = [](void* context, const char* key) {
        return static_cast<modules::GamePersistenceCoordinator*>(context)
            ->CheckControlStartOnlyGate(key);
    };
    callbacks.cancelCpuWork = [](void* context) {
        static_cast<modules::GamePersistenceCoordinator*>(context)
            ->RequestCancelCpuWork();
    };
    callbacks.cancelHintWorkAndRestart = [](void* context) {
        static_cast<modules::GamePersistenceCoordinator*>(context)
            ->RequestCancelHintWorkAndRestart();
    };
    callbacks.requestHint = [](void* context, const uint16_t requestId) {
        return static_cast<modules::GamePersistenceCoordinator*>(context)
            ->RequestControlHint(requestId);
    };
    callbacks.requestUndo = [](void* context,
                               const uint16_t requestId,
                               const uint8_t moveCount) {
        return static_cast<modules::GamePersistenceCoordinator*>(context)
            ->RequestControlUndo(requestId, moveCount);
    };
    callbacks.requestNewGame = [](void* context, const uint16_t requestId) {
        return static_cast<modules::GamePersistenceCoordinator*>(context)
            ->RequestControlNewGame(requestId);
    };
    callbacks.pollAction = [](void* context, const uint16_t requestId) {
        return static_cast<modules::GamePersistenceCoordinator*>(context)
            ->PollControlAction(requestId);
    };
    return callbacks;
}

} // namespace

int main() {
    fxe::test_support::TestResults results("CONSOLE_GAME_COMMAND_SURFACE");
    results.Start();

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_console_game_command_surface.json";
    cfg.deferredSaveDelayMs = 100000;

    (void)std::remove(cfg.path);
    (void)std::remove("build/test_console_game_command_surface.json.bak");
    (void)std::remove("build/test_console_game_command_surface.json.tmp");

    modules::SettingsStore settings(cfg);
    results.Check(settings.Load(), "settings load");

    test_support::BoundedHintEngine engine;
    runtime::GameStateMachine sm;
    modules::GamePersistenceCoordinator gamePersistence(settings, engine, sm);
    results.Check(gamePersistence.LoadOrInitialize(), "persistence load/init");
    modules::ControlMutationService controlMutation;
    controlMutation.Configure(settings, MakeControlCallbacks(gamePersistence));

    MemoryConsoleTransport transport;
    Console console(transport);
    console.SetSettingsStore(&settings);
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);

    modules::GameConsoleCommandContext gameCmdCtx;
    modules::RegisterGameConsoleCommands(
        console, settings, gamePersistence, sm, gameCmdCtx, &controlMutation);

    fxe::test_support::RunCommand(console, transport, "game status");
    results.Check(output.Str().find("search: kind=cpu active=unavailable status=unavailable") != std::string::npos &&
                      output.Str().find("search: kind=hint active=unavailable status=unavailable") != std::string::npos,
                  "DIAG-FRESHNESS unavailable requests do not assert inactivity");
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "game hint stats");
    results.Check(output.Str().find("search: kind=cpu active=0") == std::string::npos &&
                      output.Str().find("search: kind=hint active=0") == std::string::npos,
                  "DIAG-FRESHNESS unavailable detailed stats never invent active=0");
    output.Clear();

    MemoryConsoleTransport unconfiguredTransport;
    Console unconfiguredConsole(unconfiguredTransport);
    unconfiguredConsole.SetSettingsStore(&settings);
    fxe::test_support::ConsoleOutputCapture unconfiguredOutput;
    unconfiguredOutput.Attach(unconfiguredTransport);
    modules::ControlMutationService unconfiguredMutation;
    modules::GameConsoleCommandContext unconfiguredCtx;
    modules::RegisterGameConsoleCommands(
        unconfiguredConsole, settings, gamePersistence, sm, unconfiguredCtx,
        &unconfiguredMutation);
    fxe::test_support::RunCommand(
        unconfiguredConsole, unconfiguredTransport, "game mode cpu 5");
    results.Check(ReadVsCpuLevel(settings) == 0,
                  "game setting mutation fails closed without the service");
    results.Check(
        unconfiguredOutput.Str().find("FAIL reason=storage_unavailable") !=
            std::string::npos,
        "unconfigured game mutation service reports a stable rejection reason");
    unconfiguredOutput.Clear();
    fxe::test_support::RunCommand(
        unconfiguredConsole, unconfiguredTransport, "game new");
    results.Check(
        unconfiguredOutput.Str().find("reason=unsupported") !=
            std::string::npos,
        "game action fails closed before canonical callbacks are configured");

    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "game ");
    transport.PushChar('\t');
    console.Update();
    results.Check(output.Str().find("mode\r\n") != std::string::npos,
                  "game completion lists mode subcommand");
    results.Check(output.Str().find("hint\r\n") != std::string::npos,
                  "game completion lists hint subcommand");
    results.Check(output.Str().find("status\r\n") != std::string::npos,
                  "game completion lists status subcommand");
    fxe::test_support::PressEnter(console, transport);

    output.Clear();
    fxe::test_support::RunCommand(console, transport, "game status");
    results.Check(
        output.Str().find(
            "variant=Undecided duck=-- phase=ChessMove pending=0 outcome=None winner=None") !=
            std::string::npos,
        "game status reports variant and Duck state without mutation");
    results.Check(output.Str().find("search: kind=cpu active=unavailable status=unavailable") != std::string::npos &&
                      output.Str().find("search: kind=hint active=unavailable status=unavailable") != std::string::npos,
                  "existing status distinguishes both unavailable diagnostic owners");

    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "game mode ");
    transport.PushChar('\t');
    console.Update();
    results.Check(output.Str().find("human\r\n") != std::string::npos,
                  "game mode completion lists human");
    results.Check(output.Str().find("11\r\n") != std::string::npos,
                  "game mode completion lists numeric levels");
    fxe::test_support::PressEnter(console, transport);

    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "game hint ");
    transport.PushChar('\t');
    console.Update();
    results.Check(output.Str().find("level\r\n") == std::string::npos,
                  "game hint completion excludes retired level");
    fxe::test_support::PressEnter(console, transport);
    results.Check(output.Str().find("hint: no completed hint stats") != std::string::npos,
                  "game hint completion executes the sole stats subcommand");

    // Mode mapping: VS_CPU_LEVEL is authoritative.
    results.Check(ReadVsCpuLevel(settings) == 0, "default VS_CPU_LEVEL=0");

    // Hint mapping: HINT_LEVEL defaults to 8 and clamps to 1..11.
    results.Check(ReadHintLevel(settings) == 8, "default HINT_LEVEL=8");

    fxe::test_support::RunCommand(console, transport, "game mode cpu 5");
    results.Check(ReadVsCpuLevel(settings) == 5, "game mode cpu 5 sets VS_CPU_LEVEL=5");

    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "game mode h");
    transport.PushChar('\t');
    console.Update();
    fxe::test_support::PressEnter(console, transport);
    results.Check(ReadVsCpuLevel(settings) == 0, "tab-completed game mode human executes");

    fxe::test_support::RunCommand(console, transport, "game mode human");
    results.Check(ReadVsCpuLevel(settings) == 0, "game mode human sets VS_CPU_LEVEL=0");

    fxe::test_support::RunCommand(console, transport, "game mode 11");
    results.Check(ReadVsCpuLevel(settings) == 11, "game mode 11 sets VS_CPU_LEVEL=11");

    fxe::test_support::RunCommand(console, transport, "game mode 12");
    results.Check(ReadVsCpuLevel(settings) == 11,
                  "game mode 12 rejected; VS_CPU_LEVEL unchanged");

    fxe::test_support::RunCommand(console, transport, "game hint level 7");
    results.Check(ReadHintLevel(settings) == 8, "retired game hint level does not change legacy storage");

    fxe::test_support::RunCommand(console, transport, "game hint level 12");
    results.Check(ReadHintLevel(settings) == 8,
                  "game hint level 12 rejected; HINT_LEVEL unchanged");

    sm.HandleInteractionPositionAcknowledged(1ULL << 12U, 0ULL, 0ULL,
                                             1ULL << 12U);
    sm.ApplyPhysicalOccupancySnapshot(1ULL << 12U);
    sm.HandleSensorChange(12U, false);
    fxe::test_support::RunCommand(console, transport, "game undo");
    results.Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "game undo cancels picked-up move");

    // New game resets engine to initial position.
    results.Check(gamePersistence.LoadExternalFen("8/8/8/8/8/8/8/8 w - - 0 1"), "set external fen");
    fxe::test_support::RunCommand(console, transport, "game new");

    char fen[128] = {};
    results.Check(engine.GetFen(fen, sizeof(fen)), "engine get fen");
    results.Check(std::strcmp(fen, modules::INITIAL_FEN) == 0, "game new resets to INITIAL_FEN");

    results.Check(gamePersistence.LoadExternalFen("8/8/8/8/8/8/8/8 w - - 0 1"), "set external fen again");
    fxe::test_support::RunCommand(console, transport, "reset");
    results.Check(engine.GetFen(fen, sizeof(fen)), "engine get fen after reset alias");
    results.Check(std::strcmp(fen, modules::INITIAL_FEN) == 0, "reset alias resets to INITIAL_FEN");

    fxe::test_support::RunCommand(console, transport, "game placement");
    results.Check(sm.IsPlacementModeActive(),
                  "diagnostic placement command retains its direct reducer seam");

    modules::SettingsStoreConfig duckCfg;
    duckCfg.path = "build/test_console_game_command_surface_duck.json";
    duckCfg.deferredSaveDelayMs = 100000;
    (void)std::remove(duckCfg.path);
    (void)std::remove("build/test_console_game_command_surface_duck.json.bak");
    (void)std::remove("build/test_console_game_command_surface_duck.json.tmp");
    modules::SettingsStore duckSettings(duckCfg);
    results.Check(duckSettings.Load(), "Duck status settings load");
    modules::GamePersistenceState persistedDuck;
    results.Check(persistedDuck.fen.Assign(modules::INITIAL_FEN) &&
                      persistedDuck.undoBaseFen.Assign(modules::INITIAL_FEN),
                  "Duck status FEN seed");
    modules::DuckGameDurableState durableDuck;
    durableDuck.variant = modules::GameVariant::Duck;
    durableDuck.duckSquare = 18U;
    durableDuck.outcome.reason = modules::VariantOutcomeReason::Fowled;
    durableDuck.outcome.winner = modules::GameWinner::Black;
    char encodedDuck[32] = {};
    results.Check(modules::EncodeDuckGameDurableState(
                      durableDuck, encodedDuck, sizeof(encodedDuck)) &&
                      persistedDuck.variantState.Assign(encodedDuck) &&
                      modules::WriteGamePersistenceState(
                          duckSettings.Doc(), persistedDuck),
                  "Duck status persistence seed");

    test_support::BoundedHintEngine duckEngine;
    runtime::GameStateMachine duckSm;
    modules::GamePersistenceCoordinator duckPersistence(
        duckSettings, duckEngine, duckSm);
    results.Check(duckPersistence.LoadOrInitialize(),
                  "Duck status persistence load/init");
    MemoryConsoleTransport duckTransport;
    Console duckConsole(duckTransport);
    duckConsole.SetSettingsStore(&duckSettings);
    fxe::test_support::ConsoleOutputCapture duckOutput;
    duckOutput.Attach(duckTransport);
    modules::GameConsoleCommandContext duckCtx;
    modules::RegisterGameConsoleCommands(
        duckConsole, duckSettings, duckPersistence, duckSm, duckCtx);
    fxe::test_support::RunCommand(
        duckConsole, duckTransport, "game status");
    results.Check(
        duckOutput.Str().find(
            "variant=Duck duck=c6 phase=ChessMove pending=0 outcome=Fowled winner=Black") !=
            std::string::npos,
        "game status reports persisted Duck square and terminal outcome");

    return results.Finish();
}
