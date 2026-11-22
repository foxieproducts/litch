#include "modules/game_history_protocol.hpp"

#include <cstring>

#include "foxie_embedded/checksum.hpp"

namespace modules::game_history {
namespace {

constexpr uint8_t KNOWN_TRANSITION_FLAGS =
    HasCapture | HasPromotion | HasSecondaryRelocation |
    HasDuckRelocation | MovingSideBlack;

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

bool ValidSquarePair(const uint8_t from, const uint8_t to) {
    return from < 64U && to < 64U && from != to;
}

bool ValidTransition(const Transition& transition) {
    if (!ValidSquarePair(transition.from, transition.to) ||
        (transition.side != Side::White && transition.side != Side::Black)) {
        return false;
    }
    if (transition.captureSquare != INVALID_SQUARE &&
        transition.captureSquare >= 64U) {
        return false;
    }
    if (transition.promotion > Promotion::Knight) {
        return false;
    }
    const bool hasSecondary =
        transition.secondaryFrom != INVALID_SQUARE ||
        transition.secondaryTo != INVALID_SQUARE;
    if (hasSecondary &&
        !ValidSquarePair(transition.secondaryFrom, transition.secondaryTo)) {
        return false;
    }
    const bool hasDuck = transition.duckFrom != INVALID_SQUARE ||
                         transition.duckTo != INVALID_SQUARE;
    return !hasDuck || ValidSquarePair(transition.duckFrom,
                                       transition.duckTo);
}

bool WriteGamePrefix(const GameId& gameId,
                     const uint64_t lineRevision,
                     uint8_t* const out) {
    if (!IsValidGameId(gameId) || lineRevision > MAX_LINE_REVISION ||
        out == nullptr) {
        return false;
    }
    std::memcpy(out, gameId.bytes.data(), gameId.bytes.size());
    WriteLe64(out + GAME_ID_BYTES, lineRevision);
    return true;
}

bool ReadGamePrefix(const uint8_t* const bytes,
                    GameId& outGameId,
                    uint64_t& outLineRevision) {
    if (bytes == nullptr) {
        return false;
    }
    std::memcpy(outGameId.bytes.data(), bytes, outGameId.bytes.size());
    outLineRevision = ReadLe64(bytes + GAME_ID_BYTES);
    return IsValidGameId(outGameId) &&
           outLineRevision <= MAX_LINE_REVISION;
}

bool ValidEncodedPosition(const Position& position,
                          const Variant variant) {
    fxe::FixedString<litch_max::FEN_BUFFER_CAPACITY> fen;
    uint8_t duckSquare = INVALID_SQUARE;
    return DecodePosition(position, variant, fen, duckSquare);
}

bool KnownRecordType(const RecordType type) {
    return type == RecordType::LineBase ||
           type == RecordType::CommittedPly ||
           type == RecordType::Seal;
}

bool EncodeRecordPayload(const Record& record,
                         uint8_t* const out,
                         const size_t outCapacity,
                         size_t& outLength) {
    outLength = 0U;
    switch (record.type) {
        case RecordType::LineBase: {
            constexpr size_t SIZE = GAME_ID_BYTES + 8U + 2U + 1U +
                                    POSITION_BYTES;
            if (out == nullptr || outCapacity < SIZE ||
                record.ply > 4096U ||
                !IsValidVariant(record.variant) ||
                !ValidEncodedPosition(record.position, record.variant) ||
                !WriteGamePrefix(record.gameId,
                                 record.lineRevision, out)) {
                return false;
            }
            WriteLe16(out + 24U, record.ply);
            out[26U] = static_cast<uint8_t>(record.variant);
            std::memcpy(out + 27U, record.position.bytes.data(),
                        POSITION_BYTES);
            outLength = SIZE;
            return true;
        }
        case RecordType::CommittedPly: {
            size_t transitionLength = 0U;
            uint8_t encodedTransition[MAX_TRANSITION_BYTES] = {};
            if (!EncodeTransition(record.transition,
                                  encodedTransition,
                                  sizeof(encodedTransition),
                                  transitionLength)) {
                return false;
            }
            const size_t size = GAME_ID_BYTES + 8U + 2U + POSITION_BYTES +
                                transitionLength;
            const Variant positionVariant =
                record.position.bytes[POSITION_BYTES - 1U] == INVALID_SQUARE
                    ? Variant::Classic
                    : Variant::Duck;
            if (out == nullptr || outCapacity < size ||
                record.ply == 0U ||
                record.ply > 4096U ||
                !ValidEncodedPosition(record.position, positionVariant) ||
                !WriteGamePrefix(record.gameId,
                                 record.lineRevision, out)) {
                return false;
            }
            WriteLe16(out + 24U, record.ply);
            std::memcpy(out + 26U,
                        record.position.bytes.data(),
                        POSITION_BYTES);
            std::memcpy(out + 69U, encodedTransition, transitionLength);
            outLength = size;
            return true;
        }
        case RecordType::Seal: {
            constexpr size_t SIZE = GAME_ID_BYTES + 8U + 3U;
            if (out == nullptr || outCapacity < SIZE ||
                !IsValidSeal(record.status,
                             record.outcome,
                             record.winner) ||
                !WriteGamePrefix(record.gameId,
                                 record.lineRevision, out)) {
                return false;
            }
            out[24U] = static_cast<uint8_t>(record.status);
            out[25U] = static_cast<uint8_t>(record.outcome);
            out[26U] = static_cast<uint8_t>(record.winner);
            outLength = SIZE;
            return true;
        }
    }
    return false;
}

bool DecodeRecordPayload(const RecordType type,
                         const uint8_t* const bytes,
                         const size_t length,
                         Record& out) {
    if (bytes == nullptr) {
        return false;
    }
    switch (type) {
        case RecordType::LineBase: {
            constexpr size_t SIZE = GAME_ID_BYTES + 8U + 2U + 1U +
                                    POSITION_BYTES;
            if (length != SIZE ||
                !ReadGamePrefix(bytes, out.gameId,
                                out.lineRevision)) {
                return false;
            }
            out.ply = ReadLe16(bytes + 24U);
            out.variant = static_cast<Variant>(bytes[26U]);
            std::memcpy(out.position.bytes.data(), bytes + 27U,
                        POSITION_BYTES);
            return out.ply <= 4096U &&
                   IsValidVariant(out.variant) &&
                   ValidEncodedPosition(out.position, out.variant);
        }
        case RecordType::CommittedPly: {
            constexpr size_t PREFIX_SIZE = GAME_ID_BYTES + 8U + 2U +
                                           POSITION_BYTES;
            if (length < PREFIX_SIZE + 3U ||
                length > PREFIX_SIZE + MAX_TRANSITION_BYTES ||
                !ReadGamePrefix(bytes, out.gameId,
                                out.lineRevision)) {
                return false;
            }
            out.ply = ReadLe16(bytes + 24U);
            std::memcpy(out.position.bytes.data(), bytes + 26U,
                        POSITION_BYTES);
            const Variant variant =
                out.position.bytes[POSITION_BYTES - 1U] == INVALID_SQUARE
                    ? Variant::Classic
                    : Variant::Duck;
            return out.ply != 0U &&
                   out.ply <= 4096U &&
                   ValidEncodedPosition(out.position, variant) &&
                   DecodeTransition(bytes + PREFIX_SIZE,
                                    length - PREFIX_SIZE,
                                    out.transition);
        }
        case RecordType::Seal: {
            constexpr size_t SIZE = GAME_ID_BYTES + 8U + 3U;
            if (length != SIZE ||
                !ReadGamePrefix(bytes, out.gameId,
                                out.lineRevision)) {
                return false;
            }
            out.status = static_cast<ArchiveStatus>(bytes[24U]);
            out.outcome = static_cast<Outcome>(bytes[25U]);
            out.winner = static_cast<Winner>(bytes[26U]);
            return IsValidSeal(out.status,
                               out.outcome,
                               out.winner);
        }
    }
    return false;
}

} // namespace

bool IsValidSeal(const ArchiveStatus status,
                 const Outcome outcome,
                 const Winner winner) {
    if (static_cast<uint8_t>(outcome) > static_cast<uint8_t>(Outcome::Resignation)) {
        return false;
    }
    if (status == ArchiveStatus::Unfinished) {
        return outcome == Outcome::Ongoing && winner == Winner::None;
    }
    if (status != ArchiveStatus::Completed || outcome == Outcome::Ongoing) {
        return false;
    }
    if (outcome == Outcome::Draw) {
        return winner == Winner::Draw;
    }
    return winner == Winner::White || winner == Winner::Black;
}

size_t EncodedTransitionSize(const Transition& transition) {
    if (!ValidTransition(transition)) {
        return 0U;
    }
    size_t size = 3U;
    if (transition.captureSquare != INVALID_SQUARE) ++size;
    if (transition.promotion != Promotion::None) ++size;
    if (transition.secondaryFrom != INVALID_SQUARE) size += 2U;
    if (transition.duckFrom != INVALID_SQUARE) size += 2U;
    return size;
}

bool EncodeTransition(const Transition& transition,
                      uint8_t* const out,
                      const size_t outCapacity,
                      size_t& outLength) {
    outLength = 0U;
    const size_t size = EncodedTransitionSize(transition);
    if (size == 0U || out == nullptr || outCapacity < size) {
        return false;
    }
    uint8_t flags = transition.side == Side::Black ? MovingSideBlack : 0U;
    if (transition.captureSquare != INVALID_SQUARE) flags |= HasCapture;
    if (transition.promotion != Promotion::None) flags |= HasPromotion;
    if (transition.secondaryFrom != INVALID_SQUARE) {
        flags |= HasSecondaryRelocation;
    }
    if (transition.duckFrom != INVALID_SQUARE) flags |= HasDuckRelocation;

    size_t cursor = 0U;
    out[cursor++] = flags;
    out[cursor++] = transition.from;
    out[cursor++] = transition.to;
    if ((flags & HasCapture) != 0U) out[cursor++] = transition.captureSquare;
    if ((flags & HasPromotion) != 0U) {
        out[cursor++] = static_cast<uint8_t>(transition.promotion);
    }
    if ((flags & HasSecondaryRelocation) != 0U) {
        out[cursor++] = transition.secondaryFrom;
        out[cursor++] = transition.secondaryTo;
    }
    if ((flags & HasDuckRelocation) != 0U) {
        out[cursor++] = transition.duckFrom;
        out[cursor++] = transition.duckTo;
    }
    outLength = cursor;
    return cursor == size;
}

bool DecodeTransition(const uint8_t* const bytes,
                      const size_t length,
                      Transition& out) {
    if (bytes == nullptr || length < 3U || length > MAX_TRANSITION_BYTES) {
        return false;
    }
    const uint8_t flags = bytes[0];
    if ((flags & static_cast<uint8_t>(~KNOWN_TRANSITION_FLAGS)) != 0U) {
        return false;
    }
    Transition candidate;
    candidate.side = (flags & MovingSideBlack) != 0U
                         ? Side::Black
                         : Side::White;
    candidate.from = bytes[1];
    candidate.to = bytes[2];
    size_t cursor = 3U;
    if ((flags & HasCapture) != 0U) {
        if (cursor >= length) return false;
        candidate.captureSquare = bytes[cursor++];
    }
    if ((flags & HasPromotion) != 0U) {
        if (cursor >= length) return false;
        candidate.promotion = static_cast<Promotion>(bytes[cursor++]);
    }
    if ((flags & HasSecondaryRelocation) != 0U) {
        if (cursor + 2U > length) return false;
        candidate.secondaryFrom = bytes[cursor++];
        candidate.secondaryTo = bytes[cursor++];
    }
    if ((flags & HasDuckRelocation) != 0U) {
        if (cursor + 2U > length) return false;
        candidate.duckFrom = bytes[cursor++];
        candidate.duckTo = bytes[cursor++];
    }
    if (cursor != length || EncodedTransitionSize(candidate) != length) {
        return false;
    }
    out = candidate;
    return true;
}

bool EncodeRecord(const Record& record,
                  uint8_t* const out,
                  const size_t outCapacity,
                  size_t& outLength) {
    outLength = 0U;
    if (out == nullptr || outCapacity < RECORD_HEADER_BYTES +
                                      RECORD_CRC_BYTES ||
        !KnownRecordType(record.type) || record.outboxSequence == 0U) {
        return false;
    }
    size_t payloadLength = 0U;
    if (!EncodeRecordPayload(record,
                             out + RECORD_HEADER_BYTES,
                             outCapacity - RECORD_HEADER_BYTES -
                                 RECORD_CRC_BYTES,
                             payloadLength) ||
        payloadLength > UINT8_MAX) {
        return false;
    }
    out[0] = PROTOCOL_VERSION;
    out[1] = static_cast<uint8_t>(record.type);
    out[2] = static_cast<uint8_t>(payloadLength);
    WriteLe32(out + 3U, record.outboxSequence);
    const size_t crcOffset = RECORD_HEADER_BYTES + payloadLength;
    WriteLe32(out + crcOffset, fxe::Crc32(out, crcOffset));
    outLength = crcOffset + RECORD_CRC_BYTES;
    return outLength <= MAX_RECORD_BYTES;
}

bool DecodeRecord(const uint8_t* const bytes,
                  const size_t length,
                  Record& out) {
    if (bytes == nullptr || length < RECORD_HEADER_BYTES + RECORD_CRC_BYTES ||
        length > MAX_RECORD_BYTES || bytes[0] != PROTOCOL_VERSION) {
        return false;
    }
    const RecordType type = static_cast<RecordType>(bytes[1]);
    const size_t payloadLength = bytes[2];
    const size_t crcOffset = RECORD_HEADER_BYTES + payloadLength;
    if (!KnownRecordType(type) || crcOffset + RECORD_CRC_BYTES != length ||
        ReadLe32(bytes + crcOffset) != fxe::Crc32(bytes, crcOffset)) {
        return false;
    }
    Record candidate;
    candidate.type = type;
    candidate.outboxSequence = ReadLe32(bytes + 3U);
    if (candidate.outboxSequence == 0U ||
        !DecodeRecordPayload(type, bytes + RECORD_HEADER_BYTES,
                             payloadLength, candidate)) {
        return false;
    }
    out = candidate;
    return true;
}

} // namespace modules::game_history
