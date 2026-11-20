#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "foxie_embedded/button.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/button_manager.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }
    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckEqInt(const int got, const int want, const char* msg) {
    if (got == want) {
        return;
    }
    std::printf("[FAIL] %s (got=%d want=%d)\n", msg, got, want);
    ++g_failures;
}

struct CapturedEvent {
    uint32_t tMs = 0;
    fxe::Button::Event evt = fxe::Button::Event::Press;
};

struct Capture {
    std::array<CapturedEvent, 32> events = {};
    size_t count = 0;

    void Push(const uint32_t tMs, const fxe::Button::Event evt) {
        if (count >= events.size()) {
            return;
        }
        events[count++] = CapturedEvent{tMs, evt};
    }

    void Reset() {
        count = 0;
    }
};

} // namespace

int main() {
    std::puts("[BUTTON_LOGIC] start");

    fxe::ManualMillis::SetNowMs(0);
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);

    // Case 1: Debounce ignores bounces and only commits stable edges.
    {
        fxe::Button b;
        Capture cap;
        bool rawDown = false;
        uint32_t nowMs = 0;

        fxe::Button::Config cfg;
        cfg.debounceMs = 20;
        cfg.canRepeat = false;
        cfg.longHoldMs = 0;
        cfg.onEvent = [&cap, &nowMs](const fxe::Button::Event evt) { cap.Push(nowMs, evt); };
        b.SetConfig(cfg);
        b.SetDetectionFn([&rawDown]() { return rawDown; });

        fxe::ManualMillis::SetNowMs(0);
        b.Reset();
        nowMs = 0;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = false;
        b.Update();

        cap.Reset();

        nowMs = 1;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        nowMs = 5;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = false;
        b.Update();
        nowMs = 10;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();

        nowMs = 29;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        CheckEqInt(static_cast<int>(cap.count), 0, "no events before debounce window completes");

        nowMs = 30;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        CheckEqInt(static_cast<int>(cap.count), 1, "press fires once after stable debounce");
        CheckEqInt(static_cast<int>(cap.events[0].tMs), 30, "press time matches commit time");
        CheckEqInt(static_cast<int>(cap.events[0].evt), static_cast<int>(fxe::Button::Event::Press), "event is Press");

        // Release bounce.
        cap.Reset();
        nowMs = 40;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = false;
        b.Update();
        nowMs = 45;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        nowMs = 50;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = false;
        b.Update();

        nowMs = 69;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = false;
        b.Update();
        CheckEqInt(static_cast<int>(cap.count), 0, "no release before debounce window completes");

        nowMs = 70;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = false;
        b.Update();
        CheckEqInt(static_cast<int>(cap.count), 1, "release fires once after stable debounce");
        CheckEqInt(static_cast<int>(cap.events[0].tMs), 70, "release time matches commit time");
        CheckEqInt(static_cast<int>(cap.events[0].evt), static_cast<int>(fxe::Button::Event::Release), "event is Release");
    }

    // Case 2: Repeat starts after beforeRepeatMs and fires at repeatRateMs cadence.
    {
        fxe::Button b;
        Capture cap;
        bool rawDown = false;
        uint32_t nowMs = 0;

        fxe::Button::Config cfg;
        cfg.debounceMs = 0;
        cfg.canRepeat = true;
        cfg.beforeRepeatMs = 100;
        cfg.repeatRateMs = 50;
        cfg.longHoldMs = 0;
        cfg.onEvent = [&cap, &nowMs](const fxe::Button::Event evt) { cap.Push(nowMs, evt); };
        b.SetConfig(cfg);
        b.SetDetectionFn([&rawDown]() { return rawDown; });

        fxe::ManualMillis::SetNowMs(0);
        b.Reset();
        cap.Reset();

        nowMs = 0;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        nowMs = 50;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        nowMs = 100;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        nowMs = 150;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();

        CheckEqInt(static_cast<int>(cap.count), 3, "press + two repeats");
        CheckEqInt(static_cast<int>(cap.events[0].evt), static_cast<int>(fxe::Button::Event::Press), "first event is Press");
        CheckEqInt(static_cast<int>(cap.events[1].evt), static_cast<int>(fxe::Button::Event::Repeat), "second event is Repeat");
        CheckEqInt(static_cast<int>(cap.events[2].evt), static_cast<int>(fxe::Button::Event::Repeat), "third event is Repeat");
        CheckEqInt(static_cast<int>(cap.events[1].tMs), 100, "first repeat at 100ms");
        CheckEqInt(static_cast<int>(cap.events[2].tMs), 150, "second repeat at 150ms");
    }

    // Case 3: LongHold fires once after longHoldMs.
    {
        fxe::Button b;
        Capture cap;
        bool rawDown = false;
        uint32_t nowMs = 0;

        fxe::Button::Config cfg;
        cfg.debounceMs = 0;
        cfg.canRepeat = false;
        cfg.longHoldMs = 500;
        cfg.onEvent = [&cap, &nowMs](const fxe::Button::Event evt) { cap.Push(nowMs, evt); };
        b.SetConfig(cfg);
        b.SetDetectionFn([&rawDown]() { return rawDown; });

        fxe::ManualMillis::SetNowMs(0);
        b.Reset();
        cap.Reset();

        nowMs = 0;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        nowMs = 499;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        nowMs = 500;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        nowMs = 700;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();

        CheckEqInt(static_cast<int>(cap.count), 2, "press + long hold");
        CheckEqInt(static_cast<int>(cap.events[0].evt), static_cast<int>(fxe::Button::Event::Press), "first event is Press");
        CheckEqInt(static_cast<int>(cap.events[1].evt), static_cast<int>(fxe::Button::Event::LongHold), "second event is LongHold");
        CheckEqInt(static_cast<int>(cap.events[1].tMs), 500, "long hold at 500ms");
    }

    // Case 4: beforePress delays press emission until the button stays down long enough.
    {
        fxe::Button b;
        Capture cap;
        bool rawDown = false;
        uint32_t nowMs = 0;

        fxe::Button::Config cfg;
        cfg.debounceMs = 0;
        cfg.beforePressMs = 35;
        cfg.canRepeat = false;
        cfg.longHoldMs = 0;
        cfg.onEvent = [&cap, &nowMs](const fxe::Button::Event evt) { cap.Push(nowMs, evt); };
        b.SetConfig(cfg);
        b.SetDetectionFn([&rawDown]() { return rawDown; });

        fxe::ManualMillis::SetNowMs(0);
        b.Reset();
        cap.Reset();

        nowMs = 0;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        CheckEqInt(static_cast<int>(cap.count), 0, "beforePress suppresses immediate press");

        nowMs = 34;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        CheckEqInt(static_cast<int>(cap.count), 0, "beforePress still holds before threshold");

        nowMs = 35;
        fxe::ManualMillis::SetNowMs(nowMs);
        rawDown = true;
        b.Update();
        CheckEqInt(static_cast<int>(cap.count), 1, "press emits once beforePress threshold is met");
        CheckEqInt(static_cast<int>(cap.events[0].evt), static_cast<int>(fxe::Button::Event::Press), "delayed event is Press");
        CheckEqInt(static_cast<int>(cap.events[0].tMs), 35, "delayed press time matches beforePress threshold");
    }

    // The board loop services input at 50 and 88 ms. At 88 the pin is
    // already open again. Replay the two edges captured during that gap.
    for (const uint32_t releaseMs : {60U, 80U}) {
        fxe::ManualMillis::SetNowMs(50U);
        hardware::ButtonManager<1> manager;
        fxe::Button::Config config;
        config.immediatePress = true;
        config.canRepeat = false;
        manager.ConfigureButton(0U, modules::UserButton::P1O, modules::UiOwner::P1,
                                config, [] { return false; }, true);
        manager.Begin();

        fxe::ManualMillis::SetNowMs(88U);
        manager.FeedEdge(0U, true, 55U);
        manager.FeedEdge(0U, false, releaseMs);
        manager.AdvanceTo(88U);

        modules::ButtonEvent event;
        Check(manager.TryReadQueuedEvent(event) && event.button == modules::UserButton::P1O &&
              event.event == fxe::Button::Event::Press && event.timestampValid && event.tMs == 55U,
              "a completed O tap delivers its original press after a service gap");
        fxe::ManualMillis::SetNowMs(110U);
        manager.AdvanceTo(110U);
        Check(manager.TryReadQueuedEvent(event) && event.button == modules::UserButton::P1O &&
              event.event == fxe::Button::Event::Release && event.timestampValid && event.tMs == releaseMs,
              "release keeps the contact time after the stable-open interval");
        Check(!manager.TryReadQueuedEvent(event), "one contact closure delivers exactly one tap");
    }

    if (g_failures == 0) {
        std::puts("[BUTTON_LOGIC] ok");
        return 0;
    }

    std::printf("[BUTTON_LOGIC] failures=%d\n", g_failures);
    return 1;
}
