#include "modules/game_history_persistence.hpp"

#include <cstring>

namespace modules {
namespace {

constexpr char HEX_DIGITS[] = "0123456789abcdef";

int HexNibble(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

void EncodeGameId(const game_history::GameId& id, char out[33]) {
    for (uint8_t index = 0U; index < id.bytes.size(); ++index) {
        out[index * 2U] = HEX_DIGITS[id.bytes[index] >> 4U];
        out[index * 2U + 1U] =
            HEX_DIGITS[id.bytes[index] & 0x0FU];
    }
    out[32U] = '\0';
}

bool DecodeGameId(const char* const encoded,
                  game_history::GameId& out) {
    if (encoded == nullptr || std::strlen(encoded) != 32U) return false;
    game_history::GameId candidate;
    for (uint8_t index = 0U; index < candidate.bytes.size(); ++index) {
        const int high = HexNibble(encoded[index * 2U]);
        const int low = HexNibble(encoded[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        candidate.bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    if (!game_history::IsValidGameId(candidate)) return false;
    out = candidate;
    return true;
}

void EncodeRevision(const uint64_t revision, char out[17]) {
    for (uint8_t index = 0U; index < 16U; ++index) {
        const uint8_t shift = static_cast<uint8_t>((15U - index) * 4U);
        out[index] =
            HEX_DIGITS[static_cast<uint8_t>(revision >> shift) & 0x0FU];
    }
    out[16U] = '\0';
}

bool DecodeRevision(const char* const encoded, uint64_t& out) {
    if (encoded == nullptr || std::strlen(encoded) != 16U) return false;
    uint64_t value = 0U;
    for (uint8_t index = 0U; index < 16U; ++index) {
        const int nibble = HexNibble(encoded[index]);
        if (nibble < 0) return false;
        value = (value << 4U) | static_cast<uint64_t>(nibble);
    }
    if (value > game_history::MAX_LINE_REVISION) return false;
    out = value;
    return true;
}

} // namespace

bool WriteGameHistoryPersistenceState(
    JsonDocument& doc,
    const GameHistoryPersistenceState& state) {
    if (!state.valid) return true;
    if (!state.present) {
        (void)doc.remove(KEY_GAME_HISTORY_V1);
        return true;
    }
    if (!game_history::IsValidGameId(state.gameId) ||
        state.lineRevision > game_history::MAX_LINE_REVISION ||
        state.currentPly > 4096U ||
        (state.shouldSeal
             ? !game_history::IsValidSeal(
                   state.status, state.outcome, state.winner)
             : (state.status != game_history::ArchiveStatus::Unfinished ||
                state.outcome != game_history::Outcome::Ongoing ||
                state.winner != game_history::Winner::None))) {
        return false;
    }
    char gameId[33] = {};
    char revision[17] = {};
    EncodeGameId(state.gameId, gameId);
    EncodeRevision(state.lineRevision, revision);
    JsonArray record = doc[KEY_GAME_HISTORY_V1].to<JsonArray>();
    if (record.isNull()) return false;
    record.add(GAME_HISTORY_PERSISTENCE_VERSION);
    record.add(gameId);
    record.add(revision);
    record.add(state.currentPly);
    record.add(state.captureIncomplete ? 1U : 0U);
    record.add(state.shouldSeal ? 1U : 0U);
    record.add(static_cast<uint8_t>(state.status));
    record.add(static_cast<uint8_t>(state.outcome));
    record.add(static_cast<uint8_t>(state.winner));
    return !doc.overflowed();
}

bool ReadGameHistoryPersistenceState(
    const JsonDocument& doc,
    GameHistoryPersistenceState& out) {
    out = {};
    if (!doc.containsKey(KEY_GAME_HISTORY_V1)) return true;
    const JsonVariantConst value = doc[KEY_GAME_HISTORY_V1];
    if (!value.is<JsonArrayConst>()) return false;
    const JsonArrayConst record = value.as<JsonArrayConst>();
    if (record.size() != 9U ||
        !record[0U].is<uint32_t>() ||
        record[0U].as<uint32_t>() != GAME_HISTORY_PERSISTENCE_VERSION ||
        !record[1U].is<const char*>() ||
        !record[2U].is<const char*>() ||
        !record[3U].is<uint32_t>() ||
        !record[4U].is<uint32_t>() ||
        !record[5U].is<uint32_t>() ||
        !record[6U].is<uint32_t>() ||
        !record[7U].is<uint32_t>() ||
        !record[8U].is<uint32_t>()) {
        return false;
    }
    const uint32_t currentPly = record[3U].as<uint32_t>();
    const uint32_t incomplete = record[4U].as<uint32_t>();
    const uint32_t shouldSeal = record[5U].as<uint32_t>();
    GameHistoryPersistenceState candidate;
    if (record[6U].as<uint32_t>() > UINT8_MAX ||
        record[7U].as<uint32_t>() > UINT8_MAX ||
        record[8U].as<uint32_t>() > UINT8_MAX ||
        currentPly > 4096U || incomplete > 1U || shouldSeal > 1U ||
        !DecodeGameId(record[1U].as<const char*>(), candidate.gameId) ||
        !DecodeRevision(record[2U].as<const char*>(),
                        candidate.lineRevision)) {
        return false;
    }
    candidate.present = true;
    candidate.currentPly = static_cast<uint16_t>(currentPly);
    candidate.captureIncomplete = incomplete != 0U;
    candidate.shouldSeal = shouldSeal != 0U;
    candidate.status = static_cast<game_history::ArchiveStatus>(
        record[6U].as<uint32_t>());
    candidate.outcome = static_cast<game_history::Outcome>(
        record[7U].as<uint32_t>());
    candidate.winner = static_cast<game_history::Winner>(
        record[8U].as<uint32_t>());
    if (candidate.shouldSeal
            ? !game_history::IsValidSeal(
                  candidate.status, candidate.outcome, candidate.winner)
            : (candidate.status != game_history::ArchiveStatus::Unfinished ||
               candidate.outcome != game_history::Outcome::Ongoing ||
               candidate.winner != game_history::Winner::None)) {
        return false;
    }
    out = candidate;
    return true;
}

} // namespace modules
