#include "modules/current_game_log.hpp"

#include <array>
#include <cstring>

#include "modules/game_history_protocol.hpp"

namespace modules::current_game_log {
namespace {

class Crc32 final {
  public:
    explicit Crc32(const uint32_t state = 0xFFFFFFFFU)
        : m_state(state) {}

    void Update(const uint8_t* const bytes, const size_t length) {
        for (size_t index = 0U; index < length; ++index) {
            m_state ^= static_cast<uint32_t>(bytes[index]);
            for (uint8_t bit = 0U; bit < 8U; ++bit) {
                const uint32_t mask = (m_state & 1U) != 0U
                                          ? 0xEDB88320U
                                          : 0U;
                m_state = (m_state >> 1U) ^ mask;
            }
        }
    }

    uint32_t Value() const { return ~m_state; }

  private:
    uint32_t m_state;
};

uint32_t ExtendCrc32(const uint32_t prefixCrc,
                     const uint8_t* const bytes,
                     const size_t length) {
    Crc32 crc(~prefixCrc);
    crc.Update(bytes, length);
    return crc.Value();
}

bool OpsReady(const StorageOps& storage) {
    return storage.context != nullptr && storage.stat != nullptr &&
           storage.read != nullptr && storage.replace != nullptr &&
           storage.append != nullptr && storage.truncate != nullptr;
}

bool SameGame(const game_history::GameId& left,
              const game_history::GameId& right) {
    return left == right;
}

game_history::Variant PositionVariant(const game_history::Record& record) {
    return record.position.bytes[game_history::POSITION_BYTES - 1U] ==
                   game_history::INVALID_SQUARE
               ? game_history::Variant::Classic
               : game_history::Variant::Duck;
}

uint8_t PieceAt(const game_history::Position& position,
                const uint8_t square) {
    if (square >= 64U) return 0U;
    const uint8_t packed = position.bytes[square / 2U];
    return (square & 1U) == 0U
               ? static_cast<uint8_t>(packed & 0x0FU)
               : static_cast<uint8_t>(packed >> 4U);
}

uint8_t CountPiece(const game_history::Position& position,
                   const uint8_t piece) {
    uint8_t count = 0U;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if (PieceAt(position, square) == piece) ++count;
    }
    return count;
}

} // namespace

bool IsValidCommittedPlyForVariant(
    const game_history::Variant variant,
    const game_history::Position& prior,
    const game_history::Record& record) {
    if (PositionVariant(record) != variant) return false;
    const auto& move = record.transition;
    if (variant == game_history::Variant::Classic) {
        return move.duckFrom == game_history::INVALID_SQUARE &&
               move.duckTo == game_history::INVALID_SQUARE;
    }

    const uint8_t priorDuck =
        prior.bytes[game_history::POSITION_BYTES - 1U];
    const uint8_t afterDuck =
        record.position.bytes[game_history::POSITION_BYTES - 1U];
    const uint8_t capturedKing = move.side == game_history::Side::White
                                     ? 12U
                                     : 6U;
    const bool capturesOpposingKing =
        move.captureSquare == move.to &&
        PieceAt(prior, move.captureSquare) == capturedKing;
    if (capturesOpposingKing) {
        return move.duckFrom == game_history::INVALID_SQUARE &&
               move.duckTo == game_history::INVALID_SQUARE &&
               CountPiece(prior, capturedKing) == 1U &&
               CountPiece(record.position, capturedKing) == 0U &&
               afterDuck == priorDuck;
    }

    const bool relocated = move.duckFrom == priorDuck &&
                           move.duckTo == afterDuck &&
                           move.duckFrom < 64U && move.duckTo < 64U &&
                           move.duckFrom != move.duckTo &&
                           move.from != priorDuck && move.to != priorDuck;
    if (relocated) return true;
    return false;
}

namespace {

bool SamePointer(const CommitPointer& left, const CommitPointer& right) {
    return left.present == right.present && left.slot == right.slot &&
           left.generation == right.generation &&
           left.byteLength == right.byteLength &&
           left.prefixCrc32 == right.prefixCrc32 &&
           left.gameId == right.gameId &&
           left.lineRevision == right.lineRevision &&
           left.currentPly == right.currentPly &&
           left.archiveRequired == right.archiveRequired;
}

} // namespace

Result CurrentGameLog::Validate(const CommitPointer& authority,
                                game_history::Record* const outLast,
                                game_history::Variant* const outVariant) const {
    if (!OpsReady(m_storage) || !authority.present ||
        (authority.slot != Slot::A && authority.slot != Slot::B) ||
        authority.generation == 0U || authority.byteLength == 0U ||
        authority.byteLength > MAX_SLOT_BYTES ||
        authority.currentPly > MAX_PLIES ||
        !game_history::IsValidGameId(authority.gameId)) {
        return Result::Bounds;
    }
    uint32_t storedBytes = 0U;
    const StorageStatus status =
        m_storage.stat(m_storage.context, authority.slot, storedBytes);
    if (status == StorageStatus::Missing) return Result::Missing;
    if (status != StorageStatus::Present) return Result::IoFailure;
    if (storedBytes < authority.byteLength) return Result::Corrupt;

    uint32_t offset = 0U;
    uint32_t expectedSequence = 1U;
    uint16_t priorPly = 0U;
    uint64_t priorRevision = 0U;
    bool havePosition = false;
    bool sealed = false;
    game_history::Variant variant = game_history::Variant::Classic;
    game_history::Position priorPosition{};
    Crc32 crc;
    game_history::Record last;
    while (offset < authority.byteLength) {
        uint8_t header[game_history::RECORD_HEADER_BYTES] = {};
        if (authority.byteLength - offset < sizeof(header) ||
            !m_storage.read(m_storage.context, authority.slot, offset,
                            header, sizeof(header))) {
            return Result::IoFailure;
        }
        const size_t recordLength = game_history::RECORD_HEADER_BYTES +
                                    static_cast<size_t>(header[2U]) +
                                    game_history::RECORD_CRC_BYTES;
        if (recordLength > game_history::MAX_RECORD_BYTES ||
            recordLength > authority.byteLength - offset) {
            return Result::Corrupt;
        }
        std::array<uint8_t, game_history::MAX_RECORD_BYTES> encoded{};
        if (!m_storage.read(m_storage.context, authority.slot, offset,
                            encoded.data(), recordLength)) {
            return Result::IoFailure;
        }
        game_history::Record record;
        if (!game_history::DecodeRecord(encoded.data(), recordLength, record) ||
            record.outboxSequence != expectedSequence ||
            !SameGame(record.gameId, authority.gameId)) {
            return Result::Corrupt;
        }
        if (expectedSequence == 1U) {
            if (record.type != game_history::RecordType::LineBase ||
                record.ply != 0U ||
                !game_history::IsValidVariant(record.variant) ||
                PositionVariant(record) != record.variant) {
                return Result::Corrupt;
            }
            variant = record.variant;
            havePosition = true;
        } else if (record.type == game_history::RecordType::CommittedPly) {
            if (sealed || record.ply != static_cast<uint16_t>(priorPly + 1U) ||
                record.lineRevision <= priorRevision ||
                !IsValidCommittedPlyForVariant(
                    variant, priorPosition, record)) {
                return Result::Corrupt;
            }
            havePosition = true;
        } else if (record.type == game_history::RecordType::Seal) {
            if (!havePosition || sealed || record.lineRevision != priorRevision ||
                record.status != game_history::ArchiveStatus::Completed ||
                offset + recordLength != authority.byteLength) {
                return Result::Corrupt;
            }
            sealed = true;
        } else {
            return Result::Corrupt;
        }
        crc.Update(encoded.data(), recordLength);
        last = record;
        if (record.type != game_history::RecordType::Seal) {
            priorPly = record.ply;
            priorPosition = record.position;
        }
        priorRevision = record.lineRevision;
        offset += static_cast<uint32_t>(recordLength);
        ++expectedSequence;
    }
    if (offset != authority.byteLength ||
        priorPly != authority.currentPly ||
        last.lineRevision != authority.lineRevision ||
        crc.Value() != authority.prefixCrc32) {
        return Result::Corrupt;
    }
    if (outLast != nullptr) *outLast = last;
    if (outVariant != nullptr) *outVariant = variant;
    return Result::Ready;
}

Result CurrentGameLog::Open(const CommitPointer& authority) {
    if (m_pendingReceipt != 0U) return Result::BadState;
    game_history::Record latest;
    game_history::Variant variant = game_history::Variant::Classic;
    const Result result = Validate(authority, &latest, &variant);
    if (result != Result::Ready) {
        m_open = false;
        m_openAuthority = {};
        m_latest = {};
        return result;
    }
    m_openAuthority = authority;
    m_latest = latest;
    m_openVariant = variant;
    m_open = true;
    return Result::Ready;
}

Result CurrentGameLog::ReadLatest(
    const CommitPointer& authority,
    game_history::Record& out) const {
    if (!IsOpenAuthority(authority)) return Result::BadState;
    out = m_latest;
    return Result::Ready;
}

Result CurrentGameLog::Enqueue(const CommitPointer& candidate,
                               const game_history::Record& latest,
                               const game_history::Variant variant,
                               CommitPointer& outCandidate,
                               SettingsOps::Receipt& outReceipt) {
    outCandidate = {};
    outReceipt = 0U;
    if (m_settings.context == nullptr ||
        m_settings.enqueueTracked == nullptr ||
        !m_settings.enqueueTracked(m_settings.context, candidate, outReceipt) ||
        outReceipt == 0U) {
        outReceipt = 0U;
        return Result::SettingsRejected;
    }
    outCandidate = candidate;
    m_pendingAuthority = candidate;
    m_pendingLatest = latest;
    m_pendingVariant = variant;
    m_pendingReceipt = outReceipt;
    return Result::Applied;
}

bool CurrentGameLog::IsOpenAuthority(
    const CommitPointer& authority) const {
    return m_open && m_pendingReceipt == 0U &&
           SamePointer(authority, m_openAuthority);
}

bool CurrentGameLog::ConfirmPromotion(
    const CommitPointer& authority,
    const SettingsOps::Receipt receipt) {
    if (receipt == 0U || receipt != m_pendingReceipt ||
        !SamePointer(authority, m_pendingAuthority)) {
        return false;
    }
    m_openAuthority = m_pendingAuthority;
    m_latest = m_pendingLatest;
    m_openVariant = m_pendingVariant;
    m_pendingAuthority = {};
    m_pendingLatest = {};
    m_pendingVariant = game_history::Variant::Classic;
    m_pendingReceipt = 0U;
    m_open = true;
    return true;
}

bool CurrentGameLog::RejectPromotion(
    const SettingsOps::Receipt receipt) {
    if (receipt == 0U || receipt != m_pendingReceipt) return false;
    m_pendingAuthority = {};
    m_pendingLatest = {};
    m_pendingVariant = game_history::Variant::Classic;
    m_pendingReceipt = 0U;
    return true;
}

Result CurrentGameLog::Append(const CommitPointer& authority,
                              const game_history::Record& record,
                              CommitPointer& outCandidate,
                              SettingsOps::Receipt& outReceipt) {
    const CommitPointer current = authority;
    outCandidate = {};
    outReceipt = 0U;
    if (!IsOpenAuthority(current)) return Result::BadState;
    const game_history::Record& latest = m_latest;
    const bool committedPly =
        latest.type != game_history::RecordType::Seal &&
        record.type == game_history::RecordType::CommittedPly &&
        current.currentPly < MAX_PLIES &&
        record.ply == static_cast<uint16_t>(current.currentPly + 1U) &&
        record.lineRevision > current.lineRevision &&
        IsValidCommittedPlyForVariant(
            m_openVariant, latest.position, record);
    const bool terminalSeal =
        latest.type != game_history::RecordType::Seal &&
        record.type == game_history::RecordType::Seal &&
        record.lineRevision == current.lineRevision &&
        record.status == game_history::ArchiveStatus::Completed &&
        game_history::IsValidSeal(record.status, record.outcome,
                                  record.winner);
    if (!SameGame(record.gameId, current.gameId) ||
        record.outboxSequence != latest.outboxSequence + 1U ||
        (!committedPly && !terminalSeal)) {
        return Result::Bounds;
    }
    std::array<uint8_t, game_history::MAX_RECORD_BYTES> encoded{};
    size_t encodedLength = 0U;
    if (!game_history::EncodeRecord(record, encoded.data(), encoded.size(),
                                    encodedLength) ||
        encodedLength > MAX_SLOT_BYTES - current.byteLength) {
        return Result::Bounds;
    }
    uint32_t storedBytes = 0U;
    if (m_storage.stat(m_storage.context, current.slot, storedBytes) !=
            StorageStatus::Present ||
        storedBytes < current.byteLength) {
        return Result::IoFailure;
    }
    if (storedBytes > current.byteLength &&
        !m_storage.truncate(m_storage.context, current.slot,
                            current.byteLength)) {
        return Result::StorageFailure;
    }
    if (!m_storage.append(m_storage.context, current.slot, encoded.data(),
                          encodedLength)) {
        return Result::StorageFailure;
    }
    std::array<uint8_t, game_history::MAX_RECORD_BYTES> readback{};
    if (!m_storage.read(m_storage.context, current.slot,
                        current.byteLength, readback.data(), encodedLength)) {
        return Result::IoFailure;
    }
    if (std::memcmp(encoded.data(), readback.data(), encodedLength) != 0) {
        return Result::ReadbackMismatch;
    }

    CommitPointer candidate = current;
    candidate.byteLength += static_cast<uint32_t>(encodedLength);
    candidate.prefixCrc32 = ExtendCrc32(
        current.prefixCrc32, readback.data(), encodedLength);
    candidate.lineRevision = record.lineRevision;
    if (committedPly) candidate.currentPly = record.ply;
    candidate.archiveRequired = true;
    return Enqueue(candidate, record, m_openVariant,
                   outCandidate, outReceipt);
}

Result CurrentGameLog::AcknowledgeArchived(
    const CommitPointer& authority,
    CommitPointer& outCandidate,
    SettingsOps::Receipt& outReceipt) {
    const CommitPointer current = authority;
    outCandidate = {};
    outReceipt = 0U;
    if (!IsOpenAuthority(current)) return Result::BadState;
    if (!current.archiveRequired) return Result::Ready;
    CommitPointer candidate = current;
    candidate.archiveRequired = false;
    return Enqueue(candidate, m_latest, m_openVariant,
                   outCandidate, outReceipt);
}

} // namespace modules::current_game_log
