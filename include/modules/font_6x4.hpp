#pragma once

#include <cstddef>
#include <cstdint>

namespace modules {

// Fixed 4-column by 6-row monospace font for readable board overlays.
//
// Naming: "6x4" refers to height x width.
//
// Encoding:
// - Each glyph is 4 columns by 6 rows.
// - Each row is stored in the low 4 bits of a byte.
// - Bit 3 is the left-most pixel and bit 0 is the right-most pixel.
// - Glyphs are indexed by ASCII, with support for 32..126.
// - Unsupported characters map to '?' (or blank if '?' is unavailable).
struct Glyph6x4 {
    uint8_t rows[6];
};

namespace font_6x4 {

static constexpr uint8_t GLYPH_WIDTH = 4;
static constexpr uint8_t GLYPH_HEIGHT = 6;
static constexpr uint8_t FIRST_ASCII = 32;
static constexpr uint8_t LAST_ASCII = 126;
static constexpr uint8_t GLYPH_COUNT = (LAST_ASCII - FIRST_ASCII + 1);

constexpr uint32_t Pack(const uint8_t r0,
                        const uint8_t r1,
                        const uint8_t r2,
                        const uint8_t r3,
                        const uint8_t r4,
                        const uint8_t r5) {
    return static_cast<uint32_t>((r0 & 0xFU) | ((r1 & 0xFU) << 4) |
                                 ((r2 & 0xFU) << 8) | ((r3 & 0xFU) << 12) |
                                 ((r4 & 0xFU) << 16) | ((r5 & 0xFU) << 20));
}

constexpr Glyph6x4 Unpack(const uint32_t packed) {
    Glyph6x4 glyph{};
    glyph.rows[0] = static_cast<uint8_t>((packed >> 0) & 0xFU);
    glyph.rows[1] = static_cast<uint8_t>((packed >> 4) & 0xFU);
    glyph.rows[2] = static_cast<uint8_t>((packed >> 8) & 0xFU);
    glyph.rows[3] = static_cast<uint8_t>((packed >> 12) & 0xFU);
    glyph.rows[4] = static_cast<uint8_t>((packed >> 16) & 0xFU);
    glyph.rows[5] = static_cast<uint8_t>((packed >> 20) & 0xFU);
    return glyph;
}

// Table: ASCII 32..126. This is the original 4x6 face used before the glyph
// table was accidentally collapsed to three live columns and five live rows.
static constexpr uint32_t GLYPHS_PACKED[GLYPH_COUNT] = {
    0x000000U,
    0x606666U,
    0x000099U,
    0x9F99F9U,
    0xF7EEF6U,
    0x986619U,
    0x796696U,
    0x000066U,
    0x166661U,
    0x866668U,
    0x096690U,
    0x06FF60U,
    0x860000U,
    0x00FF00U,
    0x600000U,
    0x886611U,
    0xF9999FU,
    0xF666E6U,
    0xF8FF1FU,
    0xF1FF1FU,
    0x11FF99U,
    0xF1FF8FU,
    0xF9FF8FU,
    0x11111FU,
    0xF9FF9FU,
    0xF1FF9FU,
    0x060060U,
    0x860060U,
    0x168861U,
    0x0F00F0U,
    0x861168U,
    0x60771FU,
    0xF8FF9FU,
    0x99FF96U,
    0xE9EE9EU,
    0x788887U,
    0xE9999EU,
    0xF8EE8FU,
    0x88EE8FU,
    0x799987U,
    0x99FF99U,
    0xF6666FU,
    0x691111U,
    0x9E88E9U,
    0xF88888U,
    0x99FFF9U,
    0x9FFFF9U,
    0x699996U,
    0x88EE9EU,
    0x7F9996U,
    0x9EEE9EU,
    0xE16687U,
    0x66666FU,
    0xF99999U,
    0x699999U,
    0x9FFF99U,
    0x996699U,
    0x666699U,
    0xF8661FU,
    0x766667U,
    0x116688U,
    0xE6666EU,
    0x000096U,
    0xF00000U,
    0x000068U,
    0xFF11E0U,
    0xE999E8U,
    0x788870U,
    0x799971U,
    0x78FF60U,
    0x66FF61U,
    0xF79970U,
    0x9999E8U,
    0xF6EE06U,
    0x691101U,
    0x9EEE98U,
    0xF6666EU,
    0x99FFF0U,
    0x9999E0U,
    0x699960U,
    0x8E99E0U,
    0x179970U,
    0x888870U,
    0xE16670U,
    0x1666F6U,
    0x799990U,
    0x699990U,
    0x9F9990U,
    0x966690U,
    0xF79990U,
    0xF611F0U,
    0x16EE61U,
    0x666666U,
    0x867768U,
    0x00EE70U,
};

constexpr Glyph6x4 GetGlyphOrFallback(const char c, const char fallback) {
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

    return Glyph6x4{};
}

constexpr Glyph6x4 GetGlyph(const char c) {
    return GetGlyphOrFallback(c, '?');
}

} // namespace font_6x4

} // namespace modules
