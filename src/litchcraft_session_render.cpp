#include "modules/litchcraft_session.hpp"

#include <algorithm>

#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_animation.hpp"
#include "modules/litchcraft_persistence.hpp"

namespace modules {
namespace {
bool Same(const Rgb a, const Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
} // namespace

uint8_t LitchcraftSession::ColorPickBlendAlpha(const uint32_t nowMs) const {
    return litchcraft_presentation::ColorPickBlendAlpha(m_pickStartedMs, nowMs);
}

void LitchcraftSession::BlendColorPickPreview(BoardSquareLeds& square,
                                             const uint32_t nowMs) const {
    const uint8_t alpha = ColorPickBlendAlpha(nowMs);
    if (alpha == 0U) return;
    square.left = animation::BlendRgb(square.left, m_pickBefore[0], alpha);
    square.top = animation::BlendRgb(square.top, m_pickBefore[1], alpha);
    square.right = animation::BlendRgb(square.right, m_pickBefore[2], alpha);
    square.bottom = animation::BlendRgb(square.bottom, m_pickBefore[3], alpha);
}

Rgb LitchcraftSession::ColorPickSample(const uint32_t nowMs) const {
    auto colors = m_pickBefore;
    if (nowMs - m_pickStartedMs < PICK_ARM_MS + PICK_PROGRESS_MS) {
        // Sample the same square-effect owners used by Compose at the lift's
        // clock observation, before restoration or EndStroke changes authority.
        const uint64_t effectElapsed = m_advancedEffectElapsed +
            static_cast<uint32_t>(nowMs - m_advancedClock);
        const uint16_t base = static_cast<uint16_t>(m_pickSquare * 4U);
        if (HasStreamedDocument()) {
            std::array<litchcraft_frame_store::Emitter, 4U> stored{};
            uint16_t frame = 0U;
            uint16_t duration = 0U;
            if (!m_frameStream->CopyDisplayedSquare(
                    m_pickSquare, stored, frame, duration) ||
                frame != m_advancedStrokeFrame) return RGB_BLACK;
            colors = litchcraft_animation::ComposeStoredSquare(
                stored, effectElapsed, m_frameStream->SparkleSeed(), frame, base);
        } else if (HasAdvancedDocument()) {
            const auto& document = m_advancedPersistence->AdvancedDocument().Value();
            litchcraft_animation::Square square{};
            std::copy_n(document.frames[m_advancedStrokeFrame].emitters.begin() + base,
                        square.size(), square.begin());
            colors = litchcraft_animation::ComposeSquare(
                square, effectElapsed, document.sparkleSeed, m_advancedStrokeFrame, base);
        } else {
            std::copy_n(m_canvas.begin() + base, colors.size(), colors.begin());
        }
        BoardSquareLeds square{colors[0], colors[1], colors[2], colors[3]};
        BlendColorPickPreview(square, nowMs);
        colors = {square.left, square.top, square.right, square.bottom};
    }
    // A completed hold still picks intended source RGB, independent of effects.
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        if ((m_target & (1U << slot)) != 0U && !Same(colors[slot], RGB_BLACK)) {
            return colors[slot];
        }
    }
    return RGB_BLACK;
}

uint16_t LitchcraftSession::PlaybackStatusScale256() const {
    return litchcraft_presentation::PlaybackStatusScale256(m_advancedPlaying,
        m_playbackStatusFrom256, fxe::Millis() - m_playbackStatusChangedAtMs);
}

void LitchcraftSession::SetAnimationPlaying(const bool playing) {
    if (playing == m_advancedPlaying) return;
    // Retarget the existing fade from its current visible level. Rapid taps
    // cannot flash the corners or delay the authoritative playback edge.
    m_playbackStatusFrom256 = PlaybackStatusScale256();
    m_playbackStatusChangedAtMs = fxe::Millis();
    m_advancedPlaying = playing;
}

void LitchcraftSession::Compose(BoardLedFrame& board,
    std::array<Rgb, EDGE_LED_COUNT>& status) {
    litchcraft_presentation::Inputs inputs{};
    CapturePresentation(inputs);
    const litchcraft_presentation::StreamPort port{
        m_frameStream,
        [](void* owner, bool playing, uint32_t session, bool active, bool) {
            auto& self = *static_cast<LitchcraftSession*>(owner);
            if (self.m_frameStreamWakeStorage != nullptr)
                self.m_frameStreamWakeStorage(self.m_frameStreamWakeStorageContext,
                    playing, session, active);
        }, this, false};
    litchcraft_presentation::Renderer renderer;
    auto feedback = renderer.Compose(inputs, port, board, status);
    if (renderer.NotePresented(feedback, port))
        (void)ApplyPresentationFeedback(feedback);
}

void LitchcraftSession::ComposeEntryStatus(
    std::array<Rgb, EDGE_LED_COUNT>& status) const {
    const auto hold = CaptureEntryHoldPresentation();
    if (hold.active) litchcraft_presentation::ComposeEntryStatus(
        m_hardware, fxe::Millis() - hold.startedAtMs, status);
}
} // namespace modules
