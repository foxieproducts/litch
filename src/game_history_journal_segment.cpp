#include "modules/game_history_journal.hpp"

#include <algorithm>
#include <array>

namespace modules::game_history {
namespace {

bool IsPositionRecord(const RecordType type) {
    return type == RecordType::LineBase ||
           type == RecordType::CommittedPly;
}

bool NeedsTerminalSeal(const Record& record) {
    if (!IsPositionRecord(record.type) ||
        record.position.bytes[POSITION_BYTES - 1U] == INVALID_SQUARE) {
        return false;
    }
    fxe::FixedString<litch_max::FEN_BUFFER_CAPACITY> fen;
    uint8_t duck = INVALID_SQUARE;
    if (!DecodePosition(record.position, Variant::Duck, fen, duck)) return true;
    bool whiteKing = false;
    bool blackKing = false;
    for (const char token : fen.View()) {
        if (token == ' ') break;
        whiteKing = whiteKing || token == 'K';
        blackKing = blackKing || token == 'k';
    }
    return !whiteKing || !blackKing;
}

} // namespace

JournalSegmentRead GameHistoryJournal::ReadLatestRecord(
    Record& outRecord) const {
    outRecord = {};
    if (!m_open) return JournalSegmentRead::IoFailure;
    if (m_summary.hasInvalidSuffix) return JournalSegmentRead::Corrupt;
    if (m_summary.validPrefixBytes == 0U) {
        return JournalSegmentRead::Empty;
    }

    if (m_latestLength == 0U || m_latestLength > MAX_RECORD_BYTES ||
        m_latestOffset > m_summary.validPrefixBytes ||
        m_latestLength != m_summary.validPrefixBytes - m_latestOffset) {
        return JournalSegmentRead::Corrupt;
    }
    std::array<uint8_t, MAX_RECORD_BYTES> bytes{};
    if (m_ops.read == nullptr ||
        !m_ops.read(m_ops.context, JournalArtifact::Journal,
                    m_latestOffset, bytes.data(), m_latestLength) ||
        !DecodeRecord(bytes.data(), m_latestLength, outRecord)) {
        return JournalSegmentRead::IoFailure;
    }
    if (outRecord.outboxSequence != m_summary.newestAllocated) {
        outRecord = {};
        return JournalSegmentRead::Corrupt;
    }
    return JournalSegmentRead::Ready;
}

JournalSegmentRead GameHistoryJournal::ReadCurrentLinePosition(
    const GameId& gameId, const uint16_t ply, const uint64_t beforeRevision,
    Record& outRecord) const {
    outRecord = {};
    if (!m_open) return JournalSegmentRead::IoFailure;
    if (m_summary.hasInvalidSuffix) return JournalSegmentRead::Corrupt;
    if (!IsValidGameId(gameId) || ply > 4096U ||
        beforeRevision > MAX_LINE_REVISION) return JournalSegmentRead::Bounds;

    bool hasLine = false;
    bool sealed = false;
    bool found = false;
    uint16_t lastPly = 0U;
    uint64_t lastRevision = 0U;
    Variant variant = Variant::Classic;
    Record candidate;
    uint32_t offset = 0U;
    while (offset < m_summary.validPrefixBytes) {
        StoredRecord stored;
        if (!ReadRecord(offset, m_summary.validPrefixBytes, stored)) {
            return JournalSegmentRead::IoFailure;
        }
        offset += stored.length;
        if (stored.gameId != gameId) {
            hasLine = false;
            found = false;
            continue;
        }
        std::array<uint8_t, MAX_RECORD_BYTES> bytes{};
        Record record;
        if (m_ops.read == nullptr ||
            !m_ops.read(m_ops.context, JournalArtifact::Journal,
                        stored.offset, bytes.data(), stored.length)) {
            return JournalSegmentRead::IoFailure;
        }
        if (!DecodeRecord(bytes.data(), stored.length, record)) {
            return JournalSegmentRead::Corrupt;
        }
        if (record.type == RecordType::LineBase) {
            if (hasLine && record.lineRevision <= lastRevision) {
                return JournalSegmentRead::Corrupt;
            }
            hasLine = true;
            sealed = false;
            found = false;
            variant = record.variant;
        } else {
            if (!hasLine || sealed || record.lineRevision < lastRevision ||
                (record.type == RecordType::CommittedPly && record.ply != lastPly + 1U)) {
                return JournalSegmentRead::Corrupt;
            }
            sealed = record.type == RecordType::Seal;
        }
        lastRevision = record.lineRevision;
        if (!sealed) {
            lastPly = record.ply;
            if (record.ply == ply) {
                candidate = record;
                candidate.variant = variant;
                found = true;
            }
        }
    }
    if (!hasLine || !found || lastRevision >= beforeRevision) {
        return JournalSegmentRead::Unavailable;
    }
    outRecord = candidate;
    return JournalSegmentRead::Ready;
}

JournalSegmentRead GameHistoryJournal::PlanSegmentAfter(
    const uint32_t durableAppAck,
    JournalSegmentPlan& outPlan) const {
    outPlan = {};
    if (!m_open) return JournalSegmentRead::IoFailure;
    if (durableAppAck >= m_summary.newestAllocated ||
        m_summary.validPrefixBytes == 0U) {
        return JournalSegmentRead::Empty;
    }
    const uint32_t desired = durableAppAck == UINT32_MAX
                                 ? UINT32_MAX
                                 : durableAppAck + 1U;
    StoredRecord previous;
    bool hasPrevious = false;
    StoredRecord target;
    bool hasTarget = false;
    uint32_t offset = 0U;
    while (offset < m_summary.validPrefixBytes) {
        StoredRecord current;
        if (!ReadRecord(offset, m_summary.validPrefixBytes, current)) {
            return JournalSegmentRead::IoFailure;
        }
        if (current.outboxSequence >= desired) {
            target = current;
            hasTarget = true;
            break;
        }
        previous = current;
        hasPrevious = true;
        offset += current.length;
    }
    if (!hasTarget) return JournalSegmentRead::Empty;

    const bool gap = target.outboxSequence != desired;
    if (gap && target.type != RecordType::LineBase) {
        return JournalSegmentRead::Unavailable;
    }
    bool overlap = false;
    StoredRecord first = target;
    if (!gap && target.type != RecordType::LineBase) {
        if (!hasPrevious ||
            previous.outboxSequence != durableAppAck ||
            previous.gameId != target.gameId ||
            !IsPositionRecord(previous.type)) {
            return JournalSegmentRead::Unavailable;
        }
        overlap = true;
        first = previous;
    }

    Crc32Accumulator crc;
    uint32_t cursor = first.offset;
    size_t encodedBytes = 0U;
    uint8_t recordCount = 0U;
    uint8_t transitionCount = 0U;
    bool includedUnacknowledged = false;
    const GameId firstGameId = first.gameId;
    while (cursor < m_summary.validPrefixBytes) {
        StoredRecord current;
        if (!ReadRecord(cursor, m_summary.validPrefixBytes, current)) {
            return JournalSegmentRead::IoFailure;
        }
        if (firstGameId != current.gameId ||
            (recordCount != 0U &&
             current.type == RecordType::LineBase) ||
            recordCount >= MAX_SEGMENT_TRANSITIONS + 2U ||
            (current.type == RecordType::CommittedPly &&
             transitionCount >= MAX_SEGMENT_TRANSITIONS) ||
            encodedBytes + current.length > MAX_SEGMENT_BYTES) {
            break;
        }
        std::array<uint8_t, JOURNAL_COPY_BUFFER_BYTES> recordBytes{};
        Record decoded;
        if (m_ops.read == nullptr ||
            !m_ops.read(m_ops.context, JournalArtifact::Journal,
                        cursor, recordBytes.data(), current.length) ||
            !DecodeRecord(recordBytes.data(), current.length, decoded)) {
            return JournalSegmentRead::IoFailure;
        }
        if (NeedsTerminalSeal(decoded)) {
            // A terminal Duck position is not a valid active archive. Keep its
            // result in this same segment, or wait before admitting the move.
            StoredRecord seal;
            const uint32_t nextOffset = cursor + current.length;
            if (nextOffset >= m_summary.validPrefixBytes) break;
            if (!ReadRecord(nextOffset, m_summary.validPrefixBytes, seal)) {
                return JournalSegmentRead::IoFailure;
            }
            if (seal.type != RecordType::Seal || seal.gameId != current.gameId ||
                current.outboxSequence == UINT32_MAX ||
                seal.outboxSequence != current.outboxSequence + 1U ||
                encodedBytes + current.length + seal.length > MAX_SEGMENT_BYTES ||
                recordCount + 2U > MAX_SEGMENT_TRANSITIONS + 2U) {
                break;
            }
        }
        if (!crc.Update(recordBytes.data(), current.length)) {
            return JournalSegmentRead::IoFailure;
        }
        encodedBytes += current.length;
        ++recordCount;
        if (current.type == RecordType::CommittedPly) {
            ++transitionCount;
        }
        if (current.outboxSequence > durableAppAck) {
            includedUnacknowledged = true;
        }
        cursor += current.length;
        if (current.type == RecordType::Seal) break;
    }
    if (!includedUnacknowledged || recordCount == 0U) {
        return JournalSegmentRead::Unavailable;
    }
    outPlan.journalOffset = first.offset;
    outPlan.layoutEpoch = m_layoutEpoch;
    outPlan.descriptor.encodedBytes =
        static_cast<uint16_t>(encodedBytes);
    outPlan.descriptor.recordCount = recordCount;
    outPlan.descriptor.flags = overlap
                                   ? SEGMENT_FLAG_ACKNOWLEDGED_OVERLAP
                                   : 0U;
    outPlan.descriptor.firstOutboxSequence =
        first.outboxSequence;
    outPlan.descriptor.crc32 = crc.Value();
    return gap ? JournalSegmentRead::GapReady
               : JournalSegmentRead::Ready;
}

JournalSegmentRead GameHistoryJournal::ReadSegmentChunk(
    const JournalSegmentPlan& plan,
    const uint16_t segmentOffset,
    uint8_t* const out,
    const size_t outCapacity,
    size_t& outLength) const {
    outLength = 0U;
    uint8_t encodedDescriptor[12] = {};
    if (!m_open || plan.layoutEpoch == 0U ||
        plan.layoutEpoch != m_layoutEpoch) {
        return JournalSegmentRead::Unavailable;
    }
    if (out == nullptr || outCapacity == 0U ||
        outCapacity > MAX_FRAME_PAYLOAD_BYTES ||
        !EncodeSegmentDescriptor(plan.descriptor, encodedDescriptor) ||
        segmentOffset >= plan.descriptor.encodedBytes) {
        return JournalSegmentRead::Bounds;
    }
    const size_t remaining = plan.descriptor.encodedBytes - segmentOffset;
    outLength = std::min(outCapacity, remaining);
    const uint32_t journalOffset =
        plan.journalOffset + static_cast<uint32_t>(segmentOffset);
    if (journalOffset > m_summary.validPrefixBytes ||
        outLength > m_summary.validPrefixBytes - journalOffset ||
        m_ops.read == nullptr ||
        !m_ops.read(m_ops.context, JournalArtifact::Journal,
                    journalOffset, out, outLength)) {
        outLength = 0U;
        return JournalSegmentRead::IoFailure;
    }
    return JournalSegmentRead::Ready;
}

} // namespace modules::game_history
