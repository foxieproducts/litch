#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>
#include <string>

#include "firmware/firmware_console_logger_adapter.hpp"
#include "foxie_embedded/console_transport_memory.hpp"
#include "logger.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "trace/trace_replay.hpp"
#include "trace/trace_recorder.hpp"

int RunDuckCombinedTurnTraceReplayTest();

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckCaptureAuthorityReplay(const runtime::GameStateMachineConfig& config) {
    constexpr uint8_t allowedSource = 51U, deniedSource = 55U, victim = 35U;
    constexpr uint64_t mover = (1ULL << allowedSource) | (1ULL << deniedSource);
    constexpr uint64_t opponent = 1ULL << victim;
    runtime::CaptureLegality captures;
    captures.knownSources = mover;
    captures.landingsBySource[allowedSource] = opponent;

    firmware_internal::FirmwareConsoleLoggerAdapter loggerAdapter;
    fxe::MemoryConsoleTransport transport;
    std::string serial;
    transport.SetOutputCallback([&serial](const char* bytes, size_t size) {
        serial.append(bytes, size);
    });
    fxe::Console console(transport);
    bool accepted = false;
    const auto previousLogLevel = logger::GetMinLevel();
    logger::SetMinLevel(logger::Level::TRACE);
    logger::SetOutputLineCallback([&](const char* line) {
        if (std::strncmp(line, "{\"type\":\"position_ack\"", 22U) == 0) {
            accepted = loggerAdapter.Enqueue(line);
        }
    });
    trace::TraceRecorder recorder;
    recorder.Start("test", "test", 0U, {});
    recorder.RecordPositionAcknowledged(1U, mover | opponent, 0ULL, 1ULL << allowedSource,
        mover, opponent, opponent | (1ULL << deniedSource), 255U, 255U,
        true, false, true, opponent, false, 0ULL, captures);
    recorder.Stop();
    logger::SetOutputLineCallback(nullptr);
    logger::SetMinLevel(previousLogLevel);
    Check(accepted && loggerAdapter.DrainTo(console) == 1U,
          "position acknowledgement drains as one logical console trace line");
    const size_t begin = serial.find("{\"type\":\"position_ack\"");
    const size_t end = serial.find("\r\n", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        Check(false, "console emits the complete capture authority acknowledgement");
        return;
    }
    const std::string ackLine = serial.substr(begin, end - begin);
    Check(ackLine.size() >= firmware_internal::FirmwareConsoleLoggerAdapter::LINE_CAPACITY &&
              ackLine.back() == '}',
          "capture authority trace exceeds the old line limit without truncation");
    const char* const capturedLines[] = {"{\"type\":\"header\",\"ver\":1}", ackLine.c_str()};
    runtime::ManualClock clock(0U);
    runtime::GameStateMachine machine(config);
    machine.Reset();
    trace::ReplayResult result;
    const bool replayed = trace::ReplayJsonLines(capturedLines, 2U, clock, machine, result);
    if (!replayed) {
        std::printf("capture trace rejected at line %u: %s\n",
                    result.failureLine, ackLine.c_str());
    }
    Check(replayed, result.message);
    const auto& acknowledgedCaptures = machine.GetChessInteractionModel().acknowledgedCaptureLegality;
    Check(acknowledgedCaptures.knownSources == mover &&
              acknowledgedCaptures.landingsBySource == captures.landingsBySource,
          "firmware console trace carries both an allowed landing and a known empty row");

    const std::string legacyLine = ackLine.substr(0U, ackLine.find(",\"capture_rows\"")) + "}";
    const char* const legacyLines[] = {"{\"type\":\"header\",\"ver\":1}", legacyLine.c_str()};
    runtime::GameStateMachine legacy(config);
    legacy.Reset();
    Check(trace::ReplayJsonLines(legacyLines, 2U, clock, legacy, result), result.message);
    const uint64_t legacyHash = runtime::ComputeChessInteractionModelHash(legacy.GetChessInteractionModel());
    auto withoutRows = machine.GetChessInteractionModel();
    withoutRows.acknowledgedCaptureLegality.Clear();
    Check(legacy.GetChessInteractionModel().acknowledgedCaptureLegality.knownSources == 0ULL &&
              legacyHash == runtime::ComputeChessInteractionModelHash(withoutRows) &&
              legacyHash != runtime::ComputeChessInteractionModelHash(machine.GetChessInteractionModel()),
          "legacy absence preserves its digest and known capture rights affect the model digest");
}

} // namespace

int main() {
    std::puts("[TRACE_REPLAY] start");

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_trace_replay.json";
    settingsCfg.deferredSaveDelayMs = 100000;
    (void)std::remove(settingsCfg.path);
    (void)std::remove("build/test_trace_replay.json.bak");
    (void)std::remove("build/test_trace_replay.json.tmp");

    modules::SettingsStore settings(settingsCfg);
    Check(settings.Load(), "settings load");

    // A minimal trace that exercises PickedUp -> Settling -> Idle.
    const char* const lines[] = {
        "{\"type\":\"header\",\"ver\":1}",
        // Deployed-v1 traces predate the takeback setting. Missing means off
        // during replay, not whatever default this firmware happens to use.
        "{\"type\":\"settings\",\"t_ms\":0,\"json\":{\"BOARD_ROTATE\":1,\"GAME_FIRST_PLAYER\":2,\"TIMER\":2}}",
        // Canonical starting scan: the old fixture accidentally relied on an
        // uninitialized physical occupancy snapshot.
        "{\"type\":\"board_observation\",\"t_ms\":0,\"changed_mask\":\"0x0000000000000000\",\"occupied_mask\":\"0xffff00000000ffff\"}",

        // Remove e2 pawn: rotation and opposite P1 placement cancel in raw I/O.
        "{\"type\":\"sensor\",\"t_ms\":10,\"square\":52,\"occ\":0}",
        "{\"type\":\"checkpoint_state\",\"t_ms\":10,\"game_state\":\"PickedUp\",\"placement\":false,\"cpu\":false,\"human_illegal\":false,\"cpu_illegal\":false}",

        // Place piece: PickedUp -> Settling
        "{\"type\":\"sensor\",\"t_ms\":20,\"square\":36,\"occ\":1}",
        "{\"type\":\"checkpoint_state\",\"t_ms\":20,\"game_state\":\"Settling\",\"placement\":false,\"cpu\":false,\"human_illegal\":false,\"cpu_illegal\":false}",

        // Not yet settled.
        "{\"type\":\"tick\",\"t_ms\":119}",
        "{\"type\":\"checkpoint_state\",\"t_ms\":119,\"game_state\":\"Settling\",\"placement\":false,\"cpu\":false,\"human_illegal\":false,\"cpu_illegal\":false}",

        // Settled.
        "{\"type\":\"tick\",\"t_ms\":120}",
        "{\"type\":\"checkpoint_state\",\"t_ms\":120,\"game_state\":\"Idle\",\"placement\":false,\"cpu\":false,\"human_illegal\":false,\"cpu_illegal\":false}",
        "{\"type\":\"control\",\"t_ms\":121,\"seq\":8,\"control\":\"hint_toggle\",\"count\":0}",
        "{\"type\":\"settings\",\"t_ms\":122,\"json\":{\"TIMER\":5}}",
        "{\"type\":\"tick\",\"t_ms\":122}",
    };

    runtime::ManualClock clock(0);
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 100;
    cfg.cpuIdleDelayMs = 999999;
    CheckCaptureAuthorityReplay(cfg);

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator gamePersistence(settings, engine, sm);
    Check(gamePersistence.LoadOrInitialize(), "persistence load/init");

    trace::ReplayResult result;
    const bool ok = trace::ReplayJsonLines(lines, sizeof(lines) / sizeof(lines[0]),
                                           clock, sm, result, &gamePersistence, &engine);

    Check(ok, result.ok ? "replay failed" : result.message);
    Check(sm.GetLastSensorSquare() == 36,
          "composed orientation maps physical square 36 to logical e4");
    Check(sm.IsHintDisplayRequested(), "semantic hint control replays");
    Check(settings.ReadValue(
              "TAKEBACK_GRACE_SECONDS", UINT32_MAX) == 0U,
          "legacy trace replay disables absent takeback grace deterministically");
    Check(settings.ReadValue("TIMER", UINT32_MAX) == 5U,
          "trace replay applies changed TIMER setting");
    const runtime::GameClock& replayClock = gamePersistence.GetGameClock();
    Check(replayClock.IsEnabled() &&
              replayClock.GetRemainingMs(runtime::ClockSide::White) == 900000U &&
              replayClock.GetRemainingMs(runtime::ClockSide::Black) == 900000U,
          "coordinator observes replayed TIMER preset");

    const modules::GameVariantStateSnapshot replayVariant =
        gamePersistence.GetGameVariantStateSnapshot();
    const uint64_t replayModelHash = runtime::ComputeChessInteractionModelHash(
        sm.GetChessInteractionModel());
    const uint64_t replayAppHash =
        modules::ComputeAppStateHash(replayModelHash, replayVariant);
    char variantCheckpoint[384] = {};
    std::snprintf(
        variantCheckpoint, sizeof(variantCheckpoint),
        "{\"type\":\"checkpoint_variant\",\"v\":%u,\"ds\":%u,\"dp\":%u,\"pd\":%u,\"or\":%u,\"win\":%u,\"vh\":\"0x%016llx\",\"ah\":\"0x%016llx\"}",
        static_cast<unsigned>(replayVariant.variant),
        static_cast<unsigned>(replayVariant.duckSquare),
        static_cast<unsigned>(replayVariant.duckTurnPhase),
        replayVariant.pendingChessMove ? 1U : 0U,
        static_cast<unsigned>(replayVariant.outcome.reason),
        static_cast<unsigned>(replayVariant.outcome.winner),
        static_cast<unsigned long long>(
            modules::ComputeGameVariantStateHash(replayVariant)),
        static_cast<unsigned long long>(replayAppHash));
    const char* const variantLines[] = {variantCheckpoint};
    trace::ReplayResult variantResult;
    Check(trace::ReplayJsonLines(
              variantLines, 1U, clock, sm, variantResult,
              &gamePersistence, &engine),
          variantResult.ok
              ? "variant checkpoint replay failed"
              : variantResult.message);

    char mismatchedVariantCheckpoint[128] = {};
    std::snprintf(
        mismatchedVariantCheckpoint, sizeof(mismatchedVariantCheckpoint),
        "{\"type\":\"checkpoint_variant\",\"v\":%u}",
        static_cast<unsigned>(replayVariant.variant) + 1U);
    const char* const mismatchedVariantLines[] = {
        mismatchedVariantCheckpoint};
    trace::ReplayResult mismatchedVariantResult;
    Check(!trace::ReplayJsonLines(
              mismatchedVariantLines, 1U, clock, sm,
              mismatchedVariantResult, &gamePersistence, &engine) &&
              std::strcmp(mismatchedVariantResult.message,
                          "checkpoint_variant_state_mismatch") == 0,
          "variant checkpoint mismatch fails replay");

    runtime::ManualClock atomicClock(200);
    runtime::GameStateMachine atomicSm(cfg);
    atomicSm.Reset();
    const uint64_t queensidePair = (1ULL << 56) | (1ULL << 60);
    atomicSm.DebugSetPhysicalOccupancyMask(queensidePair);

    const char* const atomicLines[] = {
        "{\"type\":\"header\",\"ver\":1}",
        // Deployed-v1 position acknowledgement: typed ownership is present,
        // but the later engine-derived castling-legality fields are absent.
        "{\"type\":\"position_ack\",\"t_ms\":200,\"seq\":6,\"occupied_mask\":\"0x1100000000000000\",\"king_mask\":\"0x1000000000000000\",\"rook_mask\":\"0x0100000000000000\",\"mover_mask\":\"0x1100000000000000\"}",
        "{\"type\":\"board_observation\",\"t_ms\":210,\"seq\":7,\"changed_mask\":\"0x1100000000000000\",\"occupied_mask\":\"0x0000000000000000\"}",
        // Hardcoded deployed-v1 hash includes the retired pickup-order byte;
        // its old shadow divergence category is historical/uncheckable now.
        "{\"type\":\"checkpoint_interaction\",\"t_ms\":210,\"ph\":\"ExecutingPlan\",\"pl\":\"Castling\",\"req\":\"PlacePrimary\",\"exp\":\"0x1100000000000000\",\"l\":3,\"p\":0,\"r\":0,\"tok\":0,\"h\":\"0xe007bec328b47b57\",\"d\":\"PhysicalOccupancy\"}",
        "{\"type\":\"checkpoint_state\",\"t_ms\":210,\"game_state\":\"PickedUp\",\"human_illegal\":false}",
    };

    trace::ReplayResult atomicResult;
    const bool atomicOk = trace::ReplayJsonLines(
        atomicLines,
        sizeof(atomicLines) / sizeof(atomicLines[0]),
        atomicClock,
        atomicSm,
        atomicResult);
    Check(atomicOk,
          atomicResult.ok ? "atomic observation replay failed" : atomicResult.message);
    Check(atomicSm.GetChessInteractionModel().phase.plan.moveFrom == 60,
          "atomic replay uses same king-first-equivalent transition");
    Check(!atomicSm.HasHumanIllegal(),
          "atomic replay does not invent rook-first chronology");
    Check(atomicSm.GetLastBoardObservationTimestampMs() == 210,
          "atomic replay preserves observation timestamp");
    Check(atomicSm.GetLastBoardObservationSequence() == 7,
          "atomic replay preserves recorded observation sequence");
    Check(atomicSm.GetLastBoardObservationChangedMask() == queensidePair,
          "atomic replay preserves observation changed mask");
    Check(atomicSm.GetLastBoardObservationOccupancyMask() == 0,
          "atomic replay preserves complete observation occupancy");

    runtime::GameStateMachine rebasedSm(cfg);
    rebasedSm.Reset();
    rebasedSm.SetExpectedOccupancyMask(1ULL << 12U);
    rebasedSm.SetExpectedMoverAndOpponentOccupancyMasks(
        1ULL << 12U, 0ULL);
    rebasedSm.DebugSetPhysicalOccupancyMask(1ULL << 12U);
    runtime::ManualClock rebasedClock(220U);
    const char* const rebasedLines[] = {
        "{\"type\":\"header\",\"ver\":1}",
        "{\"type\":\"board_observation\",\"t_ms\":220,\"seq\":7,\"changed_mask\":\"0x0000000000000000\",\"occupied_mask\":\"0x0000000000001000\"}",
        "{\"type\":\"sensor\",\"t_ms\":221,\"square\":12,\"occ\":0}",
    };
    trace::ReplayResult rebasedResult;
    Check(trace::ReplayJsonLines(
              rebasedLines,
              sizeof(rebasedLines) / sizeof(rebasedLines[0]),
              rebasedClock, rebasedSm, rebasedResult),
          rebasedResult.ok
              ? "explicit observation sequence rebase failed"
              : rebasedResult.message);
    Check(rebasedSm.GetLastBoardObservationSequence() == 9U &&
              rebasedSm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::PieceLifted,
          "sensor after explicit seq N is accepted after the queued tick");

    runtime::ChessInteractionModel productionModel;
    runtime::ResetChessInteractionModel(productionModel);
    runtime::ReduceChessInteraction(
        productionModel,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            queensidePair,
            (1ULL << 60),
            (1ULL << 56),
            queensidePair,
            0));
    runtime::ReduceChessInteraction(
        productionModel,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            210,
            7,
            queensidePair,
            0));
    Check(runtime::ComputeChessInteractionModelHash(
              atomicSm.GetChessInteractionModel()) ==
              runtime::ComputeChessInteractionModelHash(productionModel),
          "recorded and replayed atomic event streams produce identical model hashes");

    runtime::ManualClock adapterClock(300);
    runtime::GameStateMachine adapterSm(cfg);
    adapterSm.Reset();
    // Legacy golden: v1 position acknowledgements had no engine-derived
    // castling metadata. Their typed king-first choreography must retain the
    // historical castling inference and checkpoint hashes.
    const char* const adapterLines[] = {
        "{\"type\":\"header\",\"ver\":1}",
        "{\"type\":\"position_ack\",\"t_ms\":300,\"seq\":1,\"occupied_mask\":\"0x9000000000000000\",\"king_mask\":\"0x1000000000000000\",\"rook_mask\":\"0x8000000000000000\"}",
        "{\"type\":\"board_observation\",\"t_ms\":310,\"seq\":2,\"changed_mask\":\"0x1000000000000000\",\"occupied_mask\":\"0x8000000000000000\"}",
        "{\"type\":\"board_observation\",\"t_ms\":320,\"seq\":3,\"changed_mask\":\"0x4000000000000000\",\"occupied_mask\":\"0xc000000000000000\"}",
        "{\"type\":\"checkpoint_interaction\",\"t_ms\":320,\"phase\":\"ExecutingPlan\",\"plan\":\"Castling\",\"required\":\"LiftSecondary\",\"expected\":\"0x9000000000000000\",\"lifted\":1,\"placed\":1,\"acked\":0,\"token\":0}",
        "{\"type\":\"board_observation\",\"t_ms\":330,\"seq\":4,\"changed_mask\":\"0x8000000000000000\",\"occupied_mask\":\"0x4000000000000000\"}",
        "{\"type\":\"board_observation\",\"t_ms\":340,\"seq\":5,\"changed_mask\":\"0x2000000000000000\",\"occupied_mask\":\"0x6000000000000000\"}",
        "{\"type\":\"checkpoint_interaction\",\"t_ms\":340,\"phase\":\"AwaitingAction\",\"plan\":\"Castling\",\"required\":\"AwaitAcknowledgement\",\"expected\":\"0x9000000000000000\",\"lifted\":3,\"placed\":3,\"acked\":0,\"token\":1}",
        "{\"type\":\"action_result\",\"t_ms\":350,\"seq\":6,\"token\":1,\"ok\":1}",
        "{\"type\":\"checkpoint_interaction\",\"t_ms\":350,\"phase\":\"Idle\",\"plan\":\"None\",\"required\":\"None\",\"expected\":\"0x6000000000000000\",\"lifted\":0,\"placed\":0,\"acked\":0,\"token\":0}",
        "{\"type\":\"adapter_result\",\"t_ms\":360,\"seq\":7,\"adapter\":\"persist\",\"ok\":1}",
    };
    trace::ReplayResult adapterResult;
    Check(trace::ReplayJsonLines(
              adapterLines,
              sizeof(adapterLines) / sizeof(adapterLines[0]),
              adapterClock,
              adapterSm,
              adapterResult),
          adapterResult.ok ? "adapter result replay failed" : adapterResult.message);

    const char* const persistedLines[] = {
        "{\"type\":\"header\",\"ver\":1}",
        "{\"type\":\"settings\",\"t_ms\":0,\"json\":{\"BOARD_ROTATE\":0}}",
        "{\"type\":\"cmd\",\"t_ms\":5,\"cmd\":\"engine fen 8/8/8/8/8/8/8/8 w - - 0 1\"}",
        "{\"type\":\"cmd\",\"t_ms\":10,\"cmd\":\"persist reload\"}",
        "{\"type\":\"checkpoint_state\",\"t_ms\":10,\"game_state\":\"Idle\"}",
        "{\"type\":\"checkpoint_engine\",\"t_ms\":10,\"fen\":\"8/8/8/8/8/8/8/8 w - - 0 1\"}",
    };

    trace::ReplayResult persistedResult;
    const bool persistedOk = trace::ReplayJsonLines(persistedLines,
                                                    sizeof(persistedLines) / sizeof(persistedLines[0]),
                                                    clock,
                                                    sm,
                                                    persistedResult,
                                                    &gamePersistence,
                                                    &engine);
    Check(persistedOk, persistedResult.ok ? "persist reload replay failed" : persistedResult.message);

    const char* const externalizedLines[] = {
        "{\"type\":\"header\",\"ver\":1}",
        "{\"type\":\"cmd\",\"t_ms\":20,\"cmd\":\"engine fen 4k3/8/8/8/8/8/8/4K2R w K - 0 1\"}",
        "{\"type\":\"position_ack\",\"t_ms\":20,\"seq\":1,\"occupied_mask\":\"0x9000000000000010\",\"king_mask\":\"0x1000000000000010\",\"rook_mask\":\"0x8000000000000000\"}",
        "{\"type\":\"adapter_result\",\"t_ms\":20,\"seq\":2,\"adapter\":\"persist\",\"ok\":1}",
    };
    trace::ReplayResult externalizedResult;
    Check(trace::ReplayJsonLines(
              externalizedLines,
              sizeof(externalizedLines) / sizeof(externalizedLines[0]),
              clock,
              sm,
              externalizedResult,
              &gamePersistence,
              &engine),
          externalizedResult.ok
              ? "externalized result replay failed"
              : externalizedResult.message);
    Check(sm.GetChessInteractionModel().acknowledged.revision == 1U,
          "recorded position acknowledgement is reduced exactly once");
    Check(!gamePersistence.AreInteractionEventsExternalized(),
          "replay restores coordinator interaction event mode");

    modules::SettingsStoreConfig vsCpuSettingsCfg;
    vsCpuSettingsCfg.path = "build/test_trace_replay_vs_cpu_reload.json";
    vsCpuSettingsCfg.deferredSaveDelayMs = 100000;
    (void)std::remove(vsCpuSettingsCfg.path);
    (void)std::remove("build/test_trace_replay_vs_cpu_reload.json.bak");
    (void)std::remove("build/test_trace_replay_vs_cpu_reload.json.tmp");

    modules::SettingsStore vsCpuSettings(vsCpuSettingsCfg);
    Check(vsCpuSettings.Load(), "vs-cpu settings load");
    vsCpuSettings.Doc()["VS_CPU_LEVEL"] = 1U;
    vsCpuSettings.NotifyDocChanged();

    runtime::ManualClock vsCpuClock(1000);
    runtime::GameStateMachineConfig vsCpuCfg;
    vsCpuCfg.settleDurationMs = 100;
    vsCpuCfg.cpuIdleDelayMs = 20;

    runtime::GameStateMachine vsCpuSm(vsCpuCfg);
    vsCpuSm.Reset();

    test_support::BoundedHintEngine vsCpuEngine;
    modules::GamePersistenceCoordinator vsCpuPersistence(vsCpuSettings, vsCpuEngine, vsCpuSm);
    Check(vsCpuPersistence.LoadOrInitialize(), "vs-cpu persistence load/init");
    vsCpuSm.DebugSetPhysicalOccupancyMask(vsCpuSm.GetExpectedOccupancyMask());

    const char* const vsCpuLines[] = {
        "{\"type\":\"header\",\"ver\":1}",
        "{\"type\":\"settings\",\"t_ms\":1000,\"json\":{\"BOARD_ROTATE\":0}}",
        "{\"type\":\"sensor\",\"t_ms\":1010,\"square\":52,\"occ\":0}",
        "{\"type\":\"checkpoint_state\",\"t_ms\":1010,\"game_state\":\"PickedUp\"}",
        "{\"type\":\"sensor\",\"t_ms\":1020,\"square\":36,\"occ\":1}",
        "{\"type\":\"checkpoint_state\",\"t_ms\":1020,\"game_state\":\"Settling\"}",
        "{\"type\":\"tick\",\"t_ms\":1120}",
        "{\"type\":\"checkpoint_state\",\"t_ms\":1120,\"game_state\":\"Idle\"}",
        "{\"type\":\"checkpoint_engine\",\"t_ms\":1120,\"fen\":\"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1\"}",
        "{\"type\":\"tick\",\"t_ms\":1260}",
        "{\"type\":\"checkpoint_state\",\"t_ms\":1260,\"game_state\":\"CpuMoving\"}",
        "{\"type\":\"cmd\",\"t_ms\":1265,\"cmd\":\"persist reload\"}",
        "{\"type\":\"checkpoint_state\",\"t_ms\":1265,\"game_state\":\"CpuMoving\"}",
        "{\"type\":\"checkpoint_engine\",\"t_ms\":1265,\"fen\":\"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1\"}",
    };

    trace::ReplayResult vsCpuResult;
    const bool vsCpuOk = trace::ReplayJsonLines(vsCpuLines,
                                                sizeof(vsCpuLines) / sizeof(vsCpuLines[0]),
                                                vsCpuClock,
                                                vsCpuSm,
                                                vsCpuResult,
                                                &vsCpuPersistence,
                                                &vsCpuEngine);
    Check(vsCpuOk, vsCpuResult.ok ? "vs-cpu persist reload replay failed" : vsCpuResult.message);
    Check(vsCpuSettings.ReadValue("TIMER", static_cast<uint32_t>(0U)) == 0U &&
              !vsCpuPersistence.GetGameClock().IsEnabled(),
          "legacy trace without TIMER keeps deployed off default");

    g_failures += RunDuckCombinedTurnTraceReplayTest();

    if (g_failures == 0) {
        std::puts("[TRACE_REPLAY] ok");
        return 0;
    }

    std::printf("[TRACE_REPLAY] failures=%d\n", g_failures);
    return 1;
}
