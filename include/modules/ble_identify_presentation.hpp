#pragma once

#include "modules/menu_level_overlay.hpp"

namespace modules {

// The firmware renderer and VirtualLitch use this same transient timing owner.
// It consumes only admitted BLE presentation requests; it grants no access.
class BleIdentifyPresentation final {
  public:
    struct Intent {
        uint32_t revision = 0U;
        uint32_t deadlineMs = 0U;
        Rgb color = RGB_WHITE;
        uint8_t number = 1U;
        bool padTwoDigits = false;
        bool active = false;
    };
    Intent Capture() const {
        return {m_revision, m_deadlineMs, m_color, m_number, m_padTwoDigits, m_active};
    }
    void Apply(const Intent& intent) {
        if (intent.revision == m_revision) return;
        m_revision = intent.revision;
        m_deadlineMs = intent.deadlineMs;
        m_color = intent.color;
        m_number = intent.number;
        m_padTwoDigits = intent.padTwoDigits;
        m_active = intent.active;
        m_lastKickMs = 0U;
    }
    template<class BleSource>
    void Consume(BleSource& source, const uint32_t nowMs) {
        if (source.ConsumeIdentifyStop()) Stop();
        uint8_t number = 1U, red = 255U, green = 255U, blue = 255U;
        bool padTwoDigits = false;
        uint32_t durationMs = 3000U;
        if (source.ConsumeIdentifyStart(number, red, green, blue, padTwoDigits, durationMs))
            Start(number, {red, green, blue}, padTwoDigits, durationMs, nowMs);
    }
    void Start(const uint8_t number, const Rgb color, const bool padTwoDigits,
               const uint32_t durationMs, const uint32_t nowMs) {
        ++m_revision;
        m_active = true;
        m_number = number;
        m_color = color;
        m_padTwoDigits = padTwoDigits;
        m_deadlineMs = nowMs + durationMs;
        m_lastKickMs = 0U;
    }
    void Stop() { ++m_revision; m_active = false; }

    bool Update(const uint32_t nowMs, const bool quickSettingsActive,
                MenuLevelOverlay& overlay,
                const std::array<Rgb, led_text_renderer::GRID_PIXELS>& background) {
        if (m_active && static_cast<int32_t>(nowMs - m_deadlineMs) >= 0) m_active = false;
        const bool visible = m_active && !quickSettingsActive;
        if (!visible && m_previousVisible && overlay.GetOwner() == UiOwner::P1) overlay.Stop();
        m_previousVisible = visible;
        if (visible) {
            const uint32_t cadenceMs = m_padTwoDigits ? 150U : 1000U;
            if (!overlay.IsActive() || m_lastKickMs == 0U || nowMs - m_lastKickMs >= cadenceMs) {
                char text[3] = {'0', '\0', '\0'};
                if (m_padTwoDigits || m_number >= 10U) {
                    text[0] = static_cast<char>('0' + m_number / 10U);
                    text[1] = static_cast<char>('0' + m_number % 10U);
                } else text[0] = static_cast<char>('0' + m_number);
                overlay.SetOwner(UiOwner::P1);
                overlay.ShowMessage(text, m_color, background);
                m_lastKickMs = nowMs;
            }
        }
        return visible;
    }

  private:
    uint32_t m_revision = 0U;
    bool m_active = false;
    uint8_t m_number = 1U;
    Rgb m_color = RGB_WHITE;
    bool m_padTwoDigits = false;
    uint32_t m_deadlineMs = 0U;
    uint32_t m_lastKickMs = 0U;
    bool m_previousVisible = false;
};

} // namespace modules
