#pragma once

#include <cstddef>
#include <cstdint>

namespace modules {

// Fixed 3-column by 5-row monospace font for deliberately compact overlays.
// Bit 0 is the left-most pixel, matching the historical compact renderer. The
// table is independent of the readable face so typography changes cannot
// silently alter compact output.
struct Glyph3x5 {
    uint8_t rows[5];
};

namespace font_3x5 {

static constexpr uint8_t GLYPH_WIDTH = 3;
static constexpr uint8_t GLYPH_HEIGHT = 5;
static constexpr uint8_t FIRST_ASCII = 32;
static constexpr uint8_t LAST_ASCII = 126;
static constexpr uint8_t GLYPH_COUNT = (LAST_ASCII - FIRST_ASCII + 1);

constexpr uint16_t Pack(const uint8_t r0,
                        const uint8_t r1,
                        const uint8_t r2,
                        const uint8_t r3,
                        const uint8_t r4) {
    return static_cast<uint16_t>((r0 & 0x7U) | ((r1 & 0x7U) << 3) |
                                 ((r2 & 0x7U) << 6) | ((r3 & 0x7U) << 9) |
                                 ((r4 & 0x7U) << 12));
}

constexpr Glyph3x5 Unpack(const uint16_t packed) {
    return Glyph3x5{{
        static_cast<uint8_t>((packed >> 0) & 0x7U),
        static_cast<uint8_t>((packed >> 3) & 0x7U),
        static_cast<uint8_t>((packed >> 6) & 0x7U),
        static_cast<uint8_t>((packed >> 9) & 0x7U),
        static_cast<uint8_t>((packed >> 12) & 0x7U),
    }};
}

// ASCII 32..126. This is the exact compact face used before readable 4x6
// became the default.
static constexpr uint16_t GLYPHS_PACKED[GLYPH_COUNT] = {
    Pack(0, 0, 0, 0, 0), // space
    Pack(2, 2, 2, 0, 2), // !
    Pack(5, 5, 0, 0, 0), // "
    Pack(5, 7, 5, 7, 5), // #
    Pack(2, 7, 6, 3, 7), // $
    Pack(5, 1, 2, 4, 5), // %
    Pack(2, 5, 2, 5, 3), // &
    Pack(2, 2, 0, 0, 0), // '
    Pack(1, 2, 2, 2, 1), // (
    Pack(4, 2, 2, 2, 4), // )
    Pack(0, 5, 2, 5, 0), // *
    Pack(0, 2, 7, 2, 0), // +
    Pack(0, 0, 0, 2, 4), // ,
    Pack(0, 0, 7, 0, 0), // -
    Pack(0, 0, 0, 0, 2), // .
    Pack(1, 1, 2, 4, 4), // /
    Pack(7, 5, 5, 5, 7), // 0
    Pack(2, 6, 2, 2, 7), // 1
    Pack(7, 1, 7, 4, 7), // 2
    Pack(7, 1, 7, 1, 7), // 3
    Pack(5, 5, 7, 1, 1), // 4
    Pack(7, 4, 7, 1, 7), // 5
    Pack(7, 4, 7, 5, 7), // 6
    Pack(7, 1, 1, 1, 1), // 7
    Pack(7, 5, 7, 5, 7), // 8
    Pack(7, 5, 7, 1, 7), // 9
    Pack(0, 2, 0, 2, 0), // :
    Pack(0, 2, 0, 2, 4), // ;
    Pack(1, 2, 4, 2, 1), // <
    Pack(0, 7, 0, 7, 0), // =
    Pack(4, 2, 1, 2, 4), // >
    Pack(7, 1, 3, 0, 2), // ?
    Pack(7, 5, 7, 4, 7), // @
    Pack(2, 5, 7, 5, 5), // A
    Pack(6, 5, 6, 5, 6), // B
    Pack(3, 4, 4, 4, 3), // C
    Pack(6, 5, 5, 5, 6), // D
    Pack(7, 4, 6, 4, 7), // E
    Pack(7, 4, 6, 4, 4), // F
    Pack(3, 4, 5, 5, 3), // G
    Pack(5, 5, 7, 5, 5), // H
    Pack(7, 2, 2, 2, 7), // I
    Pack(1, 1, 1, 5, 2), // J
    Pack(5, 6, 4, 6, 5), // K
    Pack(4, 4, 4, 4, 7), // L
    Pack(5, 7, 7, 5, 5), // M
    Pack(5, 7, 7, 7, 5), // N
    Pack(2, 5, 5, 5, 2), // O
    Pack(6, 5, 6, 4, 4), // P
    Pack(2, 5, 5, 7, 3), // Q
    Pack(6, 5, 6, 6, 5), // R
    Pack(3, 4, 2, 1, 6), // S
    Pack(7, 2, 2, 2, 2), // T
    Pack(5, 5, 5, 5, 7), // U
    Pack(5, 5, 5, 5, 2), // V
    Pack(5, 5, 7, 7, 5), // W
    Pack(5, 5, 2, 5, 5), // X
    Pack(5, 5, 2, 2, 2), // Y
    Pack(7, 1, 2, 4, 7), // Z
    Pack(3, 2, 2, 2, 3), // [
    Pack(4, 4, 2, 1, 1), // backslash
    Pack(6, 2, 2, 2, 6), // ]
    Pack(2, 5, 0, 0, 0), // ^
    Pack(0, 0, 0, 0, 7), // _
    Pack(4, 2, 0, 0, 0), // `
    Pack(0, 6, 1, 7, 7), // a
    Pack(4, 6, 5, 5, 6), // b
    Pack(0, 3, 4, 4, 3), // c
    Pack(1, 3, 5, 5, 3), // d
    Pack(0, 2, 7, 4, 3), // e
    Pack(1, 2, 7, 2, 2), // f
    Pack(0, 3, 5, 3, 7), // g
    Pack(4, 6, 5, 5, 5), // h
    Pack(2, 0, 6, 2, 7), // i
    Pack(1, 0, 1, 5, 2), // j
    Pack(4, 5, 6, 6, 5), // k
    Pack(6, 2, 2, 2, 7), // l
    Pack(0, 7, 7, 5, 5), // m
    Pack(0, 6, 5, 5, 5), // n
    Pack(0, 2, 5, 5, 2), // o
    Pack(0, 6, 5, 6, 4), // p
    Pack(0, 3, 5, 3, 1), // q
    Pack(0, 3, 4, 4, 4), // r
    Pack(0, 3, 2, 1, 6), // s
    Pack(2, 7, 2, 2, 1), // t
    Pack(0, 5, 5, 5, 3), // u
    Pack(0, 5, 5, 5, 2), // v
    Pack(0, 5, 5, 7, 5), // w
    Pack(0, 5, 2, 2, 5), // x
    Pack(0, 5, 5, 3, 7), // y
    Pack(0, 7, 1, 2, 7), // z
    Pack(1, 2, 6, 2, 1), // {
    Pack(2, 2, 2, 2, 2), // |
    Pack(4, 2, 3, 2, 4), // }
    Pack(0, 3, 6, 0, 0), // ~
};

constexpr Glyph3x5 GetGlyphOrFallback(const char c, const char fallback) {
    const int index = static_cast<int>(static_cast<unsigned char>(c)) -
                      static_cast<int>(FIRST_ASCII);
    if (index >= 0 && index < static_cast<int>(GLYPH_COUNT)) {
        return Unpack(GLYPHS_PACKED[static_cast<size_t>(index)]);
    }

    const int fallbackIndex =
        static_cast<int>(static_cast<unsigned char>(fallback)) -
        static_cast<int>(FIRST_ASCII);
    if (fallbackIndex >= 0 && fallbackIndex < static_cast<int>(GLYPH_COUNT)) {
        return Unpack(GLYPHS_PACKED[static_cast<size_t>(fallbackIndex)]);
    }

    return Glyph3x5{};
}

constexpr Glyph3x5 GetGlyph(const char c) {
    return GetGlyphOrFallback(c, '?');
}

} // namespace font_3x5

} // namespace modules
