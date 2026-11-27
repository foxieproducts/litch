#pragma once

#include "modules/board_led_frame.hpp"
#include "modules/board_style_preview.hpp"
#include "modules/checkmate_animation_setting.hpp"
#include "modules/hint_thinking_style_setting.hpp"
#include "modules/light_square_appearance.hpp"
#include "modules/settle_animation_setting.hpp"
#include "modules/turn_anim_style.hpp"

#include <array>
#include <cstdint>

#include "hardware/led_layout.hpp"
#include "modules/edge_led_cue.hpp"
#include "modules/edge_led_intent.hpp"
#include "modules/grid_foreground_layer.hpp"
#include "modules/edge_led_renderer.hpp"
#include "modules/menu_level_overlay.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "modules/side_helpers.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "runtime/interaction_domain_types.hpp"
#include "runtime/runtime_health.hpp"

namespace modules::led_frame_composer {

// Existing empty-board square background, without any claim about occupancy,
// active player or readiness. Used while authoritative startup is incomplete.
void ComposeStartupGrid(
    const fxe::SettingsStore& settings,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& outLedFrame);

constexpr modules::UiOwner OwnerForSideToMove(
    const modules::SideToMove sideToMove,
    const bool firstPlayerIsP2) {
    (void)firstPlayerIsP2;
    switch (sideToMove) {
        case modules::SideToMove::White:
            return modules::UiOwner::P1;
        case modules::SideToMove::Black:
            return modules::UiOwner::P2;
        default:
            return modules::UiOwner::None;
    }
}

// Captured by the settings owner. The animation task never reads its document.
struct ComposeSettings {
    bool minimumBrightness = false;
    bool whiteSquaresOn = true;
    bool coloredLightSquares = false;
    uint8_t lightSquareHue = 0U;
    uint8_t lightSquareBrightness = light_square_appearance::DEFAULT_BRIGHTNESS;
    uint8_t lightSquareMinimumChannel = 1U;
    uint8_t nearEndPieceWheel = 0U;
    uint8_t farEndPieceWheel = 0U;
    bool initialKingPlacementAssistance = true;
    bool persistenceDegraded = false;
    bool p1LegalMoves = true;
    bool p2LegalMoves = true;
    bool p1CheckCue = true;
    bool p2CheckCue = true;
    bool minimalAnimations = false;
    UiOwner cpuOwner = UiOwner::None;
    CheckmateAnimationStyle checkmateStyle = CheckmateAnimationStyle::FireLob;
    SettleAnimationStyle settleStyle = settle_animation_setting::DEFAULT_STYLE;
    TurnAnimStyle turnStyle = TurnAnimStyle::ActiveEndPingpongSides;
    hint_thinking_style_setting::Style thinkingStyle =
        hint_thinking_style_setting::Style::CauldronBrewing;

    bool LegalMoves(const UiOwner owner) const {
        return owner == UiOwner::P1 ? p1LegalMoves
             : owner == UiOwner::P2 && p2LegalMoves;
    }
    bool CheckCue(const UiOwner owner) const {
        return owner == UiOwner::P1 ? p1CheckCue
             : owner == UiOwner::P2 && p2CheckCue;
    }
};

ComposeSettings CaptureComposeSettings(const fxe::SettingsStore& settings);

struct HintPresentationOutcome {
    runtime::HintPresentationTuple tuple{};
    bool visible = false;
    bool retainPrior = false;
};

struct ComposeInputs {
    // Existing synchronous callers may supply settings. Cross-task publication
    // supplies the bounded resolved value instead, with no document pointer.
    const fxe::SettingsStore* settings = nullptr;
    const ComposeSettings* resolvedSettings = nullptr;
    const runtime::InteractionModelSnapshot* interactionSnapshot = nullptr;
    runtime::RuntimeHealthSnapshot runtimeHealth{};
    bool litchcraftActive = false;

    // Startup owns a deliberately neutral board cue. User-selected ordinary
    // square hue/intensity begins only after that startup owner releases it.
    bool neutralStartupGrid = false;

    // OTA/recovery-class status may replace health. Ordinary menu, BLE, and
    // gameplay overlays may not.
    bool higherPriorityStatusOverlayActive = false;

    // Logical player/palette policy only. Physical end placement is applied by
    // the output mapper and never by the composer.
    bool firstPlayerIsP2 = false;


    // Menu overlay (text/icon layer) and menu-controlled edge intent.
    modules::MenuLevelOverlay* menuOverlay = nullptr;
    bool menuActive = false;
    bool menuOverlayPreservesTurnIndicator = false;
    // Menu lighting alpha (0..255). 255 = full menu visual effect.
    // Used to fade out menu dim/overlay cleanly after the menu closes.
    uint8_t menuVisualAlpha255 = 0;
    // Board/piece background dim target while menu/overlay is active.
    // 51 ~= 20% intensity (80% dim), 26 ~= 10% intensity (90% dim).
    uint16_t menuBackgroundDimScale256 = 51;
    modules::EdgeLedIntent menuIntent{};
    modules::EdgeLedCueIntent edgeCueIntent{};

    // Optional emulator-only preview overlay inputs (e.g. mouse click-from preview).
    uint8_t previewFromLogical = 255;
    uint64_t previewDestMask = 0;

    modules::BoardStylePreviewFrame boardStylePreview{};
    uint8_t paletteEditOwners = 0U;
    bool beginnerModeP1Enabled = false;
    bool beginnerModeP2Enabled = false;
    modules::UiOwner beginnerModeStingerOwner = modules::UiOwner::None;
    bool beginnerModeStingerEnabling = false;
    uint32_t beginnerModeStingerStartMs = 0;
};

// Composes the board into outLedFrame; status remains in outEdgeFrame.
// Also fills outEdgeFrame with the rendered edge pixels (modules::EDGE_LED_COUNT).
// Also snapshots the composed board square grid (physical squares) into outGridFrame.
void Compose(const ComposeInputs& in,
             std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& outLedFrame,
             std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdgeFrame,
             std::array<modules::Rgb, 64>& outGridFrame);

// Same as Compose(), but can also optionally snapshot the grid after the menu background dim
// has been applied and before the menu overlay is applied.
//
// This is useful for menu overlay transitions: the menu overlay itself should not be able to
// contaminate the background snapshot used for subsequent icon/text compositions.
// The optional thinking output reports final edge ownership for transport overlays.
void Compose(const ComposeInputs& in,
             std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& outLedFrame,
             std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdgeFrame,
             std::array<modules::Rgb, 64>& outGridFrame,
             std::array<modules::Rgb, 64>* outGridFrameBeforeMenuOverlay,
             bool* outCpuThinkingVisible = nullptr,
             uint64_t* outSidePrioritySquareMask = nullptr,
             uint64_t* outOrdinaryPieceMask = nullptr,
             modules::status_led_frame_adapter::FixedPhysicalStatusOverlay*
                 outFixedPhysicalStatusOverlay = nullptr,
             modules::GridForegroundLayer* outMenuForeground = nullptr,
             uint64_t* outCaptureRemovalMask = nullptr,
             uint64_t* outDuckReminderMask = nullptr,
             HintPresentationOutcome* outHint = nullptr);

// Composes the board into outLedFrame; status remains in outEdgeFrame.
// Also fills outEdgeFrame with the rendered edge pixels (modules::EDGE_LED_COUNT).
// Also snapshots the composed board square grid (physical squares) into outGridFrame.
void Compose(const ComposeInputs& in,
             modules::BoardLedFrame& outLedFrame,
             std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdgeFrame,
             std::array<modules::Rgb, 64>& outGridFrame);

// Same as Compose(), but can also optionally snapshot the grid after the menu background dim
// has been applied and before the menu overlay is applied.
//
// This is useful for menu overlay transitions: the menu overlay itself should not be able to
// contaminate the background snapshot used for subsequent icon/text compositions.
// The optional thinking output reports final edge ownership for transport overlays.
void Compose(const ComposeInputs& in,
             modules::BoardLedFrame& outLedFrame,
             std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdgeFrame,
             std::array<modules::Rgb, 64>& outGridFrame,
             std::array<modules::Rgb, 64>* outGridFrameBeforeMenuOverlay,
             bool* outCpuThinkingVisible = nullptr,
             uint64_t* outSidePrioritySquareMask = nullptr,
             uint64_t* outOrdinaryPieceMask = nullptr,
             modules::status_led_frame_adapter::FixedPhysicalStatusOverlay*
                 outFixedPhysicalStatusOverlay = nullptr,
             modules::GridForegroundLayer* outMenuForeground = nullptr,
             uint64_t* outCaptureRemovalMask = nullptr,
             uint64_t* outDuckReminderMask = nullptr,
             HintPresentationOutcome* outHint = nullptr,
             uint64_t* outCaptureTapMask = nullptr);

void ComposeStartupGrid(const fxe::SettingsStore& settings, modules::BoardLedFrame& out);

// Restores warning pixels composed by the shared status owner after Litchcraft
// replaces its ordinary status baseline. Setup-complete is not a warning and
// remains below Litchcraft ownership.
void RestoreLitchcraftStatusWarnings(
    const ComposeInputs& in,
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& sharedStatusFrame,
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>& litchcraftStatusFrame);

// Applies the same occupied-piece blink phase to resting brushes after the
// Litchcraft canvas replaces the chess frame, without changing artwork.
void ApplyLitchcraftOccupiedBoardWarning(const ComposeInputs& in,
                                        uint64_t occupancy,
                                        modules::BoardLedFrame& board);

} // namespace modules::led_frame_composer
