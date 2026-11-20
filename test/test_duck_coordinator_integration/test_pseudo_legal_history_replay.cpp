#include "bounded_hint_engine.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "duck_coordinator_test_support.hpp"
#include "foxie_embedded/millis.hpp"
#include <litch_max/duck_chess_rules.hpp>
#include "modules/duck_game_persistence.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_variant.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/persisted_side.hpp"

namespace {

constexpr char PRE_FEN[] =
    "k3r3/8/8/8/8/8/8/4K1N1 w - - 0 1";
constexpr uint8_t DUCK_BEFORE = 24U;
constexpr uint8_t DUCK_AFTER = 25U;
constexpr uint8_t KNIGHT_FROM = 62U;
constexpr uint8_t KNIGHT_TO = 45U;

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

uint64_t MaskFor(const uint8_t square) {
    return 1ULL << square;
}

struct ActualEngineHarness {
    explicit ActualEngineHarness(const char* path)
        : settingsConfig(
              duck_coordinator_test::MakeSettingsConfig(path)),
          stateMachineConfig(
              duck_coordinator_test::MakeStateMachineConfig()),
          settings(settingsConfig),
          stateMachine(stateMachineConfig),
          coordinator(settings, engine, stateMachine) {
        duck_coordinator_test::RemoveSettingsFiles(path);
        stateMachine.Reset();
    }

    modules::SettingsStoreConfig settingsConfig;
    runtime::GameStateMachineConfig stateMachineConfig;
    modules::SettingsStore settings;
    test_support::BoundedHintEngine engine;
    runtime::GameStateMachine stateMachine;
    modules::GamePersistenceCoordinator coordinator;
};

bool StorePseudoLegalHistory(ActualEngineHarness& harness,
                             modules::FenString& outPostFen) {
    litch_max::DuckChessMoveResult applied;
    if (!litch_max::TryApplyDuckMove(
            PRE_FEN, DUCK_BEFORE, "g1f3", applied) ||
        !outPostFen.Assign(applied.resultingFen.CStr()) ||
        !harness.settings.Load()) {
        return false;
    }

    modules::GamePersistenceState state;
    if (!state.fen.Assign(outPostFen.CStr()) ||
        !state.undoBaseFen.Assign(PRE_FEN) ||
        !state.duckUndoState.Assign("1819")) {
        return false;
    }
    state.undoHistory[0] = modules::PersistedSideToU8(
        modules::PersistedSide::White);
    state.undoHistory[1] = static_cast<uint8_t>('N');
    state.undoHistory[5] = KNIGHT_FROM;
    state.undoHistory[6] = KNIGHT_TO;
    state.undoHistoryCount = modules::UNDO_HISTORY_FIELDS_PER_MOVE;

    modules::DuckGameDurableState durable;
    durable.variant = modules::GameVariant::Duck;
    durable.duckSquare = DUCK_AFTER;
    char encoded[32] = {};
    return modules::EncodeDuckGameDurableState(
               durable, encoded, sizeof(encoded)) &&
           state.variantState.Assign(encoded) &&
           modules::WriteGamePersistenceState(
               harness.settings.Doc(), state) &&
           harness.settings.SaveNow();
}

void RelocateDuck(ActualEngineHarness& harness,
                  uint64_t& physical,
                  const uint8_t from,
                  const uint8_t to) {
    physical &= ~MaskFor(from);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(from), physical);
    physical |= MaskFor(to);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(to), physical);
    harness.coordinator.Update();
}

void TestPseudoLegalHistoryRebuildAndCancelUndo() {
    fxe::ManualMillis::SetNowMs(40000U);
    ActualEngineHarness harness(
        "build/test_duck_coord_pseudo_history.json");
    modules::FenString postFen;
    Check(StorePseudoLegalHistory(harness, postFen),
          "pseudo-legal Duck history fixture persists");
    Check(harness.coordinator.LoadOrInitialize(),
          "reboot rebuild accepts Duck move that leaves king in check");

    uint64_t physical =
        duck_coordinator_test::FenOccupancy(postFen.CStr()) |
        MaskFor(DUCK_AFTER);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    Check(harness.coordinator.RequestUndo(1U),
          "pseudo-legal Duck history begins whole-turn undo");
    RelocateDuck(
        harness, physical, DUCK_AFTER, DUCK_BEFORE);
    uint8_t undoFrom = 0U;
    uint8_t undoTo = 0U;
    Check(harness.stateMachine.TryGetUndoPromptMove(
              undoFrom, undoTo) &&
              undoFrom == KNIGHT_FROM && undoTo == KNIGHT_TO,
          "pseudo-legal history retains inverse chess identity");

    harness.coordinator.Update();
    char fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    Check(harness.engine.GetFen(fen, sizeof(fen)) &&
              std::strcmp(fen, PRE_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "logical undo restores pre-move Duck FEN");

    Check(harness.coordinator.RequestUndo(1U),
          "cancel-undo accepts pseudo-legal Duck move");
    harness.coordinator.Update();
    Check(harness.engine.GetFen(fen, sizeof(fen)),
          "cancel-undo leaves a readable engine position");
    Check(duck_coordinator_test::FenOccupancy(fen) ==
              duck_coordinator_test::FenOccupancy(postFen.CStr()) &&
              std::strstr(fen, " b ") != nullptr,
          "cancel-undo replays Duck pseudo-legality, not orthodox check");
    Check(harness.coordinator.GetUndoHistoryMoveCount() == 1U,
          "cancel-undo restores the combined-turn history entry");
    uint8_t redoFrom = modules::INVALID_DUCK_SQUARE;
    uint8_t redoTo = modules::INVALID_DUCK_SQUARE;
    Check(harness.stateMachine.TryGetDuckRelocation(
              redoFrom, redoTo) &&
              redoFrom == DUCK_BEFORE && redoTo == DUCK_AFTER,
          "cancel-undo still requires the recorded Duck redo");
    RelocateDuck(
        harness, physical, DUCK_BEFORE, DUCK_AFTER);
    Check(harness.coordinator.GetDuckSquare() == DUCK_AFTER,
          "pseudo-legal cancel-undo completes the Duck redo relocation");
    Check(harness.coordinator.GetUndoHistoryMoveCount() == 1U,
          "pseudo-legal Duck redo retains one complete combined turn");
}

} // namespace

int RunDuckPseudoLegalHistoryReplayTests() {
    TestPseudoLegalHistoryRebuildAndCancelUndo();
    return g_failures;
}
