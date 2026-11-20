#pragma once

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/hint_thinking_style_setting.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "modules/ui_types.hpp"

namespace modules {
// Per-operation cosmetic lifetime; callers retain work ownership and priority.
// No engine or session is required to drive this presentation envelope.
class ControlLedThinkingPresentation {
  public:
    static constexpr uint32_t FADE_MS = 200U;
    void SetRequested(UiOwner owner) {
        const bool requested = owner != UiOwner::None;
        if (requested == m_requested && (!requested || owner == m_owner)) return;
        const uint16_t currentScale = Scale256();
        if (requested && (owner != m_owner || currentScale == 0U)) {
            m_owner = owner;
            m_episode.Reset();
            m_fromScale = 0U;
        } else {
            m_fromScale = currentScale;
        }
        m_requested = requested;
        m_transition.Reset();
    }
    uint16_t Scale256() const { return Scale256(fxe::Millis()); }
    uint16_t Scale256(const uint32_t nowMs) const {
        if (!m_transition.Started()) return 0U;
        const uint32_t elapsed = nowMs - m_transition.StartMs();
        if (elapsed >= FADE_MS) return m_requested ? 256U : 0U;
        const uint32_t phase = elapsed * 256U / FADE_MS;
        // Fixed-point smoothstep, independent of render cadence and millis wrap.
        const uint32_t eased = phase * phase * (768U - 2U * phase) / 65536U;
        return static_cast<uint16_t>(m_requested
            ? m_fromScale + (256U - m_fromScale) * eased / 256U
            : m_fromScale * (256U - eased) / 256U);
    }
    UiOwner Owner() const { return m_owner; }
    uint32_t ElapsedMs() const { return ElapsedMs(fxe::Millis()); }
    uint32_t ElapsedMs(const uint32_t nowMs) const {
        return m_episode.Started() ? nowMs - m_episode.StartMs() : 0U;
    }
  private:
    UiOwner m_owner = UiOwner::None;
    bool m_requested = false;
    uint16_t m_fromScale = 0U;
    fxe::ElapsedTime m_episode;
    fxe::ElapsedTime m_transition;
};

struct ControlLedThinkingFeedback {
    UiOwner owner = UiOwner::None;
    uint32_t elapsedMs = 0U;
    hint_thinking_style_setting::Style style = hint_thinking_style_setting::Style::Beachball;
    Rgb playerColor{};
    bool minimalAnimations = false;
    uint16_t scale256 = 256U;
};
// Reusable for any operation: supply its owner, elapsed time, style and fade.
// Compose additively into a fresh per-frame overlay after caller priority checks.
// On completion stop composing (or use owner None); this function does not erase
// an earlier frame or other owners' overlays. Minimal Animations stays static.
void ComposeControlLedThinkingFeedback(
    const ControlLedThinkingFeedback& feedback,
    status_led_frame_adapter::FixedPhysicalStatusOverlay& output);
void ComposeControlLedColorPreview(UiOwner owner, Rgb color,
    status_led_frame_adapter::FixedPhysicalStatusOverlay& output);
} // namespace modules
