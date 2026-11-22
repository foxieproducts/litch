#pragma once

#include <cstdint>
#include <string_view>

namespace modules::string_parsing {

inline char ToLowerAscii(const char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

inline bool EqualsNoCaseAscii(const std::string_view a, const std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); ++i) {
        if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) {
            return false;
        }
    }

    return true;
}

inline bool TryParseU16(const std::string_view s, uint16_t& out) {
    if (s.empty()) {
        return false;
    }

    uint32_t value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10U + static_cast<uint32_t>(c - '0');
        if (value > 65535U) {
            return false;
        }
    }

    out = static_cast<uint16_t>(value);
    return true;
}

inline bool TryParseU32(const std::string_view s, uint32_t& out) {
    if (s.empty()) {
        return false;
    }

    uint64_t value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }

        value = value * 10U + static_cast<uint64_t>(c - '0');
        if (value > 0xFFFFFFFFULL) {
            return false;
        }
    }

    out = static_cast<uint32_t>(value);
    return true;
}

inline bool TryParseHexByte(const char hi, const char lo, uint8_t& out) {
    const auto nibble = [](const char c) -> int {
        if (c >= '0' && c <= '9') {
            return static_cast<int>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<int>(10 + (c - 'a'));
        }
        if (c >= 'A' && c <= 'F') {
            return static_cast<int>(10 + (c - 'A'));
        }
        return -1;
    };

    const int upper = nibble(hi);
    const int lower = nibble(lo);
    if (upper < 0 || lower < 0) {
        return false;
    }

    out = static_cast<uint8_t>((upper << 4) | lower);
    return true;
}

inline bool TryParseRgbHex(const std::string_view s, uint8_t& outR, uint8_t& outG, uint8_t& outB) {
    if (s.size() != 6U) {
        return false;
    }

    return TryParseHexByte(s[0], s[1], outR) &&
           TryParseHexByte(s[2], s[3], outG) &&
           TryParseHexByte(s[4], s[5], outB);
}

} // namespace modules::string_parsing