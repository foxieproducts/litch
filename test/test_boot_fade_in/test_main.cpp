#include "foxie_embedded/millis.hpp"
#include "modules/boot_fade_in.hpp"
#include "modules/brightness_scale_policy.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckEqU16(const uint16_t a, const uint16_t b, const char* msg) {
    if (a == b) {
        return;
    }

    std::printf("[FAIL] %s: %u != %u\n", msg, static_cast<unsigned>(a), static_cast<unsigned>(b));
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[BOOT_FADE_IN] start");

    {
        modules::BootFadeIn fade;
        fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);

        // Not started: full scale.
        fxe::ManualMillis::SetNowMs(0);
        CheckEqU16(fade.GetFadeScale256(), 256, "not started => 256");
        Check(!fade.IsActive(), "not started => not active");

        fxe::ManualMillis::SetNowMs(100);
        fade.Start();

        // Immediately after start: 0.
        Check(fade.IsActive(), "active at start");
        CheckEqU16(fade.GetFadeScale256(), 0, "start => 0");

        fxe::ManualMillis::SetNowMs(110);
        const uint16_t elapsedScale = fade.GetFadeScale256();
        Check(elapsedScale > 0U && elapsedScale < 256U,
              "elapsed time advances the raw fade");

#ifndef ARDUINO
        // Native default duration matches legacy (30ms).
        fxe::ManualMillis::SetNowMs(129);
        Check(fade.IsActive(), "active before end (native)");
        fxe::ManualMillis::SetNowMs(130);
        CheckEqU16(fade.GetFadeScale256(), 256, "end => 256 (native)");
        Check(!fade.IsActive(), "inactive at end (native)");
#else
        // Embedded default duration matches legacy (500ms).
        fxe::ManualMillis::SetNowMs(599);
        Check(fade.IsActive(), "active before end (arduino)");
        fxe::ManualMillis::SetNowMs(600);
        CheckEqU16(fade.GetFadeScale256(), 256, "end => 256 (arduino)");
        Check(!fade.IsActive(), "inactive at end (arduino)");
#endif

        fxe::ManualMillis::SetNowMs(100000);
        CheckEqU16(fade.GetFadeScale256(), 256, "after end => 256");
    }

    {
        modules::BootFadeIn fade;
        fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);

        Check(fade.GetPreferredRenderCadenceMs() == 0U, "preferred cadence 0 when inactive");

        fxe::ManualMillis::SetNowMs(0);
        fade.Start();
        Check(fade.GetPreferredRenderCadenceMs() == 25U, "preferred cadence 25ms when active");

#ifndef ARDUINO
        fxe::ManualMillis::SetNowMs(30);
        Check(fade.GetPreferredRenderCadenceMs() == 0U, "preferred cadence 0 after end (native)");
#else
        fxe::ManualMillis::SetNowMs(500);
        Check(fade.GetPreferredRenderCadenceMs() == 0U, "preferred cadence 0 after end (arduino)");
#endif
    }

    {
        modules::BootFadeInConfig cfg;
        cfg.fadeDurationMs = 500U;
        modules::BootFadeIn fade(cfg);
        fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(400U);
        fade.Start();
        fxe::ManualMillis::SetNowMs(525U);
        CheckEqU16(fade.GetFadeScale256(), 64U,
                   "production fade is one quarter after125ms");
        CheckEqU16(modules::brightness_scale_policy::ComputeEffectiveScale256(
                       0U, false, 0.0f, fade.GetFadeScale256()),
                   2U, "near-dark effective scale2 is not raw fade2");
        fxe::ManualMillis::SetNowMs(900U);
        CheckEqU16(modules::brightness_scale_policy::ComputeEffectiveScale256(
                       0U, false, 0.0f, fade.GetFadeScale256()),
                   8U, "near-dark reaches its existing full scale after500ms");
    }

    if (g_failures == 0) {
        std::puts("[BOOT_FADE_IN] pass");
        return 0;
    }

    std::printf("[BOOT_FADE_IN] fail: %d\n", g_failures);
    return 1;
}
