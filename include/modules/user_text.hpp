#pragma once

#include <cstddef>
#include <cstdint>

namespace modules::user_text {

// Validate a single-line display name without allocating. Limits are counted
// both as Unicode scalar values and encoded UTF-8 bytes so a validated value
// can be copied into a fixed transport buffer without truncation.
inline bool IsSafeDisplayName(const char* const text, const size_t byteCount,
                              const size_t maximumCharacters,
                              const size_t maximumBytes) {
    if (text == nullptr || byteCount == 0U || byteCount > maximumBytes) {
        return false;
    }

    size_t offset = 0U;
    size_t characters = 0U;
    while (offset < byteCount) {
        const uint8_t first = static_cast<uint8_t>(text[offset]);
        uint32_t scalar = 0U;
        size_t width = 0U;
        if (first <= 0x7FU) {
            scalar = first;
            width = 1U;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            scalar = first & 0x1FU;
            width = 2U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            scalar = first & 0x0FU;
            width = 3U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            scalar = first & 0x07U;
            width = 4U;
        } else {
            return false;
        }
        if (width > byteCount - offset) return false;

        for (size_t index = 1U; index < width; ++index) {
            const uint8_t continuation =
                static_cast<uint8_t>(text[offset + index]);
            if ((continuation & 0xC0U) != 0x80U) return false;
            scalar = (scalar << 6U) | (continuation & 0x3FU);
        }

        // Reject overlong sequences, UTF-16 surrogate values and scalars past
        // Unicode's upper bound.
        if ((width == 2U && scalar < 0x80U) ||
            (width == 3U && scalar < 0x800U) ||
            (width == 4U && scalar < 0x10000U) ||
            (scalar >= 0xD800U && scalar <= 0xDFFFU) || scalar > 0x10FFFFU) {
            return false;
        }

        // Exclude C0/C1 controls, line separators and bidi formatting controls
        // that can hide or reorder a board name in logs and user interfaces.
        if (scalar < 0x20U || (scalar >= 0x7FU && scalar <= 0x9FU) ||
            (scalar >= 0x2028U && scalar <= 0x202EU) ||
            (scalar >= 0x2066U && scalar <= 0x2069U)) {
            return false;
        }

        offset += width;
        if (++characters > maximumCharacters) return false;
    }
    return true;
}

} // namespace modules::user_text
