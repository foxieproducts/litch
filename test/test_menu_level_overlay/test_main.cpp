#include <array>
#include <cstdio>

#include <string_view>

#include "foxie_embedded/millis.hpp"
#include "modules/menu_level_overlay.hpp"
#include "modules/quick_settings_items.hpp"
#include "modules/quick_settings_orchestration.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckEqStr(const std::string_view a, const std::string_view b, const char* msg) {
    if (a == b) {
        return;
    }

    std::printf("[FAIL] %s (got='%.*s' want='%.*s')\n",
                msg,
                static_cast<int>(a.size()),
                a.data(),
                static_cast<int>(b.size()),
                b.data());
    ++g_failures;
}

bool IsRgbEqual(const modules::Rgb a, const modules::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool IsRgbBlack(const modules::Rgb c) {
    return c.r == 0 && c.g == 0 && c.b == 0;
}

bool FramesEqual(
    const std::array<modules::Rgb,
                     modules::led_text_renderer::GRID_PIXELS>& left,
    const std::array<modules::Rgb,
                     modules::led_text_renderer::GRID_PIXELS>& right) {
    for (size_t index = 0U; index < left.size(); ++index) {
        if (!IsRgbEqual(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    std::puts("[MENU_LEVEL_OVERLAY] start");

    modules::GameDisplayTextOverlayConfig cfg;
    cfg.fadeInMs = 10;
    cfg.holdMs = 20;
    cfg.fadeOutMs = 10;

    modules::MenuLevelOverlay overlay(cfg);

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);

    std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> frame{};
    for (auto& px : frame) {
        px = modules::Rgb(50, 0, 0);
    }

    // The missing item-8 glyph was in the real selection adapter, which stopped
    // the overlay. Exercise that adapter rather than only parsing an asset.
    {
        modules::SettingsStore settings;
        modules::ControlMutationService controls;
        modules::MenuLevelOverlay::Config selectionCfg;
        selectionCfg.icon.fadeInMs = 0U;
        selectionCfg.icon.holdMs = 100U;
        selectionCfg.icon.fadeOutMs = 0U;
        modules::MenuLevelOverlay selection(selectionCfg);
        std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> black{};
        std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> selected{};
        modules::QuickSettingsOrchestrationContext context{
            settings, controls, selection, black, modules::UiOwner::P1,
            modules::quick_settings_items::OWNER_PAIRING};
        fxe::ManualMillis::SetNowMs(0U);
        modules::ShowQuickSettingsSelection(context);
        selection.Compose(black, selected, true);
        Check(selection.IsActive() && !FramesEqual(black, selected),
              "selected item 8 exposes its Bluetooth glyph through production orchestration");
        for (uint8_t y = 1U; y <= 6U; ++y)
            Check(IsRgbEqual(selected[y * 8U + 3U], modules::RGB_BLUE),
                  "Bluetooth rune has its continuous blue vertical stem");
        for (const auto square : {17U, 21U, 41U, 45U})
            Check(IsRgbEqual(selected[square], modules::RGB_BLUE),
                  "Bluetooth rune exposes both crossed strokes and right lobes");
        Check(selection.Text().empty(), "pairing selection has no fake numeric value");
        for (const auto elapsed : {101U, 12000U, 60000U}) {
            fxe::ManualMillis::SetNowMs(elapsed);
            std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> held{};
            selection.Compose(black, held, true);
            Check(selection.IsActive() && FramesEqual(selected, held),
                  "selected Bluetooth glyph remains stable beyond ordinary icon expiry without restart");
        }
        context.selectedIndex = modules::quick_settings_items::TIMER;
        modules::ShowQuickSettingsSelection(context);
        fxe::ManualMillis::SetNowMs(60101U);
        selection.Compose(black, selected, true);
        CheckEqStr(selection.Text(), "0", "navigation replaces pinned glyph with ordinary icon then value");
        context.selectedIndex = modules::quick_settings_items::OWNER_PAIRING;
        modules::ShowQuickSettingsSelection(context);
        Check(selection.IsActive() && selection.Text().empty(),
              "returning to pairing starts its glyph without retaining the prior value");
        selection.Stop();
        selection.Compose(frame, selected, false);
        Check(!selection.IsActive() && FramesEqual(frame, selected),
              "menu close clears pinned glyph and restores live board output");

        modules::MenuLevelOverlay::IconPixels transient{};
        transient[0U] = modules::RGB_BLUE;
        selection.ShowTransientIcon(transient, black, 20U, 10U);
        fxe::ManualMillis::AdvanceMs(31U);
        selection.Compose(black, selected, true);
        Check(!selection.IsActive() && FramesEqual(black, selected),
              "ordinary transient icon still expires while menu feedback reports active");
        selection.ShowTransientIcon(transient, black, 20U, 10U);
        fxe::ManualMillis::AdvanceMs(31U);
        // No Compose has retired the expired transient's captured snapshot.
        black.fill(modules::Rgb{50U, 0U, 0U});
        modules::ShowQuickSettingsSelection(context);
        selection.Compose(black, selected, true);
        Check(selected[0U].r != 0U && selected[0U].g == 0U,
              "pinned menu glyph captures current background after uncomposed transient expiry");
    }

    // Board-grid values avoid letters: <=0 => "0", otherwise digits.
    fxe::ManualMillis::SetNowMs(0);
    overlay.OnVsCpuLevelChanged(3, frame);
    Check(overlay.IsActive(), "VS CPU overlay active after change");
    Check(overlay.Test_GetLastVsCpuLevel() == 3, "VS CPU last level=3");
    CheckEqStr(overlay.Test_GetLastText(), "3", "VS CPU text '3'");

    fxe::ManualMillis::SetNowMs(1);
    overlay.OnVsCpuLevelChanged(10, frame);
    Check(overlay.IsActive(), "VS CPU overlay active after rapid change");
    Check(overlay.Test_GetLastVsCpuLevel() == 10, "VS CPU last level=10");
    CheckEqStr(overlay.Test_GetLastText(), "10", "VS CPU text '10'");

    fxe::ManualMillis::SetNowMs(2);
    overlay.OnVsCpuLevelChanged(11, frame);
    Check(overlay.IsActive(), "VS CPU overlay active after rapid change to 11");
    Check(overlay.Test_GetLastVsCpuLevel() == 11, "VS CPU last level=11");
    CheckEqStr(overlay.Test_GetLastText(), "11", "VS CPU text '11'");

    fxe::ManualMillis::SetNowMs(3);
    overlay.OnVsCpuLevelChanged(-5, frame);
    Check(overlay.Test_GetLastVsCpuLevel() == 0, "VS CPU clamps below 0 to 0");
    CheckEqStr(overlay.Test_GetLastText(), "0", "VS CPU 0 => '0'");

    fxe::ManualMillis::SetNowMs(4);
    overlay.OnVsCpuLevelChanged(999, frame);
    Check(overlay.Test_GetLastVsCpuLevel() == 11, "VS CPU clamps above 11 to 11");
    CheckEqStr(overlay.Test_GetLastText(), "11", "VS CPU clamp => '11'");

    fxe::ManualMillis::SetNowMs(50);
    Check(!overlay.IsActive(), "overlay expires after total duration");

    // An adjustment while the selection icon is showing must cancel its
    // queued old value. Compose is the production transition that used to
    // replace the new difficulty with that stale selection value.
    {
        modules::MenuLevelOverlay::Config rapidCfg;
        rapidCfg.icon.fadeInMs = 0U;
        rapidCfg.icon.holdMs = 100U;
        rapidCfg.icon.fadeOutMs = 0U;
        rapidCfg.text.fadeInMs = 0U;
        rapidCfg.text.holdMs = 500U;
        rapidCfg.text.fadeOutMs = 0U;
        modules::MenuLevelOverlay rapid(rapidCfg);
        std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> rendered{};
        fxe::ManualMillis::SetNowMs(60U);
        rapid.OnMenuSelectionChanged(modules::menu_option_indices::VS_CPU,
                                     0, frame, 60U, true, {});
        fxe::ManualMillis::SetNowMs(80U);
        rapid.OnVsCpuLevelChanged(1, frame);
        rapid.Compose(frame, rendered, true);
        CheckEqStr(rapid.Test_GetLastText(), "1",
                   "CPU adjustment supersedes queued selection value on compose");
        fxe::ManualMillis::SetNowMs(180U);
        rapid.Compose(frame, rendered, true);
        CheckEqStr(rapid.Test_GetLastText(), "1",
                   "CPU adjustment persists beyond the old icon deadline");
    }

    // BRIGHTNESS overlay: clamp 1..8, yellow.
    fxe::ManualMillis::SetNowMs(100);
    overlay.OnMenuValueChanged(modules::menu_option_indices::BRIGHTNESS,
                               6,
                               frame,
                               0,
                               true,
                               std::string_view{});
    CheckEqStr(overlay.Test_GetLastText(), "6", "BRIGHTNESS => '6'");
    Check(IsRgbEqual(overlay.Test_GetLastColor(), modules::RGB_YELLOW), "BRIGHTNESS => yellow");

    fxe::ManualMillis::SetNowMs(101);
    overlay.OnMenuValueChanged(modules::menu_option_indices::BRIGHTNESS,
                               999,
                               frame,
                               0,
                               true,
                               std::string_view{});
    CheckEqStr(overlay.Test_GetLastText(), "8", "BRIGHTNESS clamps to 8");

    // FLIP_SIDES overlay: 1/0, green when enabled.
    fxe::ManualMillis::SetNowMs(120);
    overlay.OnMenuValueChanged(modules::menu_option_indices::FLIP_SIDES,
                               1,
                               frame,
                               0,
                               true,
                               std::string_view{});
    CheckEqStr(overlay.Test_GetLastText(), "1", "FLIP_SIDES enabled => '1'");
    Check(IsRgbEqual(overlay.Test_GetLastColor(), modules::RGB_GREEN), "FLIP_SIDES enabled => green");

    fxe::ManualMillis::SetNowMs(121);
    overlay.OnMenuValueChanged(modules::menu_option_indices::FLIP_SIDES,
                               0,
                               frame,
                               0,
                               true,
                               std::string_view{});
    CheckEqStr(overlay.Test_GetLastText(), "0", "FLIP_SIDES disabled => '0'");

    // TIMER uses compact base-minute text that fits the board display.
    static constexpr struct {
        int value;
        const char* text;
    } TIMER_CASES[] = {{0, "0"}, {2, "3"}, {5, "15"}, {6, "30"}};
    for (const auto& timerCase : TIMER_CASES) {
        overlay.OnMenuValueChanged(modules::menu_option_indices::TIMER,
                                   timerCase.value,
                                   frame,
                                   0,
                                   true,
                                   std::string_view{});
        CheckEqStr(overlay.Test_GetLastText(), timerCase.text,
                   "TIMER uses the documented compact value");
    }

    // Every numeric Quick Setup item first establishes a distinct non-letter
    // identity. Shared 0/1/2 values cannot rely on perimeter position alone.
    {
        modules::MenuLevelOverlay::Config identityCfg;
        identityCfg.icon.fadeInMs = 0U;
        identityCfg.icon.holdMs = 100U;
        identityCfg.icon.fadeOutMs = 0U;
        identityCfg.text.fadeInMs = 0U;
        identityCfg.text.holdMs = 100U;
        identityCfg.text.fadeOutMs = 0U;
        constexpr std::array<uint8_t, 6> OPTIONS = {
            modules::menu_option_indices::VS_CPU,
            modules::menu_option_indices::CPU_PLAYER,
            modules::menu_option_indices::GAME_FIRST_PLAYER,
            modules::menu_option_indices::BRIGHTNESS,
            modules::menu_option_indices::FLIP_SIDES,
            modules::menu_option_indices::TIMER,
        };
        constexpr std::array<int, OPTIONS.size()> VALUES = {
            1, 1, 1, 5, 0, 0};
        std::array<std::array<modules::Rgb,
                              modules::led_text_renderer::GRID_PIXELS>,
                   OPTIONS.size()>
            identityFrames{};
        std::array<modules::Rgb,
                   modules::led_text_renderer::GRID_PIXELS>
            blackFrame{};
        blackFrame.fill(modules::RGB_BLACK);

        for (size_t option = 0U; option < OPTIONS.size(); ++option) {
            modules::MenuLevelOverlay candidate(identityCfg);
            fxe::ManualMillis::SetNowMs(200U);
            candidate.OnMenuSelectionChanged(
                OPTIONS[option],
                VALUES[option],
                blackFrame,
                0U,
                true,
                std::string_view{});
            CheckEqStr(candidate.Test_GetLastText(), std::string_view{},
                       "numeric item selection starts with no letter text");
            candidate.Compose(blackFrame, identityFrames[option]);
            bool anyIconPixel = false;
            for (const modules::Rgb pixel : identityFrames[option]) {
                anyIconPixel = anyIconPixel || !IsRgbBlack(pixel);
            }
            Check(anyIconPixel,
                  "numeric Quick Setup item exposes a non-letter icon");

            fxe::ManualMillis::SetNowMs(301U);
            std::array<modules::Rgb,
                       modules::led_text_renderer::GRID_PIXELS>
                valueFrame{};
            candidate.Compose(blackFrame, valueFrame);
            Check(!candidate.Test_GetLastText().empty(),
                  "numeric value follows the item icon");
        }
        for (size_t left = 0U; left < OPTIONS.size(); ++left) {
            for (size_t right = left + 1U; right < OPTIONS.size(); ++right) {
                Check(!FramesEqual(identityFrames[left],
                                   identityFrames[right]),
                      "numeric Quick Setup item icons remain distinct");
            }
        }
    }

    // Production render feedback reports the overlay itself as visually
    // active. A transient board-control rejection must not mistake that
    // visibility for an interactive menu owner and pin itself forever.
    fxe::ManualMillis::SetNowMs(200);
    overlay.ShowTransientMessage("NO", modules::RGB_WHITE, frame);
    std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS>
        messageFrame{};
    overlay.Compose(frame, messageFrame, overlay.IsActive());
    Check(overlay.IsActive(),
          "transient board-control message is active during its timeline");
    fxe::ManualMillis::SetNowMs(241);
    overlay.Compose(frame, messageFrame, overlay.IsActive());
    Check(!overlay.IsActive(),
          "transient board-control message expires despite active-state feedback");
    Check(IsRgbEqual(messageFrame[0], frame[0]),
          "expired transient message returns composition to its background");

    fxe::ManualMillis::SetNowMs(250);
    overlay.ShowTransientMessage("BUS", modules::RGB_WHITE, frame);
    overlay.Stop();
    Check(!overlay.IsActive(),
          "transient board-control message remains immediately cancelable");

    const auto expiredSnapshotUsesNewBackground =
        [&](const bool startMenuSelection, const uint32_t startMs) {
            modules::MenuLevelOverlay candidate(cfg);
            std::array<modules::Rgb,
                       modules::led_text_renderer::GRID_PIXELS>
                oldFrame{};
            std::array<modules::Rgb,
                       modules::led_text_renderer::GRID_PIXELS>
                newFrame{};
            oldFrame.fill(modules::Rgb{50, 0, 0});
            newFrame.fill(modules::Rgb{0, 50, 0});
            fxe::ManualMillis::SetNowMs(startMs);
            candidate.ShowTransientMessage("NO", modules::RGB_WHITE,
                                           oldFrame);
            // Match production gating: no Compose call cleans up the naturally
            // expired transient before the next surface starts.
            fxe::ManualMillis::SetNowMs(startMs + 41U);
            if (startMenuSelection) {
                candidate.OnMenuSelectionChanged(
                    modules::menu_option_indices::BRIGHTNESS,
                    5, newFrame, 0U, true, std::string_view{});
            } else {
                candidate.ShowMessage("P1", modules::RGB_WHITE, newFrame);
            }
            std::array<modules::Rgb,
                       modules::led_text_renderer::GRID_PIXELS>
                output{};
            candidate.Compose(newFrame, output);
            return output[0].r == 0U && output[0].g != 0U;
        };
    Check(expiredSnapshotUsesNewBackground(true, 300U),
          "menu selection ignores an expired transient snapshot");
    Check(expiredSnapshotUsesNewBackground(false, 400U),
          "owned text replaces an expired transient snapshot");

    constexpr uint32_t PINNED_MESSAGE_START_MS = 500U;
    overlay.SetOwner(modules::UiOwner::P1);
    fxe::ManualMillis::SetNowMs(PINNED_MESSAGE_START_MS);
    overlay.ShowMessage("P1", modules::RGB_WHITE, frame);
    overlay.Compose(frame, messageFrame, true);
    fxe::ManualMillis::SetNowMs(PINNED_MESSAGE_START_MS + 2241U);
    overlay.Compose(frame, messageFrame, true);
    Check(overlay.IsActive(),
          "ordinary owned message stays pinned beyond 2240 ms");
    overlay.Stop();
    Check(!overlay.IsActive(),
          "stop ends an ordinary pinned message immediately");

    // Regression: if a value change happens while an icon overlay is visible,
    // the number overlay must not preserve the icon pixels in its snapshot.
    {
        modules::MenuLevelOverlay::Config cfg2;
        cfg2.text.fadeInMs = 0;
        cfg2.text.holdMs = 50;
        cfg2.text.fadeOutMs = 0;
        cfg2.icon.fadeInMs = 0;
        cfg2.icon.holdMs = 500;
        cfg2.icon.fadeOutMs = 0;

        modules::MenuLevelOverlay overlay2(cfg2);

        std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> base{};
        for (auto& px : base) {
            px = modules::RGB_BLACK;
        }

        fxe::ManualMillis::SetNowMs(1000);
        overlay2.OnMenuSelectionChanged(modules::menu_option_indices::BRIGHTNESS,
                                        5,
                                        base,
                                        0,
                                        true,
                                        std::string_view{});

        std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> iconFrame{};
        overlay2.Compose(base, iconFrame);

        bool foundIconPixel = false;
        size_t probeIdx = 0;
        for (size_t i = 0; i < iconFrame.size(); ++i) {
            const uint8_t x = static_cast<uint8_t>(i % modules::led_text_renderer::GRID_W);
            const uint8_t y = static_cast<uint8_t>(i / modules::led_text_renderer::GRID_W);

            const bool inIconRegion = (x >= 1 && x <= 6 && y >= 1 && y <= 6);
            const bool outsideOneDigitText = (x >= 4);
            if (!inIconRegion || !outsideOneDigitText) {
                continue;
            }

            if (!IsRgbBlack(iconFrame[i])) {
                foundIconPixel = true;
                probeIdx = i;
                break;
            }
        }

        Check(foundIconPixel, "found a non-black icon pixel to probe");

        // Simulate menu controller behavior: value change gets passed the *current displayed frame*
        // (which already has the icon overlay drawn).
        fxe::ManualMillis::SetNowMs(1001);
        overlay2.OnMenuValueChanged(modules::menu_option_indices::BRIGHTNESS,
                                    6,
                                    iconFrame,
                                    0,
                                    true,
                                    std::string_view{});

        std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> valueFrame{};
        overlay2.Compose(base, valueFrame);

        Check(IsRgbBlack(valueFrame[probeIdx]), "value overlay snapshot does not preserve icon pixels");
    }

    if (g_failures == 0) {
        std::puts("[MENU_LEVEL_OVERLAY] ok");
        return 0;
    }

    std::printf("[MENU_LEVEL_OVERLAY] failures=%d\n", g_failures);
    return 1;
}
