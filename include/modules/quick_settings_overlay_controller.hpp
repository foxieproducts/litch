#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/ui_types.hpp"
#include "modules/user_input.hpp"

namespace modules {

inline constexpr uint32_t QUICK_SETTINGS_IDLE_CLOSE_MS = 10000U;
inline constexpr uint32_t QUICK_SETTINGS_VERTICAL_GUARD_MS = 250U;

enum class QuickSettingsActionType : uint8_t {
    None = 0,
    OpenOverlay,
    UndoTap,
    CloseOverlay,
    SelectPrevious,
    SelectNext,
    AdjustIncrease,
    AdjustDecrease,
};

struct QuickSettingsAction {
    QuickSettingsActionType type = QuickSettingsActionType::None;
    UiOwner owner = UiOwner::None;
    bool isRepeat = false;
};

struct QuickSettingsOverlayControllerConfig {
    uint32_t holdMs = 1000;
};

class QuickSettingsOverlayController final {
  public:
    explicit QuickSettingsOverlayController(const QuickSettingsOverlayControllerConfig& config = {})
        : m_config(config) {}

    void Reset() {
        m_p1State = HoldState{};
        m_p2State = HoldState{};
        m_verticalInputElapsed = fxe::ElapsedTime();
    }

    bool HandleButtonEvent(const ButtonEvent& event,
                           const bool overlayActive,
                           QuickSettingsAction& outAction) {
        return HandleButtonEvent(event, overlayActive, outAction, fxe::Millis());
    }

    bool HandleButtonEvent(const ButtonEvent& event,
                           const bool overlayActive,
                           const bool undoTapOnPress,
                           QuickSettingsAction& outAction,
                           const uint32_t eventTimeMs) {
        return HandleButtonEvent(event,
                                 overlayActive,
                                 undoTapOnPress,
                                 true,
                                 outAction,
                                 eventTimeMs);
    }

    bool HandleButtonEvent(const ButtonEvent& event,
                           const bool overlayActive,
                           QuickSettingsAction& outAction,
                           const uint32_t eventTimeMs) {
        return HandleButtonEvent(
            event, overlayActive, false, true, outAction, eventTimeMs);
    }

    bool HandleButtonEvent(const ButtonEvent& event,
                           const bool overlayActive,
                           const bool undoTapOnPress,
                           const bool directOpenAllowed,
                           QuickSettingsAction& outAction,
                           const uint32_t eventTimeMs) {
        outAction = QuickSettingsAction{};

        if (overlayActive) {
            return HandleOverlayEvent(event, outAction, eventTimeMs);
        }

        m_verticalInputElapsed = fxe::ElapsedTime();
        return HandleOpenButtonEvent(event,
                                     undoTapOnPress,
                                     directOpenAllowed,
                                     outAction,
                                     eventTimeMs);
    }

    bool Tick(QuickSettingsAction& outAction) {
        return Tick(outAction, true);
    }

    bool Tick(QuickSettingsAction& outAction,
              const bool directOpenAllowed) {
        return Tick(outAction, directOpenAllowed, fxe::Millis());
    }

    bool Tick(QuickSettingsAction& outAction, const bool directOpenAllowed,
              const uint32_t throughMs) {
        outAction = QuickSettingsAction{};

        if (TryOpenFromHold(
                UiOwner::P1, m_p1State, directOpenAllowed, outAction, throughMs)) {
            return true;
        }

        if (TryOpenFromHold(
                UiOwner::P2, m_p2State, directOpenAllowed, outAction, throughMs)) {
            return true;
        }

        return false;
    }

    // Exposes only the semantic progress needed by the status renderer. This
    // gives a held X immediate, proportional physical feedback without
    // leaking the controller's timing state or requiring the companion app.
    bool TryGetOpenHoldProgress(UiOwner& outOwner,
                                uint8_t& outProgress01_255) const {
        if (TryGetOpenHoldProgressForOwner(
                UiOwner::P1, m_p1State, outOwner, outProgress01_255)) {
            return true;
        }

        return TryGetOpenHoldProgressForOwner(
            UiOwner::P2, m_p2State, outOwner, outProgress01_255);
    }

    bool TryGetOpenHoldProgressFor(const UiOwner owner, uint8_t& progress) const {
        if (owner == UiOwner::None) return false;
        UiOwner ignored = UiOwner::None;
        return TryGetOpenHoldProgressForOwner(owner,
            owner == UiOwner::P2 ? m_p2State : m_p1State, ignored, progress);
    }
    uint32_t HoldThresholdMs() const { return m_config.holdMs; }

    // Presentation also borrows a press buffered by mode-chord arbitration.
    // Both previews use the controller's configured threshold and original edge.
    uint8_t GetOpenHoldProgress01_255(const uint32_t pressedAtMs) const {
        const uint32_t elapsed = fxe::Millis() - pressedAtMs;
        if (m_config.holdMs == 0U || elapsed >= m_config.holdMs) return 255U;
        return static_cast<uint8_t>(
            (static_cast<uint64_t>(elapsed) * 255ULL) / m_config.holdMs);
    }

  private:
    struct HoldState {
        bool down = false;
        bool holdFired = false;
        bool holdOpenAllowed = true;
        fxe::ElapsedTime holdElapsed;
    };

    static bool IsPressLike(const fxe::Button::Event event) {
        return event == fxe::Button::Event::Press || event == fxe::Button::Event::Repeat;
    }

    static HoldState& StateForOwner(const UiOwner owner,
                                    HoldState& p1,
                                    HoldState& p2) {
        return (owner == UiOwner::P2) ? p2 : p1;
    }

    bool TryGetOpenHoldProgressForOwner(
        const UiOwner owner,
        const HoldState& state,
        UiOwner& outOwner,
        uint8_t& outProgress01_255) const {
        if (!state.down || state.holdFired || !state.holdOpenAllowed ||
            !state.holdElapsed.Started()) {
            return false;
        }

        outOwner = owner;
        outProgress01_255 = GetOpenHoldProgress01_255(state.holdElapsed.StartMs());
        return true;
    }

    bool HandleOverlayEvent(const ButtonEvent& event,
                            QuickSettingsAction& outAction,
                            const uint32_t eventTimeMs) {
        outAction.owner = event.owner;
        outAction.isRepeat = (event.event == fxe::Button::Event::Repeat);
        if (!IsPressLike(event.event)) {
            return true;
        }

        if (IsXButton(event.button)) {
            m_verticalInputElapsed = fxe::ElapsedTime();
            outAction.type = QuickSettingsActionType::CloseOverlay;
            return true;
        }

        if (event.button == UserButton::Left || event.button == UserButton::Right) {
            // Side repeats are always discarded, including those following a
            // press ignored while the user was adjusting the current value.
            if (event.event == fxe::Button::Event::Press &&
                (!m_verticalInputElapsed.Started() ||
                 eventTimeMs - m_verticalInputElapsed.StartMs() >=
                     QUICK_SETTINGS_VERTICAL_GUARD_MS)) {
                outAction.type = event.button == UserButton::Left
                    ? QuickSettingsActionType::SelectPrevious
                    : QuickSettingsActionType::SelectNext;
            }
            return true;
        }

        if (event.button == UserButton::Up) {
            m_verticalInputElapsed = fxe::ElapsedTime(eventTimeMs);
            outAction.type = QuickSettingsActionType::AdjustIncrease;
            return true;
        }

        if (event.button == UserButton::Down) {
            m_verticalInputElapsed = fxe::ElapsedTime(eventTimeMs);
            outAction.type = QuickSettingsActionType::AdjustDecrease;
            return true;
        }

        return true;
    }

    bool HandleOpenButtonEvent(const ButtonEvent& event,
                               const bool undoTapOnPress,
                               const bool directOpenAllowed,
                               QuickSettingsAction& outAction,
                               const uint32_t eventTimeMs) {
        if (!IsXButton(event.button)) {
            return false;
        }

        const UiOwner owner = ResolveButtonEventOwner(event);
        if (owner == UiOwner::None) {
            return false;
        }

        HoldState& state = StateForOwner(owner, m_p1State, m_p2State);
        if (event.event == fxe::Button::Event::Press) {
            if (undoTapOnPress) {
                state = HoldState{};
                outAction.type = QuickSettingsActionType::UndoTap;
                outAction.owner = owner;
                return true;
            }
            state.down = true;
            state.holdFired = false;
            state.holdOpenAllowed = directOpenAllowed;
            state.holdElapsed = fxe::ElapsedTime(eventTimeMs);
            return true;
        }

        if (!state.down) {
            return true;
        }

        if (event.event == fxe::Button::Event::LongHold && !state.holdFired) {
            state.holdFired = true;
            if (state.holdOpenAllowed && directOpenAllowed) {
                outAction.type = QuickSettingsActionType::OpenOverlay;
                outAction.owner = owner;
            }
            return true;
        }

        if (event.event == fxe::Button::Event::Release) {
            if (!state.holdFired) {
                outAction.type = QuickSettingsActionType::UndoTap;
                outAction.owner = owner;
            }
            state.down = false;
            state.holdFired = false;
            state.holdElapsed = fxe::ElapsedTime();
            return true;
        }

        return true;
    }

    bool TryOpenFromHold(const UiOwner owner,
                         HoldState& state,
                         const bool directOpenAllowed,
                         QuickSettingsAction& outAction,
                         const uint32_t throughMs) const {
        if (!state.down || state.holdFired) {
            return false;
        }

        if (!state.holdElapsed.Started() ||
            throughMs - state.holdElapsed.StartMs() >= 0x80000000U ||
            throughMs - state.holdElapsed.StartMs() < m_config.holdMs) {
            return false;
        }

        state.holdFired = true;
        if (!state.holdOpenAllowed || !directOpenAllowed) {
            return false;
        }
        outAction.type = QuickSettingsActionType::OpenOverlay;
        outAction.owner = owner;
        outAction.isRepeat = false;
        return true;
    }

    QuickSettingsOverlayControllerConfig m_config;

    HoldState m_p1State;
    HoldState m_p2State;

    fxe::ElapsedTime m_verticalInputElapsed;
};

} // namespace modules
