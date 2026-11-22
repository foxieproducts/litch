#include <array>
#include <cstdio>

#include "foxie_embedded/button.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/user_input_timing.hpp"

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

struct CaptureSink {
    std::array<fxe::Button::Event, 16> events{};
    size_t count = 0;

    void Push(const fxe::Button::Event evt) {
        if (count >= events.size()) {
            return;
        }
        events[count++] = evt;
    }
};

} // namespace

int main() {
    std::puts("[REPEAT_USER_INPUT] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    // Case 1: Held button produces Repeat events after delay.
    {
        CaptureSink sink;
        fxe::Button btn;
        bool rawDown = false;

        fxe::Button::Config cfg;
        cfg.debounceMs = 0;
        cfg.canRepeat = true;
        cfg.beforeRepeatMs = hardware::user_input_timing::DPAD_BEFORE_REPEAT_MS;
        cfg.repeatRateMs = hardware::user_input_timing::DPAD_VALUE_REPEAT_RATE_MS;
        cfg.longHoldMs = 0;
        cfg.onEvent = [&sink](const fxe::Button::Event evt) { sink.Push(evt); };

        btn.SetConfig(cfg);
        btn.SetDetectionFn([&rawDown]() { return rawDown; });
        fxe::ManualMillis::SetNowMs(0);
        btn.Reset();

        fxe::ManualMillis::SetNowMs(0);
        rawDown = true;
        btn.Update(); // Press
        fxe::ManualMillis::SetNowMs(100);
        rawDown = true;
        btn.Update(); // nothing
        fxe::ManualMillis::SetNowMs(250);
        rawDown = true;
        btn.Update(); // Repeat
        fxe::ManualMillis::SetNowMs(274);
        rawDown = true;
        btn.Update(); // nothing
        fxe::ManualMillis::SetNowMs(275);
        rawDown = true;
        btn.Update(); // Repeat
        fxe::ManualMillis::SetNowMs(500);
        rawDown = false;
        btn.Update(); // Release

        CheckEqInt(static_cast<int>(sink.count), 4, "event count");
        if (sink.count >= 4) {
            CheckEqInt(static_cast<int>(sink.events[0]), static_cast<int>(fxe::Button::Event::Press), "evt0 press");
            CheckEqInt(static_cast<int>(sink.events[1]), static_cast<int>(fxe::Button::Event::Repeat), "evt1 repeat");
            CheckEqInt(static_cast<int>(sink.events[2]), static_cast<int>(fxe::Button::Event::Repeat), "evt2 repeat");
            CheckEqInt(static_cast<int>(sink.events[3]), static_cast<int>(fxe::Button::Event::Release), "evt3 release");
        }

        CheckEqInt(
            static_cast<int>(hardware::user_input_timing::DPAD_VALUE_REPEAT_RATE_MS),
            19,
            "up/down value adjustments use the 30-percent-faster repeat cadence");
        CheckEqInt(
            static_cast<int>(hardware::user_input_timing::DPAD_NAV_REPEAT_RATE_MS),
            50,
            "left/right navigation keeps its prior repeat cadence");
    }

    // Case 2: canRepeat=false suppresses repeats.
    {
        CaptureSink sink;
        fxe::Button btn;
        bool rawDown = false;

        fxe::Button::Config cfg;
        cfg.debounceMs = 0;
        cfg.canRepeat = false;
        cfg.beforeRepeatMs = 10;
        cfg.repeatRateMs = 1;
        cfg.longHoldMs = 0;
        cfg.onEvent = [&sink](const fxe::Button::Event evt) { sink.Push(evt); };

        btn.SetConfig(cfg);
        btn.SetDetectionFn([&rawDown]() { return rawDown; });
        fxe::ManualMillis::SetNowMs(0);
        btn.Reset();

        fxe::ManualMillis::SetNowMs(0);
        rawDown = true;
        btn.Update(); // Press
        fxe::ManualMillis::SetNowMs(1000);
        rawDown = true;
        btn.Update(); // no repeats
        fxe::ManualMillis::SetNowMs(1100);
        rawDown = false;
        btn.Update(); // Release

        CheckEqInt(static_cast<int>(sink.count), 2, "no repeats when disabled");
        if (sink.count >= 2) {
            CheckEqInt(static_cast<int>(sink.events[0]), static_cast<int>(fxe::Button::Event::Press), "press emitted");
            CheckEqInt(static_cast<int>(sink.events[1]), static_cast<int>(fxe::Button::Event::Release), "release emitted");
        }
    }

    if (g_failures == 0) {
        std::puts("[REPEAT_USER_INPUT] ok");
        return 0;
    }

    std::printf("[REPEAT_USER_INPUT] failures=%d\n", g_failures);
    return 1;
}
