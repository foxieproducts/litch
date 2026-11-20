#include "modules/litchcraft_session.hpp"

#include <algorithm>

#include "foxie_embedded/millis.hpp"
#include "modules/ui_color_wheel.hpp"
#include "modules/litchcraft_palette.hpp"

namespace modules {
namespace {
constexpr uint8_t QUICK_PALETTE_COLOR_COUNT = 32U;
constexpr uint8_t QUICK_PALETTE_WHEEL_STEP = 8U;
constexpr uint8_t QUICK_PALETTE_CENTER_SLOT = 3U;

bool Same(const Rgb a, const Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

Rgb QuickPaletteColor(const uint8_t index) {
    return litchcraft_palette::Color(index);
}
} // namespace

void LitchcraftSession::RememberColor(const Rgb color) {
    if (Same(color, RGB_BLACK) || Same(color, m_recentColors[0])) return;
    size_t found = m_recentColors.size();
    for (size_t i = 1U; i < m_recentColors.size(); ++i) {
        if (Same(color, m_recentColors[i])) {
            found = i;
            break;
        }
    }
    const size_t shiftEnd = std::min(found, m_recentColors.size() - 1U);
    for (size_t i = shiftEnd; i > 0U; --i) {
        m_recentColors[i] = m_recentColors[i - 1U];
    }
    m_recentColors[0] = color;
    MarkPaletteChanged();
}

LitchcraftSession::Status LitchcraftSession::ApplyPaletteChunk(
    const uint8_t* payload, const size_t size) {
    if (size < 5U || size > 11U || (size - 2U) % 3U != 0U) {
        m_paletteStagingCount = 0U;
        return Status::Malformed;
    }
    const uint8_t offset = payload[0];
    const uint8_t count = payload[1];
    if (count == 0U || count != (size - 2U) / 3U ||
        offset + count > m_paletteStaging.size()) {
        m_paletteStagingCount = 0U;
        return Status::Malformed;
    }
    if (offset == 0U) {
        m_paletteStagingCount = 0U;
    }
    if (offset != m_paletteStagingCount) {
        m_paletteStagingCount = 0U;
        return Status::Incomplete;
    }
    for (uint8_t i = 0U; i < count; ++i) {
        const size_t source = 2U + size_t(i) * 3U;
        m_paletteStaging[offset + i] = {
            payload[source], payload[source + 1U], payload[source + 2U]};
    }
    m_paletteStagingCount = static_cast<uint8_t>(offset + count);
    if (m_paletteStagingCount != m_paletteStaging.size()) {
        return Status::Ok;
    }
    m_paletteStagingCount = 0U;
    if (std::equal(m_recentColors.begin(), m_recentColors.end(),
                   m_paletteStaging.begin(), Same)) {
        return Status::Ok;
    }
    m_recentColors = m_paletteStaging;
    m_quickPaletteColors = m_recentColors;
    MarkPaletteChanged();
    return Status::Ok;
}

uint8_t LitchcraftSession::QuickPaletteSquare(const uint8_t index) const {
    return m_quickPaletteOwner == UiOwner::P2
        ? static_cast<uint8_t>(7U - index)
        : static_cast<uint8_t>(56U + index);
}

void LitchcraftSession::CloseQuickPalette() {
    if (!m_quickPaletteVisible) return;
    m_quickPaletteVisible = false;
    m_quickPaletteFading = true;
    m_quickPaletteFadeAtMs = fxe::Millis();
    // A piece left on the selection row must lift before becoming a brush.
    m_blockedOccupancy |= m_occupancy;
}

void LitchcraftSession::CenterQuickPaletteWindow() {
    m_quickPaletteWindowStart = static_cast<uint8_t>(
        (m_quickPaletteWheelIndex + QUICK_PALETTE_COLOR_COUNT -
         QUICK_PALETTE_CENTER_SLOT) % QUICK_PALETTE_COLOR_COUNT);
    m_quickPaletteIndex = QUICK_PALETTE_CENTER_SLOT;
    for (uint8_t slot = 0U; slot < m_quickPaletteColors.size(); ++slot) {
        const uint8_t index = static_cast<uint8_t>(
            (m_quickPaletteWindowStart + slot) % QUICK_PALETTE_COLOR_COUNT);
        m_quickPaletteColors[slot] = QuickPaletteColor(index);
    }
}

void LitchcraftSession::OpenOrStepQuickPalette(const ButtonEvent& event) {
    const uint32_t now = fxe::Millis();
    m_quickPaletteFading = false;
    if (!m_quickPaletteVisible || m_quickPaletteOwner != event.owner) {
        EndStroke();
        m_pickPending = false;
        m_pickConfirmed = false;
        m_quickPaletteVisible = true;
        m_quickPaletteOwner = event.owner == UiOwner::P2 ? UiOwner::P2 : UiOwner::P1;
        m_quickPaletteWheelIndex = static_cast<uint8_t>(
            (static_cast<uint16_t>(m_colorWheel) + 4U) /
            QUICK_PALETTE_WHEEL_STEP) % QUICK_PALETTE_COLOR_COUNT;
    }
    const bool previous = event.button == UserButton::Left;
    m_quickPaletteWheelIndex = static_cast<uint8_t>(
        (m_quickPaletteWheelIndex +
         (previous ? QUICK_PALETTE_COLOR_COUNT - 1U : 1U)) %
        QUICK_PALETTE_COLOR_COUNT);
    CenterQuickPaletteWindow();
    m_quickPaletteTouchedMs = now;
    ChangeTool(false, m_target, QuickPaletteColor(m_quickPaletteWheelIndex),
               0U, false);
}

bool LitchcraftSession::SelectQuickPaletteFromOccupancy(const uint64_t occupancy) {
    if (!m_quickPaletteVisible) return false;
    for (uint8_t slot = 0U; slot < 8U; ++slot) {
        if ((occupancy & (UINT64_C(1) << QuickPaletteSquare(slot))) == 0U) continue;
        m_quickPaletteIndex = slot;
        m_quickPaletteWheelIndex = static_cast<uint8_t>(
            (m_quickPaletteWindowStart + slot) % QUICK_PALETTE_COLOR_COUNT);
        m_quickPaletteTouchedMs = fxe::Millis();
        ChangeTool(false, m_target, QuickPaletteColor(m_quickPaletteWheelIndex),
                   0U, false);
        return true;
    }
    return false;
}
} // namespace modules
