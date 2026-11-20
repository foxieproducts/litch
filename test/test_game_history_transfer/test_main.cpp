#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "modules/game_history_transfer.hpp"

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
    size_t stateWrites = 0U;

    static size_t Index(const JournalArtifact value) {
        return static_cast<size_t>(value);
    }

    static JournalStorageStatus Stat(void* const context,
                                     const JournalArtifact artifact,
                                     uint32_t& outSize) {
        const auto& self = *static_cast<FakeStorage*>(context);
        const Artifact& value = self.artifacts[Index(artifact)];
        if (!value.present) return JournalStorageStatus::Missing;
        outSize = static_cast<uint32_t>(value.bytes.size());
        return JournalStorageStatus::Present;
    }

    static bool Read(void* const context,
                     const JournalArtifact artifact,
                     const uint32_t offset,
                     uint8_t* const out,
                     const size_t length) {
        const auto& self = *static_cast<FakeStorage*>(context);
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
        if (bytes == nullptr && length != 0U) return false;
        if (artifact == JournalArtifact::StateA || artifact == JournalArtifact::StateB) {
            ++self.stateWrites;
        }
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
        if (bytes == nullptr || length == 0U) return false;
        Artifact& value = self.artifacts[Index(artifact)];
        value.present = true;
        value.bytes.insert(value.bytes.end(), bytes, bytes + length);
        return true;
    }

    static bool Promote(void* const context,
                        const JournalArtifact from,
                        const JournalArtifact to) {
        auto& self = *static_cast<FakeStorage*>(context);
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
};

GameId Id(const uint8_t value) {
    GameId id;
    id.bytes[0U] = value;
    id.bytes[15U] = 0xA5U;
    return id;
}

Position PositionFor(const bool afterE4 = false) {
    Position position;
    Check(EncodePosition(
              afterE4
                  ? "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/"
                    "RNBQKBNR b KQkq e3 0 1"
                  : "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/"
                    "RNBQKBNR w KQkq - 0 1",
              Variant::Classic, INVALID_SQUARE, position),
          "transfer position encodes");
    return position;
}

Record Base(const uint32_t sequence, const uint8_t game) {
    Record record;
    record.type = RecordType::LineBase;
    record.outboxSequence = sequence;
    record.gameId = Id(game);
    record.lineRevision = 1U;
    record.variant = Variant::Classic;
    record.position = PositionFor();
    return record;
}

Record Move(const uint32_t sequence, const uint8_t game) {
    Record record;
    record.type = RecordType::CommittedPly;
    record.outboxSequence = sequence;
    record.gameId = Id(game);
    record.lineRevision = 2U;
    record.ply = 1U;
    record.position = PositionFor(true);
    record.transition.side = Side::White;
    record.transition.from = 52U;
    record.transition.to = 36U;
    return record;
}

Record End(const uint32_t sequence, const uint8_t game) {
    Record record;
    record.type = RecordType::Seal;
    record.outboxSequence = sequence;
    record.gameId = Id(game);
    record.lineRevision = 3U;
    record.status = ArchiveStatus::Unfinished;
    record.outcome = Outcome::Ongoing;
    record.winner = Winner::None;
    return record;
}

std::array<uint8_t, MIN_ATT_PAYLOAD_BYTES> BeginFrame(
    const uint16_t transactionId,
    const uint64_t journalId,
    const uint32_t ack) {
    BeginPayload request{journalId, ack};
    uint8_t payload[12] = {};
    Check(EncodeBeginPayload(request, payload), "begin payload encodes");
    Frame frame;
    frame.opcode = static_cast<uint8_t>(Opcode::Begin);
    frame.transactionId = transactionId;
    frame.payloadLength = sizeof(payload);
    std::memcpy(frame.payload.data(), payload, sizeof(payload));
    std::array<uint8_t, MIN_ATT_PAYLOAD_BYTES> encoded{};
    size_t length = 0U;
    Check(EncodeFrame(frame, encoded.data(), encoded.size(), length) &&
              length == encoded.size(),
          "begin frame encodes");
    return encoded;
}

std::array<uint8_t, MIN_ATT_PAYLOAD_BYTES> CommitFrame(
    const uint16_t transactionId,
    const uint16_t frameSequence,
    const SegmentDescriptor& descriptor) {
    CommitPayload commit{descriptor.firstOutboxSequence,
                         descriptor.LastOutboxSequence(),
                         descriptor.crc32};
    uint8_t payload[12] = {};
    Check(EncodeCommitPayload(commit, payload), "commit payload encodes");
    Frame frame;
    frame.opcode = static_cast<uint8_t>(Opcode::Commit);
    frame.transactionId = transactionId;
    frame.frameSequence = frameSequence;
    frame.payloadLength = sizeof(payload);
    std::memcpy(frame.payload.data(), payload, sizeof(payload));
    std::array<uint8_t, MIN_ATT_PAYLOAD_BYTES> encoded{};
    size_t length = 0U;
    Check(EncodeFrame(frame, encoded.data(), encoded.size(), length),
          "commit frame encodes");
    return encoded;
}

struct StreamedTransaction {
    StatePayload state{};
    StateBoundsPayload bounds{};
    SegmentDescriptor descriptor{};
    std::vector<uint8_t> data;
    uint16_t commitSequence = 0U;
    uint8_t stateFlags = 0U;
};

bool TakeNotification(GameHistoryTransfer& transfer, const uint32_t epoch,
                      uint8_t* const out, const size_t capacity, size_t& length) {
    if (!transfer.NextNotification(epoch, out, capacity, length)) return false;
    return transfer.NotificationAccepted(epoch, transfer.PendingNotificationToken());
}

StreamedTransaction DrainToCommit(GameHistoryTransfer& transfer,
                                  const uint32_t epoch,
                                  const uint16_t frameCapacity) {
    StreamedTransaction streamed;
    std::array<uint8_t, MAX_FRAME_BYTES> bytes{};
    uint16_t expectedSequence = 1U;
    while (transfer.HasNotification()) {
        size_t length = 0U;
        Check(TakeNotification(transfer, epoch, bytes.data(), bytes.size(),
                                        length),
              "next transfer notification builds");
        Check(length <= frameCapacity,
              "notification respects negotiated frame capacity");
        Frame frame;
        Check(DecodeFrame(bytes.data(), length, frame),
              "notification decodes");
        Check(frame.frameSequence == expectedSequence++,
              "notification sequence is contiguous");
        switch (static_cast<Opcode>(frame.opcode)) {
            case Opcode::State:
                streamed.stateFlags = frame.flags;
                Check(DecodeStatePayload(frame.payload.data(),
                                         frame.payloadLength,
                                         streamed.state),
                      "state payload decodes");
                break;
            case Opcode::StateBounds:
                Check(DecodeStateBoundsPayload(frame.payload.data(),
                                               frame.payloadLength,
                                               streamed.bounds),
                      "state bounds decode");
                break;
            case Opcode::SegmentBegin:
                Check(DecodeSegmentDescriptor(frame.payload.data(),
                                              frame.payloadLength,
                                              streamed.descriptor),
                      "segment descriptor decodes");
                break;
            case Opcode::Data:
                if (streamed.data.size() + frame.payloadLength <
                    streamed.descriptor.encodedBytes) {
                    Check(frame.payloadLength ==
                              frameCapacity - FRAME_HEADER_BYTES,
                          "non-final DATA uses negotiated stride");
                }
                streamed.data.insert(streamed.data.end(),
                                     frame.payload.begin(),
                                     frame.payload.begin() +
                                         frame.payloadLength);
                break;
            default:
                Check(false, "unexpected pre-commit notification");
                break;
        }
    }
    streamed.commitSequence = expectedSequence;
    return streamed;
}

void TestPendingNotificationRetriesIdentically() {
    FakeStorage fake;
    auto journal = fake.Journal();
    Check(journal.Open(0x8181U) == JournalOpenResult::Initialized &&
              journal.Append(Base(1U, 1U)) == JournalMutationResult::Applied,
          "backpressure fixture initializes");
    GameHistoryTransfer transfer(journal);
    const auto begin = BeginFrame(0x8181U, 0U, 0U);
    Check(transfer.HandleBegin(begin.data(), begin.size(), 51U, true, false, 20U) ==
              TransferInputResult::Accepted, "backpressure BEGIN accepted");
    std::array<uint8_t, MAX_FRAME_BYTES> first{};
    std::array<uint8_t, MAX_FRAME_BYTES> retry{};
    size_t firstLength = 0U;
    size_t retryLength = 0U;
    Check(transfer.NextNotification(51U, first.data(), first.size(), firstLength) &&
              transfer.NextNotification(51U, retry.data(), retry.size(), retryLength) &&
              firstLength == retryLength && first == retry,
          "unaccepted notification retries exact bytes and sequence");
    const uint32_t oldToken = transfer.PendingNotificationToken();
    Check(!transfer.NotificationAccepted(50U, oldToken) &&
              transfer.NotificationAccepted(51U, oldToken),
          "only matching epoch acceptance consumes prepared frame");
    SegmentDescriptor descriptor;
    size_t dataBytes = 0U;
    for (unsigned attempt = 0U; attempt < 32U && transfer.HasNotification(); ++attempt) {
        Check(transfer.NextNotification(51U, first.data(), first.size(), firstLength),
              "next backpressured frame prepares");
        Frame frame;
        Check(DecodeFrame(first.data(), firstLength, frame), "prepared frame decodes");
        if (frame.opcode == static_cast<uint8_t>(Opcode::SegmentBegin)) {
            Check(DecodeSegmentDescriptor(frame.payload.data(), frame.payloadLength, descriptor),
                  "backpressure descriptor decodes");
        }
        if (frame.opcode == static_cast<uint8_t>(Opcode::Data)) {
            dataBytes += frame.payloadLength;
            if (dataBytes == descriptor.encodedBytes) {
                const auto commit = CommitFrame(0x8181U, frame.frameSequence + 1U, descriptor);
                Check(!transfer.AwaitingCommit() &&
                          transfer.HandleCommit(commit.data(), commit.size(), 51U, true, false) ==
                              TransferInputResult::Dropped && journal.Summary().durableAck == 0U,
                      "unaccepted final DATA never authorizes first COMMIT");
                Check(transfer.NextNotification(51U, retry.data(), retry.size(), retryLength) &&
                          retryLength == firstLength &&
                          std::memcmp(first.data(), retry.data(), firstLength) == 0,
                      "final DATA retries exact bytes under backpressure");
                Check(transfer.NotificationAccepted(51U, transfer.PendingNotificationToken()) &&
                          transfer.AwaitingCommit() &&
                          transfer.HandleCommit(commit.data(), commit.size(), 51U, true, false) ==
                              TransferInputResult::Accepted,
                      "queue acceptance enables normal durable COMMIT");
                break;
            }
        }
        Check(transfer.NotificationAccepted(51U, transfer.PendingNotificationToken()),
              "accepted intermediate frame advances once");
    }
    Check(dataBytes == 81U, "backpressure test reaches complete journal segment");
    transfer.Abandon();
    Check(!transfer.HasNotification() &&
              transfer.HandleBegin(begin.data(), begin.size(), 52U, true, false, 20U) ==
                  TransferInputResult::Accepted &&
              transfer.NextNotification(52U, first.data(), first.size(), firstLength) &&
              !transfer.NotificationAccepted(52U, oldToken) &&
              transfer.HasNotification(),
          "teardown clears output and stale acceptance cannot consume a new frame");
}

void TestMinimumMtuTransactionAndDuplicateCommit() {
    FakeStorage fake;
    auto journal = fake.Journal();
    constexpr uint64_t JOURNAL_ID = 0x0102030405060708ULL;
    Check(journal.Open(JOURNAL_ID) == JournalOpenResult::Initialized &&
              journal.Append(Base(1U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(Move(2U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(End(3U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(Base(4U, 2U)) ==
                  JournalMutationResult::Applied,
          "transfer journal prepares two games");

    GameHistoryTransfer transfer(journal);
    const auto begin = BeginFrame(0x1234U, 0U, 0U);
    Check(transfer.HandleBegin(begin.data(), begin.size(), 7U, true, false,
                               MIN_ATT_PAYLOAD_BYTES) ==
              TransferInputResult::Accepted,
          "owner-ready first-contact BEGIN is accepted");
    const auto competitor = BeginFrame(0x9999U, 0U, 0U);
    Check(transfer.HandleBegin(competitor.data(), competitor.size(), 6U, true, false,
                               20U) == TransferInputResult::Dropped,
          "stale BEGIN cannot replace current connection owner");
    Check(transfer.HandleBegin(competitor.data(), competitor.size(), 7U, true, false,
                               20U) == TransferInputResult::Rejected,
          "competing transaction reports busy without taking ownership");
    std::array<uint8_t, MAX_FRAME_BYTES> busyBytes{};
    size_t busyLength = 0U;
    Frame busyFrame;
    Check(TakeNotification(transfer, 7U, busyBytes.data(), busyBytes.size(), busyLength) &&
              DecodeFrame(busyBytes.data(), busyLength, busyFrame) &&
              busyFrame.transactionId == 0x9999U &&
              busyFrame.opcode == static_cast<uint8_t>(Opcode::Reject),
          "competing BEGIN gets its own correlated rejection");
    StreamedTransaction streamed =
        DrainToCommit(transfer, 7U, MIN_ATT_PAYLOAD_BYTES);
    Check((streamed.stateFlags & STATE_FLAG_CURSOR_RECONCILED) != 0U &&
              (streamed.stateFlags & STATE_FLAG_JOURNAL_MATCHED) == 0U &&
              (streamed.stateFlags & STATE_FLAG_SEGMENT_FOLLOWS) != 0U &&
              streamed.state.journalId == JOURNAL_ID &&
              streamed.bounds.newestAllocated == 4U,
          "split state truthfully advertises first-contact journal bounds");
    Check(streamed.descriptor.firstOutboxSequence == 1U &&
              streamed.descriptor.recordCount == 3U &&
              streamed.data.size() == streamed.descriptor.encodedBytes,
          "first transaction stops at one sealed game");
    Crc32Accumulator crc;
    Check(crc.Update(streamed.data.data(), streamed.data.size()) &&
              crc.Value() == streamed.descriptor.crc32,
          "streamed DATA matches planned segment CRC");

    const auto commit = CommitFrame(0x1234U, streamed.commitSequence,
                                    streamed.descriptor);
    Check(transfer.HandleCommit(commit.data(), commit.size(), 7U,
                                true, false) ==
              TransferInputResult::Accepted &&
              journal.Summary().durableAck == 3U,
          "matching COMMIT durably advances board ACK before response");
    const auto changedBegin = BeginFrame(0x1234U, JOURNAL_ID, 3U);
    Check(transfer.HandleBegin(changedBegin.data(), changedBegin.size(), 7U, true, false, 20U) ==
              TransferInputResult::Rejected &&
              TakeNotification(transfer, 7U, busyBytes.data(), busyBytes.size(), busyLength) &&
              DecodeFrame(busyBytes.data(), busyLength, busyFrame) &&
              busyFrame.opcode == static_cast<uint8_t>(Opcode::Reject),
          "reused completed id cannot replace a pending COMMITTED response");
    std::array<uint8_t, MAX_FRAME_BYTES> response{};
    size_t responseLength = 0U;
    Check(TakeNotification(transfer, 7U, response.data(), response.size(),
                                    responseLength),
          "COMMITTED response is available");
    Frame committed;
    Check(DecodeFrame(response.data(), responseLength, committed) &&
              committed.opcode == static_cast<uint8_t>(Opcode::Committed) &&
              committed.frameSequence == streamed.commitSequence + 1U,
          "COMMITTED echoes the transaction after durability");

    Check(transfer.HandleCommit(commit.data(), commit.size(), 7U,
                                true, false) ==
              TransferInputResult::Accepted &&
              TakeNotification(transfer, 7U, response.data(), response.size(),
                                        responseLength) &&
              DecodeFrame(response.data(), responseLength, committed) &&
              committed.frameSequence == streamed.commitSequence + 1U,
          "identical duplicate COMMIT retransmits identical COMMITTED");

    Check(transfer.HandleBegin(begin.data(), begin.size(), 7U, true, false, 20U) ==
              TransferInputResult::Rejected &&
              TakeNotification(transfer, 7U, response.data(), response.size(), responseLength) &&
              DecodeFrame(response.data(), responseLength, committed) &&
              committed.opcode == static_cast<uint8_t>(Opcode::Reject) &&
              !transfer.HasNotification(),
          "completed BEGIN cannot reopen its transaction");
    Check(journal.ReclaimAcknowledged() == JournalMutationResult::Applied,
          "ordinary compaction removes acknowledged game");
    const size_t writes = fake.stateWrites;
    Check(transfer.HandleCommit(commit.data(), commit.size(), 7U, true, false) ==
              TransferInputResult::Accepted && fake.stateWrites == writes &&
              TakeNotification(transfer, 7U, response.data(), response.size(), responseLength),
          "durable exact receipt survives same-journal compaction without another write");

    const auto secondBegin = BeginFrame(0x1235U, JOURNAL_ID, 3U);
    Check(transfer.HandleBegin(secondBegin.data(), secondBegin.size(), 7U,
                               true, false,
                               MIN_ATT_PAYLOAD_BYTES) ==
              TransferInputResult::Accepted,
          "fresh transaction requests the next segment");
    streamed = DrainToCommit(transfer, 7U, MIN_ATT_PAYLOAD_BYTES);
    Check((streamed.stateFlags & STATE_FLAG_JOURNAL_MATCHED) != 0U &&
              streamed.descriptor.firstOutboxSequence == 4U &&
              streamed.descriptor.recordCount == 1U,
          "one-segment transaction ends before the next transaction");
}

void TestBeginReconcilesSafeAppCursor() {
    FakeStorage fake;
    auto journal = fake.Journal();
    constexpr uint64_t JOURNAL_ID = 0x1112131415161718ULL;
    Check(journal.Open(JOURNAL_ID) == JournalOpenResult::Initialized &&
              journal.Append(Base(1U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(Move(2U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(End(3U, 1U)) ==
                  JournalMutationResult::Applied,
          "cursor reconciliation fixture prepares journal");
    GameHistoryTransfer transfer(journal);
    const auto begin = BeginFrame(0x2222U, JOURNAL_ID, 2U);
    Check(transfer.HandleBegin(begin.data(), begin.size(), 8U, true, false,
                               64U) == TransferInputResult::Accepted &&
              journal.Summary().durableAck == 2U,
          "repeated app durable cursor closes lost-COMMIT window");
    const StreamedTransaction streamed = DrainToCommit(transfer, 8U, 64U);
    Check((streamed.stateFlags & STATE_FLAG_JOURNAL_MATCHED) != 0U &&
              (streamed.stateFlags & STATE_FLAG_CURSOR_RECONCILED) != 0U &&
              streamed.descriptor.flags ==
                  SEGMENT_FLAG_ACKNOWLEDGED_OVERLAP &&
              streamed.descriptor.firstOutboxSequence == 2U &&
              streamed.descriptor.LastOutboxSequence() == 3U,
          "remaining seal uses the ACKed ply as overlap base");
}

void TestAuthorizationOtaAndEpochFailures() {
    FakeStorage fake;
    auto journal = fake.Journal();
    Check(journal.Open(0x3031323334353637ULL) ==
              JournalOpenResult::Initialized &&
              journal.Append(Base(1U, 1U)) ==
                  JournalMutationResult::Applied,
          "security fixture prepares journal");
    GameHistoryTransfer transfer(journal);
    const auto begin = BeginFrame(0x3333U, 0U, 0U);
    Check(transfer.HandleBegin(begin.data(), begin.size(), 9U, false, false,
                               20U) == TransferInputResult::Rejected &&
              journal.Summary().durableAck == 0U,
          "unowned client cannot begin or prune history");
    std::array<uint8_t, MAX_FRAME_BYTES> response{};
    size_t length = 0U;
    Check(TakeNotification(transfer, 9U, response.data(), response.size(),
                                    length),
          "unauthorized BEGIN receives typed rejection");
    Frame rejected;
    RejectPayload reason;
    Check(DecodeFrame(response.data(), length, rejected) &&
              rejected.opcode == static_cast<uint8_t>(Opcode::Reject) &&
              DecodeRejectPayload(rejected.payload.data(),
                                  rejected.payloadLength, reason) &&
              reason.reason == RejectReason::BadState,
          "authorization rejection is explicit");

    transfer.Abandon(); // Connection owner tears down epoch 9 before epoch 10.
    Check(transfer.HandleBegin(begin.data(), begin.size(), 10U, true, true,
                               20U) == TransferInputResult::Rejected &&
              TakeNotification(transfer, 10U, response.data(), response.size(),
                                        length) &&
              DecodeFrame(response.data(), length, rejected) &&
              DecodeRejectPayload(rejected.payload.data(),
                                  rejected.payloadLength, reason) &&
              reason.reason == RejectReason::Busy,
          "OTA priority rejects history as busy");

    transfer.Abandon(); // New connection epoch requires explicit lifecycle teardown.
    Check(transfer.HandleBegin(begin.data(), begin.size(), 11U, true, false,
                               20U) == TransferInputResult::Accepted &&
              !TakeNotification(transfer, 12U, response.data(), response.size(),
                                         length),
          "stale connection epoch cannot drain notifications");
    transfer.Abandon();
    Check(!transfer.HasNotification() && !transfer.AwaitingCommit(),
          "disconnect abandonment clears only RAM transaction state");

    Check(transfer.HandleBegin(begin.data(), begin.size(), 13U, true, false,
                               20U) == TransferInputResult::Accepted &&
              transfer.HandleBegin(begin.data(), begin.size(), 13U,
                                   false, false, 20U) ==
                  TransferInputResult::Rejected &&
              TakeNotification(transfer, 13U, response.data(), response.size(),
                                        length) &&
              DecodeFrame(response.data(), length, rejected) &&
              rejected.opcode == static_cast<uint8_t>(Opcode::Reject) &&
              rejected.frameSequence == 1U &&
              DecodeRejectPayload(rejected.payload.data(),
                                  rejected.payloadLength, reason) &&
              reason.reason == RejectReason::BadState &&
              reason.expectedFrameSequence == 0U,
          "owner loss abandons cached BEGIN before typed rejection");
    Check(transfer.HandleBegin(begin.data(), begin.size(), 13U, true, false,
                               20U) == TransferInputResult::Accepted,
          "authorization rejection requires a fresh BEGIN");
}

void TestCommitRejectionsUseResponseSequence() {
    FakeStorage fake;
    auto journal = fake.Journal();
    constexpr uint64_t JOURNAL_ID = 0x4142434445464748ULL;
    Check(journal.Open(JOURNAL_ID) == JournalOpenResult::Initialized &&
              journal.Append(Base(1U, 1U)) ==
                  JournalMutationResult::Applied,
          "commit rejection fixture prepares journal");
    GameHistoryTransfer transfer(journal);
    const auto begin = BeginFrame(0x4444U, 0U, 0U);
    Check(transfer.HandleBegin(begin.data(), begin.size(), 21U, true, false,
                               20U) == TransferInputResult::Accepted,
          "commit rejection transaction begins");
    const StreamedTransaction streamed = DrainToCommit(transfer, 21U, 20U);
    SegmentDescriptor changed = streamed.descriptor;
    changed.crc32 ^= 1U;
    const auto badCommit = CommitFrame(0x4444U, streamed.commitSequence,
                                       changed);
    Check(transfer.HandleCommit(badCommit.data(), badCommit.size(), 21U,
                                true, false) ==
              TransferInputResult::Rejected,
          "mismatched commit is rejected");
    std::array<uint8_t, MAX_FRAME_BYTES> response{};
    size_t length = 0U;
    Frame rejected;
    RejectPayload reason;
    Check(TakeNotification(transfer, 21U, response.data(), response.size(),
                                    length) &&
              DecodeFrame(response.data(), length, rejected) &&
              rejected.frameSequence == streamed.commitSequence + 1U &&
              DecodeRejectPayload(rejected.payload.data(),
                                  rejected.payloadLength, reason) &&
              reason.reason == RejectReason::Crc &&
              reason.expectedFrameSequence == 0U &&
              journal.Summary().durableAck == 0U,
          "expected-sequence commit reject is observable at incoming plus one");

    const auto freshBegin = BeginFrame(0x4445U, 0U, 0U);
    Check(transfer.HandleBegin(freshBegin.data(), freshBegin.size(), 21U,
                               true, false, 20U) ==
              TransferInputResult::Accepted,
          "semantic commit failure reports fresh-BEGIN recovery");
    const StreamedTransaction retry = DrainToCommit(transfer, 21U, 20U);
    const auto commit = CommitFrame(0x4445U, retry.commitSequence,
                                    retry.descriptor);
    Check(transfer.HandleCommit(commit.data(), commit.size(), 21U,
                                false, false) ==
              TransferInputResult::Rejected &&
              TakeNotification(transfer, 21U, response.data(), response.size(),
                                        length) &&
              DecodeFrame(response.data(), length, rejected) &&
              rejected.opcode == static_cast<uint8_t>(Opcode::Reject) &&
              rejected.frameSequence == retry.commitSequence + 1U &&
              DecodeRejectPayload(rejected.payload.data(),
                                  rejected.payloadLength, reason) &&
              reason.reason == RejectReason::BadState &&
              reason.expectedFrameSequence == 0U &&
              journal.Summary().durableAck == 0U,
          "owner loss abandons cached COMMIT before replay");

    const auto otaBegin = BeginFrame(0x4446U, 0U, 0U);
    Check(transfer.HandleBegin(otaBegin.data(), otaBegin.size(), 21U,
                               true, false, 20U) ==
              TransferInputResult::Accepted,
          "OTA rejection transaction begins fresh");
    const StreamedTransaction otaStream = DrainToCommit(transfer, 21U, 20U);
    const auto otaCommit = CommitFrame(0x4446U, otaStream.commitSequence,
                                       otaStream.descriptor);
    Check(transfer.HandleCommit(otaCommit.data(), otaCommit.size(), 21U,
                                true, true) ==
              TransferInputResult::Rejected &&
              TakeNotification(transfer, 21U, response.data(), response.size(),
                                        length) &&
              DecodeFrame(response.data(), length, rejected) &&
              rejected.opcode == static_cast<uint8_t>(Opcode::Reject) &&
              rejected.frameSequence == otaStream.commitSequence + 1U &&
              DecodeRejectPayload(rejected.payload.data(),
                                  rejected.payloadLength, reason) &&
              reason.reason == RejectReason::Busy &&
              reason.expectedFrameSequence == 0U,
          "OTA abandons cached COMMIT before typed rejection");
}

void TestHeldBusyDoesNotBlockActiveCommit() {
    FakeStorage fake;
    auto journal = fake.Journal();
    Check(journal.Open(0xA1U) == JournalOpenResult::Initialized &&
              journal.Append(Base(1U, 1U)) == JournalMutationResult::Applied,
          "held Busy fixture initializes");
    GameHistoryTransfer transfer(journal);
    const auto begin = BeginFrame(0xA1U, 0U, 0U);
    const auto competitor = BeginFrame(0xA2U, 0U, 0U);
    Check(transfer.HandleBegin(begin.data(), begin.size(), 71U, true, false, 20U) ==
              TransferInputResult::Accepted, "active transaction begins before Busy");
    const auto streamed = DrainToCommit(transfer, 71U, 20U);
    const auto commit = CommitFrame(0xA1U, streamed.commitSequence, streamed.descriptor);
    std::array<uint8_t, MAX_FRAME_BYTES> bytes{};
    size_t length = 0U;
    Check(transfer.HandleBegin(competitor.data(), competitor.size(), 71U, true, false, 20U) ==
              TransferInputResult::Rejected &&
              transfer.NextNotification(71U, bytes.data(), bytes.size(), length),
          "competitor Busy is prepared but not accepted");
    const uint32_t token = transfer.PendingNotificationToken();
    Check(transfer.AwaitingCommit() &&
              transfer.HandleCommit(commit.data(), commit.size(), 71U, true, false) ==
                  TransferInputResult::Accepted && journal.Summary().durableAck == 1U,
          "independent Busy backpressure cannot block active durable COMMIT");
    Frame frame;
    Check(transfer.NotificationAccepted(71U, token) &&
              TakeNotification(transfer, 71U, bytes.data(), bytes.size(), length) &&
              DecodeFrame(bytes.data(), length, frame) &&
              frame.transactionId == 0xA1U &&
              frame.opcode == static_cast<uint8_t>(Opcode::Committed),
          "active COMMITTED follows accepted independent Busy response");
}

void TestCompletedEmptyAndReplacedReceipt() {
    FakeStorage fake;
    auto journal = fake.Journal();
    Check(journal.Open(0x9191U) == JournalOpenResult::Initialized,
          "empty transaction fixture initializes");
    GameHistoryTransfer transfer(journal);
    const auto begin = BeginFrame(0x9191U, 0U, 0U);
    Check(transfer.HandleBegin(begin.data(), begin.size(), 61U, true, false, 20U) ==
              TransferInputResult::Accepted, "empty BEGIN accepted");
    const auto empty = DrainToCommit(transfer, 61U, 20U);
    Check((empty.stateFlags & STATE_FLAG_NO_RECORDS) != 0U &&
              transfer.HandleBegin(begin.data(), begin.size(), 61U, true, false, 20U) ==
                  TransferInputResult::Rejected,
          "completed NoRecords BEGIN cannot reopen without a receipt");
    transfer.Abandon();
    Check(journal.Append(Base(1U, 1U)) == JournalMutationResult::Applied &&
              transfer.HandleBegin(begin.data(), begin.size(), 62U, true, false, 20U) ==
                  TransferInputResult::Accepted, "receipt replacement fixture begins");
    const auto streamed = DrainToCommit(transfer, 62U, 20U);
    const auto commit = CommitFrame(0x9191U, streamed.commitSequence, streamed.descriptor);
    Check(transfer.HandleCommit(commit.data(), commit.size(), 62U, true, false) ==
              TransferInputResult::Accepted, "original receipt becomes durable");
    std::array<uint8_t, MAX_FRAME_BYTES> response{};
    size_t length = 0U;
    Check(TakeNotification(transfer, 62U, response.data(), response.size(), length),
          "original receipt response accepted");
    fake.artifacts = {};
    Check(journal.Open(0x9292U) == JournalOpenResult::Initialized &&
              journal.Append(Base(1U, 2U)) == JournalMutationResult::Applied &&
              transfer.HandleCommit(commit.data(), commit.size(), 62U, true, false) ==
                  TransferInputResult::Rejected && journal.Summary().durableAck == 0U &&
              TakeNotification(transfer, 62U, response.data(), response.size(), length),
          "old durable receipt cannot authorize replacement journal and reports rejection");
}

void TestFirstCommitFencesReopenedAndReplacedJournal() {
    for (const bool replaced : {false, true}) {
        FakeStorage fake;
        auto journal = fake.Journal();
        Check(journal.Open(0x7171U) == JournalOpenResult::Initialized &&
                  journal.Append(Base(1U, 1U)) == JournalMutationResult::Applied,
              "commit fence fixture initializes");
        GameHistoryTransfer transfer(journal);
        const auto begin = BeginFrame(0x7777U, 0U, 0U);
        Check(transfer.HandleBegin(begin.data(), begin.size(), 41U, true, false, 20U) ==
                  TransferInputResult::Accepted,
              "commit fence BEGIN accepted");
        const auto streamed = DrainToCommit(transfer, 41U, 20U);
        const auto commit = CommitFrame(0x7777U, streamed.commitSequence, streamed.descriptor);
        if (replaced) fake.artifacts = {};
        Check(journal.Open(replaced ? 0x7272U : 0x7171U) ==
                  (replaced ? JournalOpenResult::Initialized : JournalOpenResult::Ready),
              "journal changes layout or identity after final DATA");
        if (replaced) Check(journal.Append(Base(1U, 2U)) == JournalMutationResult::Applied,
                            "replacement uses same sequence for different game");
        Check(transfer.HandleCommit(commit.data(), commit.size(), 41U, true, false) ==
                  TransferInputResult::Rejected && journal.Summary().durableAck == 0U,
              "stale first COMMIT cannot acknowledge changed layout or replacement journal");
        std::array<uint8_t, MAX_FRAME_BYTES> response{};
        size_t length = 0U;
        Frame rejected;
        Check(transfer.NextNotification(41U, response.data(), response.size(), length) &&
                  DecodeFrame(response.data(), length, rejected) &&
                  rejected.opcode == static_cast<uint8_t>(Opcode::Reject),
              "stale plan returns correlated recovery rejection");
        const uint32_t token = transfer.PendingNotificationToken();
        std::array<uint8_t, MAX_FRAME_BYTES> retry{};
        size_t retryLength = 0U;
        Check(transfer.NextNotification(41U, retry.data(), retry.size(), retryLength) &&
                  length == retryLength && response == retry &&
                  transfer.PendingNotificationToken() == token &&
                  transfer.NotificationAccepted(41U, token),
              "stale-plan rejection retries exact bytes and token until accepted");
    }
}

void TestCompactionInvalidatesTransferPlan() {
    FakeStorage fake;
    auto journal = fake.Journal();
    constexpr uint64_t JOURNAL_ID = 0x5152535455565758ULL;
    Check(journal.Open(JOURNAL_ID) == JournalOpenResult::Initialized &&
              journal.Append(Base(1U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(Move(2U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(End(3U, 1U)) ==
                  JournalMutationResult::Applied &&
              journal.Append(Base(4U, 2U)) ==
                  JournalMutationResult::Applied &&
              journal.AdvanceDurableAck(3U) ==
                  JournalMutationResult::Applied,
          "compaction fixture retains an acknowledged prefix");
    GameHistoryTransfer transfer(journal);
    const auto begin = BeginFrame(0x5555U, JOURNAL_ID, 3U);
    Check(transfer.HandleBegin(begin.data(), begin.size(), 31U, true, false,
                               20U) == TransferInputResult::Accepted,
          "transfer plans record after acknowledged prefix");
    std::array<uint8_t, MAX_FRAME_BYTES> response{};
    size_t length = 0U;
    for (uint8_t index = 0U; index < 3U; ++index) {
        Check(TakeNotification(transfer, 31U, response.data(), response.size(),
                                        length),
              "transfer emits metadata before DATA");
    }
    Check(journal.ReclaimAcknowledged() == JournalMutationResult::Applied &&
              journal.Summary().oldestRetained == 4U &&
              !TakeNotification(transfer, 31U, response.data(), response.size(),
                                         length) &&
              !transfer.HasNotification() && !transfer.AwaitingCommit() &&
              journal.Summary().durableAck == 3U,
          "journal compaction invalidates lazy offsets and aborts transfer");

    const auto retry = BeginFrame(0x5556U, JOURNAL_ID, 3U);
    Check(transfer.HandleBegin(retry.data(), retry.size(), 31U, true, false,
                               20U) == TransferInputResult::Accepted,
          "fresh BEGIN replans after compaction");
    const StreamedTransaction streamed = DrainToCommit(transfer, 31U, 20U);
    Check(streamed.descriptor.firstOutboxSequence == 4U &&
              streamed.descriptor.recordCount == 1U,
          "replanned transfer reads the compacted journal safely");
}

} // namespace

int main() {
    std::puts("[GAME_HISTORY_TRANSFER] start");
    TestMinimumMtuTransactionAndDuplicateCommit();
    TestPendingNotificationRetriesIdentically();
    TestFirstCommitFencesReopenedAndReplacedJournal();
    TestCompletedEmptyAndReplacedReceipt();
    TestHeldBusyDoesNotBlockActiveCommit();
    TestBeginReconcilesSafeAppCursor();
    TestAuthorizationOtaAndEpochFailures();
    TestCommitRejectionsUseResponseSequence();
    TestCompactionInvalidatesTransferPlan();

    if (g_failures == 0) {
        std::puts("[GAME_HISTORY_TRANSFER] ok");
        return 0;
    }
    std::printf("[GAME_HISTORY_TRANSFER] failures=%d\n", g_failures);
    return 1;
}
