#include "modules/game_persistence.hpp"

#include <cstring>

namespace modules {

// Convert square index array to 64-bit bitmask.
uint64_t SquareArrayToMask64(const uint8_t* squares, uint8_t count) {
    uint64_t mask = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (squares[i] < runtime::BOARD_SQUARE_COUNT) {
            mask |= (1ULL << squares[i]);
        }
    }
    return mask;
}

// Convert 64-bit bitmask to ascending square index array.
uint8_t Mask64ToSquareArray(const uint64_t mask,
                                   uint8_t* outSquares,
                                   const uint8_t outCapacity) {
    uint8_t count = 0;
    for (uint8_t sq = 0; sq < runtime::BOARD_SQUARE_COUNT && count < outCapacity; ++sq) {
        if (mask & (1ULL << sq)) {
            outSquares[count++] = sq;
        }
    }
    return count;
}

// Write a 64-bit bitmask as a 16-character hex string (MSB first).
bool WriteBitmask64Hex(JsonDocument& doc, const char* key, const uint64_t mask) {
    if (key == nullptr) {
        return false;
    }

    char hex[17];
    for (uint8_t i = 0; i < 16; ++i) {
        const uint8_t shift = static_cast<uint8_t>((15U - i) * 4U);
        const uint8_t nibble = static_cast<uint8_t>((mask >> shift) & 0x0FU);
        hex[i] = static_cast<char>((nibble < 10U) ? ('0' + nibble) : ('a' + (nibble - 10U)));
    }
    hex[16] = '\0';

    // Force copy: hex is a local buffer. ArduinoJson copies non-const char*.
    if (!doc[key].set(static_cast<char*>(hex))) {
        return false;
    }
    return !doc.overflowed();
}

// Read a 64-bit bitmask from either hex string (current) or int array (legacy).
bool ReadBitmask64HexOrLegacyArray(const JsonDocument& doc,
                                          const char* key,
                                          uint64_t& outMask) {
    outMask = 0;
    if (key == nullptr) {
        return false;
    }

    if (!doc.containsKey(key)) {
        return true;
    }

    const JsonVariantConst v = doc[key];

    // Current format: 16-character hex string encoding of 64-bit bitmask.
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (s == nullptr) {
            return false;
        }
        const size_t len = std::strlen(s);
        if (len != 16) {
            return false;
        }
        uint64_t mask = 0;
        for (size_t i = 0; i < 16; ++i) {
            uint8_t nibbleVal = 0;
            const char c = s[i];
            if (c >= '0' && c <= '9') {
                nibbleVal = static_cast<uint8_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                nibbleVal = static_cast<uint8_t>(10U + static_cast<uint8_t>(c - 'a'));
            } else if (c >= 'A' && c <= 'F') {
                nibbleVal = static_cast<uint8_t>(10U + static_cast<uint8_t>(c - 'A'));
            } else {
                return false;
            }
            mask |= static_cast<uint64_t>(nibbleVal) << ((15U - i) * 4U);
        }
        outMask = mask;
        return true;
    }

    // Legacy format: JSON int array of square indices (0..63).
    if (v.is<JsonArrayConst>()) {
        const JsonArrayConst arr = v.as<JsonArrayConst>();
        uint64_t mask = 0;
        for (const JsonVariantConst e : arr) {
            const uint32_t idx = e | 0U;
            if (idx >= runtime::BOARD_SQUARE_COUNT) {
                return false;
            }
            mask |= (1ULL << idx);
        }
        outMask = mask;
        return true;
    }

    return false;
}

bool WriteSquareIndexArray(JsonDocument& doc,
                                  const char* key,
                                  const uint8_t* squares,
                                  const uint8_t count) {
    if (key == nullptr) {
        return false;
    }

    JsonArray arr = doc[key].to<JsonArray>();
    if (arr.isNull()) {
        return false;
    }
    arr.clear();

    for (uint8_t i = 0; i < count; ++i) {
        if (squares[i] >= runtime::BOARD_SQUARE_COUNT) {
            return false;
        }
        if (!arr.add(static_cast<uint32_t>(squares[i]))) {
            return false;
        }
    }

    if (doc.overflowed()) {
        return false;
    }
    return true;
}

bool ReadSquareIndexArray(const JsonDocument& doc,
                                 const char* key,
                                 uint8_t* outSquares,
                                 const uint8_t outCapacity,
                                 uint8_t& outCount) {
    outCount = 0;

    if (key == nullptr || outSquares == nullptr || outCapacity == 0) {
        return false;
    }

    if (!doc.containsKey(key)) {
        return true;
    }

    const JsonVariantConst v = doc[key];
    if (!v.is<JsonArrayConst>()) {
        return false;
    }

    const JsonArrayConst arr = v.as<JsonArrayConst>();
    for (const JsonVariantConst e : arr) {
        if (outCount >= outCapacity) {
            return false;
        }
        const uint32_t idx = e | 0U;
        if (idx >= runtime::BOARD_SQUARE_COUNT) {
            return false;
        }
        outSquares[outCount] = static_cast<uint8_t>(idx);
        ++outCount;
    }

    return true;
}

bool WriteUndoHistory(JsonDocument& doc,
                             const uint8_t* fields,
                             const uint16_t fieldCount) {
    if (fieldCount == 0) {
        (void)doc.remove(KEY_GAME_UNDO_HISTORY);
        return true;
    }

    if (fields == nullptr) {
        return false;
    }

    if ((fieldCount % UNDO_HISTORY_FIELDS_PER_MOVE) != 0) {
        return false;
    }

    // Compact encoding: hex string (2 chars per byte). This keeps the persisted
    // JSON small enough to fit within the SettingsStore JSON capacity.
    static constexpr uint16_t MAX_FIELDS =
        static_cast<uint16_t>(UNDO_HISTORY_FIELDS_PER_MOVE * UNDO_HISTORY_MOVE_CAPACITY);
    if (fieldCount > MAX_FIELDS) {
        return false;
    }

    static constexpr size_t HEX_STRING_CAPACITY =
        (UNDO_HISTORY_FIELDS_PER_MOVE * UNDO_HISTORY_MOVE_CAPACITY * 2U) + 1U;

    fxe::FixedString<HEX_STRING_CAPACITY> hex;

    const auto nibble = [](const uint8_t v) -> char {
        const uint8_t x = static_cast<uint8_t>(v & 0x0FU);
        return static_cast<char>((x < 10U) ? ('0' + x) : ('a' + (x - 10U)));
    };

    for (uint16_t i = 0; i < fieldCount; ++i) {
        const uint8_t b = fields[i];
        if (!hex.PushBack(nibble(static_cast<uint8_t>(b >> 4U)))) {
            return false;
        }
        if (!hex.PushBack(nibble(static_cast<uint8_t>(b & 0x0FU)))) {
            return false;
        }
    }

    // ArduinoJson may treat `const char*` as a "static" string and link instead of
    // copy. `hex` is a local buffer, so force a copy via `char*`.
    if (!doc[KEY_GAME_UNDO_HISTORY].set(const_cast<char*>(hex.CStr()))) {
        return false;
    }

    if (doc.overflowed()) {
        return false;
    }
    return true;
}

bool ReadUndoHistory(const JsonDocument& doc,
                            uint8_t* outFields,
                            const uint16_t outCapacity,
                            uint16_t& outFieldCount) {
    outFieldCount = 0;

    if (outFields == nullptr || outCapacity == 0) {
        return false;
    }

    if (!doc.containsKey(KEY_GAME_UNDO_HISTORY)) {
        return true;
    }

    const JsonVariantConst v = doc[KEY_GAME_UNDO_HISTORY];

    const auto hexVal = [](const char c, uint8_t& out) -> bool {
        if (c >= '0' && c <= '9') {
            out = static_cast<uint8_t>(c - '0');
            return true;
        }
        if (c >= 'a' && c <= 'f') {
            out = static_cast<uint8_t>(10U + static_cast<uint8_t>(c - 'a'));
            return true;
        }
        if (c >= 'A' && c <= 'F') {
            out = static_cast<uint8_t>(10U + static_cast<uint8_t>(c - 'A'));
            return true;
        }
        return false;
    };

    // Legacy format: JSON int array.
    // Current format: JSON array of hex-string chunks.
    if (v.is<JsonArrayConst>()) {
        const JsonArrayConst arr = v.as<JsonArrayConst>();
        if (arr.size() == 0) {
            outFieldCount = 0;
            return true;
        }

        const JsonVariantConst first = arr[0];
        if (first.is<JsonString>()) {
            // Chunked hex strings.
            size_t totalLen = 0;
            for (const JsonVariantConst e : arr) {
                const char* s = e.as<const char*>();
                if (s == nullptr) {
                    return false;
                }
                const size_t len = std::strlen(s);
                if ((len % 2U) != 0U) {
                    return false;
                }
                totalLen += len;
            }

            if ((totalLen % 2U) != 0U) {
                return false;
            }

            const size_t bytes = totalLen / 2U;
            if (bytes > outCapacity) {
                return false;
            }
            if ((bytes % UNDO_HISTORY_FIELDS_PER_MOVE) != 0U) {
                return false;
            }

            for (const JsonVariantConst e : arr) {
                const char* s = e.as<const char*>();
                const size_t len = std::strlen(s);
                for (size_t i = 0; i < len; i += 2U) {
                    uint8_t hi = 0;
                    uint8_t lo = 0;
                    if (!hexVal(s[i + 0U], hi) || !hexVal(s[i + 1U], lo)) {
                        return false;
                    }
                    if (outFieldCount >= outCapacity) {
                        return false;
                    }
                    outFields[outFieldCount] = static_cast<uint8_t>((hi << 4U) | lo);
                    ++outFieldCount;
                }
            }

            return true;
        }

        // Legacy numeric array.
        const size_t n = arr.size();
        if (n > outCapacity) {
            return false;
        }
        if ((n % UNDO_HISTORY_FIELDS_PER_MOVE) != 0) {
            return false;
        }

        for (const JsonVariantConst e : arr) {
            if (outFieldCount >= outCapacity) {
                return false;
            }
            const uint32_t vv = e | 0U;
            if (vv > 255U) {
                return false;
            }
            outFields[outFieldCount] = static_cast<uint8_t>(vv);
            ++outFieldCount;
        }

        return true;
    }

    // Legacy format: hex string (2 chars per byte).
    {
        const char* s = v.as<const char*>();
        if (s == nullptr) {
            return false;
        }

        if (s[0] == '\0') {
            outFieldCount = 0;
            return true;
        }

        const size_t len = std::strlen(s);

        if ((len % 2U) != 0U) {
            return false;
        }

        const size_t bytes = len / 2U;
        if (bytes > outCapacity) {
            return false;
        }
        if ((bytes % UNDO_HISTORY_FIELDS_PER_MOVE) != 0U) {
            return false;
        }

        for (size_t i = 0; i < bytes; ++i) {
            uint8_t hi = 0;
            uint8_t lo = 0;
            if (!hexVal(s[i * 2U + 0U], hi) || !hexVal(s[i * 2U + 1U], lo)) {
                return false;
            }
            outFields[outFieldCount] = static_cast<uint8_t>((hi << 4U) | lo);
            ++outFieldCount;
        }

        return true;
    }

    return false;
}

} // namespace modules
