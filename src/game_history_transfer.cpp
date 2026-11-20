#include "modules/game_history_transfer.hpp"

#include <cstring>

namespace modules::game_history {
namespace {

uint16_t ReadLe16(const uint8_t* const bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
}

} // namespace

TransferInputResult GameHistoryTransfer::HandleBegin(
    const uint8_t* const bytes,
    const size_t length,
    const uint32_t connectionEpoch,
    const bool ownerReady,
    const bool otaBusy,
    const uint16_t frameCapacity) {
    if (bytes == nullptr || length < FRAME_HEADER_BYTES ||
        bytes[0U] != FRAME_MAGIC) {
        return TransferInputResult::Dropped;
    }
    const uint16_t transactionId = ReadLe16(bytes + 4U);
    if (transactionId == 0U || connectionEpoch == 0U) {
        return TransferInputResult::Dropped;
    }
    if (m_connectionEpoch != 0U && connectionEpoch != m_connectionEpoch) {
        return TransferInputResult::Dropped;
    }
    if (!ownerReady || otaBusy) {
        Abandon();
        m_connectionEpoch = connectionEpoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(transactionId, bytes[2U],
                        otaBusy ? RejectReason::Busy
                                : RejectReason::BadState,
                        0U);
        return TransferInputResult::Rejected;
    }
    if (connectionEpoch == m_connectionEpoch && transactionId == m_transactionId &&
        m_beginLength != 0U && (m_phase == Phase::Done || m_commitLength != 0U)) {
        return QueueBusy(transactionId, RejectReason::BadState);
    }
    if (connectionEpoch == m_connectionEpoch &&
        transactionId == m_transactionId && SameBegin(bytes, length)) {
        if (!PlanIsCurrent()) {
            Abandon();
            m_connectionEpoch = connectionEpoch;
            m_nextFrameSequence = 1U;
            (void)SetReject(transactionId, bytes[2U], RejectReason::UnavailableWatermark, 0U);
            return TransferInputResult::Rejected;
        }
        m_phase = Phase::State;
        m_nextFrameSequence = 1U;
        m_segmentOffset = 0U;
        m_pendingLength = 0U;
        m_pendingToken = 0U;
        return TransferInputResult::Accepted;
    }
    if (connectionEpoch == m_connectionEpoch &&
        ((m_phase != Phase::Idle && m_phase != Phase::Done) || m_pendingLength != 0U)) {
        return QueueBusy(transactionId, RejectReason::Busy);
    }
    if (bytes[1U] != PROTOCOL_VERSION) {
        m_connectionEpoch = connectionEpoch;
        m_nextFrameSequence = 1U;
        return SetReject(transactionId, bytes[2U],
                         RejectReason::UnsupportedVersion, 0U,
                         bytes[1U])
                   ? TransferInputResult::Rejected
                   : TransferInputResult::Dropped;
    }

    Frame frame;
    if (!DecodeFrame(bytes, length, frame)) {
        return TransferInputResult::Dropped;
    }
    if (frame.opcode != static_cast<uint8_t>(Opcode::Begin) ||
        frame.flags != 0U || frame.frameSequence != 0U ||
        frame.payloadLength != 12U) {
        m_connectionEpoch = connectionEpoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(transactionId, frame.opcode,
                        IsKnownOpcode(frame.opcode)
                            ? RejectReason::BadState
                            : RejectReason::UnsupportedOpcode,
                        0U);
        return TransferInputResult::Rejected;
    }
    if (frameCapacity < MIN_ATT_PAYLOAD_BYTES ||
        frameCapacity > MAX_FRAME_BYTES) {
        m_connectionEpoch = connectionEpoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(transactionId, frame.opcode,
                        RejectReason::Bounds, 0U, frameCapacity);
        return TransferInputResult::Rejected;
    }
    BeginPayload begin;
    if (!DecodeBeginPayload(frame.payload.data(), frame.payloadLength,
                            begin)) {
        m_connectionEpoch = connectionEpoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(transactionId, frame.opcode,
                        RejectReason::Bounds, 0U);
        return TransferInputResult::Rejected;
    }
    const JournalSummary before = m_journal.Summary();
    const bool journalMatched = begin.journalId == before.journalId;
    uint32_t effectiveAck = 0U;
    if (journalMatched) {
        if (begin.appDurableAck > before.newestAllocated) {
            m_connectionEpoch = connectionEpoch;
            m_nextFrameSequence = 1U;
            (void)SetReject(transactionId, frame.opcode,
                            RejectReason::UnavailableWatermark, 0U,
                            begin.appDurableAck);
            return TransferInputResult::Rejected;
        }
        effectiveAck = begin.appDurableAck;
        if (effectiveAck > before.durableAck &&
            m_journal.AdvanceDurableAck(effectiveAck) !=
                JournalMutationResult::Applied) {
            m_connectionEpoch = connectionEpoch;
            m_nextFrameSequence = 1U;
            (void)SetReject(transactionId, frame.opcode,
                            RejectReason::Storage, 0U);
            return TransferInputResult::IoFailure;
        }
    }

    const JournalSegmentRead planned =
        m_journal.PlanSegmentAfter(effectiveAck, m_plan);
    if (planned == JournalSegmentRead::Unavailable ||
        planned == JournalSegmentRead::Corrupt) {
        m_connectionEpoch = connectionEpoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(transactionId, frame.opcode,
                        RejectReason::UnavailableWatermark, 0U,
                        m_journal.Summary().oldestRetained);
        return TransferInputResult::Rejected;
    }
    if (planned == JournalSegmentRead::IoFailure ||
        planned == JournalSegmentRead::Bounds) {
        m_connectionEpoch = connectionEpoch;
        m_nextFrameSequence = 1U;
        (void)SetReject(transactionId, frame.opcode,
                        RejectReason::Storage, 0U);
        return TransferInputResult::IoFailure;
    }

    const JournalSummary summary = m_journal.Summary();
    m_connectionEpoch = connectionEpoch;
    m_transactionId = transactionId;
    m_frameCapacity = frameCapacity;
    m_nextFrameSequence = 1U;
    m_segmentOffset = 0U;
    m_state = {summary.journalId, summary.oldestRetained};
    m_bounds = {summary.newestAllocated, summary.durableAck,
                summary.lostGameCount};
    m_stateFlags = STATE_FLAG_CURSOR_RECONCILED |
                   (journalMatched ? STATE_FLAG_JOURNAL_MATCHED : 0U) |
                   (planned == JournalSegmentRead::Empty
                        ? STATE_FLAG_NO_RECORDS
                        : STATE_FLAG_SEGMENT_FOLLOWS);
    m_phase = Phase::State;
    m_beginLength = length;
    std::memcpy(m_beginBytes.data(), bytes, length);
    m_commitLength = 0U;
    return TransferInputResult::Accepted;
}

TransferInputResult GameHistoryTransfer::HandleCommit(
    const uint8_t* const bytes,
    const size_t length,
    const uint32_t connectionEpoch,
    const bool ownerReady,
    const bool otaBusy) {
    if (bytes == nullptr || length < FRAME_HEADER_BYTES ||
        bytes[0U] != FRAME_MAGIC) {
        return TransferInputResult::Dropped;
    }
    const uint16_t transactionId = ReadLe16(bytes + 4U);
    if (transactionId == 0U || connectionEpoch != m_connectionEpoch ||
        transactionId != m_transactionId) {
        return TransferInputResult::Dropped;
    }
    if (!ownerReady || otaBusy) {
        Frame rejectedFrame;
        const bool decoded = DecodeFrame(bytes, length, rejectedFrame);
        const uint8_t opcode = decoded ? rejectedFrame.opcode : bytes[2U];
        uint16_t responseSequence = m_nextFrameSequence;
        if (decoded && rejectedFrame.frameSequence != UINT16_MAX) {
            responseSequence = static_cast<uint16_t>(
                rejectedFrame.frameSequence + 1U);
        }
        Abandon();
        m_connectionEpoch = connectionEpoch;
        m_nextFrameSequence = responseSequence;
        (void)SetReject(transactionId, opcode,
                        otaBusy ? RejectReason::Busy
                                : RejectReason::BadState,
                        0U);
        return TransferInputResult::Rejected;
    }
    if (SameCommit(bytes, length)) {
        if (m_state.journalId != m_journal.Summary().journalId) {
            const uint16_t responseSequence = static_cast<uint16_t>(ReadLe16(bytes + 6U) + 1U);
            Abandon();
            m_connectionEpoch = connectionEpoch;
            m_nextFrameSequence = responseSequence;
            (void)SetReject(transactionId, bytes[2U], RejectReason::UnavailableWatermark, 0U);
            return TransferInputResult::Rejected;
        }
        Frame duplicate;
        if (!DecodeFrame(bytes, length, duplicate)) {
            return TransferInputResult::Dropped;
        }
        m_nextFrameSequence = static_cast<uint16_t>(
            duplicate.frameSequence + 1U);
        m_phase = Phase::Committed;
        return TransferInputResult::Accepted;
    }
    if (m_pendingLength != 0U && !PendingIsRejection()) return TransferInputResult::Dropped;
    if (bytes[1U] != PROTOCOL_VERSION) {
        const uint16_t incomingSequence = ReadLe16(bytes + 6U);
        const uint16_t semanticExpected = m_nextFrameSequence;
        if (incomingSequence == semanticExpected &&
            incomingSequence != UINT16_MAX) {
            m_nextFrameSequence = static_cast<uint16_t>(
                incomingSequence + 1U);
        }
        (void)SetReject(transactionId, bytes[2U],
                        RejectReason::UnsupportedVersion,
                        incomingSequence == semanticExpected ? 0U
                                                             : semanticExpected,
                        bytes[1U]);
        return TransferInputResult::Rejected;
    }
    Frame frame;
    if (!DecodeFrame(bytes, length, frame)) {
        return TransferInputResult::Dropped;
    }
    if (m_phase != Phase::AwaitCommit ||
        frame.opcode != static_cast<uint8_t>(Opcode::Commit) ||
        frame.flags != 0U ||
        frame.frameSequence != m_nextFrameSequence) {
        const uint16_t semanticExpected = m_nextFrameSequence;
        const bool expectedCommitSequence =
            m_phase == Phase::AwaitCommit &&
            frame.opcode == static_cast<uint8_t>(Opcode::Commit) &&
            frame.frameSequence == semanticExpected &&
            frame.frameSequence != UINT16_MAX;
        if (expectedCommitSequence) {
            m_nextFrameSequence = static_cast<uint16_t>(
                frame.frameSequence + 1U);
        }
        (void)SetReject(transactionId, frame.opcode,
                        !IsKnownOpcode(frame.opcode)
                            ? RejectReason::UnsupportedOpcode
                            : RejectReason::Sequence,
                        expectedCommitSequence ? 0U : semanticExpected);
        return TransferInputResult::Rejected;
    }
    CommitPayload commit;
    if (!DecodeCommitPayload(frame.payload.data(), frame.payloadLength,
                             commit) ||
        commit.firstOutboxSequence !=
            m_plan.descriptor.firstOutboxSequence ||
        commit.lastOutboxSequence !=
            m_plan.descriptor.LastOutboxSequence() ||
        commit.crc32 != m_plan.descriptor.crc32) {
        m_nextFrameSequence = static_cast<uint16_t>(
            frame.frameSequence + 1U);
        (void)SetReject(transactionId, frame.opcode,
                        RejectReason::Crc, 0U);
        return TransferInputResult::Rejected;
    }
    if (!PlanIsCurrent()) {
        m_nextFrameSequence = static_cast<uint16_t>(frame.frameSequence + 1U);
        (void)SetReject(transactionId, frame.opcode, RejectReason::UnavailableWatermark, 0U);
        return TransferInputResult::Rejected;
    }
    if (m_journal.AdvanceDurableAck(commit.lastOutboxSequence) !=
        JournalMutationResult::Applied) {
        m_nextFrameSequence = static_cast<uint16_t>(
            frame.frameSequence + 1U);
        (void)SetReject(transactionId, frame.opcode,
                        RejectReason::Storage, 0U);
        return TransferInputResult::IoFailure;
    }
    m_commit = commit;
    m_commitLength = length;
    std::memcpy(m_commitBytes.data(), bytes, length);
    m_nextFrameSequence = static_cast<uint16_t>(
        frame.frameSequence + 1U);
    m_phase = Phase::Committed;
    return TransferInputResult::Accepted;
}

} // namespace modules::game_history
