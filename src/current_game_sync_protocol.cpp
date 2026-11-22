#include "modules/current_game_sync_protocol.hpp"

#include <algorithm>

#include "modules/current_game_log.hpp"

namespace modules::current_game_sync {
namespace {

void Write32(uint8_t* out, uint32_t value) {
    for (uint8_t i = 0U; i < 4U; ++i) out[i] = uint8_t(value >> (i * 8U));
}
void Write64(uint8_t* out, uint64_t value) {
    for (uint8_t i = 0U; i < 8U; ++i) out[i] = uint8_t(value >> (i * 8U));
}
uint16_t Read16(const uint8_t* bytes) {
    return uint16_t(bytes[0U]) | uint16_t(uint16_t(bytes[1U]) << 8U);
}
uint32_t Read32(const uint8_t* bytes) {
    uint32_t value = 0U;
    for (uint8_t i = 0U; i < 4U; ++i) value |= uint32_t(bytes[i]) << (i * 8U);
    return value;
}
uint64_t Read64(const uint8_t* bytes) {
    uint64_t value = 0U;
    for (uint8_t i = 0U; i < 8U; ++i) value |= uint64_t(bytes[i]) << (i * 8U);
    return value;
}

} // namespace

bool IsKnownOpcode(const uint8_t opcode) {
    return opcode >= static_cast<uint8_t>(Opcode::Begin) &&
        opcode <= static_cast<uint8_t>(Opcode::Archived);
}

bool IsValidResumeCursor(const ResumeCursor& cursor) {
    const bool fingerprintAbsent = std::all_of(
        cursor.prefixFingerprint.begin(), cursor.prefixFingerprint.end(),
        [](const uint8_t value) { return value == 0U; });
    if (cursor.nextRecordSequence == 0U) return fingerprintAbsent;
    return !fingerprintAbsent &&
        cursor.nextRecordSequence <= current_game_log::MAX_LINE_RECORDS + 1U;
}

bool IsValidDigest(const Digest& digest, const bool allowAbsent) {
    const bool absent = std::all_of(
        digest.fingerprint.begin(), digest.fingerprint.end(),
        [](const uint8_t value) { return value == 0U; });
    return !absent || allowAbsent;
}

bool IsValidBounds(const Bounds& bounds) {
    if ((bounds.flags & ~KNOWN_FLAGS) != 0U) return false;
    const bool present = (bounds.flags & CURRENT_LINE_PRESENT) != 0U;
    if (!present) {
        return bounds.flags == 0U && bounds.lineRevision == 0U &&
            bounds.recordCount == 0U;
    }
    const uint16_t maximumRecords =
        (bounds.flags & LINE_SEALED) != 0U
            ? current_game_log::MAX_LINE_RECORDS
            : current_game_log::MAX_LINE_RECORDS - 1U;
    return bounds.lineRevision != 0U &&
        bounds.lineRevision <= game_history::MAX_LINE_REVISION &&
        bounds.recordCount != 0U &&
        bounds.recordCount <= maximumRecords &&
        ((bounds.flags & LINE_SEALED) == 0U || bounds.recordCount >= 2U);
}

bool EncodeResumeCursor(const ResumeCursor& cursor, uint8_t out[12]) {
    if (out == nullptr || !IsValidResumeCursor(cursor)) return false;
    Write32(out, cursor.nextRecordSequence);
    std::copy(cursor.prefixFingerprint.begin(), cursor.prefixFingerprint.end(),
              out + 4U);
    return true;
}

bool DecodeResumeCursor(const uint8_t* bytes, const size_t length,
                        ResumeCursor& out) {
    if (bytes == nullptr || length != 12U) return false;
    ResumeCursor candidate;
    candidate.nextRecordSequence = Read32(bytes);
    std::copy(bytes + 4U, bytes + 12U,
              candidate.prefixFingerprint.begin());
    if (!IsValidResumeCursor(candidate)) return false;
    out = candidate;
    return true;
}

bool EncodeDigest(const Digest& digest, const bool allowAbsent,
                  uint8_t out[12]) {
    if (out == nullptr || !IsValidDigest(digest, allowAbsent)) return false;
    std::copy(digest.fingerprint.begin(), digest.fingerprint.end(), out);
    return true;
}

bool DecodeDigest(const uint8_t* bytes, const size_t length,
                  const bool allowAbsent, Digest& out) {
    if (bytes == nullptr || length != 12U) return false;
    Digest candidate;
    std::copy(bytes, bytes + 12U, candidate.fingerprint.begin());
    if (!IsValidDigest(candidate, allowAbsent)) return false;
    out = candidate;
    return true;
}

bool EncodeBounds(const Bounds& bounds, uint8_t out[12]) {
    if (out == nullptr || !IsValidBounds(bounds)) return false;
    Write64(out, bounds.lineRevision);
    out[8U] = uint8_t(bounds.recordCount);
    out[9U] = uint8_t(bounds.recordCount >> 8U);
    out[10U] = bounds.flags;
    out[11U] = 0U;
    return true;
}

bool DecodeBounds(const uint8_t* bytes, const size_t length, Bounds& out) {
    if (bytes == nullptr || length != 12U) return false;
    if (bytes[11U] != 0U) return false;
    const Bounds candidate{Read64(bytes), Read16(bytes + 8U), bytes[10U]};
    if (!IsValidBounds(candidate)) return false;
    out = candidate;
    return true;
}

} // namespace modules::current_game_sync
