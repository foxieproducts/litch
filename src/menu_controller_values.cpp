#include "modules/menu_controller.hpp"

#include <algorithm>

#include <ArduinoJson.h>

#include "modules/control_mutation_service.hpp"

namespace modules {

namespace {

int ReadIntSetting(const SettingsStore& settings, const char* key, const int defaultValue) {
    return static_cast<int>(settings.ReadValue(key, static_cast<int32_t>(defaultValue)));
}

void WriteInt(SettingsStore& settings, const char* key, const int value) {
    if (key == nullptr) {
        return;
    }

    settings.Doc()[key] = static_cast<int32_t>(value);
}

bool ReadBoolSetting(const SettingsStore& settings, const char* key, const bool defaultValue) {
    return settings.ReadValue(key, defaultValue);
}

void WriteBool(SettingsStore& settings, const char* key, const bool value) {
    if (key == nullptr) {
        return;
    }

    settings.Doc()[key] = value;
}

bool MutateMenuBool(ControlMutationService* const controlMutation,
                    const char* const key,
                    const bool value,
                    ControlMutationOutcome& outOutcome) {
    if (controlMutation == nullptr) {
        return false;
    }
    StaticJsonDocument<16> doc;
    doc.set(value);
    outOutcome = controlMutation->MutateSetting(key, doc.as<JsonVariantConst>());
    return true;
}

bool MutateMenuU32(ControlMutationService* const controlMutation,
                   const char* const key,
                   const uint32_t value,
                   ControlMutationOutcome& outOutcome) {
    if (controlMutation == nullptr) {
        return false;
    }
    StaticJsonDocument<16> doc;
    doc.set(value);
    outOutcome = controlMutation->MutateSetting(key, doc.as<JsonVariantConst>());
    return true;
}

std::string_view BlockReasonToText(const MenuToggleBlockReason reason) {
    switch (reason) {
        case MenuToggleBlockReason::Busy:
            return "BUS";
        case MenuToggleBlockReason::Game:
            return "GAM";
        case MenuToggleBlockReason::Position:
            return "POS";
        case MenuToggleBlockReason::Swap:
            return "SWP";
        case MenuToggleBlockReason::None:
        default:
            return "NO";
    }
}

} // namespace

int MenuController::ReadValueForSelectedOption(const SettingsStore& settings) const {
    switch (m_selectedOptionIndex) {
        case menu_option_indices::VS_CPU:
            return ReadIntSetting(settings, KEY_VS_CPU_LEVEL, 0);
        case menu_option_indices::BRIGHTNESS:
            return ReadIntSetting(settings, KEY_BRIGHTNESS, 5);
        case menu_option_indices::FLIP_SIDES:
            return ReadBoolSetting(settings, "BOARD_ROTATE", false) ? 1 : 0;
        case menu_option_indices::GAME_FIRST_PLAYER: {
            const int value = settings.ReadValue("GAME_FIRST_PLAYER", static_cast<int32_t>(1));
            return (value == 2) ? 2 : 1;
        }
        default:
            return 0;
    }
}

bool MenuController::TryAdjustValue(const int delta,
                                    SettingsStore& settings,
                                    MenuLevelOverlay& overlay,
                                    const MenuOverlayContext& overlayCtx,
                                    const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentGrid,
                                    ControlMutationService* const controlMutation) {
    overlay.SetOwner(m_owner);
    const int cur = ReadValueForSelectedOption(settings);
    int next = cur;

    switch (m_selectedOptionIndex) {
        case menu_option_indices::VS_CPU:
            next = std::clamp(cur + delta, 0, 11);
            break;
        case menu_option_indices::BRIGHTNESS:
            next = std::clamp(cur + delta, 1, 8);
            break;
        case menu_option_indices::FLIP_SIDES:
            if (delta == 0) {
                return false;
            }

            if (!overlayCtx.flipSidesToggleAllowed) {
                if (overlayCtx.flipSidesToggleBlockReason == MenuToggleBlockReason::Position) {
                    m_notAtStartingPositionFlashRequested = true;
                }
                overlay.ShowMessage(BlockReasonToText(overlayCtx.flipSidesToggleBlockReason), RGB_WHITE, currentGrid);
                return false;
            }

            next = (cur == 0) ? 1 : 0;
            break;
        case menu_option_indices::GAME_FIRST_PLAYER:
            if (delta == 0) {
                return false;
            }

            if (!overlayCtx.firstPlayerToggleAllowed) {
                if (overlayCtx.firstPlayerToggleBlockReason == MenuToggleBlockReason::Position) {
                    m_notAtStartingPositionFlashRequested = true;
                }
                overlay.ShowMessage(BlockReasonToText(overlayCtx.firstPlayerToggleBlockReason), RGB_WHITE, currentGrid);
                return false;
            }

            next = (cur == 1) ? 2 : 1;
            break;
        default:
            return false;
    }

    if (next == cur) {
        return false;
    }

    return WriteValueForSelectedOption(
        settings, overlay, overlayCtx, currentGrid, next, controlMutation);
}

bool MenuController::WriteValueForSelectedOption(SettingsStore& settings,
                                                 MenuLevelOverlay& overlay,
                                                 const MenuOverlayContext& overlayCtx,
                                                 const std::array<Rgb, led_text_renderer::GRID_PIXELS>& currentGrid,
                                                 const int value,
                                                 ControlMutationService* const controlMutation) {
    overlay.SetOwner(m_owner);
    int writtenValue = value;
    if (controlMutation != nullptr) {
        ControlMutationOutcome outcome;
        bool handledByService = true;
        switch (m_selectedOptionIndex) {
            case menu_option_indices::VS_CPU:
                handledByService = MutateMenuU32(
                    controlMutation, KEY_VS_CPU_LEVEL,
                    static_cast<uint32_t>(std::clamp(value, 0, 11)), outcome);
                break;
            case menu_option_indices::BRIGHTNESS:
                handledByService = MutateMenuU32(
                    controlMutation, KEY_BRIGHTNESS,
                    static_cast<uint32_t>(std::clamp(value, 1, 8)), outcome);
                break;
            case menu_option_indices::FLIP_SIDES:
                handledByService = MutateMenuBool(
                    controlMutation, "BOARD_ROTATE", value != 0, outcome);
                break;
            case menu_option_indices::GAME_FIRST_PLAYER:
                writtenValue = std::clamp(value, 1, 2);
                handledByService = MutateMenuU32(
                    controlMutation, "GAME_FIRST_PLAYER",
                    static_cast<uint32_t>(writtenValue), outcome);
                break;
            default:
                handledByService = false;
                break;
        }

        if (handledByService) {
            if (outcome.status == ControlMutationStatus::Rejected) {
                MenuToggleBlockReason blockReason = MenuToggleBlockReason::None;
                if (outcome.reason == ControlMutationReason::NotAtStart) {
                    m_notAtStartingPositionFlashRequested = true;
                    blockReason = MenuToggleBlockReason::Position;
                } else if (outcome.reason == ControlMutationReason::Busy) {
                    blockReason = MenuToggleBlockReason::Busy;
                }
                if (blockReason != MenuToggleBlockReason::None) {
                    overlay.ShowMessage(BlockReasonToText(blockReason),
                                        RGB_WHITE, currentGrid);
                }
                return false;
            }

            writtenValue = ReadValueForSelectedOption(settings);
            if (m_selectedOptionIndex == menu_option_indices::FLIP_SIDES ||
                m_selectedOptionIndex ==
                    menu_option_indices::GAME_FIRST_PLAYER) {
                overlay.OnMenuValueChanged(m_selectedOptionIndex,
                                           writtenValue,
                                           currentGrid,
                                           overlayCtx.uptimeMs,
                                           overlayCtx.settingsLoaded,
                                           overlayCtx.fwVersion);
            } else {
                overlay.OnMenuOptionChanged(m_selectedOptionIndex,
                                            writtenValue,
                                            currentGrid,
                                            overlayCtx.uptimeMs,
                                            overlayCtx.settingsLoaded,
                                            overlayCtx.fwVersion);
            }
            m_idleElapsed.Reset();
            return true;
        }
    }

    switch (m_selectedOptionIndex) {
        case menu_option_indices::VS_CPU:
            WriteInt(settings, KEY_VS_CPU_LEVEL, value);
            break;
        case menu_option_indices::BRIGHTNESS:
            WriteInt(settings, KEY_BRIGHTNESS, value);
            break;
        case menu_option_indices::FLIP_SIDES:
            WriteBool(settings, "BOARD_ROTATE", value != 0);
            settings.MarkDirty();
            overlay.OnMenuValueChanged(m_selectedOptionIndex,
                                       value,
                                       currentGrid,
                                       overlayCtx.uptimeMs,
                                       overlayCtx.settingsLoaded,
                                       overlayCtx.fwVersion);
            m_idleElapsed.Reset();
            return true;
        case menu_option_indices::GAME_FIRST_PLAYER:
            writtenValue = std::clamp(value, 1, 2);
            settings.Doc()["GAME_FIRST_PLAYER"] = static_cast<uint32_t>((writtenValue == 2) ? 2U : 1U);
            (void)settings.Doc().remove("GAME_P1_PLAYS_BLACK");
            (void)settings.Doc().remove("p1_plays_black");
            settings.MarkDirty();
            overlay.OnMenuValueChanged(m_selectedOptionIndex,
                                       writtenValue,
                                       currentGrid,
                                       overlayCtx.uptimeMs,
                                       overlayCtx.settingsLoaded,
                                       overlayCtx.fwVersion);
            m_idleElapsed.Reset();
            return true;
        default:
            return false;
    }

    settings.MarkDirty();
    overlay.OnMenuOptionChanged(m_selectedOptionIndex,
                                writtenValue,
                                currentGrid,
                                overlayCtx.uptimeMs,
                                overlayCtx.settingsLoaded,
                                overlayCtx.fwVersion);
    m_idleElapsed.Reset();
    return true;
}

} // namespace modules
