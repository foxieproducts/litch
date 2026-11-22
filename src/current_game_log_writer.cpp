#include "modules/current_game_log.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "foxie_embedded/checksum.hpp"

namespace modules::current_game_log {
namespace {

// Bounded writer workspace; never holds a flash lease during source replay.
constexpr size_t WRITE_CHUNK_BYTES = 512U;

Result WriteChunk(const StorageOps &storage, const Slot slot, const uint8_t *const bytes,
                  const size_t length, const uint32_t offset, uint32_t &crc) {
    const bool written = offset == 0U ? storage.replace(storage.context, slot, bytes, length)
                                      : storage.append(storage.context, slot, bytes, length);
    if (!written)
        return Result::StorageFailure;
    std::array<uint8_t, game_history::MAX_RECORD_BYTES> readback{};
    for (size_t at = 0U; at < length;) {
        const size_t count = std::min(readback.size(), length - at);
        if (!storage.read(storage.context, slot, offset + static_cast<uint32_t>(at),
                          readback.data(), count))
            return Result::IoFailure;
        if (std::memcmp(bytes + at, readback.data(), count) != 0) {
            return Result::ReadbackMismatch;
        }
        crc = fxe::Crc32(readback.data(), count, ~crc);
        at += count;
    }
    return Result::Ready;
}

bool WriteOpsReady(const StorageOps &storage) {
    return storage.context != nullptr && storage.stat != nullptr && storage.read != nullptr &&
           storage.replace != nullptr && storage.append != nullptr && storage.truncate != nullptr;
}

game_history::Variant LineBaseVariant(const game_history::Record &record) {
    return record.position.bytes[game_history::POSITION_BYTES - 1U] == game_history::INVALID_SQUARE
               ? game_history::Variant::Classic
               : game_history::Variant::Duck;
}

} // namespace

Result CurrentGameLog::WriteLine(const Slot slot, const LineSource &source,
                                 CommitPointer &outCandidate, game_history::Record &outLast) const {
    outCandidate = {};
    outLast = {};
    if (!WriteOpsReady(m_storage) || source.context == nullptr || source.readRecord == nullptr ||
        source.recordCount == 0U || source.recordCount > MAX_LINE_RECORDS) {
        return Result::Bounds;
    }

    uint32_t crc = 0U;
    std::array<uint8_t, WRITE_CHUNK_BYTES> chunk{};
    size_t chunkLength = 0U;
    uint32_t totalBytes = 0U;
    game_history::Record first;
    game_history::Record prior;
    game_history::Position priorPosition{};
    game_history::Variant variant = game_history::Variant::Classic;
    uint16_t lastPositionPly = 0U;
    bool sealed = false;
    for (uint16_t index = 0U; index < source.recordCount; ++index) {
        game_history::Record record;
        if (!source.readRecord(source.context, index, record)) {
            return Result::Bounds;
        }
        if (index == 0U) {
            if (record.type != game_history::RecordType::LineBase || record.outboxSequence != 1U ||
                record.ply != 0U || !game_history::IsValidVariant(record.variant) ||
                LineBaseVariant(record) != record.variant) {
                return Result::Corrupt;
            }
            first = record;
            variant = record.variant;
            lastPositionPly = record.ply;
        } else if (record.type == game_history::RecordType::CommittedPly) {
            if (sealed || record.outboxSequence != static_cast<uint32_t>(index + 1U) ||
                !(record.gameId == first.gameId) ||
                record.ply != static_cast<uint16_t>(lastPositionPly + 1U) ||
                record.lineRevision <= prior.lineRevision ||
                !IsValidCommittedPlyForVariant(variant, priorPosition, record)) {
                return Result::Corrupt;
            }
            lastPositionPly = record.ply;
        } else if (record.type == game_history::RecordType::Seal) {
            if (sealed || index + 1U != source.recordCount ||
                record.outboxSequence != static_cast<uint32_t>(index + 1U) ||
                !(record.gameId == first.gameId) ||
                record.status != game_history::ArchiveStatus::Completed ||
                record.lineRevision != prior.lineRevision) {
                return Result::Corrupt;
            }
            sealed = true;
        } else {
            return Result::Corrupt;
        }
        if (chunk.size() - chunkLength < game_history::MAX_RECORD_BYTES) {
            const Result flushed =
                WriteChunk(m_storage, slot, chunk.data(), chunkLength, totalBytes, crc);
            if (flushed != Result::Ready)
                return flushed;
            totalBytes += static_cast<uint32_t>(chunkLength);
            chunkLength = 0U;
        }
        size_t encodedLength = 0U;
        if (!game_history::EncodeRecord(record, chunk.data() + chunkLength,
                                        chunk.size() - chunkLength, encodedLength) ||
            chunkLength > MAX_SLOT_BYTES - totalBytes ||
            encodedLength > MAX_SLOT_BYTES - totalBytes - chunkLength) {
            return Result::Bounds;
        }
        chunkLength += encodedLength;
        prior = record;
        if (record.type != game_history::RecordType::Seal) {
            priorPosition = record.position;
        }
    }

    if (chunkLength != 0U) {
        const Result flushed =
            WriteChunk(m_storage, slot, chunk.data(), chunkLength, totalBytes, crc);
        if (flushed != Result::Ready)
            return flushed;
        totalBytes += static_cast<uint32_t>(chunkLength);
    }

    outCandidate.present = true;
    outCandidate.slot = slot;
    outCandidate.byteLength = totalBytes;
    outCandidate.prefixCrc32 = crc;
    outCandidate.gameId = first.gameId;
    outCandidate.lineRevision = prior.lineRevision;
    outCandidate.currentPly = lastPositionPly;
    outLast = prior;
    return Result::Ready;
}

Result CurrentGameLog::Initialize(const game_history::Record &base, const bool archiveRequired,
                                  CommitPointer &outCandidate, SettingsOps::Receipt &outReceipt) {
    outCandidate = {};
    outReceipt = 0U;
    if (m_open || m_pendingReceipt != 0U)
        return Result::BadState;
    struct SourceContext {
        const game_history::Record *record;
    } context{&base};
    const LineSource source{&context, 1U, [](void *raw, uint16_t index, game_history::Record &out) {
                                if (raw == nullptr || index != 0U)
                                    return false;
                                out = *static_cast<SourceContext *>(raw)->record;
                                return true;
                            }};
    return InitializeLine(source, archiveRequired, outCandidate, outReceipt);
}

Result CurrentGameLog::InitializeLine(const LineSource &source, const bool archiveRequired,
                                      CommitPointer &outCandidate,
                                      SettingsOps::Receipt &outReceipt) {
    outCandidate = {};
    outReceipt = 0U;
    if (m_open || m_pendingReceipt != 0U)
        return Result::BadState;
    CommitPointer candidate;
    game_history::Record latest;
    const Result written = WriteLine(Slot::A, source, candidate, latest);
    if (written != Result::Ready)
        return written;
    candidate.generation = 1U;
    candidate.archiveRequired = archiveRequired;
    const Result validated = Validate(candidate, nullptr);
    if (validated != Result::Ready) {
        return validated;
    }
    game_history::Record base;
    if (!source.readRecord(source.context, 0U, base)) {
        return Result::Bounds;
    }
    return Enqueue(candidate, latest, base.variant, outCandidate, outReceipt);
}

Result CurrentGameLog::Rewrite(const CommitPointer &authority, const LineSource &source,
                               CommitPointer &outCandidate, SettingsOps::Receipt &outReceipt) {
    const CommitPointer current = authority;
    outCandidate = {};
    outReceipt = 0U;
    if (!IsOpenAuthority(current))
        return Result::BadState;
    if (current.generation == UINT32_MAX)
        return Result::Bounds;
    game_history::Record requestedBase;
    if (source.context == nullptr || source.readRecord == nullptr ||
        !source.readRecord(source.context, 0U, requestedBase) ||
        requestedBase.type != game_history::RecordType::LineBase ||
        !(requestedBase.gameId == current.gameId) || requestedBase.variant != m_openVariant) {
        return Result::Bounds;
    }
    CommitPointer candidate;
    const Slot inactive = current.slot == Slot::A ? Slot::B : Slot::A;
    game_history::Record latest;
    const Result written = WriteLine(inactive, source, candidate, latest);
    if (written != Result::Ready)
        return written;
    candidate.generation = current.generation + 1U;
    candidate.archiveRequired = true;
    const Result validated = Validate(candidate, nullptr);
    if (validated != Result::Ready) {
        return validated;
    }
    return Enqueue(candidate, latest, m_openVariant, outCandidate, outReceipt);
}

Result CurrentGameLog::ReplaceLine(const CommitPointer &authority, const LineSource &source,
                                   const bool archiveRequired, CommitPointer &outCandidate,
                                   SettingsOps::Receipt &outReceipt) {
    const CommitPointer current = authority;
    outCandidate = {};
    outReceipt = 0U;
    if (!IsOpenAuthority(current))
        return Result::BadState;
    if (current.generation == UINT32_MAX)
        return Result::Bounds;
    const Slot inactive = current.slot == Slot::A ? Slot::B : Slot::A;
    CommitPointer candidate;
    game_history::Record latest;
    const Result written = WriteLine(inactive, source, candidate, latest);
    if (written != Result::Ready)
        return written;
    candidate.generation = current.generation + 1U;
    candidate.archiveRequired = archiveRequired;
    game_history::Record base;
    const Result validated = Validate(candidate, nullptr);
    if (validated != Result::Ready) {
        return validated;
    }
    if (!source.readRecord(source.context, 0U, base))
        return Result::Bounds;
    return Enqueue(candidate, latest, base.variant, outCandidate, outReceipt);
}

} // namespace modules::current_game_log
