#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/edge_led_intent.hpp"
#include "modules/menu_level_overlay.hpp"
#include "modules/menu_option_indices.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "modules/ui_types.hpp"
#include "modules/user_input.hpp"

namespace modules {

class ControlMutationService;

enum class MenuToggleBlockReason : uint8_t {
    None = 0,
    Busy,
    Game,
    Position,
    Swap,
};

struct MenuOverlayContext {
    uint32_t uptimeMs = 0;
    bool settingsLoaded = true;
    std::string_view fwVersion;

    // If false, the flip-sides menu action must not toggle the setting.
    // This is expected to match the console flip command gating policy.
    bool flipSidesToggleAllowed = true;
    MenuToggleBlockReason flipSidesToggleBlockReason = MenuToggleBlockReason::None;

    // If false, the GAME_FIRST_PLAYER menu action must not toggle the setting.
    // This is expected to match the console first-player command gating policy.
    bool firstPlayerToggleAllowed = true;
    MenuToggleBlockReason firstPlayerToggleBlockReason = MenuToggleBlockReason::None;
};

struct MenuControllerConfig {
    // Hold duration for confirming new/reset game (part of VS_CPU option).
    uint32_t holdToConfirmMs = 2000;

    // Legacy hold-to-open duration (menu now opens via O toggle).
    uint32_t holdToOpenMenuMs = 1000;

    // While active, close the menu after this long without any input.
    uint32_t idleCloseMs = 5000;

    // When the menu closes (X press or idle timeout), keep the menu lighting
    // visible briefly and fade it out smoothly.
    uint32_t closeFadeOutMs = 200;

    // If true, selection wraps at ends. Prototype wants no wrap.
    bool wrapNavigation = false;

    // If true, P? O (or Press+X chord) toggles the menu open/close.
    bool bTapTogglesMenu = false;

    // Menu option index range (inclusive). Matches docs/spec/interaction_model_edge_leds.md.
    uint8_t minOptionIndex = 0;
    uint8_t maxOptionIndex = 4;

    // Time window where Press+P? chord counts as O.
    uint32_t chordWindowMs = 80;
};

class MenuController final {
  public:
    explicit MenuController(const MenuControllerConfig& config = {}) : m_config(config) {}

    const MenuControllerConfig& GetConfig() const { return m_config; }

    void Reset();

    bool IsActive() const { return m_active; }

    //
    // \brief Returns true while the menu lighting should still be visible.
    // This includes the normal active state and the short close fade-out.
    bool IsVisualActive() const;

    //
    // \brief Visual alpha in 0..255 for menu lighting.
    // 255 while active, and ramps down to 0 during close fade-out.
    uint8_t GetVisualAlpha255() const;
    UiOwner GetOwner() const { return m_owner; }
    uint8_t GetSelectedOptionIndex() const { return m_selectedOptionIndex; }

    // \brief Opens the menu and triggers an initial selection overlay.
    void OpenWithOverlay(UiOwner owner,
                         const SettingsStore& settings,
                         MenuLevelOverlay& overlay,
                         const MenuOverlayContext& overlayCtx,
                         const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentGrid);

    // \brief Feed a raw (post-press-gate) button event. Returns true if consumed.
    bool HandleButtonEvent(UserButton button,
                           fxe::Button::Event event,
                           SettingsStore& settings,
                           MenuLevelOverlay& overlay,
                           const MenuOverlayContext& overlayCtx,
                           const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentGrid,
                           ControlMutationService* controlMutation = nullptr);

    // \brief Drive time-based behaviors (e.g. hold-to-confirm). Call once per tick.
    void Tick();

    bool ConsumeNewGameRequested();

    // \brief Set when the owner confirms Sleep on the Sleep option.
    bool ConsumeSleepRequested();

    // \brief Set when the non-owner taps O during owned menu UI.
    bool ConsumeExitMenuRequested();

    // \brief UX hook: true if a menu toggle was blocked because the game is not
    // at the starting position. Intended to trigger a short whole-board cue.
    bool ConsumeNotAtStartingPositionFlashRequested();

    void PopulateEdgeIntent(const SettingsStore& settings, EdgeLedIntent& outIntent) const;

  private:
    enum class HoldAction : uint8_t {
        None = 0,
        NewGame,
        Sleep,
    };

    static constexpr const char* KEY_VS_CPU_LEVEL = "VS_CPU_LEVEL";
    static constexpr const char* KEY_BRIGHTNESS = "BRIGHTNESS";

    static UiOwner OwnerFromXButton(UserButton button);

    void Open(UiOwner owner);
    void Close();

    bool HandleOChord(UserButton button, fxe::Button::Event event);

    bool IsOwnerButton(UserButton button) const;
    bool IsNavigationButton(UserButton button) const;

    void StepSelection(int delta);

    bool TryAdjustValue(int delta,
                        SettingsStore& settings,
                        MenuLevelOverlay& overlay,
                        const MenuOverlayContext& overlayCtx,
                        const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentGrid,
                        ControlMutationService* controlMutation);

    int ReadValueForSelectedOption(const SettingsStore& settings) const;
    bool WriteValueForSelectedOption(SettingsStore& settings,
                                     MenuLevelOverlay& overlay,
                                     const MenuOverlayContext& overlayCtx,
                                     const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentGrid,
                                     int value,
                                     ControlMutationService* controlMutation);

    static uint8_t ComputeMenuValue01_255(uint8_t optionIndex, int value);

    MenuControllerConfig m_config;

    bool m_active = false;
    UiOwner m_owner = UiOwner::None;
    uint8_t m_selectedOptionIndex = 0;

    // O chord tracking.

    // Hold-to-confirm tracking (currently used for VS_CPU new/reset).
    bool m_holdActive = false;
    fxe::ElapsedTime m_holdElapsed;
    HoldAction m_holdAction = HoldAction::None;

    bool m_newGameRequested = false;

    bool m_sleepRequested = false;

    bool m_exitMenuRequested = false;

    bool m_notAtStartingPositionFlashRequested = false;

    fxe::ElapsedTime m_idleElapsed;
    bool m_holdFillAll = false;

    // Close fade-out state.
    bool m_closeFadeActive = false;
    fxe::ElapsedTime m_closeFadeElapsed;

    // Snapshot menu ownership/selection at close so edge/menu rendering can
    // fade out cleanly even after the active state clears.
    UiOwner m_closeFadeOwner = UiOwner::None;
    uint8_t m_closeFadeSelectedOptionIndex = 0;
};

} // namespace modules
