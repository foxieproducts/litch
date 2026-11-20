#include "modules/menu_controller.hpp"

#include <algorithm>

#include "modules/control_mutation_service.hpp"

namespace modules {

void MenuController::Reset() {
    m_active = false;
    m_owner = UiOwner::None;
    m_selectedOptionIndex = m_config.minOptionIndex;


    m_holdActive = false;
    m_holdElapsed = fxe::ElapsedTime();
    m_holdAction = HoldAction::None;
    m_newGameRequested = false;
    m_sleepRequested = false;

    m_exitMenuRequested = false;

    m_idleElapsed = fxe::ElapsedTime();
    m_holdFillAll = false;

    m_closeFadeActive = false;
    m_closeFadeElapsed = fxe::ElapsedTime();
    m_closeFadeOwner = UiOwner::None;
    m_closeFadeSelectedOptionIndex = m_config.minOptionIndex;
}

bool MenuController::IsVisualActive() const {
    return m_active || (m_closeFadeActive && m_closeFadeOwner != UiOwner::None);
}

uint8_t MenuController::GetVisualAlpha255() const {
    if (m_active) {
        return 255U;
    }

    if (!m_closeFadeActive || m_closeFadeOwner == UiOwner::None) {
        return 0U;
    }

    const uint32_t d = m_config.closeFadeOutMs;
    if (d == 0U) {
        return 0U;
    }

    const uint32_t t = m_closeFadeElapsed.ElapsedMs();
    if (t >= d) {
        return 0U;
    }

    const uint32_t num = (d - t) * 255U;
    const uint32_t a = (num + (d / 2U)) / d;
    return (a > 255U) ? 255U : static_cast<uint8_t>(a);
}

UiOwner MenuController::OwnerFromXButton(const UserButton button) {
    switch (button) {
        case UserButton::P1X:
            return UiOwner::P1;
        case UserButton::P2X:
            return UiOwner::P2;
        default:
            return UiOwner::None;
    }
}

void MenuController::Open(const UiOwner owner) {
    m_active = true;
    m_owner = owner;
    if (m_selectedOptionIndex < m_config.minOptionIndex || m_selectedOptionIndex > m_config.maxOptionIndex) {
        m_selectedOptionIndex = m_config.minOptionIndex;
    }

    m_holdActive = false;
    m_holdElapsed = fxe::ElapsedTime();
    m_holdAction = HoldAction::None;
    m_holdFillAll = false;

    m_exitMenuRequested = false;
    m_sleepRequested = false;

    // Cancel any in-flight close fade when the menu is reopened.
    m_closeFadeActive = false;
    m_closeFadeElapsed = fxe::ElapsedTime();
    m_closeFadeOwner = UiOwner::None;

    m_idleElapsed.Reset();
}

void MenuController::Close() {
    // Snapshot menu ownership/selection for a short visual fade-out.
    m_closeFadeOwner = m_owner;
    m_closeFadeSelectedOptionIndex = m_selectedOptionIndex;
    if (m_config.closeFadeOutMs != 0U && m_closeFadeOwner != UiOwner::None) {
        m_closeFadeActive = true;
        m_closeFadeElapsed.Reset();
    } else {
        m_closeFadeActive = false;
        m_closeFadeElapsed = fxe::ElapsedTime();
    }

    m_active = false;
    m_owner = UiOwner::None;

    m_holdActive = false;
    m_holdElapsed = fxe::ElapsedTime();
    m_holdAction = HoldAction::None;
    m_holdFillAll = false;

    m_exitMenuRequested = false;

    m_idleElapsed = fxe::ElapsedTime();
}

void MenuController::OpenWithOverlay(const UiOwner owner,
                                     const SettingsStore& settings,
                                     MenuLevelOverlay& overlay,
                                     const MenuOverlayContext& overlayCtx,
                                     const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentGrid) {
    Open(owner);
    m_idleElapsed.Reset();

    overlay.SetOwner(owner);

    const int value = ReadValueForSelectedOption(settings);
    overlay.OnMenuSelectionChanged(m_selectedOptionIndex,
                                   value,
                                   currentGrid,
                                   overlayCtx.uptimeMs,
                                   overlayCtx.settingsLoaded,
                                   overlayCtx.fwVersion);
}

bool MenuController::HandleOChord(const UserButton button, const fxe::Button::Event event) {
    if (!m_config.bTapTogglesMenu) {
        return false;
    }

    // Preferred model: explicit O button per player.
    const UiOwner oOwner = (button == UserButton::P1O) ? UiOwner::P1 : ((button == UserButton::P2O) ? UiOwner::P2 : UiOwner::None);
    if (oOwner != UiOwner::None) {
        if (event != fxe::Button::Event::Press) {
            return true;
        }

        if (!m_active) {
            Open(oOwner);
        } else {
            // Ownership gate: non-owner can request exit, but owner controls final state.
            if (oOwner == m_owner) {
                Close();
            } else {
                m_exitMenuRequested = true;
            }
        }
        return true;
    }

    return false;
}

bool MenuController::IsOwnerButton(const UserButton button) const {
    if (!m_active || m_owner == UiOwner::None) {
        return false;
    }

    if (m_owner == UiOwner::P1) {
        return button == UserButton::P1X;
    }

    if (m_owner == UiOwner::P2) {
        return button == UserButton::P2X;
    }

    return false;
}

bool MenuController::IsNavigationButton(const UserButton button) const {
    return button == UserButton::Up || button == UserButton::Down || button == UserButton::Left || button == UserButton::Right;
}

void MenuController::StepSelection(const int delta) {
    const int lo = static_cast<int>(m_config.minOptionIndex);
    const int hi = static_cast<int>(m_config.maxOptionIndex);
    const int span = (hi >= lo) ? (hi - lo + 1) : 1;

    int idx = static_cast<int>(m_selectedOptionIndex);
    idx += delta;

    if (m_config.wrapNavigation) {
        // Wrap in [lo, hi].
        while (idx < lo) {
            idx += span;
        }
        while (idx > hi) {
            idx -= span;
        }
    } else {
        idx = std::clamp(idx, lo, hi);
    }

    m_selectedOptionIndex = static_cast<uint8_t>(idx);

    // Any selection change cancels in-flight hold.
    m_holdActive = false;
    m_holdElapsed = fxe::ElapsedTime();
    m_holdAction = HoldAction::None;
}

void MenuController::Tick() {
    if (!m_active && m_closeFadeActive) {
        const uint32_t d = m_config.closeFadeOutMs;
        if (d == 0U || m_closeFadeElapsed.HasElapsed(d)) {
            m_closeFadeActive = false;
            m_closeFadeOwner = UiOwner::None;
        }
    }

    if (m_active && m_config.idleCloseMs != 0 && m_idleElapsed.Started()) {
        if (m_idleElapsed.HasElapsed(m_config.idleCloseMs)) {
            Close();
            return;
        }
    }

    if (!m_holdActive) {
        return;
    }

    const auto CommitHoldAction = [this]() {
        if (m_holdAction == HoldAction::NewGame) {
            m_newGameRequested = true;
        } else if (m_holdAction == HoldAction::Sleep) {
            m_sleepRequested = true;
        }

        m_holdActive = false;
        m_holdAction = HoldAction::None;
        Close();
    };

    if (m_config.holdToConfirmMs == 0) {
        CommitHoldAction();
        return;
    }

    if (m_holdElapsed.HasElapsed(m_config.holdToConfirmMs)) {
        CommitHoldAction();
    }
}

bool MenuController::ConsumeNotAtStartingPositionFlashRequested() {
    const bool out = m_notAtStartingPositionFlashRequested;
    m_notAtStartingPositionFlashRequested = false;
    return out;
}

bool MenuController::ConsumeNewGameRequested() {
    if (!m_newGameRequested) {
        return false;
    }

    m_newGameRequested = false;
    return true;
}

bool MenuController::ConsumeSleepRequested() {
    if (!m_sleepRequested) {
        return false;
    }

    m_sleepRequested = false;
    return true;
}

bool MenuController::ConsumeExitMenuRequested() {
    if (!m_exitMenuRequested) {
        return false;
    }

    m_exitMenuRequested = false;
    return true;
}

bool MenuController::HandleButtonEvent(const UserButton button,
                                       const fxe::Button::Event event,
                                       SettingsStore& settings,
                                       MenuLevelOverlay& overlay,
                                       const MenuOverlayContext& overlayCtx,
                                       const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentGrid,
                                       ControlMutationService* const controlMutation) {
    const bool wasActive = m_active;
    const bool pressLike = (event == fxe::Button::Event::Press) || (event == fxe::Button::Event::Repeat);
    const bool toggleOnAdjust = (m_selectedOptionIndex == menu_option_indices::FLIP_SIDES) ||
                                (m_selectedOptionIndex == menu_option_indices::GAME_FIRST_PLAYER);

    // Always track and interpret the O chord even when menu is inactive.
    if (HandleOChord(button, event)) {
        // If this event opened the menu, show an initial overlay for the current selection.
        if (!wasActive && m_active && pressLike) {
            overlay.SetOwner(m_owner);
            const int value = ReadValueForSelectedOption(settings);
            overlay.OnMenuSelectionChanged(m_selectedOptionIndex,
                                           value,
                                           currentGrid,
                                           overlayCtx.uptimeMs,
                                           overlayCtx.settingsLoaded,
                                           overlayCtx.fwVersion);
        }
        return true;
    }

    if (!m_active) {
        return false;
    }

    // Any input while active resets idle close.
    m_idleElapsed.Reset();

    // Ignore non-press events for navigation.
    // Note: LongHold is emitted by the button manager (e.g. for hint UX) and must
    // not be treated as a release, otherwise it cancels hold-to-confirm actions.
    if (!pressLike) {
        if (event == fxe::Button::Event::Release) {
            // Cancel hold-to-confirm on O release.
            if (IsOButton(button) && m_holdActive) {
                m_holdActive = false;
                m_holdElapsed = fxe::ElapsedTime();
                m_holdAction = HoldAction::None;
                m_holdFillAll = false;
            }
        }
        return true;
    }

    // Menu should care about logical X/O/D-pad, not specific P1/P2 ids.
    if (IsXButton(button)) {
        Close();
        return true;
    }

    if (button == UserButton::Left) {
        StepSelection(-1);
        const int value = ReadValueForSelectedOption(settings);
        overlay.OnMenuSelectionChanged(m_selectedOptionIndex,
                                       value,
                                       currentGrid,
                                       overlayCtx.uptimeMs,
                                       overlayCtx.settingsLoaded,
                                       overlayCtx.fwVersion);
        return true;
    }

    if (button == UserButton::Right) {
        StepSelection(+1);
        const int value = ReadValueForSelectedOption(settings);
        overlay.OnMenuSelectionChanged(m_selectedOptionIndex,
                                       value,
                                       currentGrid,
                                       overlayCtx.uptimeMs,
                                       overlayCtx.settingsLoaded,
                                       overlayCtx.fwVersion);
        return true;
    }

    if (button == UserButton::Up) {
        // For boolean-style options, ignore auto-repeat so we don't toggle back and forth
        // when the user holds the button slightly too long.
        if (toggleOnAdjust && event == fxe::Button::Event::Repeat) {
            return true;
        }
        (void)TryAdjustValue(+1, settings, overlay, overlayCtx, currentGrid,
                             controlMutation);
        return true;
    }

    if (button == UserButton::Down) {
        if (toggleOnAdjust && event == fxe::Button::Event::Repeat) {
            return true;
        }
        (void)TryAdjustValue(-1, settings, overlay, overlayCtx, currentGrid,
                             controlMutation);
        return true;
    }

    // O acts as a "select" button in the menu.
    // Used for VS_CPU + SLEEP hold-to-confirm.
    if (IsOButton(button) &&
        (m_selectedOptionIndex == menu_option_indices::VS_CPU ||
         m_selectedOptionIndex == menu_option_indices::SLEEP)) {
        if (event == fxe::Button::Event::Press) {
            m_holdActive = true;
            m_holdElapsed.Reset();
            m_holdFillAll = true;
            m_holdAction = (m_selectedOptionIndex == menu_option_indices::SLEEP) ? HoldAction::Sleep : HoldAction::NewGame;
            m_idleElapsed.Reset();
        }
        return true;
    }

    // Non-owner button presses are ignored in menu.
    return true;
}

} // namespace modules
