#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "modules/control_mutation_service.hpp"
#include "modules/edge_led_cue.hpp"
#include "modules/led_text_renderer.hpp"
#include "modules/menu_level_overlay.hpp"
#include "modules/quick_settings_overlay_controller.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "modules/ui_types.hpp"

namespace modules {

struct QuickSettingsDirectEntryContext {
    bool wakeInputSuppressed = false;
    bool hardwareTestActive = false;
    bool ampTestActive = false;
    bool resetHoldActive = false;
    bool menuOverlayActive = false;
    bool quickSettingsOverlayActive = false;
    bool textOverlayActive = false;
    bool placementActive = false;
    bool kingQueenSwapActive = false;
    bool undoPromptActive = false;
    bool promotionActive = false;
};

constexpr bool IsQuickSettingsDirectEntryAllowed(
    const QuickSettingsDirectEntryContext& context) {
    return !context.wakeInputSuppressed &&
           !context.hardwareTestActive &&
           !context.ampTestActive &&
           !context.resetHoldActive &&
           !context.menuOverlayActive &&
           !context.quickSettingsOverlayActive &&
           !context.textOverlayActive &&
           // Ordinary setup must retain access to brightness before pieces
           // are placed; dedicated correction/modal owners remain guarded.
           !context.kingQueenSwapActive &&
           !context.undoPromptActive &&
           !context.promotionActive;
}

struct QuickSettingsOrchestrationContext {
    SettingsStore& settings;
    ControlMutationService& controlMutation;
    MenuLevelOverlay& overlay;
    const std::array<Rgb, led_text_renderer::GRID_PIXELS>& background;
    UiOwner owner = UiOwner::None;
    uint8_t selectedIndex = 0U;
    uint32_t nowMs = 0U;
    bool settingsLoaded = true;
    std::string_view firmwareVersion;
};

struct QuickSettingsMutationResult {
    bool handled = false;
    bool hasOutcome = false;
    bool showBlockedFeedback = false;
    ControlMutationOutcome outcome;
};

struct QuickSettingsBlockedFeedback {
    bool visible = false;
    bool flashAllPieces = false;
    EdgeLedCueKind edgeCue = EdgeLedCueKind::None;
};

[[nodiscard]] QuickSettingsBlockedFeedback
ClassifyQuickSettingsBlockedFeedback(ControlMutationReason reason);

void ShowQuickSettingsSelection(
    const QuickSettingsOrchestrationContext& context);

void RefreshQuickSettingsCpuDifficulty(
    const SettingsStore& settings, MenuLevelOverlay& overlay,
    const std::array<Rgb, led_text_renderer::GRID_PIXELS>& background);

QuickSettingsMutationResult ApplyQuickSettingsAdjustment(
    const QuickSettingsOrchestrationContext& context,
    const QuickSettingsAction& action);

} // namespace modules
