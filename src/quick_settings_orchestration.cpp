#include "modules/quick_settings_orchestration.hpp"

#include <algorithm>

#include <ArduinoJson.h>

#include "modules/menu_option_indices.hpp"
#include "modules/quick_settings_items.hpp"
#include "modules/white_squares_setting.hpp"

namespace modules {
namespace {

bool MutationSucceeded(const ControlMutationOutcome& outcome) {
    return outcome.status == ControlMutationStatus::Applied ||
           outcome.status == ControlMutationStatus::Noop;
}

ControlMutationOutcome MutateBool(
    QuickSettingsOrchestrationContext const& context,
    const char* key,
    const bool value) {
    StaticJsonDocument<16> document;
    document.set(value);
    return context.controlMutation.MutateSetting(
        key, document.as<JsonVariantConst>());
}

ControlMutationOutcome MutateU32(
    QuickSettingsOrchestrationContext const& context,
    const char* key,
    const uint32_t value) {
    StaticJsonDocument<16> document;
    document.set(value);
    return context.controlMutation.MutateSetting(
        key, document.as<JsonVariantConst>());
}

uint32_t ReadVsCpuLevel(const SettingsStore& settings) {
    return std::min<uint32_t>(
        settings.ReadValue("VS_CPU_LEVEL", static_cast<uint32_t>(0U)),
        11U);
}

bool ReadCpuPlayerIsP2(const SettingsStore& settings) {
    return settings.ReadValue(
               "VS_CPU_PLAYER", static_cast<uint32_t>(2U)) == 2U;
}

bool ReadFirstPlayerIsP2(const SettingsStore& settings) {
    return settings.ReadValue(
               "GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U;
}

uint32_t ReadBrightness(const SettingsStore& settings) {
    const uint32_t value =
        settings.ReadValue("BRIGHTNESS", static_cast<uint32_t>(5U));
    return std::clamp<uint32_t>(value, 0U, 8U);
}

bool ReadBoardRotate(const SettingsStore& settings) {
    return settings.ReadValue("BOARD_ROTATE", false);
}

uint32_t ReadTimer(const SettingsStore& settings) {
    return std::min<uint32_t>(
        settings.ReadValue("TIMER", static_cast<uint32_t>(0U)), 6U);
}

void ShowMenuSelection(const QuickSettingsOrchestrationContext& context,
                       const uint8_t optionIndex,
                       const int value) {
    context.overlay.OnMenuSelectionChanged(
        optionIndex,
        value,
        context.background,
        context.nowMs,
        context.settingsLoaded,
        context.firmwareVersion);
}

void ShowMenuValue(const QuickSettingsOrchestrationContext& context,
                   const uint8_t optionIndex,
                   const int value) {
    context.overlay.OnMenuValueChanged(
        optionIndex,
        value,
        context.background,
        context.nowMs,
        context.settingsLoaded,
        context.firmwareVersion);
}

} // namespace

QuickSettingsBlockedFeedback ClassifyQuickSettingsBlockedFeedback(
    const ControlMutationReason reason) {
    if (reason == ControlMutationReason::NotAtStart) {
        return QuickSettingsBlockedFeedback{
            true, true, EdgeLedCueKind::OccupiedBoardBlocked};
    }
    if (reason == ControlMutationReason::Busy) {
        return QuickSettingsBlockedFeedback{
            true, false, EdgeLedCueKind::Unavailable};
    }
    return QuickSettingsBlockedFeedback{};
}

void ShowQuickSettingsSelection(
    const QuickSettingsOrchestrationContext& context) {
    context.overlay.SetOwner(context.owner);
    switch (context.selectedIndex) {
        case quick_settings_items::CPU_DIFFICULTY:
            ShowMenuSelection(
                context,
                menu_option_indices::VS_CPU,
                static_cast<int>(ReadVsCpuLevel(context.settings)));
            return;
        case quick_settings_items::CPU_PLAYER:
            ShowMenuSelection(
                context,
                menu_option_indices::CPU_PLAYER,
                ReadCpuPlayerIsP2(context.settings) ? 2 : 1);
            return;
        case quick_settings_items::FIRST_PLAYER:
            ShowMenuSelection(
                context,
                menu_option_indices::GAME_FIRST_PLAYER,
                ReadFirstPlayerIsP2(context.settings) ? 2 : 1);
            return;
        case quick_settings_items::BRIGHTNESS:
            ShowMenuSelection(
                context,
                menu_option_indices::BRIGHTNESS,
                static_cast<int>(ReadBrightness(context.settings)));
            return;
        case quick_settings_items::WHITE_SQUARES:
            context.overlay.Stop();
            return;
        case quick_settings_items::OWNER_PAIRING: {
            MenuLevelOverlay::IconPixels pixels{};
            if (menu_icons_6x6::ParseIcon(menu_icons_6x6::BLUETOOTH_ICON, pixels))
                context.overlay.ShowPinnedIcon(pixels, context.background);
            return;
        }
        case quick_settings_items::ROTATE:
            ShowMenuSelection(
                context,
                menu_option_indices::FLIP_SIDES,
                ReadBoardRotate(context.settings) ? 1 : 0);
            return;
        case quick_settings_items::TIMER:
            ShowMenuSelection(
                context,
                menu_option_indices::TIMER,
                static_cast<int>(ReadTimer(context.settings)));
            return;
        default:
            return;
    }
}

void RefreshQuickSettingsCpuDifficulty(
    const SettingsStore& settings, MenuLevelOverlay& overlay,
    const std::array<Rgb, led_text_renderer::GRID_PIXELS>& background) {
    overlay.RefreshVsCpuLevel(static_cast<int>(ReadVsCpuLevel(settings)), background);
}

QuickSettingsMutationResult ApplyQuickSettingsAdjustment(
    const QuickSettingsOrchestrationContext& context,
    const QuickSettingsAction& action) {
    QuickSettingsMutationResult result;
    const int delta =
        action.type == QuickSettingsActionType::AdjustIncrease ? 1 : -1;
    context.overlay.SetOwner(context.owner);

    switch (context.selectedIndex) {
        case quick_settings_items::CPU_DIFFICULTY: {
            result.handled = true;
            const uint32_t current = ReadVsCpuLevel(context.settings);
            const uint32_t next = static_cast<uint32_t>(std::clamp(
                static_cast<int>(current) + delta, 0, 11));
            if (next != current) {
                result.outcome = MutateU32(
                    context, "VS_CPU_LEVEL", next);
                result.hasOutcome = true;
            }
            context.overlay.OnVsCpuLevelChanged(
                static_cast<int>(ReadVsCpuLevel(context.settings)),
                context.background);
            return result;
        }
        case quick_settings_items::CPU_PLAYER: {
            result.handled = true;
            if (action.isRepeat) {
                return result;
            }
            result.outcome = MutateU32(
                context,
                "VS_CPU_PLAYER",
                ReadCpuPlayerIsP2(context.settings) ? 1U : 2U);
            result.hasOutcome = true;
            if (MutationSucceeded(result.outcome)) {
                ShowMenuValue(
                    context,
                    menu_option_indices::CPU_PLAYER,
                    ReadCpuPlayerIsP2(context.settings) ? 2 : 1);
            }
            return result;
        }
        case quick_settings_items::FIRST_PLAYER: {
            result.handled = true;
            result.showBlockedFeedback = true;
            if (action.isRepeat) {
                return result;
            }
            result.outcome = MutateU32(
                context,
                "GAME_FIRST_PLAYER",
                ReadFirstPlayerIsP2(context.settings) ? 1U : 2U);
            result.hasOutcome = true;
            if (MutationSucceeded(result.outcome)) {
                ShowMenuValue(
                    context,
                    menu_option_indices::GAME_FIRST_PLAYER,
                    ReadFirstPlayerIsP2(context.settings) ? 2 : 1);
            }
            return result;
        }
        case quick_settings_items::BRIGHTNESS: {
            result.handled = true;
            const uint32_t current = ReadBrightness(context.settings);
            const uint32_t next = static_cast<uint32_t>(std::clamp(
                static_cast<int>(current) + delta, 0, 8));
            if (next == current) {
                return result;
            }
            result.outcome = MutateU32(
                context, "BRIGHTNESS", next);
            result.hasOutcome = true;
            if (MutationSucceeded(result.outcome)) {
                ShowMenuValue(
                    context,
                    menu_option_indices::BRIGHTNESS,
                    static_cast<int>(ReadBrightness(context.settings)));
            }
            return result;
        }
        case quick_settings_items::WHITE_SQUARES:
            result.handled = true;
            if (!action.isRepeat) {
                result.outcome = MutateBool(
                    context,
                    "WHITE_SQUARES_ON",
                    !white_squares_setting::ReadWhiteSquaresOn(
                        context.settings));
                result.hasOutcome = true;
            }
            return result;
        case quick_settings_items::ROTATE:
            result.handled = true;
            result.showBlockedFeedback = true;
            if (action.isRepeat) {
                return result;
            }
            result.outcome = MutateBool(
                context,
                "BOARD_ROTATE",
                !ReadBoardRotate(context.settings));
            result.hasOutcome = true;
            if (MutationSucceeded(result.outcome)) {
                ShowMenuValue(
                    context,
                    menu_option_indices::FLIP_SIDES,
                    ReadBoardRotate(context.settings) ? 1 : 0);
            }
            return result;
        case quick_settings_items::TIMER: {
            result.handled = true;
            result.showBlockedFeedback = true;
            const uint32_t current = ReadTimer(context.settings);
            const uint32_t next = static_cast<uint32_t>(std::clamp(
                static_cast<int>(current) + delta, 0, 6));
            result.outcome = MutateU32(context, "TIMER", next);
            result.hasOutcome = true;
            if (MutationSucceeded(result.outcome)) {
                ShowMenuValue(
                    context,
                    menu_option_indices::TIMER,
                    static_cast<int>(ReadTimer(context.settings)));
            }
            return result;
        }
        default:
            return result;
    }
}

} // namespace modules
