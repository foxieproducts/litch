#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include <ArduinoJson.h>

#include "foxie_embedded/checksum.hpp"
#include "modules/current_game_log.hpp"
#include "modules/current_game_log_persistence.hpp"

namespace {

using namespace modules::current_game_log;
using namespace modules::game_history;

int failures = 0;

void Check(const bool condition, const char* const message) {
    if (!condition) {
        ++failures;
        std::printf("[FAIL] %s\n", message);
    }
}

struct Artifact {
    bool present = false;
    std::vector<uint8_t> bytes;
};

struct FakeStorage {
    std::array<Artifact, 2U> slots{};
    uint32_t appendCalls = 0U;
    uint32_t readCalls = 0U;
    uint32_t failAppendCall = 0U;
    bool writeBeforeAppendFailure = false;
    uint32_t replaceCalls = 0U;
    uint32_t failReplaceCall = 0U;
    bool writeBeforeReplaceFailure = false;
    bool partialFailure = false;
    bool corruptNextAppendReadback = false;
    uint32_t lastAppendOffset = 0U;
    uint32_t lastAppendLength = 0U;

    static size_t Index(const Slot slot) { return static_cast<size_t>(slot); }

    static StorageStatus Stat(void *const context, const Slot slot, uint32_t &outSize) {
        const Artifact &artifact = static_cast<FakeStorage *>(context)->slots[Index(slot)];
        if (!artifact.present) {
            outSize = 0U;
            return StorageStatus::Missing;
        }
        outSize = static_cast<uint32_t>(artifact.bytes.size());
        return StorageStatus::Present;
    }

    static bool Read(void *const context, const Slot slot, const uint32_t offset,
                     uint8_t *const out, const size_t length) {
        auto &self = *static_cast<FakeStorage *>(context);
        ++self.readCalls;
        const Artifact &artifact = self.slots[Index(slot)];
        if (!artifact.present || out == nullptr || offset > artifact.bytes.size() ||
            length > artifact.bytes.size() - offset) {
            return false;
        }
        std::memcpy(out, artifact.bytes.data() + offset, length);
        if (self.corruptNextAppendReadback && offset == self.lastAppendOffset &&
            length == self.lastAppendLength && length != 0U) {
            out[0U] ^= 0x80U;
            self.corruptNextAppendReadback = false;
        }
        return true;
    }

    static bool Replace(void *const context, const Slot slot, const uint8_t *const bytes,
                        const size_t length) {
        if (bytes == nullptr || length == 0U)
            return false;
        auto &self = *static_cast<FakeStorage *>(context);
        Artifact &artifact = self.slots[Index(slot)];
        const bool fail = ++self.replaceCalls == self.failReplaceCall;
        if (!fail || self.writeBeforeReplaceFailure) {
            artifact.present = true;
            artifact.bytes.assign(bytes,
                                  bytes + (fail && self.partialFailure ? length / 2U : length));
        }
        return !fail;
    }

    static bool Append(void *const context, const Slot slot, const uint8_t *const bytes,
                       const size_t length) {
        if (bytes == nullptr || length == 0U)
            return false;
        auto &self = *static_cast<FakeStorage *>(context);
        Artifact &artifact = self.slots[Index(slot)];
        const bool fail = ++self.appendCalls == self.failAppendCall;
        self.lastAppendOffset = static_cast<uint32_t>(artifact.bytes.size());
        self.lastAppendLength = static_cast<uint32_t>(length);
        if (!fail || self.writeBeforeAppendFailure) {
            artifact.present = true;
            artifact.bytes.insert(artifact.bytes.end(), bytes,
                                  bytes + (fail && self.partialFailure ? length / 2U : length));
        }
        return !fail;
    }

    static bool Truncate(void *const context, const Slot slot, const uint32_t length) {
        Artifact &artifact = static_cast<FakeStorage *>(context)->slots[Index(slot)];
        if (!artifact.present || length > artifact.bytes.size())
            return false;
        artifact.bytes.resize(length);
        return true;
    }

    StorageOps Ops() { return {this, &Stat, &Read, &Replace, &Append, &Truncate}; }
};

struct FakeSettings {
    CommitPointer pending{};
    uint32_t calls = 0U;
    uint32_t nextReceipt = 40U;
    bool reject = false;

    static bool Enqueue(void* const context,
                        const CommitPointer& pointer,
                        SettingsOps::Receipt& outReceipt) {
        auto& self = *static_cast<FakeSettings*>(context);
        ++self.calls;
        if (self.reject) {
            outReceipt = 0U;
            return false;
        }
        self.pending = pointer;
        outReceipt = ++self.nextReceipt;
        return true;
    }

    SettingsOps Ops() { return {this, &Enqueue}; }
};

GameId Id() {
    GameId id;
    for (size_t index = 0U; index < id.bytes.size(); ++index) {
        id.bytes[index] = static_cast<uint8_t>(index + 1U);
    }
    return id;
}

Record Initial() {
    Record record;
    record.type = RecordType::LineBase;
    record.outboxSequence = 1U;
    record.gameId = Id();
    record.lineRevision = 1U;
    record.ply = 0U;
    record.variant = Variant::Classic;
    Check(EncodePosition(
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
              Variant::Classic, INVALID_SQUARE, record.position),
          "initial position encodes");
    return record;
}

Record E4() {
    Record record;
    record.type = RecordType::CommittedPly;
    record.outboxSequence = 2U;
    record.gameId = Id();
    record.lineRevision = 2U;
    record.ply = 1U;
    record.transition.side = Side::White;
    record.transition.from = 52U;
    record.transition.to = 36U;
    Check(EncodePosition(
              "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
              Variant::Classic, INVALID_SQUARE, record.position),
          "e4 position encodes");
    return record;
}

Record CheckmateSeal() {
    Record record;
    record.type = RecordType::Seal;
    record.outboxSequence = 3U;
    record.gameId = Id();
    record.lineRevision = 2U;
    record.status = ArchiveStatus::Completed;
    record.outcome = Outcome::Checkmate;
    record.winner = Winner::White;
    return record;
}

Record DuckBase() {
    Record record = Initial();
    record.variant = Variant::Duck;
    Check(EncodePosition(
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
              Variant::Duck, 24U, record.position),
          "Duck base fixture encodes");
    return record;
}

Record DuckPly() {
    Record record = E4();
    record.transition.duckFrom = 24U;
    record.transition.duckTo = 25U;
    Check(EncodePosition(
              "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
              Variant::Duck, 25U, record.position),
          "Duck ply fixture encodes");
    return record;
}

Record ClassicPlyWithDuckTransition() {
    Record record = E4();
    record.transition.duckFrom = 24U;
    record.transition.duckTo = 25U;
    return record;
}

Record DuckKingCaptureBase() {
    Record record = DuckBase();
    Check(EncodePosition(
              "k7/8/8/8/8/8/8/R6K w - - 0 1",
              Variant::Duck, 35U, record.position),
          "Duck king-capture base fixture encodes");
    return record;
}

Record DuckKingCapturePly() {
    Record record = DuckPly();
    record.transition.from = 56U;
    record.transition.to = 0U;
    record.transition.captureSquare = 0U;
    record.transition.duckFrom = INVALID_SQUARE;
    record.transition.duckTo = INVALID_SQUARE;
    Check(EncodePosition(
              "R7/8/8/8/8/8/8/7K b - - 0 1",
              Variant::Duck, 35U, record.position),
          "Duck king-capture result fixture encodes");
    return record;
}

Record DuckKingCaptureWithRelocation() {
    Record record = DuckKingCapturePly();
    record.transition.duckFrom = 35U;
    record.transition.duckTo = 36U;
    Check(EncodePosition(
              "R7/8/8/8/8/8/8/7K b - - 0 1",
              Variant::Duck, 36U, record.position),
          "invalid post-capture Duck relocation fixture encodes");
    return record;
}

Record DuckKingCaptureSeal() {
    Record record = CheckmateSeal();
    record.outcome = Outcome::KingCaptured;
    return record;
}

CommitPointer InstallLine(FakeStorage& storage,
                          const std::vector<Record>& records,
                          const Slot slot = Slot::A) {
    Artifact& artifact = storage.slots[FakeStorage::Index(slot)];
    artifact = {};
    artifact.present = true;
    for (const Record& record : records) {
        std::array<uint8_t, MAX_RECORD_BYTES> encoded{};
        size_t length = 0U;
        Check(EncodeRecord(record, encoded.data(), encoded.size(), length),
              "direct line fixture record encodes");
        artifact.bytes.insert(artifact.bytes.end(), encoded.begin(),
                              encoded.begin() + static_cast<ptrdiff_t>(length));
    }
    CommitPointer pointer;
    pointer.present = true;
    pointer.slot = slot;
    pointer.generation = 1U;
    pointer.byteLength = static_cast<uint32_t>(artifact.bytes.size());
    pointer.prefixCrc32 =
        fxe::Crc32(artifact.bytes.data(), artifact.bytes.size());
    pointer.gameId = records.front().gameId;
    pointer.lineRevision = records.back().lineRevision;
    pointer.currentPly = records.back().type == RecordType::Seal
        ? records[records.size() - 2U].ply
        : records.back().ply;
    pointer.archiveRequired = pointer.currentPly != 0U;
    return pointer;
}

struct Records {
    std::array<Record, 3U> values{};
    uint16_t count = 0U;

    static bool Read(void* const context,
                     const uint16_t index,
                     Record& out) {
        if (context == nullptr) return false;
        auto& self = *static_cast<Records*>(context);
        if (index >= self.count) return false;
        out = self.values[index];
        return true;
    }

    LineSource Source() { return {this, count, &Read}; }
};

CommitPointer Initialize(CurrentGameLog& log,
                         FakeSettings& settings) {
    CommitPointer candidate;
    SettingsOps::Receipt receipt = 0U;
    Check(log.Initialize(Initial(), false, candidate, receipt) ==
              Result::Applied,
          "initial Classic line writes slot A and queues settings");
    Check(candidate.slot == Slot::A && candidate.generation == 1U &&
              candidate.currentPly == 0U && candidate.byteLength == 81U &&
              receipt == 41U && settings.calls == 1U &&
              settings.pending.prefixCrc32 == candidate.prefixCrc32,
          "initial pointer is exact and has tracked receipt");
    Check(log.ConfirmPromotion(candidate, receipt),
          "initial tracked settings promotion opens trusted session");
    return candidate;
}

void TestInitializeCompleteLineUsesOneSettingsPromotion() {
    FakeStorage storage;
    FakeSettings settings;
    CurrentGameLog log(storage.Ops(), settings.Ops());
    Records line;
    line.values[0U] = Initial();
    line.values[1U] = E4();
    line.values[2U] = {};
    line.values[2U].type = RecordType::CommittedPly;
    line.values[2U].outboxSequence = 3U;
    line.values[2U].gameId = Id();
    line.values[2U].lineRevision = 3U;
    line.values[2U].ply = 2U;
    line.values[2U].transition.side = Side::Black;
    line.values[2U].transition.from = 12U;
    line.values[2U].transition.to = 28U;
    Check(EncodePosition(
              "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
              Variant::Classic, INVALID_SQUARE,
              line.values[2U].position),
          "e5 position encodes");
    line.count = 3U;

    CommitPointer candidate;
    SettingsOps::Receipt receipt = 0U;
    Check(log.InitializeLine(
              line.Source(), true, candidate, receipt) == Result::Applied &&
              candidate.currentPly == 2U &&
              candidate.lineRevision == 3U &&
              candidate.archiveRequired && settings.calls == 1U &&
              receipt == 41U,
          "complete reconstructed line queues one exact settings promotion");
    Check(log.ConfirmPromotion(candidate, receipt) &&
              log.Open(candidate) == Result::Ready,
          "complete reconstructed line becomes cold-open authority");
}

void TestReplacementPreservesOldAuthorityUntilPromotion() {
    FakeStorage storage;
    FakeSettings settings;
    CurrentGameLog log(storage.Ops(), settings.Ops());
    const CommitPointer authority = Initialize(log, settings);
    const auto oldBytes = storage.slots[0U].bytes;
    Records replacement;
    replacement.values[0U] = Initial();
    replacement.values[0U].gameId.bytes[0U] ^= 0x40U;
    replacement.count = 1U;

    CommitPointer candidate;
    SettingsOps::Receipt receipt = 0U;
    Check(log.ReplaceLine(authority, replacement.Source(), false,
                          candidate, receipt) == Result::Applied &&
              candidate.slot == Slot::B && candidate.generation == 2U &&
              candidate.gameId == replacement.values[0U].gameId &&
              storage.slots[0U].bytes == oldBytes,
          "replacement writes only the inactive slot before promotion");
    Check(log.RejectPromotion(receipt) &&
              log.Open(authority) == Result::Ready &&
              storage.slots[0U].bytes == oldBytes,
          "failed replacement leaves the old settings authority readable");
}

void TestPointerCodec() {
    FakeStorage storage;
    FakeSettings settings;
    CurrentGameLog log(storage.Ops(), settings.Ops());
    const CommitPointer pointer = Initialize(log, settings);
    StaticJsonDocument<384> document;
    Check(WriteCommitPointer(document, pointer), "V2 pointer writes");
    CommitPointer decoded;
    Check(ReadCommitPointer(document, decoded) && decoded.present &&
              decoded.slot == pointer.slot &&
              decoded.generation == pointer.generation &&
              decoded.byteLength == pointer.byteLength &&
              decoded.prefixCrc32 == pointer.prefixCrc32 &&
              decoded.gameId == pointer.gameId &&
              decoded.lineRevision == pointer.lineRevision &&
              decoded.currentPly == pointer.currentPly &&
              decoded.archiveRequired == pointer.archiveRequired,
          "V2 pointer roundtrips without narrowing");
    document[KEY_CURRENT_GAME_LOG_V2][0U] = 1U;
    Check(!ReadCommitPointer(document, decoded) && !decoded.present,
          "V1-shaped pointer cannot alias V2 authority");
}

void TestPointerWriteFailuresAreAtomic() {
    FakeStorage storage;
    FakeSettings settings;
    CurrentGameLog log(storage.Ops(), settings.Ops());
    const CommitPointer pointer = Initialize(log, settings);
    CommitPointer replacement = pointer;
    replacement.currentPly = 1U;
    replacement.lineRevision = 2U;

    StaticJsonDocument<768> constrained;
    Check(WriteCommitPointer(constrained, pointer),
          "constrained pointer fixture writes");
    JsonArray padding = constrained.createNestedArray("PADDING");
    constexpr size_t required = JSON_OBJECT_SIZE(1) + JSON_ARRAY_SIZE(9) +
                                33U + 17U;
    while (!constrained.overflowed() &&
           constrained.capacity() - constrained.memoryUsage() >= required) {
        padding.add(1U);
    }
    Check(!constrained.overflowed(),
          "tight-capacity fixture remains internally valid");
    std::string before;
    serializeJson(constrained, before);
    Check(!WriteCommitPointer(constrained, replacement),
          "tight capacity rejects pointer replacement");
    std::string after;
    serializeJson(constrained, after);
    CommitPointer retained;
    Check(after == before && ReadCommitPointer(constrained, retained) &&
              retained.currentPly == pointer.currentPly &&
              retained.lineRevision == pointer.lineRevision,
          "tight-capacity failure preserves prior pointer byte-for-byte");

    StaticJsonDocument<512> sticky;
    Check(WriteCommitPointer(sticky, pointer),
          "sticky-overflow pointer fixture writes");
    sticky["TOO_LARGE"] = std::string(2048U, 'x');
    sticky.remove("TOO_LARGE");
    Check(sticky.overflowed() && sticky.memoryUsage() < sticky.capacity(),
          "sticky overflow retains nominal spare arena capacity");
    before.clear();
    serializeJson(sticky, before);
    Check(!WriteCommitPointer(sticky, replacement),
          "sticky overflow rejects before pointer mutation");
    after.clear();
    serializeJson(sticky, after);
    Check(after == before && ReadCommitPointer(sticky, retained) &&
              retained.currentPly == pointer.currentPly,
          "sticky-overflow failure preserves prior pointer");

    StaticJsonDocument<384> wrongRoot;
    JsonArray rootArray = wrongRoot.to<JsonArray>();
    rootArray.add(7U);
    before.clear();
    serializeJson(wrongRoot, before);
    Check(!WriteCommitPointer(wrongRoot, pointer),
          "non-object document rejects pointer write");
    after.clear();
    serializeJson(wrongRoot, after);
    Check(after == before,
          "non-object rejection preserves document bytes");
}

void TestVariantLinePolicy() {
    {
        FakeStorage storage;
        FakeSettings settings;
        CurrentGameLog log(storage.Ops(), settings.Ops());
        CommitPointer candidate;
        SettingsOps::Receipt receipt = 0U;
        Record nonzeroBase = Initial();
        nonzeroBase.ply = 1U;
        Check(log.Initialize(nonzeroBase, false, candidate, receipt) ==
                  Result::Corrupt,
              "Initialize rejects a nonzero base ply");
    }
    {
        FakeStorage storage;
        FakeSettings settings;
        CurrentGameLog log(storage.Ops(), settings.Ops());
        const CommitPointer authority = Initialize(log, settings);
        CommitPointer candidate;
        SettingsOps::Receipt receipt = 0U;
        Check(log.Append(authority, DuckPly(), candidate, receipt) ==
                  Result::Bounds && settings.calls == 1U,
              "Append rejects a Duck position on a Classic line");
        Check(log.Append(authority, ClassicPlyWithDuckTransition(),
                         candidate, receipt) == Result::Bounds &&
                  settings.calls == 1U,
              "Append rejects Duck relocation semantics on a Classic line");
        Records records{{Initial(), DuckPly(), {}}, 2U};
        Check(log.Rewrite(authority, records.Source(), candidate, receipt) ==
                  Result::Corrupt && settings.calls == 1U,
              "Rewrite rejects a Duck position on a Classic line");
        records.values[1U] = ClassicPlyWithDuckTransition();
        Check(log.Rewrite(authority, records.Source(), candidate, receipt) ==
                  Result::Corrupt && settings.calls == 1U,
              "Rewrite rejects Duck transition semantics on a Classic line");
    }
    {
        FakeStorage storage;
        FakeSettings settings;
        CurrentGameLog log(storage.Ops(), settings.Ops());
        const CommitPointer duckBase = InstallLine(storage, {DuckBase()});
        Check(log.Open(duckBase) == Result::Ready,
              "Open accepts a CRC-valid Duck base");
        CurrentGameLog mixed(storage.Ops(), settings.Ops());
        const CommitPointer duckSuffix =
            InstallLine(storage, {Initial(), DuckPly()});
        Check(mixed.Open(duckSuffix) == Result::Corrupt,
              "Open rejects a CRC-valid Duck suffix on a Classic base");
        const CommitPointer semanticDuck = InstallLine(
            storage, {Initial(), ClassicPlyWithDuckTransition()});
        Check(mixed.Open(semanticDuck) == Result::Corrupt,
              "Open rejects CRC-valid Duck semantics with a Classic position");
    }
    {
        FakeStorage storage;
        FakeSettings settings;
        CurrentGameLog log(storage.Ops(), settings.Ops());
        CommitPointer authority;
        SettingsOps::Receipt receipt = 0U;
        Check(log.Initialize(DuckBase(), false, authority, receipt) ==
                  Result::Applied &&
                  log.ConfirmPromotion(authority, receipt),
              "Initialize accepts a valid Duck base");

        Record wrongOrigin = DuckPly();
        wrongOrigin.transition.duckFrom = 23U;
        CommitPointer rejected;
        Check(log.Append(authority, wrongOrigin, rejected, receipt) ==
                  Result::Bounds,
              "Duck append rejects a relocation from the wrong square");
        Record missingRelocation = DuckPly();
        missingRelocation.transition.duckFrom = INVALID_SQUARE;
        missingRelocation.transition.duckTo = INVALID_SQUARE;
        Check(log.Append(authority, missingRelocation,
                         rejected, receipt) == Result::Bounds,
              "nonterminal Duck append cannot omit relocation");
        Record movesOntoDuck = DuckPly();
        movesOntoDuck.transition.to = 24U;
        Check(log.Append(authority, movesOntoDuck,
                         rejected, receipt) == Result::Bounds,
              "Duck append rejects a chess move onto the blocking Duck");

        CommitPointer appended;
        Check(log.Append(authority, DuckPly(), appended, receipt) ==
                  Result::Applied && appended.currentPly == 1U &&
                  log.ConfirmPromotion(appended, receipt),
              "Append accepts the mandatory Duck relocation");
        CurrentGameLog rebooted(storage.Ops(), settings.Ops());
        Check(rebooted.Open(appended) == Result::Ready,
              "Open restores a CRC-valid Duck line");

        Records classicRecords{{Initial(), E4(), {}}, 2U};
        Check(rebooted.Rewrite(appended, classicRecords.Source(),
                              authority, receipt) == Result::Bounds &&
                  !storage.slots[FakeStorage::Index(Slot::B)].present,
              "Rewrite rejects a variant change before touching the inactive slot");
        Records records{{DuckBase(), DuckPly(), {}}, 2U};
        Check(rebooted.Rewrite(appended, records.Source(),
                              authority, receipt) == Result::Applied,
              "Rewrite accepts a complete Duck line");
    }
    {
        FakeStorage storage;
        FakeSettings settings;
        CurrentGameLog log(storage.Ops(), settings.Ops());
        CommitPointer authority;
        SettingsOps::Receipt receipt = 0U;
        Check(log.Initialize(DuckKingCaptureBase(), false,
                             authority, receipt) == Result::Applied &&
                  log.ConfirmPromotion(authority, receipt),
              "terminal Duck fixture initializes");
        const Record relocatedAfterCapture = DuckKingCaptureWithRelocation();
        CommitPointer rejected;
        Check(log.Append(authority, relocatedAfterCapture,
                         rejected, receipt) == Result::Bounds,
              "Duck king capture cannot relocate the Duck afterward");
        Records rejectedLine{
            {DuckKingCaptureBase(), relocatedAfterCapture, {}}, 2U};
        Check(log.Rewrite(authority, rejectedLine.Source(),
                          rejected, receipt) == Result::Corrupt,
              "Rewrite rejects a Duck relocation after king capture");
        FakeStorage persistedInvalid;
        const CommitPointer invalidPointer = InstallLine(
            persistedInvalid,
            {DuckKingCaptureBase(), relocatedAfterCapture});
        CurrentGameLog coldInvalid(
            persistedInvalid.Ops(), settings.Ops());
        Check(coldInvalid.Open(invalidPointer) == Result::Corrupt,
              "Open rejects a CRC-valid Duck relocation after king capture");

        CommitPointer terminalMove;
        Check(log.Append(authority, DuckKingCapturePly(),
                         terminalMove, receipt) == Result::Applied &&
                  log.ConfirmPromotion(terminalMove, receipt),
              "Duck king capture is the only ply allowed to omit relocation");
        Record seal = DuckKingCaptureSeal();
        CommitPointer sealed;
        Check(log.Append(terminalMove, seal, sealed, receipt) ==
                  Result::Applied && log.ConfirmPromotion(sealed, receipt),
              "Duck king-capture line accepts its terminal seal");
        CurrentGameLog rebooted(storage.Ops(), settings.Ops());
        Check(rebooted.Open(sealed) == Result::Ready,
              "Open recovers a sealed Duck king-capture line");

        FakeStorage falseTerminalStorage;
        FakeSettings falseTerminalSettings;
        CurrentGameLog falseTerminal(
            falseTerminalStorage.Ops(), falseTerminalSettings.Ops());
        Check(falseTerminal.Initialize(DuckKingCaptureBase(), false,
                                       authority, receipt) == Result::Applied &&
                  falseTerminal.ConfirmPromotion(authority, receipt),
              "false terminal Duck fixture initializes");
        Record relocatedKing = DuckKingCapturePly();
        Check(EncodePosition(
                  "Rk6/8/8/8/8/8/8/7K b - - 0 1",
                  Variant::Duck, 35U, relocatedKing.position),
              "false terminal result fixture encodes");
        Check(falseTerminal.Append(authority, relocatedKing,
                                   sealed, receipt) == Result::Bounds,
              "omitted relocation cannot masquerade as a surviving king capture");
    }
}

void TestInitializeRequiresFreshSession() {
    {
        FakeStorage storage;
        FakeSettings settings;
        CurrentGameLog log(storage.Ops(), settings.Ops());
        CommitPointer pending;
        SettingsOps::Receipt pendingReceipt = 0U;
        Check(log.Initialize(Initial(), false, pending, pendingReceipt) ==
                  Result::Applied,
              "pending Initialize fixture writes once");
        const std::vector<uint8_t> bytes =
            storage.slots[FakeStorage::Index(Slot::A)].bytes;
        CommitPointer rejected;
        SettingsOps::Receipt rejectedReceipt = 99U;
        Check(log.Initialize(Initial(), false, rejected, rejectedReceipt) ==
                  Result::BadState &&
                  !rejected.present && rejectedReceipt == 0U &&
                  settings.calls == 1U &&
                  storage.slots[FakeStorage::Index(Slot::A)].bytes == bytes &&
                  log.ConfirmPromotion(pending, pendingReceipt),
              "Initialize cannot overwrite pending bytes or receipt authority");
    }
    {
        FakeStorage storage;
        FakeSettings settings;
        CurrentGameLog log(storage.Ops(), settings.Ops());
        const CommitPointer authority = Initialize(log, settings);
        const std::vector<uint8_t> bytes =
            storage.slots[FakeStorage::Index(Slot::A)].bytes;
        CommitPointer rejected;
        SettingsOps::Receipt rejectedReceipt = 99U;
        Check(log.Initialize(Initial(), false, rejected, rejectedReceipt) ==
                  Result::BadState &&
                  !rejected.present && rejectedReceipt == 0U &&
                  settings.calls == 1U &&
                  storage.slots[FakeStorage::Index(Slot::A)].bytes == bytes,
              "Initialize cannot overwrite an open active authority");
        CommitPointer noChange;
        Check(log.AcknowledgeArchived(authority, noChange, rejectedReceipt) ==
                  Result::Ready,
              "rejected Initialize leaves the open authority usable");
    }
}

void TestAppendAndResetBoundaries() {
    FakeStorage storage;
    FakeSettings settings;
    CurrentGameLog log(storage.Ops(), settings.Ops());
    const CommitPointer oldAuthority = Initialize(log, settings);

    CommitPointer appended;
    SettingsOps::Receipt receipt = 0U;
    Check(log.Append(oldAuthority, E4(), appended, receipt) ==
              Result::Applied &&
              receipt == 42U && appended.currentPly == 1U &&
              appended.archiveRequired &&
              appended.byteLength > oldAuthority.byteLength,
          "one committed move is read back before tracked settings receipt");

    CurrentGameLog beforePromotion(storage.Ops(), settings.Ops());
    Check(beforePromotion.Open(oldAuthority) == Result::Ready,
          "reset before promotion keeps old pointer authoritative despite suffix");
    CurrentGameLog afterPromotion(storage.Ops(), settings.Ops());
    Check(afterPromotion.Open(appended) == Result::Ready,
          "reset after promotion restores the appended boundary");
    Check(log.ConfirmPromotion(appended, receipt),
          "durable move receipt advances trusted session without rescan");

    storage.slots[FakeStorage::Index(Slot::A)].bytes.push_back(0xEEU);
    Check(afterPromotion.Open(appended) == Result::Ready,
          "extra bytes beyond committed prefix are ignored");
    CommitPointer terminal;
    Check(log.Append(appended, CheckmateSeal(), terminal, receipt) ==
              Result::Applied && terminal.currentPly == appended.currentPly &&
              terminal.lineRevision == appended.lineRevision &&
              terminal.byteLength ==
                  appended.byteLength + SEAL_RECORD_BYTES &&
              storage.slots[FakeStorage::Index(Slot::A)].bytes.size() ==
                  terminal.byteLength,
          "terminal append truncates an uncommitted suffix before writing");
    Check(log.ConfirmPromotion(terminal, receipt),
          "terminal settings receipt advances trusted session");
    CommitPointer rejected;
    Check(log.Append(terminal, E4(), rejected, receipt) == Result::Bounds,
          "sealed line cannot append another position");
}

void TestAppendFailuresPreserveAuthority() {
    for (const bool partialWrite : {false, true}) {
        FakeStorage storage;
        FakeSettings settings;
        CurrentGameLog log(storage.Ops(), settings.Ops());
        const CommitPointer authority = Initialize(log, settings);
        storage.failAppendCall = storage.appendCalls + 1U;
        storage.writeBeforeAppendFailure = partialWrite;
        CommitPointer candidate;
        SettingsOps::Receipt receipt = 99U;
        Check(log.Append(authority, E4(), candidate, receipt) ==
                  Result::StorageFailure &&
                  settings.calls == 1U && receipt == 0U &&
                  !candidate.present,
              partialWrite
                  ? "uncertain complete append leaves old authority readable"
                  : "failed append leaves old authority readable");
    }

    FakeStorage storage;
    FakeSettings settings;
    CurrentGameLog log(storage.Ops(), settings.Ops());
    const CommitPointer authority = Initialize(log, settings);
    storage.corruptNextAppendReadback = true;
    CommitPointer candidate;
    SettingsOps::Receipt receipt = 99U;
    Check(log.Append(authority, E4(), candidate, receipt) ==
              Result::ReadbackMismatch &&
              settings.calls == 1U && receipt == 0U,
          "readback mismatch cannot queue settings authority");

    FakeStorage admittedStorage;
    FakeSettings admittedSettings;
    CurrentGameLog admittedLog(admittedStorage.Ops(), admittedSettings.Ops());
    const CommitPointer admittedAuthority =
        Initialize(admittedLog, admittedSettings);
    Check(admittedLog.Append(admittedAuthority, E4(), candidate, receipt) ==
              Result::Applied &&
              !admittedLog.RejectPromotion(receipt + 1U) &&
              admittedLog.RejectPromotion(receipt),
          "failed tracked receipt is explicitly rejected by exact token");
    Check(admittedLog.Append(admittedAuthority, E4(), candidate, receipt) ==
              Result::Applied,
          "retry truncates uncommitted bytes after settings failure");
}

void TestRewriteFailureAndSelectedCorruption() {
    FakeStorage storage;
    FakeSettings settings;
    CurrentGameLog log(storage.Ops(), settings.Ops());
    const CommitPointer authority = Initialize(log, settings);
    Records records{{Initial(), E4(), {}}, 2U};

    storage.failReplaceCall = storage.replaceCalls + 1U;
    storage.writeBeforeReplaceFailure = true;
    CommitPointer candidate;
    SettingsOps::Receipt receipt = 0U;
    Check(log.Rewrite(authority, records.Source(), candidate, receipt) == Result::StorageFailure &&
              settings.calls == 1U,
          "interrupted inactive-slot rewrite preserves active authority");

    storage.failReplaceCall = 0U;
    storage.writeBeforeReplaceFailure = false;
    Check(log.Rewrite(authority, records.Source(), candidate, receipt) == Result::Applied &&
              candidate.slot == Slot::B && candidate.generation == 2U,
          "complete rewrite prepares inactive slot before promotion");
    CurrentGameLog verifier(storage.Ops(), settings.Ops());
    Check(verifier.Open(candidate) == Result::Ready, "complete rewrite validates after reset");
    storage.slots[FakeStorage::Index(Slot::B)].bytes[20U] ^= 0x40U;
    Check(verifier.Open(candidate) == Result::Corrupt && verifier.Open(authority) == Result::Ready,
          "corrupt selected slot fails closed despite valid inactive old slot");

    FakeStorage terminalStorage;
    FakeSettings terminalSettings;
    CurrentGameLog terminalLog(terminalStorage.Ops(), terminalSettings.Ops());
    const CommitPointer terminalAuthority = Initialize(terminalLog, terminalSettings);
    records.values[2U] = CheckmateSeal();
    records.count = 3U;
    Check(terminalLog.Rewrite(terminalAuthority, records.Source(), candidate, receipt) ==
                  Result::Applied &&
              candidate.currentPly == 1U && candidate.archiveRequired,
          "inactive rewrite retains a validated terminal result");

    Record invalidSeal = CheckmateSeal();
    invalidSeal.status = ArchiveStatus::Completed;
    invalidSeal.outcome = Outcome::Ongoing;
    invalidSeal.winner = Winner::None;
    records.values[2U] = invalidSeal;
    CurrentGameLog invalidLog(terminalStorage.Ops(), terminalSettings.Ops());
    Check(invalidLog.Open(terminalAuthority) == Result::Ready,
          "invalid rewrite fixture reopens old authority");
    Check(invalidLog.Rewrite(terminalAuthority, records.Source(), candidate, receipt) ==
              Result::Bounds,
          "invalid terminal tuple cannot become current-line authority");
    invalidSeal.status = ArchiveStatus::Unfinished;
    invalidSeal.outcome = Outcome::Ongoing;
    records.values[2U] = invalidSeal;
    Check(invalidLog.Rewrite(terminalAuthority, records.Source(), candidate, receipt) ==
              Result::Corrupt,
          "old multi-game unfinished seal is not a current-line terminal");
}

void TestMaximumLineIncludesTerminalSeal() {
    struct MaximumSource {
        Record base = Initial();
        Record ply = E4();
        Record seal = CheckmateSeal();

        static bool Read(void* const context,
                         const uint16_t index,
                         Record& out) {
            if (context == nullptr || index >= MAX_LINE_RECORDS) return false;
            auto& self = *static_cast<MaximumSource*>(context);
            if (index == 0U) {
                out = self.base;
            } else if (index <= MAX_PLIES) {
                out = self.ply;
                out.outboxSequence = static_cast<uint32_t>(index) + 1U;
                out.lineRevision = static_cast<uint64_t>(index) + 1U;
                out.ply = index;
            } else {
                out = self.seal;
                out.outboxSequence = MAX_LINE_RECORDS;
                out.lineRevision = static_cast<uint64_t>(MAX_PLIES) + 1U;
            }
            return true;
        }
    } source;

    Check(MAX_LINE_RECORDS == 4098U &&
              MAX_SLOT_BYTES ==
                  LINE_BASE_RECORD_BYTES +
                      static_cast<uint32_t>(MAX_PLIES) *
                          MAX_COMMITTED_PLY_RECORD_BYTES +
                      SEAL_RECORD_BYTES,
          "maximum line bound includes base, 4096 plies, and terminal seal");
    FakeStorage storage;
    FakeSettings settings;
    CurrentGameLog log(storage.Ops(), settings.Ops());
    const CommitPointer authority = Initialize(log, settings);
    const LineSource line{&source, MAX_LINE_RECORDS, &MaximumSource::Read};
    CommitPointer candidate;
    SettingsOps::Receipt receipt = 0U;
    Check(log.Rewrite(authority, line, candidate, receipt) ==
              Result::Applied &&
              candidate.currentPly == MAX_PLIES &&
              candidate.byteLength <= MAX_SLOT_BYTES,
          "maximum 4096-ply line plus terminal seal validates");
}

void TestSequentialAppendReadScalingAndArchiveAck() {
    FakeStorage storage;
    FakeSettings settings;
    CurrentGameLog log(storage.Ops(), settings.Ops());
    CommitPointer authority = Initialize(log, settings);
    storage.readCalls = 0U;

    for (uint16_t ply = 1U; ply <= MAX_PLIES; ++ply) {
        Record record = E4();
        record.outboxSequence = static_cast<uint32_t>(ply) + 1U;
        record.lineRevision = static_cast<uint64_t>(ply) + 1U;
        record.ply = ply;
        CommitPointer candidate;
        SettingsOps::Receipt receipt = 0U;
        if (log.Append(authority, record, candidate, receipt) !=
                Result::Applied ||
            !candidate.archiveRequired ||
            !log.ConfirmPromotion(candidate, receipt)) {
            Check(false, "sequential append and promotion succeeds");
            return;
        }
        authority = candidate;
    }
    Check(storage.readCalls == MAX_PLIES,
          "4096 sequential appends perform one exact readback each, not rescans");

    storage.readCalls = 0U;
    CurrentGameLog rebooted(storage.Ops(), settings.Ops());
    Check(rebooted.Open(authority) == Result::Ready &&
              storage.readCalls ==
                  static_cast<uint32_t>(MAX_PLIES + 1U) * 2U,
          "one reboot Open performs one linear crash-validation scan");

    CommitPointer acknowledged;
    SettingsOps::Receipt receipt = 0U;
    Check(rebooted.AcknowledgeArchived(authority, acknowledged, receipt) ==
              Result::Applied &&
              !acknowledged.archiveRequired && receipt != 0U &&
              !rebooted.ConfirmPromotion(acknowledged, receipt + 1U) &&
              rebooted.ConfirmPromotion(acknowledged, receipt),
          "only an explicit durable acknowledgment clears archiveRequired");
    CommitPointer unchanged;
    receipt = 99U;
    Check(rebooted.AcknowledgeArchived(acknowledged, unchanged, receipt) ==
              Result::Ready && receipt == 0U && !unchanged.present,
          "already-acknowledged authority needs no settings write");
    Record seal = CheckmateSeal();
    seal.outboxSequence = static_cast<uint32_t>(MAX_LINE_RECORDS);
    seal.lineRevision = acknowledged.lineRevision;
    CommitPointer terminal;
    Check(rebooted.Append(acknowledged, seal, terminal, receipt) ==
              Result::Applied && terminal.archiveRequired,
          "a later terminal mutation restores archiveRequired monotonically");
}

void TestUndecidedOpeningRetirement() {
    CommitPointer authority;
    Check(CanRetireUndecidedOpening(authority, 0U, true),
          "fresh undecided setup can save without a current line");
    authority.present = true;
    authority.currentPly = 0U;
    authority.lineRevision = 1U;
    Check(CanRetireUndecidedOpening(authority, 0U, true),
          "undecided setup retires the prior empty opening on save");
    Check(!CanRetireUndecidedOpening(authority, 0U, false),
          "invalid non-undecided snapshot cannot retire the opening");
    authority.archiveRequired = true;
    Check(!CanRetireUndecidedOpening(authority, 0U, true),
          "unarchived opening cannot be discarded");
    authority.archiveRequired = false;
    authority.lineRevision = 2U;
    Check(!CanRetireUndecidedOpening(authority, 0U, true),
          "rewound opening with prior history cannot be discarded");
    authority.lineRevision = 1U;
    authority.currentPly = 1U;
    Check(!CanRetireUndecidedOpening(authority, 0U, true),
          "played current line cannot be discarded");
    authority.currentPly = 0U;
    Check(!CanRetireUndecidedOpening(authority, 1U, true),
          "played snapshot is not an undecided opening");
}

void TestChunkMutationFailures() {
    struct ManyRecords {
        static bool Read(void *, uint16_t index, Record &out) {
            out = index == 0U ? Initial() : E4();
            out.outboxSequence = index + 1U;
            out.lineRevision = index + 1U;
            out.ply = index;
            return true;
        }
    } context;
    const LineSource source{&context, 24U, &ManyRecords::Read};
    for (const bool replace : {false, true}) {
        for (const bool partial : {false, true}) {
            FakeStorage storage;
            FakeSettings settings;
            CurrentGameLog log(storage.Ops(), settings.Ops());
            const CommitPointer authority = Initialize(log, settings);
            if (replace)
                storage.failReplaceCall = storage.replaceCalls + 1U;
            else
                storage.failAppendCall = storage.appendCalls + 1U;
            storage.writeBeforeReplaceFailure = true;
            storage.writeBeforeAppendFailure = true;
            storage.partialFailure = partial;
            CommitPointer candidate;
            SettingsOps::Receipt receipt = 0U;
            Check(log.Rewrite(authority, source, candidate, receipt) == Result::StorageFailure &&
                      settings.calls == 1U && !candidate.present,
                  "partial/complete replace or genuine multi-chunk append cannot promote");
            CurrentGameLog verifier(storage.Ops(), settings.Ops());
            Check(verifier.Open(authority) == Result::Ready,
                  "chunk mutation failure preserves prior committed bytes");
            storage.failAppendCall = storage.failReplaceCall = 0U;
            Check(log.Rewrite(authority, source, candidate, receipt) == Result::Applied &&
                      verifier.Open(candidate) == Result::Ready,
                  "subsequent full multi-chunk rewrite recovers and validates");
        }
    }
}


} // namespace

int main() {
    TestInitializeCompleteLineUsesOneSettingsPromotion();
    TestReplacementPreservesOldAuthorityUntilPromotion();
    TestPointerCodec();
    TestPointerWriteFailuresAreAtomic();
    TestVariantLinePolicy();
    TestInitializeRequiresFreshSession();
    TestAppendAndResetBoundaries();
    TestAppendFailuresPreserveAuthority();
    TestRewriteFailureAndSelectedCorruption();
    TestChunkMutationFailures();
    TestMaximumLineIncludesTerminalSeal();
    TestSequentialAppendReadScalingAndArchiveAck();
    TestUndecidedOpeningRetirement();
    if (failures == 0) std::printf("[CURRENT_GAME_LOG] ok\n");
    return failures == 0 ? 0 : 1;
}
