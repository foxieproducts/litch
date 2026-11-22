#include "modules/board_owner_persistence.hpp"

namespace modules {

BoardOwnerRecord ReadBoardOwner(const JsonDocument& document) {
    if (!document.containsKey(KEY_BOARD_OWNER_V1)) {
        return BoardOwnerRecord::Absent;
    }
    const JsonVariantConst value = document[KEY_BOARD_OWNER_V1];
    if (!value.is<JsonArrayConst>()) {
        return BoardOwnerRecord::Invalid;
    }
    const JsonArrayConst record = value.as<JsonArrayConst>();
    if (record.size() == OWNER_JOURNAL_SLOTS && record[0U].is<uint32_t>() &&
        record[0U].as<uint32_t>() == 3U && record[1U].is<uint32_t>()) {
        const uint32_t state = record[1U].as<uint32_t>();
        if (state == 3U) return ReadBoardOwnerAddBaseline(document).Valid()
            ? BoardOwnerRecord::Adding : BoardOwnerRecord::Invalid;
        if (state > 2U) return BoardOwnerRecord::Invalid;
        for (size_t slot = 2U; slot < OWNER_JOURNAL_SLOTS; ++slot)
            if (!record[slot].is<uint32_t>() || record[slot].as<uint32_t>() != 0U)
                return BoardOwnerRecord::Invalid;
        return static_cast<BoardOwnerRecord>(state);
    }
    if (record.size() != 2U || !record[0].is<uint32_t>() ||
        !record[1].is<uint32_t>()) {
        return BoardOwnerRecord::Invalid;
    }
    const uint32_t version = record[0].as<uint32_t>();
    if (version != 1U && version != 2U) {
        return BoardOwnerRecord::Invalid;
    }
    if (version == 2U && record[1].as<uint32_t>() == 0U) {
        return BoardOwnerRecord::Absent;
    }
    if (record[1].as<uint32_t>() == 1U) {
        return BoardOwnerRecord::Active;
    }
    if (record[1].as<uint32_t>() == 2U) {
        return BoardOwnerRecord::Clearing;
    }
    return BoardOwnerRecord::Invalid;
}

bool WriteBoardOwner(JsonDocument& document, BoardOwnerRecord record) {
    if (document.overflowed()) {
        return false;
    }

    if (record != BoardOwnerRecord::Absent && record != BoardOwnerRecord::Active &&
        record != BoardOwnerRecord::Clearing) {
        return false;
    }
    const uint32_t state = record == BoardOwnerRecord::Absent ? 0U :
        (record == BoardOwnerRecord::Active ? 1U : 2U);
    if (document.containsKey(KEY_BOARD_OWNER_V1)) {
        if (ReadBoardOwner(document) == BoardOwnerRecord::Invalid) return false;
        JsonArray value = document[KEY_BOARD_OWNER_V1].as<JsonArray>();
        value[0].set(value.size() == OWNER_JOURNAL_SLOTS ? 3U : 2U);
        value[1].set(state);
        for (size_t slot = 2U; slot < value.size(); ++slot) value[slot].set(0U);
        return true;
    }
    if (!document.isNull() && !document.is<JsonObject>()) return false;

    // ArduinoJson's monotonic arena cannot reliably roll back a partially
    // allocated replacement. Require enough untouched capacity for the whole
    // initial record before changing the document. Existing scalar slots above
    // need no replacement allocation.
    constexpr size_t requiredCapacity =
        JSON_OBJECT_SIZE(1) + JSON_ARRAY_SIZE(2);
    const size_t capacity = document.capacity();
    const size_t usage = document.memoryUsage();
    if (usage > capacity || capacity - usage < requiredCapacity) {
        return false;
    }

    JsonArray value = document[KEY_BOARD_OWNER_V1].to<JsonArray>();
    if (value.isNull()) {
        return false;
    }
    value.add(2U);
    value.add(state);
    return !document.overflowed();
}

bool IsBoardOwnerSettingKey(std::string_view key) {
    const std::string_view expected(KEY_BOARD_OWNER_V1);
    if (key.size() != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < key.size(); ++i) {
        const char c = key[i] >= 'a' && key[i] <= 'z'
                           ? static_cast<char>(key[i] - 'a' + 'A')
                           : key[i];
        if (c != expected[i]) {
            return false;
        }
    }
    return true;
}

} // namespace modules
