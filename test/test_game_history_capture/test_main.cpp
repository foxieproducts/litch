#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "modules/game_history_capture.hpp"
#include "game_history_memory_storage.hpp"

namespace {

using namespace modules::game_history;

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) return;
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

using game_history_test::FakeStorage;

GameId Id(const uint8_t value) {
    GameId id;
    id.bytes[0U] = value;
    id.bytes[15U] = 0xA5U;
    return id;
}

Position PositionFromFen(const char* const fen) {
    Position position;
    Check(EncodePosition(fen, Variant::Classic, INVALID_SQUARE, position),
          "capture fixture position encodes");
    return position;
}

GameHistoryCaptureStep Step(const char* const fen,
                            const Side side,
                            const uint8_t from,
                            const uint8_t to) {
    GameHistoryCaptureStep step;
    step.afterPosition = PositionFromFen(fen);
    step.transition.side = side;
    step.transition.from = from;
    step.transition.to = to;
    return step;
}

struct Steps {
    std::vector<GameHistoryCaptureStep> values;

    static bool Read(void* const context,
                     const uint16_t index,
                     GameHistoryCaptureStep& out) {
        const auto& self = *static_cast<Steps*>(context);
        if (index >= self.values.size()) return false;
        out = self.values[index];
        return true;
    }
};

GameHistoryCaptureSnapshot Snapshot(
    const GameId gameId,
    const uint64_t revision,
    const uint16_t basePly,
    const Position& base,
    Steps& steps) {
    GameHistoryCaptureSnapshot snapshot;
    snapshot.gameId = gameId;
    snapshot.lineRevision = revision;
    snapshot.variant = Variant::Classic;
    snapshot.basePly = basePly;
    snapshot.basePosition = base;
    snapshot.transitionCount =
        static_cast<uint16_t>(steps.values.size());
    snapshot.stepContext = &steps;
    snapshot.readStep = &Steps::Read;
    return snapshot;
}

void TestDurableSnapshotReconciliationIsIdempotent() {
    FakeStorage storage;
    auto journal = storage.Journal();
    Check(journal.Open(0x0102030405060708ULL) ==
              JournalOpenResult::Initialized,
          "capture journal initializes");
    GameHistoryCapture capture(journal);
    Steps steps{{
        Step("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/"
             "RNBQKBNR b KQkq e3 0 1",
             Side::White, 52U, 36U),
        Step("rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/"
             "RNBQKBNR w KQkq e6 0 2",
             Side::Black, 12U, 28U),
    }};
    auto snapshot = Snapshot(
        Id(1U), 3U, 0U,
        PositionFromFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/"
                        "RNBQKBNR w KQkq - 0 1"),
        steps);
    snapshot.shouldSeal = true;
    snapshot.status = ArchiveStatus::Completed;
    snapshot.outcome = Outcome::Draw;
    snapshot.winner = Winner::Draw;

    Check(capture.Reconcile(snapshot) == GameHistoryCaptureResult::Applied &&
              journal.Summary().newestAllocated == 4U,
          "snapshot appends base, every missing ply, and terminal seal");
    Check(capture.Reconcile(snapshot) == GameHistoryCaptureResult::NoChange &&
              journal.Summary().newestAllocated == 4U,
          "retry after durable append is exactly idempotent");
    Record latest;
    Check(journal.ReadLatestRecord(latest) == JournalSegmentRead::Ready &&
              latest.type == RecordType::Seal &&
              latest.outboxSequence == 4U,
          "journal exposes only its newest validated record");
    Check(journal.AdvanceDurableAck(4U) == JournalMutationResult::Applied &&
              journal.ReclaimAcknowledged() == JournalMutationResult::Applied &&
              capture.Reconcile(snapshot) == GameHistoryCaptureResult::NoChange &&
              journal.Summary().newestAllocated == 4U,
          "reclaim retains capture anchor and never republishes unchanged completed game");
    auto undo = snapshot;
    undo.lineRevision = 4U;
    undo.transitionCount = 1U;
    undo.shouldSeal = false;
    Check(capture.Reconcile(undo) == GameHistoryCaptureResult::Applied &&
              journal.ReadLatestRecord(latest) == JournalSegmentRead::Ready &&
              latest.type == RecordType::CommittedPly && latest.ply == 1U &&
              latest.lineRevision == 4U,
          "newer Undo after terminal Seal resumes same-game authoritative capture");
}

void TestMissedDuckSealRecoversBeforeNextGame() {
    for (const bool black : {false, true}) {
        const char* beforeFen = black ? "4k2r/8/8/8/8/8/8/7K b - - 0 1"
                                      : "7k/8/8/8/8/8/8/4K2R w - - 0 1";
        const char* afterFen = black ? "4k3/8/8/8/8/8/8/7r w - - 0 2"
                                     : "7R/8/8/8/8/8/8/4K3 b - - 0 1";
        const Winner winner = black ? Winner::Black : Winner::White;
        FakeStorage storage;
        auto journal = storage.Journal();
        Check(journal.Open(0x3434U) == JournalOpenResult::Initialized,
              "missed Duck result fixture initializes");
        GameHistoryCapture capture(journal);
        Position before;
        Position after;
        Check(EncodePosition(beforeFen, Variant::Duck, 34U, before) &&
                  EncodePosition(afterFen, Variant::Duck, 34U, after),
              "Duck terminal before/after positions encode");
        GameHistoryCaptureStep terminal;
        terminal.afterPosition = after;
        terminal.transition.side = black ? Side::Black : Side::White;
        terminal.transition.from = black ? 7U : 63U;
        terminal.transition.to = black ? 63U : 7U;
        terminal.transition.captureSquare = terminal.transition.to;
        Steps steps{{terminal}};
        auto snapshot = Snapshot(Id(7U), 2U, 0U, before, steps);
        snapshot.variant = Variant::Duck;
        snapshot.shouldSeal = true;
        snapshot.status = ArchiveStatus::Completed;
        snapshot.outcome = Outcome::KingCaptured;
        snapshot.winner = winner;
        storage.failJournalAppend = 3U;
        Check(capture.Reconcile(snapshot) == GameHistoryCaptureResult::IoFailure &&
                  journal.Open(0x3434U) == JournalOpenResult::Ready,
              "terminal move is durable but Seal was interrupted");
        Steps empty;
        auto next = Snapshot(Id(8U), 1U, 0U, PositionFromFen(
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"), empty);
        for (const bool wrongCapture : {false, true}) {
            FakeStorage corrupt = storage;
            auto& damaged = corrupt.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes;
            Record changed;
            size_t encodedLength = 0U;
            Check(DecodeRecord(damaged.data() + 81U, 84U, changed),
                  "contradictory durable transition control decodes");
            if (wrongCapture) changed.transition.captureSquare = 8U;
            else changed.transition.from = black ? 4U : 60U;
            Check(EncodeRecord(changed, damaged.data() + 81U, 84U, encodedLength) &&
                      encodedLength == 84U, "contradiction retains canonical record CRC");
            const auto preserved = damaged;
            auto badJournal = corrupt.Journal();
            GameHistoryCapture badCapture(badJournal);
            Check(badJournal.Open(0x3434U) == JournalOpenResult::Ready &&
                      badCapture.Reconcile(next) == GameHistoryCaptureResult::Corrupt &&
                      damaged == preserved,
                  "contradictory terminal capture is preserved without invented outcome");
        }
        Check(capture.Reconcile(next) == GameHistoryCaptureResult::Applied &&
                  journal.Summary().newestAllocated == 4U,
              "next game repairs prior terminal result before its own base");
        const auto& bytes = storage.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes;
        Record seal;
        Check(bytes.size() >= 203U && DecodeRecord(bytes.data() + 165U, 38U, seal) &&
                  seal.type == RecordType::Seal && seal.status == ArchiveStatus::Completed &&
                  seal.outcome == Outcome::KingCaptured && seal.winner == winner,
              "durable king-capture evidence never receives an Unfinished result");
        Check(journal.AdvanceDurableAck(3U) == JournalMutationResult::Applied &&
                  journal.ReclaimAcknowledged() == JournalMutationResult::Applied &&
                  journal.Summary().oldestRetained == 4U,
              "successor base releases previous completed capture anchor");
    }
}

void TestTerminalBaseCannotInventMissingCapture() {
    FakeStorage storage;
    auto journal = storage.Journal();
    Check(journal.Open(0x3535U) == JournalOpenResult::Initialized,
          "ambiguous terminal base fixture initializes");
    Position terminal;
    Check(EncodePosition("7R/8/8/8/8/8/8/4K3 b - - 0 1", Variant::Duck, 34U, terminal),
          "terminal base encodes structurally");
    Steps empty;
    auto snapshot = Snapshot(Id(9U), 2U, 1U, terminal, empty);
    snapshot.variant = Variant::Duck;
    snapshot.shouldSeal = true;
    snapshot.status = ArchiveStatus::Completed;
    snapshot.outcome = Outcome::KingCaptured;
    snapshot.winner = Winner::White;
    GameHistoryCapture capture(journal);
    Check(capture.Reconcile(snapshot) == GameHistoryCaptureResult::Bounds &&
              journal.Summary().newestAllocated == 0U,
          "terminal base without final transition is rejected before capture writes");
    Record base;
    base.type = RecordType::LineBase;
    base.outboxSequence = 1U;
    base.gameId = Id(9U);
    base.lineRevision = 2U;
    base.ply = 1U;
    base.variant = Variant::Duck;
    base.position = terminal;
    Check(journal.Append(base) == JournalMutationResult::Applied,
          "legacy ambiguous terminal base prepares recovery control");
    auto next = Snapshot(Id(10U), 1U, 0U, PositionFromFen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"), empty);
    const auto saved = storage.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes;
    Check(capture.Reconcile(next) == GameHistoryCaptureResult::Corrupt &&
              storage.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes == saved,
          "ambiguous old terminal tail is preserved without a fabricated result");
}

void TestTransposedUndoLineRemainsAuthoritative() {
    FakeStorage storage;
    auto journal = storage.Journal();
    Check(journal.Open(0x3131U) == JournalOpenResult::Initialized,
          "transposition capture fixture initializes");
    GameHistoryCapture capture(journal);
    const auto initial = PositionFromFen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    Steps original{{
        Step("rnbqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQKB1R b KQkq - 1 1", Side::White, 62U, 45U),
        Step("rnbqkb1r/pppppppp/5n2/8/8/5N2/PPPPPPPP/RNBQKB1R w KQkq - 2 2", Side::Black, 6U, 21U),
        Step("rnbqkb1r/pppppppp/5n2/8/8/2N2N2/PPPPPPPP/R1BQKB1R b KQkq - 3 2", Side::White, 57U, 42U),
        Step("r1bqkb1r/pppppppp/2n2n2/8/8/2N2N2/PPPPPPPP/R1BQKB1R w KQkq - 4 3", Side::Black, 1U, 18U),
    }};
    auto first = Snapshot(Id(5U), 5U, 0U, initial, original);
    Check(capture.Reconcile(first) == GameHistoryCaptureResult::Applied,
          "original knight move order captured");
    Steps replacement{{
        Step("rnbqkbnr/pppppppp/8/8/8/2N5/PPPPPPPP/R1BQKBNR b KQkq - 1 1", Side::White, 57U, 42U),
        Step("r1bqkbnr/pppppppp/2n5/8/8/2N5/PPPPPPPP/R1BQKBNR w KQkq - 2 2", Side::Black, 1U, 18U),
        Step("r1bqkbnr/pppppppp/2n5/8/8/2N2N2/PPPPPPPP/R1BQKB1R b KQkq - 3 2", Side::White, 62U, 45U),
        original.values.back(),
    }};
    replacement.values.back().transition.from = 6U;
    replacement.values.back().transition.to = 21U;
    auto branch = Snapshot(Id(5U), 13U, 0U, initial, replacement);
    Check(capture.Reconcile(branch) == GameHistoryCaptureResult::Applied &&
              journal.Summary().newestAllocated == 10U,
          "Undo and reordered moves replace history despite identical final position");
    Record latest;
    Check(journal.ReadLatestRecord(latest) == JournalSegmentRead::Ready &&
              latest.lineRevision == 13U && latest.transition.from == 6U,
          "new revision and actual final move remain authoritative");
}

void TestInterruptedCaptureReconcilesWithoutDuplicates() {
    for (const unsigned failedAppend : {3U, 4U}) {
        for (const bool written : {false, true}) {
            FakeStorage storage;
            auto journal = storage.Journal();
            Check(journal.Open(0x3232U) == JournalOpenResult::Initialized,
                  "interrupted capture initializes");
            GameHistoryCapture capture(journal);
            Steps steps{{
                Step("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1", Side::White, 52U, 36U),
                Step("rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2", Side::Black, 12U, 28U),
            }};
            auto snapshot = Snapshot(Id(6U), 3U, 0U, PositionFromFen(
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"), steps);
            snapshot.shouldSeal = true;
            snapshot.status = ArchiveStatus::Completed;
            snapshot.outcome = Outcome::Draw;
            snapshot.winner = Winner::Draw;
            storage.failJournalAppend = failedAppend;
            storage.writeBeforeFailure = written;
            Check(capture.Reconcile(snapshot) == GameHistoryCaptureResult::IoFailure &&
                      journal.Open(0x3232U) == JournalOpenResult::Ready,
                  "failed ply or Seal append requires reopen before capture retry");
            const auto recovered = capture.Reconcile(snapshot);
            Check((recovered == GameHistoryCaptureResult::Applied ||
                   recovered == GameHistoryCaptureResult::NoChange) &&
                      journal.Summary().newestAllocated == 4U &&
                      capture.Reconcile(snapshot) == GameHistoryCaptureResult::NoChange,
                  "reopened capture repairs missing suffix or Seal exactly once");
        }
    }
}

void TestUndoDivergenceStartsAnAuthoritativeSuffix() {
    FakeStorage storage;
    auto journal = storage.Journal();
    Check(journal.Open(0x1112131415161718ULL) ==
              JournalOpenResult::Initialized,
          "branch journal initializes");
    GameHistoryCapture capture(journal);
    const Position initial = PositionFromFen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const Position afterE4 = PositionFromFen(
        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    Steps original{{
        Step("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/"
             "RNBQKBNR b KQkq e3 0 1",
             Side::White, 52U, 36U),
        Step("rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/"
             "RNBQKBNR w KQkq e6 0 2",
             Side::Black, 12U, 28U),
    }};
    auto first = Snapshot(Id(2U), 3U, 0U, initial, original);
    Check(capture.Reconcile(first) == GameHistoryCaptureResult::Applied,
          "original line is captured");

    Steps replacement{{
        Step("rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/"
             "RNBQKBNR w KQkq c6 0 2",
             Side::Black, 10U, 26U),
    }};
    auto branch = Snapshot(Id(2U), 5U, 1U, afterE4, replacement);
    Check(capture.Reconcile(branch) == GameHistoryCaptureResult::Applied &&
              journal.Summary().newestAllocated == 5U,
          "undo divergence appends a line base and replacement suffix");
    Record latest;
    Check(journal.ReadLatestRecord(latest) == JournalSegmentRead::Ready &&
              latest.type == RecordType::CommittedPly &&
              latest.ply == 2U && latest.lineRevision == 5U,
          "replacement suffix becomes the newest authority");
}

void TestNewIdentitySealsPriorIncompleteGame() {
    FakeStorage storage;
    auto journal = storage.Journal();
    Check(journal.Open(0x2122232425262728ULL) ==
              JournalOpenResult::Initialized,
          "new-game journal initializes");
    GameHistoryCapture capture(journal);
    const Position initial = PositionFromFen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    Steps firstSteps;
    auto first = Snapshot(Id(3U), 1U, 0U, initial, firstSteps);
    Check(capture.Reconcile(first) == GameHistoryCaptureResult::Applied,
          "first active game base is captured");

    Steps nextSteps;
    auto next = Snapshot(Id(4U), 1U, 0U, initial, nextSteps);
    Check(capture.Reconcile(next) == GameHistoryCaptureResult::Applied &&
              journal.Summary().newestAllocated == 3U,
          "new identity truthfully seals then starts the next game");
    JournalSegmentPlan firstPlan;
    Check(journal.PlanSegmentAfter(0U, firstPlan) ==
              JournalSegmentRead::Ready &&
              firstPlan.descriptor.recordCount == 2U,
          "unfinished seal stays grouped with the prior game");
    JournalSegmentPlan nextPlan;
    Check(journal.PlanSegmentAfter(2U, nextPlan) ==
              JournalSegmentRead::Ready &&
              nextPlan.descriptor.firstOutboxSequence == 3U,
          "following transaction begins the new game");
}

// Structural lookup controls complement the physical rolling-tail journey.
void TestLookupUsesOnlyLatestRetainedLine() {
    FakeStorage storage;
    auto journal = storage.Journal();
    Check(journal.Open(0x3737U) == JournalOpenResult::Initialized, "lookup journal opens");
    auto appendBase = [&](uint16_t ply, uint64_t revision, const char* fen) {
        Record base;
        base.outboxSequence = journal.Summary().newestAllocated + 1U;
        base.gameId = Id(12U);
        base.lineRevision = revision;
        base.ply = ply;
        base.position = PositionFromFen(fen);
        Check(journal.Append(base) == JournalMutationResult::Applied, "lookup base appends");
    };
    constexpr auto INITIAL = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 8 5";
    appendBase(8U, 9U, INITIAL);
    Record found;
    Check(journal.ReadCurrentLinePosition(Id(12U),8U,10U,found)==JournalSegmentRead::Ready &&
              found.position.bytes[34U]==8U, "lookup recovers exact retained counters");
    Check(journal.AdvanceDurableAck(1U)==JournalMutationResult::Applied &&
              journal.ReclaimAcknowledged()==JournalMutationResult::Applied &&
              journal.ReadCurrentLinePosition(Id(12U),8U,10U,found)==JournalSegmentRead::Ready,
          "acknowledged retained anchor remains available after compaction attempt");
    appendBase(4U,10U,"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 4 3");
    Check(journal.ReadCurrentLinePosition(Id(12U),8U,11U,found)==JournalSegmentRead::Unavailable,
          "new incomplete line invalidates an older matching ply");
    appendBase(4U,11U,"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 40 30");
    Check(journal.ReadCurrentLinePosition(Id(12U),4U,12U,found)==JournalSegmentRead::Ready &&
              found.position.bytes[34U]==40U && found.position.bytes[38U]==30U,
          "same board and ply use latest branch counters only");
    Check(journal.ReadCurrentLinePosition(Id(12U),4U,11U,found)==JournalSegmentRead::Unavailable,
          "lookup never borrows from an equal or newer revision");
    Record seal;
    seal.type=RecordType::Seal;
    seal.gameId=Id(12U);
    seal.outboxSequence=journal.Summary().newestAllocated+1U;
    seal.lineRevision=12U;
    Check(journal.Append(seal)==JournalMutationResult::Applied &&
              journal.ReadCurrentLinePosition(Id(12U),4U,12U,found)==JournalSegmentRead::Unavailable &&
              journal.ReadCurrentLinePosition(Id(12U),4U,13U,found)==JournalSegmentRead::Ready,
          "revision fence includes the latest Seal, not just the donor record");
    appendBase(4U,12U,INITIAL);
    Check(journal.ReadCurrentLinePosition(Id(12U),4U,13U,found)==JournalSegmentRead::Corrupt,
          "nonadvancing revision barrier fails closed");
}

void TestRebaseCounterRecoverySurvivesInterruptedWrites() {
    for (const unsigned failedAppend : {6U,7U,8U}) {
        for (const bool written : {false,true}) {
            FakeStorage storage;
            auto journal = storage.Journal();
            Check(journal.Open(0x3838U)==JournalOpenResult::Initialized,"rebase recovery opens");
            GameHistoryCapture capture(journal);
            Steps steps{{
                Step("rnbqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQKB1R b KQkq - 1 1",Side::White,62U,45U),
                Step("rnbqkb1r/pppppppp/5n2/8/8/5N2/PPPPPPPP/RNBQKB1R w KQkq - 2 2",Side::Black,6U,21U),
                Step("rnbqkb1r/pppppppp/5n2/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 3 2",Side::White,45U,62U),
                Step("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 4 3",Side::Black,21U,6U),
            }};
            auto snapshot=Snapshot(Id(13U),13U,8U,PositionFromFen(
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 8 5"),steps);
            Check(capture.Reconcile(snapshot)==GameHistoryCaptureResult::Applied,"original retained line captures");
            snapshot.lineRevision=14U;
            snapshot.transitionCount=3U;
            snapshot.basePosition=PositionFromFen(
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
            if (failedAppend==6U && !written) {
                // One bounded latest decode precedes the anchor lookup.
                // Fail its first read after that successful latest read.
                storage.failJournalRead=storage.journalReads+2U;
                Check(capture.Reconcile(snapshot)==GameHistoryCaptureResult::IoFailure &&
                          journal.Summary().newestAllocated==5U && storage.journalAppends==5U,
                      "anchor read failure cannot append a normalized replacement");
                storage.failJournalRead=0U;
            }
            storage.failJournalAppend=failedAppend;
            storage.writeBeforeFailure=written;
            Check(capture.Reconcile(snapshot)==GameHistoryCaptureResult::IoFailure &&
                      journal.Open(0x3838U)==JournalOpenResult::Ready,
                  "interrupted rebase base or suffix reopens");
            Check(capture.Reconcile(snapshot)==GameHistoryCaptureResult::Applied,
                  "reopened rebase finishes missing suffix");
            Record latest;
            Check(journal.ReadLatestRecord(latest)==JournalSegmentRead::Ready &&
                      latest.ply==11U && latest.position.bytes[34U]==11U &&
                      latest.position.bytes[38U]==6U && journal.Summary().newestAllocated==9U,
                  "retry preserves original counters with exactly one revised base and suffix");
            Check(capture.Reconcile(snapshot)==GameHistoryCaptureResult::NoChange,
                  "completed revised line is idempotent");
        }
    }
}

void TestInvalidSnapshotMutatesNothing() {
    FakeStorage storage;
    auto journal = storage.Journal();
    Check(journal.Open(0x3132333435363738ULL) ==
              JournalOpenResult::Initialized,
          "invalid-snapshot journal initializes");
    GameHistoryCapture capture(journal);
    Steps steps;
    auto invalid = Snapshot(
        Id(5U), MAX_LINE_REVISION + 1U, 0U,
        PositionFromFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/"
                        "RNBQKBNR w KQkq - 0 1"),
        steps);
    Check(capture.Reconcile(invalid) == GameHistoryCaptureResult::Bounds &&
              journal.Summary().newestAllocated == 0U,
          "out-of-range revision is rejected before journal mutation");
}

} // namespace

int main() {
    TestLookupUsesOnlyLatestRetainedLine();
    TestRebaseCounterRecoverySurvivesInterruptedWrites();
    TestMissedDuckSealRecoversBeforeNextGame();
    TestTerminalBaseCannotInventMissingCapture();
    TestTransposedUndoLineRemainsAuthoritative();
    TestInterruptedCaptureReconcilesWithoutDuplicates();
    TestDurableSnapshotReconciliationIsIdempotent();
    TestUndoDivergenceStartsAnAuthoritativeSuffix();
    TestNewIdentitySealsPriorIncompleteGame();
    TestInvalidSnapshotMutatesNothing();
    if (g_failures == 0) {
        std::printf("[PASS] game-history capture reconciliation tests\n");
    }
    return g_failures == 0 ? 0 : 1;
}
