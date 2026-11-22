#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "modules/game_history_journal.hpp"

namespace {

using namespace modules::game_history;

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) return;
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

struct Artifact {
    bool present = false;
    std::vector<uint8_t> bytes;
};

struct FakeStorage {
    std::array<Artifact, 4U> artifacts{};
    JournalArtifact failNextReplace = static_cast<JournalArtifact>(0xFFU);
    bool failNextAppend = false;
    unsigned journalReads = 0U;
    size_t failAfterAppendBytes = 0U;
    bool failNextPromote = false;
    bool alterCompactionRecord = false;
    JournalArtifact failNextRead = static_cast<JournalArtifact>(0xFFU);

    static size_t Index(const JournalArtifact artifact) {
        return static_cast<size_t>(artifact);
    }

    static JournalStorageStatus Stat(void* const context,
                                     const JournalArtifact artifact,
                                     uint32_t& outSize) {
        auto& self = *static_cast<FakeStorage*>(context);
        const Artifact& value = self.artifacts[Index(artifact)];
        if (!value.present) {
            outSize = 0U;
            return JournalStorageStatus::Missing;
        }
        outSize = static_cast<uint32_t>(value.bytes.size());
        return JournalStorageStatus::Present;
    }

    static bool Read(void* const context,
                     const JournalArtifact artifact,
                     const uint32_t offset,
                     uint8_t* const out,
                     const size_t length) {
        auto& self = *static_cast<FakeStorage*>(context);
        if (artifact == JournalArtifact::Journal) ++self.journalReads;
        if (self.failNextRead == artifact) {
            self.failNextRead = static_cast<JournalArtifact>(0xFFU);
            return false;
        }
        const Artifact& value = self.artifacts[Index(artifact)];
        if (!value.present || out == nullptr ||
            offset > value.bytes.size() ||
            length > value.bytes.size() - offset) {
            return false;
        }
        std::memcpy(out, value.bytes.data() + offset, length);
        return true;
    }

    static bool Replace(void* const context,
                        const JournalArtifact artifact,
                        const uint8_t* const bytes,
                        const size_t length) {
        auto& self = *static_cast<FakeStorage*>(context);
        if (self.failNextReplace == artifact) {
            self.failNextReplace = static_cast<JournalArtifact>(0xFFU);
            return false;
        }
        if (bytes == nullptr && length != 0U) return false;
        Artifact& value = self.artifacts[Index(artifact)];
        value.present = true;
        if (length == 0U) {
            value.bytes.clear();
        } else {
            value.bytes.assign(bytes, bytes + length);
        }
        return true;
    }

    static bool Append(void* const context,
                       const JournalArtifact artifact,
                       const uint8_t* const bytes,
                       const size_t length) {
        auto& self = *static_cast<FakeStorage*>(context);
        if (self.failNextAppend || bytes == nullptr || length == 0U) {
            self.failNextAppend = false;
            return false;
        }
        Artifact& value = self.artifacts[Index(artifact)];
        value.present = true;
        if (self.failAfterAppendBytes != 0U) {
            const size_t written = self.failAfterAppendBytes < length
                                       ? self.failAfterAppendBytes : length;
            self.failAfterAppendBytes = 0U;
            value.bytes.insert(value.bytes.end(), bytes, bytes + written);
            return false;
        }
        value.bytes.insert(value.bytes.end(), bytes, bytes + length);
        if (artifact == JournalArtifact::Compaction && self.alterCompactionRecord) {
            self.alterCompactionRecord = false;
            Record changed;
            size_t encodedLength = 0U;
            Check(DecodeRecord(value.bytes.data(), value.bytes.size(), changed),
                  "compaction corruption fixture decodes complete record");
            ++changed.lineRevision;
            Check(EncodeRecord(changed, value.bytes.data(), value.bytes.size(), encodedLength) &&
                      encodedLength == value.bytes.size(),
                  "compaction corruption retains valid record CRC and length");
        }
        return true;
    }

    static bool Promote(void* const context,
                        const JournalArtifact from,
                        const JournalArtifact to) {
        auto& self = *static_cast<FakeStorage*>(context);
        if (self.failNextPromote) {
            self.failNextPromote = false;
            return false;
        }
        Artifact& source = self.artifacts[Index(from)];
        if (!source.present) return false;
        self.artifacts[Index(to)] = source;
        source = {};
        return true;
    }

    GameHistoryJournal Journal() {
        return GameHistoryJournal(
            {this, &Stat, &Read, &Replace, &Append, &Promote});
    }

    void AppendRaw(const Record& record) {
        std::array<uint8_t, MAX_RECORD_BYTES> encoded{};
        size_t length = 0U;
        Check(EncodeRecord(record, encoded.data(), encoded.size(), length),
              "raw fixture record encodes");
        Artifact& journal = artifacts[Index(JournalArtifact::Journal)];
        journal.present = true;
        journal.bytes.insert(journal.bytes.end(), encoded.begin(),
                             encoded.begin() + length);
    }
};

GameId Id(const uint32_t value) {
    GameId id;
    id.bytes[0U] = static_cast<uint8_t>(value & 0xFFU);
    id.bytes[1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    id.bytes[2U] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    id.bytes[3U] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
    id.bytes[15U] = 0xA5U;
    return id;
}

Position InitialPosition() {
    Position position;
    Check(EncodePosition(
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/"
              "RNBQKBNR w KQkq - 0 1",
              Variant::Classic, INVALID_SQUARE, position),
          "initial position encodes");
    return position;
}

Position AfterE4Position() {
    Position position;
    Check(EncodePosition(
              "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/"
              "RNBQKBNR b KQkq e3 0 1",
              Variant::Classic, INVALID_SQUARE, position),
          "e4 position encodes");
    return position;
}

Record LineBase(const uint32_t sequence,
                const uint32_t game,
                const uint16_t ply = 0U) {
    Record record;
    record.type = RecordType::LineBase;
    record.outboxSequence = sequence;
    record.gameId = Id(game);
    record.lineRevision = 1U;
    record.ply = ply;
    record.variant = Variant::Classic;
    record.position = InitialPosition();
    return record;
}

Record Ply(const uint32_t sequence, const uint32_t game) {
    Record record;
    record.type = RecordType::CommittedPly;
    record.outboxSequence = sequence;
    record.gameId = Id(game);
    record.lineRevision = 2U;
    record.ply = 1U;
    record.position = AfterE4Position();
    record.transition.side = Side::White;
    record.transition.from = 52U;
    record.transition.to = 36U;
    return record;
}

Record Seal(const uint32_t sequence, const uint32_t game) {
    Record record;
    record.type = RecordType::Seal;
    record.outboxSequence = sequence;
    record.gameId = Id(game);
    record.lineRevision = 3U;
    record.status = ArchiveStatus::Completed;
    record.outcome = Outcome::Draw;
    record.winner = Winner::Draw;
    return record;
}

void TestSteadyStateReadsDoNotGrowWithRetainedHistory() {
    for (uint32_t retained : {1U,4096U}) {
        FakeStorage storage;
        auto journal=storage.Journal();
        Check(journal.Open(0x7171U)==JournalOpenResult::Initialized,"read budget fixture initializes");
        // Valid encoded storage corpus; gameplay correctness is established by
        // the physical rolling-tail journey, not these synthetic repeated plies.
        storage.AppendRaw(LineBase(1U,1U));
        for (uint32_t sequence=2U;sequence<=retained;++sequence) {
            Record ply=Ply(sequence,1U);
            ply.ply=static_cast<uint16_t>(sequence-1U);
            ply.lineRevision=sequence;
            storage.AppendRaw(ply);
        }
        journal.Close();
        const unsigned beforeOpen=storage.journalReads;
        Check(journal.Open(0x7171U)==JournalOpenResult::Ready,"preexisting bounded corpus fully scans");
        const unsigned openReads=storage.journalReads-beforeOpen;
        Record latest;
        const unsigned beforeLatest=storage.journalReads;
        Check(journal.ReadLatestRecord(latest)==JournalSegmentRead::Ready &&
                  latest.outboxSequence==retained,"latest retained record remains disk authority");
        const unsigned latestReads=storage.journalReads-beforeLatest;
        Record next=Ply(retained+1U,1U);
        next.ply=static_cast<uint16_t>(retained);
        next.lineRevision=retained+1U;
        const unsigned beforeAppend=storage.journalReads;
        Check(journal.Append(next)==JournalMutationResult::Applied,"verified append succeeds at corpus bound");
        const unsigned appendReads=storage.journalReads-beforeAppend;
        std::printf("[HISTORY_IO_BUDGET] retained=%u open_reads=%u latest_reads=%u append_reads=%u\n",
                    retained,openReads,latestReads,appendReads);
        Check(latestReads==1U && appendReads==1U,
              "ordinary latest read and append each read one bounded record independent of journal size");
        Check(journal.ReadLatestRecord(latest)==JournalSegmentRead::Ready &&
                  latest.outboxSequence==retained+1U && latest.ply==retained,
              "appended tail metadata names the new record");
        storage.failNextRead=JournalArtifact::Journal;
        Check(journal.ReadLatestRecord(latest)==JournalSegmentRead::IoFailure,
              "latest position is reread and never served from a stale RAM position cache");
        journal.Close();
        const unsigned beforeClosed=storage.journalReads;
        Check(journal.ReadLatestRecord(latest)==JournalSegmentRead::IoFailure &&
                  storage.journalReads==beforeClosed,"closed journal cannot use cached tail metadata");
    }
}

void TestLatestMetadataTracksCompaction() {
    FakeStorage storage;
    auto journal=storage.Journal();
    Check(journal.Open(0x7272U)==JournalOpenResult::Initialized &&
              journal.Append(LineBase(1U,1U))==JournalMutationResult::Applied &&
              journal.Append(Seal(2U,1U))==JournalMutationResult::Applied &&
              journal.Append(LineBase(3U,2U))==JournalMutationResult::Applied &&
              journal.Append(Ply(4U,2U))==JournalMutationResult::Applied,
          "two-game compaction fixture captures");
    Check(journal.AdvanceDurableAck(2U)==JournalMutationResult::Applied &&
              journal.ReclaimAcknowledged()==JournalMutationResult::Applied,
          "acknowledged preceding game compacts away");
    Record latest;
    Check(journal.ReadLatestRecord(latest)==JournalSegmentRead::Ready &&
              latest.gameId==Id(2U) && latest.outboxSequence==4U,
          "latest metadata follows shifted journal offsets");
    Check(journal.Append(Seal(5U,2U))==JournalMutationResult::Applied &&
              journal.ReadLatestRecord(latest)==JournalSegmentRead::Ready &&
              latest.type==RecordType::Seal && latest.outboxSequence==5U,
          "append after compaction extends the new layout");
    auto& bytes=storage.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes;
    const auto saved=bytes;
    bytes.back()^=1U;
    Check(journal.ReadLatestRecord(latest)==JournalSegmentRead::IoFailure,
          "cached location still validates the actual stored record CRC");
    bytes=saved;
    const Record wrong=Seal(6U,2U);
    std::array<uint8_t,MAX_RECORD_BYTES> encoded{};
    size_t length=0U;
    Check(EncodeRecord(wrong,encoded.data(),encoded.size(),length),"wrong sequence control encodes");
    std::copy(encoded.begin(),encoded.begin()+length,bytes.end()-length);
    Check(journal.ReadLatestRecord(latest)==JournalSegmentRead::Corrupt,
          "valid-CRC tail with wrong allocated sequence cannot masquerade as current");
}

void TestCleanTailLossCannotResumeAtAllocationWatermark() {
    for (bool exhausted : {false,true}) {
        FakeStorage storage;
        auto journal=storage.Journal();
        Check(journal.Open(0x7373U)==JournalOpenResult::Initialized,"truncated tail fixture opens");
        const uint32_t first=exhausted ? UINT32_MAX-1U : 1U;
        storage.AppendRaw(LineBase(first,1U));
        const size_t retainedBytes=storage.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes.size();
        storage.AppendRaw(Seal(first+1U,1U));
        journal.Close();
        Check(journal.Open(0x7373U)==JournalOpenResult::Ready,"durable allocation includes the final complete record");
        auto& bytes=storage.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes;
        bytes.resize(retainedBytes);
        const auto retained=storage.artifacts;
        journal.Close();
        Check(journal.Open(0x7373U)==JournalOpenResult::Corrupt && !journal.IsOpen(),
              "clean record-boundary tail loss cannot reopen below allocated watermark");
        Check(journal.Append(LineBase(exhausted ? 1U : 3U,2U))!=JournalMutationResult::Applied,
              "missing durable record cannot be skipped by a new append");
        for (size_t index=0U;index<retained.size();++index) {
            Check(storage.artifacts[index].bytes==retained[index].bytes &&
                      storage.artifacts[index].present==retained[index].present,
                  "truncated journal and state slots remain intact for recovery");
        }
    }
}

void TestAppendReopenAndInterruptedMetadataRepair() {
    FakeStorage fake;
    auto journal = fake.Journal();
    Check(journal.Open(0x0102030405060708ULL) ==
              JournalOpenResult::Initialized,
          "empty journal initializes with durable identity");
    Check(journal.Append(LineBase(1U, 1U)) ==
              JournalMutationResult::Applied &&
              journal.Append(Ply(2U, 1U)) ==
              JournalMutationResult::Applied,
          "records append and flush in sequence");
    Check(journal.Summary().oldestRetained == 1U &&
              journal.Summary().newestAllocated == 2U &&
              journal.Summary().journalBytes == 164U,
          "summary measures retained record prefix");

    const JournalArtifact nextSlot = JournalArtifact::StateB;
    fake.failNextReplace = nextSlot;
    Check(journal.Append(Seal(3U, 1U)) ==
              JournalMutationResult::IoFailure,
          "metadata failure does not claim append success");

    auto recovered = fake.Journal();
    Check(recovered.Open(0x9999999999999999ULL) ==
              JournalOpenResult::Ready &&
              recovered.Summary().newestAllocated == 3U &&
              recovered.Summary().journalBytes == 202U,
          "reopen repairs metadata from the durable CRC-valid append");
    Check(recovered.Append(LineBase(4U, 2U)) ==
              JournalMutationResult::Applied,
          "repaired next sequence accepts the following game");
}

void TestInvalidSuffixIsRetainedAndNeverAppendedThrough() {
    FakeStorage fake;
    auto journal = fake.Journal();
    Check(journal.Open(0x101U) == JournalOpenResult::Initialized &&
              journal.Append(LineBase(1U, 1U)) ==
                  JournalMutationResult::Applied,
          "valid prefix prepares torn-tail fixture");
    Artifact& bytes =
        fake.artifacts[FakeStorage::Index(JournalArtifact::Journal)];
    bytes.bytes.push_back(0x01U);
    bytes.bytes.push_back(0x02U);
    bytes.bytes.push_back(0x03U);

    auto reopened = fake.Journal();
    Check(reopened.Open(0x202U) == JournalOpenResult::Ready &&
              reopened.Summary().hasInvalidSuffix &&
              reopened.Summary().validPrefixBytes == 81U &&
              reopened.Summary().journalBytes == 84U,
          "torn suffix remains diagnostic while prefix stays readable");
    Check(reopened.Append(Ply(2U, 1U)) ==
              JournalMutationResult::Corrupt &&
              bytes.bytes.size() == 84U,
          "journal never appends through a corrupt suffix");
}

void TestUncertainAppendRequiresReopen() {
    for (const size_t written : std::array<size_t, 2U>{3U, MAX_RECORD_BYTES}) {
        FakeStorage fake;
        auto journal = fake.Journal();
        Check(journal.Open(0x211U) == JournalOpenResult::Initialized &&
                  journal.Append(LineBase(1U, 1U)) == JournalMutationResult::Applied,
              "uncertain append fixture initializes");
        fake.failAfterAppendBytes = written;
        Check(journal.Append(Ply(2U, 1U)) == JournalMutationResult::IoFailure,
              "partially or fully written failed append reports failure");
        const auto saved = fake.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes;
        Check(journal.Append(Ply(2U, 1U)) == JournalMutationResult::IoFailure &&
                  fake.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes == saved,
              "retry cannot write until uncertain append is reconciled");
        Check(journal.Open(0x211U) == JournalOpenResult::Ready,
              "explicit reopen reconciles uncertain append");
        if (written == 3U) {
            Check(journal.Summary().hasInvalidSuffix &&
                      journal.Append(Ply(2U, 1U)) == JournalMutationResult::Corrupt,
                  "partial append is preserved and blocks mutation");
        } else {
            Check(journal.Summary().newestAllocated == 2U &&
                      journal.Append(Seal(3U, 1U)) == JournalMutationResult::Applied,
                  "complete failed append recovers without duplicating sequence");
        }
    }
}

void TestReclaimPreservesInvalidTail() {
    FakeStorage fake;
    auto journal = fake.Journal();
    Check(journal.Open(0x212U) == JournalOpenResult::Initialized &&
              journal.Append(LineBase(1U, 1U)) == JournalMutationResult::Applied &&
              journal.Append(Seal(2U, 1U)) == JournalMutationResult::Applied &&
              journal.AdvanceDurableAck(2U) == JournalMutationResult::Applied,
          "ACKed game prepares corrupt-tail reclaim fixture");
    auto& bytes = fake.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes;
    bytes.push_back(0x01U);
    const auto saved = bytes;
    Check(journal.Open(0x212U) == JournalOpenResult::Ready &&
              journal.Summary().hasInvalidSuffix,
          "reopen exposes invalid tail after ACKed game");
    Check(journal.ReclaimAcknowledged() == JournalMutationResult::Corrupt &&
              bytes == saved && journal.Summary().hasInvalidSuffix,
          "reclaim preserves unacknowledged damaged tail and diagnostics");
}

void TestCompactionVerifiesExactCopiedBytes() {
    FakeStorage fake;
    auto journal = fake.Journal();
    Check(journal.Open(0x213U) == JournalOpenResult::Initialized &&
              journal.Append(LineBase(1U, 1U)) == JournalMutationResult::Applied &&
              journal.Append(Seal(2U, 1U)) == JournalMutationResult::Applied &&
              journal.Append(LineBase(3U, 2U)) == JournalMutationResult::Applied &&
              journal.AdvanceDurableAck(2U) == JournalMutationResult::Applied,
          "exact compaction fixture prepares retained game");
    const auto saved = fake.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes;
    fake.alterCompactionRecord = true;
    Check(journal.ReclaimAcknowledged() == JournalMutationResult::IoFailure &&
              fake.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes == saved,
          "same-length CRC-valid altered copy cannot replace original journal");
    Check(journal.Open(0x213U) == JournalOpenResult::Ready &&
              journal.Summary().oldestRetained == 3U,
          "reopen retries pending compaction from preserved original");
    Record retained;
    Check(journal.ReadLatestRecord(retained) == JournalSegmentRead::Ready &&
              retained.lineRevision == 1U,
          "recovered compaction preserves exact original revision");
}

Position DuckPosition(const std::array<char, 64U>& board, const bool black,
                      const uint8_t duck) {
    std::string fen;
    for (size_t rank = 0U; rank < 8U; ++rank) {
        unsigned empty = 0U;
        for (size_t file = 0U; file < 8U; ++file) {
            const char piece = board[rank * 8U + file];
            if (piece == 0) { ++empty; continue; }
            if (empty != 0U) { fen += static_cast<char>('0' + empty); empty = 0U; }
            fen += piece;
        }
        if (empty != 0U) fen += static_cast<char>('0' + empty);
        if (rank != 7U) fen += '/';
    }
    fen += black ? " b - - 0 1" : " w - - 0 1";
    Position result;
    Check(EncodePosition(fen.c_str(), Variant::Duck, duck, result),
          "Duck boundary position encodes through canonical codec");
    return result;
}

void TestTerminalDuckCaptureWaitsForSealAndFitsTogether() {
    FakeStorage fake;
    auto journal = fake.Journal();
    Check(journal.Open(0x214U) == JournalOpenResult::Initialized,
          "Duck terminal boundary journal initializes");
    std::array<char, 64U> board{};
    board[0] = 'n'; board[7] = 'k'; board[56] = 'N';
    board[60] = 'K'; board[63] = 'R';
    uint8_t duck = 34U;
    Record base = LineBase(1U, 1U);
    base.variant = Variant::Duck;
    base.position = DuckPosition(board, false, duck);
    Check(journal.Append(base) == JournalMutationResult::Applied,
          "Duck partial line base appends");
    uint8_t whiteKnight = 56U;
    uint8_t blackKnight = 0U;
    for (uint16_t ply = 1U; ply <= 23U; ++ply) {
        const bool black = ply % 2U == 0U;
        Record move = Ply(ply + 1U, 1U);
        move.ply = ply;
        move.lineRevision = ply + 1U;
        move.transition = {};
        move.transition.side = black ? Side::Black : Side::White;
        if (ply == 23U) {
            move.transition.from = 63U;
            move.transition.to = 7U;
            move.transition.captureSquare = 7U;
        } else {
            uint8_t& knight = black ? blackKnight : whiteKnight;
            const uint8_t home = black ? 0U : 56U;
            const uint8_t away = black ? 17U : 41U;
            move.transition.from = knight;
            move.transition.to = knight == home ? away : home;
            knight = move.transition.to;
            move.transition.duckFrom = duck;
            duck = duck == 34U ? 35U : 34U;
            move.transition.duckTo = duck;
        }
        board[move.transition.to] = board[move.transition.from];
        board[move.transition.from] = 0;
        move.position = DuckPosition(board, !black, duck);
        Check(journal.Append(move) == JournalMutationResult::Applied,
              "Duck knight shuffle and final rook king capture append");
    }
    Check(journal.Summary().journalBytes == 2035U,
          "terminal fixture reaches real 2048-byte split boundary");
    JournalSegmentPlan plan;
    Check(journal.PlanSegmentAfter(0U, plan) == JournalSegmentRead::Ready &&
              plan.descriptor.recordCount == 23U &&
              plan.descriptor.encodedBytes == 1951U,
          "pending terminal capture is withheld while valid prefix remains available");
    Check(journal.PlanSegmentAfter(23U, plan) == JournalSegmentRead::Unavailable,
          "no malformed active Duck archive is exported before durable Seal");
    for (const bool unrelatedSeal : {false, true}) {
        FakeStorage other = fake;
        auto wrong = other.Journal();
        Check(wrong.Open(0x214U) == JournalOpenResult::Ready &&
                  wrong.Append(unrelatedSeal ? Seal(25U, 2U) : LineBase(25U, 1U)) ==
                      JournalMutationResult::Applied,
              "intervening record negative control appends");
        Check(wrong.PlanSegmentAfter(23U, plan) == JournalSegmentRead::Unavailable,
              "terminal capture cannot borrow another game result or skip a record");
    }
    Record seal = Seal(25U, 1U);
    seal.lineRevision = 25U;
    seal.outcome = Outcome::KingCaptured;
    seal.winner = Winner::White;
    Check(journal.Append(seal) == JournalMutationResult::Applied,
          "terminal result becomes durable");
    Check(journal.PlanSegmentAfter(0U, plan) == JournalSegmentRead::Ready &&
              plan.descriptor.recordCount == 23U,
          "byte cap splits before terminal capture even when Seal is available");
    Check(journal.PlanSegmentAfter(23U, plan) == JournalSegmentRead::Ready &&
              plan.descriptor.recordCount == 3U &&
              plan.descriptor.encodedBytes == 207U &&
              plan.descriptor.firstOutboxSequence == 23U &&
              plan.descriptor.flags == SEGMENT_FLAG_ACKNOWLEDGED_OVERLAP,
          "next segment includes exact overlap, terminal capture and Seal together");
    std::array<uint8_t, MAX_SEGMENT_BYTES> emitted{};
    size_t received = 0U;
    while (received < plan.descriptor.encodedBytes) {
        size_t count = 0U;
        const auto read = journal.ReadSegmentChunk(
            plan, static_cast<uint16_t>(received), emitted.data() + received, 12U, count);
        Check(read == JournalSegmentRead::Ready && count > 0U,
              "terminal pair streams through minimum-MTU chunks");
        if (read != JournalSegmentRead::Ready || count == 0U) break;
        received += count;
    }
    Record overlap;
    Record capture;
    Record result;
    Check(received == 207U &&
              DecodeRecord(emitted.data(), 85U, overlap) &&
              DecodeRecord(emitted.data() + 85U, 84U, capture) &&
              DecodeRecord(emitted.data() + 169U, 38U, result) &&
              overlap.outboxSequence == 23U && capture.outboxSequence == 24U &&
              capture.transition.captureSquare == 7U &&
              capture.transition.duckTo == INVALID_SQUARE &&
              result.type == RecordType::Seal && result.outboxSequence == 25U &&
              result.outcome == Outcome::KingCaptured && result.winner == Winner::White,
          "emitted canonical bytes retain overlap, capture without Duck move, and winning result");
}

void TestSegmentsUseDurableOverlapAndReportGaps() {
    FakeStorage fake;
    auto journal = fake.Journal();
    Check(journal.Open(0x303U) == JournalOpenResult::Initialized &&
              journal.Append(LineBase(1U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(Ply(2U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(Seal(3U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(LineBase(4U, 2U)) ==
                  JournalMutationResult::Applied,
          "two-game segment fixture appends");

    JournalSegmentPlan plan;
    Check(journal.PlanSegmentAfter(1U, plan) ==
              JournalSegmentRead::Ready &&
              plan.descriptor.flags ==
                  SEGMENT_FLAG_ACKNOWLEDGED_OVERLAP &&
              plan.descriptor.firstOutboxSequence == 1U &&
              plan.descriptor.recordCount == 3U &&
              plan.descriptor.encodedBytes == 202U,
          "continuation includes one ACKed position as overlap");
    std::array<uint8_t, MAX_FRAME_PAYLOAD_BYTES> frame{};
    size_t frameLength = 0U;
    Check(journal.ReadSegmentChunk(plan, 0U, frame.data(), 12U,
                                   frameLength) ==
              JournalSegmentRead::Ready && frameLength == 12U,
          "planned segment streams through a minimum-MTU data payload");

    Check(journal.AdvanceDurableAck(3U) ==
              JournalMutationResult::Applied &&
              journal.ReclaimAcknowledged() ==
                  JournalMutationResult::Applied &&
              journal.Summary().oldestRetained == 4U &&
              journal.Summary().lostGameCount == 0U,
          "ACKed sealed game reclaims without reporting loss");
    Check(journal.PlanSegmentAfter(0U, plan) ==
              JournalSegmentRead::GapReady &&
              plan.descriptor.firstOutboxSequence == 4U &&
              plan.descriptor.flags == 0U,
          "evicted prefix produces a self-contained partial line base");
}

void TestInterruptedCompactionResumesFromDurableIntent() {
    FakeStorage fake;
    auto journal = fake.Journal();
    Check(journal.Open(0x404U) == JournalOpenResult::Initialized &&
              journal.Append(LineBase(1U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(Seal(2U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(LineBase(3U, 2U)) ==
                  JournalMutationResult::Applied &&
              journal.AdvanceDurableAck(2U) ==
                  JournalMutationResult::Applied,
          "compaction recovery fixture prepares ACKed game");
    fake.failNextPromote = true;
    Check(journal.ReclaimAcknowledged() ==
              JournalMutationResult::IoFailure,
          "failed promotion leaves durable compaction intent");

    const auto stateA = fake.artifacts[FakeStorage::Index(JournalArtifact::StateA)].bytes;
    const auto stateB = fake.artifacts[FakeStorage::Index(JournalArtifact::StateB)].bytes;
    const auto original = fake.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes;
    fake.failNextRead = JournalArtifact::StateB;
    auto unreadable = fake.Journal();
    Check(unreadable.Open(0x505U) == JournalOpenResult::IoFailure &&
              fake.artifacts[FakeStorage::Index(JournalArtifact::StateA)].bytes == stateA &&
              fake.artifacts[FakeStorage::Index(JournalArtifact::StateB)].bytes == stateB &&
              fake.artifacts[FakeStorage::Index(JournalArtifact::Journal)].bytes == original,
          "unreadable state slot fails without silently using older intent");

    auto recovered = fake.Journal();
    Check(recovered.Open(0x505U) == JournalOpenResult::Ready &&
              recovered.Summary().journalBytes == 81U &&
              recovered.Summary().oldestRetained == 3U &&
              recovered.Summary().newestAllocated == 3U &&
              recovered.Summary().durableAck == 2U &&
              recovered.Summary().compactionCount == 1U,
          "reopen resumes intent without losing sequence or ACK state");
}

void TestCapacityEvictsWholeOldestGameAndCountsLoss() {
    FakeStorage fake;
    auto initialized = fake.Journal();
    Check(initialized.Open(0x606U) == JournalOpenResult::Initialized,
          "large fixture journal initializes");

    uint32_t sequence = 1U;
    for (uint32_t game = 1U; game <= 4404U; ++game) {
        fake.AppendRaw(LineBase(sequence++, game));
        fake.AppendRaw(Seal(sequence++, game));
    }
    fake.AppendRaw(LineBase(sequence++, 4405U));
    fake.AppendRaw(Ply(sequence++, 4405U));
    fake.AppendRaw(Seal(sequence++, 4405U));
    Check(fake.artifacts[FakeStorage::Index(JournalArtifact::Journal)]
                  .bytes.size() == 524278U,
          "large fixture reaches cap with ten bytes spare");

    auto journal = fake.Journal();
    Check(journal.Open(0x707U) == JournalOpenResult::Ready &&
              journal.Summary().newestAllocated == sequence - 1U,
          "large fixture repairs its append watermark");
    Check(journal.Append(LineBase(sequence, 4406U)) ==
              JournalMutationResult::Applied &&
              journal.Summary().oldestRetained == 3U &&
              journal.Summary().lostGameCount == 1U &&
              journal.Summary().journalBytes <= JOURNAL_MAX_BYTES,
          "overflow evicts exactly one complete oldest game");
}

void TestSequenceExhaustionNeverWraps() {
    FakeStorage fake;
    auto initialized = fake.Journal();
    Check(initialized.Open(0x808U) == JournalOpenResult::Initialized,
          "exhaustion fixture initializes");
    fake.AppendRaw(LineBase(UINT32_MAX, 1U));
    auto journal = fake.Journal();
    Check(journal.Open(0x909U) == JournalOpenResult::Ready &&
              journal.Summary().sequenceExhausted &&
              journal.Summary().newestAllocated == UINT32_MAX,
          "maximum sequence repairs into exhausted state");
    Check(journal.Append(LineBase(1U, 2U)) ==
              JournalMutationResult::Exhausted,
          "exhausted journal never wraps to sequence one");
}

} // namespace

int main() {
    std::puts("[GAME_HISTORY_JOURNAL] start");
    TestSteadyStateReadsDoNotGrowWithRetainedHistory();
    TestLatestMetadataTracksCompaction();
    TestCleanTailLossCannotResumeAtAllocationWatermark();
    TestAppendReopenAndInterruptedMetadataRepair();
    TestInvalidSuffixIsRetainedAndNeverAppendedThrough();
    TestUncertainAppendRequiresReopen();
    TestReclaimPreservesInvalidTail();
    TestCompactionVerifiesExactCopiedBytes();
    TestTerminalDuckCaptureWaitsForSealAndFitsTogether();
    TestSegmentsUseDurableOverlapAndReportGaps();
    TestInterruptedCompactionResumesFromDurableIntent();
    TestCapacityEvictsWholeOldestGameAndCountsLoss();
    TestSequenceExhaustionNeverWraps();

    if (g_failures == 0) {
        std::puts("[GAME_HISTORY_JOURNAL] ok");
        return 0;
    }
    std::printf("[GAME_HISTORY_JOURNAL] failures=%d\n", g_failures);
    return 1;
}
