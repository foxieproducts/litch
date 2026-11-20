#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/ui_types.hpp"
#include "modules/user_input.hpp"

namespace modules {

// The established board tap-on-release / one-shot hold recognizer.
// Each caller owns one control role; player states remain independent.

enum class TapHoldActionType : uint8_t {
    None = 0,
    Tap,
    Hold,
};

struct TapHoldAction {
    TapHoldActionType type = TapHoldActionType::None;
    UiOwner owner = UiOwner::None;
};

struct TapHoldGestureControllerConfig {
    uint32_t holdMs = 1000;
};

class TapHoldGestureController final {
  public:
    explicit TapHoldGestureController(const TapHoldGestureControllerConfig& config = {})
        : m_config(config) {}

    void Reset() {
        m_p1State = HoldState{};
        m_p2State = HoldState{};
    }

    void Cancel(const UiOwner owner) {
        if (owner != UiOwner::P1 && owner != UiOwner::P2) return;
        HoldState& state = StateForOwner(owner, m_p1State, m_p2State);
        state = HoldState{};
        // The unused false/true state is a cancellation barrier. A chord
        // arbiter may replay an older Press after a CPU role has changed.
        state.holdFired = true;
        state.holdElapsed = fxe::ElapsedTime(fxe::Millis());
    }

    bool HandleButtonEvent(const ButtonEvent& event, TapHoldAction& outAction) {
        return HandleButtonEvent(event, outAction, fxe::Millis());
    }

    bool HandleButtonEvent(const ButtonEvent& event,
                           TapHoldAction& outAction,
                           const uint32_t eventTimeMs) {
        outAction = TapHoldAction{};

        const UiOwner owner = ResolveButtonEventOwner(event);
        if (owner == UiOwner::None) {
            return false;
        }

        HoldState& state = StateForOwner(owner, m_p1State, m_p2State);
        if (!state.down && state.holdFired) {
            const uint32_t afterCancellation = eventTimeMs - state.holdElapsed.StartMs();
            const bool freshPress = event.event == fxe::Button::Event::Press &&
                afterCancellation != 0U && afterCancellation < 0x80000000U;
            if (!freshPress) {
                if (event.event == fxe::Button::Event::Release) state = HoldState{};
                return true;
            }
            state = HoldState{};
        }
        if (event.event == fxe::Button::Event::Press) {
            state.down = true;
            state.holdFired = false;
            state.holdElapsed = fxe::ElapsedTime(eventTimeMs);
            return true;
        }

        if (!state.down) {
            return true;
        }

        if (event.event == fxe::Button::Event::LongHold && !state.holdFired) {
            state.holdFired = true;
            outAction.type = TapHoldActionType::Hold;
            outAction.owner = owner;
            return true;
        }

        if (event.event == fxe::Button::Event::Release) {
            if (!state.holdFired) {
                outAction.type = TapHoldActionType::Tap;
                outAction.owner = owner;
            }
            state.down = false;
            state.holdFired = false;
            state.holdElapsed = fxe::ElapsedTime();
            return true;
        }

        return true;
    }

    bool Tick(TapHoldAction& outAction) {
        return Tick(outAction, fxe::Millis());
    }

    bool Tick(TapHoldAction& outAction, const uint32_t throughMs) {
        outAction = TapHoldAction{};

        if (TryHold(UiOwner::P1, m_p1State, outAction, throughMs)) {
            return true;
        }

        if (TryHold(UiOwner::P2, m_p2State, outAction, throughMs)) {
            return true;
        }

        return false;
    }

    bool IsHeld(const UiOwner owner) const {
        switch (owner) {
            case UiOwner::P1:
                return m_p1State.down;
            case UiOwner::P2:
                return m_p2State.down;
            default:
                return false;
        }
    }

    uint32_t HeldMs(const UiOwner owner) const {
        if (owner == UiOwner::None) return 0U;
        const auto& state = owner == UiOwner::P2 ? m_p2State : m_p1State;
        return state.down && !state.holdFired && state.holdElapsed.Started()
            ? state.holdElapsed.ElapsedMs() : 0U;
    }
    uint32_t HoldThresholdMs() const { return m_config.holdMs; }

  private:
    struct HoldState {
        bool down = false;
        bool holdFired = false;
        fxe::ElapsedTime holdElapsed;
    };

    static HoldState& StateForOwner(const UiOwner owner,
                                    HoldState& p1,
                                    HoldState& p2) {
        return (owner == UiOwner::P2) ? p2 : p1;
    }

    bool TryHold(const UiOwner owner,
                 HoldState& state,
                 TapHoldAction& outAction,
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
        outAction.type = TapHoldActionType::Hold;
        outAction.owner = owner;
        return true;
    }

    TapHoldGestureControllerConfig m_config;
    HoldState m_p1State;
    HoldState m_p2State;
};

} // namespace modules
