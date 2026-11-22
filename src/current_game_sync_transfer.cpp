#include "modules/current_game_sync_transfer.hpp"

#include <algorithm>
#include <cstring>

namespace modules::current_game_sync {
namespace {
using game_history::Frame;
using GOpcode = game_history::Opcode;
using game_history::RejectReason;

uint16_t Read16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0U]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1U]) << 8U);
}
void Write16(uint8_t* out, uint16_t value) {
    out[0U] = static_cast<uint8_t>(value);
    out[1U] = static_cast<uint8_t>(value >> 8U);
}
} // namespace

void CurrentGameSyncTransfer::Abandon() {
    m_source.Close();
    m_phase = Phase::Idle;
    m_connectionEpoch = 0U;
    SetTransactionId(0U);
    m_nextFrameSequence = 0U;
    m_busyTransactionId = 0U;
    m_reject = {};
    m_pendingLength = 0U;
    m_pendingToken = 0U;
}

bool CurrentGameSyncTransfer::SameAuthority(const current_game_log::CommitPointer& a) const {
    const auto& b = m_authority;
    return a.present == b.present && a.slot == b.slot && a.generation == b.generation &&
           a.byteLength == b.byteLength && a.prefixCrc32 == b.prefixCrc32 &&
           a.gameId == b.gameId && a.lineRevision == b.lineRevision &&
           a.currentPly == b.currentPly && a.archiveRequired == b.archiveRequired;
}
bool CurrentGameSyncTransfer::SameBegin(const uint8_t* bytes, size_t length) const {
    return bytes != nullptr && length != 0U && length == m_beginLength &&
           std::memcmp(bytes, m_beginBytes.data(), length) == 0;
}
bool CurrentGameSyncTransfer::SameCommit(const uint8_t* bytes, size_t length) const {
    return bytes != nullptr && length != 0U && length == m_commitLength &&
           std::memcmp(bytes, m_commitBytes.data(), length) == 0;
}
bool CurrentGameSyncTransfer::PlanIsCurrent(const current_game_log::CommitPointer& authority) {
    if (!SameAuthority(authority))
        return false;
    if (m_plan.descriptor.encodedBytes == 0U)
        return true;
    uint8_t byte = 0U;
    size_t length = 0U;
    return m_source.ReadSegmentChunk(authority, m_plan, 0U, &byte, 1U, length) ==
               SourceResult::Ready &&
           length == 1U;
}
void CurrentGameSyncTransfer::SetTransactionId(uint16_t id) {
    if (m_transactionId == id)
        return;
    m_transactionId = id;
    // A successor, including a rejected Begin, cannot inherit the previous line
    // or its acknowledgement.
    m_segmentOffset = 0U;
    m_pendingDataLength = 0U;
    m_snapshot = {};
    m_plan = {};
    m_commit = {};
    m_beginLength = 0U;
    m_commitLength = 0U;
    m_authority = {};
    m_archiveAckSequence = 0U;
    m_archiveOperationToken = 0U;
    m_archiveSucceeded = false;
    m_archivedLength = 0U;
}
bool CurrentGameSyncTransfer::SetReject(uint16_t id, uint8_t opcode, RejectReason reason, uint16_t expected, uint32_t detail) {
    if (id == 0U)
        return false;
    if (id != m_transactionId) {
        m_source.Close();
        SetTransactionId(id);
    }
    m_reject = {reason, opcode, expected, detail};
    m_phase = Phase::Reject;
    return true;
}
bool CurrentGameSyncTransfer::IsArchiveAdoption(
    const current_game_log::CommitPointer& authority) const {
    if (authority.archiveRequired || !m_authority.archiveRequired)
        return false;
    current_game_log::CommitPointer expected = m_authority;
    expected.archiveRequired = false;
    return authority.present == expected.present && authority.slot == expected.slot &&
           authority.generation == expected.generation && authority.byteLength == expected.byteLength &&
           authority.prefixCrc32 == expected.prefixCrc32 && authority.gameId == expected.gameId &&
           authority.lineRevision == expected.lineRevision && authority.currentPly == expected.currentPly;
}
TransferInputResult CurrentGameSyncTransfer::QueueBusy(uint16_t id,
                                                       RejectReason reason) {
    if (m_busyTransactionId != 0U)
        return TransferInputResult::Dropped;
    m_busyTransactionId = id;
    m_reject.reason = reason;
    return TransferInputResult::Rejected;
}

TransferInputResult CurrentGameSyncTransfer::HandleBegin(
    const uint8_t* bytes,
    size_t length,
    uint32_t epoch,
    bool ownerReady,
    bool otaBusy,
    uint16_t capacity,
    const current_game_log::CommitPointer& authority) {
    if (bytes == nullptr || length < game_history::FRAME_HEADER_BYTES ||
        bytes[0U] != game_history::FRAME_MAGIC || epoch == 0U)
        return TransferInputResult::Dropped;
    const uint16_t id = Read16(bytes + 4U);
    if (id == 0U || (m_connectionEpoch != 0U && epoch != m_connectionEpoch))
        return TransferInputResult::Dropped;
    if (!ownerReady || otaBusy) {
        Abandon();
        m_connectionEpoch = epoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(id, bytes[2U], otaBusy ? RejectReason::Busy : RejectReason::BadState, 0U);
        return TransferInputResult::Rejected;
    }
    if (capacity < game_history::MIN_ATT_PAYLOAD_BYTES || capacity > game_history::MAX_FRAME_BYTES) {
        if (m_connectionEpoch == epoch && m_phase != Phase::Idle)
            return QueueBusy(id, RejectReason::Bounds);
        m_connectionEpoch = epoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(id, bytes[2U], RejectReason::Bounds, 0U, capacity);
        return TransferInputResult::Rejected;
    }
    if (m_connectionEpoch == epoch && m_transactionId == id && SameBegin(bytes, length) &&
        capacity == m_frameCapacity) {
        if (m_phase == Phase::Done || m_phase == Phase::Archived || m_phase == Phase::ArchivePending ||
            m_commitLength != 0U)
            return QueueBusy(id);
        if (!PlanIsCurrent(authority)) {
            Abandon();
            return TransferInputResult::Dropped;
        }
        m_phase = Phase::State;
        m_nextFrameSequence = 1U;
        m_segmentOffset = 0U;
        m_pendingDataLength = 0U;
        m_pendingLength = 0U;
        m_pendingToken = 0U;
        return TransferInputResult::Accepted;
    }
    const bool nextTransaction = m_connectionEpoch == epoch && id != m_transactionId &&
                                 m_phase == Phase::Done && m_pendingLength == 0U &&
                                 m_busyTransactionId == 0U;
    if (m_connectionEpoch == epoch && !nextTransaction &&
        (m_phase != Phase::Idle || m_pendingLength != 0U))
        return QueueBusy(id);
    Frame frame;
    if (bytes[1U] != game_history::PROTOCOL_VERSION) {
        m_connectionEpoch = epoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(id, bytes[2U], RejectReason::UnsupportedVersion, 0U, bytes[1U]);
        return TransferInputResult::Rejected;
    }
    if (!game_history::DecodeFrame(bytes, length, frame))
        return TransferInputResult::Dropped;
    if (frame.opcode != static_cast<uint8_t>(GOpcode::CurrentBegin) || frame.flags != 0U ||
        frame.frameSequence != 0U || frame.payloadLength != 12U) {
        m_connectionEpoch = epoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(id, frame.opcode, game_history::IsKnownOpcode(frame.opcode) ? RejectReason::BadState : RejectReason::UnsupportedOpcode, 0U);
        return TransferInputResult::Rejected;
    }
    ResumeCursor cursor;
    if (!DecodeResumeCursor(frame.payload.data(), frame.payloadLength, cursor)) {
        m_connectionEpoch = epoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(id, frame.opcode, RejectReason::Bounds, 0U);
        return TransferInputResult::Rejected;
    }
    SourceSnapshot snapshot;
    const SourceResult opened = m_source.Open(authority, snapshot);
    if (opened == SourceResult::NoLine) {
        m_connectionEpoch = epoch;
        SetTransactionId(id);
        m_frameCapacity = capacity;
        m_authority = authority;
        m_phase = Phase::State;
        m_nextFrameSequence = 1U;
        std::memcpy(m_beginBytes.data(), bytes, length);
        m_beginLength = length;
        return TransferInputResult::Accepted;
    }
    if (opened != SourceResult::Ready) {
        m_connectionEpoch = epoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(id, frame.opcode, opened == SourceResult::IoFailure ? RejectReason::Storage : RejectReason::Bounds, 0U);
        return opened == SourceResult::IoFailure ? TransferInputResult::IoFailure : TransferInputResult::Rejected;
    }
    SegmentPlan plan;
    SourceResult planned = m_source.Plan(authority, cursor, plan);
    if (planned == SourceResult::CursorMismatch)
        planned = m_source.Plan(authority, ResumeCursor{}, plan);
    if (planned != SourceResult::Ready && planned != SourceResult::Complete) {
        m_source.Close();
        m_connectionEpoch = epoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(id, frame.opcode, planned == SourceResult::IoFailure ? RejectReason::Storage : RejectReason::Bounds, 0U);
        return planned == SourceResult::IoFailure ? TransferInputResult::IoFailure : TransferInputResult::Rejected;
    }
    m_connectionEpoch = epoch;
    SetTransactionId(id);
    m_frameCapacity = capacity;
    m_authority = authority;
    m_snapshot = snapshot;
    m_plan = plan;
    m_phase = Phase::State;
    m_nextFrameSequence = 1U;
    m_beginLength = length;
    std::memcpy(m_beginBytes.data(), bytes, length);
    return TransferInputResult::Accepted;
}

TransferInputResult CurrentGameSyncTransfer::HandleCommit(const uint8_t* bytes, size_t length, uint32_t epoch, bool ownerReady, bool otaBusy, const current_game_log::CommitPointer& authority) {
    if (bytes == nullptr || length < game_history::FRAME_HEADER_BYTES || bytes[0U] != game_history::FRAME_MAGIC ||
        epoch != m_connectionEpoch || Read16(bytes + 4U) != m_transactionId || m_transactionId == 0U)
        return TransferInputResult::Dropped;
    if (!ownerReady || otaBusy || !SameAuthority(authority)) {
        Abandon();
        return TransferInputResult::Dropped;
    }
    if (m_pendingLength != 0U)
        return TransferInputResult::Dropped;
    Frame frame;
    if (!game_history::DecodeFrame(bytes, length, frame))
        return TransferInputResult::Dropped;
    if (SameCommit(bytes, length)) {
        if (m_phase == Phase::ArchivePending || m_phase == Phase::Archived ||
            (m_phase == Phase::Done && m_archiveSucceeded))
            return TransferInputResult::Accepted;
        m_phase = Phase::Committed;
        m_nextFrameSequence = static_cast<uint16_t>(frame.frameSequence + 1U);
        return TransferInputResult::Accepted;
    }
    if (m_phase == Phase::ArchivePending || m_phase == Phase::Archived)
        return TransferInputResult::Rejected;
    if (m_phase != Phase::AwaitCommit || frame.opcode != static_cast<uint8_t>(GOpcode::CurrentSegmentCommit) || frame.flags != 0U || frame.frameSequence != m_nextFrameSequence) {
        const uint16_t semanticExpected = m_nextFrameSequence;
        const bool consumedExpected =
            m_phase == Phase::AwaitCommit &&
            frame.opcode == static_cast<uint8_t>(GOpcode::CurrentSegmentCommit) &&
            frame.frameSequence == semanticExpected && frame.frameSequence != UINT16_MAX;
        if (consumedExpected)
            m_nextFrameSequence = static_cast<uint16_t>(frame.frameSequence + 1U);
        (void)SetReject(m_transactionId, frame.opcode, !game_history::IsKnownOpcode(frame.opcode) ? RejectReason::UnsupportedOpcode : RejectReason::Sequence, consumedExpected ? 0U : semanticExpected);
        return TransferInputResult::Rejected;
    }
    game_history::CommitPayload commit;
    if (!game_history::DecodeCommitPayload(frame.payload.data(), frame.payloadLength, commit) || commit.firstOutboxSequence != m_plan.descriptor.firstOutboxSequence || commit.lastOutboxSequence != m_plan.descriptor.LastOutboxSequence() || commit.crc32 != m_plan.descriptor.crc32 || !PlanIsCurrent(authority)) {
        m_nextFrameSequence = static_cast<uint16_t>(frame.frameSequence + 1U);
        (void)SetReject(m_transactionId, frame.opcode, RejectReason::Crc, 0U);
        return TransferInputResult::Rejected;
    }
    m_commit = commit;
    m_commitLength = length;
    std::memcpy(m_commitBytes.data(), bytes, length);
    m_nextFrameSequence = static_cast<uint16_t>(frame.frameSequence + 1U);
    m_phase = Phase::Committed;
    return TransferInputResult::Accepted;
}

TransferInputResult CurrentGameSyncTransfer::HandleArchiveAck(const uint8_t* bytes, size_t length, uint32_t epoch, bool ownerReady, bool otaBusy, const current_game_log::CommitPointer& authority) {
    if (bytes == nullptr || length < game_history::FRAME_HEADER_BYTES || bytes[0U] != game_history::FRAME_MAGIC || epoch != m_connectionEpoch || Read16(bytes + 4U) != m_transactionId)
        return TransferInputResult::Dropped;
    if (!ownerReady || otaBusy || !SameAuthority(authority)) {
        Abandon();
        return TransferInputResult::Dropped;
    }
    Frame frame;
    if (!game_history::DecodeFrame(bytes, length, frame))
        return TransferInputResult::Dropped;
    if (m_phase == Phase::ArchivePending &&
        frame.opcode == static_cast<uint8_t>(GOpcode::CurrentArchiveAck) &&
        frame.frameSequence == m_archiveAckSequence && frame.payloadLength == 12U &&
        std::equal(frame.payload.begin(), frame.payload.begin() + 12U,
                   m_snapshot.digest.fingerprint.begin())) {
        return TransferInputResult::Accepted;
    }
    if (m_archiveSucceeded && frame.opcode == static_cast<uint8_t>(GOpcode::CurrentArchiveAck) &&
        frame.frameSequence == m_archiveAckSequence && frame.payloadLength == 12U &&
        std::equal(frame.payload.begin(), frame.payload.begin() + 12U,
                   m_snapshot.digest.fingerprint.begin())) {
        m_phase = Phase::Archived;
        if (m_pendingLength == 0U && m_archivedLength != 0U &&
            m_lastDeliveryToken != UINT32_MAX) {
            std::memcpy(m_pendingFrame.data(), m_archivedFrame.data(),
                        m_archivedLength);
            m_pendingLength = m_archivedLength;
            m_pendingToken = ++m_lastDeliveryToken;
        }
        return TransferInputResult::Accepted;
    }
    Digest acknowledged;
    if (m_phase != Phase::Done || frame.opcode != static_cast<uint8_t>(GOpcode::CurrentArchiveAck) || frame.flags != 0U || frame.frameSequence != m_nextFrameSequence || !DecodeDigest(frame.payload.data(), frame.payloadLength, false, acknowledged) || acknowledged.fingerprint != m_snapshot.digest.fingerprint) {
        const uint16_t semanticExpected = m_nextFrameSequence;
        const bool consumedExpected =
            m_phase == Phase::Done &&
            frame.opcode == static_cast<uint8_t>(GOpcode::CurrentArchiveAck) &&
            frame.frameSequence == semanticExpected && frame.frameSequence != UINT16_MAX;
        if (consumedExpected)
            m_nextFrameSequence = static_cast<uint16_t>(frame.frameSequence + 1U);
        (void)SetReject(m_transactionId, frame.opcode, RejectReason::BadState,
                        consumedExpected ? 0U : semanticExpected);
        return TransferInputResult::Rejected;
    }
    // A cold complete resume after the settings pointer has already adopted the
    // archive acknowledgement needs no second mutation; it only replays reply.
    if (!authority.archiveRequired) {
        m_archiveAckSequence = frame.frameSequence;
        m_archiveSucceeded = true;
        ++m_nextFrameSequence;
        m_phase = Phase::Archived;
        return TransferInputResult::Accepted;
    }
    if (m_archive.admit == nullptr || m_archive.admit(m_archive.context, m_authority, m_snapshot.digest) != ArchiveAdmission::Accepted)
        return TransferInputResult::Rejected;
    if (m_lastArchiveOperationToken == UINT32_MAX)
        return TransferInputResult::Dropped;
    m_archiveAckSequence = frame.frameSequence;
    m_archiveOperationToken = ++m_lastArchiveOperationToken;
    ++m_nextFrameSequence;
    m_phase = Phase::ArchivePending;
    return TransferInputResult::Accepted;
}

void CurrentGameSyncTransfer::ArchiveCompleted(
    const uint32_t epoch,
    const uint32_t operationToken,
    const bool succeeded,
    const current_game_log::CommitPointer& authority) {
    if (epoch != m_connectionEpoch || operationToken == 0U ||
        operationToken != m_archiveOperationToken ||
        m_phase != Phase::ArchivePending)
        return;
    if (!succeeded || !IsArchiveAdoption(authority)) {
        Abandon();
        return;
    }
    m_authority = authority;
    m_archiveSucceeded = true;
    m_phase = Phase::Archived;
}

bool CurrentGameSyncTransfer::BuildFrame(GOpcode opcode, const uint8_t* payload, uint16_t payloadLength, uint8_t* out, size_t capacity, size_t& length) {
    length = game_history::FRAME_HEADER_BYTES + payloadLength;
    if (out == nullptr || m_nextFrameSequence == UINT16_MAX || length > capacity || length > m_frameCapacity || length > game_history::MAX_FRAME_BYTES) {
        length = 0U;
        return false;
    }
    out[0U] = game_history::FRAME_MAGIC;
    out[1U] = game_history::PROTOCOL_VERSION;
    out[2U] = static_cast<uint8_t>(opcode);
    out[3U] = 0U;
    Write16(out + 4U, m_transactionId);
    Write16(out + 6U, m_nextFrameSequence++);
    if (payloadLength != 0U)
        std::memcpy(out + 8U, payload, payloadLength);
    return true;
}
bool CurrentGameSyncTransfer::PrepareNotification(uint8_t* out, size_t capacity, size_t& length) {
    uint8_t payload[12] = {};
    if (m_busyTransactionId != 0U) {
        game_history::RejectPayload busy{m_reject.reason, static_cast<uint8_t>(GOpcode::CurrentBegin), 0U, 0U};
        if (!game_history::EncodeRejectPayload(busy, payload) || capacity < 16U)
            return false;
        out[0U] = game_history::FRAME_MAGIC;
        out[1U] = game_history::PROTOCOL_VERSION;
        out[2U] = static_cast<uint8_t>(GOpcode::Reject);
        out[3U] = 0U;
        Write16(out + 4U, m_busyTransactionId);
        Write16(out + 6U, 1U);
        std::memcpy(out + 8U, payload, 8U);
        length = 16U;
        m_busyTransactionId = 0U;
        return true;
    }
    switch (m_phase) {
        case Phase::State:
            if (!EncodeDigest(m_snapshot.digest, true, payload) || !BuildFrame(GOpcode::CurrentState, payload, 12U, out, capacity, length))
                return false;
            m_phase = Phase::Bounds;
            return true;
        case Phase::Bounds:
            if (!EncodeBounds(m_snapshot.bounds, payload) || !BuildFrame(GOpcode::CurrentBounds, payload, 12U, out, capacity, length))
                return false;
            m_phase = m_plan.descriptor.encodedBytes != 0U ? Phase::SegmentBegin : Phase::Done;
            return true;
        case Phase::SegmentBegin:
            if (!game_history::EncodeSegmentDescriptor(m_plan.descriptor, payload) || !BuildFrame(GOpcode::CurrentSegmentBegin, payload, 12U, out, capacity, length))
                return false;
            m_phase = Phase::Data;
            return true;
        case Phase::Data: {
            const size_t stride = m_frameCapacity - game_history::FRAME_HEADER_BYTES;
            size_t read = 0U;
            if (stride == 0U || m_segmentOffset >= m_plan.descriptor.encodedBytes || m_source.ReadSegmentChunk(m_authority, m_plan, m_segmentOffset, out + 8U, stride, read) != SourceResult::Ready || read == 0U || m_nextFrameSequence == UINT16_MAX)
                return false;
            out[0U] = game_history::FRAME_MAGIC;
            out[1U] = game_history::PROTOCOL_VERSION;
            out[2U] = static_cast<uint8_t>(GOpcode::CurrentData);
            out[3U] = 0U;
            Write16(out + 4U, m_transactionId);
            Write16(out + 6U, m_nextFrameSequence++);
            length = 8U + read;
            m_pendingDataLength = static_cast<uint16_t>(read);
            return true;
        }
        case Phase::Committed:
            if (!game_history::EncodeCommitPayload(m_commit, payload) || !BuildFrame(GOpcode::CurrentSegmentCommitted, payload, 12U, out, capacity, length))
                return false;
            m_phase = Phase::Done;
            return true;
        case Phase::Archived:
            if (!EncodeDigest(m_snapshot.digest, false, payload) || !BuildFrame(GOpcode::CurrentArchived, payload, 12U, out, capacity, length))
                return false;
            std::memcpy(m_archivedFrame.data(), out, length);
            m_archivedLength = length;
            m_phase = Phase::Done;
            return true;
        case Phase::Reject:
            if (!game_history::EncodeRejectPayload(m_reject, payload) || !BuildFrame(GOpcode::Reject, payload, 8U, out, capacity, length))
                return false;
            m_phase = Phase::Done;
            return true;
        default:
            return false;
    }
}
bool CurrentGameSyncTransfer::NextNotification(uint32_t epoch, const current_game_log::CommitPointer& authority, uint8_t* out, size_t capacity, size_t& length) {
    length = 0U;
    if (epoch == 0U || epoch != m_connectionEpoch || out == nullptr) {
        return false;
    }
    // A stale callback must not be able to tear down work owned by the current
    // connection. A same-epoch authority replacement does invalidate it.
    const bool rejectionPending =
        m_pendingLength != 0U &&
        m_pendingFrame[2U] == static_cast<uint8_t>(GOpcode::Reject);
    if (!SameAuthority(authority) && m_phase != Phase::Reject && !rejectionPending) {
        Abandon();
        return false;
    }
    if (m_pendingLength == 0U) {
        if (m_lastDeliveryToken == UINT32_MAX || !PrepareNotification(m_pendingFrame.data(), m_pendingFrame.size(), m_pendingLength))
            return false;
        m_pendingToken = ++m_lastDeliveryToken;
    }
    if (capacity < m_pendingLength)
        return false;
    std::memcpy(out, m_pendingFrame.data(), m_pendingLength);
    length = m_pendingLength;
    return true;
}
bool CurrentGameSyncTransfer::NotificationAccepted(uint32_t epoch, uint32_t token) {
    if (epoch != m_connectionEpoch || token == 0U || token != m_pendingToken || m_pendingLength == 0U)
        return false;
    if (m_pendingDataLength != 0U) {
        m_segmentOffset = static_cast<uint16_t>(m_segmentOffset + m_pendingDataLength);
        m_pendingDataLength = 0U;
        if (m_segmentOffset == m_plan.descriptor.encodedBytes)
            m_phase = Phase::AwaitCommit;
    }
    if (m_pendingFrame[2U] == static_cast<uint8_t>(GOpcode::CurrentArchived))
        m_phase = Phase::Done;
    m_pendingLength = 0U;
    m_pendingToken = 0U;
    return true;
}
bool CurrentGameSyncTransfer::HasNotification() const {
    return m_pendingLength != 0U || m_busyTransactionId != 0U || m_phase == Phase::State || m_phase == Phase::Bounds || m_phase == Phase::SegmentBegin || m_phase == Phase::Data || m_phase == Phase::Committed || m_phase == Phase::Archived || m_phase == Phase::Reject;
}
bool CurrentGameSyncTransfer::AwaitingCommit() const {
    return m_phase == Phase::AwaitCommit && m_pendingLength == 0U;
}
} // namespace modules::current_game_sync
