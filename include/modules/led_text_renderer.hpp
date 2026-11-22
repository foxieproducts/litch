#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/font_3x5.hpp"
#include "modules/font_6x4.hpp"
#include "modules/rgb.hpp"

namespace modules {

// Board text rasterizer:
// - Readable role draws 4-column by 6-row glyphs by default.
// - Compact role preserves the former 3-column by 5-row footprint.
// - Only blends "on" glyph pixels; "off" pixels leave the destination unchanged.
// - No heap usage; uses const char* (no Arduino String).
namespace led_text_renderer {

static constexpr uint8_t GRID_W = 8;
static constexpr uint8_t GRID_H = 8;
static constexpr size_t GRID_PIXELS = static_cast<size_t>(GRID_W) * static_cast<size_t>(GRID_H);

enum class FontRole : uint8_t {
    Readable = 0,
    Compact,
};

struct FontMetrics {
    uint8_t glyphWidth = 0;
    uint8_t glyphHeight = 0;
    uint8_t spacingX = 0;
};

constexpr FontMetrics GetFontMetrics(const FontRole role) {
    return role == FontRole::Compact
               ? FontMetrics{font_3x5::GLYPH_WIDTH,
                             font_3x5::GLYPH_HEIGHT, 1U}
               : FontMetrics{font_6x4::GLYPH_WIDTH,
                             font_6x4::GLYPH_HEIGHT, 0U};
}

// Compatibility aliases describe the default readable role. New layout code
// should use GetFontMetrics() so its role choice remains explicit.
static constexpr uint8_t GLYPH_W = font_6x4::GLYPH_WIDTH;
static constexpr uint8_t GLYPH_H = font_6x4::GLYPH_HEIGHT;
static constexpr uint8_t GLYPH_SPACING_X = 0;

constexpr uint8_t ClampU8(const uint16_t v) {
    return (v > 255U) ? 255U : static_cast<uint8_t>(v);
}

constexpr Rgb AlphaBlend(const Rgb base, const Rgb over, const uint8_t alpha) {
    const uint16_t inv = static_cast<uint16_t>(255U - alpha);

    const uint16_t r = static_cast<uint16_t>(base.r) * inv + static_cast<uint16_t>(over.r) * alpha;
    const uint16_t g = static_cast<uint16_t>(base.g) * inv + static_cast<uint16_t>(over.g) * alpha;
    const uint16_t b = static_cast<uint16_t>(base.b) * inv + static_cast<uint16_t>(over.b) * alpha;

    // Rounded divide by 255.
    return {
        ClampU8(static_cast<uint16_t>((r + 127U) / 255U)),
        ClampU8(static_cast<uint16_t>((g + 127U) / 255U)),
        ClampU8(static_cast<uint16_t>((b + 127U) / 255U)),
    };
}

inline void BlendPixel(std::array<Rgb, GRID_PIXELS>& grid, const uint8_t x, const uint8_t y, const Rgb color, const uint8_t alpha) {
    if (x >= GRID_W || y >= GRID_H) {
        return;
    }

    const size_t idx = static_cast<size_t>(y) * GRID_W + x;
    grid[idx] = AlphaBlend(grid[idx], color, alpha);
}

inline void DrawGlyphBlended(std::array<Rgb, GRID_PIXELS>& grid, const Glyph6x4 glyph, const int16_t originX, const int16_t originY, const Rgb color, const uint8_t alpha) {
    for (uint8_t row = 0; row < GLYPH_H; ++row) {
        const uint8_t bits = glyph.rows[row];
        for (uint8_t col = 0; col < GLYPH_W; ++col) {
            const uint8_t bit = static_cast<uint8_t>((GLYPH_W - 1U) - col);
            const bool on = ((bits >> bit) & 1U) != 0U;
            if (!on) {
                continue;
            }

            const int16_t x = static_cast<int16_t>(originX + col);
            const int16_t y = static_cast<int16_t>(originY + row);
            if (x < 0 || y < 0) {
                continue;
            }

            BlendPixel(grid, static_cast<uint8_t>(x), static_cast<uint8_t>(y), color, alpha);
        }
    }
}

inline void DrawGlyphBlended(std::array<Rgb, GRID_PIXELS>& grid,
                             const Glyph3x5 glyph,
                             const int16_t originX,
                             const int16_t originY,
                             const Rgb color,
                             const uint8_t alpha) {
    for (uint8_t row = 0; row < font_3x5::GLYPH_HEIGHT; ++row) {
        const uint8_t bits = glyph.rows[row];
        for (uint8_t col = 0; col < font_3x5::GLYPH_WIDTH; ++col) {
            if (((bits >> col) & 1U) == 0U) {
                continue;
            }

            const int16_t pixelX = static_cast<int16_t>(originX + col);
            const int16_t pixelY = static_cast<int16_t>(originY + row);
            if (pixelX < 0 || pixelY < 0) {
                continue;
            }

            BlendPixel(grid, static_cast<uint8_t>(pixelX),
                       static_cast<uint8_t>(pixelY), color, alpha);
        }
    }
}

inline bool IsGlyphPixelOn(const char c,
                           const FontRole role,
                           const uint8_t row,
                           const uint8_t col) {
    const FontMetrics metrics = GetFontMetrics(role);
    if (row >= metrics.glyphHeight || col >= metrics.glyphWidth) {
        return false;
    }

    const uint8_t bit = role == FontRole::Compact
                            ? col
                            : static_cast<uint8_t>(metrics.glyphWidth - 1U - col);
    const uint8_t bits = role == FontRole::Compact
                             ? font_3x5::GetGlyph(c).rows[row]
                             : font_6x4::GetGlyph(c).rows[row];
    return ((bits >> bit) & 1U) != 0U;
}

inline uint64_t RasterizeTextMask(const char* text,
                                  const uint8_t x,
                                  const uint8_t y,
                                  const FontRole role = FontRole::Readable) {
    if (text == nullptr) {
        return 0ULL;
    }

    const FontMetrics metrics = GetFontMetrics(role);
    int16_t cursorX = x;
    uint64_t mask = 0ULL;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '\n') {
            cursorX = x;
            continue;
        }

        for (uint8_t row = 0; row < metrics.glyphHeight; ++row) {
            for (uint8_t col = 0; col < metrics.glyphWidth; ++col) {
                const int16_t pixelX = static_cast<int16_t>(cursorX + col);
                const int16_t pixelY = static_cast<int16_t>(y + row);
                if (pixelX < 0 || pixelY < 0 || pixelX >= GRID_W ||
                    pixelY >= GRID_H ||
                    !IsGlyphPixelOn(text[i], role, row, col)) {
                    continue;
                }

                mask |= 1ULL << static_cast<uint8_t>(pixelY * GRID_W + pixelX);
            }
        }

        cursorX = static_cast<int16_t>(
            cursorX + metrics.glyphWidth + metrics.spacingX);
        if (cursorX >= GRID_W) {
            break;
        }
    }
    return mask;
}

inline void DrawTextBlended(std::array<Rgb, GRID_PIXELS>& grid,
                            const char* text,
                            const uint8_t x,
                            const uint8_t y,
                            const Rgb color,
                            const uint8_t alpha,
                            const FontRole role = FontRole::Readable) {
    if (text == nullptr) {
        return;
    }

    const FontMetrics metrics = GetFontMetrics(role);
    int16_t cursorX = x;
    const int16_t cursorY = y;

    for (size_t i = 0; text[i] != '\0'; ++i) {
        const char c = text[i];
        if (c == '\n') {
            cursorX = x;
            continue;
        }

        if (role == FontRole::Compact) {
            DrawGlyphBlended(grid, font_3x5::GetGlyph(c), cursorX, cursorY,
                             color, alpha);
        } else {
            DrawGlyphBlended(grid, font_6x4::GetGlyph(c), cursorX, cursorY,
                             color, alpha);
        }

        cursorX = static_cast<int16_t>(
            cursorX + metrics.glyphWidth + metrics.spacingX);
        if (cursorX >= GRID_W) {
            return;
        }
    }
}

} // namespace led_text_renderer

} // namespace modules
