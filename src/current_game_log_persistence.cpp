#include "modules/current_game_log_persistence.hpp"

#include <cstring>

namespace modules::current_game_log {
namespace {

constexpr char HEX_DIGITS[] = "0123456789abcdef";

int HexNibble(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

template <typename Integer>
void EncodeHex(const Integer value, char* const out, const size_t digits) {
    for (size_t index = 0U; index < digits; ++index) {
        const size_t shift = (digits - index - 1U) * 4U;
        out[index] = HEX_DIGITS[static_cast<uint8_t>(value >> shift) & 0x0FU];
    }
    out[digits] = '\0';
}

bool DecodeHex(const char* const encoded,
               const size_t digits,
               uint64_t& out) {
    if (encoded == nullptr || std::strlen(encoded) != digits) return false;
    uint64_t value = 0U;
    for (size_t index = 0U; index < digits; ++index) {
        const int nibble = HexNibble(encoded[index]);
        if (nibble < 0) return false;
        value = (value << 4U) | static_cast<uint64_t>(nibble);
    }
    out = value;
    return true;
}

bool Valid(const CommitPointer& pointer) {
    return pointer.present &&
           (pointer.slot == Slot::A || pointer.slot == Slot::B) &&
           pointer.generation != 0U && pointer.byteLength != 0U &&
           pointer.byteLength <= MAX_SLOT_BYTES &&
           game_history::IsValidGameId(pointer.gameId) &&
           pointer.lineRevision <= game_history::MAX_LINE_REVISION &&
           pointer.currentPly <= MAX_PLIES;
}

} // namespace

bool WriteCommitPointer(JsonDocument& document,
                        const CommitPointer& pointer) {
    if (document.overflowed()) return false;
    const JsonVariantConst root = document.as<JsonVariantConst>();
    if (!root.isNull() && !root.is<JsonObjectConst>()) return false;
    if (!pointer.present) {
        document.remove(KEY_CURRENT_GAME_LOG_V2);
        return true;
    }
    if (!Valid(pointer)) return false;

    constexpr size_t required = JSON_OBJECT_SIZE(1) + JSON_ARRAY_SIZE(9) +
                                33U + 17U;
    const size_t capacity = document.capacity();
    const size_t usage = document.memoryUsage();
    if (usage > capacity || capacity - usage < required) return false;

    char gameId[33] = {};
    for (size_t index = 0U; index < pointer.gameId.bytes.size(); ++index) {
        gameId[index * 2U] = HEX_DIGITS[pointer.gameId.bytes[index] >> 4U];
        gameId[index * 2U + 1U] =
            HEX_DIGITS[pointer.gameId.bytes[index] & 0x0FU];
    }
    char revision[17] = {};
    EncodeHex(pointer.lineRevision, revision, 16U);

    document.remove(KEY_CURRENT_GAME_LOG_V2);
    JsonArray record = document[KEY_CURRENT_GAME_LOG_V2].to<JsonArray>();
    if (record.isNull()) return false;
    record.add(COMMIT_POINTER_VERSION);
    record.add(static_cast<uint8_t>(pointer.slot));
    record.add(pointer.generation);
    record.add(pointer.byteLength);
    record.add(pointer.prefixCrc32);
    record.add(gameId);
    record.add(revision);
    record.add(pointer.currentPly);
    record.add(pointer.archiveRequired ? 1U : 0U);
    return !document.overflowed();
}

bool ReadCommitPointer(const JsonDocument& document,
                       CommitPointer& out) {
    out = {};
    if (!document.containsKey(KEY_CURRENT_GAME_LOG_V2)) return true;
    const JsonVariantConst value = document[KEY_CURRENT_GAME_LOG_V2];
    if (!value.is<JsonArrayConst>()) return false;
    const JsonArrayConst record = value.as<JsonArrayConst>();
    if (record.size() != 9U || !record[0U].is<uint32_t>() ||
        record[0U].as<uint32_t>() != COMMIT_POINTER_VERSION ||
        !record[1U].is<uint32_t>() || !record[2U].is<uint32_t>() ||
        !record[3U].is<uint32_t>() || !record[4U].is<uint32_t>() ||
        !record[5U].is<const char*>() || !record[6U].is<const char*>() ||
        !record[7U].is<uint32_t>() || !record[8U].is<uint32_t>()) {
        return false;
    }

    const uint32_t slot = record[1U].as<uint32_t>();
    const uint32_t ply = record[7U].as<uint32_t>();
    const uint32_t archiveRequired = record[8U].as<uint32_t>();
    uint64_t revision = 0U;
    CommitPointer candidate;
    if (slot > 1U || ply > MAX_PLIES || archiveRequired > 1U ||
        !DecodeHex(record[6U].as<const char*>(), 16U, revision) ||
        revision > game_history::MAX_LINE_REVISION) {
        return false;
    }
    const char* const gameId = record[5U].as<const char*>();
    if (gameId == nullptr || std::strlen(gameId) != 32U) return false;
    for (size_t index = 0U; index < candidate.gameId.bytes.size(); ++index) {
        const int high = HexNibble(gameId[index * 2U]);
        const int low = HexNibble(gameId[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        candidate.gameId.bytes[index] =
            static_cast<uint8_t>((high << 4U) | low);
    }
    candidate.present = true;
    candidate.slot = static_cast<Slot>(slot);
    candidate.generation = record[2U].as<uint32_t>();
    candidate.byteLength = record[3U].as<uint32_t>();
    candidate.prefixCrc32 = record[4U].as<uint32_t>();
    candidate.lineRevision = revision;
    candidate.currentPly = static_cast<uint16_t>(ply);
    candidate.archiveRequired = archiveRequired != 0U;
    if (!Valid(candidate)) return false;
    out = candidate;
    return true;
}

} // namespace modules::current_game_log
