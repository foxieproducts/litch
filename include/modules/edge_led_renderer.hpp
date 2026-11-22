#pragma once

#include <array>
#include <cstdint>

#include "modules/edge_led_constants.hpp"
#include "modules/rgb.hpp"
#include "modules/turn_anim_style.hpp"
#include "modules/ui_types.hpp"

namespace modules {

enum class EdgeUiMode : uint8_t {
    IdleTurn = 0,
    Thinking = 1,
    Error = 2,
    Placement = 3,
    Menu = 4,
    BrightnessPreview = 5,
    FatalCheckmate = 6,
    FatalDraw = 7,
    SetupCompleteSweep = 8,
    HealthPaused = 9,
    HealthDegradedUsable = 10,
    UnavailableNudge = 11,
    OccupiedBoardWarning = 12,
};

struct EdgeLedTiming {
    uint32_t turnFrameMs = 80;
    uint32_t illegalFlashMs = 300;
};

struct EdgeLedPalette {
    Rgb p1 = {0, 128, 255};
    Rgb p2 = {255, 0, 0};
    Rgb error = {255, 0, 0};
    Rgb prompt = RGB_YELLOW;
    Rgb neutral = RGB_WHITE;
    Rgb confirm = RGB_GREEN;
    Rgb off = RGB_BLACK;
};

struct EdgeLedState {
    bool animationTimeExplicit = false;
    uint32_t nowMs = 0;
    uint32_t faultFlashElapsedMs = 0;
    uint32_t faultFlashPeriodMs = 1;

    // Priority is enforced by selecting one mode.
    EdgeUiMode mode = EdgeUiMode::IdleTurn;

    // Used by IdleTurn/Thinking.
    UiOwner activeSide = UiOwner::P1;

    // Used by IdleTurn.
    TurnAnimStyle turnAnimStyle = TurnAnimStyle::ActiveEndPingpongSides;
    bool minimalAnimations = false;

    // Optional override for IdleTurn/Thinking color selection.
    // When set, geometry still uses activeSide, but colors use activePalette.
    // If None, colors follow activeSide.
    UiOwner activePalette = UiOwner::None;

    // Used by Menu/BrightnessPreview.
    UiOwner owner = UiOwner::None;
    uint8_t value01_8 = 0; // for brightness preview: 0..8

    // Used by Menu.
    // Menu display can be shown on a different home edge than the logical owner.
    // BOARD_ROTATE rotates output indices instead of swapping owners here.
    UiOwner menuEdgeOwner = UiOwner::None;
    uint8_t menuSelectionIndex = 0; // 0-based menu option index
    uint8_t menuValue01_255 = 0;    // 0..255 bar fill (or hold progress)
    bool menuValueIsHoldProgress = false;
    bool menuHoldFillAll = false;

    // Used by bounded one-shot perimeter cues.
    uint32_t effectStartMs = 0;

    // Used by fatal game-over modes. This is episode-relative rather than
    // absolute uptime so every game-over animation starts at a stable phase.
    uint32_t gameOverElapsedMs = 0;

    UiOwner hintFeedbackOwner = UiOwner::None;
    bool hintFeedbackPending = false;
    bool hintFeedbackHasMove = false;
    uint32_t hintFeedbackElapsedMs = 0U;

    bool beginnerModeP1Enabled = false;
    bool beginnerModeP2Enabled = false;
    UiOwner beginnerModeStingerOwner = UiOwner::None;
    bool beginnerModeStingerEnabling = false;
    uint32_t beginnerModeStingerStartMs = 0;
};

class EdgeLedRenderer {
  public:
    explicit EdgeLedRenderer(const EdgeLedPalette& palette = {},
                             const EdgeLedTiming& timing = {});

    void Render(const EdgeLedState& state,
                std::array<Rgb, EDGE_LED_COUNT>& out) const;

  private:
    EdgeLedPalette m_palette;
    EdgeLedTiming m_timing;
};

} // namespace modules
