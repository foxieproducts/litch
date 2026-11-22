#include <array>
#include <cstdint>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/menu_level_overlay.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"
#include "../support/led_frame_composer_snapshot_adapter.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

modules::Rgb SquareGridAt(const std::array<modules::Rgb, 64>& gridFrame, const uint8_t physicalSq) {
    return gridFrame[physicalSq];
}

modules::Rgb PieceLedAt(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame, const uint8_t physicalSq) {
    return ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Piece)];
}

uint16_t Intensity(const modules::Rgb c) {
    return static_cast<uint16_t>(c.r) + static_cast<uint16_t>(c.g) + static_cast<uint16_t>(c.b);
}

bool RgbEq(const modules::Rgb a, const modules::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

} // namespace

int main() {
    std::puts("[MENU_BACKGROUND_DIM] start");

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_menu_background_dim.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    // Make the board background dark so menu overlay pixels are clearly distinguishable.
    settings.Doc()["LIGHT_SQUARE_WHEEL"] = 0;
    settings.Doc()["DARK_SQUARE_WHEEL"] = 0;

    // Keep piece LEDs bright so dimming is easy to detect.
    settings.Doc()["P1_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_PIECE_WHEEL"] = 128;

    runtime::GameStateMachineConfig smCfg;
    smCfg.illegalFlashMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);

    std::array<bool, 64> occupiedPhysical{};
    occupiedPhysical.fill(false);
    occupiedPhysical[0] = true; // Ensure at least one piece LED is lit.
    sm.DebugSetPhysicalOccupancyMask(1ULL << 0U);

    // Activate a menu overlay so we can verify overlay pixels are not dimmed.
    modules::MenuLevelOverlay menuOverlay;
    menuOverlay.SetOwner(modules::UiOwner::P1);

    std::array<modules::Rgb, 64> blank{};
    blank.fill(modules::RGB_BLACK);
    menuOverlay.ShowMessage("OF", modules::RGB_WHITE, blank);

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeBoard{};
    std::array<modules::Rgb, 64> gridBoard{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledOverlayNoMenu{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeOverlayNoMenu{};
    std::array<modules::Rgb, 64> gridOverlayNoMenu{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledOverlayMenu{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeOverlayMenu{};
    std::array<modules::Rgb, 64> gridOverlayMenu{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledMenuNoOverlay{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeMenuNoOverlay{};
    std::array<modules::Rgb, 64> gridMenuNoOverlay{};

    modules::led_frame_composer::ComposeInputs in;
    in.settings = &settings;
    in.firstPlayerIsP2 = false;

    // A) Base board without overlay.
    in.menuOverlay = nullptr;
    in.menuActive = false;
    in.menuVisualAlpha255 = 0;
    in.menuIntent = modules::EdgeLedIntent{};
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledBoard, edgeBoard, gridBoard);

    // D) Menu active without overlay (background dim should apply everywhere).
    in.menuOverlay = nullptr;
    in.menuActive = true;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledMenuNoOverlay, edgeMenuNoOverlay, gridMenuNoOverlay);

    // B) Overlay active but menuActive=false (overlay should render, but background should not be dimmed).
    in.menuOverlay = &menuOverlay;
    in.menuActive = false;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledOverlayNoMenu, edgeOverlayNoMenu, gridOverlayNoMenu);

    // C) Overlay active and menuActive=true (background dim should run, but overlay pixels should match case B).
    in.menuActive = true;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledOverlayMenu, edgeOverlayMenu, gridOverlayMenu);

    // E) Edge-only hold progress overlay (fill-all) should render even when
    // menuActive=false and menuVisualAlpha255=0, and must not dim the board.
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledEdgeOnlyHold{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeEdgeOnlyHold{};
    std::array<modules::Rgb, 64> gridEdgeOnlyHold{};
    in.menuOverlay = nullptr;
    in.menuActive = false;
    in.menuVisualAlpha255 = 0;
    in.menuIntent = modules::EdgeLedIntent{};
    in.menuIntent.menuActive = true;
    in.menuIntent.menuOwner = modules::UiOwner::P1;
    in.menuIntent.menuHoldActive = true;
    in.menuIntent.menuHoldFillAll = true;
    in.menuIntent.menuValue01_255 = 255;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledEdgeOnlyHold, edgeEdgeOnlyHold, gridEdgeOnlyHold);

    Check(Intensity(edgeEdgeOnlyHold[0]) > 0U, "edge-only hold overlay lights edge LEDs");

    // Board squares and piece LEDs must match the baseline (no dim).
    Check(RgbEq(SquareGridAt(gridEdgeOnlyHold, 0), SquareGridAt(gridBoard, 0)),
          "edge-only hold overlay does not dim board squares");
    Check(RgbEq(PieceLedAt(ledEdgeOnlyHold, 0), PieceLedAt(ledBoard, 0)),
          "edge-only hold overlay does not dim piece LEDs");

    // Find one square that the overlay changes, and one square it leaves untouched.
    bool overlaySqFound = false;
    bool bgSqFound = false;
    uint8_t overlaySq = 0;
    uint8_t bgSq = 0;
    for (uint8_t sq = 0; sq < 64U; ++sq) {
        const modules::Rgb a = SquareGridAt(gridBoard, sq);
        const modules::Rgb b = SquareGridAt(gridOverlayNoMenu, sq);
        if (!overlaySqFound && !RgbEq(a, b)) {
            overlaySqFound = true;
            overlaySq = sq;
        }
        if (!bgSqFound && RgbEq(a, b)) {
            bgSqFound = true;
            bgSq = sq;
        }
        if (overlaySqFound && bgSqFound) {
            break;
        }
    }

    Check(overlaySqFound, "overlay affects at least one square");
    Check(bgSqFound, "overlay leaves at least one square untouched");

    if (overlaySqFound) {
        const modules::Rgb b = SquareGridAt(gridOverlayNoMenu, overlaySq);
        const modules::Rgb c = SquareGridAt(gridOverlayMenu, overlaySq);
        Check(RgbEq(b, c), "menu background dim does not dim menu overlay squares");
    }

    // Background dim should reduce ordinary board squares when the menu is open,
    // even when no overlay is active.
    const modules::Rgb baseSq = SquareGridAt(gridBoard, 0);
    const modules::Rgb dimSq = SquareGridAt(gridMenuNoOverlay, 0);
    Check(Intensity(dimSq) + 1U < Intensity(baseSq), "menu background dim reduces board squares");

    // Piece LEDs should also be dimmed when the menu is active.
    const modules::Rgb pieceNoMenu = PieceLedAt(ledOverlayNoMenu, 0);
    const modules::Rgb pieceMenu = PieceLedAt(ledOverlayMenu, 0);
    Check(Intensity(pieceNoMenu) > 0U, "baseline piece LED is lit");
    Check(Intensity(pieceMenu) < Intensity(pieceNoMenu), "menu background dim reduces piece LED intensity");

    if (g_failures == 0) {
        std::puts("[MENU_BACKGROUND_DIM] ok");
        return 0;
    }

    std::printf("[MENU_BACKGROUND_DIM] failures=%d\n", g_failures);
    return 1;
}
