#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "modules/current_game_sync_source.hpp"
#include "modules/sha256.hpp"

namespace {

using namespace modules;
using namespace modules::current_game_log;
using namespace modules::current_game_sync;
using namespace modules::game_history;

int failures = 0;

void Check(const bool condition, const char* const message) {
    if (!condition) {
        ++failures;
        std::printf("[FAIL] %s\n", message);
    }
}

template <size_t Count>
bool EqualsHex(const std::array<uint8_t, Count>& bytes,
               const char* const hex) {
    if (hex == nullptr || std::strlen(hex) != Count * 2U) return false;
    for (size_t index = 0U; index < Count; ++index) {
        const auto nibble = [](const char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            return -1;
        };
        const int high = nibble(hex[index * 2U]);
        const int low = nibble(hex[index * 2U + 1U]);
        if (high < 0 || low < 0 ||
            bytes[index] != static_cast<uint8_t>((high << 4U) | low)) {
            return false;
        }
    }
    return true;
}

struct FakeStorage {
    std::array<std::vector<uint8_t>, 2U> slots{};
    uint32_t beginCalls = 0U;
    uint32_t endCalls = 0U;
    uint32_t readCalls = 0U;
    bool failBatch = false;
    bool failRead = false;

    static size_t Index(const Slot slot) {
        return static_cast<size_t>(slot);
    }
    static StorageStatus Stat(void* const context,
                              const Slot slot,
                              uint32_t& outSize) {
        const auto& bytes =
            static_cast<FakeStorage*>(context)->slots[Index(slot)];
        if (bytes.empty()) {
            outSize = 0U;
            return StorageStatus::Missing;
        }
        outSize = static_cast<uint32_t>(bytes.size());
        return StorageStatus::Present;
    }
    static bool Read(void* const context,
                     const Slot slot,
                     const uint32_t offset,
                     uint8_t* const out,
                     const size_t length) {
        auto& self = *static_cast<FakeStorage*>(context);
        ++self.readCalls;
        const auto& bytes = self.slots[Index(slot)];
        if (self.failRead || out == nullptr || offset > bytes.size() ||
            length > bytes.size() - offset) {
            return false;
        }
        std::memcpy(out, bytes.data() + offset, length);
        return true;
    }
    static bool Begin(void* const context) {
        auto& self = *static_cast<FakeStorage*>(context);
        ++self.beginCalls;
        return !self.failBatch;
    }
    static void End(void* const context) {
        ++static_cast<FakeStorage*>(context)->endCalls;
    }
    StorageOps Ops() {
        return {this, &Stat, &Read, nullptr, nullptr, nullptr, &Begin, &End};
    }
};

GameId Id() {
    GameId id{};
    for (size_t index = 0U; index < id.bytes.size(); ++index) {
        id.bytes[index] = static_cast<uint8_t>(index + 1U);
    }
    return id;
}

Record Base(const uint16_t ply = 0U) {
    Record record;
    record.type = RecordType::LineBase;
    record.outboxSequence = 1U;
    record.gameId = Id();
    record.lineRevision = 100U;
    record.ply = ply;
    record.variant = Variant::Classic;
    Check(EncodePosition(
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
              Variant::Classic, INVALID_SQUARE, record.position),
          "base position encodes");
    return record;
}

Record Move(const uint32_t sequence, const uint16_t ply) {
    Record record;
    record.type = RecordType::CommittedPly;
    record.outboxSequence = sequence;
    record.gameId = Id();
    record.lineRevision = static_cast<uint64_t>(99U + sequence);
    record.ply = ply;
    record.position = Base().position;
    record.transition.side = (ply & 1U) == 0U ? Side::Black : Side::White;
    record.transition.from = static_cast<uint8_t>((ply - 1U) % 64U);
    record.transition.to = static_cast<uint8_t>(ply % 64U);
    return record;
}

struct Line {
    FakeStorage storage;
    CommitPointer pointer;
    std::vector<size_t> offsets;
};

void AppendRecord(Line& line, const Record& record) {
    std::array<uint8_t, MAX_RECORD_BYTES> encoded{};
    size_t length = 0U;
    Check(EncodeRecord(record, encoded.data(), encoded.size(), length),
          "line record encodes");
    auto& bytes = line.storage.slots[0U];
    line.offsets.push_back(bytes.size());
    bytes.insert(bytes.end(), encoded.begin(), encoded.begin() + length);
}

void FinalizeLine(Line& line,
                  const uint16_t finalPly,
                  const uint64_t revision) {
    Crc32Accumulator crc;
    const auto& bytes = line.storage.slots[0U];
    Check(crc.Update(bytes.data(), bytes.size()), "line CRC computes");
    line.pointer.present = true;
    line.pointer.slot = Slot::A;
    line.pointer.generation = 7U;
    line.pointer.byteLength = static_cast<uint32_t>(bytes.size());
    line.pointer.prefixCrc32 = crc.Value();
    line.pointer.gameId = Id();
    line.pointer.lineRevision = revision;
    line.pointer.currentPly = finalPly;
    line.pointer.archiveRequired = true;
}

Line BuildLine(const uint16_t firstPly,
               const uint16_t moveCount,
               const bool sealed = false) {
    Line line;
    Record base = Base(firstPly);
    AppendRecord(line, base);
    uint64_t revision = base.lineRevision;
    uint16_t finalPly = firstPly;
    for (uint16_t index = 0U; index < moveCount; ++index) {
        ++finalPly;
        Record move = Move(static_cast<uint32_t>(index) + 2U, finalPly);
        revision = move.lineRevision;
        AppendRecord(line, move);
    }
    if (sealed) {
        Record seal;
        seal.type = RecordType::Seal;
        seal.outboxSequence = static_cast<uint32_t>(moveCount) + 2U;
        seal.gameId = Id();
        seal.lineRevision = revision;
        seal.status = ArchiveStatus::Completed;
        seal.outcome = Outcome::Draw;
        seal.winner = Winner::Draw;
        AppendRecord(line, seal);
    }
    FinalizeLine(line, finalPly, revision);
    return line;
}

ResumeCursor CursorForPrefix(const Line& line, const uint32_t nextSequence) {
    Sha256 sha;
    const size_t prefixLength = nextSequence <= 1U
        ? 0U
        : (nextSequence > line.offsets.size()
               ? line.storage.slots[0U].size()
               : line.offsets[nextSequence - 1U]);
    Check(sha.Update(line.storage.slots[0U].data(), prefixLength),
          "prefix SHA computes");
    const Sha256::Digest digest = sha.Finish();
    ResumeCursor cursor;
    cursor.nextRecordSequence = nextSequence;
    std::copy_n(digest.begin(), cursor.prefixFingerprint.size(),
                cursor.prefixFingerprint.begin());
    return cursor;
}

void RunShaVectors() {
    Sha256 empty;
    Check(EqualsHex(
              empty.Finish(),
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855"),
          "SHA-256 empty vector matches");
    Sha256 abc;
    const uint8_t bytes[] = {'a', 'b', 'c'};
    Check(abc.Update(bytes, sizeof(bytes)) &&
              EqualsHex(
                  abc.Finish(),
                  "ba7816bf8f01cfea414140de5dae2223"
                  "b00361a396177a9cb410ff61f20015ad"),
          "SHA-256 abc vector matches");
    constexpr char paddingBoundary[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha256 multiBlock;
    Check(multiBlock.Update(
              reinterpret_cast<const uint8_t*>(paddingBoundary), 13U) &&
              multiBlock.Update(
                  reinterpret_cast<const uint8_t*>(paddingBoundary) + 13U,
                  sizeof(paddingBoundary) - 1U - 13U) &&
              EqualsHex(
                  multiBlock.Finish(),
                  "248d6a61d20638b8e5c026930c3e6039"
                  "a33ce45964ff2167f6ecedd419db06c1"),
          "SHA-256 padding-boundary multi-block vector matches");
}

void RunOpenAndPlanJourney() {
    Line line = BuildLine(42U, 40U, true);
    CurrentGameSyncSource source(line.storage.Ops());
    SourceSnapshot snapshot;
    Sha256 directSha;
    Check(directSha.Update(line.storage.slots[0U].data(),
                           line.storage.slots[0U].size()),
          "direct full-line SHA computes");
    const Sha256::Digest directDigest = directSha.Finish();
    Check(source.Open(line.pointer, snapshot) == SourceResult::Ready &&
              snapshot.bounds.recordCount == 42U &&
              snapshot.bounds.lineRevision == line.pointer.lineRevision &&
              snapshot.bounds.flags ==
                  uint8_t(CURRENT_LINE_PRESENT | ARCHIVE_REQUIRED |
                          LINE_SEALED),
          "arbitrary-FEN line opens with exact record count");
    Check(std::equal(snapshot.digest.fingerprint.begin(),
                     snapshot.digest.fingerprint.end(),
                     directDigest.begin()),
          "advertised digest is the exact committed line SHA prefix");
    Check(line.storage.beginCalls == line.storage.endCalls,
          "open releases its storage batch");

    SegmentPlan first;
    Check(source.Plan(line.pointer, ResumeCursor{}, first) ==
              SourceResult::Ready &&
              first.descriptor.firstOutboxSequence == 1U &&
              first.descriptor.recordCount <= MAX_SEGMENT_RECORDS &&
              first.descriptor.encodedBytes <=
                  modules::current_game_sync::MAX_SEGMENT_BYTES,
          "empty cursor plans a bounded first segment");
    std::vector<uint8_t> received(first.descriptor.encodedBytes);
    size_t copied = 0U;
    while (copied < received.size()) {
        size_t length = 0U;
        const bool read = source.ReadSegmentChunk(
                              line.pointer, first,
                              static_cast<uint16_t>(copied),
                              received.data() + copied, 19U, length) ==
                              SourceResult::Ready &&
            length != 0U;
        Check(read,
              "segment streams through minimum-ATT-sized chunks");
        if (!read) break;
        copied += length;
    }
    Crc32Accumulator crc;
    Check(crc.Update(received.data(), received.size()) &&
              crc.Value() == first.descriptor.crc32,
          "streamed segment matches its advertised CRC");

    const uint32_t next = first.descriptor.LastOutboxSequence() + 1U;
    SegmentPlan resumed;
    Check(source.Plan(line.pointer, CursorForPrefix(line, next), resumed) ==
              SourceResult::Ready &&
              resumed.descriptor.firstOutboxSequence == next,
          "exact durable prefix resumes at the next record");
    ResumeCursor changed = CursorForPrefix(line, next);
    changed.prefixFingerprint[0U] ^= 0x80U;
    Check(source.Plan(line.pointer, changed, resumed) ==
              SourceResult::CursorMismatch,
          "changed resume fingerprint restarts rather than skipping records");
    Check(source.Plan(
              line.pointer,
              CursorForPrefix(line, snapshot.bounds.recordCount + 1U),
              resumed) == SourceResult::Complete,
          "complete cursor requires no data segment");

    Check(source.Plan(line.pointer, ResumeCursor{}, first) ==
              SourceResult::Ready,
          "valid plan can be re-armed after complete cursor");
    CommitPointer stale = line.pointer;
    ++stale.generation;
    size_t ignored = 0U;
    uint8_t byte = 0U;
    Check(source.ReadSegmentChunk(
              line.pointer, first, 0U, &byte, 1U, ignored) ==
              SourceResult::Ready && ignored == 1U &&
              source.ReadSegmentChunk(stale, first, 0U, &byte, 1U, ignored) ==
              SourceResult::BadState,
          "changed durable authority, not the plan lifecycle, fences reads");

    uint32_t coldNext = 1U;
    uint32_t receivedRecords = 0U;
    for (;;) {
        CurrentGameSyncSource cold(line.storage.Ops());
        SourceSnapshot coldSnapshot;
        SegmentPlan coldPlan;
        Check(cold.Open(line.pointer, coldSnapshot) == SourceResult::Ready,
              "cold source reopens the pinned line");
        const SourceResult planned = cold.Plan(
            line.pointer, CursorForPrefix(line, coldNext), coldPlan);
        if (planned == SourceResult::Complete) break;
        Check(planned == SourceResult::Ready,
              "cold source resumes the next bounded segment");
        if (planned != SourceResult::Ready) break;
        std::vector<uint8_t> segment(coldPlan.descriptor.encodedBytes);
        size_t length = 0U;
        Check(cold.ReadSegmentChunk(
                  line.pointer, coldPlan, 0U, segment.data(),
                  segment.size(), length) == SourceResult::Ready &&
                  length == segment.size(),
              "cold source reads the complete planned segment");
        receivedRecords += coldPlan.descriptor.recordCount;
        coldNext = coldPlan.descriptor.LastOutboxSequence() + 1U;
    }
    Check(receivedRecords == snapshot.bounds.recordCount,
          "cold reopen transfers every record across all segments");
}

void RunFailures() {
    Line line = BuildLine(0U, 1U);
    CurrentGameSyncSource source(line.storage.Ops());
    SourceSnapshot snapshot;
    line.storage.failBatch = true;
    Check(source.Open(line.pointer, snapshot) == SourceResult::IoFailure &&
              line.storage.endCalls == 0U,
          "failed batch admission performs no unmatched release");
    line.storage.failBatch = false;
    line.storage.failRead = true;
    Check(source.Open(line.pointer, snapshot) == SourceResult::IoFailure,
          "storage read failure remains distinguishable from corruption");
    line.storage.failRead = false;
    line.storage.slots[0U][10U] ^= 0x80U;
    Check(source.Open(line.pointer, snapshot) == SourceResult::Corrupt,
          "corrupt committed bytes cannot become a sync snapshot");
    Check(source.Open(CommitPointer{}, snapshot) == SourceResult::NoLine &&
              snapshot.bounds.flags == 0U,
          "absent line produces an explicit empty state");
}

void RunShortenedLineResume() {
    Line oldLine = BuildLine(0U, 20U);
    const ResumeCursor oldCursor = CursorForPrefix(oldLine, 21U);
    Line rewritten = BuildLine(0U, 4U);
    ++rewritten.pointer.generation;
    CurrentGameSyncSource source(rewritten.storage.Ops());
    SourceSnapshot snapshot;
    SegmentPlan plan;
    Check(source.Open(rewritten.pointer, snapshot) == SourceResult::Ready &&
              source.Plan(rewritten.pointer, oldCursor, plan) ==
                  SourceResult::CursorMismatch,
          "cursor beyond a shortened Undo line restarts at record one");
}

void RunVariantContinuityFailures() {
    Line mixed;
    AppendRecord(mixed, Base());
    Record duckMove = Move(2U, 1U);
    Check(EncodePosition(
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1",
              Variant::Duck, 24U, duckMove.position),
          "cross-variant Duck position encodes");
    AppendRecord(mixed, duckMove);
    FinalizeLine(mixed, 1U, duckMove.lineRevision);
    CurrentGameSyncSource source(mixed.storage.Ops());
    SourceSnapshot snapshot;
    Check(source.Open(mixed.pointer, snapshot) == SourceResult::Corrupt,
          "Classic line rejects a CRC-valid Duck committed position");

    Line invalidDuck;
    Record duckBase = Base();
    duckBase.variant = Variant::Duck;
    Check(EncodePosition(
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
              Variant::Duck, 24U, duckBase.position),
          "Duck base position encodes");
    AppendRecord(invalidDuck, duckBase);
    Record missingRelocation = Move(2U, 1U);
    Check(EncodePosition(
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1",
              Variant::Duck, 25U, missingRelocation.position),
          "Duck result position encodes without relocation metadata");
    AppendRecord(invalidDuck, missingRelocation);
    FinalizeLine(invalidDuck, 1U, missingRelocation.lineRevision);
    CurrentGameSyncSource duckSource(invalidDuck.storage.Ops());
    Check(duckSource.Open(invalidDuck.pointer, snapshot) ==
              SourceResult::Corrupt,
          "Duck line rejects missing relocation before advertising digest");
}

} // namespace

int main() {
    RunShaVectors();
    RunOpenAndPlanJourney();
    RunFailures();
    RunShortenedLineResume();
    RunVariantContinuityFailures();
    if (failures == 0) std::puts("[CURRENT_GAME_SYNC_SOURCE] ok");
    return failures == 0 ? 0 : 1;
}
