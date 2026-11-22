#include "modules/litchcraft_session.hpp"

#include <algorithm>
#include <new>

#include "foxie_embedded/millis.hpp"

namespace modules {
void LitchcraftSession::SetMutationsPaused(const bool paused) {
    if (paused) CancelPendingStoreHistory();
    if (paused && m_pendingStoreStructural && m_frameStream != nullptr) {
        m_frameStream->CancelStructuralEdit();
        m_pendingStoreStructural = false;
        m_pendingStoreOpcode = 0U;
    }
    if (paused) m_pendingPhysicalHistoryFrame = UINT16_MAX;
    if (paused) RetireV2Transfer();
    if (paused && HasStreamedDocument() && m_frameStream->HasUnpersistedEdit()) {
        m_frameStream->RequestImmediateSave();
        WakeStreamedStorage();
    }
    m_mutationsPaused = paused;
    if (!paused) return;
    EndStroke();
    CancelEntryHold();
    m_pickPending = false;
    m_pickConfirmed = false;
    CloseQuickPalette();
    m_replacing = false;
    m_received.reset();
    m_restoreOccupancyPending = m_active;
}
void LitchcraftSession::MarkCanvasChanged() {
    ++m_canvasGeneration;
    m_canvasChangedAtMs = fxe::Millis();
}
void LitchcraftSession::MarkModeChanged() {
    CancelPendingStoreHistory();
    if (m_pendingStoreStructural && m_frameStream != nullptr) {
        m_frameStream->CancelStructuralEdit();
        m_pendingStoreStructural = false;
        m_pendingStoreOpcode = 0U;
    }
    // Local and app entry both retire inactive-storage staging; a transfer
    // cannot cross the boundary into a newly acquired presentation session.
    RetireV2Transfer();
    m_storeGuardConnectionEpoch = 0U;
    m_storeGuardRevision = 0U;
    ++m_modeGeneration;
    m_modeChangedAtMs = fxe::Millis();
}
void LitchcraftSession::MarkPaletteChanged() {
    if (HasAdvancedDocument()) { UpdateAdvancedFavorites(); return; }
    ++m_paletteGeneration;
    m_paletteChangedAtMs = fxe::Millis();
}
bool LitchcraftSession::ReplaceCanvas(const Canvas& canvas) {
    if (m_mutationsPaused) return false;
    const bool same = std::equal(m_canvas.begin(), m_canvas.end(), canvas.begin(),
        [](const Rgb a, const Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; });
    if (same) return false;
    m_canvas = canvas;
    MarkCanvasChanged();
    return true;
}
void LitchcraftSession::RestoreArtwork(const Canvas& canvas, const bool active,
                                      const uint64_t occupancy,
                                      const LitchcraftQuickPalette* quickPalette,
                                      const bool bootOccupancyKnown) {
    EndConnection();
    ResetPhysicalStoreBatch();
#if !defined(LITCH_ESP32S3)
    if (m_advancedRuntime) {
        new (&m_history) LitchcraftHistory();
        m_advancedRuntime = false;
    } else {
        m_history.~LitchcraftHistory();
        new (&m_history) LitchcraftHistory();
    }
#endif
    m_canvas = canvas;
    m_recentColors = quickPalette == nullptr
        ? LITCHCRAFT_DEFAULT_QUICK_PALETTE
        : *quickPalette;
    m_quickPaletteColors = m_recentColors;
    m_quickPaletteVisible = false;
    m_quickPaletteFading = false;
    m_quickPaletteOwner = UiOwner::None;
    m_strokeChanged = false;
    m_strokeSource = Source::None;
    m_pickPending = false;
    m_pickConfirmed = false;
    m_modeChordFormed = false;
    m_active = active;
    m_exitFlushRequested = false;
    m_exited = false;
    m_occupancy = occupancy;
    m_blockedOccupancy = occupancy;
    m_restoreOccupancyPending = active && !bootOccupancyKnown;
    m_session = ++m_nonce;
    if (m_session == 0U) m_session = ++m_nonce;
    m_revision = 1U;
    m_canvasGeneration = 0U;
    m_modeGeneration = 0U;
    m_paletteGeneration = 0U;
    m_canvasInvalidated = true;
}
} // namespace modules
