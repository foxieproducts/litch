#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <unity.h>

#include "modules/current_game_sync_transfer.hpp"
#include "modules/sha256.hpp"

namespace {
using modules::current_game_log::CommitPointer;
using namespace modules::current_game_sync;
using modules::game_history::Frame;
using GOpcode = modules::game_history::Opcode;
using modules::game_history::ArchiveStatus;
using modules::game_history::Crc32Accumulator;
using modules::game_history::GameId;
using modules::game_history::INVALID_SQUARE;
using modules::game_history::Record;
using modules::game_history::RecordType;
using modules::game_history::Side;
using modules::game_history::Variant;

struct Storage {
    std::vector<uint8_t> bytes;
    static modules::current_game_log::StorageStatus Stat(void* context,
                                                         modules::current_game_log::Slot,
                                                         uint32_t& length) {
        const auto& self = *static_cast<Storage*>(context);
        length = static_cast<uint32_t>(self.bytes.size());
        return self.bytes.empty() ? modules::current_game_log::StorageStatus::Missing
                                  : modules::current_game_log::StorageStatus::Present;
    }
    static bool Read(void* context, modules::current_game_log::Slot, uint32_t offset, uint8_t* out, size_t length) {
        const auto& bytes = static_cast<Storage*>(context)->bytes;
        if (out == nullptr || offset > bytes.size() || length > bytes.size() - offset)
            return false;
        std::memcpy(out, bytes.data() + offset, length);
        return true;
    }
    modules::current_game_log::StorageOps Ops() {
        return {this, &Stat, &Read, nullptr, nullptr, nullptr, nullptr, nullptr};
    }
};

GameId Game() {
    GameId game;
    for (size_t i = 0U; i < game.bytes.size(); ++i)
        game.bytes[i] = static_cast<uint8_t>(i + 1U);
    return game;
}
CommitPointer Populated(Storage& storage, uint16_t moves, bool archiveRequired = true) {
    uint64_t revision = 100U;
    for (uint16_t index = 0U; index <= moves; ++index) {
        Record record;
        record.type = index == 0U ? RecordType::LineBase : RecordType::CommittedPly;
        record.outboxSequence = static_cast<uint32_t>(index) + 1U;
        record.gameId = Game();
        record.lineRevision = revision++;
        record.ply = index;
        record.variant = Variant::Classic;
        TEST_ASSERT_TRUE(modules::game_history::EncodePosition(
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            Variant::Classic, INVALID_SQUARE, record.position));
        if (index != 0U) {
            record.transition.side = (index & 1U) == 0U ? Side::Black : Side::White;
            record.transition.from = static_cast<uint8_t>(index - 1U);
            record.transition.to = static_cast<uint8_t>(index);
        }
        std::array<uint8_t, modules::game_history::MAX_RECORD_BYTES> encoded{};
        size_t length = 0U;
        TEST_ASSERT_TRUE(modules::game_history::EncodeRecord(record, encoded.data(), encoded.size(), length));
        storage.bytes.insert(storage.bytes.end(), encoded.begin(), encoded.begin() + length);
    }
    Crc32Accumulator crc;
    TEST_ASSERT_TRUE(crc.Update(storage.bytes.data(), storage.bytes.size()));
    CommitPointer pointer;
    pointer.present = true;
    pointer.slot = modules::current_game_log::Slot::A;
    pointer.generation = 7U;
    pointer.byteLength = static_cast<uint32_t>(storage.bytes.size());
    pointer.prefixCrc32 = crc.Value();
    pointer.gameId = Game();
    pointer.lineRevision = revision - 1U;
    pointer.currentPly = moves;
    pointer.archiveRequired = archiveRequired;
    return pointer;
}

modules::current_game_log::StorageStatus EmptyStat(void*, modules::current_game_log::Slot, uint32_t& length) {
    length = 0U;
    return modules::current_game_log::StorageStatus::Missing;
}
bool EmptyRead(void*, modules::current_game_log::Slot, uint32_t, uint8_t*, size_t) {
    return false;
}
modules::current_game_log::StorageOps EmptyStorage() {
    static uint8_t context = 0U;
    modules::current_game_log::StorageOps storage;
    storage.context = &context;
    storage.stat = EmptyStat;
    storage.read = EmptyRead;
    return storage;
}

std::array<uint8_t, 20U> Begin(const uint16_t id) {
    Frame frame;
    frame.opcode = static_cast<uint8_t>(GOpcode::CurrentBegin);
    frame.transactionId = id;
    frame.frameSequence = 0U;
    frame.payloadLength = 12U;
    std::array<uint8_t, 20U> out{};
    size_t length = 0U;
    TEST_ASSERT_TRUE(modules::game_history::EncodeFrame(frame, out.data(), out.size(), length));
    TEST_ASSERT_EQUAL_UINT32(out.size(), length);
    return out;
}

std::array<uint8_t, 20U> BeginAt(const uint16_t id,
                                 const std::vector<uint8_t>& prefix,
                                 const uint32_t nextSequence) {
    Frame frame;
    frame.opcode = static_cast<uint8_t>(GOpcode::CurrentBegin);
    frame.transactionId = id;
    frame.frameSequence = 0U;
    frame.payloadLength = 12U;
    ResumeCursor cursor;
    cursor.nextRecordSequence = nextSequence;
    modules::Sha256 sha;
    TEST_ASSERT_TRUE(sha.Update(prefix.data(), prefix.size()));
    const auto digest = sha.Finish();
    std::copy(digest.begin(), digest.begin() + cursor.prefixFingerprint.size(),
              cursor.prefixFingerprint.begin());
    TEST_ASSERT_TRUE(EncodeResumeCursor(cursor, frame.payload.data()));
    std::array<uint8_t, 20U> out{};
    size_t length = 0U;
    TEST_ASSERT_TRUE(modules::game_history::EncodeFrame(frame, out.data(), out.size(), length));
    return out;
}

std::array<uint8_t, 20U> CompleteBegin(const uint16_t id, const Storage& storage, uint32_t nextSequence) {
    auto bytes = Begin(id);
    modules::Sha256 sha;
    TEST_ASSERT_TRUE(sha.Update(storage.bytes.data(), storage.bytes.size()));
    const auto digest = sha.Finish();
    bytes[8U] = static_cast<uint8_t>(nextSequence);
    bytes[9U] = static_cast<uint8_t>(nextSequence >> 8U);
    bytes[10U] = static_cast<uint8_t>(nextSequence >> 16U);
    bytes[11U] = static_cast<uint8_t>(nextSequence >> 24U);
    std::memcpy(bytes.data() + 12U, digest.data(), 8U);
    return bytes;
}

bool Take(CurrentGameSyncTransfer& transfer, uint32_t epoch, const CommitPointer& pointer, Frame& frame, bool accept = true) {
    std::array<uint8_t, 512U> bytes{};
    size_t length = 0U;
    if (!transfer.NextNotification(epoch, pointer, bytes.data(), bytes.size(), length) ||
        !modules::game_history::DecodeFrame(bytes.data(), length, frame))
        return false;
    return !accept || transfer.NotificationAccepted(epoch, transfer.PendingNotificationToken());
}

std::array<uint8_t, 20U> Commit(uint16_t id, uint16_t sequence, const modules::game_history::SegmentDescriptor& descriptor) {
    Frame frame;
    frame.opcode = static_cast<uint8_t>(GOpcode::CurrentSegmentCommit);
    frame.transactionId = id;
    frame.frameSequence = sequence;
    frame.payloadLength = 12U;
    const modules::game_history::CommitPayload payload{descriptor.firstOutboxSequence,
                                                       descriptor.LastOutboxSequence(), descriptor.crc32};
    TEST_ASSERT_TRUE(modules::game_history::EncodeCommitPayload(payload, frame.payload.data()));
    std::array<uint8_t, 20U> bytes{};
    size_t length = 0U;
    TEST_ASSERT_TRUE(modules::game_history::EncodeFrame(frame, bytes.data(), bytes.size(), length));
    return bytes;
}

std::array<uint8_t, 20U> ArchiveAck(uint16_t id, uint16_t sequence,
                                    const Digest& digest) {
    Frame frame;
    frame.opcode = static_cast<uint8_t>(GOpcode::CurrentArchiveAck);
    frame.transactionId = id;
    frame.frameSequence = sequence;
    frame.payloadLength = 12U;
    TEST_ASSERT_TRUE(EncodeDigest(digest, false, frame.payload.data()));
    std::array<uint8_t, 20U> bytes{};
    size_t length = 0U;
    TEST_ASSERT_TRUE(modules::game_history::EncodeFrame(frame, bytes.data(), bytes.size(), length));
    return bytes;
}

struct StreamedSegment {
    modules::game_history::SegmentDescriptor descriptor{};
    std::vector<uint8_t> bytes;
    uint16_t commitSequence = 0U;
};

StreamedSegment DrainToCommit(CurrentGameSyncTransfer& transfer, uint32_t epoch,
                              const CommitPointer& pointer) {
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, epoch, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentState), frame.opcode);
    TEST_ASSERT_TRUE(Take(transfer, epoch, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentBounds), frame.opcode);
    TEST_ASSERT_TRUE(Take(transfer, epoch, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentSegmentBegin), frame.opcode);
    StreamedSegment result;
    TEST_ASSERT_TRUE(modules::game_history::DecodeSegmentDescriptor(
        frame.payload.data(), frame.payloadLength, result.descriptor));
    do {
        TEST_ASSERT_TRUE(Take(transfer, epoch, pointer, frame));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentData), frame.opcode);
        result.bytes.insert(result.bytes.end(), frame.payload.begin(),
                            frame.payload.begin() + frame.payloadLength);
        result.commitSequence = static_cast<uint16_t>(frame.frameSequence + 1U);
    } while (!transfer.AwaitingCommit());
    TEST_ASSERT_EQUAL_UINT32(result.descriptor.encodedBytes, result.bytes.size());
    return result;
}

void CommitSegment(CurrentGameSyncTransfer& transfer, uint32_t epoch,
                   const CommitPointer& pointer, uint16_t transactionId,
                   const StreamedSegment& segment) {
    const auto commit = Commit(transactionId, segment.commitSequence, segment.descriptor);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleCommit(
                          commit.data(), commit.size(), epoch, true, false, pointer)));
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, epoch, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentSegmentCommitted), frame.opcode);
}

Digest BeginCompleteTransaction(CurrentGameSyncTransfer& transfer, uint32_t epoch,
                                uint16_t transactionId, const Storage& storage,
                                const CommitPointer& pointer) {
    const auto begin = CompleteBegin(transactionId, storage,
                                     static_cast<uint32_t>(pointer.currentPly) + 2U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(
                          begin.data(), begin.size(), epoch, true, false, 20U, pointer)));
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, epoch, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentState), frame.opcode);
    TEST_ASSERT_EQUAL_UINT16(transactionId, frame.transactionId);
    Digest digest;
    std::memcpy(digest.fingerprint.data(), frame.payload.data(), digest.fingerprint.size());
    TEST_ASSERT_TRUE(Take(transfer, epoch, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentBounds), frame.opcode);
    TEST_ASSERT_FALSE(transfer.HasNotification());
    return digest;
}

void TestNoLineFramesAreTokenGatedAndRetryExactly() {
    modules::current_game_log::StorageOps storage = EmptyStorage();
    CurrentGameSyncSource source(storage);
    CurrentGameSyncTransfer transfer(source);
    const CommitPointer none{};
    const auto begin = Begin(7U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 1U, true, false, 20U, none)));
    uint8_t first[20] = {};
    size_t firstLength = 0U;
    TEST_ASSERT_TRUE(transfer.NextNotification(1U, none, first, sizeof(first), firstLength));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentState), first[2U]);
    uint8_t retry[20] = {};
    size_t retryLength = 0U;
    TEST_ASSERT_TRUE(transfer.NextNotification(1U, none, retry, sizeof(retry), retryLength));
    TEST_ASSERT_EQUAL_UINT32(firstLength, retryLength);
    TEST_ASSERT_EQUAL_MEMORY(first, retry, firstLength);
    TEST_ASSERT_FALSE(transfer.NotificationAccepted(2U, transfer.PendingNotificationToken()));
    TEST_ASSERT_FALSE(transfer.NotificationAccepted(1U, 0U));
    TEST_ASSERT_TRUE(transfer.NotificationAccepted(1U, transfer.PendingNotificationToken()));
    TEST_ASSERT_TRUE(transfer.NextNotification(1U, none, first, sizeof(first), firstLength));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentBounds), first[2U]);
}

void TestCompetingAndStaleBeginDoNotReplaceActiveTransaction() {
    modules::current_game_log::StorageOps storage = EmptyStorage();
    CurrentGameSyncSource source(storage);
    CurrentGameSyncTransfer transfer(source);
    const CommitPointer none{};
    const auto first = Begin(7U);
    const auto competing = Begin(8U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted), static_cast<int>(transfer.HandleBegin(first.data(), first.size(), 3U, true, false, 20U, none)));
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Rejected), static_cast<int>(transfer.HandleBegin(competing.data(), competing.size(), 3U, true, false, 20U, none)));
    uint8_t out[20] = {};
    size_t length = 0U;
    TEST_ASSERT_TRUE(transfer.NextNotification(3U, none, out, sizeof(out), length));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::Reject), out[2U]);
    TEST_ASSERT_EQUAL_UINT16(8U, static_cast<uint16_t>(out[4U]) | static_cast<uint16_t>(out[5U] << 8U));
    TEST_ASSERT_TRUE(transfer.NotificationAccepted(3U, transfer.PendingNotificationToken()));
    TEST_ASSERT_TRUE(transfer.NextNotification(3U, none, out, sizeof(out), length));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentState), out[2U]);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Dropped), static_cast<int>(transfer.HandleBegin(first.data(), first.size(), 4U, true, false, 20U, none)));
}

void TestPopulatedLineHoldsFinalDataAndRejectsWrongCommit() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 30U);
    CurrentGameSyncSource source(storage.Ops());
    CurrentGameSyncTransfer transfer(source);
    const auto begin = Begin(41U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 9U,
                                                            true, false, 20U, pointer)));
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, 9U, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentState), frame.opcode);
    TEST_ASSERT_TRUE(Take(transfer, 9U, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentBounds), frame.opcode);
    TEST_ASSERT_TRUE(Take(transfer, 9U, pointer, frame));
    modules::game_history::SegmentDescriptor descriptor;
    TEST_ASSERT_TRUE(modules::game_history::DecodeSegmentDescriptor(
        frame.payload.data(), frame.payloadLength, descriptor));
    TEST_ASSERT_TRUE(descriptor.recordCount < 31U);
    // Each DATA remains pending until its delivery token is accepted. Hold the
    // final one and prove that its otherwise-correct commit is dropped.
    uint16_t deliveredBytes = 0U;
    while (true) {
        TEST_ASSERT_TRUE(Take(transfer, 9U, pointer, frame, false));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentData), frame.opcode);
        deliveredBytes = static_cast<uint16_t>(deliveredBytes + frame.payloadLength);
        if (deliveredBytes == descriptor.encodedBytes) {
            const auto heldCommit = Commit(41U, frame.frameSequence + 1U, descriptor);
            TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Dropped),
                              static_cast<int>(transfer.HandleCommit(heldCommit.data(), heldCommit.size(), 9U,
                                                                     true, false, pointer)));
        }
        const uint32_t token = transfer.PendingNotificationToken();
        TEST_ASSERT_TRUE(transfer.NotificationAccepted(9U, token));
        if (transfer.AwaitingCommit())
            break;
    }
    const auto correct = Commit(41U, frame.frameSequence + 1U, descriptor);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleCommit(correct.data(), correct.size(), 9U,
                                                             true, false, pointer)));
}

void TestStaleNotificationDoesNotCancelPopulatedTransfer() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 1U);
    CurrentGameSyncSource source(storage.Ops());
    CurrentGameSyncTransfer transfer(source);
    const auto begin = Begin(51U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 10U,
                                                            true, false, 20U, pointer)));
    std::array<uint8_t, 20U> bytes{};
    size_t length = 0U;
    TEST_ASSERT_FALSE(transfer.NextNotification(11U, pointer, bytes.data(), bytes.size(), length));
    TEST_ASSERT_TRUE(transfer.NextNotification(10U, pointer, bytes.data(), bytes.size(), length));
    CommitPointer changed = pointer;
    changed.lineRevision++;
    TEST_ASSERT_FALSE(transfer.NextNotification(10U, changed, bytes.data(), bytes.size(), length));
}

void TestCompleteResumeArchiveAdoptionAndReplay() {
    Storage storage;
    const CommitPointer original = Populated(storage, 1U, true);
    CurrentGameSyncSource source(storage.Ops());
    ArchiveOps ops;
    ops.context = &ops;
    ops.admit = [](void*, const CommitPointer&, const Digest&) { return ArchiveAdmission::Accepted; };
    CurrentGameSyncTransfer transfer(source, ops);
    const auto begin = CompleteBegin(61U, storage, 3U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 12U,
                                                            true, false, 20U, original)));
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, 12U, original, frame));
    Digest stateDigest;
    std::memcpy(stateDigest.fingerprint.data(), frame.payload.data(), stateDigest.fingerprint.size());
    TEST_ASSERT_TRUE(Take(transfer, 12U, original, frame));
    Frame ack;
    ack.opcode = static_cast<uint8_t>(GOpcode::CurrentArchiveAck);
    ack.transactionId = 61U;
    ack.frameSequence = 3U;
    ack.payloadLength = 12U;
    TEST_ASSERT_TRUE(EncodeDigest(stateDigest, false, ack.payload.data()));
    std::array<uint8_t, 20U> encoded{};
    size_t length = 0U;
    TEST_ASSERT_TRUE(modules::game_history::EncodeFrame(ack, encoded.data(), encoded.size(), length));
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleArchiveAck(encoded.data(), length, 12U, true,
                                                                 false, original)));
    CommitPointer adopted = original;
    adopted.archiveRequired = false;
    transfer.ArchiveCompleted(12U, transfer.PendingArchiveOperationToken(), true, adopted);
    TEST_ASSERT_TRUE(Take(transfer, 12U, adopted, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentArchived), frame.opcode);
    std::array<uint8_t, 20U> archived{};
    size_t archivedLength = 0U;
    TEST_ASSERT_TRUE(transfer.HandleArchiveAck(encoded.data(), length, 12U, true, false, adopted) ==
                     TransferInputResult::Accepted);
    TEST_ASSERT_TRUE(transfer.NextNotification(12U, adopted, archived.data(), archived.size(), archivedLength));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentArchived), archived[2U]);
    TEST_ASSERT_EQUAL_UINT16(4U, static_cast<uint16_t>(archived[6U]) |
                                     static_cast<uint16_t>(archived[7U] << 8U));
}

void MultiSegmentTransfersEveryExactByteAcrossNewTransactionIds() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 63U);
    TEST_ASSERT_TRUE(storage.bytes.size() > MAX_SEGMENT_BYTES);
    CurrentGameSyncSource source(storage.Ops());
    CurrentGameSyncTransfer transfer(source);
    std::vector<uint8_t> received;
    uint32_t nextSequence = 1U;
    uint16_t transactionId = 70U;
    do {
        const auto begin = received.empty() ? Begin(transactionId)
                                            : BeginAt(transactionId, received, nextSequence);
        TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                          static_cast<int>(transfer.HandleBegin(
                              begin.data(), begin.size(), 20U, true, false, 20U, pointer)));
        const StreamedSegment segment = DrainToCommit(transfer, 20U, pointer);
        TEST_ASSERT_EQUAL_UINT32(nextSequence, segment.descriptor.firstOutboxSequence);
        received.insert(received.end(), segment.bytes.begin(), segment.bytes.end());
        CommitSegment(transfer, 20U, pointer, transactionId, segment);
        nextSequence = segment.descriptor.LastOutboxSequence() + 1U;
        ++transactionId;
    } while (received.size() < storage.bytes.size());
    TEST_ASSERT_EQUAL_UINT32(storage.bytes.size(), received.size());
    TEST_ASSERT_EQUAL_MEMORY(storage.bytes.data(), received.data(), received.size());
}

void ChangedCapacityAndSameIdBeginRejected() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 1U);
    CurrentGameSyncSource source(storage.Ops());
    CurrentGameSyncTransfer transfer(source);
    const auto begin = Begin(71U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 21U,
                                                            true, false, 20U, pointer)));
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Rejected),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 21U,
                                                            true, false, 21U, pointer)));
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, 21U, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::Reject), frame.opcode);
    TEST_ASSERT_EQUAL_UINT16(71U, frame.transactionId);
    TEST_ASSERT_TRUE(Take(transfer, 21U, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentState), frame.opcode);

    CurrentGameSyncSource completedSource(storage.Ops());
    CurrentGameSyncTransfer completed(completedSource);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(completed.HandleBegin(begin.data(), begin.size(), 31U,
                                                             true, false, 20U, pointer)));
    const StreamedSegment segment = DrainToCommit(completed, 31U, pointer);
    CommitSegment(completed, 31U, pointer, 71U, segment);
    const auto changedAfterDone = BeginAt(71U, segment.bytes,
                                          segment.descriptor.LastOutboxSequence() + 1U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Rejected),
                      static_cast<int>(completed.HandleBegin(
                          changedAfterDone.data(), changedAfterDone.size(), 31U,
                          true, false, 20U, pointer)));
}

void PopulatedRejectReplaysIdenticallyUntilAccepted() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 1U);
    CurrentGameSyncSource source(storage.Ops());
    CurrentGameSyncTransfer transfer(source);
    const auto begin = Begin(72U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Rejected),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 22U,
                                                            false, false, 20U, pointer)));
    std::array<uint8_t, 20U> first{};
    std::array<uint8_t, 20U> retry{};
    size_t firstLength = 0U;
    size_t retryLength = 0U;
    TEST_ASSERT_TRUE(transfer.NextNotification(22U, pointer, first.data(), first.size(), firstLength));
    const uint32_t token = transfer.PendingNotificationToken();
    TEST_ASSERT_TRUE(transfer.NextNotification(22U, pointer, retry.data(), retry.size(), retryLength));
    TEST_ASSERT_EQUAL_UINT32(firstLength, retryLength);
    TEST_ASSERT_EQUAL_MEMORY(first.data(), retry.data(), firstLength);
    TEST_ASSERT_TRUE(transfer.NotificationAccepted(22U, token));
    TEST_ASSERT_FALSE(transfer.HasNotification());
}

void BadCommitRejectUsesNextSequence() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 1U);
    CurrentGameSyncSource source(storage.Ops());
    CurrentGameSyncTransfer transfer(source);
    const auto begin = Begin(73U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 23U,
                                                            true, false, 20U, pointer)));
    const StreamedSegment streamed = DrainToCommit(transfer, 23U, pointer);
    auto changed = streamed.descriptor;
    changed.crc32 ^= 1U;
    const auto bad = Commit(73U, streamed.commitSequence, changed);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Rejected),
                      static_cast<int>(transfer.HandleCommit(bad.data(), bad.size(), 23U,
                                                             true, false, pointer)));
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, 23U, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::Reject), frame.opcode);
    TEST_ASSERT_EQUAL_UINT16(streamed.commitSequence + 1U, frame.frameSequence);
    modules::game_history::RejectPayload rejection;
    TEST_ASSERT_TRUE(modules::game_history::DecodeRejectPayload(
        frame.payload.data(), frame.payloadLength, rejection));
    TEST_ASSERT_EQUAL(static_cast<int>(modules::game_history::RejectReason::Crc),
                      static_cast<int>(rejection.reason));
}

void MalformedCommitFlagsRejectAtNextSequence() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 1U);
    CurrentGameSyncSource source(storage.Ops());
    CurrentGameSyncTransfer transfer(source);
    const auto begin = Begin(85U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 34U,
                                                            true, false, 20U, pointer)));
    const StreamedSegment streamed = DrainToCommit(transfer, 34U, pointer);
    auto malformed = Commit(85U, streamed.commitSequence, streamed.descriptor);
    malformed[3U] = 1U;
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Rejected),
                      static_cast<int>(transfer.HandleCommit(
                          malformed.data(), malformed.size(), 34U, true, false, pointer)));
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, 34U, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::Reject), frame.opcode);
    TEST_ASSERT_EQUAL_UINT16(streamed.commitSequence + 1U, frame.frameSequence);
    modules::game_history::RejectPayload rejection;
    TEST_ASSERT_TRUE(modules::game_history::DecodeRejectPayload(
        frame.payload.data(), frame.payloadLength, rejection));
    TEST_ASSERT_EQUAL_UINT16(0U, rejection.expectedFrameSequence);
}

void DuplicateExactAndChangedCommit() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 1U);
    CurrentGameSyncSource source(storage.Ops());
    CurrentGameSyncTransfer transfer(source);
    const auto begin = Begin(74U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 24U,
                                                            true, false, 20U, pointer)));
    const StreamedSegment streamed = DrainToCommit(transfer, 24U, pointer);
    const auto exact = Commit(74U, streamed.commitSequence, streamed.descriptor);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleCommit(exact.data(), exact.size(), 24U,
                                                             true, false, pointer)));
    std::array<uint8_t, 20U> firstReceipt{};
    std::array<uint8_t, 20U> replayedReceipt{};
    size_t firstLength = 0U;
    size_t replayedLength = 0U;
    TEST_ASSERT_TRUE(transfer.NextNotification(24U, pointer, firstReceipt.data(),
                                               firstReceipt.size(), firstLength));
    TEST_ASSERT_TRUE(transfer.NotificationAccepted(24U, transfer.PendingNotificationToken()));
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleCommit(exact.data(), exact.size(), 24U,
                                                             true, false, pointer)));
    TEST_ASSERT_TRUE(transfer.NextNotification(24U, pointer, replayedReceipt.data(),
                                               replayedReceipt.size(), replayedLength));
    TEST_ASSERT_EQUAL_UINT32(firstLength, replayedLength);
    TEST_ASSERT_EQUAL_MEMORY(firstReceipt.data(), replayedReceipt.data(), firstLength);
    TEST_ASSERT_TRUE(transfer.NotificationAccepted(24U, transfer.PendingNotificationToken()));
    auto changedDescriptor = streamed.descriptor;
    changedDescriptor.crc32 ^= 1U;
    const auto changed = Commit(74U, streamed.commitSequence, changedDescriptor);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Rejected),
                      static_cast<int>(transfer.HandleCommit(changed.data(), changed.size(), 24U,
                                                             true, false, pointer)));
}

void DuplicateArchiveAckWhilePending() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 1U, true);
    CurrentGameSyncSource source(storage.Ops());
    uint32_t admissions = 0U;
    ArchiveOps ops{&admissions, [](void* context, const CommitPointer&, const Digest&) {
                       ++*static_cast<uint32_t*>(context);
                       return ArchiveAdmission::Accepted;
                   }};
    CurrentGameSyncTransfer transfer(source, ops);
    const Digest digest = BeginCompleteTransaction(transfer, 25U, 75U, storage, pointer);
    const auto ack = ArchiveAck(75U, 3U, digest);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleArchiveAck(
                          ack.data(), ack.size(), 25U, true, false, pointer)));
    const uint32_t token = transfer.PendingArchiveOperationToken();
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleArchiveAck(
                          ack.data(), ack.size(), 25U, true, false, pointer)));
    TEST_ASSERT_EQUAL_UINT32(1U, admissions);
    TEST_ASSERT_EQUAL_UINT32(token, transfer.PendingArchiveOperationToken());
}

void DuplicateCommitCannotCancelArchivePending() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 1U, true);
    CurrentGameSyncSource source(storage.Ops());
    ArchiveOps ops{nullptr, [](void*, const CommitPointer&, const Digest&) {
                       return ArchiveAdmission::Accepted;
                   }};
    CurrentGameSyncTransfer transfer(source, ops);
    const auto begin = Begin(83U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 32U,
                                                            true, false, 20U, pointer)));
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, 32U, pointer, frame));
    Digest digest;
    std::memcpy(digest.fingerprint.data(), frame.payload.data(), digest.fingerprint.size());
    TEST_ASSERT_TRUE(Take(transfer, 32U, pointer, frame));
    TEST_ASSERT_TRUE(Take(transfer, 32U, pointer, frame));
    modules::game_history::SegmentDescriptor descriptor;
    TEST_ASSERT_TRUE(modules::game_history::DecodeSegmentDescriptor(
        frame.payload.data(), frame.payloadLength, descriptor));
    uint16_t commitSequence = 0U;
    do {
        TEST_ASSERT_TRUE(Take(transfer, 32U, pointer, frame));
        commitSequence = static_cast<uint16_t>(frame.frameSequence + 1U);
    } while (!transfer.AwaitingCommit());
    const auto commit = Commit(83U, commitSequence, descriptor);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleCommit(commit.data(), commit.size(), 32U,
                                                             true, false, pointer)));
    TEST_ASSERT_TRUE(Take(transfer, 32U, pointer, frame));
    const auto ack = ArchiveAck(83U, static_cast<uint16_t>(frame.frameSequence + 1U), digest);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleArchiveAck(
                          ack.data(), ack.size(), 32U, true, false, pointer)));
    const uint32_t archiveToken = transfer.PendingArchiveOperationToken();
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleCommit(commit.data(), commit.size(), 32U,
                                                             true, false, pointer)));
    CommitPointer adopted = pointer;
    adopted.archiveRequired = false;
    transfer.ArchiveCompleted(32U, archiveToken, true, adopted);
    TEST_ASSERT_TRUE(Take(transfer, 32U, adopted, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentArchived), frame.opcode);
}

void NoLineArchiveAckIsRejectedWithoutAdmission() {
    const auto storage = EmptyStorage();
    CurrentGameSyncSource source(storage);
    uint32_t admissions = 0U;
    ArchiveOps ops{&admissions, [](void* context, const CommitPointer&, const Digest&) {
                       ++*static_cast<uint32_t*>(context);
                       return ArchiveAdmission::Accepted;
                   }};
    CurrentGameSyncTransfer transfer(source, ops);
    const CommitPointer none{};
    const auto begin = Begin(84U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(begin.data(), begin.size(), 33U,
                                                            true, false, 20U, none)));
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, 33U, none, frame));
    TEST_ASSERT_TRUE(Take(transfer, 33U, none, frame));
    Digest nonexistent;
    nonexistent.fingerprint[0U] = 1U;
    const auto ack = ArchiveAck(84U, 3U, nonexistent);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Rejected),
                      static_cast<int>(transfer.HandleArchiveAck(
                          ack.data(), ack.size(), 33U, true, false, none)));
    TEST_ASSERT_EQUAL_UINT32(0U, admissions);
    std::array<uint8_t, 20U> first{};
    std::array<uint8_t, 20U> retry{};
    size_t firstLength = 0U;
    size_t retryLength = 0U;
    TEST_ASSERT_TRUE(transfer.NextNotification(33U, none, first.data(), first.size(), firstLength));
    TEST_ASSERT_TRUE(transfer.NextNotification(33U, none, retry.data(), retry.size(), retryLength));
    TEST_ASSERT_EQUAL_UINT32(firstLength, retryLength);
    TEST_ASSERT_EQUAL_MEMORY(first.data(), retry.data(), firstLength);
    TEST_ASSERT_TRUE(modules::game_history::DecodeFrame(first.data(), firstLength, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::Reject), frame.opcode);
    TEST_ASSERT_EQUAL_UINT16(4U, frame.frameSequence);
}

void ArchivedReplyReplaysUntilFreshBegin() {
    Storage storage;
    const CommitPointer original = Populated(storage, 1U, true);
    CurrentGameSyncSource source(storage.Ops());
    uint32_t admissions = 0U;
    ArchiveOps ops{&admissions, [](void* context, const CommitPointer&, const Digest&) {
                       ++*static_cast<uint32_t*>(context);
                       return ArchiveAdmission::Accepted;
                   }};
    CurrentGameSyncTransfer transfer(source, ops);
    const Digest digest = BeginCompleteTransaction(transfer, 26U, 76U, storage, original);
    const auto ack = ArchiveAck(76U, 3U, digest);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleArchiveAck(
                          ack.data(), ack.size(), 26U, true, false, original)));
    CommitPointer adopted = original;
    adopted.archiveRequired = false;
    transfer.ArchiveCompleted(26U, transfer.PendingArchiveOperationToken(), true, adopted);
    std::array<uint8_t, 20U> first{};
    std::array<uint8_t, 20U> retry{};
    size_t firstLength = 0U;
    size_t retryLength = 0U;
    TEST_ASSERT_TRUE(transfer.NextNotification(26U, adopted, first.data(), first.size(), firstLength));
    TEST_ASSERT_TRUE(transfer.NotificationAccepted(26U, transfer.PendingNotificationToken()));
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleArchiveAck(
                          ack.data(), ack.size(), 26U, true, false, adopted)));
    TEST_ASSERT_TRUE(transfer.NextNotification(26U, adopted, retry.data(), retry.size(), retryLength));
    TEST_ASSERT_EQUAL_UINT32(firstLength, retryLength);
    TEST_ASSERT_EQUAL_MEMORY(first.data(), retry.data(), firstLength);
    TEST_ASSERT_TRUE(transfer.NotificationAccepted(26U, transfer.PendingNotificationToken()));

    const Digest freshDigest = BeginCompleteTransaction(transfer, 26U, 77U, storage, adopted);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Dropped),
                      static_cast<int>(transfer.HandleArchiveAck(
                          ack.data(), ack.size(), 26U, true, false, adopted)));

    const auto freshAck = ArchiveAck(77U, 3U, freshDigest);
    for (int attempt = 0; attempt < 2; ++attempt) {
        TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                          static_cast<int>(transfer.HandleArchiveAck(
                              freshAck.data(), freshAck.size(), 26U, true, false, adopted)));
    }
    Frame archived;
    TEST_ASSERT_TRUE(Take(transfer, 26U, adopted, archived, false));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentArchived), archived.opcode);
    TEST_ASSERT_EQUAL_UINT16(77U, archived.transactionId);
    TEST_ASSERT_EQUAL_UINT16(4U, archived.frameSequence);
    TEST_ASSERT_EQUAL_MEMORY(digest.fingerprint.data(), archived.payload.data(),
                             digest.fingerprint.size());

    const auto competingBegin = CompleteBegin(78U, storage, 3U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Rejected),
                      static_cast<int>(transfer.HandleBegin(competingBegin.data(),
                          competingBegin.size(), 26U, true, false, 20U, adopted)));
    TEST_ASSERT_TRUE(transfer.NotificationAccepted(26U, transfer.PendingNotificationToken()));
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Dropped),
                      static_cast<int>(transfer.HandleBegin(competingBegin.data(),
                          competingBegin.size(), 26U, true, false, 20U, adopted)));
    Frame rejection;
    TEST_ASSERT_TRUE(Take(transfer, 26U, adopted, rejection));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::Reject), rejection.opcode);
    TEST_ASSERT_EQUAL_UINT16(78U, rejection.transactionId);

    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Rejected),
                      static_cast<int>(transfer.HandleBegin(competingBegin.data(),
                          competingBegin.size() - 1U, 26U, true, false, 20U, adopted)));
    TEST_ASSERT_TRUE(Take(transfer, 26U, adopted, rejection));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::Reject), rejection.opcode);
    const auto rejectedAck = ArchiveAck(78U, 2U, freshDigest);
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                          static_cast<int>(transfer.HandleArchiveAck(
                              rejectedAck.data(), rejectedAck.size(), 26U, true, false, adopted)));
    TEST_ASSERT_EQUAL_UINT32(1U, admissions);
}

void ColdClearedAuthorityAckReplaysWithoutAdmission() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 1U, false);
    CurrentGameSyncSource source(storage.Ops());
    uint32_t admissions = 0U;
    ArchiveOps ops{&admissions, [](void* context, const CommitPointer&, const Digest&) {
                       ++*static_cast<uint32_t*>(context);
                       return ArchiveAdmission::Accepted;
                   }};
    CurrentGameSyncTransfer transfer(source, ops);
    const Digest digest = BeginCompleteTransaction(transfer, 27U, 77U, storage, pointer);
    const auto ack = ArchiveAck(77U, 3U, digest);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleArchiveAck(
                          ack.data(), ack.size(), 27U, true, false, pointer)));
    TEST_ASSERT_EQUAL_UINT32(0U, admissions);
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, 27U, pointer, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentArchived), frame.opcode);
}

void StaleArchiveCompletionCannotCancelNewTransaction() {
    Storage storage;
    const CommitPointer original = Populated(storage, 1U, true);
    CurrentGameSyncSource source(storage.Ops());
    ArchiveOps ops{nullptr, [](void*, const CommitPointer&, const Digest&) {
                       return ArchiveAdmission::Accepted;
                   }};
    CurrentGameSyncTransfer transfer(source, ops);
    Digest digest = BeginCompleteTransaction(transfer, 28U, 78U, storage, original);
    auto ack = ArchiveAck(78U, 3U, digest);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleArchiveAck(
                          ack.data(), ack.size(), 28U, true, false, original)));
    const uint32_t staleToken = transfer.PendingArchiveOperationToken();
    const auto reset = Begin(79U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Rejected),
                      static_cast<int>(transfer.HandleBegin(reset.data(), reset.size(), 28U,
                                                            false, false, 20U, original)));
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, 28U, original, frame));
    digest = BeginCompleteTransaction(transfer, 28U, 80U, storage, original);
    ack = ArchiveAck(80U, 3U, digest);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleArchiveAck(
                          ack.data(), ack.size(), 28U, true, false, original)));
    const uint32_t currentToken = transfer.PendingArchiveOperationToken();
    TEST_ASSERT_NOT_EQUAL(staleToken, currentToken);
    CommitPointer adopted = original;
    adopted.archiveRequired = false;
    transfer.ArchiveCompleted(28U, staleToken, true, adopted);
    TEST_ASSERT_EQUAL_UINT32(currentToken, transfer.PendingArchiveOperationToken());
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleArchiveAck(
                          ack.data(), ack.size(), 28U, true, false, original)));
}

void PopulatedToNoLineClearsPlan() {
    Storage storage;
    const CommitPointer pointer = Populated(storage, 1U);
    CurrentGameSyncSource source(storage.Ops());
    CurrentGameSyncTransfer transfer(source);
    const auto first = Begin(81U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(first.data(), first.size(), 29U,
                                                            true, false, 20U, pointer)));
    const StreamedSegment segment = DrainToCommit(transfer, 29U, pointer);
    CommitSegment(transfer, 29U, pointer, 81U, segment);
    storage.bytes.clear();
    const CommitPointer none{};
    const auto second = Begin(82U);
    TEST_ASSERT_EQUAL(static_cast<int>(TransferInputResult::Accepted),
                      static_cast<int>(transfer.HandleBegin(second.data(), second.size(), 29U,
                                                            true, false, 20U, none)));
    Frame frame;
    TEST_ASSERT_TRUE(Take(transfer, 29U, none, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentState), frame.opcode);
    TEST_ASSERT_TRUE(Take(transfer, 29U, none, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GOpcode::CurrentBounds), frame.opcode);
    TEST_ASSERT_FALSE(transfer.HasNotification());
}
} // namespace

void setup() {
    UNITY_BEGIN();
    RUN_TEST(TestNoLineFramesAreTokenGatedAndRetryExactly);
    RUN_TEST(TestCompetingAndStaleBeginDoNotReplaceActiveTransaction);
    RUN_TEST(TestPopulatedLineHoldsFinalDataAndRejectsWrongCommit);
    RUN_TEST(TestStaleNotificationDoesNotCancelPopulatedTransfer);
    RUN_TEST(TestCompleteResumeArchiveAdoptionAndReplay);
    RUN_TEST(MultiSegmentTransfersEveryExactByteAcrossNewTransactionIds);
    RUN_TEST(ChangedCapacityAndSameIdBeginRejected);
    RUN_TEST(PopulatedRejectReplaysIdenticallyUntilAccepted);
    RUN_TEST(BadCommitRejectUsesNextSequence);
    RUN_TEST(MalformedCommitFlagsRejectAtNextSequence);
    RUN_TEST(DuplicateExactAndChangedCommit);
    RUN_TEST(DuplicateArchiveAckWhilePending);
    RUN_TEST(DuplicateCommitCannotCancelArchivePending);
    RUN_TEST(NoLineArchiveAckIsRejectedWithoutAdmission);
    RUN_TEST(ArchivedReplyReplaysUntilFreshBegin);
    RUN_TEST(ColdClearedAuthorityAckReplaysWithoutAdmission);
    RUN_TEST(StaleArchiveCompletionCannotCancelNewTransaction);
    RUN_TEST(PopulatedToNoLineClearsPlan);
    UNITY_END();
}
void loop() {}

int main() {
    setup();
    return 0;
}
