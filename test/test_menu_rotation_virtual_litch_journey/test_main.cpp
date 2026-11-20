#include <filesystem>
#include <string>

#include "modules/litch_max_engine.hpp"
#include "modules/white_squares_setting.hpp"
#include "modules/player_end_palette.hpp"
#include "modules/ui_color_keys.hpp"
#include "modules/ui_color_wheel.hpp"
#include "modules/rgb_optical_calibration.hpp"
#include "modules/rgbw_auto_map.hpp"
#include "bounded_hint_engine.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

#include "hardware/led_layout.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/game_display_text_overlay.hpp"
#include "modules/orientation.hpp"
#include "runtime/virtual_litch_session.h"
#include "virtual_litch_session.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

uint16_t ColorEnergy(const virtual_litch_color color) {
    return static_cast<uint16_t>(color.r) + color.g + color.b + color.w;
}

uint64_t HighIntensityBoardMask(
    const virtual_litch_physical_frame& frame) {
    uint64_t mask = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        const uint16_t base = static_cast<uint16_t>(
            square * hardware::LEDS_PER_SQUARE);
        for (uint8_t slot = 0U; slot < hardware::LEDS_PER_SQUARE; ++slot) {
            if (base + slot < frame.led_count &&
                ColorEnergy(frame.pixels[base + slot]) >= 128U) {
                mask |= UINT64_C(1) << square;
                break;
            }
        }
    }
    return mask;
}

uint64_t Rotate180(const uint64_t mask) {
    uint64_t rotated = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((mask & (UINT64_C(1) << square)) != 0ULL) {
            rotated |= UINT64_C(1) << (63U - square);
        }
    }
    return rotated;
}

uint64_t PhysicalStatusHash(const virtual_litch_physical_frame& frame,
                            const hardware::BoardRevision revision) {
    constexpr uint64_t OFFSET = UINT64_C(1469598103934665603);
    constexpr uint64_t PRIME = UINT64_C(1099511628211);
    const uint16_t first = hardware::BoardLedTopologyFor(
        revision).physicalBoardLedCount;
    Check(first < frame.led_count, "physical perimeter hash samples actual status pixels");
    uint64_t hash = OFFSET;
    for (uint16_t index = first; index < frame.led_count; ++index) {
        for (const uint8_t channel : {frame.pixels[index].r,
                                      frame.pixels[index].g,
                                      frame.pixels[index].b,
                                      frame.pixels[index].w}) {
            hash = (hash ^ channel) * PRIME;
        }
    }
    return hash;
}

struct MenuCapture {
    uint64_t boardMask = 0ULL;
    uint64_t statusHash = 0ULL;
    uint8_t physicalMarker = 0U;
};

void CheckNoOldOrientationGhost(const hardware::BoardRevision revision) {
    modules::BoardLedFrame board{};
    board[0].left = modules::RGB_WHITE;
    board[0].right = modules::RGB_WHITE;
    modules::GridForegroundLayer foreground{};
    foreground.sourceCoverageMask = 1ULL;
    foreground.placementCoverageMask = 1ULL;
    foreground.background[0] = modules::RGB_BLACK;
    foreground.colors[0] = modules::RGB_WHITE;
    foreground.alpha255 = 255U;
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    const auto physical = hardware::MapSemanticLedFrameWithGridForeground(
        {revision, true, false, 0ULL}, false, board, foreground, status);
    const auto correct = hardware::MapSemanticSquareLedTargets(
        revision, 0U, hardware::SquareLedSlot::Square0, false);
    const auto ghost = hardware::MapSemanticSquareLedTargets(
        revision, 0U, hardware::SquareLedSlot::Square0, true);
    Check(correct.count == 1U && ghost.count == 1U &&
              ColorEnergy(virtual_litch_color{
                  physical[correct.indices[0]].r,
                  physical[correct.indices[0]].g,
                  physical[correct.indices[0]].b, 0U}) != 0U,
          "menu foreground appears at its literal/owner orientation");
    Check(ColorEnergy(virtual_litch_color{
              physical[ghost.indices[0]].r,
              physical[ghost.indices[0]].g,
              physical[ghost.indices[0]].b, 0U}) == 0U,
          "menu foreground leaves no ghost at gameplay orientation");
}

void CheckExplicitCoverageSurvivesSameColorAndBlack() {
    modules::GameDisplayTextOverlayConfig config{};
    config.fadeInMs = 0U;
    config.holdMs = 1000U;
    config.fadeOutMs = 0U;
    config.backgroundScale = 255U;
    std::array<modules::Rgb, 64> white{};
    white.fill(modules::RGB_WHITE);

    modules::GameDisplayTextOverlay sameColor(config);
    sameColor.Start("1", modules::RGB_WHITE, white);
    std::array<modules::Rgb, 64> composed{};
    modules::GridForegroundLayer foreground{};
    sameColor.Compose(white, composed, &foreground);
    Check(foreground.sourceCoverageMask != 0ULL,
          "same-color glyph retains explicit foreground coverage");
    const uint8_t covered = static_cast<uint8_t>(__builtin_ctzll(
        foreground.sourceCoverageMask));
    Check(foreground.background[covered].r ==
              foreground.colors[covered].r,
          "same-color coverage does not depend on an RGB difference");

    modules::GameDisplayTextOverlay black(config);
    black.Start("1", modules::RGB_BLACK, white);
    black.Compose(white, composed, &foreground);
    Check(foreground.sourceCoverageMask != 0ULL &&
              foreground.colors[covered].r == 0U &&
              foreground.background[covered].r == 255U,
          "black glyph is explicit foreground rather than transparent RGB");
}

void CheckV1StatusKeepsPriority() {
    modules::BoardLedFrame board{};
    modules::GridForegroundLayer foreground{};
    foreground.sourceCoverageMask = UINT64_MAX;
    foreground.placementCoverageMask = UINT64_MAX;
    foreground.colors.fill(modules::RGB_WHITE);
    foreground.alpha255 = 255U;
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    status[0] = modules::Rgb{0U, 255U, 0U};
    const hardware::LedFrameMappingConfig config{
        hardware::BoardRevision::V1C3, false, false, 0ULL};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> noStatus{};
    const auto foregroundOnly = hardware::MapSemanticLedFrameWithGridForeground(
        config, true, board, foreground, noStatus);
    const auto actual = hardware::MapSemanticLedFrameWithGridForeground(
        config, true, board, foreground, status);
    const uint8_t physicalStatus = modules::status_led_frame_adapter::
        PhysicalIndexForSemanticIndex(0U, false);
    const uint16_t target = modules::status_led_frame_adapter::
        VirtualLedIndexForPhysicalStatusIndex(physicalStatus);
    const modules::Rgb expected = modules::status_led_frame_adapter::
        BlendVirtualStatus(foregroundOnly[target], status[0]);
    Check(actual[target].r == expected.r &&
              actual[target].g == expected.g &&
              actual[target].b == expected.b,
          "V1 shared status LED retains priority above menu foreground");
}

void CheckFadeUsesDestinationBackground() {
    modules::BoardLedFrame board{};
    const modules::Rgb sourceBackground{120U, 0U, 0U};
    const modules::Rgb destinationBackground{0U, 0U, 120U};
    const modules::Rgb foregroundColor{120U, 120U, 120U};
    board[0].left = modules::led_text_renderer::AlphaBlend(
        sourceBackground, foregroundColor, 128U);
    board[0].right = board[0].left;
    board[63].left = destinationBackground;
    board[63].right = destinationBackground;
    modules::GridForegroundLayer foreground{};
    foreground.background[0] = sourceBackground;
    foreground.background[63] = destinationBackground;
    foreground.colors[0] = foregroundColor;
    foreground.alpha255 = 128U;
    foreground.sourceCoverageMask = 1ULL;
    foreground.placementCoverageMask = 1ULL;
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    const auto physical = hardware::MapSemanticLedFrameWithGridForeground(
        {hardware::BoardRevision::V3S3, true, false, 0ULL}, false,
        board, foreground, status);
    const modules::Rgb expected = modules::led_text_renderer::AlphaBlend(
        destinationBackground, foregroundColor, 128U);
    Check(physical[0].r == expected.r && physical[0].g == expected.g &&
              physical[0].b == expected.b,
          "faded foreground blends with its rotated destination background");
    Check(physical[63U * 4U].r == sourceBackground.r &&
              physical[63U * 4U].b == sourceBackground.b,
          "faded foreground restores its gameplay-orientation source");
}

void CheckWhitePreviewClipsDestinationOccupancy() {
    modules::GridForegroundLayer foreground{};
    foreground.sourceCoverageMask = 1ULL;
    foreground.placementCoverageMask = 1ULL;
    hardware::ClipGridForegroundToOccupiedPreview(
        1ULL, true, false, foreground);
    Check(foreground.sourceCoverageMask == 1ULL &&
              foreground.placementCoverageMask == 0ULL,
          "white preview removes foreground whose rotated destination is empty");

    foreground.sourceCoverageMask = 1ULL;
    foreground.placementCoverageMask = 1ULL;
    hardware::ClipGridForegroundToOccupiedPreview(
        1ULL << 63U, true, false, foreground);
    Check(foreground.sourceCoverageMask == 0ULL &&
              foreground.placementCoverageMask == 1ULL,
          "white preview independently retains foreground for occupied destination");
}

MenuCapture OpenMenuAndCapture(const bool boardRotate,
                               const bool firstPlayerIsP2,
                               const bool physicalOwnerIsP2) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "menu-rotation journey resets production session");
    session.SettingsForPersistenceTest().Doc()["BOARD_ROTATE"] = boardRotate;
    session.SettingsForPersistenceTest().Doc()["GAME_FIRST_PLAYER"] =
        firstPlayerIsP2 ? 2U : 1U;
    Check(session.ObserveHallMask(UINT64_C(0xFFFF00000000FFFF)) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK,
          "menu-rotation journey reaches the opening");

    const uint8_t physicalOwner = physicalOwnerIsP2
        ? VIRTUAL_LITCH_OWNER_P2
        : VIRTUAL_LITCH_OWNER_P1;
    Check(session.SetButtonDown(
              physicalOwner,
              runtime::VirtualLitchPhysicalButton::X, true) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              session.Advance(1000U) == VIRTUAL_LITCH_OK &&
              session.SetButtonDown(
                  physicalOwner,
                  runtime::VirtualLitchPhysicalButton::X, false) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              session.QuickSettingsActive(),
          "physical X hold opens Quick Setup from P1's mapped end");
    // The neutral selected marker has equal positive semantic RGB channels.
    // Observe its rendered position separately from colors that follow players.
    uint8_t marker = 0U;
    unsigned markerCount = 0U;
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        const auto color = session.SemanticFrame().status[index];
        if (color.r > 0U && color.r == color.g && color.g == color.b) {
            marker = modules::status_led_frame_adapter::PhysicalIndexForSemanticIndex(
                index, boardRotate);
            ++markerCount;
        }
    }
    Check(markerCount == 1U, "Quick Setup has exactly one neutral selected marker");
    uint8_t physicalMarker = 0U;
    uint16_t brightest = 0U;
    unsigned brightestCount = 0U;
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        const uint16_t energy = ColorEnergy(session.PhysicalFrame().pixels[192U + index]);
        if (energy > brightest) {
            physicalMarker = index;
            brightest = energy;
            brightestCount = 1U;
        } else if (energy == brightest) {
            ++brightestCount;
        }
    }
    Check(brightest > 0U && brightestCount == 1U && physicalMarker == marker,
          "Quick Setup neutral marker reaches the unique brightest V2 status pixel");
    const MenuCapture capture{
        HighIntensityBoardMask(session.PhysicalFrame()),
        PhysicalStatusHash(session.PhysicalFrame(), hardware::BoardRevision::V2S3),
        physicalMarker};
    std::printf(
        "[MENU_ROTATION_CHECKPOINT] board_rotate=%u first_player_p2=%u "
        "physical_owner=%s menu_mask=%016llx status_hash=%016llx marker=%u "
        "next=read-Quick-Setup\n",
        boardRotate ? 1U : 0U, firstPlayerIsP2 ? 1U : 0U,
        physicalOwnerIsP2 ? "P2" : "P1",
        static_cast<unsigned long long>(capture.boardMask),
        static_cast<unsigned long long>(capture.statusHash), capture.physicalMarker);
    Check(session.SetButtonDown(
              physicalOwner, runtime::VirtualLitchPhysicalButton::X, true) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              session.SetButtonDown(
                  physicalOwner, runtime::VirtualLitchPhysicalButton::X,
                  false) == VIRTUAL_LITCH_OK &&
              session.Advance(140U) == VIRTUAL_LITCH_OK &&
              !session.QuickSettingsActive(),
          "mapped X closes Quick Setup and completes foreground recovery");
    std::printf(
        "[MENU_ROTATION_RECOVERY] board_rotate=%u first_player_p2=%u "
        "physical_owner=%s active=%u next=ordinary-gameplay\n",
        boardRotate ? 1U : 0U, firstPlayerIsP2 ? 1U : 0U,
        physicalOwnerIsP2 ? "P2" : "P1",
        session.QuickSettingsActive() ? 1U : 0U);
    return capture;
}

void V3_3LocalDirectionMenuJourney() {
    // Named physical inputs exercise debounce, owner normalization, real menu
    // actions and timed rendering. test_gpio_control_mapping separately drives
    // the actual GPIO owner: this adapter bypasses contact voltage/copper.
    for (const bool boardRotate : {false, true}) {
        for (const bool playersSwapped : {false, true}) {
            for (const bool physicalP2 : {false, true}) {
                virtual_litch_config config{};
                virtual_litch_default_config(&config);
                config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
                test_support::BoundedHintSession fixture(config);
                auto& s = fixture.session;
                s.SettingsForPersistenceTest().Doc()["BOARD_ROTATE"] = boardRotate;
                s.SettingsForPersistenceTest().Doc()["GAME_FIRST_PLAYER"] =
                    playersSwapped ? 2U : 1U;
                const uint64_t occupancy = UINT64_C(0xFFFF00000000FFFF);
                Check(s.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
                          s.Advance(200U) == VIRTUAL_LITCH_OK,
                      "V3.3 direction journey reaches opening through Hall input");
                const uint8_t bank = physicalP2 ? VIRTUAL_LITCH_OWNER_P2
                                               : VIRTUAL_LITCH_OWNER_P1;
                auto edge = [&](const runtime::VirtualLitchPhysicalButton button,
                                const bool down) {
                    Check(s.SetButtonDown(bank, button, down) == VIRTUAL_LITCH_OK &&
                              s.Advance(20U) == VIRTUAL_LITCH_OK,
                          "V3.3 physical button edge is debounced and accepted");
                };
                auto tap = [&](const runtime::VirtualLitchPhysicalButton button) {
                    if (button == runtime::VirtualLitchPhysicalButton::Left ||
                        button == runtime::VirtualLitchPhysicalButton::Right) {
                        Check(s.Advance(250U) == VIRTUAL_LITCH_OK,
                              "wait out the vertical guard before a fresh side Press");
                    }
                    edge(button, true);
                    edge(button, false);
                    s.Advance(80U);
                };
                auto readWindow = [&]() {
                    for (unsigned frame = 0U; frame < 22U; ++frame) s.Advance(50U);
                };
                auto checkpoint = [&](const char* stage, const uint8_t selection,
                                      const char* next) {
                    const uint8_t ring = static_cast<uint8_t>(
                        (physicalP2 ? 7U : 23U) - selection);
                    const uint8_t marker = hardware::V3_3PhysicalIndexForSemanticStatus(ring);
                    const uint8_t semantic = static_cast<uint8_t>(
                        (ring + (boardRotate ? 16U : 0U)) % 32U);
                    const auto white = s.SemanticFrame().status[semantic];
                    const uint8_t otherRing = static_cast<uint8_t>(
                        (physicalP2 ? 7U : 23U) - (selection == 0U ? 1U : 0U));
                    const uint8_t other = hardware::V3_3PhysicalIndexForSemanticStatus(otherRing);
                    const auto& frame = s.PhysicalFrame();
                    const bool selected = s.QuickSettingsActive() &&
                        s.QuickSettingsSelectedIndex() == selection;
                    const bool markerOk = white.r > 0U && white.r == white.g && white.g == white.b &&
                        ColorEnergy(frame.pixels[256U + marker]) >
                            ColorEnergy(frame.pixels[256U + other]);
                    Check(selected,
                          "V3.3 local direction selects the expected menu item");
                    Check(markerOk &&
                              s.State().physical_occupied_mask == occupancy,
                          "V3.3 selected marker stays on the user's physical edge and game is retained");
                    std::printf("[V33_LOCAL_DIRECTION_MENU] stage=%s rotate=%u "
                                "players_swapped=%u physical_bank=%s owner=%u "
                                "t_ms=%u occupancy=%016llx mode=Quick-Setup "
                                "selection_ok=%u marker_ok=%u selection=%u "
                                "marker_chain=%u display=%.*s next=%s\n",
                                stage, boardRotate ? 1U : 0U, playersSwapped ? 1U : 0U,
                                physicalP2 ? "P2" : "P1", s.QuickSettingsOwner(),
                                s.State().now_ms, static_cast<unsigned long long>(occupancy),
                                selected ? 1U : 0U, markerOk ? 1U : 0U, selection, marker,
                                static_cast<int>(s.DisplayText().size()), s.DisplayText().data(), next);
                };
                auto holdCheckpoint = [&](const unsigned heldMs) {
                    const bool swapped = boardRotate != playersSwapped;
                    const bool logicalP2 = physicalP2 != swapped;
                    // Wheels are stored at fixed reference ends; the logical
                    // player selects its assigned slot before literal rotation.
                    const bool paletteP2 = logicalP2 != playersSwapped;
                    const uint8_t localRing = static_cast<uint8_t>((physicalP2 ? 7U : 23U));
                    const uint8_t semanticRing = static_cast<uint8_t>(
                        (localRing + (swapped ? 16U : 0U)) % 32U);
                    const uint8_t wheel = static_cast<uint8_t>(
                        modules::player_end_palette::ReadSettingUInt32(
                            s.SettingsForPersistenceTest(),
                            paletteP2 ? modules::ui_color_keys::P2_END_PIECE_WHEEL
                                      : modules::ui_color_keys::P1_END_PIECE_WHEEL,
                            paletteP2 ? modules::player_end_palette::P2_END_DEFAULT_WHEEL
                                      : modules::player_end_palette::P1_END_DEFAULT_WHEEL));
                    const auto bright = modules::ui_color_wheel::WheelToRgb(wheel);
                    const auto expected = modules::ScaleRgbSaturatingBy256(bright, 320U);
                    const auto color = s.SemanticFrame().status[semanticRing];
                    const uint8_t pin = hardware::V3_3PhysicalIndexForSemanticStatus(localRing);
                    const bool localCue = color.r == expected.r && color.g == expected.g &&
                        color.b == expected.b &&
                        ColorEnergy(s.PhysicalFrame().pixels[256U + pin]) > 0U;
                    Check(!s.QuickSettingsActive() && localCue,
                          "V3.3 pre-open X hold stays beside its physical bank");
                    std::printf("[V33_LOCAL_DIRECTION_MENU] stage=X-hold t_ms=%u held_ms=%u "
                                "rotate=%u players_swapped=%u physical_bank=%s logical_owner=%u "
                                "mode=gameplay menu_active=%u local_cue=%u "
                                "actual_rgb=%u,%u,%u expected_rgb=%u,%u,%u "
                                "next=continue-X-to-open\n",
                                s.State().now_ms, heldMs, boardRotate ? 1U : 0U,
                                playersSwapped ? 1U : 0U, physicalP2 ? "P2" : "P1",
                                logicalP2 ? 2U : 1U, s.QuickSettingsActive() ? 1U : 0U,
                                localCue ? 1U : 0U, color.r, color.g, color.b,
                                expected.r, expected.g, expected.b);
                };
                edge(runtime::VirtualLitchPhysicalButton::X, true);
                s.Advance(200U);
                holdCheckpoint(200U);
                s.Advance(400U);
                holdCheckpoint(600U);
                s.Advance(400U);
                edge(runtime::VirtualLitchPhysicalButton::X, false);
                s.Advance(100U);
                readWindow();
                checkpoint("open", 0U, "Right-to-CPU-seat");
                tap(runtime::VirtualLitchPhysicalButton::Left);
                checkpoint("left-at-first-item-no-op", 0U, "Right-to-CPU-seat");
                tap(runtime::VirtualLitchPhysicalButton::Right);
                readWindow();
                Check(s.DisplayText() == "2", "CPU seat value is readable after its icon window");
                checkpoint("right-next", 1U, "Left-to-CPU-difficulty");
                tap(runtime::VirtualLitchPhysicalButton::Left);
                checkpoint("left-previous", 0U, "Up-increases-difficulty");
                const uint8_t before = s.SettingsForPersistenceTest().Doc()["VS_CPU_LEVEL"].as<uint8_t>();
                tap(runtime::VirtualLitchPhysicalButton::Up);
                readWindow();
                Check(s.SettingsForPersistenceTest().Doc()["VS_CPU_LEVEL"].as<uint8_t>() == before + 1U,
                      "V3.3 local Up increases menu value");
                Check(s.DisplayText() == std::to_string(before + 1U),
                      "V3.3 local Up displays the increased difficulty");
                checkpoint("value-increased", 0U, "Down-restores-difficulty");
                tap(runtime::VirtualLitchPhysicalButton::Down);
                readWindow();
                Check(s.SettingsForPersistenceTest().Doc()["VS_CPU_LEVEL"].as<uint8_t>() == before,
                      "V3.3 local Down restores menu value");
                Check(s.DisplayText() == std::to_string(before),
                      "V3.3 local Down displays the restored difficulty");
                checkpoint("value-restored", 0U, "X-exits-with-game-retained");
                tap(runtime::VirtualLitchPhysicalButton::X);
                Check(!s.QuickSettingsActive() && s.State().physical_occupied_mask == occupancy,
                      "V3.3 X exit restores gameplay without changing pieces");
                std::printf("[V33_LOCAL_DIRECTION_MENU] stage=X-exit t_ms=%u menu_active=%u "
                            "occupancy_retained=%u next=hold-X-to-reopen\n", s.State().now_ms,
                            s.QuickSettingsActive() ? 1U : 0U,
                            s.State().physical_occupied_mask == occupancy ? 1U : 0U);
                edge(runtime::VirtualLitchPhysicalButton::X, true);
                s.Advance(1000U);
                const uint32_t openedAtMs = s.State().now_ms;
                edge(runtime::VirtualLitchPhysicalButton::X, false);
                s.Advance(100U);
                checkpoint("reopen", 0U, "wait-for-ten-second-expiry");
                s.Advance(9999U - (s.State().now_ms - openedAtMs));
                checkpoint("idle-9999ms", 0U, "one-ms-to-expiry");
                s.Advance(1U);
                Check(!s.QuickSettingsActive() && s.State().physical_occupied_mask == occupancy,
                      "V3.3 menu expires and restores unchanged game");
                std::printf("[V33_LOCAL_DIRECTION_MENU] stage=idle-10000ms t_ms=%u "
                            "menu_active=%u occupancy_retained=%u next=resume-game\n",
                            s.State().now_ms, s.QuickSettingsActive() ? 1U : 0U,
                            s.State().physical_occupied_mask == occupancy ? 1U : 0U);
            }
        }
    }
}


unsigned PhysicalGlyphPixels(runtime::VirtualLitchSession& s) {
    unsigned n = 0;
    for (unsigned i = 0; i < 256; ++i)
        if (s.PhysicalFrame().pixels[i].w > 20)
            ++n;
    return n;
}
unsigned GlyphPixels(runtime::VirtualLitchSession& s) {
    unsigned n = 0;
    const auto& f = s.SemanticFrame();
    for (unsigned q = 0; q < 64; ++q) {
        const auto c = f.board[q][0];
        if (c.r >= 220 && c.g >= 220 && c.b >= 220)
            ++n;
    }
    return n;
}
void TerminalCheckpoint(runtime::VirtualLitchSession& s,
                        const char* stage,
                        unsigned bank,
                        const char* next) {
    const auto st = s.State();
    modules::ControlMutationOutcome a{};
    bool has = s.TryGetLastBoardControlOutcome(a);
    std::printf(
        "[TERMINAL_QUICK_MENU] stage=%s bank=%u t=%u physical=%016llx expected=%016llx phase=%u "
        "menu=%u owner=%u selected=%u history=%u undo=%u ack_present=%u status=%u action=%u "
        "display=%.*s glyph_pixels=%u next=%s fen=%s\n",
        stage, bank, st.now_ms, (unsigned long long)st.physical_occupied_mask,
        (unsigned long long)st.expected_occupied_mask, st.interaction_phase,
        s.QuickSettingsActive(), s.QuickSettingsOwner(), s.QuickSettingsSelectedIndex(),
        s.UndoHistoryMoveCount(), s.IsUndoActionPending(), has, unsigned(a.status),
        unsigned(a.actionState), int(s.DisplayText().size()), s.DisplayText().data(),
        GlyphPixels(s), next, s.HomeStateSnapshot().fen);
    const auto& f = s.SemanticFrame();
    const auto& p = s.PhysicalFrame();
    std::printf("[QUICK_MENU_RGBW] stage=%s semantic_status=", stage);
    for (auto c : f.status)
        std::printf("%u,%u,%u;", c.r, c.g, c.b);
    std::printf(" mapped_status=");
    for (unsigned i = 256; i < p.led_count; ++i) {
        auto c = p.pixels[i];
        std::printf("%u,%u,%u,%u;", c.r, c.g, c.b, c.w);
    }
    std::puts("");
    if (std::string(stage) == "menu-read-window") {
        std::printf("[BOARD_RGBW] bank=%u stage=%s pixels=", bank, stage);
        for (unsigned i = 0; i < 256; ++i) {
            auto c = p.pixels[i];
            std::printf("%u,%u,%u,%u;", c.r, c.g, c.b, c.w);
        }
        std::puts("");
    }
}
// Physical-bank X has already debounced at20ms. Progress starts at that
// accepted edge, including while the X+O arbiter retains the ordinary event.
void CheckTerminalXHoldProgress(runtime::VirtualLitchSession& session,
                                const unsigned bank,
                                const unsigned elapsedSinceRawDown) {
    const auto& settings = session.SettingsForPersistenceTest();
    const uint8_t wheel = static_cast<uint8_t>(
        modules::player_end_palette::ReadSettingUInt32(settings,
            bank == 1U ? modules::ui_color_keys::P1_END_PIECE_WHEEL
                       : modules::ui_color_keys::P2_END_PIECE_WHEEL,
            bank == 1U ? modules::player_end_palette::P1_END_DEFAULT_WHEEL
                       : modules::player_end_palette::P2_END_DEFAULT_WHEEL));
    const auto bright = modules::ui_color_wheel::WheelToRgb(wheel);
    const modules::Rgb dim{static_cast<uint8_t>(bright.r / 4U),
                           static_cast<uint8_t>(bright.g / 4U),
                           static_cast<uint8_t>(bright.b / 4U)};
    const unsigned holdMs = elapsedSinceRawDown - 20U;
    const unsigned progress = holdMs * 255U / 1000U;
    const unsigned fill = (progress * 8U + 254U) / 255U;
    const unsigned base = bank == 1U ? 16U : 0U;
    unsigned semanticMatches = 0U, mappedMatches = 0U;
    const auto& physical = session.PhysicalFrame();
    const float scale = static_cast<float>(physical.applied_brightness_scale_65535) / 65535.0f;
    for (unsigned local = 0U; local < 8U; ++local) {
        // The composer applies its established1.25x status presentation boost.
        const auto expected = modules::ScaleRgbSaturatingBy256(
            local < fill ? bright : dim, 320U);
        const unsigned index = base + 7U - local;
        const auto actual = session.SemanticFrame().status[index];
        semanticMatches += actual.r == expected.r && actual.g == expected.g && actual.b == expected.b;
        const auto scaled = [scale](const uint8_t channel) {
            return static_cast<uint8_t>(static_cast<float>(channel) * scale + 0.5f);
        };
        const auto presented = modules::rgb_optical_calibration::PresentForS3(
            {scaled(expected.r), scaled(expected.g), scaled(expected.b)});
        const auto mapped = modules::AutoMapRgbToRgbw(presented, 32U);
        const auto pixel = physical.pixels[256U + index];
        const auto close = [](const uint8_t a, const uint8_t b) {
            return a <= b + 1U && b <= a + 1U;
        };
        mappedMatches += close(pixel.r, mapped.r) && close(pixel.g, mapped.g) &&
                         close(pixel.b, mapped.b) && close(pixel.w, mapped.w);
    }
    std::printf("[TERMINAL_X_HOLD_PROGRESS] bank=%u raw_elapsed_ms=%u held_ms=%u expected_bright=%u expected_dim=%u semantic_matches=%u/8 mapped_rgbw_matches=%u/8 scale=%u\n",
        bank, elapsedSinceRawDown, holdMs, fill, 8U-fill, semanticMatches, mappedMatches,
        physical.applied_brightness_scale_65535);
    Check(semanticMatches == 8U,
          "terminal raw X hold has immediate dim acknowledgement and proportional owner-color semantic fill");
    Check(mappedMatches == 8U,
          "terminal raw X hold has proportional mappedV3 RGBW owner-edge fill");
}

void TerminalQuickMenuJourney() {
    std::filesystem::create_directories("build");
    for (unsigned bank : {1U, 2U}) {
        const std::string path = "build/terminal-menu-" + std::to_string(bank) + ".json";
        std::filesystem::copy_file(std::filesystem::path(__FILE__).lexically_normal().parent_path().parent_path() /
                                       "data/terminal_checkmate_107.json",
                                   path, std::filesystem::copy_options::overwrite_existing);
        // Undo reopens a human turn. Keep its synchronous Hint work bounded
        // under this journey's frozen clock, as in the other menu journeys.
        test_support::BoundedHintEngine engine;
        virtual_litch_config cfg{};
        virtual_litch_default_config(&cfg);
        cfg.hardware = VIRTUAL_LITCH_HARDWARE_V3;
        runtime::VirtualLitchSession s(cfg, engine, path.c_str());
        const auto occ = s.State().expected_occupied_mask;
        s.ObserveBootHallMask(occ);
        s.Advance(200);
        const std::string uid =
            s.SettingsForPersistenceTest().Doc()["GAME_HISTORY_V1"][1].as<std::string>();
        const std::string fen = s.HomeStateSnapshot().fen;
        auto retained = [&] {
            return s.UndoHistoryMoveCount() == 107 &&
                   std::string(s.HomeStateSnapshot().fen) == fen &&
                   engine.IsInCheckmate(modules::SideToMove::Black) &&
                   s.State().physical_occupied_mask == occ &&
                   s.SettingsForPersistenceTest().Doc()["GAME_HISTORY_V1"][1].as<std::string>() ==
                       uid;
        };
        auto button = [&](runtime::VirtualLitchPhysicalButton b, bool down) {
            Check(s.SetButtonDown(bank, b, down) == VIRTUAL_LITCH_OK &&
                      s.Advance(20) == VIRTUAL_LITCH_OK,
                  "raw terminal button accepted");
        };
        auto tap = [&](runtime::VirtualLitchPhysicalButton b) {
            if (s.QuickSettingsActive() &&
                (b == runtime::VirtualLitchPhysicalButton::Left ||
                 b == runtime::VirtualLitchPhysicalButton::Right)) {
                Check(s.Advance(250U) == VIRTUAL_LITCH_OK,
                      "wait out the terminal Quick Setup vertical guard before side Press");
            }
            button(b, true);
            button(b, false);
        };
        TerminalCheckpoint(s, "mate-before-hold", bank, "hold X1000ms");
        button(runtime::VirtualLitchPhysicalButton::X, true);
        unsigned sampledAt = 20U;
        for (const unsigned sample : {20U, 200U, 400U, 600U, 800U}) {
            if (sample > sampledAt) s.Advance(sample - sampledAt);
            sampledAt = sample;
            CheckTerminalXHoldProgress(s, bank, sample);
            TerminalCheckpoint(s, "hold-progress", bank, "continue X to1000ms; release keeps Undo semantics");
            Check(!s.QuickSettingsActive() && retained(),
                  "partial X hold retains107 terminal game and waits until1sec");
        }
        s.Advance(20U);
        s.Advance(200);
        button(runtime::VirtualLitchPhysicalButton::X, false);
        s.Advance(200);
        TerminalCheckpoint(s, "menu-open", bank, "Left Right select; Up Down adjust; X exits");
        Check(s.QuickSettingsActive() && retained(),
              "raw hold X opens authoritative Quick Setup during checkmate without Undo");
        unsigned glyphMax = GlyphPixels(s), physicalGlyphMax = PhysicalGlyphPixels(s);
        for (unsigned frame = 0; frame < 20; ++frame) {
            s.Advance(50);
            glyphMax = std::max(glyphMax, GlyphPixels(s));
            physicalGlyphMax = std::max(physicalGlyphMax, PhysicalGlyphPixels(s));
        }
        TerminalCheckpoint(s, "menu-read-window", bank, "read scrolling CPU setting");
        std::printf("[GLYPH_WINDOW] bank=%u max_white_glyph_pixels=%u window_ms=1000\n", bank,
                    glyphMax);
        std::printf("[MAPPED_GLYPH_WINDOW] bank=%u max_physical_white_rgbw_pixels=%u\n", bank,
                    physicalGlyphMax);
        Check(glyphMax > 4 && physicalGlyphMax > 4,
              "Quick Setup glyph is visibly rendered semantically and mappedV3 above terminal "
              "celebration");
        for (unsigned n = 0; n < 3; ++n)
            tap(runtime::VirtualLitchPhysicalButton::Right);
        Check(s.QuickSettingsSelectedIndex() == 3, "terminal menu navigates to brightness");
        TerminalCheckpoint(s, "brightness-selected", bank, "Up then Down restores brightness");
        auto brightness =
            modules::white_squares_setting::ReadBrightness0To8(s.SettingsForPersistenceTest());
        tap(brightness < 8 ? runtime::VirtualLitchPhysicalButton::Up
                           : runtime::VirtualLitchPhysicalButton::Down);
        Check(modules::white_squares_setting::ReadBrightness0To8(s.SettingsForPersistenceTest()) !=
                  brightness,
              "terminal brightness control applies");
        tap(brightness < 8 ? runtime::VirtualLitchPhysicalButton::Down
                           : runtime::VirtualLitchPhysicalButton::Up);
        Check(modules::white_squares_setting::ReadBrightness0To8(s.SettingsForPersistenceTest()) ==
                  brightness,
              "terminal brightness control restores original value");
        Check(retained(), "ordinary terminal menu adjustments retain exact game");
        (void)brightness;
        tap(runtime::VirtualLitchPhysicalButton::X);
        s.Advance(200);
        TerminalCheckpoint(s, "menu-exit", bank, "terminal celebration resumes; hold X reopens");
        Check(!s.QuickSettingsActive() && retained(),
              "X exits menu without Undo and terminal game retained");
        button(runtime::VirtualLitchPhysicalButton::X, true);
        s.Advance(1000);
        button(runtime::VirtualLitchPhysicalButton::X, false);
        s.Advance(200);
        TerminalCheckpoint(s, "menu-reopened", bank,
                           "X exits; unchanged firmware timeout remains10s");
        Check(s.QuickSettingsActive() && retained(), "terminal Quick Setup reopens");
        tap(runtime::VirtualLitchPhysicalButton::X);
        s.Advance(200);
        Check(!s.QuickSettingsActive() && retained(), "explicit exit recovers after reentry");
        // A second face button inside the arbitration window must take over
        // the preview without opening Quick Setup or emitting an early Undo.
        button(runtime::VirtualLitchPhysicalButton::X, true);
        s.Advance(200);
        button(runtime::VirtualLitchPhysicalButton::O, true);
        s.Advance(400);
        TerminalCheckpoint(s, "partial-XO-chord", bank, "release both to cancel New Game hold");
        Check(!s.QuickSettingsActive() && retained(),
              "X+O arbitration preserves terminal game and suppresses Quick Setup");
        button(runtime::VirtualLitchPhysicalButton::X, false);
        button(runtime::VirtualLitchPhysicalButton::O, false);
        s.Advance(1000);
        Check(!s.QuickSettingsActive() && retained(),
              "released partial X+O chord has no delayed menu, Undo or New Game");
        tap(runtime::VirtualLitchPhysicalButton::X);
        s.Advance(1);
        modules::ControlMutationOutcome ack{};
        Check(s.TryGetLastBoardControlOutcome(ack) &&
                  ack.status == modules::ControlMutationStatus::Applied &&
                  ack.actionState == modules::ControlActionState::UndoPending &&
                  s.UndoHistoryMoveCount() == 106,
              "short X tap still applies Undo after terminal menu exit");
        TerminalCheckpoint(s, "short-X-Undo", bank, "lift a3 queen and replace d6");
        const auto lifted = occ & ~(1ULL << 40), pre = lifted | (1ULL << 19);
        s.ObserveHallMask(lifted);
        s.ObserveHallMask(pre);
        s.Advance(1);
        Check(!s.IsUndoActionPending(), "short-tap Undo recovers through physical queen return");
        s.ObserveHallMask(lifted);
        s.ObserveHallMask(occ);
        s.Advance(2000);
        Check(retained(), "physical remate restores exact107 terminal game");
        TerminalCheckpoint(s, "remate-recovered", bank, "hold X opens Quick Setup again");
        button(runtime::VirtualLitchPhysicalButton::X, true);
        s.Advance(500);
        button(runtime::VirtualLitchPhysicalButton::X, false);
        s.Advance(1);
        Check(
            !s.QuickSettingsActive() && s.IsUndoActionPending() && s.UndoHistoryMoveCount() == 106,
            "canceled subsecond X hold retains normal Undo semantics");
        TerminalCheckpoint(s, "early-hold-release-Undo", bank, "return queen d6 to complete Undo");
        s.ObserveHallMask(lifted);
        s.ObserveHallMask(pre);
        s.Advance(1);
        s.ObserveHallMask(lifted);
        s.ObserveHallMask(occ);
        s.Advance(2000);
        Check(retained(),
              "early hold release recovers exact107 through physical return and remate");
    }
}

}  // namespace

int main() {
    std::puts("[MENU_ROTATION_VIRTUAL_LITCH_JOURNEY] start");
    V3_3LocalDirectionMenuJourney();
    TerminalQuickMenuJourney();
    CheckNoOldOrientationGhost(hardware::BoardRevision::V2S3);
    CheckNoOldOrientationGhost(hardware::BoardRevision::V3S3);
    CheckExplicitCoverageSurvivesSameColorAndBlack();
    CheckV1StatusKeepsPriority();
    CheckFadeUsesDestinationBackground();
    CheckWhitePreviewClipsDestinationOccupancy();
    const MenuCapture unrotatedP1 = OpenMenuAndCapture(false, false, false);
    const MenuCapture unrotatedP1Swapped = OpenMenuAndCapture(false, true, false);
    const MenuCapture rotatedP1 = OpenMenuAndCapture(true, false, false);
    const MenuCapture rotatedP1Swapped = OpenMenuAndCapture(true, true, false);
    const MenuCapture unrotatedP2 = OpenMenuAndCapture(false, false, true);
    const MenuCapture unrotatedP2Swapped = OpenMenuAndCapture(false, true, true);
    const MenuCapture rotatedP2 = OpenMenuAndCapture(true, false, true);
    const MenuCapture rotatedP2Swapped = OpenMenuAndCapture(true, true, true);

    Check(unrotatedP1.boardMask != 0ULL &&
              unrotatedP1.boardMask != Rotate180(unrotatedP1.boardMask),
          "Quick Setup foreground is visible and orientation-sensitive");
    Check(unrotatedP1Swapped.boardMask == unrotatedP1.boardMask,
          "fixed P1-bank menu ignores player-end placement");
    Check(rotatedP1.boardMask == unrotatedP1.boardMask &&
              rotatedP1Swapped.boardMask == unrotatedP1.boardMask,
          "fixed P1-bank menu stays upright for its user in every orientation");
    Check(unrotatedP2.boardMask == Rotate180(unrotatedP1.boardMask),
          "fixed P2-bank menu reads from the opposite board edge");
    Check(unrotatedP2Swapped.boardMask == unrotatedP2.boardMask &&
              rotatedP2.boardMask == unrotatedP2.boardMask &&
              rotatedP2Swapped.boardMask == unrotatedP2.boardMask,
          "fixed P2-bank menu stays upright for its user in every orientation");
    Check(unrotatedP1.physicalMarker == 23U &&
              unrotatedP1Swapped.physicalMarker == unrotatedP1.physicalMarker &&
              rotatedP1.physicalMarker == unrotatedP1.physicalMarker &&
              rotatedP1Swapped.physicalMarker == unrotatedP1.physicalMarker,
          "fixed P1-bank menu perimeter stays with its user in every orientation");
    Check(unrotatedP2.physicalMarker == 7U &&
              unrotatedP2Swapped.physicalMarker == unrotatedP2.physicalMarker &&
              rotatedP2.physicalMarker == unrotatedP2.physicalMarker &&
              rotatedP2Swapped.physicalMarker == unrotatedP2.physicalMarker,
          "fixed P2-bank menu perimeter stays with its user in every orientation");
    Check(unrotatedP1Swapped.statusHash == unrotatedP1.statusHash &&
              rotatedP1Swapped.statusHash == rotatedP1.statusHash &&
              unrotatedP2Swapped.statusHash == unrotatedP2.statusHash &&
              rotatedP2Swapped.statusHash == rotatedP2.statusHash,
          "player placement preserves the menu's assigned physical-end colors");

    if (g_failures != 0) {
        std::printf("[MENU_ROTATION_VIRTUAL_LITCH_JOURNEY] failures=%d\n",
                    g_failures);
        return 1;
    }
    std::puts(
        "[MENU_ROTATION_VIRTUAL_LITCH_JOURNEY] ok; "
        "observable=Quick-Setup-board-text; authority=fixed-physical-owner; "
        "recovery=close-with-mapped-X");
    return 0;
}
