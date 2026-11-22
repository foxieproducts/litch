#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <thread>
#include <ArduinoJson.h>
#include "console.hpp"
#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/classic_search_config.hpp"
#include "modules/game_console_commands.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "runtime/interaction_dispatcher.hpp"
void CheckBenchmarkCleanup(fxe::test_support::TestResults& results);
void CheckBenchmarkJourney(fxe::test_support::TestResults& results);
void CheckBenchmarkRawControls(fxe::test_support::TestResults& results);
namespace {
std::string Document(const modules::SettingsStore& settings) {
    std::string text;
    serializeJson(settings.Doc(), text);
    return text;
}
bool WaitQuiescent(modules::GamePersistenceCoordinator& coordinator) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (coordinator.IsEngineLaneActive() && std::chrono::steady_clock::now() < end) {
        coordinator.Update();
        std::this_thread::yield();
    }
    return !coordinator.IsEngineLaneActive();
}
} // namespace
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fxe::test_support::TestResults results("ENGINE_BENCHMARK");
    results.Start();
    std::filesystem::create_directories("build");
    for (uint8_t id = 1U; id <= modules::ENGINE_BENCHMARK_CASE_COUNT; ++id) {
        const auto& item = *modules::GetEngineBenchmarkCase(id);
        modules::LitchMaxEngine importer;
        results.Check(importer.SetFen(item.fen), "host provenance imports through production adapter");
        mcumax_position actual{};
        results.Check(mcumax_position_import(&actual, &mcumax) &&
            std::memcmp(&actual, &item.root, sizeof(actual)) == 0,
            "fixed root preserves all board/moved/castling/EP/side/king bits");
    }
    results.Check(modules::GetEngineBenchmarkCase(0U) == nullptr &&
        modules::GetEngineBenchmarkCase(7U) == nullptr, "fixed bounded case lookup");
    for (uint8_t level = 1U; level <= 11U; ++level) {
        const auto config = modules::MakeThinkConfigForVsCpuLevel(level, 5U);
        const auto effective = modules::ResolveClassicSearchConfig(config);
        results.Check(effective.maxNodes == config.maxNodes && effective.maxDepth == config.maxDepth &&
            effective.minimumDepth == 1U && effective.firstRootNodeReserve == config.firstRootNodeReserve,
            "all eleven CPU profiles retain full production limits without opening ramp");
    }
    {
        fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(UINT32_MAX - 1U);
        modules::LitchMaxEngine timed;
        auto config = modules::MakeThinkConfigForVsCpuLevel(1U, 5U);
        config.maxThinkMs = 2U;
        results.Check(timed.StartBenchmark(1U, config), "deadline benchmark admits before unsigned wrap");
        fxe::ManualMillis::SetNowMs(2U);
        uint8_t progress = 0U;
        modules::UciMove best;
        (void)timed.PollThinkProgress(progress, best);
        modules::ThinkStats stats;
        results.Check(timed.GetLastThinkStats(stats) && stats.stopReason == modules::ThinkStopReason::Deadline,
            "actual stop callback detects deadline across unsigned millis wrap");
        config.maxThinkMs = 0U;
        config.maxNodes = 1U;
        config.firstRootNodeReserve = 0U;
        results.Check(timed.StartBenchmark(1U, config), "independent bounded node-cap probe starts");
        (void)timed.PollThinkProgress(progress, best);
        results.Check(timed.GetLastThinkStats(stats) && stats.stopReason == modules::ThinkStopReason::NodeCap,
            "actual workspace interruption names node cap separately from deadline/error");
    }
    constexpr const char* PATH = "build/test_engine_benchmark.json";
    (void)std::remove(PATH);
    (void)std::remove("build/test_engine_benchmark.json.bak");
    (void)std::remove("build/test_engine_benchmark.json.tmp");
    { std::ifstream input("test/data/terminal_checkmate_107.json");
      std::ofstream output(PATH); output << input.rdbuf(); }
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    settingsConfig.deferredSaveDelayMs = 100000U;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "actual 107-ply terminal fixture loads");
    modules::LitchMaxEngine engine;
    runtime::GameStateMachineConfig smConfig;
    smConfig.settleDurationMs = 0U;
    smConfig.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(smConfig);
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine, nullptr, &worker);
    coordinator.SetBenchmarkControlsIdleProbe(nullptr, [](void*) { return true; });
    results.Check(coordinator.LoadOrInitialize(), "production coordinator reconstructs actual history");
    results.Check(coordinator.GetEngineDerivedState().inCheckmate, "admission terminal is engine-derived Classic mate");
    results.Check(!coordinator.StartBenchmark(1U, 1U), "mismatched starting occupancy rejects diagnostic");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    const uint64_t expected = stateMachine.GetExpectedOccupancyMask();
    (void)dispatcher.DispatchBoardObservation({fxe::Millis(), 1U,
        stateMachine.GetPhysicalOccupancyMask() ^ expected, expected});
    coordinator.Update();
    MemoryConsoleTransport transport;
    Console console(transport);
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);
    modules::GameConsoleCommandContext context;
    modules::RegisterGameConsoleCommands(console, settings, coordinator, stateMachine, context);
    for (const char* command : {"game bench start 0 1", "game bench start 7 1",
        "game bench start 1 0", "game bench start 1 12", "game bench start -1 1",
        "game bench start 1 1x", "game bench start 99999999999999999999 1",
        "game bench start 1 1 extra", "game bench status extra", "game bench stop extra"}) {
        output.Clear();
        fxe::test_support::RunCommand(console, transport, command);
        results.Check(output.Str().find("usage:") != std::string::npos && !coordinator.IsEngineLaneActive(),
            "strict console bounds reject malformed command without work");
    }
    coordinator.SetBenchmarkControlsIdleProbe(nullptr, nullptr);
    results.Check(!coordinator.StartBenchmark(1U, 1U), "unbound physical admission fails closed");
    coordinator.SetBenchmarkControlsIdleProbe(nullptr, [](void*) { return true; });
    const auto before = Document(settings);
    const auto global = mcumax;
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "game bench start 1 1");
    results.Check(output.Str().find("started compute-only") != std::string::npos,
        "named terminal107 -> console start -> production worker job admitted");
    results.Check(!coordinator.StartBenchmark(2U, 1U), "duplicate request rejects while job owned");
    results.Check(WaitQuiescent(coordinator), "finite CPU job reaches complete cleanup");
    modules::EngineBenchmarkStatus status;
    results.Check(coordinator.TryGetBenchmarkStatus(status) && status.diagnostics.quiescentValid &&
        status.diagnostics.finalValid && status.diagnostics.effectiveValid &&
        status.diagnostics.kind == modules::ThinkRequestKind::Benchmark &&
        !status.diagnostics.firstAcceptedHintValid && status.diagnostics.finalNodesValid,
        "coherent benchmark result contains actual final/cleanup and no accepted Hint event");
    results.Check(status.diagnostics.stopReason == modules::ThinkStopReason::Exhausted ||
        status.diagnostics.stopReason == modules::ThinkStopReason::NodeCap,
        "actual finite search reports exhausted or node cap");
    results.Check(Document(settings) == before && std::memcmp(&global, &mcumax, sizeof(global)) == 0,
        "compute preserves singleton and complete game/preferences/history/clock document byte-for-byte");
    modules::ClassicThinkResult productResult;
    results.Check(!engine.GetLastClassicThinkResult(productResult), "benchmark never installs a live product result");
    results.Check(status.finalMoveValid && !status.finalMove.Empty(), "completed compute-only root exposes bounded final UCI for quality comparison");
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "game bench status");
    results.Check(output.Str().find("requested level=1") != std::string::npos &&
        output.Str().find("first_publication=1/") != std::string::npos &&
        output.Str().find("state=done") != std::string::npos && output.Str().find("units=ClassicPawn100") != std::string::npos,
        "completed cached Console exports actual requested limits/publication/named state and known score units");

    const uint32_t firstId = status.diagnostics.requestId;
    results.Check(coordinator.StartBenchmark(6U, 11U), "new case starts with unique diagnostic identity");
    results.Check(coordinator.TryGetBenchmarkStatus(status) && status.diagnostics.requestId != firstId &&
        !status.diagnostics.firstAcceptedHintValid, "new job resets per-request observations");
    const uint64_t lifted = expected & ~(UINT64_C(1) << 40U);
    (void)dispatcher.DispatchBoardObservation({fxe::Millis(), 2U, expected ^ lifted, lifted});
    coordinator.Update();
    results.Check(WaitQuiescent(coordinator) && coordinator.TryGetBenchmarkStatus(status) &&
        status.cancelRequested && status.diagnostics.state == modules::ThinkState::Canceled,
        "real queen lift cancels benchmark and waits for full cleanup before gameplay recovery");
    results.Check(Document(settings) == before && std::memcmp(&global, &mcumax, sizeof(global)) == 0,
        "input cancellation discards diagnostic output without engine/document mutation");
    (void)dispatcher.DispatchBoardObservation({fxe::Millis(), 3U, expected ^ lifted, expected});
    coordinator.Update();
    coordinator.SetBoardSettingsActive(true);
    results.Check(!coordinator.StartBenchmark(1U, 1U), "board settings/menu blocks terminal benchmark");
    coordinator.SetBoardSettingsActive(false);
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "game bench stop");
    results.Check(output.Str().find("no active benchmark") != std::string::npos, "idle stop reports no active benchmark");
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "game bench status");
    results.Check(output.Str().find("accepted_hint=omitted") != std::string::npos &&
        output.Str().find("stop=canceled") != std::string::npos,
        "status uses cached canceled record after engine lane is released");
    results.Check(output.Str().find("requested level=") != std::string::npos &&
        output.Str().find("first_publication=") != std::string::npos && output.Str().find("state=canceled") != std::string::npos &&
        output.Str().find("units=") != std::string::npos,
        "cached Console exports requested profile, first publication and named states/units");
    std::printf("RESOURCE native_root=%zu diagnostics=%zu benchmark_status=%zu engine=%zu coordinator=%zu worker=%zu workspace_required=%zu\n",
        sizeof(mcumax_position), sizeof(modules::ThinkDiagnostics), sizeof(modules::EngineBenchmarkStatus),
        sizeof(modules::LitchMaxEngine), sizeof(modules::GamePersistenceCoordinator), sizeof(modules::AsyncThinkWorker),
        litch_max::classic_chess::SearchWorkspace::RequiredBytes());
    CheckBenchmarkCleanup(results);
    CheckBenchmarkJourney(results);
    CheckBenchmarkRawControls(results);
    return results.Finish();
}
