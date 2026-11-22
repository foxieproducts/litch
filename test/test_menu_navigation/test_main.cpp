#include <cstdio>
#include <cstring>

#include <array>

#include "foxie_embedded/millis.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/menu_controller.hpp"
#include "modules/menu_level_overlay.hpp"
#include "modules/settings_store.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckEqInt(const int a, const int b, const char* msg) {
    if (a == b) {
        return;
    }

    std::printf("[FAIL] %s (got=%d want=%d)\n", msg, a, b);
    ++g_failures;
}

void CheckEqBool(const bool a, const bool b, const char* msg) {
    if (a == b) {
        return;
    }

    std::printf("[FAIL] %s (got=%s want=%s)\n", msg, a ? "true" : "false", b ? "true" : "false");
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

void Send(modules::MenuController& controller,
          const uint32_t nowMs,
          const modules::UserButton btn,
          const fxe::Button::Event event,
          modules::SettingsStore& settings,
          modules::MenuLevelOverlay& overlay,
          const modules::MenuOverlayContext& overlayCtx,
          const std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS>& grid,
          modules::ControlMutationService* const controlMutation = nullptr) {
    fxe::ManualMillis::SetNowMs(nowMs);
    (void)controller.HandleButtonEvent(
        btn, event, settings, overlay, overlayCtx, grid, controlMutation);
}

struct ControlHarness {
    modules::ControlMutationService service;
    modules::ControlCoordinatorResult startGate =
        modules::ControlCoordinatorResult::Applied;
    uint32_t cpuCancelCount = 0U;

    explicit ControlHarness(modules::SettingsStore& settings) {
        modules::ControlMutationCallbacks callbacks;
        callbacks.context = this;
        callbacks.checkStartOnlyGate = [](void* context, const char*) {
            return static_cast<ControlHarness*>(context)->startGate;
        };
        callbacks.cancelCpuWork = [](void* context) {
            ++static_cast<ControlHarness*>(context)->cpuCancelCount;
        };
        service.Configure(settings, callbacks);
    }
};

} // namespace

int main() {
    std::puts("[MENU_NAVIGATION] start");

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    modules::SettingsStoreConfig storeCfg;
    storeCfg.path = "test_menu_navigation_settings.json";
    storeCfg.deferredSaveDelayMs = 1;

    (void)std::remove(storeCfg.path);

    modules::SettingsStore settings(storeCfg);
    Check(settings.Load(), "settings.Load succeeds when missing");
    ControlHarness control(settings);

    modules::MenuController controller;
    controller.Reset();

    modules::GameDisplayTextOverlayConfig overlayCfg;
    overlayCfg.fadeInMs = 0;
    overlayCfg.holdMs = 50;
    overlayCfg.fadeOutMs = 0;

    modules::MenuLevelOverlay::Config menuOverlayCfg;
    menuOverlayCfg.text = overlayCfg;
    menuOverlayCfg.icon.fadeInMs = 0;
    menuOverlayCfg.icon.holdMs = 50;
    menuOverlayCfg.icon.fadeOutMs = 0;
    modules::MenuLevelOverlay overlay(menuOverlayCfg);

    std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> grid{};
    for (auto& px : grid) {
        px = modules::Rgb(0, 0, 0);
    }

    modules::MenuOverlayContext overlayCtx;
    overlayCtx.uptimeMs = 107U * 60U * 1000U;
    overlayCtx.settingsLoaded = true;
    overlayCtx.fwVersion = "0.2";
    overlayCtx.flipSidesToggleAllowed = true;
    overlayCtx.flipSidesToggleBlockReason = modules::MenuToggleBlockReason::None;
    overlayCtx.firstPlayerToggleAllowed = true;
    overlayCtx.firstPlayerToggleBlockReason = modules::MenuToggleBlockReason::None;

    uint32_t nowMs = 100;
    auto AdvanceMs = [&nowMs](const uint32_t deltaMs) {
        nowMs += deltaMs;
        return nowMs;
    };

    // Open menu explicitly (firmware owns hold-to-open behavior).
    fxe::ManualMillis::SetNowMs(nowMs);
    controller.OpenWithOverlay(modules::UiOwner::P1, settings, overlay, overlayCtx, grid);

    Check(controller.IsActive(), "menu active after OpenWithOverlay");
    Check(controller.GetOwner() == modules::UiOwner::P1, "menu owner=P1");
    CheckEqInt(controller.GetSelectedOptionIndex(), modules::menu_option_indices::VS_CPU, "menu selection starts at VS_CPU");

    // Navigation does not wrap.
    Send(controller, AdvanceMs(40), modules::UserButton::Left, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid);
    CheckEqInt(controller.GetSelectedOptionIndex(), modules::menu_option_indices::VS_CPU, "Left at start clamps (no wrap)");

    // Move right to FLIP_SIDES (rotate) and toggle via Up/Down (not via A).
    for (int i = 0; i < static_cast<int>(modules::menu_option_indices::FLIP_SIDES); ++i) {
        Send(controller, AdvanceMs(10), modules::UserButton::Right, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid);
    }
    CheckEqInt(controller.GetSelectedOptionIndex(), modules::menu_option_indices::FLIP_SIDES, "selection at FLIP_SIDES");

    CheckEqBool(settings.ReadValue("BOARD_ROTATE", false), false, "BOARD_ROTATE default=false");
    Send(controller, AdvanceMs(40), modules::UserButton::Up, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid);
    CheckEqBool(settings.ReadValue("BOARD_ROTATE", false), true, "BOARD_ROTATE toggles to true via Up");

    control.startGate = modules::ControlCoordinatorResult::Busy;
    Send(controller, AdvanceMs(10), modules::UserButton::Down,
         fxe::Button::Event::Press, settings, overlay, overlayCtx, grid,
         &control.service);
    CheckEqBool(settings.ReadValue("BOARD_ROTATE", false), true,
                "service-backed menu toggle honors busy start gate");
    control.startGate = modules::ControlCoordinatorResult::Applied;

    // Menu uses logical X/O/D-pad controls: any X exits the menu.
    Send(controller, AdvanceMs(1), modules::UserButton::P2X, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid);
    Check(!controller.IsActive(), "X exits menu regardless of owner");

    // Closing starts a short visual fade-out.
    Check(controller.IsVisualActive(), "menu visual active during close fade");
    Check(controller.GetVisualAlpha255() != 0, "menu visual alpha nonzero during close fade");

    // After the fade window, visual activity ends.
    fxe::ManualMillis::SetNowMs(AdvanceMs(controller.GetConfig().closeFadeOutMs + 1U));
    controller.Tick();
    Check(!controller.IsVisualActive(), "menu visual inactive after close fade");
    Check(controller.GetVisualAlpha255() == 0, "menu visual alpha 0 after close fade");

    // Re-open for subsequent navigation checks.
    controller.OpenWithOverlay(modules::UiOwner::P1, settings, overlay, overlayCtx, grid);
    Check(controller.IsActive(), "menu active after re-open");
    CheckEqInt(controller.GetSelectedOptionIndex(), modules::menu_option_indices::FLIP_SIDES, "selection preserved on re-open");

    // Gating blocks toggles.
    modules::MenuOverlayContext blockedCtx = overlayCtx;
    blockedCtx.flipSidesToggleAllowed = false;
    blockedCtx.flipSidesToggleBlockReason = modules::MenuToggleBlockReason::Position;
    Send(controller, AdvanceMs(9), modules::UserButton::Down, fxe::Button::Event::Press, settings, overlay, blockedCtx, grid);
    CheckEqBool(settings.ReadValue("BOARD_ROTATE", false), true, "BOARD_ROTATE unchanged when gated off");
    Check(controller.ConsumeNotAtStartingPositionFlashRequested(), "blocked rotate requests piece flash");

    // First player gating also requests a flash when blocked by position.
    // Navigate to GAME_FIRST_PLAYER and attempt to toggle.
    controller.OpenWithOverlay(modules::UiOwner::P1, settings, overlay, overlayCtx, grid);
    while (controller.GetSelectedOptionIndex() != modules::menu_option_indices::GAME_FIRST_PLAYER) {
        Send(controller, AdvanceMs(10), modules::UserButton::Left, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid);
    }

    modules::MenuOverlayContext blockedFirstPlayerCtx = overlayCtx;
    blockedFirstPlayerCtx.firstPlayerToggleAllowed = false;
    blockedFirstPlayerCtx.firstPlayerToggleBlockReason = modules::MenuToggleBlockReason::Position;
    const int beforeFirstPlayer = settings.ReadValue("GAME_FIRST_PLAYER", static_cast<int32_t>(1));
    Send(controller, AdvanceMs(10), modules::UserButton::Up, fxe::Button::Event::Press, settings, overlay, blockedFirstPlayerCtx, grid);
    CheckEqInt(settings.ReadValue("GAME_FIRST_PLAYER", static_cast<int32_t>(1)),
               beforeFirstPlayer,
               "GAME_FIRST_PLAYER unchanged when gated off");
    Check(controller.ConsumeNotAtStartingPositionFlashRequested(), "blocked first player requests piece flash");

    // Move back to VS_CPU (clamps at ends).
    for (int i = 0; i < 10; ++i) {
        Send(controller, AdvanceMs(10), modules::UserButton::Left, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid);
    }
    CheckEqInt(controller.GetSelectedOptionIndex(), modules::menu_option_indices::VS_CPU, "clamped back to VS_CPU");

    // Selection change shows icon first, then value.
    fxe::ManualMillis::SetNowMs(AdvanceMs(10));
    controller.Tick();
    overlay.Compose(grid, grid);
    fxe::ManualMillis::SetNowMs(AdvanceMs(50));
    controller.Tick();
    overlay.Compose(grid, grid);

    Send(controller, AdvanceMs(10), modules::UserButton::Up, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid);
    const int vsCpu = static_cast<int>(settings.ReadValue("VS_CPU_LEVEL", static_cast<int32_t>(0)));
    CheckEqInt(vsCpu, 1, "VS_CPU_LEVEL increments to 1");
    CheckEqInt(overlay.Test_GetLastVsCpuLevel(), 1, "VS CPU overlay last level=1");
    CheckEqStr(overlay.Test_GetLastText(), "1", "VS CPU overlay text '1'");

    const uint32_t cpuCancelBefore = control.cpuCancelCount;
    Send(controller, AdvanceMs(10), modules::UserButton::Up,
         fxe::Button::Event::Press, settings, overlay, overlayCtx, grid,
         &control.service);
    Check(control.cpuCancelCount == cpuCancelBefore + 1U,
          "service-backed VS_CPU menu writes preserve targeted CPU cancel");

    // Edge LED intent reflects menu state + selection.
    modules::EdgeLedIntent intent;
    fxe::ManualMillis::SetNowMs(AdvanceMs(10));
    controller.PopulateEdgeIntent(settings, intent);
    Check(intent.menuActive, "EdgeLedIntent.menuActive true");
    Check(intent.menuOwner == modules::UiOwner::P1, "EdgeLedIntent.menuOwner=P1");
    CheckEqInt(intent.menuSelectionIndex, modules::menu_option_indices::VS_CPU, "VS_CPU option => selection index is option index");

    // BRIGHTNESS option adjusts and shows value overlay.
    Send(controller, AdvanceMs(10), modules::UserButton::Right, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid); // BRIGHTNESS
    CheckEqInt(controller.GetSelectedOptionIndex(), modules::menu_option_indices::BRIGHTNESS, "selection at BRIGHTNESS option");

    Send(controller, AdvanceMs(10), modules::UserButton::Up, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid);
    const int brightness = static_cast<int>(settings.ReadValue("BRIGHTNESS", static_cast<int32_t>(0)));
    CheckEqInt(brightness, 6, "BRIGHTNESS increments from default 5 to 6");
    CheckEqStr(overlay.Test_GetLastText(), "6", "BRIGHTNESS overlay shows new level '6'");

    // Hold-to-confirm new game via P1O while on VS_CPU. Cancel on release.
    Send(controller, AdvanceMs(10), modules::UserButton::Left, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid); // VS_CPU
    CheckEqInt(controller.GetSelectedOptionIndex(), modules::menu_option_indices::VS_CPU, "selection at VS_CPU");

    Send(controller, AdvanceMs(100), modules::UserButton::P1O, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid);

    modules::EdgeLedIntent holdIntent;
    fxe::ManualMillis::SetNowMs(AdvanceMs(50));
    controller.PopulateEdgeIntent(settings, holdIntent);
    Check(holdIntent.menuValue01_255 != 0, "hold progress begins (menuValue01_255 nonzero)");
    Check(holdIntent.menuHoldActive, "hold progress sets menuHoldActive");
    Check(holdIntent.menuHoldFillAll, "hold progress uses fill-all mode");

    fxe::ManualMillis::SetNowMs(AdvanceMs(350));
    controller.Tick();
    Send(controller, AdvanceMs(100), modules::UserButton::P1O, fxe::Button::Event::Release, settings, overlay, overlayCtx, grid);
    fxe::ManualMillis::SetNowMs(AdvanceMs(1100));
    controller.Tick();
    Check(!controller.ConsumeNewGameRequested(), "hold cancelled by release");

    // Successful hold.
    CheckEqInt(controller.GetSelectedOptionIndex(), modules::menu_option_indices::VS_CPU, "still at VS_CPU for successful hold");
    Send(controller, AdvanceMs(1000), modules::UserButton::P1O, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid);

    modules::EdgeLedIntent holdIntent2;
    fxe::ManualMillis::SetNowMs(AdvanceMs(500));
    controller.PopulateEdgeIntent(settings, holdIntent2);
    Check(holdIntent2.menuValue01_255 != 0, "hold progress reflected in edge intent");
    Check(holdIntent2.menuHoldActive, "hold progress reflected in menuHoldActive");

    fxe::ManualMillis::SetNowMs(AdvanceMs(2000));
    controller.Tick();
    Check(controller.ConsumeNewGameRequested(), "hold triggers new game request");

    // Idle close.
    fxe::ManualMillis::SetNowMs(AdvanceMs(200));
    controller.OpenWithOverlay(modules::UiOwner::P1, settings, overlay, overlayCtx, grid);
    Check(controller.IsActive(), "menu active after re-open");
    fxe::ManualMillis::SetNowMs(AdvanceMs(6000));
    controller.Tick();
    Check(!controller.IsActive(), "menu closes after idle timeout");
    Check(controller.IsVisualActive(), "idle close starts visual fade");

    // Owner X exits immediately.
    fxe::ManualMillis::SetNowMs(AdvanceMs(100));
    controller.OpenWithOverlay(modules::UiOwner::P1, settings, overlay, overlayCtx, grid);
    Check(controller.IsActive(), "menu active before exit");
    Send(controller, AdvanceMs(10), modules::UserButton::P1X, fxe::Button::Event::Press, settings, overlay, overlayCtx, grid);
    Check(!controller.IsActive(), "menu exits via owner X");

    (void)std::remove(storeCfg.path);

    if (g_failures == 0) {
        std::puts("[MENU_NAVIGATION] ok");
        return 0;
    }

    std::printf("[MENU_NAVIGATION] failures=%d\n", g_failures);
    return 1;
}
