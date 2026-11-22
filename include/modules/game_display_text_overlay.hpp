#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/fixed_string.hpp"
#include "modules/game_display_overlay_shared.hpp"
#include "modules/grid_foreground_layer.hpp"
#include "modules/led_text_renderer.hpp"
#include "modules/rgb.hpp"

namespace modules {

struct GameDisplayTextOverlayConfig {
    // Timing: fade-in, hold, fade-out. Total duration is sum.
    uint32_t fadeInMs = 120;
    uint32_t holdMs = 2000;
    uint32_t fadeOutMs = 120;

    // Background dim scale while overlay active: 0=black, 255=unchanged.
    uint8_t backgroundScale = 96;

    // Text origin in the 8x8 grid.
    uint8_t textX = 0;
    uint8_t textY = 1;

    // Readable is the product default. Compact is retained for intentionally
    // dense, secondary overlays that cannot fit the readable footprint.
    led_text_renderer::FontRole fontRole =
        led_text_renderer::FontRole::Readable;
};

class GameDisplayTextOverlay final {
  public:
    explicit GameDisplayTextOverlay(const GameDisplayTextOverlayConfig& cfg = {}) : m_cfg(cfg) {}

    void SetPinned(const bool pinned) {
        if (m_pinned && !pinned && m_state.active) {
            // Transitioning from pinned->unpinned: start a short fade-out now.
            // This ensures menu overlays don't disappear instantly after being
            // pinned longer than their nominal duration.
            m_unpinFadeOutActive = true;
            m_unpinFadeOutElapsed.Reset();
        }

        if (pinned) {
            // Re-pinning cancels any in-progress unpin fade.
            m_unpinFadeOutActive = false;
            m_unpinFadeOutElapsed = fxe::ElapsedTime();
        }

        m_pinned = pinned;
    }

    bool TryCopySnapshot(std::array<Rgb, led_text_renderer::GRID_PIXELS>& outFrame) const {
        return game_display_overlay_shared::TryCopySnapshot(m_state, outFrame);
    }

    void Stop() {
        game_display_overlay_shared::ResetTimedSnapshotOverlay(m_state);
        m_text.Clear();
        m_textColor = RGB_WHITE;
        m_pinned = false;
        m_unpinFadeOutActive = false;
        m_unpinFadeOutElapsed = fxe::ElapsedTime();
    }

    // Starts or restarts the overlay.
    //
    // Snapshot behavior:
    // - If not currently active, captures the provided frame as background.
    // - If already active, preserves the existing snapshot to avoid flicker.
    void Start(const std::string_view text,
               const Rgb textColor,
               const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame) {
        game_display_overlay_shared::StartTimedSnapshotOverlay(m_state, currentFrame);
        (void)m_text.Assign(text);
        m_textColor = textColor;
    }

    bool IsActive() const {
        if (!m_state.active) {
            return false;
        }

        if (m_pinned) {
            return true;
        }

        if (m_unpinFadeOutActive) {
            if (m_cfg.fadeOutMs == 0U) {
                return false;
            }
            return m_unpinFadeOutElapsed.ElapsedMs() < m_cfg.fadeOutMs;
        }

        const uint32_t total = game_display_overlay_shared::TotalDurationMs(Timeline());
        if (total == 0) {
            return false;
        }

        return game_display_overlay_shared::IsTimedOverlayActive(m_state, Timeline());
    }

    // Composes the output frame.
    // - If overlay inactive, passes through currentFrame.
    // - If overlay active, dims the captured snapshot and blends text.
    void Compose(const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame,
                 std::array<Rgb, led_text_renderer::GRID_PIXELS>& outFrame,
                 GridForegroundLayer* outForeground = nullptr) {
        if (outForeground != nullptr) {
            outForeground->Clear();
        }
        if (!IsActive()) {
            outFrame = currentFrame;
            m_state.active = false;
            m_unpinFadeOutActive = false;
            return;
        }

        outFrame = m_state.snapshot;
        game_display_overlay_shared::ApplyBackgroundDim(outFrame, m_cfg.backgroundScale);

        uint8_t alpha = 255;
        if (!m_pinned) {
            if (m_unpinFadeOutActive) {
                const uint32_t d = m_cfg.fadeOutMs;
                if (d == 0U) {
                    alpha = 0;
                } else {
                    const uint32_t t = m_unpinFadeOutElapsed.ElapsedMs();
                    if (t >= d) {
                        alpha = 0;
                    } else {
                        const uint32_t num = (d - t) * 255U;
                        alpha = static_cast<uint8_t>((num + (d / 2U)) / d);
                    }
                }
            } else {
                alpha = game_display_overlay_shared::ComputeAlpha255(Timeline(), ElapsedMs());
            }
        }
        const uint64_t coverageMask = alpha == 0U
            ? 0ULL
            : led_text_renderer::RasterizeTextMask(
                  m_text.CStr(), m_cfg.textX, m_cfg.textY, m_cfg.fontRole);
        if (outForeground != nullptr) {
            outForeground->background = outFrame;
            outForeground->colors.fill(m_textColor);
            outForeground->sourceCoverageMask = coverageMask;
            outForeground->placementCoverageMask = coverageMask;
            outForeground->alpha255 = alpha;
        }
        led_text_renderer::DrawTextBlended(outFrame,
                                           m_text.CStr(),
                                           m_cfg.textX,
                                           m_cfg.textY,
                                           m_textColor,
                                           alpha,
                                           m_cfg.fontRole);
    }

  private:
    uint32_t ElapsedMs() const {
        return m_state.elapsed.ElapsedMs();
    }

    game_display_overlay_shared::Timeline Timeline() const {
        return game_display_overlay_shared::Timeline{m_cfg.fadeInMs, m_cfg.holdMs, m_cfg.fadeOutMs};
    }

    GameDisplayTextOverlayConfig m_cfg;

    game_display_overlay_shared::TimedSnapshotOverlayState<led_text_renderer::GRID_PIXELS> m_state{};

    fxe::FixedString<32> m_text;
    Rgb m_textColor = RGB_WHITE;

    bool m_pinned = false;

    // One-shot fade-out timer when pinned is released.
    bool m_unpinFadeOutActive = false;
    fxe::ElapsedTime m_unpinFadeOutElapsed;
};

} // namespace modules
