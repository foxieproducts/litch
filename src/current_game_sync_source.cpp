#include "modules/current_game_sync_source.hpp"

#include <algorithm>
#include <array>

#include "modules/sha256.hpp"

namespace modules::current_game_sync {
namespace {

using current_game_log::CommitPointer;
using current_game_log::Slot;
using current_game_log::StorageOps;
using current_game_log::StorageStatus;
using game_history::Record;
using game_history::RecordType;

class StorageBatch final {
  public:
    explicit StorageBatch(const StorageOps& storage) : m_storage(storage) {
        m_open = storage.beginBatch == nullptr ||
            storage.beginBatch(storage.context);
    }
    ~StorageBatch() {
        if (m_open && m_storage.beginBatch != nullptr &&
            m_storage.endBatch != nullptr) {
            m_storage.endBatch(m_storage.context);
        }
    }
    bool IsOpen() const { return m_open; }

  private:
    const StorageOps& m_storage;
    bool m_open = false;
};

bool OpsReady(const StorageOps& storage) {
    return storage.context != nullptr && storage.stat != nullptr &&
        storage.read != nullptr &&
        ((storage.beginBatch == nullptr && storage.endBatch == nullptr) ||
         (storage.beginBatch != nullptr && storage.endBatch != nullptr));
}

bool SameAuthority(const CommitPointer& left, const CommitPointer& right) {
    return left.present == right.present && left.slot == right.slot &&
        left.generation == right.generation &&
        left.byteLength == right.byteLength &&
        left.prefixCrc32 == right.prefixCrc32 &&
        left.gameId == right.gameId &&
        left.lineRevision == right.lineRevision &&
        left.currentPly == right.currentPly &&
        left.archiveRequired == right.archiveRequired;
}

bool SamePlan(const SegmentPlan& left, const SegmentPlan& right) {
    return left.byteOffset == right.byteOffset &&
        left.descriptor.encodedBytes == right.descriptor.encodedBytes &&
        left.descriptor.recordCount == right.descriptor.recordCount &&
        left.descriptor.flags == right.descriptor.flags &&
        left.descriptor.firstOutboxSequence ==
            right.descriptor.firstOutboxSequence &&
        left.descriptor.crc32 == right.descriptor.crc32;
}

SourceResult ReadRecord(
    const StorageOps& storage,
    const CommitPointer& authority,
    const uint32_t offset,
    std::array<uint8_t, game_history::MAX_RECORD_BYTES>& encoded,
    size_t& length,
    Record& record) {
    uint8_t header[game_history::RECORD_HEADER_BYTES] = {};
    if (offset >= authority.byteLength ||
        authority.byteLength - offset < sizeof(header)) {
        return SourceResult::Corrupt;
    }
    if (!storage.read(storage.context, authority.slot, offset,
                      header, sizeof(header))) {
        return SourceResult::IoFailure;
    }
    length = game_history::RECORD_HEADER_BYTES +
        static_cast<size_t>(header[2U]) + game_history::RECORD_CRC_BYTES;
    if (length > encoded.size() ||
        length > authority.byteLength - offset) {
        return SourceResult::Corrupt;
    }
    if (!storage.read(storage.context, authority.slot, offset,
                      encoded.data(), length)) {
        return SourceResult::IoFailure;
    }
    return game_history::DecodeRecord(encoded.data(), length, record)
        ? SourceResult::Ready
        : SourceResult::Corrupt;
}

bool IsAuthorityBounded(const CommitPointer& authority) {
    return authority.present &&
        (authority.slot == Slot::A || authority.slot == Slot::B) &&
        authority.generation != 0U && authority.byteLength != 0U &&
        authority.byteLength <= current_game_log::MAX_SLOT_BYTES &&
        authority.currentPly <= current_game_log::MAX_PLIES &&
        game_history::IsValidGameId(authority.gameId) &&
        authority.lineRevision != 0U &&
        authority.lineRevision <= game_history::MAX_LINE_REVISION;
}

template <size_t Count>
void CopyDigestPrefix(const Sha256::Digest& digest,
                      std::array<uint8_t, Count>& out) {
    std::copy_n(digest.begin(), Count, out.begin());
}

} // namespace

SourceResult CurrentGameSyncSource::Open(
    const CommitPointer& authority,
    SourceSnapshot& out) {
    Close();
    if (!OpsReady(m_storage)) return SourceResult::BadState;
    if (!authority.present) {
        if (!SameAuthority(authority, CommitPointer{})) {
            return SourceResult::Bounds;
        }
        out = {};
        return SourceResult::NoLine;
    }
    if (!IsAuthorityBounded(authority)) return SourceResult::Bounds;
    const StorageBatch batch(m_storage);
    if (!batch.IsOpen()) return SourceResult::IoFailure;
    uint32_t storedBytes = 0U;
    const StorageStatus status =
        m_storage.stat(m_storage.context, authority.slot, storedBytes);
    if (status != StorageStatus::Present) return SourceResult::IoFailure;
    if (storedBytes < authority.byteLength) return SourceResult::Corrupt;

    Sha256 sha;
    game_history::Crc32Accumulator crc;
    uint32_t offset = 0U;
    uint32_t expectedSequence = 1U;
    uint16_t recordCount = 0U;
    uint16_t finalPly = 0U;
    uint64_t priorRevision = 0U;
    bool havePosition = false;
    bool sealed = false;
    game_history::Variant variant = game_history::Variant::Classic;
    game_history::Position priorPosition{};
    while (offset < authority.byteLength) {
        std::array<uint8_t, game_history::MAX_RECORD_BYTES> encoded{};
        size_t length = 0U;
        Record record;
        const SourceResult read =
            ReadRecord(m_storage, authority, offset, encoded, length, record);
        if (read != SourceResult::Ready) return read;
        const bool first = expectedSequence == 1U;
        if (record.outboxSequence != expectedSequence ||
            record.gameId != authority.gameId ||
            record.lineRevision == 0U ||
            (first && record.type != RecordType::LineBase) ||
            (!first && record.type == RecordType::LineBase) ||
            sealed) {
            return SourceResult::Corrupt;
        }
        if (record.type == RecordType::CommittedPly) {
            if (!havePosition || finalPly == current_game_log::MAX_PLIES ||
                record.ply != static_cast<uint16_t>(finalPly + 1U) ||
                record.lineRevision <= priorRevision ||
                !current_game_log::IsValidCommittedPlyForVariant(
                    variant, priorPosition, record)) {
                return SourceResult::Corrupt;
            }
        } else if (record.type == RecordType::Seal) {
            if (!havePosition || record.lineRevision != priorRevision ||
                offset + length != authority.byteLength) {
                return SourceResult::Corrupt;
            }
            sealed = true;
        } else if (record.type != RecordType::LineBase) {
            return SourceResult::Corrupt;
        }
        if (!sha.Update(encoded.data(), length) ||
            !crc.Update(encoded.data(), length)) {
            return SourceResult::Bounds;
        }
        if (record.type != RecordType::Seal) {
            if (record.type == RecordType::LineBase) {
                variant = record.variant;
            }
            havePosition = true;
            finalPly = record.ply;
            priorPosition = record.position;
        }
        priorRevision = record.lineRevision;
        offset += static_cast<uint32_t>(length);
        ++expectedSequence;
        if (recordCount == UINT16_MAX) return SourceResult::Bounds;
        ++recordCount;
    }
    if (!havePosition || offset != authority.byteLength ||
        finalPly != authority.currentPly ||
        priorRevision != authority.lineRevision ||
        crc.Value() != authority.prefixCrc32) {
        return SourceResult::Corrupt;
    }
    const uint8_t flags = static_cast<uint8_t>(
        CURRENT_LINE_PRESENT |
        (authority.archiveRequired ? ARCHIVE_REQUIRED : 0U) |
        (sealed ? LINE_SEALED : 0U));
    const Bounds bounds{authority.lineRevision, recordCount, flags};
    if (!IsValidBounds(bounds)) return SourceResult::Bounds;
    const Sha256::Digest fullDigest = sha.Finish();
    SourceSnapshot snapshot;
    CopyDigestPrefix(fullDigest, snapshot.digest.fingerprint);
    snapshot.bounds = bounds;
    if (!IsValidDigest(snapshot.digest, false)) return SourceResult::Corrupt;
    m_authority = authority;
    m_snapshot = snapshot;
    m_open = true;
    out = snapshot;
    return SourceResult::Ready;
}

SourceResult CurrentGameSyncSource::Plan(
    const CommitPointer& authority,
    const ResumeCursor& cursor,
    SegmentPlan& out) {
    m_planReady = false;
    m_plan = {};
    if (!m_open || !SameAuthority(authority, m_authority)) {
        return SourceResult::BadState;
    }
    if (!IsValidResumeCursor(cursor)) return SourceResult::Bounds;
    const uint32_t firstSequence =
        cursor.nextRecordSequence == 0U ? 1U : cursor.nextRecordSequence;
    if (firstSequence > static_cast<uint32_t>(m_snapshot.bounds.recordCount) + 1U) {
        return SourceResult::CursorMismatch;
    }
    const StorageBatch batch(m_storage);
    if (!batch.IsOpen()) return SourceResult::IoFailure;
    Sha256 prefixSha;
    game_history::Crc32Accumulator segmentCrc;
    uint32_t offset = 0U;
    uint32_t sequence = 1U;
    uint32_t segmentOffset = 0U;
    uint16_t segmentBytes = 0U;
    uint8_t segmentRecords = 0U;
    while (offset < authority.byteLength) {
        std::array<uint8_t, game_history::MAX_RECORD_BYTES> encoded{};
        size_t length = 0U;
        Record record;
        const SourceResult read =
            ReadRecord(m_storage, authority, offset, encoded, length, record);
        if (read != SourceResult::Ready) return read;
        if (record.outboxSequence != sequence ||
            record.gameId != authority.gameId) return SourceResult::Corrupt;
        if (sequence < firstSequence) {
            if (!prefixSha.Update(encoded.data(), length)) {
                return SourceResult::Bounds;
            }
        } else if (segmentRecords < MAX_SEGMENT_RECORDS &&
                   length <= MAX_SEGMENT_BYTES - segmentBytes) {
            if (segmentRecords == 0U) segmentOffset = offset;
            if (!segmentCrc.Update(encoded.data(), length)) {
                return SourceResult::Bounds;
            }
            segmentBytes = static_cast<uint16_t>(segmentBytes + length);
            ++segmentRecords;
        } else {
            break;
        }
        offset += static_cast<uint32_t>(length);
        ++sequence;
    }
    if (cursor.nextRecordSequence != 0U) {
        const Sha256::Digest prefixDigest = prefixSha.Finish();
        std::array<uint8_t, 8U> expected{};
        CopyDigestPrefix(prefixDigest, expected);
        if (expected != cursor.prefixFingerprint) {
            return SourceResult::CursorMismatch;
        }
    }
    if (firstSequence ==
        static_cast<uint32_t>(m_snapshot.bounds.recordCount) + 1U) {
        return SourceResult::Complete;
    }
    if (segmentRecords == 0U || segmentBytes == 0U) {
        return SourceResult::Corrupt;
    }
    SegmentPlan plan;
    plan.byteOffset = segmentOffset;
    plan.descriptor.encodedBytes = segmentBytes;
    plan.descriptor.recordCount = segmentRecords;
    plan.descriptor.firstOutboxSequence = firstSequence;
    plan.descriptor.crc32 = segmentCrc.Value();
    m_plan = plan;
    m_planReady = true;
    out = plan;
    return SourceResult::Ready;
}

SourceResult CurrentGameSyncSource::ReadSegmentChunk(
    const CommitPointer& authority,
    const SegmentPlan& plan,
    const uint16_t offset,
    uint8_t* const out,
    const size_t capacity,
    size_t& outLength) {
    outLength = 0U;
    if (!m_open || !m_planReady || !SameAuthority(authority, m_authority) ||
        !SamePlan(plan, m_plan)) {
        return SourceResult::BadState;
    }
    if (out == nullptr || capacity == 0U ||
        offset >= plan.descriptor.encodedBytes) {
        return SourceResult::Bounds;
    }
    const size_t remaining = plan.descriptor.encodedBytes - offset;
    outLength = std::min(capacity, remaining);
    const StorageBatch batch(m_storage);
    if (!batch.IsOpen() ||
        !m_storage.read(m_storage.context, authority.slot,
                        plan.byteOffset + offset, out, outLength)) {
        outLength = 0U;
        return SourceResult::IoFailure;
    }
    return SourceResult::Ready;
}

void CurrentGameSyncSource::Close() {
    m_authority = {};
    m_snapshot = {};
    m_plan = {};
    m_open = false;
    m_planReady = false;
}

} // namespace modules::current_game_sync
