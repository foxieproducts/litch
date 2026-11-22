#include <cstdint>
#include <cstdio>
#include <cstring>

#include "duck_coordinator_test_support.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_variant.hpp"
#include "modules/persisted_side.hpp"

int RunDuckCoordinatorPersistenceClockTests();

namespace {

using duck_coordinator_test::Harness;

int g_failures = 0;

constexpr uint8_t DUCK_FROM = 24U;
constexpr uint8_t DUCK_TO = 25U;
constexpr char KNIGHT_START_FEN[] =
    "6nk/8/8/8/8/8/8/6NK w - - 0 1";
constexpr char KNIGHT_AFTER_WHITE_FEN[] =
    "6nk/8/8/8/8/5N2/8/7K b - - 1 1";

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

bool CommitKnightTurn(Harness& harness,
                      uint64_t& physical,
                      const uint8_t from,
                      const uint8_t to,
                      const uint8_t duckFrom,
                      const uint8_t duckTo) {
    physical &= ~MaskFor(from);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(from), physical);
    harness.coordinator.UpdateLegalDestinationsMask();
    physical |= MaskFor(to);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(to), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    if (harness.coordinator.GetDuckTurnPhase() !=
        modules::DuckTurnPhase::DuckRelocation) {
        return false;
    }
    physical &= ~MaskFor(duckFrom);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(duckFrom), physical);
    physical |= MaskFor(duckTo);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(duckTo), physical);
    harness.coordinator.Update();
    return harness.coordinator.GetDuckTurnPhase() ==
               modules::DuckTurnPhase::ChessMove &&
           harness.coordinator.GetDuckSquare() == duckTo;
}

void TestUndoHistoryCapacityRolloverKeepsDuckPairAligned() {
    fxe::ManualMillis::SetNowMs(20000U);
    Harness harness("build/test_duck_coord_capacity.json");
    const bool seeded = duck_coordinator_test::SeedPersistedDuckPosition(
        harness, KNIGHT_START_FEN, DUCK_FROM);
    harness.settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    Check(seeded && harness.coordinator.LoadOrInitialize(),
          "capacity Duck position loads");
    uint64_t physical =
        duck_coordinator_test::FenOccupancy(KNIGHT_START_FEN) |
        MaskFor(DUCK_FROM);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);

    for (uint16_t turn = 0U; turn < 129U; ++turn) {
        const uint8_t cycle = static_cast<uint8_t>(turn % 4U);
        const uint8_t from = cycle == 0U ? 62U : cycle == 1U ? 6U
                                             : cycle == 2U   ? 45U
                                                             : 21U;
        const uint8_t to = cycle == 0U ? 45U : cycle == 1U ? 21U
                                           : cycle == 2U   ? 62U
                                                           : 6U;
        const uint8_t duckFrom = (turn % 2U) == 0U
                                     ? DUCK_FROM
                                     : DUCK_TO;
        const uint8_t duckTo = (turn % 2U) == 0U
                                   ? DUCK_TO
                                   : DUCK_FROM;
        if (!CommitKnightTurn(
                harness, physical, from, to, duckFrom, duckTo)) {
            Check(false, "all 129 Duck turns commit across capacity rollover");
            return;
        }
    }

    Check(harness.coordinator.GetUndoHistoryMoveCount() == 128U &&
              harness.coordinator.GetDuckSquare() == DUCK_TO,
          "129th turn rolls the bounded history to 128 aligned turns");
    physical &= ~MaskFor(DUCK_TO);
    const runtime::AppEvent graceLift =
        runtime::AppEvent::MakeBoardObservation(
            fxe::Millis(), MaskFor(DUCK_TO), physical);
    Check(harness.coordinator.TryHandleTakebackGraceBoardEvent(
              graceLift, MaskFor(DUCK_TO), physical),
          "grace after capacity rollover begins latest duck inverse");
    uint8_t requestedDuckFrom = modules::INVALID_DUCK_SQUARE;
    uint8_t requestedDuckTo = modules::INVALID_DUCK_SQUARE;
    Check(harness.stateMachine.TryGetDuckRelocation(
              requestedDuckFrom, requestedDuckTo) &&
              requestedDuckFrom == DUCK_TO &&
              requestedDuckTo == DUCK_FROM,
          "rollover retains latest Duck undo pair");

    physical |= MaskFor(DUCK_FROM);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_FROM), physical);
    harness.coordinator.Update();
    uint8_t undoFrom = 0U;
    uint8_t undoTo = 0U;
    Check(harness.stateMachine.TryGetUndoPromptMove(undoFrom, undoTo) &&
              undoFrom == 62U && undoTo == 45U,
          "rollover pairs latest Duck move with latest chess move");
}

bool StoreOneMoveWithoutDuckUndo(Harness& harness,
                                 const modules::GameVariant variant) {
    if (!harness.settings.Load()) {
        return false;
    }
    modules::GamePersistenceState state;
    if (!state.fen.Assign(KNIGHT_AFTER_WHITE_FEN) ||
        !state.undoBaseFen.Assign(KNIGHT_START_FEN)) {
        return false;
    }
    state.undoHistory[0] = modules::PersistedSideToU8(
        modules::PersistedSide::White);
    state.undoHistory[1] = static_cast<uint8_t>('N');
    state.undoHistory[5] = 62U;
    state.undoHistory[6] = 45U;
    state.undoHistoryCount = modules::UNDO_HISTORY_FIELDS_PER_MOVE;

    modules::DuckGameDurableState durable;
    durable.variant = variant;
    durable.duckSquare = variant == modules::GameVariant::Duck
                             ? DUCK_FROM
                             : modules::INVALID_DUCK_SQUARE;
    char encoded[32] = {};
    return modules::EncodeDuckGameDurableState(
               durable, encoded, sizeof(encoded)) &&
           state.variantState.Assign(encoded) &&
           modules::WriteGamePersistenceState(
               harness.settings.Doc(), state) &&
           harness.settings.SaveNow();
}

void TestMissingDuckUndoRecordFailsClosedButClassicStillLoads() {
    fxe::ManualMillis::SetNowMs(30000U);
    Harness duck("build/test_duck_coord_missing_duck_undo.json");
    Check(StoreOneMoveWithoutDuckUndo(
              duck, modules::GameVariant::Duck),
          "Duck fixture stores history without aligned Duck record");
    Check(!duck.coordinator.LoadOrInitialize(),
          "Duck history without GAME_DUCK_UNDO_V1 fails closed");

    Harness classic("build/test_duck_coord_legacy_classic_undo.json");
    Check(StoreOneMoveWithoutDuckUndo(
              classic, modules::GameVariant::Classic),
          "Classic fixture stores legacy history without Duck record");
    Check(classic.coordinator.LoadOrInitialize() &&
              classic.coordinator.GetGameVariant() ==
                  modules::GameVariant::Classic &&
              classic.coordinator.GetUndoHistoryMoveCount() == 1U,
          "Classic history remains backward compatible without Duck record");
}

struct StoredDuckHistory {
    const char* baseFen = nullptr;
    const char* currentFen = nullptr;
    const char* duckUndo = nullptr;
    uint8_t currentDuckSquare = modules::INVALID_DUCK_SQUARE;
    modules::VariantOutcome outcome{};
    uint8_t fields[2U * modules::UNDO_HISTORY_FIELDS_PER_MOVE] = {};
    uint16_t fieldCount = 0U;
};

bool StoreDuckHistory(Harness& harness,
                      const StoredDuckHistory& fixture) {
    if (!harness.settings.Load()) {
        return false;
    }
    modules::GamePersistenceState state;
    if (!state.fen.Assign(fixture.currentFen) ||
        !state.undoBaseFen.Assign(fixture.baseFen) ||
        !state.duckUndoState.Assign(fixture.duckUndo) ||
        fixture.fieldCount > sizeof(state.undoHistory)) {
        return false;
    }
    std::memcpy(state.undoHistory, fixture.fields, fixture.fieldCount);
    state.undoHistoryCount = fixture.fieldCount;
    modules::DuckGameDurableState durable;
    durable.variant = modules::GameVariant::Duck;
    durable.duckSquare = fixture.currentDuckSquare;
    durable.outcome = fixture.outcome;
    char encoded[32] = {};
    return modules::EncodeDuckGameDurableState(
               durable, encoded, sizeof(encoded)) &&
           state.variantState.Assign(encoded) &&
           modules::WriteGamePersistenceState(
               harness.settings.Doc(), state) &&
           harness.settings.SaveNow();
}

StoredDuckHistory MakeNormalOneMoveHistory(const char* duckUndo,
                                           const uint8_t duckSquare) {
    StoredDuckHistory fixture;
    fixture.baseFen = KNIGHT_START_FEN;
    fixture.currentFen = KNIGHT_AFTER_WHITE_FEN;
    fixture.duckUndo = duckUndo;
    fixture.currentDuckSquare = duckSquare;
    fixture.fields[0] = modules::PersistedSideToU8(
        modules::PersistedSide::White);
    fixture.fields[1] = static_cast<uint8_t>('N');
    fixture.fields[5] = 62U;
    fixture.fields[6] = 45U;
    fixture.fieldCount = modules::UNDO_HISTORY_FIELDS_PER_MOVE;
    return fixture;
}

void TestDuckUndoSemanticCorruptionFailsClosed() {
    Harness absent("build/test_duck_coord_absent_pair.json");
    const StoredDuckHistory absentFixture =
        MakeNormalOneMoveHistory("ffff", DUCK_FROM);
    Check(StoreDuckHistory(absent, absentFixture),
          "ordinary Duck move fixture stores absent pair");
    Check(!absent.coordinator.LoadOrInitialize(),
          "ordinary Duck move cannot decode as no relocation");

    Harness wrongCurrent("build/test_duck_coord_wrong_current_duck.json");
    const StoredDuckHistory wrongCurrentFixture =
        MakeNormalOneMoveHistory("1819", DUCK_FROM);
    Check(StoreDuckHistory(wrongCurrent, wrongCurrentFixture),
          "Duck fixture stores pair ending away from durable square");
    Check(!wrongCurrent.coordinator.LoadOrInitialize(),
          "Duck undo tail must end at durable current square");

    static constexpr char TWO_MOVE_FEN[] =
        "7k/8/5n2/8/8/5N2/8/7K w - - 2 2";
    StoredDuckHistory brokenChain;
    brokenChain.baseFen = KNIGHT_START_FEN;
    brokenChain.currentFen = TWO_MOVE_FEN;
    brokenChain.duckUndo = "18191b1a";
    brokenChain.currentDuckSquare = 26U;
    brokenChain.fields[0] = modules::PersistedSideToU8(
        modules::PersistedSide::White);
    brokenChain.fields[1] = static_cast<uint8_t>('N');
    brokenChain.fields[5] = 62U;
    brokenChain.fields[6] = 45U;
    brokenChain.fields[7] = modules::PersistedSideToU8(
        modules::PersistedSide::Black);
    brokenChain.fields[8] = static_cast<uint8_t>('n');
    brokenChain.fields[12] = 6U;
    brokenChain.fields[13] = 21U;
    brokenChain.fieldCount =
        2U * modules::UNDO_HISTORY_FIELDS_PER_MOVE;
    Harness chain("build/test_duck_coord_broken_duck_chain.json");
    Check(StoreDuckHistory(chain, brokenChain),
          "Duck fixture stores discontinuous relocation chain");
    Check(!chain.coordinator.LoadOrInitialize(),
          "Duck undo relocation chain must remain continuous");
}

void TestKingCaptureWithoutDuckRelocationRestores() {
    static constexpr char PRE_FEN[] =
        "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1";
    static constexpr char POST_FEN[] =
        "4Q3/8/8/8/8/8/8/4K3 b - - 0 1";
    StoredDuckHistory fixture;
    fixture.baseFen = PRE_FEN;
    fixture.currentFen = POST_FEN;
    fixture.duckUndo = "ffff";
    fixture.currentDuckSquare = 32U;
    fixture.outcome = modules::VariantOutcome{
        modules::VariantOutcomeReason::KingCaptured,
        modules::GameWinner::White};
    fixture.fields[0] = modules::PersistedSideToU8(
        modules::PersistedSide::White);
    fixture.fields[1] = static_cast<uint8_t>('Q');
    fixture.fields[2] = static_cast<uint8_t>('k');
    fixture.fields[3] = 4U;
    fixture.fields[5] = 52U;
    fixture.fields[6] = 4U;
    fixture.fieldCount = modules::UNDO_HISTORY_FIELDS_PER_MOVE;

    Harness harness("build/test_duck_coord_king_capture_restore.json");
    Check(StoreDuckHistory(harness, fixture),
          "king-capture fixture stores absent Duck relocation");
    Check(harness.coordinator.LoadOrInitialize() &&
              harness.coordinator.GetVariantOutcome().reason ==
                  modules::VariantOutcomeReason::KingCaptured &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U,
          "terminal king capture legitimately restores without relocation");
}

void TestPersistedCaptureIdentityMustMatchReplay() {
    StoredDuckHistory falseCapture =
        MakeNormalOneMoveHistory("ffff", DUCK_FROM);
    falseCapture.outcome = modules::VariantOutcome{
        modules::VariantOutcomeReason::KingCaptured,
        modules::GameWinner::White};
    falseCapture.fields[2] = static_cast<uint8_t>('k');
    falseCapture.fields[3] = 7U;
    Harness falseCaptureHarness(
        "build/test_duck_coord_false_king_capture.json");
    Check(StoreDuckHistory(falseCaptureHarness, falseCapture),
          "false king-capture history fixture stores");
    Check(!falseCaptureHarness.coordinator.LoadOrInitialize(),
          "ordinary move cannot be restored as a recorded king capture");

    static constexpr char PRE_FEN[] =
        "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1";
    static constexpr char POST_FEN[] =
        "4Q3/8/8/8/8/8/8/4K3 b - - 0 1";
    StoredDuckHistory hiddenCapture;
    hiddenCapture.baseFen = PRE_FEN;
    hiddenCapture.currentFen = POST_FEN;
    hiddenCapture.duckUndo = "2021";
    hiddenCapture.currentDuckSquare = 33U;
    hiddenCapture.fields[0] = modules::PersistedSideToU8(
        modules::PersistedSide::White);
    hiddenCapture.fields[1] = static_cast<uint8_t>('Q');
    hiddenCapture.fields[5] = 52U;
    hiddenCapture.fields[6] = 4U;
    hiddenCapture.fieldCount = modules::UNDO_HISTORY_FIELDS_PER_MOVE;
    Harness hiddenCaptureHarness(
        "build/test_duck_coord_hidden_king_capture.json");
    Check(StoreDuckHistory(hiddenCaptureHarness, hiddenCapture),
          "unlabelled king-capture history fixture stores");
    Check(!hiddenCaptureHarness.coordinator.LoadOrInitialize(),
          "king capture cannot restore as an ordinary relocated turn");
}

void TestPersistedDuckCannotOverlapFenPiece() {
    StoredDuckHistory fixture;
    fixture.baseFen = KNIGHT_START_FEN;
    fixture.currentFen = KNIGHT_START_FEN;
    fixture.duckUndo = "";
    fixture.currentDuckSquare = 62U;
    Harness harness("build/test_duck_coord_overlap_fen_piece.json");
    Check(StoreDuckHistory(harness, fixture),
          "overlapping Duck/FEN fixture stores corrupt durable state");
    Check(!harness.coordinator.LoadOrInitialize(),
          "persisted Duck square overlapping a chess piece fails closed");
}

} // namespace

int RunDuckCoordinatorRecoveryCapacityTests() {
    g_failures += RunDuckCoordinatorPersistenceClockTests();
    TestUndoHistoryCapacityRolloverKeepsDuckPairAligned();
    TestMissingDuckUndoRecordFailsClosedButClassicStillLoads();
    TestDuckUndoSemanticCorruptionFailsClosed();
    TestKingCaptureWithoutDuckRelocationRestores();
    TestPersistedCaptureIdentityMustMatchReplay();
    TestPersistedDuckCannotOverlapFenPiece();
    return g_failures;
}
