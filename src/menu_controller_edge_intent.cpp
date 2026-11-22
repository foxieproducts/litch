#include "modules/menu_controller.hpp"

namespace modules {

namespace {

int ClampInt(const int v, const int lo, const int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

} // namespace

uint8_t MenuController::ComputeMenuValue01_255(const uint8_t optionIndex, const int value) {
    auto clamp255 = [](const int v) {
        return static_cast<uint8_t>((v < 0) ? 0 : ((v > 255) ? 255 : v));
    };

    switch (optionIndex) {
        case menu_option_indices::VS_CPU: {
            const int clamped = ClampInt(value, 0, 11);
            return clamp255((clamped * 255 + 5) / 11);
        }
        case menu_option_indices::BRIGHTNESS: {
            const int clamped = ClampInt(value, 1, 8);
            return clamp255(((clamped - 1) * 255 + 3) / 7);
        }
        case menu_option_indices::FLIP_SIDES:
            return value != 0 ? 255 : 0;
        case menu_option_indices::GAME_FIRST_PLAYER:
            return value == 2 ? 255 : 0;
        default:
            return 0;
    }
}

void MenuController::PopulateEdgeIntent(const SettingsStore& settings, EdgeLedIntent& outIntent) const {
    const bool visualActive = IsVisualActive();
    const UiOwner owner = m_active ? m_owner : m_closeFadeOwner;
    const uint8_t selection = m_active ? m_selectedOptionIndex : m_closeFadeSelectedOptionIndex;

    if (!visualActive || owner == UiOwner::None) {
        return;
    }

    outIntent.menuActive = true;
    outIntent.menuOwner = owner;
    // Selection index is the 0-based menu option index.
    outIntent.menuSelectionIndex = selection;

    if (m_active && m_holdActive &&
        (m_selectedOptionIndex == menu_option_indices::VS_CPU ||
         m_selectedOptionIndex == menu_option_indices::SLEEP)) {
        outIntent.menuHoldActive = true;
        outIntent.menuHoldFillAll = m_holdFillAll;
        if (m_config.holdToConfirmMs == 0) {
            outIntent.menuValue01_255 = 255;
            return;
        }

        const uint32_t elapsed = m_holdElapsed.ElapsedMs();
        if (elapsed >= m_config.holdToConfirmMs) {
            outIntent.menuValue01_255 = 255;
            return;
        }

        const uint32_t scaled = (elapsed * 255U) / m_config.holdToConfirmMs;
        outIntent.menuValue01_255 = (scaled > 255U) ? 255U : static_cast<uint8_t>(scaled);
        return;
    }

    // During the close fade-out, treat hold progress as inactive and show the
    // steady value for the last selected option.
    const auto ReadValueForOption = [&](const uint8_t optionIndex) -> int {
        switch (optionIndex) {
            case menu_option_indices::VS_CPU:
                return static_cast<int>(settings.ReadValue(KEY_VS_CPU_LEVEL, static_cast<int32_t>(0)));
            case menu_option_indices::BRIGHTNESS:
                return static_cast<int>(settings.ReadValue(KEY_BRIGHTNESS, static_cast<int32_t>(5)));
            case menu_option_indices::FLIP_SIDES:
                return settings.ReadValue("BOARD_ROTATE", false) ? 1 : 0;
            case menu_option_indices::GAME_FIRST_PLAYER: {
                const int v = settings.ReadValue("GAME_FIRST_PLAYER", static_cast<int32_t>(1));
                return (v == 2) ? 2 : 1;
            }
            default:
                return 0;
        }
    };

    const int value = ReadValueForOption(selection);
    outIntent.menuValue01_255 = ComputeMenuValue01_255(selection, value);
}

} // namespace modules
