#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/game_display_text_overlay.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

bool AnyNonBlack(const std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS>& frame) {
    for (const auto& px : frame) {
        if (px.r != 0 || px.g != 0 || px.b != 0) {
            return true;
        }
    }
    return false;
}

bool FramesEqual(const std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS>& a,
                 const std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS>& b) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    std::puts("[GAME_DISPLAY_TEXT_OVERLAY] start");

    modules::GameDisplayTextOverlayConfig cfg;
    cfg.fadeInMs = 10;
    cfg.holdMs = 10;
    cfg.fadeOutMs = 10;
    cfg.backgroundScale = 128;

    modules::GameDisplayTextOverlay overlay(cfg);

    std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> snapshotA{};
    std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> snapshotB{};

    for (auto& px : snapshotA) {
        px = modules::Rgb(100, 0, 0);
    }
    for (auto& px : snapshotB) {
        px = modules::Rgb(0, 100, 0);
    }

    std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> out{};

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);

    // Inactive passthrough.
    fxe::ManualMillis::SetNowMs(0);
    overlay.Compose(snapshotA, out);
    Check(FramesEqual(out, snapshotA), "inactive: passthrough");

    // Start overlay and confirm background is dimmed snapshot.
    fxe::ManualMillis::SetNowMs(0);
    overlay.Start("A", modules::RGB_WHITE, snapshotA);
    overlay.Compose(snapshotB, out);

    // Background should be from snapshotA (not current), and dimmed.
    Check(out[0].r < 100, "active: background dimmed");
    Check(out[0].g == 0 && out[0].b == 0, "active: background color preserved");

    // At full alpha (during hold), text should add some non-black component beyond the dimmed background.
    fxe::ManualMillis::SetNowMs(12);
    overlay.Compose(snapshotB, out);
    Check(AnyNonBlack(out), "hold: produces visible output");

    // Restart while active with a different snapshot should not change background snapshot (no flicker).
    fxe::ManualMillis::SetNowMs(13);
    overlay.Start("B", modules::RGB_WHITE, snapshotB);
    overlay.Compose(snapshotB, out);
    Check(out[0].g == 0, "restart: preserves previous snapshot (no flicker)");

    // After total duration, overlay becomes inactive and passthrough resumes.
    // Restart occurred at t=13; total duration is 30ms, so overlay ends at t=43.
    fxe::ManualMillis::SetNowMs(45);
    overlay.Compose(snapshotB, out);
    Check(FramesEqual(out, snapshotB), "after: inactive passthrough");

    if (g_failures == 0) {
        std::puts("[GAME_DISPLAY_TEXT_OVERLAY] ok");
        return 0;
    }

    std::printf("[GAME_DISPLAY_TEXT_OVERLAY] failures=%d\n", g_failures);
    return 1;
}
