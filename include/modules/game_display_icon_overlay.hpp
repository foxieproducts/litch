#pragma once

#include <array>
#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/game_display_overlay_shared.hpp"
#include "modules/grid_foreground_layer.hpp"
#include "modules/led_text_renderer.hpp"
#include "modules/rgb.hpp"

namespace modules {

struct GameDisplayIconOverlayConfig {
    // Timing: fade-in, hold, fade-out. Total duration is sum.
    uint32_t fadeInMs = 80;
    uint32_t holdMs = 840;
    uint32_t fadeOutMs = 100;

    // Background dim scale while overlay active: 0=black, 255=unchanged.
    uint8_t backgroundScale = 96;

    // Icon origin in the 8x8 grid.
    uint8_t iconX = 1;
    uint8_t iconY = 1;
};

// Icon overlay for a fixed 6x6 pixel icon rendered into the 8x8 grid.
//
// Snapshot behavior matches GameDisplayTextOverlay:
// - If not currently active, captures the provided frame as background.
// - If already active, preserves the existing snapshot to avoid flicker.
class GameDisplayIconOverlay final {
  public:
    static constexpr uint8_t ICON_W = 6;
    static constexpr uint8_t ICON_H = 6;
    static constexpr size_t ICON_PIXELS = static_cast<size_t>(ICON_W) * static_cast<size_t>(ICON_H);

    explicit GameDisplayIconOverlay(const GameDisplayIconOverlayConfig& cfg = {}) : m_cfg(cfg) {}

    bool TryCopySnapshot(std::array<Rgb, led_text_renderer::GRID_PIXELS>& outFrame) const {
        return game_display_overlay_shared::TryCopySnapshot(m_state, outFrame);
    }

    void Stop() {
        game_display_overlay_shared::ResetTimedSnapshotOverlay(m_state);
        m_iconPixels.fill(RGB_BLACK);
        m_pinned = false;
    }

    void Start(const std::array<Rgb, ICON_PIXELS>& iconPixels,
               const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame) {
        game_display_overlay_shared::StartTimedSnapshotOverlay(m_state, currentFrame);
        m_iconPixels = iconPixels;
        m_timeline = TimelineFromConfig();
        m_pinned = false;
    }

    // Owner-managed menu identity remains visible until Stop or another Start.
    void StartPinned(const std::array<Rgb, ICON_PIXELS>& iconPixels,
                     const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame) {
        if (!IsActive()) Stop();
        Start(iconPixels, currentFrame);
        m_pinned = true;
    }

    void Start(const std::array<Rgb, ICON_PIXELS>& iconPixels,
               const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame,
               const uint32_t holdMs,
               const uint32_t fadeOutMs = 100U) {
        game_display_overlay_shared::StartTimedSnapshotOverlay(m_state, currentFrame);
        m_iconPixels = iconPixels;
        m_timeline = game_display_overlay_shared::Timeline{0U, holdMs, fadeOutMs};
        m_pinned = false;
    }

    bool IsActive() const {
        return m_state.active && (m_pinned ||
            game_display_overlay_shared::IsTimedOverlayActive(m_state, m_timeline));
    }

    void Compose(const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame,
                 std::array<Rgb, led_text_renderer::GRID_PIXELS>& outFrame,
                 GridForegroundLayer* outForeground = nullptr) {
        if (outForeground != nullptr) {
            outForeground->Clear();
        }
        if (!IsActive()) {
            outFrame = currentFrame;
            m_state.active = false;
            return;
        }

        outFrame = m_state.snapshot;
        game_display_overlay_shared::ApplyBackgroundDim(outFrame, m_cfg.backgroundScale);

        const uint8_t alpha = m_pinned ? 255U :
            game_display_overlay_shared::ComputeAlpha255(m_timeline, ElapsedMs());
        if (outForeground != nullptr) {
            outForeground->background = outFrame;
            ExportForeground(alpha, *outForeground);
        }
        DrawIconBlended(outFrame, alpha);
    }

  private:
    uint32_t ElapsedMs() const {
        return m_state.elapsed.ElapsedMs();
    }

    game_display_overlay_shared::Timeline TimelineFromConfig() const {
        return game_display_overlay_shared::Timeline{m_cfg.fadeInMs, m_cfg.holdMs, m_cfg.fadeOutMs};
    }

    void DrawIconBlended(std::array<Rgb, led_text_renderer::GRID_PIXELS>& frame, const uint8_t alpha) const {
        const uint8_t originX = m_cfg.iconX;
        const uint8_t originY = m_cfg.iconY;

        for (uint8_t y = 0; y < ICON_H; ++y) {
            for (uint8_t x = 0; x < ICON_W; ++x) {
                const size_t idx = static_cast<size_t>(y) * ICON_W + x;
                const Rgb c = m_iconPixels[idx];

                // Treat RGB_BLACK as transparent for icons.
                if (c.r == 0 && c.g == 0 && c.b == 0) {
                    continue;
                }

                led_text_renderer::BlendPixel(frame,
                                              static_cast<uint8_t>(originX + x),
                                              static_cast<uint8_t>(originY + y),
                                              c,
                                              alpha);
            }
        }
    }

    uint64_t CoverageMask(const uint8_t alpha) const {
        if (alpha == 0U) {
            return 0ULL;
        }
        uint64_t mask = 0ULL;
        for (uint8_t y = 0; y < ICON_H; ++y) {
            for (uint8_t x = 0; x < ICON_W; ++x) {
                const Rgb c = m_iconPixels[static_cast<size_t>(y) * ICON_W + x];
                if (c.r == 0U && c.g == 0U && c.b == 0U) {
                    continue;
                }
                const uint8_t gridX = static_cast<uint8_t>(m_cfg.iconX + x);
                const uint8_t gridY = static_cast<uint8_t>(m_cfg.iconY + y);
                if (gridX < led_text_renderer::GRID_W &&
                    gridY < led_text_renderer::GRID_H) {
                    mask |= 1ULL << static_cast<uint8_t>(
                        gridY * led_text_renderer::GRID_W + gridX);
                }
            }
        }
        return mask;
    }

    void ExportForeground(const uint8_t alpha,
                          GridForegroundLayer& out) const {
        out.alpha255 = alpha;
        out.sourceCoverageMask = CoverageMask(alpha);
        out.placementCoverageMask = out.sourceCoverageMask;
        for (uint8_t y = 0; y < ICON_H; ++y) {
            for (uint8_t x = 0; x < ICON_W; ++x) {
                const uint8_t gridX = static_cast<uint8_t>(m_cfg.iconX + x);
                const uint8_t gridY = static_cast<uint8_t>(m_cfg.iconY + y);
                if (gridX >= led_text_renderer::GRID_W ||
                    gridY >= led_text_renderer::GRID_H) {
                    continue;
                }
                out.colors[static_cast<size_t>(gridY) *
                               led_text_renderer::GRID_W +
                           gridX] =
                    m_iconPixels[static_cast<size_t>(y) * ICON_W + x];
            }
        }
    }

    GameDisplayIconOverlayConfig m_cfg;
    bool m_pinned = false;
    game_display_overlay_shared::Timeline m_timeline = TimelineFromConfig();

    game_display_overlay_shared::TimedSnapshotOverlayState<led_text_renderer::GRID_PIXELS> m_state{};

    std::array<Rgb, ICON_PIXELS> m_iconPixels{};
};

} // namespace modules
