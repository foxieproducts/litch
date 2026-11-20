#include <array>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/edge_led_renderer.hpp"
#include "modules/font_3x5.hpp"
#include "modules/font_6x4.hpp"
#include "modules/game_display_text_overlay.hpp"
#include "modules/led_text_renderer.hpp"
#include "modules/quick_settings_overlay_controller.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }

    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

uint8_t CountLit(const uint64_t mask) {
    uint8_t count = 0U;
    for (uint8_t bit = 0U; bit < 64U; ++bit) {
        if (((mask >> bit) & 1ULL) != 0ULL) {
            ++count;
        }
    }
    return count;
}

uint16_t Intensity(const modules::Rgb color) {
    return static_cast<uint16_t>(color.r) + color.g + color.b;
}

} // namespace

int main() {
    std::puts("[UX_FEEDBACK_SURFACE] start");

    const modules::led_text_renderer::FontMetrics readable =
        modules::led_text_renderer::GetFontMetrics(
            modules::led_text_renderer::FontRole::Readable);
    const modules::led_text_renderer::FontMetrics compact =
        modules::led_text_renderer::GetFontMetrics(
            modules::led_text_renderer::FontRole::Compact);
    Check(readable.glyphWidth == 4U && readable.glyphHeight == 6U,
          "readable role is a real 4-column by 6-row face");
    Check(compact.glyphWidth == 3U && compact.glyphHeight == 5U,
          "compact role preserves the 3-column by 5-row face");
    const modules::GameDisplayTextOverlayConfig overlayConfig;
    Check(overlayConfig.fontRole ==
              modules::led_text_renderer::FontRole::Readable,
          "menu and display overlays default to the readable role");

    const modules::Glyph6x4 readableZero = modules::font_6x4::GetGlyph('0');
    Check(readableZero.rows[0] == 0xFU,
          "readable zero uses all four columns");
    Check(readableZero.rows[5] == 0xFU,
          "readable zero uses the sixth row");

    const modules::Glyph3x5 compactA = modules::font_3x5::GetGlyph('A');
    Check(compactA.rows[0] == 0b010U && compactA.rows[1] == 0b101U &&
              compactA.rows[2] == 0b111U && compactA.rows[3] == 0b101U &&
              compactA.rows[4] == 0b101U,
          "compact A preserves the former glyph exactly");
    const uint64_t compactOne = modules::led_text_renderer::RasterizeTextMask(
        "1", 0U, 0U, modules::led_text_renderer::FontRole::Compact);
    Check((compactOne & (1ULL << 8U)) == 0ULL &&
              (compactOne & (1ULL << 9U)) != 0ULL &&
              (compactOne & (1ULL << 10U)) != 0ULL,
          "compact raster preserves historical least-significant-bit-left orientation");

    const uint64_t readableMask = modules::led_text_renderer::RasterizeTextMask(
        "10", 0U, 1U, modules::led_text_renderer::FontRole::Readable);
    const uint64_t compactMask = modules::led_text_renderer::RasterizeTextMask(
        "10", 0U, 1U, modules::led_text_renderer::FontRole::Compact);
    Check(readableMask != 0ULL && compactMask != 0ULL,
          "both named roles rasterize headlessly");
    Check(readableMask != compactMask,
          "headless mask distinguishes readable and compact output");
    Check(CountLit(readableMask) > CountLit(compactMask),
          "readable two-character token uses the larger footprint");
    Check((readableMask & (0xFFULL << 56U)) == 0ULL,
          "readable two-character menu token stays inside its 8x8 layout");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    modules::QuickSettingsOverlayController controller(
        modules::QuickSettingsOverlayControllerConfig{1000U});
    modules::QuickSettingsAction action;
    const modules::ButtonEvent press{
        modules::UserButton::P1X, fxe::Button::Event::Press,
        modules::UiOwner::P1};
    Check(controller.HandleButtonEvent(press, false, action, 100U),
          "X press is tracked for quick settings");

    modules::UiOwner holdOwner = modules::UiOwner::None;
    uint8_t holdProgress = 255U;
    Check(controller.TryGetOpenHoldProgress(holdOwner, holdProgress) &&
              holdOwner == modules::UiOwner::P1 && holdProgress == 0U,
          "hold feedback acknowledges the press immediately");

    fxe::ManualMillis::SetNowMs(600U);
    Check(controller.TryGetOpenHoldProgress(holdOwner, holdProgress) &&
              holdProgress >= 127U && holdProgress <= 128U,
          "hold feedback reports deterministic proportional progress");

    modules::EdgeLedPalette palette;
    palette.p1 = modules::Rgb{80U, 40U, 20U};
    modules::EdgeLedRenderer renderer(palette);
    modules::EdgeLedState state;
    state.mode = modules::EdgeUiMode::Menu;
    state.owner = modules::UiOwner::P1;
    state.menuEdgeOwner = modules::UiOwner::P1;
    state.menuValueIsHoldProgress = true;
    state.menuHoldFillAll = false;
    state.menuValue01_255 = holdProgress;

    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    renderer.Render(state, edgeFrame);
    Check(Intensity(edgeFrame[23]) > Intensity(edgeFrame[16]),
          "non-destructive hold fills the owner's home edge progressively");
    Check(Intensity(edgeFrame[0]) == 0U && Intensity(edgeFrame[8]) == 0U,
          "hold discoverability does not add unrelated edge noise");

    fxe::ManualMillis::SetNowMs(1100U);
    Check(controller.Tick(action) &&
              action.type == modules::QuickSettingsActionType::OpenOverlay,
          "hold still opens quick settings at the configured threshold");
    Check(!controller.TryGetOpenHoldProgress(holdOwner, holdProgress),
          "preview ends when the held action fires");

    modules::QuickSettingsOverlayController tapController(
        modules::QuickSettingsOverlayControllerConfig{1000U});
    fxe::ManualMillis::SetNowMs(2000U);
    Check(tapController.HandleButtonEvent(press, false, action, 2000U),
          "short X tap begins with the same acknowledgement");
    const modules::ButtonEvent release{
        modules::UserButton::P1X, fxe::Button::Event::Release,
        modules::UiOwner::P1};
    fxe::ManualMillis::SetNowMs(2050U);
    Check(tapController.HandleButtonEvent(release, false, action, 2050U) &&
              action.type == modules::QuickSettingsActionType::UndoTap,
          "releasing early preserves the normal X-tap Undo action");
    Check(!tapController.TryGetOpenHoldProgress(holdOwner, holdProgress),
          "short-tap release removes hold feedback");

    modules::QuickSettingsOverlayController modalController(
        modules::QuickSettingsOverlayControllerConfig{1000U});
    fxe::ManualMillis::SetNowMs(3000U);
    Check(modalController.HandleButtonEvent(
              press, false, false, false, action, 3000U),
          "modal-owned X press remains tracked for its short cancel path");
    fxe::ManualMillis::SetNowMs(3500U);
    Check(!modalController.TryGetOpenHoldProgress(holdOwner, holdProgress),
          "blocked X hold never advertises Quick Setup progress");
    Check(!modalController.Tick(action, false),
          "blocked X hold cannot open Quick Setup at its threshold");

    if (g_failures == 0) {
        std::puts("[UX_FEEDBACK_SURFACE] ok");
        return 0;
    }

    std::printf("[UX_FEEDBACK_SURFACE] failures=%d\n", g_failures);
    return 1;
}
