#include "modules/game_history_transfer.hpp"

#include <cstring>

namespace modules::game_history {
namespace {

void WriteLe16(uint8_t* const out, const uint16_t value) {
    out[0U] = static_cast<uint8_t>(value & 0xFFU);
    out[1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

} // namespace

void GameHistoryTransfer::Abandon() {
    m_phase = Phase::Idle;
    m_connectionEpoch = 0U;
    m_transactionId = 0U;
    m_nextFrameSequence = 0U;
    m_segmentOffset = 0U;
    m_stateFlags = 0U;
    m_state = {};
    m_bounds = {};
    m_plan = {};
    m_commit = {};
    m_reject = {};
    m_beginLength = 0U;
    m_commitLength = 0U;
    m_busyTransactionId = 0U;
    m_pendingLength = 0U;
    m_pendingToken = 0U;
}

bool GameHistoryTransfer::PlanIsCurrent() const {
    if (m_state.journalId != m_journal.Summary().journalId) return false;
    if ((m_stateFlags & STATE_FLAG_SEGMENT_FOLLOWS) == 0U) return true;
    uint8_t checked = 0U;
    size_t length = 0U;
    return m_journal.ReadSegmentChunk(m_plan, 0U, &checked, 1U, length) ==
               JournalSegmentRead::Ready && length == 1U;
}

TransferInputResult GameHistoryTransfer::QueueBusy(const uint16_t transactionId,
                                                  const RejectReason reason) {
    if (m_busyTransactionId != 0U) return TransferInputResult::Dropped;
    m_busyTransactionId = transactionId;
    m_busyReason = reason;
    return TransferInputResult::Rejected;
}

bool GameHistoryTransfer::HasNotification() const {
    return m_pendingLength != 0U || m_busyTransactionId != 0U ||
           m_phase == Phase::State || m_phase == Phase::Bounds ||
           m_phase == Phase::SegmentBegin || m_phase == Phase::Data ||
           m_phase == Phase::Committed || m_phase == Phase::Reject;
}

bool GameHistoryTransfer::AwaitingCommit() const {
    return m_phase == Phase::AwaitCommit &&
           (m_pendingLength == 0U || PendingIsRejection());
}

bool GameHistoryTransfer::PendingIsRejection() const {
    return m_pendingLength != 0U &&
           m_pendingFrame[2U] == static_cast<uint8_t>(Opcode::Reject);
}

bool GameHistoryTransfer::SameBegin(const uint8_t* const bytes,
                                    const size_t length) const {
    return bytes != nullptr && length == m_beginLength &&
           length != 0U &&
           std::memcmp(bytes, m_beginBytes.data(), length) == 0;
}

bool GameHistoryTransfer::SameCommit(const uint8_t* const bytes,
                                     const size_t length) const {
    return bytes != nullptr && length == m_commitLength &&
           length != 0U &&
           std::memcmp(bytes, m_commitBytes.data(), length) == 0;
}

bool GameHistoryTransfer::SetReject(
    const uint16_t transactionId,
    const uint8_t offendingOpcode,
    const RejectReason reason,
    const uint16_t expectedFrameSequence,
    const uint32_t detail) {
    if (transactionId == 0U) return false;
    m_transactionId = transactionId;
    m_reject.reason = reason;
    m_reject.rejectedOpcode = offendingOpcode;
    m_reject.expectedFrameSequence = expectedFrameSequence;
    m_reject.detail = detail;
    m_phase = Phase::Reject;
    return true;
}

bool GameHistoryTransfer::BuildFrame(
    const Opcode opcode,
    const uint8_t flags,
    const uint8_t* const payload,
    const uint16_t payloadLength,
    uint8_t* const out,
    const size_t outCapacity,
    size_t& outLength) {
    outLength = FRAME_HEADER_BYTES + payloadLength;
    if (out == nullptr || m_transactionId == 0U ||
        (payload == nullptr && payloadLength != 0U) ||
        outLength > outCapacity || outLength > m_frameCapacity ||
        outLength > MAX_FRAME_BYTES || m_nextFrameSequence == UINT16_MAX) {
        outLength = 0U;
        return false;
    }
    out[0U] = FRAME_MAGIC;
    out[1U] = PROTOCOL_VERSION;
    out[2U] = static_cast<uint8_t>(opcode);
    out[3U] = flags;
    WriteLe16(out + 4U, m_transactionId);
    WriteLe16(out + 6U, m_nextFrameSequence);
    if (payloadLength != 0U) {
        std::memcpy(out + FRAME_HEADER_BYTES, payload, payloadLength);
    }
    ++m_nextFrameSequence;
    return true;
}

bool GameHistoryTransfer::NextNotification(
    const uint32_t connectionEpoch,
    uint8_t* const out,
    const size_t outCapacity,
    size_t& outLength) {
    outLength = 0U;
    if (connectionEpoch == 0U || connectionEpoch != m_connectionEpoch || out == nullptr) {
        return false;
    }
    if (m_beginLength != 0U && m_phase != Phase::Reject && !PendingIsRejection() &&
        (m_state.journalId != m_journal.Summary().journalId ||
         (m_commitLength == 0U && !PlanIsCurrent()))) {
        Abandon();
        return false;
    }
    if (m_pendingLength == 0U) {
        if (m_lastDeliveryToken == UINT32_MAX) { Abandon(); return false; }
        if (!PrepareNotification(connectionEpoch, m_pendingFrame.data(),
                                 m_pendingFrame.size(), m_pendingLength)) return false;
        m_pendingToken = ++m_lastDeliveryToken;
    }
    if (outCapacity < m_pendingLength) return false;
    std::memcpy(out, m_pendingFrame.data(), m_pendingLength);
    outLength = m_pendingLength;
    return true;
}

bool GameHistoryTransfer::NotificationAccepted(const uint32_t connectionEpoch,
                                              const uint32_t token) {
    if (connectionEpoch != m_connectionEpoch || m_pendingLength == 0U ||
        token == 0U || token != m_pendingToken) return false;
    m_pendingLength = 0U;
    m_pendingToken = 0U;
    return true;
}

bool GameHistoryTransfer::PrepareNotification(
    const uint32_t connectionEpoch,
    uint8_t* const out,
    const size_t outCapacity,
    size_t& outLength) {
    outLength = 0U;
    if (connectionEpoch != m_connectionEpoch || out == nullptr ||
        outCapacity < MIN_ATT_PAYLOAD_BYTES) {
        return false;
    }
    uint8_t payload[12] = {};
    if (m_busyTransactionId != 0U) {
        const RejectPayload busy{m_busyReason, static_cast<uint8_t>(Opcode::Begin), 0U, 0U};
        if (!EncodeRejectPayload(busy, payload)) return false;
        out[0U] = FRAME_MAGIC;
        out[1U] = PROTOCOL_VERSION;
        out[2U] = static_cast<uint8_t>(Opcode::Reject);
        out[3U] = 0U;
        WriteLe16(out + 4U, m_busyTransactionId);
        WriteLe16(out + 6U, 1U);
        std::memcpy(out + FRAME_HEADER_BYTES, payload, 8U);
        outLength = FRAME_HEADER_BYTES + 8U;
        m_busyTransactionId = 0U;
        return true;
    }
    switch (m_phase) {
        case Phase::State:
            if (!EncodeStatePayload(m_state, payload) ||
                !BuildFrame(Opcode::State, m_stateFlags, payload,
                            sizeof(payload), out, outCapacity, outLength)) {
                return false;
            }
            m_phase = Phase::Bounds;
            return true;
        case Phase::Bounds:
            if (!EncodeStateBoundsPayload(m_bounds, payload) ||
                !BuildFrame(Opcode::StateBounds, 0U, payload,
                            sizeof(payload), out, outCapacity, outLength)) {
                return false;
            }
            m_phase = (m_stateFlags & STATE_FLAG_SEGMENT_FOLLOWS) != 0U
                          ? Phase::SegmentBegin
                          : Phase::Done;
            return true;
        case Phase::SegmentBegin:
            if (!EncodeSegmentDescriptor(m_plan.descriptor, payload) ||
                !BuildFrame(Opcode::SegmentBegin, 0U, payload,
                            sizeof(payload), out, outCapacity, outLength)) {
                return false;
            }
            m_phase = Phase::Data;
            return true;
        case Phase::Data: {
            const size_t stride = m_frameCapacity - FRAME_HEADER_BYTES;
            size_t chunkLength = 0U;
            if (outCapacity < FRAME_HEADER_BYTES + 1U ||
                stride == 0U || stride > outCapacity - FRAME_HEADER_BYTES ||
                m_nextFrameSequence == UINT16_MAX ||
                m_journal.ReadSegmentChunk(
                    m_plan, m_segmentOffset, out + FRAME_HEADER_BYTES,
                    stride, chunkLength) != JournalSegmentRead::Ready) {
                Abandon();
                return false;
            }
            out[0U] = FRAME_MAGIC;
            out[1U] = PROTOCOL_VERSION;
            out[2U] = static_cast<uint8_t>(Opcode::Data);
            out[3U] = 0U;
            WriteLe16(out + 4U, m_transactionId);
            WriteLe16(out + 6U, m_nextFrameSequence);
            ++m_nextFrameSequence;
            outLength = FRAME_HEADER_BYTES + chunkLength;
            m_segmentOffset = static_cast<uint16_t>(
                m_segmentOffset + chunkLength);
            if (m_segmentOffset == m_plan.descriptor.encodedBytes) {
                m_phase = Phase::AwaitCommit;
            }
            return true;
        }
        case Phase::Committed:
            if (!EncodeCommitPayload(m_commit, payload) ||
                !BuildFrame(Opcode::Committed, 0U, payload,
                            sizeof(payload), out, outCapacity, outLength)) {
                return false;
            }
            m_phase = Phase::Done;
            return true;
        case Phase::Reject: {
            uint8_t rejection[8] = {};
            if (!EncodeRejectPayload(m_reject, rejection) ||
                !BuildFrame(Opcode::Reject, 0U, rejection,
                            sizeof(rejection), out, outCapacity, outLength)) {
                return false;
            }
            m_phase = Phase::Done;
            return true;
        }
        case Phase::Idle:
        case Phase::AwaitCommit:
        case Phase::Done:
        default:
            return false;
    }
}

} // namespace modules::game_history
