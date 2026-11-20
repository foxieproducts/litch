#include <cstdio>
#include <cstring>

#include "modules/duck_game_persistence.hpp"
#include "modules/game_history_snapshot_reader.hpp"

namespace {

using namespace modules;
using namespace modules::game_history;

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) return;
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

GameId Id() {
    GameId id;
    id.bytes[0U] = 0x42U;
    id.bytes[15U] = 0xA5U;
    return id;
}

void SetVariant(GamePersistenceState& state,
                const GameVariant variant,
                const uint8_t duckSquare = INVALID_DUCK_SQUARE,
                const VariantOutcome outcome = {}) {
    DuckGameDurableState durable;
    durable.variant = variant;
    durable.duckSquare = duckSquare;
    durable.outcome = outcome;
    char encoded[32] = {};
    Check(EncodeDuckGameDurableState(durable, encoded, sizeof(encoded)) &&
              state.variantState.Assign(encoded),
          "variant fixture encodes");
}

void SetHistory(GamePersistenceState& state,
                const uint16_t currentPly,
                const uint64_t revision) {
    state.gameHistory.present = true;
    state.gameHistory.gameId = Id();
    state.gameHistory.currentPly = currentPly;
    state.gameHistory.lineRevision = revision;
}

void AddMove(GamePersistenceState& state,
             const uint8_t side,
             const char mover,
             const char captured,
             const uint8_t captureSquare,
             const char promotion,
             const uint8_t from,
             const uint8_t to) {
    const uint16_t offset = state.undoHistoryCount;
    state.undoHistory[offset + 0U] = side;
    state.undoHistory[offset + 1U] = static_cast<uint8_t>(mover);
    state.undoHistory[offset + 2U] = static_cast<uint8_t>(captured);
    state.undoHistory[offset + 3U] = captureSquare;
    state.undoHistory[offset + 4U] = static_cast<uint8_t>(promotion);
    state.undoHistory[offset + 5U] = from;
    state.undoHistory[offset + 6U] = to;
    state.undoHistoryCount = static_cast<uint16_t>(
        offset + UNDO_HISTORY_FIELDS_PER_MOVE);
}

GameHistoryCaptureStep Read(const GameHistoryCaptureSnapshot& snapshot,
                            const uint16_t index) {
    GameHistoryCaptureStep step;
    Check(snapshot.readStep != nullptr &&
              snapshot.readStep(snapshot.stepContext, index, step),
          "snapshot step reads");
    return step;
}

void TestClassicReplayProducesProtocolPositions() {
    GamePersistenceState state;
    Check(state.undoBaseFen.Assign(
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/"
              "RNBQKBNR w KQkq - 0 1") &&
              state.fen.Assign(
                  "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/"
                  "RNBQKBNR w KQkq e6 0 1"),
          "classic fixture FENs fit");
    SetVariant(state, GameVariant::Classic);
    SetHistory(state, 2U, 3U);
    AddMove(state, 0U, 'P', '\0', INVALID_SQUARE, '\0', 52U, 36U);
    AddMove(state, 1U, 'p', '\0', INVALID_SQUARE, '\0', 12U, 28U);

    GameHistorySnapshotReader reader(state);
    GameHistoryCaptureSnapshot snapshot;
    Check(reader.Build(snapshot) && snapshot.basePly == 0U &&
              snapshot.transitionCount == 2U &&
              snapshot.variant == Variant::Classic,
          "classic durable tail becomes a complete capture snapshot");
    const GameHistoryCaptureStep e4 = Read(snapshot, 0U);
    const GameHistoryCaptureStep e5 = Read(snapshot, 1U);
    FenString fen;
    uint8_t duck = 0U;
    Check(e4.transition.side == Side::White &&
              e4.transition.from == 52U && e4.transition.to == 36U &&
              DecodePosition(e4.afterPosition, Variant::Classic, fen, duck) &&
              fen.Equals("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/"
                         "RNBQKBNR b KQkq e3 0 1"),
          "first classic step reconstructs exact FEN metadata");
    Check(DecodePosition(e5.afterPosition, Variant::Classic, fen, duck) &&
              fen.Equals("rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/"
                         "RNBQKBNR w KQkq e6 0 2"),
          "black move advances the reconstructed fullmove counter");
}

void TestCastlingAndSealMetadata() {
    GamePersistenceState state;
    Check(state.undoBaseFen.Assign(
              "4k3/8/8/8/8/8/8/R3K2R w KQ - 7 12") &&
              state.fen.Assign(
                  "4k3/8/8/8/8/8/8/R4RK1 b - - 0 1"),
          "castling fixture FENs fit");
    SetVariant(state, GameVariant::Classic);
    SetHistory(state, 18U, 9U);
    state.gameHistory.shouldSeal = true;
    state.gameHistory.status = ArchiveStatus::Completed;
    state.gameHistory.outcome = Outcome::Checkmate;
    state.gameHistory.winner = Winner::White;
    AddMove(state, 0U, 'K', '\0', INVALID_SQUARE, '\0', 60U, 62U);

    GameHistorySnapshotReader reader(state);
    GameHistoryCaptureSnapshot snapshot;
    Check(reader.Build(snapshot) && snapshot.basePly == 17U &&
              snapshot.shouldSeal && snapshot.outcome == Outcome::Checkmate,
          "partial tail and terminal metadata are preserved");
    const GameHistoryCaptureStep castle = Read(snapshot, 0U);
    FenString fen;
    uint8_t duck = 0U;
    Check(castle.transition.secondaryFrom == 63U &&
              castle.transition.secondaryTo == 61U &&
              DecodePosition(
                  castle.afterPosition, Variant::Classic, fen, duck) &&
              fen.Equals("4k3/8/8/8/8/8/8/R4RK1 b - - 8 12"),
          "castling moves the rook and clears rights");
}

void TestDuckReplayIncludesMandatoryRelocation() {
    GamePersistenceState state;
    Check(state.undoBaseFen.Assign(
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/"
              "RNBQKBNR w KQkq - 0 1") &&
              state.fen.Assign(
                  "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/"
                  "RNBQKBNR b KQkq e3 0 1"),
          "Duck fixture FENs fit");
    SetVariant(state, GameVariant::Duck, 35U);
    SetHistory(state, 1U, 2U);
    AddMove(state, 0U, 'P', '\0', INVALID_SQUARE, '\0', 52U, 36U);
    const uint8_t duckFrom[1] = {27U};
    const uint8_t duckTo[1] = {35U};
    char encoded[8] = {};
    Check(EncodeDuckUndoHistory(
              duckFrom, duckTo, 1U, encoded, sizeof(encoded)) &&
              state.duckUndoState.Assign(encoded),
          "Duck relocation tail encodes");

    GameHistorySnapshotReader reader(state);
    GameHistoryCaptureSnapshot snapshot;
    Check(reader.Build(snapshot) && snapshot.variant == Variant::Duck &&
              snapshot.basePosition.bytes[42U] == 27U,
          "Duck base position uses the first retained origin");
    const GameHistoryCaptureStep step = Read(snapshot, 0U);
    Check(step.transition.duckFrom == 27U &&
              step.transition.duckTo == 35U &&
              step.afterPosition.bytes[42U] == 35U,
          "Duck move retains its mandatory relocation");
}

void TestCorruptOrDivergentTailFailsClosed() {
    GamePersistenceState state;
    Check(state.undoBaseFen.Assign(
              "8/8/8/8/8/8/4P3/4K2k w - - 0 1") &&
              state.fen.Assign(
                  "8/8/8/8/4P3/8/8/4K2k b - e3 0 1"),
          "corrupt fixture FENs fit");
    SetVariant(state, GameVariant::Classic);
    SetHistory(state, 1U, 2U);
    AddMove(state, 1U, 'P', '\0', INVALID_SQUARE, '\0', 52U, 36U);
    GameHistorySnapshotReader reader(state);
    GameHistoryCaptureSnapshot snapshot;
    Check(!reader.Build(snapshot),
          "side-corrupt persisted history is never journaled");

    state.undoHistory[0U] = 0U;
    Check(state.fen.Assign("8/8/8/8/3PP3/8/8/4K2k b - - 0 1"),
          "divergent final FEN fits");
    GameHistorySnapshotReader divergent(state);
    Check(!divergent.Build(snapshot),
          "tail that does not reach the durable final position fails closed");
}

void TestExternalPositionStaysExplicitlyPartial() {
    GamePersistenceState state;
    Check(state.fen.Assign("8/8/8/8/8/8/8/4K2k w - - 0 1"),
          "external-position fixture FEN fits");
    state.undoBaseFen = state.fen;
    SetVariant(state, GameVariant::Classic);
    SetHistory(state, 1U, 1U);
    state.gameHistory.captureIncomplete = true;

    GameHistorySnapshotReader reader(state);
    GameHistoryCaptureSnapshot snapshot;
    Check(reader.Build(snapshot) && snapshot.basePly == 1U &&
              snapshot.transitionCount == 0U,
          "unknown external prefix cannot become a complete ply-zero archive");
}

void TestUndecidedSetupIsNotArchivedAsClassic() {
    GamePersistenceState state;
    Check(state.fen.Assign(
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") &&
              state.undoBaseFen.Assign(state.fen.CStr()),
          "undecided setup FEN fits");
    SetVariant(state, GameVariant::Undecided);
    SetHistory(state, 1U, 0U);

    GameHistorySnapshotReader reader(state);
    GameHistoryCaptureSnapshot snapshot;
    Check(!reader.Build(snapshot),
          "variant-neutral setup cannot permanently claim Classic history");
}


void SetDuckTail(GamePersistenceState& state, const uint8_t* from,
                 const uint8_t* to, const uint16_t count) {
    char encoded[(UNDO_HISTORY_MOVE_CAPACITY * 4U) + 1U] = {};
    Check(EncodeDuckUndoHistory(from, to, count, encoded, sizeof(encoded)) &&
              state.duckUndoState.Assign(encoded), "Duck tail encodes");
}

void TestTerminalDuckResultMatchesFinalCapture(const bool white) {
    GamePersistenceState state;
    Check(state.undoBaseFen.Assign(white ? "k6r/8/8/8/8/8/8/R6K w - - 0 1"
                                        : "r6k/8/8/8/8/8/8/K6R b - - 0 1") &&
              state.fen.Assign(white ? "R6r/8/8/8/8/8/8/7K b - - 0 1"
                                     : "7k/8/8/8/8/8/8/r6R w - - 0 1"),
          "terminal Duck FENs fit");
    SetVariant(state, GameVariant::Duck, 35U,
               {VariantOutcomeReason::KingCaptured,
                white ? GameWinner::White : GameWinner::Black});
    SetHistory(state, 1U, 2U);
    state.gameHistory.shouldSeal = true;
    state.gameHistory.status = ArchiveStatus::Completed;
    state.gameHistory.outcome = Outcome::KingCaptured;
    state.gameHistory.winner = white ? Winner::White : Winner::Black;
    AddMove(state, white ? 0U : 1U, white ? 'R' : 'r', white ? 'k' : 'K',
            white ? 0U : 56U, '\0', white ? 56U : 0U, white ? 0U : 56U);
    const uint8_t noDuck[] = {INVALID_SQUARE};
    SetDuckTail(state, noDuck, noDuck, 1U);
    GameHistoryCaptureSnapshot snapshot;
    GameHistorySnapshotReader reader(state);
    Check(reader.Build(snapshot), "final king capture with matching winner is accepted");
    SetVariant(state, GameVariant::Duck, 35U);
    Check(!reader.Build(snapshot), "archive capture result agrees with durable variant outcome");
    SetVariant(state, GameVariant::Duck, 35U,
               {VariantOutcomeReason::KingCaptured,
                white ? GameWinner::White : GameWinner::Black});
    state.gameHistory.winner = white ? Winner::Black : Winner::White;
    Check(!reader.Build(snapshot), "king capture cannot award the other player");
    state.gameHistory.winner = white ? Winner::White : Winner::Black;
    state.gameHistory.outcome = Outcome::Timeout;
    Check(!reader.Build(snapshot), "king capture cannot be mislabeled as a timeout");
    state.gameHistory.shouldSeal = false;
    state.gameHistory.status = ArchiveStatus::Unfinished;
    state.gameHistory.outcome = Outcome::Ongoing;
    state.gameHistory.winner = Winner::None;
    Check(!reader.Build(snapshot), "king capture cannot remain an ongoing archive");
    state.gameHistory.shouldSeal = true;
    state.gameHistory.status = ArchiveStatus::Completed;
    state.gameHistory.outcome = Outcome::KingCaptured;
    state.gameHistory.winner = white ? Winner::White : Winner::Black;
    // A plausible retained move by a remaining enemy rook is still impossible
    // after the captured king ended the game.
    AddMove(state, white ? 1U : 0U, white ? 'r' : 'R', '\0', INVALID_SQUARE,
            '\0', white ? 7U : 63U, white ? 15U : 55U);
    state.gameHistory.currentPly = 2U;
    state.gameHistory.lineRevision = 3U;
    const uint8_t duckFrom[] = {INVALID_SQUARE, 35U};
    const uint8_t duckTo[] = {INVALID_SQUARE, 34U};
    SetDuckTail(state, duckFrom, duckTo, 2U);
    SetVariant(state, GameVariant::Duck, 34U,
               {VariantOutcomeReason::KingCaptured,
                white ? GameWinner::White : GameWinner::Black});
    Check(state.fen.Assign(white ? "R7/7r/8/8/8/8/8/7K w - - 0 1"
                                 : "7k/8/8/8/8/8/7R/r7 b - - 0 1"),
          "continued terminal position fits");
    AddMove(state, white ? 0U : 1U, white ? 'R' : 'r', '\0', INVALID_SQUARE,
            '\0', white ? 0U : 56U, white ? 8U : 48U);
    state.gameHistory.currentPly = 3U;
    state.gameHistory.lineRevision = 4U;
    const uint8_t roundTripFrom[] = {INVALID_SQUARE, 35U, 34U};
    const uint8_t roundTripTo[] = {INVALID_SQUARE, 34U, 35U};
    SetDuckTail(state, roundTripFrom, roundTripTo, 3U);
    SetVariant(state, GameVariant::Duck, 35U,
               {VariantOutcomeReason::KingCaptured,
                white ? GameWinner::White : GameWinner::Black});
    Check(state.fen.Assign(white ? "8/R6r/8/8/8/8/8/7K b - - 0 1"
                                 : "7k/8/8/8/8/8/r6R/8 w - - 0 1"),
          "continued terminal Duck round trip fits");
    Check(!reader.Build(snapshot), "no retained move may follow a king capture");
}

void TestArchiveEligibilityAndFalseTerminal() {
    GamePersistenceState state;
    Check(state.fen.Assign("4k3/8/8/8/8/8/8/4K3 w - - 0 1"), "eligibility FEN fits");
    state.undoBaseFen = state.fen;
    SetVariant(state, GameVariant::Duck, 35U);
    SetHistory(state, 0U, 1U);
    GameHistorySnapshotReader reader(state);
    GameHistoryCaptureSnapshot snapshot;
    Check(reader.Build(snapshot), "eligible nonterminal base is accepted");
    state.gameHistory.valid = false;
    Check(!reader.Build(snapshot), "quarantined history cannot enter capture");
    state.gameHistory.valid = true;
    state.gameHistory.captureIncomplete = true;
    state.gameHistory.lineRevision = MAX_LINE_REVISION;
    Check(!reader.Build(snapshot), "exhausted history cannot enter capture");
    state.gameHistory.captureIncomplete = false;
    state.gameHistory.lineRevision = 1U;
    state.gameHistory.shouldSeal = true;
    state.gameHistory.status = ArchiveStatus::Completed;
    state.gameHistory.outcome = Outcome::KingCaptured;
    state.gameHistory.winner = Winner::White;
    Check(!reader.Build(snapshot), "king-capture result requires a final captured-king step");
}


void TestEnPassantProvenance(const bool white) {
    const char* whiteBases[] = {
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 10",
        "4k3/8/8/3pP3/8/8/8/4K3 w - - 0 10",
        "4k3/8/8/3qP3/8/8/8/4K3 w - d6 0 10",
        "4k3/8/8/2p1P3/8/8/8/4K3 w - d6 0 10",
        "4k3/8/8/3Pp3/8/8/8/4K3 w - d6 0 10",
        "4k3/8/8/3pP3/8/8/8/4K3 w - e6 0 10"};
    const char* blackBases[] = {
        "4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 10",
        "4k3/8/8/8/3Pp3/8/8/4K3 b - - 0 10",
        "4k3/8/8/8/3Qp3/8/8/4K3 b - d3 0 10",
        "4k3/8/8/8/2P1p3/8/8/4K3 b - d3 0 10",
        "4k3/8/8/8/3pP3/8/8/4K3 b - d3 0 10",
        "4k3/8/8/8/3Pp3/8/8/4K3 b - e3 0 10"};
    for (uint8_t scenario = 0U; scenario < 6U; ++scenario) {
        GamePersistenceState state;
        Check(state.undoBaseFen.Assign(white ? whiteBases[scenario] : blackBases[scenario]) &&
              state.fen.Assign(white ? "4k3/8/3P4/8/8/8/8/4K3 b - - 0 1"
                                     : "4k3/8/8/8/8/3p4/8/4K3 w - - 0 1"),
              "EP fixture FENs fit");
        SetVariant(state, GameVariant::Classic);
        SetHistory(state, 1U, 2U);
        const uint8_t from = scenario == 4U ? (white ? 27U : 35U) : (white ? 28U : 36U);
        const uint8_t captured = scenario == 3U ? (white ? 26U : 34U)
            : scenario == 4U ? (white ? 28U : 36U) : (white ? 27U : 35U);
        AddMove(state, white ? 0U : 1U, white ? 'P' : 'p',
                scenario == 2U ? (white ? 'q' : 'Q') : (white ? 'p' : 'P'),
                captured, '\0', from, white ? 19U : 43U);
        GameHistorySnapshotReader reader(state);
        GameHistoryCaptureSnapshot snapshot;
        Check(reader.Build(snapshot) == (scenario == 0U),
              "only matching pawn/target/diagonal/adjacent EP provenance is accepted");
    }
}

} // namespace

int main() {
    TestEnPassantProvenance(true);
    TestEnPassantProvenance(false);
    TestTerminalDuckResultMatchesFinalCapture(true);
    TestTerminalDuckResultMatchesFinalCapture(false);
    TestArchiveEligibilityAndFalseTerminal();
    TestClassicReplayProducesProtocolPositions();
    TestCastlingAndSealMetadata();
    TestDuckReplayIncludesMandatoryRelocation();
    TestCorruptOrDivergentTailFailsClosed();
    TestExternalPositionStaysExplicitlyPartial();
    TestUndecidedSetupIsNotArchivedAsClassic();
    if (g_failures == 0) {
        std::puts("[PASS] game-history durable snapshot reader tests");
    }
    return g_failures == 0 ? 0 : 1;
}
