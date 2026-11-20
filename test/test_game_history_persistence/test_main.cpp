#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/initial_fen.hpp"
#include "runtime/clock.hpp"

#include <ArduinoJson.h>

#include "modules/game_history_persistence.hpp"
#include "modules/game_persistence.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) return;
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

modules::GameHistoryPersistenceState Example() {
    modules::GameHistoryPersistenceState state;
    state.present = true;
    for (uint8_t index = 0U; index < state.gameId.bytes.size(); ++index) {
        state.gameId.bytes[index] = static_cast<uint8_t>(index + 1U);
    }
    state.lineRevision = 0x0102030405060708ULL;
    state.currentPly = 4096U;
    state.captureIncomplete = true;
    state.shouldSeal = true;
    state.status = modules::game_history::ArchiveStatus::Completed;
    state.outcome = modules::game_history::Outcome::Checkmate;
    state.winner = modules::game_history::Winner::White;
    return state;
}

void TestExactRoundTripAndEmbedding() {
    StaticJsonDocument<4096> doc;
    const modules::GameHistoryPersistenceState input = Example();
    Check(modules::WriteGameHistoryPersistenceState(doc, input),
          "history metadata writes");
    Check(std::strcmp(doc[modules::KEY_GAME_HISTORY_V1][1U] | "",
                      "0102030405060708090a0b0c0d0e0f10") == 0 &&
              std::strcmp(doc[modules::KEY_GAME_HISTORY_V1][2U] | "",
                          "0102030405060708") == 0,
          "identity and revision use exact lowercase fixed-width hex");

    modules::GameHistoryPersistenceState output;
    Check(modules::ReadGameHistoryPersistenceState(doc, output) &&
              output.present && output.gameId == input.gameId &&
              output.lineRevision == input.lineRevision &&
              output.currentPly == input.currentPly &&
              output.captureIncomplete && output.shouldSeal &&
              output.status == input.status &&
              output.outcome == input.outcome &&
              output.winner == input.winner,
          "history metadata round-trips without JSON number narrowing");

    modules::GamePersistenceState game;
    Check(game.fen.Assign(
              "8/8/8/8/8/8/8/8 w - - 0 1"),
          "game fixture FEN fits");
    game.gameHistory = input;
    StaticJsonDocument<4096> embedded;
    Check(modules::WriteGamePersistenceState(embedded, game),
          "game persistence embeds archive identity atomically");
    modules::GamePersistenceState decoded;
    Check(modules::ReadGamePersistenceState(embedded, decoded) &&
              decoded.gameHistory.present &&
              decoded.gameHistory.gameId == input.gameId,
          "embedded game history metadata restores");
}

void TestAbsentAndMalformedFailClosed() {
    StaticJsonDocument<512> doc;
    modules::GameHistoryPersistenceState output;
    Check(modules::ReadGameHistoryPersistenceState(doc, output) &&
              !output.present,
          "legacy snapshot without metadata remains readable");

    doc[modules::KEY_GAME_HISTORY_V1] =
        "not-a-versioned-record";
    Check(!modules::ReadGameHistoryPersistenceState(doc, output),
          "wrong metadata shape is rejected");

    doc.clear();
    JsonArray record =
        doc[modules::KEY_GAME_HISTORY_V1].to<JsonArray>();
    record.add(1U);
    record.add("00000000000000000000000000000000");
    record.add("0000000000000001");
    record.add(0U);
    record.add(0U);
    record.add(0U);
    record.add(static_cast<uint8_t>(
        modules::game_history::ArchiveStatus::Unfinished));
    record.add(static_cast<uint8_t>(
        modules::game_history::Outcome::Ongoing));
    record.add(static_cast<uint8_t>(
        modules::game_history::Winner::None));
    Check(!modules::ReadGameHistoryPersistenceState(doc, output),
          "zero game identity is rejected");

    doc.clear();
    const auto invalidRevision = Example();
    auto invalid = invalidRevision;
    invalid.lineRevision = modules::game_history::MAX_LINE_REVISION + 1U;
    Check(!modules::WriteGameHistoryPersistenceState(doc, invalid),
          "revision beyond app-safe range is rejected");
}

void TestClearRemovesHistoryWithCurrentGame() {
    StaticJsonDocument<512> doc;
    Check(modules::WriteGameHistoryPersistenceState(doc, Example()),
          "clear fixture writes");
    modules::ClearGameState(doc);
    Check(!doc.containsKey(modules::KEY_GAME_HISTORY_V1),
          "current-game clear removes its archive identity");
}


void TestEnumWidthsAndAuxiliaryRecovery() {
    for (uint8_t field = 6U; field < 9U; ++field) {
        StaticJsonDocument<1024> doc;
        Check(modules::WriteGameHistoryPersistenceState(doc, Example()),
              "enum fixture writes");
        const uint32_t original = doc[modules::KEY_GAME_HISTORY_V1][field];
        doc[modules::KEY_GAME_HISTORY_V1][field] = original + 256U;
        modules::GameHistoryPersistenceState decoded;
        Check(!modules::ReadGameHistoryPersistenceState(doc, decoded),
              "wide enum cannot alias a valid status/outcome/winner");
    }
    StaticJsonDocument<4096> doc;
    modules::GamePersistenceState state;
    Check(state.fen.Assign(modules::InitialFenForFirstPlayerIsP2(false)), "initial FEN fits");
    Check(modules::WriteGamePersistenceState(doc, state), "recovery FEN writes");
    doc[modules::KEY_GAME_HISTORY_V1] = "damaged archive";
    modules::GamePersistenceState decoded;
    Check(modules::ReadGamePersistenceState(doc, decoded) &&
              decoded.fen == state.fen && !decoded.gameHistory.present,
          "damaged optional archive does not prevent current game restore");
    auto history = Example();
    history.currentPly = 0U;
    Check(modules::WriteGameHistoryPersistenceState(doc, history),
          "contradictory archive fixture writes");
    const uint8_t move[] = {0U, 1U, 0U, 0U, 0U, 52U, 44U};
    Check(modules::WriteUndoHistory(doc, move, sizeof(move)), "undo fixture writes");
    Check(modules::ReadGamePersistenceState(doc, decoded) &&
              decoded.undoHistoryCount == sizeof(move) &&
              !decoded.gameHistory.present,
          "archive ply contradiction preserves game and valid Undo tail");
}

bool GenerateIdentity(void* context, modules::game_history::GameId& id) {
    auto& calls = *static_cast<unsigned*>(context);
    id = {};
    id.bytes[0] = static_cast<uint8_t>(++calls);
    return true;
}

void TestDamagedArchiveSurvivesCoordinatorSave(const size_t damageLength) {
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_history_auxiliary_recovery.json";
    std::remove(cfg.path);
    std::remove("build/test_history_auxiliary_recovery.json.bak");
    std::remove("build/test_history_auxiliary_recovery.json.tmp");
    modules::SettingsStore settings(cfg);
    Check(settings.Load(), "recovery settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = modules::InitialFenForFirstPlayerIsP2(false);
    const std::string damage(damageLength, 'x');
    settings.Doc()[modules::KEY_GAME_HISTORY_V1] = damage;
    Check(settings.SaveNow(), "persist malformed auxiliary record with valid game");
    runtime::ManualClock clock(1000U);
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    runtime::GameStateMachine sm(config);
    sm.Reset();
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
    unsigned generated = 0;
    coordinator.SetGameHistoryIdentityGenerator(&GenerateIdentity, &generated);
    const bool restored = coordinator.LoadOrInitialize();
    Check(restored, "coordinator restores valid game despite archive damage");
    if (!restored) return;
    Check(generated == 0U, "corrupt archive must not be silently reminted as legacy");
    // Exercise the real move finalization and snapshot compaction owners. Sensor
    // occupancy is narrowed to the mover here; this is persistence coverage,
    // not a complete board interaction/LED acceptance journey.
    sm.SetExpectedOccupancyMask(1ULL << 52U);
    sm.DebugSetPhysicalOccupancyMask(1ULL << 52U);
    sm.HandleSensorChange(52U, false);
    sm.HandleSensorChange(44U, true);
    clock.AdvanceMs(1000U);
    sm.Tick();
    coordinator.Update();
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "ordinary play continues with malformed archive metadata");
    Check(std::strcmp(settings.Doc()[modules::KEY_GAME_HISTORY_V1] | "",
                      damage.c_str()) == 0,
          "snapshot compaction and fallback preserve raw damaged archive");
    Check(settings.SaveNow(), "continued game can still reach durable storage");
    modules::SettingsStore reloaded(cfg);
    Check(reloaded.Load(), "continued game reloads");
    modules::GamePersistenceState persisted;
    Check(modules::ReadGamePersistenceState(reloaded.Doc(), persisted) &&
              persisted.undoHistoryCount == modules::UNDO_HISTORY_FIELDS_PER_MOVE,
          "new move survives save/reload despite archive damage");
    Check(std::strcmp(reloaded.Doc()[modules::KEY_GAME_HISTORY_V1] | "",
                      damage.c_str()) == 0, "raw archive damage survives disk round trip");
    Check(coordinator.ResetEngine(), "explicit new game still works");
    Check(generated == 1U && coordinator.GetGameHistoryPersistenceState().present,
          "explicit new game replaces quarantined archive with fresh identity");
}


void TestLegacyMigrationWaitsForUserSaveWithoutDroppingUndo() {
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_history_identity_migration.json";
    std::remove(cfg.path);
    std::remove("build/test_history_identity_migration.json.bak");
    std::remove("build/test_history_identity_migration.json.tmp");
    modules::SettingsStore settings(cfg);
    Check(settings.Load(), "migration settings load");
    modules::GamePersistenceState legacy;
    (void)legacy.fen.Assign("rnbqkbnr/pppppppp/8/8/8/4P3/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
    (void)legacy.undoBaseFen.Assign(modules::InitialFenForFirstPlayerIsP2(false));
    const uint8_t move[] = {0U, 1U, 0U, 0U, 0U, 52U, 44U};
    std::memcpy(legacy.undoHistory, move, sizeof(move));
    legacy.undoHistoryCount = sizeof(move);
    Check(modules::WriteGamePersistenceState(settings.Doc(), legacy) && settings.SaveNowBlocking(),
          "legacy game with real retained Undo is durable");
    const auto readSaved = [&]() {
        std::ifstream file(cfg.path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    const std::string legacyBytes = readSaved();
    Check(!legacyBytes.empty(), "legacy primary exists before boot migration");
    runtime::GameStateMachineConfig config;
    runtime::GameStateMachine sm(config);
    sm.Reset();
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
    unsigned generated = 0U;
    coordinator.SetGameHistoryIdentityGenerator(&GenerateIdentity, &generated);
    Check(coordinator.LoadOrInitialize(), "legacy game restores with identity generator");
    modules::GamePersistenceState candidate;
    Check(modules::ReadGamePersistenceState(settings.Doc(), candidate) && candidate.gameHistory.present &&
              candidate.gameHistory.currentPly == 1U && candidate.undoHistoryCount == sizeof(move) &&
              std::memcmp(candidate.undoHistory, move, sizeof(move)) == 0,
          "migration prepares identity and retains Undo before any move");
    unsigned saves = 0U;
    settings.SetAsyncSaveRequestCallback([&](const JsonDocument& doc) {
        modules::GamePersistenceState queued;
        Check(modules::ReadGamePersistenceState(doc, queued) && queued.gameHistory.present &&
                  queued.undoHistoryCount == sizeof(move), "queued migration has matching game and identity");
        ++saves;
        return true;
    });
    fxe::ManualMillis::AdvanceMs(cfg.mustSaveDelayMs + cfg.deferredSaveDelayMs + 1U);
    settings.Update();
    Check(saves == 0U && readSaved() == legacyBytes,
          "ordinary boot migration neither schedules a save nor rewrites the durable legacy primary");
    settings.Doc()["MINIMAL_ANIMATIONS"] = true;
    settings.MarkDirty();
    fxe::ManualMillis::AdvanceMs(cfg.deferredSaveDelayMs + 1U);
    settings.Update();
    Check(saves == 1U,
          "next user setting save includes migrated identity and intact Undo");
    settings.SetAsyncSaveRequestCallback(nullptr);
    Check(settings.SaveNowBlocking(), "migration promotes to real file");
    modules::SettingsStore reloaded(cfg);
    Check(reloaded.Load(), "migration reloads");
    modules::GamePersistenceState restored;
    Check(modules::ReadGamePersistenceState(reloaded.Doc(), restored) &&
              restored.gameHistory.present &&
              restored.gameHistory.gameId == candidate.gameHistory.gameId &&
              restored.undoHistoryCount == sizeof(move) &&
              reloaded.Doc()["MINIMAL_ANIMATIONS"].as<bool>(),
          "reboot retains user change, exact migrated identity and Undo");
}

void TestCaptureExhaustionSurvivesReboot(const bool revisionLimit) {
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_history_exhaustion.json";
    std::remove(cfg.path);
    std::remove("build/test_history_exhaustion.json.bak");
    std::remove("build/test_history_exhaustion.json.tmp");
    modules::SettingsStore settings(cfg);
    Check(settings.Load(), "exhaustion settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = modules::InitialFenForFirstPlayerIsP2(false);
    auto metadata = Example();
    metadata.currentPly = revisionLimit ? 0U : 4096U;
    metadata.lineRevision = revisionLimit ? modules::game_history::MAX_LINE_REVISION : 4U;
    metadata.captureIncomplete = false;
    metadata.shouldSeal = false;
    metadata.status = modules::game_history::ArchiveStatus::Unfinished;
    metadata.outcome = modules::game_history::Outcome::Ongoing;
    metadata.winner = modules::game_history::Winner::None;
    Check(modules::WriteGameHistoryPersistenceState(settings.Doc(), metadata),
          "saturated but not yet exhausted identity writes");
    runtime::ManualClock clock(1000U);
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    runtime::GameStateMachine sm(config);
    sm.Reset();
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
    Check(coordinator.LoadOrInitialize(), "saturated identity restores game");
    sm.SetExpectedOccupancyMask(1ULL << 52U);
    sm.DebugSetPhysicalOccupancyMask(1ULL << 52U);
    sm.HandleSensorChange(52U, false);
    sm.HandleSensorChange(44U, true);
    clock.AdvanceMs(1000U);
    sm.Tick();
    coordinator.Update();
    Check(coordinator.GetUndoHistoryMoveCount() == 1U, "play continues at archive bound");
    Check(coordinator.GetGameHistoryPersistenceState().IsCaptureExhausted(),
          "unrepresentable mutation stops archive tracking");
    Check(settings.SaveNow(), "exhausted identity saves with the current game");
    modules::SettingsStore loaded(cfg);
    Check(loaded.Load(), "exhaustion reload reads file");
    runtime::GameStateMachine sm2(config);
    sm2.Reset();
    test_support::BoundedHintEngine engine2;
    engine2.Reset();
    modules::GamePersistenceCoordinator restored(loaded, engine2, sm2);
    Check(restored.LoadOrInitialize(), "exhausted archive still restores current game");
    const auto stopped = restored.GetGameHistoryPersistenceState();
    Check(stopped.IsCaptureExhausted(), "capture stop survives reboot");
    Check(restored.RequestUndo(1U), "Undo remains available after archive exhaustion");
    restored.Update();
    Check(restored.GetUndoHistoryMoveCount() == 0U, "Undo actually removes the retained move");
    const auto undone = restored.GetGameHistoryPersistenceState();
    Check(undone.IsCaptureExhausted() && undone.lineRevision == stopped.lineRevision &&
              undone.currentPly == stopped.currentPly,
          "Undo cannot resume an exhausted identity or wrap its counters");
}

} // namespace

int main() {
    TestLegacyMigrationWaitsForUserSaveWithoutDroppingUndo();
    TestCaptureExhaustionSurvivesReboot(false);
    TestCaptureExhaustionSurvivesReboot(true);
    TestEnumWidthsAndAuxiliaryRecovery();
    TestDamagedArchiveSurvivesCoordinatorSave(16U);
    TestDamagedArchiveSurvivesCoordinatorSave(1100U);
    TestExactRoundTripAndEmbedding();
    TestAbsentAndMalformedFailClosed();
    TestClearRemovesHistoryWithCurrentGame();
    if (g_failures == 0) {
        std::puts("[PASS] game-history persistence metadata tests");
    }
    return g_failures == 0 ? 0 : 1;
}
