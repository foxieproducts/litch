#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "foxie_embedded/fixed_string.hpp"
#include "modules/game_display_icon_overlay.hpp"
#include "modules/game_display_text_overlay.hpp"
#include "modules/grid_foreground_layer.hpp"
#include "modules/menu_icons_6x6.hpp"
#include "modules/menu_option_indices.hpp"
#include "modules/rgb.hpp"
#include "modules/ui_types.hpp"

namespace modules {

class MenuLevelOverlay final {
  public:
    using IconPixels = menu_icons_6x6::IconPixels;

    struct Config {
        GameDisplayTextOverlayConfig text;
        GameDisplayIconOverlayConfig icon;
    };

    explicit MenuLevelOverlay(const Config& cfg = {})
        : m_textOverlay(cfg.text),
          m_iconOverlay(cfg.icon) {}

    // Backward-compatible: allow existing call sites to override only text overlay timing.
    explicit MenuLevelOverlay(const GameDisplayTextOverlayConfig& overlayCfg)
        : m_textOverlay(overlayCfg),
          m_iconOverlay(GameDisplayIconOverlayConfig{}) {}

    void Stop() {
        ++m_revision;
        m_textOverlay.Stop();
        m_iconOverlay.Stop();
        m_pendingValueText.Clear();
        m_pendingValueAfterIcon = false;
        m_textCanPin = false;
        m_lastText.Clear();
        m_lastColor = RGB_WHITE;
        m_lastVsCpuLevel = 0;
        m_owner = UiOwner::None;
        m_preservesTurnIndicator = false;
    }

    void SetOwner(const UiOwner owner) { m_owner = owner; }
    // Input intent revision only. Per-frame Compose advances the renderer's
    // own copy without making an unchanged publication restart its timeline.
    uint32_t Revision() const { return m_revision; }
    UiOwner GetOwner() const { return m_owner; }

    bool IsActive() const {
        return m_textOverlay.IsActive() || m_iconOverlay.IsActive() || m_pendingValueAfterIcon;
    }
    bool PreservesTurnIndicator() const { return m_preservesTurnIndicator; }

    void ShowPinnedIcon(const IconPixels& pixels,
                        const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame) {
        ++m_revision;
        std::array<Rgb, led_text_renderer::GRID_PIXELS> background = currentFrame;
        if (m_iconOverlay.IsActive()) {
            (void)m_iconOverlay.TryCopySnapshot(background);
        } else if (m_textOverlay.IsActive()) {
            (void)m_textOverlay.TryCopySnapshot(background);
        }
        m_textOverlay.Stop();
        m_pendingValueAfterIcon = false;
        m_pendingValueText.Clear();
        m_textCanPin = false;
        m_lastText.Clear();
        m_preservesTurnIndicator = false;
        m_iconOverlay.StartPinned(pixels, background);
    }

    void ShowTransientIcon(const IconPixels& pixels,
                           const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame,
                           const uint32_t holdMs = 1000U,
                           const uint32_t fadeOutMs = 100U) {
        ++m_revision;
        m_textOverlay.Stop();
        m_pendingValueAfterIcon = false;
        m_pendingValueText.Clear();
        m_textCanPin = false;
        m_preservesTurnIndicator = true;
        m_iconOverlay.Start(pixels, currentFrame, holdMs, fadeOutMs);
    }

    void Compose(const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame,
                 std::array<Rgb, led_text_renderer::GRID_PIXELS>& outFrame,
                 const bool menuActive = false,
                 GridForegroundLayer* outForeground = nullptr) {
        // `menuActive` is also true while this overlay is merely visible.
        // Only owner-managed text may turn that visibility into a pin;
        // transient board feedback must keep its bounded timeline.
        m_textOverlay.SetPinned(menuActive && m_textCanPin);

        // Transition: if we were showing an icon and it just expired, start the value overlay now.
        if (m_pendingValueAfterIcon && !m_iconOverlay.IsActive()) {
            m_pendingValueAfterIcon = false;
            if (!m_pendingValueText.View().empty()) {
                std::array<Rgb, led_text_renderer::GRID_PIXELS> background = currentFrame;
                (void)m_iconOverlay.TryCopySnapshot(background);

                (void)StartTextOverlay(m_pendingValueText.View(),
                                       m_pendingValueColor,
                                       background,
                                       true);
            }
            m_pendingValueText.Clear();
        }

        if (m_iconOverlay.IsActive()) {
            m_iconOverlay.Compose(currentFrame, outFrame, outForeground);
            return;
        }

        m_textOverlay.Compose(currentFrame, outFrame, outForeground);
    }

    void OnMenuSelectionChanged(const uint8_t optionIndex,
                                const int value,
                                const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame,
                                const uint32_t uptimeMs,
                                const bool settingsLoaded,
                                const std::string_view fwVersion) {
        ++m_revision;
        // Selection change: show icon first, then show current value after the icon duration.
        IconPixels iconPixels;
        std::array<Rgb, led_text_renderer::GRID_PIXELS> background = currentFrame;
        // Important: if an overlay is already active, base the next overlay on the underlying
        // snapshot rather than the currently-composited frame. Otherwise icon pixels can leak
        // into the background and appear "behind" the next overlay.
        if (m_iconOverlay.IsActive()) {
            (void)m_iconOverlay.TryCopySnapshot(background);
        } else if (m_textOverlay.IsActive()) {
            (void)m_textOverlay.TryCopySnapshot(background);
        }
        const bool iconOk = TryStartIconOverlay(optionIndex, background, iconPixels);

        // Compute value text now so it is deterministic and independent of any later mutations.
        fxe::FixedString<3> text;
        Rgb color = RGB_WHITE;
        if (!ComputeValueOverlay(optionIndex, value, uptimeMs, settingsLoaded, fwVersion, text, color)) {
            // Unsupported option: no icon/value overlay.
            m_pendingValueAfterIcon = false;
            m_pendingValueText.Clear();
            return;
        }

        if (!iconOk) {
            // No icon available: show value immediately.
            (void)StartTextOverlay(text.View(), color, background);
            return;
        }

        m_textOverlay.Stop();
        m_lastText.Clear();
        m_pendingValueAfterIcon = true;
        m_pendingValueText.Clear();
        (void)m_pendingValueText.Assign(text.View());
        m_pendingValueColor = color;
    }

    void OnMenuValueChanged(const uint8_t optionIndex,
                            const int value,
                            const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame,
                            const uint32_t uptimeMs,
                            const bool settingsLoaded,
                            const std::string_view fwVersion) {
        ++m_revision;
        // Adjustment/toggle: show value immediately, and cancel any in-flight icon preview.
        std::array<Rgb, led_text_renderer::GRID_PIXELS> background = currentFrame;
        (void)m_iconOverlay.TryCopySnapshot(background);

        m_pendingValueAfterIcon = false;
        m_pendingValueText.Clear();
        m_iconOverlay.Stop();

        // Keep VS_CPU last-level tracking for tests and parity.
        if (optionIndex == menu_option_indices::VS_CPU) {
            m_lastVsCpuLevel = ClampVsCpuLevel(value);
        }

        fxe::FixedString<3> text;
        Rgb color = RGB_WHITE;
        if (!ComputeValueOverlay(optionIndex, value, uptimeMs, settingsLoaded, fwVersion, text, color)) {
            return;
        }

        (void)StartTextOverlay(text.View(), color, background);
    }

    void ShowMessage(const std::string_view text,
                     const Rgb color,
                     const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame) {
        ++m_revision;
        StartMessageOverlay(text, color, currentFrame, true);
    }

    void ShowTransientMessage(
        const std::string_view text,
        const Rgb color,
        const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame) {
        ++m_revision;
        StartMessageOverlay(text, color, currentFrame, false);
    }

    void OnMenuOptionChanged(const uint8_t optionIndex,
                             const int value,
                             const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame,
                             const uint32_t uptimeMs,
                             const bool settingsLoaded,
                             const std::string_view fwVersion) {
        OnMenuValueChanged(optionIndex, value, currentFrame, uptimeMs, settingsLoaded, fwVersion);
    }

    void OnVsCpuLevelChanged(const int level,
                             const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame) {
        m_lastVsCpuLevel = ClampVsCpuLevel(level);
        fxe::FixedString<3> text;
        (void)text.Format("%d", m_lastVsCpuLevel);
        if (!m_pendingValueAfterIcon && !m_iconOverlay.IsActive() &&
            m_textOverlay.IsActive() && m_textCanPin && m_lastText.View() == text.View() &&
            m_lastColor.r == RGB_WHITE.r && m_lastColor.g == RGB_WHITE.g &&
            m_lastColor.b == RGB_WHITE.b) {
            return;
        }
        ++m_revision;
        std::array<Rgb, led_text_renderer::GRID_PIXELS> background = currentFrame;
        (void)m_iconOverlay.TryCopySnapshot(background);
        m_pendingValueAfterIcon = false;
        m_pendingValueText.Clear();
        m_iconOverlay.Stop();
        (void)StartTextOverlay(text.View(), RGB_WHITE, background);
    }

    void RefreshVsCpuLevel(const int level,
                           const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame) {
        if (!m_pendingValueAfterIcon || !m_iconOverlay.IsActive()) {
            OnVsCpuLevelChanged(level, currentFrame);
            return;
        }

        // A remote change during the selection icon updates its queued value
        // without restarting the icon's existing timeline.
        ++m_revision;
        m_lastVsCpuLevel = ClampVsCpuLevel(level);
        (void)m_pendingValueText.Format("%d", m_lastVsCpuLevel);
    }

    std::string_view Text() const { return m_lastText.View(); }

    // Test-friendly accessors.
    int Test_GetLastVsCpuLevel() const { return m_lastVsCpuLevel; }
    std::string_view Test_GetLastText() const { return m_lastText.View(); }
    Rgb Test_GetLastColor() const { return m_lastColor; }

  private:
    static int ClampVsCpuLevel(const int level) {
        if (level <= 0) {
            return 0;
        }
        if (level >= 11) {
            return 11;
        }
        return level;
    }

    static constexpr std::array<Rgb, 8> PLAYER_COLOR_PRESETS = {
        Rgb{0, 128, 255},  // blue (default P1)
        Rgb{255, 64, 0},   // orange
        Rgb{0, 255, 0},    // green
        Rgb{255, 0, 0},    // red (default P2)
        Rgb{255, 255, 0},  // yellow
        Rgb{255, 0, 255},  // magenta
        Rgb{0, 255, 255},  // cyan
        Rgb{255, 255, 255} // white
    };

    static uint8_t ScaleU8(const uint8_t v, const uint16_t scale256) {
        const uint32_t scaled = static_cast<uint32_t>(v) * static_cast<uint32_t>(scale256);
        return static_cast<uint8_t>((scaled + 128U) >> 8);
    }

    static Rgb MakeTextColorLegible(const Rgb c) {
        const uint8_t maxc = std::max(c.r, std::max(c.g, c.b));
        if (maxc == 0) {
            return RGB_WHITE;
        }

        static constexpr uint8_t MIN_MAX_CHANNEL = 64;
        if (maxc >= MIN_MAX_CHANNEL) {
            return c;
        }

        const uint16_t scale256 = static_cast<uint16_t>((static_cast<uint32_t>(MIN_MAX_CHANNEL) * 256U + (maxc / 2U)) / maxc);
        return Rgb{ScaleU8(c.r, scale256), ScaleU8(c.g, scale256), ScaleU8(c.b, scale256)};
    }

    void StartMessageOverlay(
        const std::string_view text,
        const Rgb color,
        const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame,
        const bool canPin) {
        std::array<Rgb, led_text_renderer::GRID_PIXELS> background = currentFrame;
        (void)m_iconOverlay.TryCopySnapshot(background);

        m_pendingValueAfterIcon = false;
        m_pendingValueText.Clear();
        m_iconOverlay.Stop();

        (void)StartTextOverlay(text, color, background, canPin);
    }

    bool StartTextOverlay(const std::string_view text,
                          const Rgb color,
                          const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame,
                          const bool canPin = true) {
        m_preservesTurnIndicator = false;
        if (!m_textOverlay.IsActive()) {
            m_textOverlay.Stop();
        }
        if (!canPin) {
            // Clear any pin inherited from an earlier interactive menu before
            // starting a bounded message timeline.
            m_textOverlay.Stop();
        }
        m_textCanPin = canPin;
        m_lastText.Clear();
        (void)m_lastText.Assign(text);
        m_lastColor = color;

        m_textOverlay.Start(text, color, currentFrame);
        return true;
    }

    bool TryStartIconOverlay(const uint8_t optionIndex,
                             const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentFrame,
                             IconPixels& outPixels) {
        m_preservesTurnIndicator = false;
        const auto* def = menu_icons_6x6::GetIconForOptionIndex(optionIndex);
        if (!def) {
            return false;
        }

        if (!menu_icons_6x6::ParseIcon(*def, outPixels)) {
            return false;
        }

        m_iconOverlay.Start(outPixels, currentFrame);
        return true;
    }

    static bool ComputeValueOverlay(const uint8_t optionIndex,
                                    const int value,
                                    const uint32_t uptimeMs,
                                    const bool settingsLoaded,
                                    const std::string_view fwVersion,
                                    fxe::FixedString<3>& outText,
                                    Rgb& outColor) {
        switch (optionIndex) {
            case menu_option_indices::VS_CPU: {
                const int clamped = ClampVsCpuLevel(value);
                if (clamped <= 0) {
                    (void)outText.Assign("0");
                    outColor = RGB_WHITE;
                    return true;
                }

                (void)outText.Format("%d", clamped);
                outColor = RGB_WHITE;
                return true;
            }
            case menu_option_indices::BRIGHTNESS: {
                const int clamped = (value < 0) ? 0 : ((value > 8) ? 8 : value);
                (void)outText.Format("%d", clamped);
                outColor = RGB_YELLOW;
                return true;
            }
            case menu_option_indices::FLIP_SIDES: {
                const bool enabled = value != 0;
                (void)outText.Assign(enabled ? "1" : "0");
                outColor = enabled ? RGB_GREEN : RGB_WHITE;
                return true;
            }
            case menu_option_indices::CPU_PLAYER: {
                const bool isP2 = value == 2;
                (void)outText.Assign(isP2 ? "2" : "1");
                outColor = RGB_WHITE;
                return true;
            }
            case menu_option_indices::GAME_FIRST_PLAYER: {
                const bool p2First = value == 2;
                (void)outText.Assign(p2First ? "2" : "1");
                outColor = RGB_WHITE;
                return true;
            }
            case menu_option_indices::SLEEP: {
                (void)outText.Assign("SLP");
                outColor = RGB_WHITE;
                return true;
            }
            case menu_option_indices::TIMER: {
                static constexpr const char* TIMER_TEXT[] = {
                    "0",
                    "1",
                    "3",
                    "5",
                    "10",
                    "15",
                    "30",
                };
                const int clamped =
                    (value < 0) ? 0 : ((value > 6) ? 6 : value);
                (void)outText.Assign(TIMER_TEXT[clamped]);
                outColor = RGB_WHITE;
                return true;
            }
            default:
                return false;
        }
    }

    GameDisplayTextOverlay m_textOverlay;
    GameDisplayIconOverlay m_iconOverlay;

    bool m_pendingValueAfterIcon = false;
    bool m_textCanPin = false;
    fxe::FixedString<3> m_pendingValueText;
    Rgb m_pendingValueColor = RGB_WHITE;

    fxe::FixedString<3> m_lastText;
    Rgb m_lastColor = RGB_WHITE;

    int m_lastVsCpuLevel = 0;

    uint32_t m_revision = 0U;
    UiOwner m_owner = UiOwner::None;
    bool m_preservesTurnIndicator = false;
};

} // namespace modules
