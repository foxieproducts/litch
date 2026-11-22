#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace trace::jsonl {

struct ObjectView {
    const char* begin = nullptr; // points at '{'
    const char* end = nullptr;   // points one past matching '}'

    bool IsValid() const { return begin != nullptr && end != nullptr && begin < end; }
};

namespace detail {

inline bool IsWhitespace(const char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline const char* SkipWhitespace(const char* p, const char* end) {
    while (p && p < end && IsWhitespace(*p)) {
        ++p;
    }
    return p;
}

inline bool ReadRawStringView(const char*& p, const char* end, std::string_view& out) {
    out = {};
    p = SkipWhitespace(p, end);
    if (!p || p >= end || *p != '"') {
        return false;
    }

    const char* start = ++p;
    bool escaped = false;
    while (p < end) {
        const char c = *p;
        if (escaped) {
            escaped = false;
            ++p;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            ++p;
            continue;
        }
        if (c == '"') {
            out = std::string_view(start, static_cast<size_t>(p - start));
            ++p; // consume closing quote
            return true;
        }
        ++p;
    }

    return false;
}

inline bool SkipValue(const char*& p, const char* end);

inline bool SkipObject(const char*& p, const char* end) {
    p = SkipWhitespace(p, end);
    if (!p || p >= end || *p != '{') {
        return false;
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;

    while (p < end) {
        const char c = *p;

        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            ++p;
            continue;
        }

        if (c == '"') {
            inString = true;
            ++p;
            continue;
        }

        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            ++p;
            if (depth == 0) {
                return true;
            }
            continue;
        }

        ++p;
    }

    return false;
}

inline bool SkipArray(const char*& p, const char* end) {
    p = SkipWhitespace(p, end);
    if (!p || p >= end || *p != '[') {
        return false;
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;

    while (p < end) {
        const char c = *p;

        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            ++p;
            continue;
        }

        if (c == '"') {
            inString = true;
            ++p;
            continue;
        }

        if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
            ++p;
            if (depth == 0) {
                return true;
            }
            continue;
        }

        ++p;
    }

    return false;
}

inline bool SkipPrimitive(const char*& p, const char* end) {
    p = SkipWhitespace(p, end);
    if (!p || p >= end) {
        return false;
    }

    if (*p == '"') {
        std::string_view ignored;
        return ReadRawStringView(p, end, ignored);
    }

    // number / true / false / null
    const char* start = p;
    while (p < end) {
        const char c = *p;
        if (c == ',' || c == '}' || c == ']' || IsWhitespace(c)) {
            break;
        }
        ++p;
    }

    return p > start;
}

inline bool SkipValue(const char*& p, const char* end) {
    p = SkipWhitespace(p, end);
    if (!p || p >= end) {
        return false;
    }

    if (*p == '{') {
        return SkipObject(p, end);
    }
    if (*p == '[') {
        return SkipArray(p, end);
    }
    return SkipPrimitive(p, end);
}

inline bool TryGetValueRange(const ObjectView& obj,
                             const std::string_view key,
                             const char*& outValueBegin,
                             const char*& outValueEnd) {
    outValueBegin = nullptr;
    outValueEnd = nullptr;

    if (!obj.IsValid()) {
        return false;
    }

    const char* p = SkipWhitespace(obj.begin, obj.end);
    if (!p || p >= obj.end || *p != '{') {
        return false;
    }
    ++p; // consume '{'

    while (true) {
        p = SkipWhitespace(p, obj.end);
        if (!p || p >= obj.end) {
            return false;
        }
        if (*p == '}') {
            return false; // not found
        }

        std::string_view keyView;
        if (!ReadRawStringView(p, obj.end, keyView)) {
            return false;
        }

        p = SkipWhitespace(p, obj.end);
        if (!p || p >= obj.end || *p != ':') {
            return false;
        }
        ++p;

        const char* valueStart = SkipWhitespace(p, obj.end);
        if (!valueStart || valueStart >= obj.end) {
            return false;
        }

        const char* valueEnd = valueStart;
        if (!SkipValue(valueEnd, obj.end)) {
            return false;
        }

        if (keyView == key) {
            outValueBegin = valueStart;
            outValueEnd = valueEnd;
            return true;
        }

        p = SkipWhitespace(valueEnd, obj.end);
        if (!p || p >= obj.end) {
            return false;
        }
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}') {
            return false;
        }

        return false;
    }
}

inline ObjectView RootObject(const char* line) {
    if (!line) {
        return {};
    }

    const char* begin = line;
    const char* end = begin;
    while (*end != '\0') {
        ++end;
    }

    // Trim trailing whitespace (including CR from CRLF).
    while (end > begin && IsWhitespace(*(end - 1))) {
        --end;
    }

    const char* p = SkipWhitespace(begin, end);
    if (!p || p >= end || *p != '{') {
        return {};
    }

    const char* objBegin = p;
    if (!SkipObject(p, end)) {
        return {};
    }

    return ObjectView{objBegin, p};
}

inline bool TryParseU64FromView(const std::string_view s, uint64_t& out) {
    out = 0;
    if (s.empty()) {
        return false;
    }

    // Copy into a small temp buffer (strtoull expects NUL termination).
    char buf[32] = {};
    const size_t n = (s.size() < (sizeof(buf) - 1)) ? s.size() : (sizeof(buf) - 1);
    for (size_t i = 0; i < n; ++i) {
        buf[i] = s[i];
    }
    buf[n] = '\0';

    char* endPtr = nullptr;
    const unsigned long long v = std::strtoull(buf, &endPtr, 0);
    if (endPtr == buf) {
        return false;
    }
    out = static_cast<uint64_t>(v);
    return true;
}

inline bool TryParseU32FromView(const std::string_view s, uint32_t& out) {
    uint64_t tmp = 0;
    if (!TryParseU64FromView(s, tmp)) {
        return false;
    }
    out = static_cast<uint32_t>(tmp);
    return true;
}

} // namespace detail

inline bool TryGetStringView(const char* line, const std::string_view key, std::string_view& out) {
    out = {};
    const ObjectView root = detail::RootObject(line);
    const char* valueBegin = nullptr;
    const char* valueEnd = nullptr;
    if (!detail::TryGetValueRange(root, key, valueBegin, valueEnd)) {
        return false;
    }

    const char* p = valueBegin;
    std::string_view raw;
    if (!detail::ReadRawStringView(p, valueEnd, raw)) {
        return false;
    }
    out = raw;
    return true;
}

inline bool TryGetObject(const char* line, const std::string_view key, ObjectView& outObj) {
    outObj = {};
    const ObjectView root = detail::RootObject(line);
    const char* valueBegin = nullptr;
    const char* valueEnd = nullptr;
    if (!detail::TryGetValueRange(root, key, valueBegin, valueEnd)) {
        return false;
    }

    const char* p = detail::SkipWhitespace(valueBegin, valueEnd);
    if (!p || p >= valueEnd || *p != '{') {
        return false;
    }

    const char* objBegin = p;
    if (!detail::SkipObject(p, valueEnd)) {
        return false;
    }

    outObj = ObjectView{objBegin, p};
    return true;
}

inline bool TryGetStringView(const ObjectView& obj, const std::string_view key, std::string_view& out) {
    out = {};
    const char* valueBegin = nullptr;
    const char* valueEnd = nullptr;
    if (!detail::TryGetValueRange(obj, key, valueBegin, valueEnd)) {
        return false;
    }

    const char* p = valueBegin;
    std::string_view raw;
    if (!detail::ReadRawStringView(p, valueEnd, raw)) {
        return false;
    }
    out = raw;
    return true;
}

inline bool TryGetU32(const char* line, const std::string_view key, uint32_t& out) {
    out = 0;
    const ObjectView root = detail::RootObject(line);
    const char* valueBegin = nullptr;
    const char* valueEnd = nullptr;
    if (!detail::TryGetValueRange(root, key, valueBegin, valueEnd)) {
        return false;
    }

    const char* p = detail::SkipWhitespace(valueBegin, valueEnd);
    if (!p || p >= valueEnd) {
        return false;
    }

    // Accept quoted numbers too.
    if (*p == '"') {
        std::string_view raw;
        if (!detail::ReadRawStringView(p, valueEnd, raw)) {
            return false;
        }
        return detail::TryParseU32FromView(raw, out);
    }

    std::string_view raw(p, static_cast<size_t>(valueEnd - p));
    // Trim trailing whitespace/comma guards.
    while (!raw.empty() && detail::IsWhitespace(raw.back())) {
        raw = raw.substr(0, raw.size() - 1);
    }

    return detail::TryParseU32FromView(raw, out);
}

inline bool TryGetU32(const ObjectView& obj, const std::string_view key, uint32_t& out) {
    out = 0;
    const char* valueBegin = nullptr;
    const char* valueEnd = nullptr;
    if (!detail::TryGetValueRange(obj, key, valueBegin, valueEnd)) {
        return false;
    }

    const char* p = detail::SkipWhitespace(valueBegin, valueEnd);
    if (!p || p >= valueEnd) {
        return false;
    }

    if (*p == '"') {
        std::string_view raw;
        if (!detail::ReadRawStringView(p, valueEnd, raw)) {
            return false;
        }
        return detail::TryParseU32FromView(raw, out);
    }

    std::string_view raw(p, static_cast<size_t>(valueEnd - p));
    while (!raw.empty() && detail::IsWhitespace(raw.back())) {
        raw = raw.substr(0, raw.size() - 1);
    }

    return detail::TryParseU32FromView(raw, out);
}

inline bool TryGetU64HexOrDec(const char* line, const std::string_view key, uint64_t& out) {
    out = 0;
    const ObjectView root = detail::RootObject(line);
    const char* valueBegin = nullptr;
    const char* valueEnd = nullptr;
    if (!detail::TryGetValueRange(root, key, valueBegin, valueEnd)) {
        return false;
    }

    const char* p = detail::SkipWhitespace(valueBegin, valueEnd);
    if (!p || p >= valueEnd) {
        return false;
    }

    if (*p == '"') {
        std::string_view raw;
        if (!detail::ReadRawStringView(p, valueEnd, raw)) {
            return false;
        }
        return detail::TryParseU64FromView(raw, out);
    }

    std::string_view raw(p, static_cast<size_t>(valueEnd - p));
    while (!raw.empty() && detail::IsWhitespace(raw.back())) {
        raw = raw.substr(0, raw.size() - 1);
    }

    return detail::TryParseU64FromView(raw, out);
}

inline bool TryParseBoolFromValueRange(const char* valueBegin, const char* valueEnd, bool& out) {
    out = false;

    const char* value = detail::SkipWhitespace(valueBegin, valueEnd);
    if (!value || value >= valueEnd) {
        return false;
    }

    if (*value == '"') {
        std::string_view raw;
        if (!detail::ReadRawStringView(value, valueEnd, raw)) {
            return false;
        }

        uint32_t parsedNumber = 0;
        if (detail::TryParseU32FromView(raw, parsedNumber)) {
            out = parsedNumber != 0U;
            return true;
        }

        if (raw == "true") {
            out = true;
            return true;
        }
        if (raw == "false") {
            out = false;
            return true;
        }
        return false;
    }

    if ((valueEnd - value) >= 4 && value[0] == 't' && value[1] == 'r' && value[2] == 'u' &&
        value[3] == 'e') {
        out = true;
        return true;
    }
    if ((valueEnd - value) >= 5 && value[0] == 'f' && value[1] == 'a' && value[2] == 'l' &&
        value[3] == 's' && value[4] == 'e') {
        out = false;
        return true;
    }

    uint32_t parsedNumber = 0;
    std::string_view raw(value, static_cast<size_t>(valueEnd - value));
    while (!raw.empty() && detail::IsWhitespace(raw.back())) {
        raw = raw.substr(0, raw.size() - 1);
    }

    if (!detail::TryParseU32FromView(raw, parsedNumber)) {
        return false;
    }

    out = parsedNumber != 0U;
    return true;
}

inline bool TryGetBool(const char* line, const std::string_view key, bool& out) {
    out = false;

    const ObjectView root = detail::RootObject(line);
    const char* valueBegin = nullptr;
    const char* valueEnd = nullptr;
    if (!detail::TryGetValueRange(root, key, valueBegin, valueEnd)) {
        return false;
    }

    return TryParseBoolFromValueRange(valueBegin, valueEnd, out);
}

inline bool TryGetBool(const ObjectView& obj, const std::string_view key, bool& out) {
    out = false;

    const char* valueBegin = nullptr;
    const char* valueEnd = nullptr;
    if (!detail::TryGetValueRange(obj, key, valueBegin, valueEnd)) {
        return false;
    }

    return TryParseBoolFromValueRange(valueBegin, valueEnd, out);
}

inline size_t UnescapeJsonString(const std::string_view raw, char* out, const size_t outCap) {
    if (!out || outCap == 0) {
        return 0;
    }

    size_t w = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (w + 1 >= outCap) {
            break;
        }

        const char c = raw[i];
        if (c != '\\') {
            out[w++] = c;
            continue;
        }

        if (i + 1 >= raw.size()) {
            break;
        }

        const char e = raw[++i];
        if (e == '"') {
            out[w++] = '"';
        } else if (e == '\\') {
            out[w++] = '\\';
        } else if (e == '/') {
            out[w++] = '/';
        } else if (e == 'b') {
            out[w++] = '\b';
        } else if (e == 'f') {
            out[w++] = '\f';
        } else if (e == 'n') {
            out[w++] = '\n';
        } else if (e == 'r') {
            out[w++] = '\r';
        } else if (e == 't') {
            out[w++] = '\t';
        } else if (e == 'u') {
            // Decode \uXXXX only for ASCII range; otherwise replace with '?'.
            if (i + 4 < raw.size()) {
                auto hex = [&](const char h) -> uint8_t {
                    if (h >= '0' && h <= '9') {
                        return static_cast<uint8_t>(h - '0');
                    }
                    if (h >= 'a' && h <= 'f') {
                        return static_cast<uint8_t>(10 + (h - 'a'));
                    }
                    if (h >= 'A' && h <= 'F') {
                        return static_cast<uint8_t>(10 + (h - 'A'));
                    }
                    return 0xFF;
                };

                const uint8_t h0 = hex(raw[i + 1]);
                const uint8_t h1 = hex(raw[i + 2]);
                const uint8_t h2 = hex(raw[i + 3]);
                const uint8_t h3 = hex(raw[i + 4]);
                const bool ok = (h0 != 0xFF && h1 != 0xFF && h2 != 0xFF && h3 != 0xFF);
                const uint16_t code = static_cast<uint16_t>((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                out[w++] = (ok && code <= 0x7F) ? static_cast<char>(code) : '?';
                i += 4;
            } else {
                out[w++] = '?';
            }
        } else {
            out[w++] = e;
        }
    }

    out[w] = '\0';
    return w;
}

} // namespace trace::jsonl
