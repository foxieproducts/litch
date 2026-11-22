#pragma once

#ifdef ARDUINO

#include <cstdint>

#include "firmware/firmware_internal.hpp"
#include "modules/quick_settings_items.hpp"

namespace firmware_internal {
void OpenBoardQuickSettings(FirmwareSystem& sys, modules::UiOwner owner, uint32_t nowMs);
void CloseBoardQuickSettings(FirmwareSystem& sys);
void HandleBoardQuickSettingsAction(FirmwareSystem& sys,
    const modules::QuickSettingsAction& action, uint32_t nowMs);
modules::ControlLedFeedbackInputs CaptureControlFeedback(FirmwareSystem& sys,
    const modules::led_frame_composer::ComposeInputs& presentation,
    bool identifyActive);
bool StartupPresentationComplete(const FirmwareSystem& sys);


struct ToggleGate {
    bool allowed = false;
    modules::MenuToggleBlockReason reason = modules::MenuToggleBlockReason::None;
};

inline constexpr uint32_t QUICK_SETTINGS_HOLD_MS = 1000U;

inline constexpr uint8_t QUICK_SETTINGS_ITEM_CPU_DIFFICULTY =
    modules::quick_settings_items::CPU_DIFFICULTY;
inline constexpr uint8_t QUICK_SETTINGS_ITEM_CPU_PLAYER =
    modules::quick_settings_items::CPU_PLAYER;
inline constexpr uint8_t QUICK_SETTINGS_ITEM_FIRST_PLAYER =
    modules::quick_settings_items::FIRST_PLAYER;
inline constexpr uint8_t QUICK_SETTINGS_ITEM_BRIGHTNESS =
    modules::quick_settings_items::BRIGHTNESS;
inline constexpr uint8_t QUICK_SETTINGS_ITEM_WHITE_SQUARES =
    modules::quick_settings_items::WHITE_SQUARES;
inline constexpr uint8_t QUICK_SETTINGS_ITEM_ROTATE =
    modules::quick_settings_items::ROTATE;
inline constexpr uint8_t QUICK_SETTINGS_ITEM_TIMER =
    modules::quick_settings_items::TIMER;
inline constexpr uint8_t QUICK_SETTINGS_ITEM_COUNT =
    modules::quick_settings_items::COUNT;

uint32_t ReadVsCpuLevel(const modules::SettingsStore& settings);
uint32_t ClampVsCpuLevel(int value);
void WriteVsCpuLevel(modules::SettingsStore& settings, uint32_t level);

uint32_t ReadQuickBrightness(const modules::SettingsStore& settings);
bool ReadWhiteSquaresOn(const modules::SettingsStore& settings);
void WriteWhiteSquaresOn(modules::SettingsStore& settings, bool whiteSquaresOn);

uint8_t ComputeMenuValue01_255ForQuickSettings(uint8_t optionIndex, int value);

const char* UserButtonToString(modules::UserButton button);
const char* ButtonEventToString(fxe::Button::Event event);

ToggleGate EvaluateFlipSidesToggleGate(const FirmwareSystem& sys);
ToggleGate EvaluateFirstPlayerToggleGate(const FirmwareSystem& sys);

void ShowQuickSettingsSelection(FirmwareSystem& sys, uint32_t nowMs);
uint8_t QuickSettingsItemCount(const FirmwareSystem& sys);
void SyncOwnerPairingControls(FirmwareSystem& sys);
void HandleQuickSettingsAdjust(FirmwareSystem& sys,
                               const modules::QuickSettingsAction& action,
                               uint32_t nowMs);

uint8_t GetBrightnessLevel0To8(const modules::SettingsStore& settings);

} // namespace firmware_internal

#endif
