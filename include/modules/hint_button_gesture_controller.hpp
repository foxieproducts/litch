#pragma once

#include "modules/tap_hold_gesture_controller.hpp"

namespace modules {
enum class HintButtonActionType : uint8_t { None = 0, ToggleHint, BeginnerModeHold };
struct HintButtonAction {
    HintButtonActionType type = HintButtonActionType::None;
    UiOwner owner = UiOwner::None;
};
using HintButtonGestureControllerConfig = TapHoldGestureControllerConfig;

class HintButtonGestureController final {
  public:
    explicit HintButtonGestureController(const HintButtonGestureControllerConfig& config = {})
        : m_gesture(config) {}
    void Reset() { m_gesture.Reset(); }
    void Cancel(const UiOwner owner) { m_gesture.Cancel(owner); }
    bool HandleButtonEvent(const ButtonEvent& event, HintButtonAction& action) {
        return HandleButtonEvent(event, action, fxe::Millis());
    }
    bool HandleButtonEvent(const ButtonEvent& event, HintButtonAction& action,
                           uint32_t eventTimeMs) {
        action = {};
        if (!IsOButton(event.button)) return false;
        TapHoldAction result;
        const bool consumed = m_gesture.HandleButtonEvent(event, result, eventTimeMs);
        action = Convert(result);
        return consumed;
    }
    bool Tick(HintButtonAction& action) {
        return Tick(action, fxe::Millis());
    }
    bool Tick(HintButtonAction& action, const uint32_t throughMs) {
        TapHoldAction result;
        const bool emitted = m_gesture.Tick(result, throughMs);
        action = Convert(result);
        return emitted;
    }
    bool IsHeld(UiOwner owner) const { return m_gesture.IsHeld(owner); }
    uint32_t HeldMs(UiOwner owner) const { return m_gesture.HeldMs(owner); }
    uint32_t HoldThresholdMs() const { return m_gesture.HoldThresholdMs(); }
  private:
    static HintButtonAction Convert(const TapHoldAction& action) {
        return {action.type == TapHoldActionType::Tap ? HintButtonActionType::ToggleHint :
                action.type == TapHoldActionType::Hold ? HintButtonActionType::BeginnerModeHold :
                HintButtonActionType::None, action.owner};
    }
    TapHoldGestureController m_gesture;
};
} // namespace modules
