#pragma once

#include <array>
#include <cstdint>

#include "hardware/status_led_chain_profile.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "modules/ui_types.hpp"

namespace modules {
using ControlRole = hardware::ControlLedRole;

enum class ControlFeedbackScene : uint8_t { Play, QuickSetup, Litchcraft, Suppressed };
enum class ControlFeedbackAction : uint8_t {
    Acknowledge, Undo, Redo, Hint, Coaching, Clear, Navigate, Cancel, Unavailable,
    PaletteEdit,
};

// Compile-time presentation controls only; functional thinking/safety owners
// and gameplay do not depend on these cosmetic switches.
struct ControlLedFeedbackConfig {
    bool enabled = true;
    bool persistent = true;
    bool confirmations = true;
    bool holdProgress = true;
    bool historySweeps = true;
    bool menuGuidance = true;
    uint16_t persistentScale256 = 8U;
    uint16_t guidanceScale256 = 8U;
    uint16_t actionScale256 = 38U;
    uint16_t holdScale256 = 51U;
    uint32_t actionMs = 150U;
    uint32_t historyMs = 180U;
    uint32_t progressDelayMs = 200U;
    uint32_t menuFadeMs = 150U;
    uint32_t unavailablePulseMs = 300U;
    uint32_t unavailableGapMs = 120U;
    Rgb neutral{255U, 255U, 255U};
    Rgb navigation{48U, 220U, 255U};
    Rgb caution{255U, 150U, 12U};
    Rgb destructive{255U, 0U, 0U};
    Rgb coaching{150U, 50U, 255U};
};
inline constexpr ControlLedFeedbackConfig DEFAULT_CONTROL_LED_FEEDBACK{};

struct ControlFeedbackHold {
    ControlRole role = ControlRole::O;
    uint32_t elapsedMs = 0U;
    uint32_t thresholdMs = 1000U;
    bool destructive = false;
    bool active = false;
};
struct ControlLedFeedbackInputs {
    ControlFeedbackScene scene = ControlFeedbackScene::Play;
    std::array<bool, 2> coaching{};
    std::array<Rgb, 2> playerColors{};
    bool hintFeedbackAllowed = true;
    std::array<uint8_t, 2> availableControls{};
    std::array<std::array<ControlFeedbackHold, 2>, 2> holds{};
    Rgb paintColor{};
    bool minimalAnimations = false;
};

// Main publishes replaceable event intents; the renderer owns scene fades.
struct ControlFeedbackEvent {
    ControlFeedbackAction action = ControlFeedbackAction::Acknowledge;
    ControlFeedbackScene scene = ControlFeedbackScene::Play;
    ControlRole role = ControlRole::O;
    Rgb color{};
    uint32_t startedMs = 0U;
    bool active = false;
};
struct ControlLedFeedbackEvents {
    std::array<ControlFeedbackEvent, 2> events{};
    uint32_t revision = 0U;
    uint32_t resetRevision = 0U;
};

// One replaceable event per player: no queue, allocation, sensor access or I/O.
// Inputs carry mode, history availability, Coaching and gesture authority.
class ControlLedFeedback final {
  public:
    explicit ControlLedFeedback(const ControlLedFeedbackConfig& config = DEFAULT_CONTROL_LED_FEEDBACK)
        : m_config(config) {}
    void Reset();
    void Trigger(ControlFeedbackAction action, UiOwner owner,
                 ControlRole role = ControlRole::O, Rgb color = {},
                 ControlFeedbackScene scene = ControlFeedbackScene::Play);
    ControlLedFeedbackEvents CaptureEvents() const {
        return {m_events, m_eventRevision, m_resetRevision};
    }
    void ApplyEvents(const ControlLedFeedbackEvents& events);
    void Compose(const ControlLedFeedbackInputs& inputs,
                 status_led_frame_adapter::FixedPhysicalStatusOverlay& output);
  private:
    ControlLedFeedbackConfig m_config;
    std::array<ControlFeedbackEvent, 2> m_events{};
    uint32_t m_eventRevision = 0U;
    uint32_t m_resetRevision = 0U;
    uint32_t m_appliedEventRevision = 0U;
    uint32_t m_appliedResetRevision = 0U;
    ControlFeedbackScene m_scene = ControlFeedbackScene::Suppressed;
    uint32_t m_sceneStartedMs = 0U;
    std::array<uint8_t, 2> m_lastMenuControls{};
    bool m_menuFadingOut = false;
};
} // namespace modules
