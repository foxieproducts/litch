#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/ui_types.hpp"
#include "modules/user_input.hpp"

namespace modules {

enum class ResetHoldActionType : uint8_t {
    None = 0,
    StartHold,
    CancelHold,
    TriggerReset,
};

struct ResetHoldAction {
    ResetHoldActionType type = ResetHoldActionType::None;
    UiOwner owner = UiOwner::None;
};

struct ResetHoldGestureControllerConfig {
    uint32_t holdMs = 2000;
    uint32_t chordWindowMs = 750;
};

class ResetHoldGestureController final {
  public:
    explicit ResetHoldGestureController(const ResetHoldGestureControllerConfig& config = {})
        : m_config(config) {}

    void Reset() {
        m_p1State = ChordState{};
        m_p2State = ChordState{};
    }

    bool HandleButtonEvent(const ButtonEvent& event, ResetHoldAction& outAction) {
        return HandleButtonEvent(event, outAction, fxe::Millis());
    }

    bool HandleButtonEvent(const ButtonEvent& event,
                           ResetHoldAction& outAction,
                           const uint32_t eventTimeMs) {
        outAction = ResetHoldAction{};

        if (!IsXButton(event.button) && !IsOButton(event.button)) {
            return false;
        }

        const UiOwner owner = ResolveButtonEventOwner(event);
        if (owner == UiOwner::None) {
            return false;
        }

        ChordState& state = StateForOwner(owner, m_p1State, m_p2State);
        const bool anyFaceDown = state.xDown || state.oDown;
        const bool wasChordHeld = state.xDown && state.oDown;
        UpdateChordState(event, state);
        const bool chordHeld = state.xDown && state.oDown;

        if (event.event == fxe::Button::Event::Press && !anyFaceDown) {
            state.firstPressedAtMs = eventTimeMs;
            state.chordEligible = true;
        }

        if (chordHeld && !wasChordHeld) {
            // Once a hold has fired, require both buttons to be released before
            // another chord can start. Re-pressing only the first released
            // button remains part of the same physical gesture.
            if (state.holdFired) {
                return true;
            }
            if (!state.chordEligible ||
                eventTimeMs - state.firstPressedAtMs >
                    m_config.chordWindowMs) {
                state.chordEligible = false;
                return false;
            }
            state.holdElapsed = fxe::ElapsedTime(eventTimeMs);
            outAction.type = ResetHoldActionType::StartHold;
            outAction.owner = owner;
            return true;
        }

        if (!chordHeld && wasChordHeld && state.holdElapsed.Started()) {
            state.holdElapsed = fxe::ElapsedTime();
            outAction.type = ResetHoldActionType::CancelHold;
            outAction.owner = owner;
            return true;
        }

        return state.holdElapsed.Started() && (chordHeld || wasChordHeld);
    }

    bool Tick(ResetHoldAction& outAction) {
        return Tick(outAction, fxe::Millis());
    }

    bool Tick(ResetHoldAction& outAction, const uint32_t throughMs) {
        outAction = ResetHoldAction{};

        if (TryTriggerReset(UiOwner::P1, m_p1State, outAction, throughMs)) {
            return true;
        }

        if (TryTriggerReset(UiOwner::P2, m_p2State, outAction, throughMs)) {
            return true;
        }

        return false;
    }

  private:
    struct ChordState {
        bool xDown = false;
        bool oDown = false;
        bool holdFired = false;
        bool chordEligible = false;
        uint32_t firstPressedAtMs = 0U;
        fxe::ElapsedTime holdElapsed;
    };

    static ChordState& StateForOwner(const UiOwner owner,
                                     ChordState& p1,
                                     ChordState& p2) {
        return (owner == UiOwner::P2) ? p2 : p1;
    }

    static void UpdateChordState(const ButtonEvent& event, ChordState& state) {
        if (event.button == UserButton::P1X || event.button == UserButton::P2X) {
            if (event.event == fxe::Button::Event::Press) {
                state.xDown = true;
            } else if (event.event == fxe::Button::Event::Release) {
                state.xDown = false;
            }
        }

        if (event.button == UserButton::P1O || event.button == UserButton::P2O) {
            if (event.event == fxe::Button::Event::Press) {
                state.oDown = true;
            } else if (event.event == fxe::Button::Event::Release) {
                state.oDown = false;
            }
        }

        if (!state.xDown && !state.oDown) {
            state.holdFired = false;
            state.chordEligible = false;
            state.firstPressedAtMs = 0U;
            state.holdElapsed = fxe::ElapsedTime();
        }
    }

    bool TryTriggerReset(const UiOwner owner,
                         ChordState& state,
                         ResetHoldAction& outAction,
                         const uint32_t throughMs) const {
        if (!state.xDown || !state.oDown || state.holdFired) {
            return false;
        }

        if (!state.holdElapsed.Started() ||
            throughMs - state.holdElapsed.StartMs() >= 0x80000000U ||
            throughMs - state.holdElapsed.StartMs() < m_config.holdMs) {
            return false;
        }

        state.holdFired = true;
        outAction.type = ResetHoldActionType::TriggerReset;
        outAction.owner = owner;
        return true;
    }

    ResetHoldGestureControllerConfig m_config;
    ChordState m_p1State;
    ChordState m_p2State;
};

} // namespace modules
