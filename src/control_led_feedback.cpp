#include "modules/control_led_feedback.hpp"

#include <algorithm>

#include "foxie_embedded/millis.hpp"
#include "modules/animation_timing.hpp"

namespace modules {
namespace {
constexpr uint32_t PALETTE_EDIT_FADE_MS = 500U;

void NormalizeBank(status_led_frame_adapter::FixedPhysicalStatusOverlay& out, const uint8_t bank) {
    if (out.controlAlpha255[bank] == 255U) return;
    for (uint8_t position = 0U; position < 6U; ++position) {
        auto& pixel = out.controlPixels[bank * 6U + position];
        pixel = AlphaBlendRgb({}, pixel, out.controlAlpha255[bank]);
    }
    out.controlAlpha255[bank] = 255U;
}
void Set(status_led_frame_adapter::FixedPhysicalStatusOverlay& out,
         const uint8_t bank, const ControlRole role, const Rgb color) {
    NormalizeBank(out, bank);
    const uint8_t index = bank * 6U + static_cast<uint8_t>(role);
    out.controlPixels[index] = color;
    out.controlMask |= static_cast<uint16_t>(1U << index);
}
Rgb Blend(const Rgb a, const Rgb b, const uint32_t phase256) {
    return AlphaBlendRgb(a, b, static_cast<uint8_t>(std::min(phase256, 255U)));
}
uint16_t Fade(const uint32_t elapsed, const uint32_t duration) {
    return duration == 0U || elapsed >= duration ? 0U :
        static_cast<uint16_t>((duration - elapsed) * 256U / duration);
}
} // namespace

void ControlLedFeedback::Reset() {
    ++m_eventRevision;
    ++m_resetRevision;
    m_events = {};
    m_scene = ControlFeedbackScene::Suppressed;
    m_lastMenuControls = {};
    m_menuFadingOut = false;
}
void ControlLedFeedback::Trigger(const ControlFeedbackAction action,
    const UiOwner owner, const ControlRole role, const Rgb color,
    const ControlFeedbackScene scene) {
    if (owner == UiOwner::None) return;
    ++m_eventRevision;
    m_events[owner == UiOwner::P2 ? 1U : 0U] =
        {action, scene, role, color, fxe::Millis(), true};
}
void ControlLedFeedback::ApplyEvents(const ControlLedFeedbackEvents& events) {
    if (m_appliedResetRevision != events.resetRevision) {
        Reset();
        m_appliedResetRevision = events.resetRevision;
    }
    if (m_appliedEventRevision == events.revision) return;
    m_events = events.events;
    m_appliedEventRevision = events.revision;
}
void ControlLedFeedback::Compose(const ControlLedFeedbackInputs& in,
    status_led_frame_adapter::FixedPhysicalStatusOverlay& out) {
    const uint32_t now = fxe::Millis();
    if (!m_config.enabled || in.scene == ControlFeedbackScene::Suppressed) {
        Reset();
        return;
    }
    for (auto& event : m_events) {
        if (event.action == ControlFeedbackAction::Hint &&
            !in.hintFeedbackAllowed)
            event.active = false;
    }
    if (m_scene != in.scene) {
        m_menuFadingOut = m_scene == ControlFeedbackScene::QuickSetup &&
            m_config.menuGuidance;
        m_scene = in.scene;
        m_sceneStartedMs = now;
        for (auto& event : m_events)
            if (event.scene != in.scene) event.active = false;
    }
    const bool menu = in.scene == ControlFeedbackScene::QuickSetup;
    const bool craft = in.scene == ControlFeedbackScene::Litchcraft;
    if (menu) m_lastMenuControls = in.availableControls;
    const uint16_t menuExitFade = m_menuFadingOut
        ? Fade(now - m_sceneStartedMs, m_config.menuFadeMs) : 0U;
    if (menuExitFade == 0U) m_menuFadingOut = false;
    for (uint8_t bank = 0U; bank < 2U; ++bank) {
        if (menu && m_config.menuGuidance) {
            const uint32_t elapsed = now - m_sceneStartedMs;
            const uint16_t fade = m_config.menuFadeMs == 0U ? 256U :
                std::min<uint32_t>(256U, elapsed * 256U / m_config.menuFadeMs);
            for (uint8_t role = 0U; role < 6U; ++role) {
                Rgb color{};
                if ((in.availableControls[bank] & (1U << role)) != 0U) {
                    color = role == static_cast<uint8_t>(ControlRole::X)
                        ? m_config.caution : m_config.navigation;
                    color = ScaleRgbSaturatingBy256(color,
                        m_config.guidanceScale256 * fade / 256U);
                }
                Set(out, bank, static_cast<ControlRole>(role), color);
            }
        } else if (craft && m_config.persistent) {
            for (const auto role : {ControlRole::X, ControlRole::O}) {
                const bool available = (in.availableControls[bank] &
                    (1U << static_cast<uint8_t>(role))) != 0U;
                Set(out, bank, role, available
                    ? ScaleRgbSaturatingBy256(m_config.neutral, m_config.persistentScale256)
                    : Rgb{});
            }
        } else if (m_config.persistent &&
                   (out.controlMask & (1U << (bank * 6U))) == 0U) {
            Rgb color{};
            if (in.coaching[bank]) color = m_config.coaching;
            if (color.r || color.g || color.b)
                Set(out, bank, ControlRole::O,
                    ScaleRgbSaturatingBy256(color, m_config.persistentScale256));
        }
        if (!menu && menuExitFade != 0U) {
            for (uint8_t role = 0U; role < 6U; ++role) {
                if ((m_lastMenuControls[bank] & (1U << role)) == 0U) continue;
                const Rgb old = ScaleRgbSaturatingBy256(
                    role == static_cast<uint8_t>(ControlRole::X)
                        ? m_config.caution : m_config.navigation,
                    m_config.guidanceScale256);
                Set(out, bank, static_cast<ControlRole>(role),
                    Blend(out.controlPixels[bank * 6U + role], old, menuExitFade));
            }
        }
        auto& event = m_events[bank];
        const uint32_t elapsed = now - event.startedMs;
        const bool history = event.action == ControlFeedbackAction::Undo ||
                             event.action == ControlFeedbackAction::Redo;
        const bool brewAcknowledgment = event.action == ControlFeedbackAction::Hint;
        const bool unavailable = event.action == ControlFeedbackAction::Unavailable;
        const bool paletteEdit = event.action == ControlFeedbackAction::PaletteEdit;
        const uint32_t duration = unavailable
            ? m_config.unavailablePulseMs * 2U + m_config.unavailableGapMs
            : brewAcknowledgment
            ? animation::LED_FRAME_TOKENS.hintBrewAcknowledgment.phase.periodMs
            : paletteEdit ? PALETTE_EDIT_FADE_MS
            : history ? m_config.historyMs : m_config.actionMs;
        if (event.active && elapsed >= duration) event.active = false;
        if (event.active && event.scene == in.scene && m_config.confirmations && brewAcknowledgment) {
            const auto scale = animation::SampleValue({elapsed},
                animation::LED_FRAME_TOKENS.hintBrewAcknowledgment);
            const uint8_t peak = animation::LED_FRAME_TOKENS.hintBrewAcknowledgment.curve.maxValue;
            const uint8_t alpha = peak == 0U ? 0U : static_cast<uint8_t>(scale * 255U / peak);
            const Rgb color = ScaleRgbSaturatingBy256(in.playerColors[bank], peak);
            NormalizeBank(out, bank);
            const auto base = out.controlPixels[bank * 6U + static_cast<uint8_t>(ControlRole::O)];
            Set(out, bank, ControlRole::O, AlphaBlendRgb(base, color, alpha));
        }
        if (event.active && event.scene == in.scene && m_config.confirmations && unavailable) {
            const uint32_t secondStart = m_config.unavailablePulseMs + m_config.unavailableGapMs;
            const uint32_t pulseElapsed = elapsed < m_config.unavailablePulseMs ? elapsed
                : elapsed >= secondStart ? elapsed - secondStart : m_config.unavailablePulseMs;
            const uint8_t alpha = animation::SampleValue({pulseElapsed}, {
                {m_config.unavailablePulseMs, animation::PlaybackMode::OneShot, 0U},
                {animation::Waveform::Breathe, 0U, 255U, 128U, 28U}});
            NormalizeBank(out, bank);
            const auto base = out.controlPixels[bank * 6U + static_cast<uint8_t>(ControlRole::O)];
            Set(out, bank, ControlRole::O, AlphaBlendRgb(base,
                ScaleRgbSaturatingBy256(m_config.caution, m_config.actionScale256), alpha));
        }
        if (event.active && event.scene == in.scene && m_config.confirmations &&
            !brewAcknowledgment && !unavailable) {
            const bool directionalAcknowledgment =
                paletteEdit || event.action == ControlFeedbackAction::Acknowledge;
            Rgb color = m_config.neutral;
            switch (event.action) {
                case ControlFeedbackAction::Navigate:
                case ControlFeedbackAction::Hint: color = m_config.navigation; break;
                case ControlFeedbackAction::Coaching: color = m_config.coaching; break;
                case ControlFeedbackAction::Clear: color = m_config.destructive; break;
                case ControlFeedbackAction::Cancel: color = m_config.caution; break;
                default: break;
            }
            if (directionalAcknowledgment &&
                (event.color.r || event.color.g || event.color.b)) color = event.color;
            if (craft && event.action == ControlFeedbackAction::Acknowledge &&
                (event.role == ControlRole::Up || event.role == ControlRole::Down)) color = in.paintColor;
            const Rgb peak = ScaleRgbSaturatingBy256(color, m_config.actionScale256);
            std::array<Rgb, 6> layer{};
            layer[static_cast<uint8_t>(event.role)] = peak;
            if (history && m_config.historySweeps && !in.minimalAnimations && duration != 0U) {
                const auto first = event.action == ControlFeedbackAction::Undo ? ControlRole::Right : ControlRole::Left;
                const auto second = event.action == ControlFeedbackAction::Undo ? ControlRole::Left : ControlRole::Right;
                const uint16_t progress = std::min<uint32_t>(256U, elapsed * 256U / duration);
                layer[static_cast<uint8_t>(first)] = ScaleRgbSaturatingBy256(peak, 256U - progress);
                layer[static_cast<uint8_t>(second)] = ScaleRgbSaturatingBy256(peak, progress);
            }
            NormalizeBank(out, bank);
            const uint16_t alpha = Fade(elapsed, duration);
            for (uint8_t role = 0U; role < 6U; ++role) {
                if ((menu || directionalAcknowledgment) &&
                    role != static_cast<uint8_t>(event.role) && !history) continue;
                Set(out, bank, static_cast<ControlRole>(role),
                    Blend(out.controlPixels[bank * 6U + role], layer[role], alpha));
            }
        }
        if (!m_config.holdProgress) continue;
        const ControlFeedbackHold* activeHold = nullptr;
        for (const auto& hold : in.holds[bank]) {
            if (hold.elapsedMs <= m_config.progressDelayMs ||
                hold.thresholdMs <= m_config.progressDelayMs) continue;
            if (activeHold == nullptr || hold.destructive) activeHold = &hold;
        }
        if (activeHold != nullptr) {
            const auto& hold = *activeHold;
            const uint32_t progress = std::min<uint32_t>(256U,
                (hold.elapsedMs - m_config.progressDelayMs) * 256U /
                (hold.thresholdMs - m_config.progressDelayMs));
            const uint16_t scale = m_config.holdScale256 * progress / 256U;
            if (scale == 0U) continue;
            const Rgb color = hold.destructive
                ? Blend(m_config.neutral, m_config.destructive, progress) : m_config.neutral;
            for (uint8_t role = 0U; role < 6U; ++role)
                Set(out, bank, static_cast<ControlRole>(role), {});
            Set(out, bank, hold.role, ScaleRgbSaturatingBy256(color, scale));
        }
    }
}
} // namespace modules
