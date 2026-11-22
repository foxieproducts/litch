#include "modules/game_history_protocol.hpp"

#include <cstring>

namespace modules::game_history {
namespace {

void WriteLe16(uint8_t* const out, const uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFU);
    out[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

void WriteLe32(uint8_t* const out, const uint32_t value) {
    for (uint8_t index = 0U; index < 4U; ++index) {
        out[index] = static_cast<uint8_t>(
            (value >> static_cast<uint8_t>(index * 8U)) & 0xFFU);
    }
}

void WriteLe64(uint8_t* const out, const uint64_t value) {
    for (uint8_t index = 0U; index < 8U; ++index) {
        out[index] = static_cast<uint8_t>(
            (value >> static_cast<uint8_t>(index * 8U)) & 0xFFU);
    }
}

uint16_t ReadLe16(const uint8_t* const bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
}

uint32_t ReadLe32(const uint8_t* const bytes) {
    uint32_t value = 0U;
    for (uint8_t index = 0U; index < 4U; ++index) {
        value |= static_cast<uint32_t>(bytes[index]) <<
                 static_cast<uint8_t>(index * 8U);
    }
    return value;
}

uint64_t ReadLe64(const uint8_t* const bytes) {
    uint64_t value = 0U;
    for (uint8_t index = 0U; index < 8U; ++index) {
        value |= static_cast<uint64_t>(bytes[index]) <<
                 static_cast<uint8_t>(index * 8U);
    }
    return value;
}

bool IsKnownRejectReason(const RejectReason reason) {
    return reason >= RejectReason::UnsupportedVersion &&
           reason <= RejectReason::UnsupportedOpcode;
}

} // namespace

bool IsKnownOpcode(const uint8_t opcode) {
    return (opcode >= static_cast<uint8_t>(Opcode::Begin) &&
            opcode <= static_cast<uint8_t>(Opcode::Committed)) ||
           (opcode >= static_cast<uint8_t>(Opcode::RestoreBegin) &&
            opcode <= static_cast<uint8_t>(Opcode::RestoreAccepted)) ||
           (opcode >= static_cast<uint8_t>(Opcode::CurrentBegin) &&
            opcode <= static_cast<uint8_t>(Opcode::CurrentArchived)) ||
           opcode == static_cast<uint8_t>(Opcode::Reject);
}

bool IsValidRestoreCursor(const RestoreCursorPayload& payload) {
    return payload.journalId != 0U && payload.durableAck != 0U;
}

bool IsValidRestoreManifest(const RestoreManifestPayload& payload) {
    if (!IsValidVariant(payload.variant) || payload.recordCount == 0U ||
        payload.recordCount > MAX_RESTORE_RECORDS ||
        payload.currentPly < payload.basePly ||
        payload.currentPly > MAX_RESTORE_PLY) {
        return false;
    }
    const uint16_t transitions =
        static_cast<uint16_t>(payload.recordCount - 1U);
    if (payload.currentPly - payload.basePly != transitions) {
        return false;
    }
    const uint16_t minimumBytes = static_cast<uint16_t>(
        LINE_BASE_RECORD_BYTES +
        transitions * MIN_COMMITTED_PLY_RECORD_BYTES);
    const uint16_t maximumBytes = static_cast<uint16_t>(
        LINE_BASE_RECORD_BYTES +
        transitions * MAX_COMMITTED_PLY_RECORD_BYTES);
    return payload.encodedBytes >= minimumBytes &&
           payload.encodedBytes <= maximumBytes &&
           payload.encodedBytes <= MAX_RESTORE_UPLOAD_BYTES;
}

bool IsValidRestoreAccepted(const RestoreAcceptedPayload& payload) {
    if (!IsValidVariant(payload.variant) ||
        payload.currentPly > MAX_RESTORE_PLY ||
        payload.transitionCount > MAX_RESTORE_TRANSITIONS ||
        payload.transitionCount > payload.currentPly) {
        return false;
    }
    const uint16_t minimumBytes = static_cast<uint16_t>(
        LINE_BASE_RECORD_BYTES +
        payload.transitionCount * MIN_COMMITTED_PLY_RECORD_BYTES);
    const uint16_t maximumBytes = static_cast<uint16_t>(
        LINE_BASE_RECORD_BYTES +
        payload.transitionCount * MAX_COMMITTED_PLY_RECORD_BYTES);
    return payload.encodedBytes >= minimumBytes &&
           payload.encodedBytes <= maximumBytes &&
           payload.encodedBytes <= MAX_RESTORE_UPLOAD_BYTES;
}

bool EncodeRestoreCursorPayload(const RestoreCursorPayload& payload,
                                uint8_t out[CONTROL_PAYLOAD_BYTES]) {
    if (out == nullptr || !IsValidRestoreCursor(payload)) return false;
    WriteLe64(out, payload.journalId);
    WriteLe32(out + 8U, payload.durableAck);
    return true;
}

bool DecodeRestoreCursorPayload(const uint8_t* const bytes,
                                const size_t length,
                                RestoreCursorPayload& out) {
    if (bytes == nullptr || length != CONTROL_PAYLOAD_BYTES) return false;
    const RestoreCursorPayload candidate{ReadLe64(bytes),
                                         ReadLe32(bytes + 8U)};
    if (!IsValidRestoreCursor(candidate)) return false;
    out = candidate;
    return true;
}

bool EncodeRestoreManifestPayload(const RestoreManifestPayload& payload,
                                  uint8_t out[CONTROL_PAYLOAD_BYTES]) {
    if (out == nullptr || !IsValidRestoreManifest(payload)) return false;
    WriteLe16(out, payload.encodedBytes);
    out[2U] = payload.recordCount;
    out[3U] = static_cast<uint8_t>(payload.variant);
    WriteLe16(out + 4U, payload.basePly);
    WriteLe16(out + 6U, payload.currentPly);
    WriteLe32(out + 8U, payload.crc32);
    return true;
}

bool DecodeRestoreManifestPayload(const uint8_t* const bytes,
                                  const size_t length,
                                  RestoreManifestPayload& out) {
    if (bytes == nullptr || length != CONTROL_PAYLOAD_BYTES) return false;
    const RestoreManifestPayload candidate{
        ReadLe16(bytes), bytes[2U], static_cast<Variant>(bytes[3U]),
        ReadLe16(bytes + 4U), ReadLe16(bytes + 6U), ReadLe32(bytes + 8U)};
    if (!IsValidRestoreManifest(candidate)) return false;
    out = candidate;
    return true;
}

bool EncodeRestoreAcceptedPayload(const RestoreAcceptedPayload& payload,
                                  uint8_t out[CONTROL_PAYLOAD_BYTES]) {
    if (out == nullptr || !IsValidRestoreAccepted(payload)) return false;
    WriteLe16(out, payload.currentPly);
    out[2U] = payload.transitionCount;
    out[3U] = static_cast<uint8_t>(payload.variant);
    WriteLe16(out + 4U, payload.encodedBytes);
    out[6U] = 0U;
    out[7U] = 0U;
    WriteLe32(out + 8U, payload.crc32);
    return true;
}

bool DecodeRestoreAcceptedPayload(const uint8_t* const bytes,
                                  const size_t length,
                                  RestoreAcceptedPayload& out) {
    if (bytes == nullptr || length != CONTROL_PAYLOAD_BYTES ||
        bytes[6U] != 0U || bytes[7U] != 0U) {
        return false;
    }
    const RestoreAcceptedPayload candidate{
        ReadLe16(bytes), bytes[2U], static_cast<Variant>(bytes[3U]),
        ReadLe16(bytes + 4U), ReadLe32(bytes + 8U)};
    if (!IsValidRestoreAccepted(candidate)) return false;
    out = candidate;
    return true;
}

void Crc32Accumulator::Reset() {
    m_state = 0xFFFFFFFFU;
}

bool Crc32Accumulator::Update(const uint8_t* const bytes,
                              const size_t length) {
    if (bytes == nullptr && length != 0U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        m_state ^= static_cast<uint32_t>(bytes[index]);
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (m_state & 1U) != 0U
                                      ? 0xEDB88320U
                                      : 0U;
            m_state = (m_state >> 1U) ^ mask;
        }
    }
    return true;
}

uint32_t Crc32Accumulator::Value() const {
    return ~m_state;
}

uint32_t SegmentDescriptor::LastOutboxSequence() const {
    if (recordCount == 0U || firstOutboxSequence == 0U ||
        firstOutboxSequence > UINT32_MAX -
            static_cast<uint32_t>(recordCount - 1U)) {
        return 0U;
    }
    return firstOutboxSequence + static_cast<uint32_t>(recordCount - 1U);
}

bool EncodeFrame(const Frame& frame,
                 uint8_t* const out,
                 const size_t outCapacity,
                 size_t& outLength) {
    outLength = 0U;
    if (out == nullptr || frame.transactionId == 0U ||
        frame.payloadLength > MAX_FRAME_PAYLOAD_BYTES ||
        outCapacity < FRAME_HEADER_BYTES + frame.payloadLength) {
        return false;
    }
    out[0] = FRAME_MAGIC;
    out[1] = PROTOCOL_VERSION;
    out[2] = frame.opcode;
    out[3] = frame.flags;
    WriteLe16(out + 4U, frame.transactionId);
    WriteLe16(out + 6U, frame.frameSequence);
    std::memcpy(out + FRAME_HEADER_BYTES,
                frame.payload.data(), frame.payloadLength);
    outLength = FRAME_HEADER_BYTES + frame.payloadLength;
    return outLength <= MAX_FRAME_BYTES;
}

bool DecodeFrame(const uint8_t* const bytes,
                 const size_t length,
                 Frame& out) {
    if (bytes == nullptr || length < FRAME_HEADER_BYTES ||
        length > MAX_FRAME_BYTES || bytes[0] != FRAME_MAGIC ||
        bytes[1] != PROTOCOL_VERSION) {
        return false;
    }
    Frame candidate;
    candidate.opcode = bytes[2];
    candidate.flags = bytes[3];
    candidate.transactionId = ReadLe16(bytes + 4U);
    candidate.frameSequence = ReadLe16(bytes + 6U);
    candidate.payloadLength = static_cast<uint16_t>(
        length - FRAME_HEADER_BYTES);
    if (candidate.transactionId == 0U) {
        return false;
    }
    std::memcpy(candidate.payload.data(), bytes + FRAME_HEADER_BYTES,
                candidate.payloadLength);
    out = candidate;
    return true;
}

bool EncodeBeginPayload(const BeginPayload& payload, uint8_t out[12]) {
    if (out == nullptr) {
        return false;
    }
    WriteLe64(out, payload.journalId);
    WriteLe32(out + 8U, payload.appDurableAck);
    return true;
}

bool DecodeBeginPayload(const uint8_t* const bytes,
                        const size_t length,
                        BeginPayload& out) {
    if (bytes == nullptr || length != 12U) {
        return false;
    }
    BeginPayload candidate;
    candidate.journalId = ReadLe64(bytes);
    candidate.appDurableAck = ReadLe32(bytes + 8U);
    out = candidate;
    return true;
}

bool EncodeStatePayload(const StatePayload& payload, uint8_t out[12]) {
    if (out == nullptr || payload.journalId == 0U) {
        return false;
    }
    WriteLe64(out, payload.journalId);
    WriteLe32(out + 8U, payload.oldestRetained);
    return true;
}

bool DecodeStatePayload(const uint8_t* const bytes,
                        const size_t length,
                        StatePayload& out) {
    if (bytes == nullptr || length != 12U) {
        return false;
    }
    StatePayload candidate;
    candidate.journalId = ReadLe64(bytes);
    candidate.oldestRetained = ReadLe32(bytes + 8U);
    if (candidate.journalId == 0U) {
        return false;
    }
    out = candidate;
    return true;
}

bool EncodeStateBoundsPayload(const StateBoundsPayload& payload,
                              uint8_t out[12]) {
    if (out == nullptr || payload.boardDurableAck > payload.newestAllocated) {
        return false;
    }
    WriteLe32(out, payload.newestAllocated);
    WriteLe32(out + 4U, payload.boardDurableAck);
    WriteLe32(out + 8U, payload.lostGameCount);
    return true;
}

bool DecodeStateBoundsPayload(const uint8_t* const bytes,
                              const size_t length,
                              StateBoundsPayload& out) {
    if (bytes == nullptr || length != 12U) {
        return false;
    }
    StateBoundsPayload candidate;
    candidate.newestAllocated = ReadLe32(bytes);
    candidate.boardDurableAck = ReadLe32(bytes + 4U);
    candidate.lostGameCount = ReadLe32(bytes + 8U);
    if (candidate.boardDurableAck > candidate.newestAllocated) {
        return false;
    }
    out = candidate;
    return true;
}

bool EncodeSegmentDescriptor(const SegmentDescriptor& descriptor,
                             uint8_t out[12]) {
    if (out == nullptr || descriptor.encodedBytes == 0U ||
        descriptor.encodedBytes > MAX_SEGMENT_BYTES ||
        descriptor.recordCount == 0U ||
        descriptor.recordCount > MAX_SEGMENT_TRANSITIONS + 2U ||
        (descriptor.flags &
         static_cast<uint8_t>(~SEGMENT_FLAG_ACKNOWLEDGED_OVERLAP)) != 0U ||
        descriptor.LastOutboxSequence() == 0U) {
        return false;
    }
    WriteLe16(out, descriptor.encodedBytes);
    out[2] = descriptor.recordCount;
    out[3] = descriptor.flags;
    WriteLe32(out + 4U, descriptor.firstOutboxSequence);
    WriteLe32(out + 8U, descriptor.crc32);
    return true;
}

bool DecodeSegmentDescriptor(const uint8_t* const bytes,
                             const size_t length,
                             SegmentDescriptor& out) {
    if (bytes == nullptr || length != 12U) {
        return false;
    }
    SegmentDescriptor candidate;
    candidate.encodedBytes = ReadLe16(bytes);
    candidate.recordCount = bytes[2];
    candidate.flags = bytes[3];
    candidate.firstOutboxSequence = ReadLe32(bytes + 4U);
    candidate.crc32 = ReadLe32(bytes + 8U);
    if (candidate.encodedBytes == 0U ||
        candidate.encodedBytes > MAX_SEGMENT_BYTES ||
        candidate.recordCount == 0U ||
        candidate.recordCount > MAX_SEGMENT_TRANSITIONS + 2U ||
        (candidate.flags &
         static_cast<uint8_t>(~SEGMENT_FLAG_ACKNOWLEDGED_OVERLAP)) != 0U ||
        candidate.LastOutboxSequence() == 0U) {
        return false;
    }
    out = candidate;
    return true;
}

bool EncodeCommitPayload(const CommitPayload& payload, uint8_t out[12]) {
    if (out == nullptr || payload.firstOutboxSequence == 0U ||
        payload.lastOutboxSequence < payload.firstOutboxSequence ||
        payload.lastOutboxSequence - payload.firstOutboxSequence >=
            MAX_SEGMENT_TRANSITIONS + 2U) {
        return false;
    }
    WriteLe32(out, payload.firstOutboxSequence);
    WriteLe32(out + 4U, payload.lastOutboxSequence);
    WriteLe32(out + 8U, payload.crc32);
    return true;
}

bool DecodeCommitPayload(const uint8_t* const bytes,
                         const size_t length,
                         CommitPayload& out) {
    if (bytes == nullptr || length != 12U) {
        return false;
    }
    CommitPayload candidate;
    candidate.firstOutboxSequence = ReadLe32(bytes);
    candidate.lastOutboxSequence = ReadLe32(bytes + 4U);
    candidate.crc32 = ReadLe32(bytes + 8U);
    uint8_t encoded[12] = {};
    if (!EncodeCommitPayload(candidate, encoded)) {
        return false;
    }
    out = candidate;
    return true;
}

bool EncodeRejectPayload(const RejectPayload& payload, uint8_t out[8]) {
    if (out == nullptr || !IsKnownRejectReason(payload.reason)) {
        return false;
    }
    out[0] = static_cast<uint8_t>(payload.reason);
    out[1] = payload.rejectedOpcode;
    WriteLe16(out + 2U, payload.expectedFrameSequence);
    WriteLe32(out + 4U, payload.detail);
    return true;
}

bool DecodeRejectPayload(const uint8_t* const bytes,
                         const size_t length,
                         RejectPayload& out) {
    if (bytes == nullptr || length != 8U) {
        return false;
    }
    RejectPayload candidate;
    candidate.reason = static_cast<RejectReason>(bytes[0]);
    candidate.rejectedOpcode = bytes[1];
    candidate.expectedFrameSequence = ReadLe16(bytes + 2U);
    candidate.detail = ReadLe32(bytes + 4U);
    if (!IsKnownRejectReason(candidate.reason)) {
        return false;
    }
    out = candidate;
    return true;
}

} // namespace modules::game_history
