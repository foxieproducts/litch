#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/quick_settings_orchestration.hpp"
#include "modules/hint_button_gesture_controller.hpp"
#include "modules/quick_settings_overlay_controller.hpp"
#include "modules/reset_hold_gesture_controller.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

modules::ButtonEvent MakeEvent(const modules::UserButton button,
                               const fxe::Button::Event event,
                               const modules::UiOwner owner) {
    modules::ButtonEvent e;
    e.button = button;
    e.event = event;
    e.owner = owner;
    return e;
}

struct ControlObservations {
    uint8_t resetStarts = 0U;
    uint8_t resetCancels = 0U;
    uint8_t resetTriggers = 0U;
    uint8_t quickSetupOpens = 0U;
    uint8_t undoTaps = 0U;
    uint8_t hintTaps = 0U;
    uint8_t coachingHolds = 0U;
    modules::UiOwner lastQuickSetupOwner = modules::UiOwner::None;
};

// Test-local observation seam that composes the three gesture controllers in
// the same priority order as firmware button routing. It intentionally records
// semantic effects rather than changing the public VirtualLitch ABI.
class ControlHarness final {
  public:
    ControlHarness()
        : m_quickController(
              modules::QuickSettingsOverlayControllerConfig{1000U}),
          m_resetController(
              modules::ResetHoldGestureControllerConfig{2000U}),
          m_hintController(
              modules::HintButtonGestureControllerConfig{1000U}) {}

    void SetUndoTapOnPress(const bool enabled) {
        m_undoTapOnPress = enabled;
    }

    void SetDirectOpenAllowed(const bool allowed) {
        m_directOpenAllowed = allowed;
    }

    void Feed(const modules::ButtonEvent& event, const uint32_t eventTimeMs) {
        fxe::ManualMillis::SetNowMs(eventTimeMs);

        modules::ResetHoldAction resetAction;
        const bool resetConsumed = m_resetController.HandleButtonEvent(
            event, resetAction, eventTimeMs);
        if (resetAction.type == modules::ResetHoldActionType::StartHold) {
            ++observed.resetStarts;
            m_resetActive = true;
            m_hintController.Reset();
            m_quickController.Reset();
            return;
        }
        if (resetAction.type == modules::ResetHoldActionType::CancelHold) {
            ++observed.resetCancels;
            m_resetActive = false;
            m_hintController.Reset();
            m_quickController.Reset();
            return;
        }
        if (resetConsumed) {
            return;
        }

        modules::HintButtonAction hintAction;
        if (m_hintController.HandleButtonEvent(
                event, hintAction, eventTimeMs)) {
            ObserveHintAction(hintAction);
            return;
        }

        modules::QuickSettingsAction quickAction;
        if (m_quickController.HandleButtonEvent(
                event,
                false,
                m_undoTapOnPress,
                m_directOpenAllowed,
                quickAction,
                eventTimeMs)) {
            ObserveQuickAction(quickAction);
        }
    }

    void Tick(const uint32_t nowMs) {
        fxe::ManualMillis::SetNowMs(nowMs);

        modules::ResetHoldAction resetAction;
        while (m_resetController.Tick(resetAction)) {
            if (resetAction.type ==
                modules::ResetHoldActionType::TriggerReset) {
                ++observed.resetTriggers;
            }
        }
        if (m_resetActive) {
            return;
        }

        modules::HintButtonAction hintAction;
        while (m_hintController.Tick(hintAction)) {
            ObserveHintAction(hintAction);
        }

        modules::QuickSettingsAction quickAction;
        while (m_quickController.Tick(
            quickAction, m_directOpenAllowed)) {
            ObserveQuickAction(quickAction);
        }
    }

    ControlObservations observed;

  private:
    void ObserveQuickAction(const modules::QuickSettingsAction& action) {
        if (action.type == modules::QuickSettingsActionType::OpenOverlay) {
            ++observed.quickSetupOpens;
            observed.lastQuickSetupOwner = action.owner;
        } else if (action.type ==
                   modules::QuickSettingsActionType::UndoTap) {
            ++observed.undoTaps;
        }
    }

    void ObserveHintAction(const modules::HintButtonAction& action) {
        if (action.type == modules::HintButtonActionType::ToggleHint) {
            ++observed.hintTaps;
        } else if (action.type ==
                   modules::HintButtonActionType::BeginnerModeHold) {
            ++observed.coachingHolds;
        }
    }

    modules::QuickSettingsOverlayController m_quickController;
    modules::ResetHoldGestureController m_resetController;
    modules::HintButtonGestureController m_hintController;
    bool m_undoTapOnPress = false;
    bool m_directOpenAllowed = true;
    bool m_resetActive = false;
};

enum class FacePressOrder : uint8_t {
    XThenO,
    OThenX,
};

enum class FaceReleaseOrder : uint8_t {
    XThenO,
    OThenX,
};

void CheckChordPath(const FacePressOrder pressOrder,
                    const FaceReleaseOrder releaseOrder,
                    const bool reachHoldThreshold) {
    ControlHarness harness;
    const modules::ButtonEvent xPress =
        MakeEvent(modules::UserButton::P1X,
                  fxe::Button::Event::Press,
                  modules::UiOwner::P1);
    const modules::ButtonEvent oPress =
        MakeEvent(modules::UserButton::P1O,
                  fxe::Button::Event::Press,
                  modules::UiOwner::P1);
    const modules::ButtonEvent xRelease =
        MakeEvent(modules::UserButton::P1X,
                  fxe::Button::Event::Release,
                  modules::UiOwner::P1);
    const modules::ButtonEvent oRelease =
        MakeEvent(modules::UserButton::P1O,
                  fxe::Button::Event::Release,
                  modules::UiOwner::P1);

    harness.Feed(pressOrder == FacePressOrder::XThenO ? xPress : oPress,
                 100U);
    harness.Feed(pressOrder == FacePressOrder::XThenO ? oPress : xPress,
                 200U);
    Check(harness.observed.resetStarts == 1U,
          "each X+O press ordering starts one reset hold");
    harness.Feed(MakeEvent(modules::UserButton::P1X,
                           fxe::Button::Event::Repeat,
                           modules::UiOwner::P1),
                 201U);
    harness.Feed(MakeEvent(modules::UserButton::P1O,
                           fxe::Button::Event::Repeat,
                           modules::UiOwner::P1),
                 202U);
    Check(harness.observed.resetStarts == 1U,
          "face-button repeats cannot restart an active chord");

    harness.Tick(2199U);
    Check(harness.observed.resetTriggers == 0U,
          "X+O never resets before the hold boundary");
    if (reachHoldThreshold) {
        harness.Tick(2200U);
        harness.Tick(2300U);
        Check(harness.observed.resetTriggers == 1U,
              "held X+O resets exactly once at and after the boundary");
    }

    const uint32_t releaseAtMs = reachHoldThreshold ? 2400U : 2199U;
    harness.Feed(releaseOrder == FaceReleaseOrder::XThenO
                     ? xRelease
                     : oRelease,
                 releaseAtMs);
    harness.Feed(releaseOrder == FaceReleaseOrder::XThenO
                     ? oRelease
                     : xRelease,
                 releaseAtMs + 1U);
    harness.Tick(5000U);

    Check(harness.observed.resetCancels == 1U,
          "each X+O release ordering ends one reset hold");
    Check(harness.observed.resetTriggers ==
              (reachHoldThreshold ? 1U : 0U),
          "release cannot add or repeat a reset");
    Check(harness.observed.undoTaps == 0U &&
              harness.observed.hintTaps == 0U &&
              harness.observed.coachingHolds == 0U &&
              harness.observed.quickSetupOpens == 0U,
          "X+O press and release paths emit no stray face-button action");

    // Both releases must leave the recognizers reusable rather than carrying a
    // suppressed or half-held sequence into the next gesture.
    harness.Feed(xPress, 6000U);
    harness.Feed(xRelease, 6050U);
    harness.Feed(oPress, 6100U);
    harness.Feed(oRelease, 6150U);
    Check(harness.observed.undoTaps == 1U,
          "first solo X tap after a chord emits one Undo");
    Check(harness.observed.hintTaps == 1U,
          "first solo O tap after a chord emits one Hint");
}

void CheckEveryChordOrdering() {
    constexpr FacePressOrder pressOrders[] = {
        FacePressOrder::XThenO,
        FacePressOrder::OThenX,
    };
    constexpr FaceReleaseOrder releaseOrders[] = {
        FaceReleaseOrder::XThenO,
        FaceReleaseOrder::OThenX,
    };
    for (const FacePressOrder pressOrder : pressOrders) {
        for (const FaceReleaseOrder releaseOrder : releaseOrders) {
            CheckChordPath(pressOrder, releaseOrder, false);
            CheckChordPath(pressOrder, releaseOrder, true);
        }
    }
}

void CheckImmediateSidePressAndVerticalGuard() {
    using modules::UserButton;
    using modules::UiOwner;
    using modules::QuickSettingsActionType;
    using Event = fxe::Button::Event;
    modules::QuickSettingsOverlayController controller;
    const auto feed = [&](const UserButton button, const Event event,
                          const uint32_t time, const UiOwner owner = UiOwner::P1) {
        modules::QuickSettingsAction action;
        Check(controller.HandleButtonEvent(MakeEvent(button, event, owner), true, action, time),
              "active Quick Setup consumes every navigation edge");
        return action.type;
    };
    Check(feed(UserButton::Right, Event::Press, 100U) == QuickSettingsActionType::SelectNext &&
              feed(UserButton::Right, Event::Repeat, 349U) == QuickSettingsActionType::None &&
              feed(UserButton::Right, Event::Repeat, 350U) == QuickSettingsActionType::None &&
              feed(UserButton::Right, Event::Repeat, 900U) == QuickSettingsActionType::None,
          "side Press selects immediately and held side Repeat never selects");
    feed(UserButton::Right, Event::Release, 901U);
    Check(feed(UserButton::Right, Event::Repeat, 1000U) == QuickSettingsActionType::None,
          "release cannot leave a replayable side selection");
    Check(feed(UserButton::Left, Event::Press, 1100U) == QuickSettingsActionType::SelectPrevious,
          "fresh opposite side Press selects immediately");
    Check(feed(UserButton::Up, Event::Press, 2000U) == QuickSettingsActionType::AdjustIncrease &&
              feed(UserButton::Right, Event::Press, 2000U) == QuickSettingsActionType::None,
          "accepted vertical Press blocks the same-timestamp diagonal side Press");
    feed(UserButton::Up, Event::Release, 2020U);
    Check(feed(UserButton::Left, Event::Press, 2239U) == QuickSettingsActionType::None &&
              feed(UserButton::Right, Event::Press, 2250U) == QuickSettingsActionType::SelectNext &&
              feed(UserButton::Left, Event::Repeat, 2300U) == QuickSettingsActionType::None,
          "vertical guard rejects 239ms, accepts a fresh Press at 250ms, and never replays an ignored side");
    Check(feed(UserButton::Down, Event::Press, 3000U) == QuickSettingsActionType::AdjustDecrease &&
              feed(UserButton::Down, Event::Repeat, 3250U) == QuickSettingsActionType::AdjustDecrease &&
              feed(UserButton::Right, Event::Press, 3489U) == QuickSettingsActionType::None &&
              feed(UserButton::Down, Event::Repeat, 3490U) == QuickSettingsActionType::AdjustDecrease &&
              feed(UserButton::Left, Event::Press, 3500U) == QuickSettingsActionType::None &&
              feed(UserButton::Right, Event::Press, 3740U) == QuickSettingsActionType::SelectNext,
          "every vertical Repeat adjusts and restarts the 250ms side guard");
    feed(UserButton::Up, Event::Press, 4000U, UiOwner::P2);
    Check(feed(UserButton::Right, Event::Press, 4239U, UiOwner::P1) == QuickSettingsActionType::None &&
              feed(UserButton::Right, Event::Press, 4250U, UiOwner::P1) == QuickSettingsActionType::SelectNext,
          "either control-bank vertical input guards the shared selected item");
    feed(UserButton::Up, Event::Press, 5000U);
    controller.Reset();
    Check(feed(UserButton::Right, Event::Repeat, 5001U) == QuickSettingsActionType::None &&
              feed(UserButton::Right, Event::Press, 5002U) == QuickSettingsActionType::SelectNext,
          "close/reopen or orientation Reset cancels guard and rejects stale Repeat");
    controller.Reset();
    feed(UserButton::Down, Event::Press, UINT32_MAX - 100U);
    Check(feed(UserButton::Left, Event::Press, 138U) == QuickSettingsActionType::None &&
              feed(UserButton::Left, Event::Press, 149U) == QuickSettingsActionType::SelectPrevious,
          "239ms rejection and 250ms acceptance survive millis rollover");
    controller.Reset();
    fxe::ManualMillis::SetNowMs(9000U);
    modules::QuickSettingsAction action;
    controller.HandleButtonEvent(MakeEvent(UserButton::Up, Event::Press, UiOwner::P1), true, action, 6000U);
    controller.HandleButtonEvent(MakeEvent(UserButton::Right, Event::Press, UiOwner::P1), true, action, 6239U);
    Check(action.type == QuickSettingsActionType::None,
          "queued overlay events use capture timestamps instead of current dispatch time");
    controller.HandleButtonEvent(MakeEvent(UserButton::Right, Event::Press, UiOwner::P1), true, action, 6250U);
    Check(action.type == QuickSettingsActionType::SelectNext,
          "queued side Press becomes eligible exactly 250ms after captured vertical input");
}

void CheckResetRequiresFullReleaseBeforeRetrigger() {
    modules::ResetHoldGestureController controller(
        modules::ResetHoldGestureControllerConfig{2000U});
    modules::ResetHoldAction action;
    const modules::ButtonEvent xPress =
        MakeEvent(modules::UserButton::P1X,
                  fxe::Button::Event::Press,
                  modules::UiOwner::P1);
    const modules::ButtonEvent oPress =
        MakeEvent(modules::UserButton::P1O,
                  fxe::Button::Event::Press,
                  modules::UiOwner::P1);
    const modules::ButtonEvent xRelease =
        MakeEvent(modules::UserButton::P1X,
                  fxe::Button::Event::Release,
                  modules::UiOwner::P1);
    const modules::ButtonEvent oRelease =
        MakeEvent(modules::UserButton::P1O,
                  fxe::Button::Event::Release,
                  modules::UiOwner::P1);

    Check(!controller.HandleButtonEvent(xPress, action, 100U),
          "first face press does not form a reset chord");
    Check(controller.HandleButtonEvent(oPress, action, 200U) &&
              action.type == modules::ResetHoldActionType::StartHold,
          "second face press starts the reset chord");
    fxe::ManualMillis::SetNowMs(2200U);
    Check(controller.Tick(action) &&
              action.type == modules::ResetHoldActionType::TriggerReset,
          "completed reset chord fires once");

    Check(controller.HandleButtonEvent(xRelease, action, 2300U) &&
              action.type == modules::ResetHoldActionType::CancelHold,
          "partial release ends the completed hold");
    Check(controller.HandleButtonEvent(xPress, action, 2301U) &&
              action.type == modules::ResetHoldActionType::None,
          "re-press before full release cannot restart reset hold");
    fxe::ManualMillis::SetNowMs(5000U);
    Check(!controller.Tick(action),
          "partial release and re-press cannot trigger a second reset");

    (void)controller.HandleButtonEvent(oRelease, action, 5001U);
    (void)controller.HandleButtonEvent(xRelease, action, 5002U);
    Check(!controller.HandleButtonEvent(xPress, action, 6000U),
          "controller accepts a fresh gesture after complete release");
    Check(controller.HandleButtonEvent(oPress, action, 6001U) &&
              action.type == modules::ResetHoldActionType::StartHold,
          "complete release rearms a later reset chord");
}

void CheckResetChordUsesBoundedHumanStagger() {
    modules::ResetHoldGestureController controller(
        modules::ResetHoldGestureControllerConfig{2000U, 750U});
    modules::ResetHoldAction action;
    const auto xPress = MakeEvent(modules::UserButton::P1X,
                                  fxe::Button::Event::Press,
                                  modules::UiOwner::P1);
    const auto oPress = MakeEvent(modules::UserButton::P1O,
                                  fxe::Button::Event::Press,
                                  modules::UiOwner::P1);
    const auto xRelease = MakeEvent(modules::UserButton::P1X,
                                    fxe::Button::Event::Release,
                                    modules::UiOwner::P1);
    const auto oRelease = MakeEvent(modules::UserButton::P1O,
                                    fxe::Button::Event::Release,
                                    modules::UiOwner::P1);

    Check(!controller.HandleButtonEvent(xPress, action, 100U) &&
              controller.HandleButtonEvent(oPress, action, 850U) &&
              action.type == modules::ResetHoldActionType::StartHold,
          "same-owner face buttons at the inclusive 750ms boundary form New Game");
    (void)controller.HandleButtonEvent(xRelease, action, 851U);
    (void)controller.HandleButtonEvent(oRelease, action, 852U);

    Check(!controller.HandleButtonEvent(xPress, action, 1000U) &&
              !controller.HandleButtonEvent(oPress, action, 1751U) &&
              action.type == modules::ResetHoldActionType::None,
          "same-owner face buttons beyond 750ms do not form New Game");
    fxe::ManualMillis::SetNowMs(5000U);
    Check(!controller.Tick(action),
          "an over-window pair cannot accumulate into a delayed New Game");
    (void)controller.HandleButtonEvent(xRelease, action, 5001U);
    (void)controller.HandleButtonEvent(oRelease, action, 5002U);
}

void CheckDirectEntryContexts() {
    modules::QuickSettingsDirectEntryContext context;
    Check(modules::IsQuickSettingsDirectEntryAllowed(context),
          "idle context allows X-hold entry to Quick Setup");

    const auto CheckBlocked = [](const modules::QuickSettingsDirectEntryContext& blocked,
                                 const char* message) {
        Check(!modules::IsQuickSettingsDirectEntryAllowed(blocked), message);
    };

    context = {};
    context.wakeInputSuppressed = true;
    CheckBlocked(context, "wake guard blocks X-hold Quick Setup entry");
    context = {};
    context.hardwareTestActive = true;
    CheckBlocked(context, "hardware test owns controls before Quick Setup");
    context = {};
    context.ampTestActive = true;
    CheckBlocked(context, "amp test owns controls before Quick Setup");
    context = {};
    context.resetHoldActive = true;
    CheckBlocked(context, "reset hold blocks X-hold Quick Setup entry");
    context = {};
    context.menuOverlayActive = true;
    CheckBlocked(context, "menu overlay blocks X-hold Quick Setup entry");
    context = {};
    context.quickSettingsOverlayActive = true;
    CheckBlocked(context, "active Quick Setup blocks duplicate X-hold entry");
    context = {};
    context.textOverlayActive = true;
    CheckBlocked(context, "text overlay blocks X-hold Quick Setup entry");
    context = {};
    context.placementActive = true;
    Check(modules::IsQuickSettingsDirectEntryAllowed(context),
          "ordinary placement permits brightness access before pieces are placed");
    context = {};
    context.kingQueenSwapActive = true;
    CheckBlocked(context, "king-queen swap blocks X-hold Quick Setup entry");
    context = {};
    context.undoPromptActive = true;
    CheckBlocked(context, "undo prompt blocks X-hold Quick Setup entry");
    context = {};
    context.promotionActive = true;
    CheckBlocked(context, "promotion blocks X-hold Quick Setup entry");
}

void CheckUnusedCenterAndFaceFallbacks() {
    const modules::ButtonEvent p1Center =
        MakeEvent(modules::UserButton::Press,
                  fxe::Button::Event::Press,
                  modules::UiOwner::P1);
    const modules::ButtonEvent p2Center =
        MakeEvent(modules::UserButton::Press,
                  fxe::Button::Event::Press,
                  modules::UiOwner::P2);

    ControlHarness p1;
    p1.Feed(p1Center, 100U);
    p1.Feed(MakeEvent(modules::UserButton::Press,
                      fxe::Button::Event::Repeat,
                      modules::UiOwner::P1),
            101U);
    p1.Feed(MakeEvent(modules::UserButton::Press,
                      fxe::Button::Event::Release,
                      modules::UiOwner::P1),
            102U);
    Check(p1.observed.quickSetupOpens == 0U,
          "unused P1 center cannot open Quick Setup");

    ControlHarness p2;
    p2.Feed(p2Center, 200U);
    Check(p2.observed.quickSetupOpens == 0U,
          "unused P2 center cannot open Quick Setup");

    ControlHarness blocked;
    modules::QuickSettingsDirectEntryContext wakeContext;
    wakeContext.wakeInputSuppressed = true;
    blocked.SetDirectOpenAllowed(
        modules::IsQuickSettingsDirectEntryAllowed(wakeContext));
    blocked.Feed(p1Center, 300U);
    Check(blocked.observed.quickSetupOpens == 0U,
          "wake guard prevents direct center entry");

    const modules::ButtonEvent xPress =
        MakeEvent(modules::UserButton::P1X,
                  fxe::Button::Event::Press,
                  modules::UiOwner::P1);
    const modules::ButtonEvent xRelease =
        MakeEvent(modules::UserButton::P1X,
                  fxe::Button::Event::Release,
                  modules::UiOwner::P1);
    blocked.SetDirectOpenAllowed(true);
    blocked.Feed(xPress, 400U);
    blocked.Tick(1400U);
    Check(blocked.observed.quickSetupOpens == 1U,
          "X fallback remains available after the separate wake gate clears");

    ControlHarness promotionBlocked;
    promotionBlocked.SetDirectOpenAllowed(false);
    promotionBlocked.Feed(xPress, 500U);
    promotionBlocked.Tick(1600U);
    promotionBlocked.Feed(xRelease, 1700U);
    Check(promotionBlocked.observed.quickSetupOpens == 0U &&
              promotionBlocked.observed.undoTaps == 0U,
          "promotion ownership blocks X hold without swallowing its events");

    ControlHarness promotionCancel;
    promotionCancel.SetDirectOpenAllowed(false);
    promotionCancel.Feed(xPress, 2000U);
    promotionCancel.Feed(xRelease, 2050U);
    Check(promotionCancel.observed.undoTaps == 1U &&
              promotionCancel.observed.quickSetupOpens == 0U,
          "promotion ownership preserves the short X cancel path");

    ControlHarness xFallback;
    xFallback.Feed(xPress, 1000U);
    xFallback.Tick(1999U);
    Check(xFallback.observed.quickSetupOpens == 0U,
          "X fallback waits for its hold boundary");
    xFallback.Tick(2000U);
    xFallback.Tick(2100U);
    xFallback.Feed(xRelease, 2200U);
    Check(xFallback.observed.quickSetupOpens == 1U &&
              xFallback.observed.undoTaps == 0U,
          "X fallback opens once and release does not also Undo");

    ControlHarness xTap;
    xTap.Feed(xPress, 3000U);
    xTap.Feed(xRelease, 3050U);
    Check(xTap.observed.undoTaps == 1U &&
              xTap.observed.quickSetupOpens == 0U,
          "short X remains one Undo tap");

    ControlHarness cancelUndo;
    cancelUndo.SetUndoTapOnPress(true);
    cancelUndo.Feed(xPress, 4000U);
    cancelUndo.Feed(xRelease, 4050U);
    cancelUndo.Tick(6000U);
    Check(cancelUndo.observed.undoTaps == 1U &&
              cancelUndo.observed.quickSetupOpens == 0U,
          "active undo cancels once on X press and never becomes a hold");

    const modules::ButtonEvent oPress =
        MakeEvent(modules::UserButton::P1O,
                  fxe::Button::Event::Press,
                  modules::UiOwner::P1);
    const modules::ButtonEvent oRelease =
        MakeEvent(modules::UserButton::P1O,
                  fxe::Button::Event::Release,
                  modules::UiOwner::P1);
    ControlHarness oHold;
    oHold.Feed(oPress, 7000U);
    oHold.Tick(8000U);
    oHold.Tick(8100U);
    oHold.Feed(oRelease, 8200U);
    Check(oHold.observed.coachingHolds == 1U &&
              oHold.observed.hintTaps == 0U,
          "O hold emits Coaching once and release never also Hints");

    ControlHarness crossOwner;
    crossOwner.Feed(xPress, 9000U);
    crossOwner.Feed(MakeEvent(modules::UserButton::P2O,
                              fxe::Button::Event::Press,
                              modules::UiOwner::P2),
                    9001U);
    crossOwner.Feed(xRelease, 9050U);
    crossOwner.Feed(MakeEvent(modules::UserButton::P2O,
                              fxe::Button::Event::Release,
                              modules::UiOwner::P2),
                    9051U);
    Check(crossOwner.observed.resetStarts == 0U &&
              crossOwner.observed.resetTriggers == 0U &&
              crossOwner.observed.undoTaps == 1U &&
              crossOwner.observed.hintTaps == 1U,
          "P1 X plus P2 O never forms a cross-owner reset chord");

    ControlHarness simultaneous;
    simultaneous.Feed(xPress, 10000U);
    simultaneous.Feed(oPress, 10010U);
    simultaneous.Feed(MakeEvent(modules::UserButton::P2O,
                                fxe::Button::Event::Press,
                                modules::UiOwner::P2),
                      10020U);
    simultaneous.Feed(MakeEvent(modules::UserButton::P2X,
                                fxe::Button::Event::Press,
                                modules::UiOwner::P2),
                      10030U);
    simultaneous.Tick(12030U);
    simultaneous.Tick(12100U);
    Check(simultaneous.observed.resetStarts == 2U &&
              simultaneous.observed.resetTriggers == 2U,
          "simultaneous same-owner chords each trigger exactly once");
}

} // namespace

int main() {
    std::puts("[BUTTON_HOLD_PRIORITY] start");

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    modules::QuickSettingsOverlayController quickController(modules::QuickSettingsOverlayControllerConfig{1000U});
    modules::ResetHoldGestureController resetController(modules::ResetHoldGestureControllerConfig{2000U});
    modules::HintButtonGestureController hintController(modules::HintButtonGestureControllerConfig{1000U});

    modules::QuickSettingsAction quickAction;
    modules::ResetHoldAction resetAction;
    modules::HintButtonAction hintAction;

    const modules::ButtonEvent xPress =
        MakeEvent(modules::UserButton::P1X, fxe::Button::Event::Press, modules::UiOwner::P1);

    modules::QuickSettingsOverlayController cancelController(
        modules::QuickSettingsOverlayControllerConfig{1000U});
    const bool consumedByCancelPress = cancelController.HandleButtonEvent(
        xPress, false, true, quickAction, 0U);
    Check(consumedByCancelPress,
          "X press is consumed while an undo prompt is active");
    Check(quickAction.type == modules::QuickSettingsActionType::UndoTap,
          "active undo cancels on X press without waiting for release");
    const modules::ButtonEvent cancelRelease =
        MakeEvent(modules::UserButton::P1X,
                  fxe::Button::Event::Release,
                  modules::UiOwner::P1);
    Check(cancelController.HandleButtonEvent(
              cancelRelease, false, false, quickAction, 100U),
          "release after immediate cancel is consumed");
    Check(quickAction.type == modules::QuickSettingsActionType::None,
          "release after immediate cancel does not request a second undo");
    fxe::ManualMillis::SetNowMs(1200U);
    Check(!cancelController.Tick(quickAction),
          "immediate cancel cannot become a quick-settings hold");
    fxe::ManualMillis::SetNowMs(0U);

    const bool consumedByResetXPress = resetController.HandleButtonEvent(xPress, resetAction);
    const bool consumedByQuickXPress = quickController.HandleButtonEvent(xPress, false, quickAction);
    Check(!consumedByResetXPress, "single X press not consumed by reset chord");
    Check(consumedByQuickXPress, "single X press consumed by quick settings controller");

    fxe::ManualMillis::AdvanceMs(1200U);

    const bool quickTicked = quickController.Tick(quickAction);
    Check(quickTicked, "X hold opens quick settings");
    Check(quickAction.type == modules::QuickSettingsActionType::OpenOverlay,
          "X hold emits open-overlay action");
    Check(quickAction.owner == modules::UiOwner::P1, "X hold opens overlay for P1 owner");

    quickController.Reset();
    resetController.Reset();
    fxe::ManualMillis::SetNowMs(0);

    (void)resetController.HandleButtonEvent(xPress, resetAction);
    (void)quickController.HandleButtonEvent(xPress, false, quickAction);

    fxe::ManualMillis::AdvanceMs(700U);

    const modules::ButtonEvent oPress =
        MakeEvent(modules::UserButton::P1O, fxe::Button::Event::Press, modules::UiOwner::P1);
    const bool consumedByResetOPress = resetController.HandleButtonEvent(oPress, resetAction);
    Check(consumedByResetOPress, "X+O chord consumed by reset controller");
    Check(resetAction.type == modules::ResetHoldActionType::StartHold,
          "X+O chord starts reset hold");

    quickController.Reset();

    fxe::ManualMillis::AdvanceMs(1500U);
    const bool quickTickBlocked = quickController.Tick(quickAction);
    Check(!quickTickBlocked, "quick settings stays blocked while reset chord is active");

    fxe::ManualMillis::AdvanceMs(600U);
    const bool resetTicked = resetController.Tick(resetAction);
    Check(resetTicked, "reset controller triggers reset after hold elapsed");
    Check(resetAction.type == modules::ResetHoldActionType::TriggerReset,
          "reset tick emits trigger-reset action");
    Check(resetAction.owner == modules::UiOwner::P1, "reset tick targets P1 owner");

    const modules::ButtonEvent oRelease =
        MakeEvent(modules::UserButton::P1O, fxe::Button::Event::Release, modules::UiOwner::P1);
    const bool consumedByResetORelease = resetController.HandleButtonEvent(oRelease, resetAction);
    Check(consumedByResetORelease, "reset controller consumes chord release");
    Check(resetAction.type == modules::ResetHoldActionType::CancelHold,
          "releasing reset chord cancels the hold session");

    quickController.Reset();
    resetController.Reset();
    fxe::ManualMillis::SetNowMs(0);

    const modules::ButtonEvent xRelease =
        MakeEvent(modules::UserButton::P1X, fxe::Button::Event::Release, modules::UiOwner::P1);
    (void)quickController.HandleButtonEvent(xPress, false, quickAction);
    const bool consumedByQuickRelease = quickController.HandleButtonEvent(xRelease, false, quickAction);
    Check(consumedByQuickRelease, "X release consumed by quick settings controller");
    Check(quickAction.type == modules::QuickSettingsActionType::UndoTap,
          "short X press still emits undo tap");

    hintController.Reset();
    fxe::ManualMillis::SetNowMs(0);

    const bool consumedByHintPress = hintController.HandleButtonEvent(oPress, hintAction);
    Check(consumedByHintPress, "Circle press consumed by hint controller");
    Check(hintAction.type == modules::HintButtonActionType::None,
          "Circle press alone does not toggle hint immediately");

    fxe::ManualMillis::AdvanceMs(100U);
    const bool consumedByHintRelease = hintController.HandleButtonEvent(oRelease, hintAction);
    Check(consumedByHintRelease, "Circle release consumed by hint controller");
    Check(hintAction.type == modules::HintButtonActionType::ToggleHint,
          "short Circle press toggles hint on release");

    hintController.Reset();
    fxe::ManualMillis::SetNowMs(0);
    (void)hintController.HandleButtonEvent(oPress, hintAction);
    fxe::ManualMillis::AdvanceMs(1200U);
    const bool hintTicked = hintController.Tick(hintAction);
    Check(hintTicked, "Circle hold reaches beginner-mode hold path");
    Check(hintAction.type == modules::HintButtonActionType::BeginnerModeHold,
          "Circle hold emits beginner-mode action");
    (void)hintController.HandleButtonEvent(oRelease, hintAction);
    Check(hintAction.type == modules::HintButtonActionType::None,
          "releasing after Circle hold does not also toggle hint");

    quickController.Reset();
    fxe::ManualMillis::SetNowMs(500U);
    (void)quickController.HandleButtonEvent(xPress, false, quickAction, 0U);
    fxe::ManualMillis::SetNowMs(1001U);
    Check(quickController.Tick(quickAction),
          "queued X press uses capture timestamp for quick-settings hold timing");
    Check(quickAction.type == modules::QuickSettingsActionType::OpenOverlay,
          "queued X press still opens quick settings");

    resetController.Reset();
    fxe::ManualMillis::SetNowMs(700U);
    (void)resetController.HandleButtonEvent(xPress, resetAction, 0U);
    fxe::ManualMillis::SetNowMs(800U);
    (void)resetController.HandleButtonEvent(oPress, resetAction, 100U);
    fxe::ManualMillis::SetNowMs(2101U);
    Check(resetController.Tick(resetAction),
          "queued reset chord uses capture timestamp for hold timing");
    Check(resetAction.type == modules::ResetHoldActionType::TriggerReset,
          "queued reset chord still triggers reset action");

    hintController.Reset();
    fxe::ManualMillis::SetNowMs(700U);
    (void)hintController.HandleButtonEvent(oPress, hintAction, 0U);
    fxe::ManualMillis::SetNowMs(1001U);
    Check(hintController.Tick(hintAction),
          "queued Circle press uses capture timestamp for beginner hold timing");
    Check(hintAction.type == modules::HintButtonActionType::BeginnerModeHold,
          "queued Circle press still emits beginner hold action");

    // Normalized ownership is authoritative. Rotation-aware input adapters may
    // preserve a physical P1/P2 button ID while retagging the logical owner;
    // gesture actions must follow the owner tag instead of reconstructing it.
    quickController.Reset();
    fxe::ManualMillis::SetNowMs(0U);
    const modules::ButtonEvent rotatedXPress =
        MakeEvent(modules::UserButton::P1X,
                  fxe::Button::Event::Press,
                  modules::UiOwner::P2);
    (void)quickController.HandleButtonEvent(rotatedXPress, false, quickAction, 0U);
    fxe::ManualMillis::SetNowMs(1001U);
    Check(quickController.Tick(quickAction),
          "owner-retagged X hold opens quick settings");
    Check(quickAction.owner == modules::UiOwner::P2,
          "owner-retagged X hold opens quick settings for logical P2");

    hintController.Reset();
    const modules::ButtonEvent rotatedOPress =
        MakeEvent(modules::UserButton::P1O,
                  fxe::Button::Event::Press,
                  modules::UiOwner::P2);
    const modules::ButtonEvent rotatedORelease =
        MakeEvent(modules::UserButton::P1O,
                  fxe::Button::Event::Release,
                  modules::UiOwner::P2);
    (void)hintController.HandleButtonEvent(rotatedOPress, hintAction, 0U);
    (void)hintController.HandleButtonEvent(rotatedORelease, hintAction, 100U);
    Check(hintAction.type == modules::HintButtonActionType::ToggleHint,
          "owner-retagged Circle tap toggles hint");
    Check(hintAction.owner == modules::UiOwner::P2,
          "owner-retagged Circle tap targets logical P2");

    resetController.Reset();
    (void)resetController.HandleButtonEvent(rotatedXPress, resetAction, 0U);
    Check(resetController.HandleButtonEvent(rotatedOPress, resetAction, 100U),
          "owner-retagged X+O chord starts reset hold");
    Check(resetAction.owner == modules::UiOwner::P2,
          "owner-retagged reset chord targets logical P2");

    CheckDirectEntryContexts();
    CheckUnusedCenterAndFaceFallbacks();
    CheckEveryChordOrdering();
    CheckImmediateSidePressAndVerticalGuard();
    CheckResetRequiresFullReleaseBeforeRetrigger();
    CheckResetChordUsesBoundedHumanStagger();

    if (g_failures == 0) {
        std::puts("[BUTTON_HOLD_PRIORITY] ok");
        return 0;
    }

    std::printf("[BUTTON_HOLD_PRIORITY] failures=%d\n", g_failures);
    return 1;
}
