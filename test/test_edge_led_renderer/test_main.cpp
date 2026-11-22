#include <array>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/edge_led_cue.hpp"
#include "modules/edge_led_intent.hpp"
#include "modules/edge_led_layer.hpp"
#include "modules/edge_led_renderer.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/status_led_frame_adapter.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

bool AnyLit(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& leds) {
    for (const auto& c : leds) {
        if (c.r != 0 || c.g != 0 || c.b != 0) {
            return true;
        }
    }
    return false;
}

uint32_t TotalRed(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& leds) {
    uint32_t total = 0;
    for (const auto& c : leds) {
        total += c.r;
    }
    return total;
}

bool IsLit(const modules::Rgb& c) {
    return (c.r != 0) || (c.g != 0) || (c.b != 0);
}

uint16_t CountLit(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& leds) {
    uint16_t lit = 0;
    for (const auto& c : leds) {
        if (IsLit(c)) {
            ++lit;
        }
    }
    return lit;
}

uint32_t Intensity(const modules::Rgb& c) {
    return static_cast<uint32_t>(c.r) + static_cast<uint32_t>(c.g) + static_cast<uint32_t>(c.b);
}

uint32_t TotalIntensityInRange(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& leds,
                               const uint8_t start,
                               const uint8_t count) {
    uint32_t total = 0;
    const uint8_t end = static_cast<uint8_t>(start + count);
    for (uint8_t i = start; i < end; ++i) {
        total += Intensity(leds[i]);
    }
    return total;
}

uint8_t BrightestIndexInRange(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& leds,
                              const uint8_t start,
                              const uint8_t count) {
    const uint8_t end = static_cast<uint8_t>(start + count);
    uint8_t bestIndex = start;
    uint32_t bestIntensity = 0;
    for (uint8_t i = start; i < end; ++i) {
        const uint32_t current = Intensity(leds[i]);
        if (current > bestIntensity) {
            bestIntensity = current;
            bestIndex = i;
        }
    }
    return bestIndex;
}

bool AllOffOutsideRange(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& leds,
                        const uint8_t start,
                        const uint8_t count) {
    const uint8_t end = static_cast<uint8_t>(start + count);
    for (uint8_t i = 0; i < modules::EDGE_LED_COUNT; ++i) {
        const bool inRange = (i >= start) && (i < end);
        if (!inRange && IsLit(leds[i])) {
            return false;
        }
    }
    return true;
}

bool AllOffOutsideTwoRanges(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& leds,
                            const uint8_t start1,
                            const uint8_t count1,
                            const uint8_t start2,
                            const uint8_t count2) {
    const uint8_t end1 = static_cast<uint8_t>(start1 + count1);
    const uint8_t end2 = static_cast<uint8_t>(start2 + count2);

    for (uint8_t i = 0; i < modules::EDGE_LED_COUNT; ++i) {
        const bool in1 = (i >= start1) && (i < end1);
        const bool in2 = (i >= start2) && (i < end2);
        if (!in1 && !in2 && IsLit(leds[i])) {
            return false;
        }
    }
    return true;
}

bool AllOffInRange(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& leds,
                   const uint8_t start,
                   const uint8_t count) {
    const uint8_t end = static_cast<uint8_t>(start + count);
    for (uint8_t i = start; i < end; ++i) {
        if (IsLit(leds[i])) {
            return false;
        }
    }
    return true;
}

bool AnyLitInRange(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& leds,
                   const uint8_t start,
                   const uint8_t count) {
    const uint8_t end = static_cast<uint8_t>(start + count);
    for (uint8_t i = start; i < end; ++i) {
        if (IsLit(leds[i])) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    std::puts("[EDGE_LED] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    modules::EdgeLedRenderer r;
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> leds;

    // Turn indicator (active_end_only) lights the active home edge + both side edges.
    modules::EdgeLedState st;
    st.mode = modules::EdgeUiMode::IdleTurn;
    st.activeSide = modules::UiOwner::P1;
    st.turnAnimStyle = modules::TurnAnimStyle::ActiveEndOnly;
    fxe::ManualMillis::SetNowMs(800);
    r.Render(st, leds);
    Check(AnyLit(leds), "turn indicator emits light");
    Check(AnyLitInRange(leds, 16, 8), "turn indicator lights active home edge");
    Check(AnyLitInRange(leds, 8, 8), "turn indicator lights east edge");
    Check(AnyLitInRange(leds, 24, 8), "turn indicator lights west edge");
    Check(AllOffInRange(leds, 0, 8), "turn indicator keeps opponent home edge off");

    // Thinking now belongs to the control bank; no retired perimeter scanner.
    st.mode = modules::EdgeUiMode::Thinking;
    for (const uint32_t time : {0U, 200U, 800U, 1600U}) {
        fxe::ManualMillis::SetNowMs(time);
        r.Render(st, leds);
        Check(!AnyLit(leds), "reserved Thinking mode never emits the retired perimeter scanner");
    }

    // Menu owner banner should be present.
    st.mode = modules::EdgeUiMode::Menu;
    st.owner = modules::UiOwner::P2;
    st.menuSelectionIndex = 7;
    st.menuValue01_255 = 255;
    fxe::ManualMillis::SetNowMs(0);
    r.Render(st, leds);
    Check(AnyLit(leds), "menu owner banner emits light");
    Check(IsLit(leds[0]), "menu selection indicator lights bottom-right for option index 7");
    Check(IsLit(leds[0]), "menu owner banner lights the home edge");

    // Error flashes on/off.
    st.mode = modules::EdgeUiMode::Error;
    st.activeSide = modules::UiOwner::P1;
    fxe::ManualMillis::SetNowMs(0);
    r.Render(st, leds);
    Check(AnyLit(leds), "error on phase emits light");
    const uint32_t redAtStart = TotalRed(leds);
    fxe::ManualMillis::SetNowMs(150);
    r.Render(st, leds);
    Check(TotalRed(leds) < redAtStart, "error pulse fades down before the dark phase");
    fxe::ManualMillis::SetNowMs(300);
    r.Render(st, leds);
    Check(!AnyLit(leds), "error off phase emits no light");

    // Paused health is a persistent dashed warning with a calm cadence. Its
    // least-salient phase remains spatially visible instead of going dark.
    st.mode = modules::EdgeUiMode::HealthPaused;
    fxe::ManualMillis::SetNowMs(0U);
    r.Render(st, leds);
    const auto healthStart = leds;
    Check(CountLit(leds) == 16U,
          "paused health renders eight persistent perimeter dashes");
    fxe::ManualMillis::SetNowMs(800U);
    r.Render(st, leds);
    Check(CountLit(leds) == 16U,
          "paused health least-salient cadence phase stays visible");
    Check(!(healthStart[0].r == leds[0].r &&
            healthStart[0].g == leds[0].g &&
            healthStart[0].b == leds[0].b),
          "paused health cue uses a measured cadence");

    // Usable health alternates beside the physical USB port for one second,
    // then stays quiet for the rest of its five-second period.
    st.mode = modules::EdgeUiMode::HealthDegradedUsable;
    fxe::ManualMillis::SetNowMs(0U);
    r.Render(st, leds);
    const auto usableStart = leds;
    modules::BoardLedFrame emptyBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> ordinaryStatus{};
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        ordinaryStatus[index] = modules::Rgb{index, 0U, 0U};
    }
    const auto rotatedOrdinary = hardware::MapSemanticLedFrame(
        {hardware::BoardRevision::V2S3, true, true},
        emptyBoard, ordinaryStatus);
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay healthOverlay{};
    healthOverlay.Set(27U, usableStart[27U]);
    healthOverlay.Set(28U, usableStart[28U]);
    auto rotatedHealth = rotatedOrdinary;
    hardware::ApplyFixedPhysicalStatusOverlay(
        hardware::BoardRevision::V2S3, healthOverlay, rotatedHealth);
    Check(CountLit(usableStart) == 1U && IsLit(usableStart[27]) &&
              usableStart[27].r == 255U && usableStart[27].g == 255U,
          "usable health starts yellow beside USB");
    fxe::ManualMillis::SetNowMs(250U);
    r.Render(st, leds);
    Check(CountLit(leds) == 1U && IsLit(leds[28]) &&
              leds[28].r == 255U && leds[28].g == 128U,
          "usable health alternates orange to the neighboring USB-side LED");
    fxe::ManualMillis::SetNowMs(1000U);
    r.Render(st, leds);
    Check(!AnyLit(leds), "usable health rests after its one-second warning");
    fxe::ManualMillis::SetNowMs(5000U);
    r.Render(st, leds);
    Check(CountLit(leds) == 1U &&
              leds[27].r == usableStart[27].r &&
              leds[27].g == usableStart[27].g &&
              leds[27].b == usableStart[27].b,
          "usable health repeats every five seconds");
    fxe::ManualMillis::SetNowMs(0U);
    r.Render(st, leds);
    Check(CountLit(leds) == 1U &&
              rotatedHealth[hardware::BOARD_LED_COUNT + 27U].r != 0U &&
              rotatedHealth[hardware::BOARD_LED_COUNT + 11U].r == 27U &&
              rotatedHealth[hardware::BOARD_LED_COUNT + 12U].r == 28U,
          "post-map health keeps USB placement without dropping rotated ordinary pixels");
    Check(CountLit(healthStart) != CountLit(usableStart),
          "usable and paused health have distinct spatial geometry");

    // Fatal victory: only the winner edge carries the expanding sparks.
    st.mode = modules::EdgeUiMode::FatalCheckmate;
    st.activeSide = modules::UiOwner::P1;
    st.gameOverElapsedMs = 0U;
    fxe::ManualMillis::SetNowMs(0);
    r.Render(st, leds);
    Check(AnyLit(leds), "fatal checkmate emits light");
    Check(CountLit(leds) == 8U && AllOffOutsideRange(leds, 16U, 8U),
          "fatal victory leaves only the uninterrupted winner edge lit");

    // Winner color remains continuously visible between the brighter sparks.
    const uint8_t winnerBase = 16;
    const modules::Rgb c0 = leds[winnerBase + 0];
    Check(IsLit(c0) && TotalIntensityInRange(leds, winnerBase, 8U) > 8U * 96U,
          "fatal victory has a bright winner-colored edge");

    st.gameOverElapsedMs = 1100U;
    fxe::ManualMillis::SetNowMs(9100U);
    r.Render(st, leds);
    const modules::Rgb c1 = leds[winnerBase + 0];
    Check(!(c0.r == c1.r && c0.g == c1.g && c0.b == c1.b), "fatal checkmate animates winner edge");
    for (const auto owner : {modules::UiOwner::P1, modules::UiOwner::P2}) {
            st.activeSide = owner;
            const uint8_t base = owner == modules::UiOwner::P2 ? 0U : 16U;
            for (const bool minimal : {false, true}) {
                st.minimalAnimations = minimal;
                for (uint32_t elapsed = 0U; elapsed <= 3300U; elapsed += 50U) {
                    st.gameOverElapsedMs = elapsed;
                    r.Render(st, leds);
                    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
                        Check(IsLit(leds[index]) == (index >= base && index < base + 8U),
                              "victory stays confined through full bursts and minimal output");
                    }
                }
            }
    }
    st.activeSide = modules::UiOwner::P1;
    st.minimalAnimations = false;

    st.gameOverElapsedMs = 0U;
    r.Render(st, leds);
    Check(CountLit(leds) == 8U &&
              AllOffOutsideRange(leds, 16U, 8U) &&
              IsLit(leds[18U]) && IsLit(leds[21U]),
          "winner sparks keep the former C/F cue positions in the result color");

    // Fatal draw: calm neutral ring with both home edges still present.
    st.mode = modules::EdgeUiMode::FatalDraw;
    st.gameOverElapsedMs = 0U;
    fxe::ManualMillis::SetNowMs(0);
    r.Render(st, leds);
    Check(AnyLit(leds), "fatal draw emits light");
    Check(CountLit(leds) == modules::EDGE_LED_COUNT, "fatal draw lights full ring");
    Check(IsLit(leds[0]), "fatal draw lights P2 home edge");
    Check(IsLit(leds[16]), "fatal draw lights P1 home edge");
    Check(!(leds[0].r == leds[16].r && leds[0].g == leds[16].g && leds[0].b == leds[16].b),
          "fatal draw keeps player home edges distinct");

    // Setup completion: both physical-end colors sweep across the full perimeter.
    st.mode = modules::EdgeUiMode::SetupCompleteSweep;
    st.effectStartMs = 1000;
    fxe::ManualMillis::SetNowMs(1000);
    r.Render(st, leds);
    Check(CountLit(leds) == 4,
          "setup-complete sweep starts at both player-edge centers");
    Check(leds[3].r > leds[3].g && leds[3].r > leds[3].b &&
              leds[19].b > leds[19].r && leds[19].b > leds[19].g,
          "setup-complete sweep uses the two player palettes, not generic green");

    fxe::ManualMillis::SetNowMs(1075);
    r.Render(st, leds);
    Check(CountLit(leds) > 4 &&
              leds[2].r < leds[3].r &&
              leds[18].b < leds[19].b,
          "setup-complete sweep eases away from both player-edge centers");

    fxe::ManualMillis::SetNowMs(1300);
    r.Render(st, leds);
    Check(CountLit(leds) > 4 &&
              CountLit(leds) < modules::EDGE_LED_COUNT,
          "setup-complete sweep remains followable midway toward the side edges");
    fxe::ManualMillis::SetNowMs(1850);
    r.Render(st, leds);
    const uint32_t beforeTurnaroundIntensity = TotalIntensityInRange(
        leds, 0U, modules::EDGE_LED_COUNT);
    fxe::ManualMillis::SetNowMs(1900);
    r.Render(st, leds);
    Check(CountLit(leds) == modules::EDGE_LED_COUNT,
          "setup-complete sweep meets at the side-edge centers");
    const uint32_t turnaroundIntensity = TotalIntensityInRange(
        leds, 0U, modules::EDGE_LED_COUNT);
    fxe::ManualMillis::SetNowMs(1950);
    r.Render(st, leds);
    Check(beforeTurnaroundIntensity < turnaroundIntensity &&
              TotalIntensityInRange(leds, 0U, modules::EDGE_LED_COUNT) <
                  turnaroundIntensity,
          "setup-complete sweep reverses through an eased apex without a hold");
    fxe::ManualMillis::SetNowMs(2350);
    r.Render(st, leds);
    Check(CountLit(leds) > 4 &&
              CountLit(leds) < modules::EDGE_LED_COUNT,
          "setup-complete sweep reverses toward the player ends");
    fxe::ManualMillis::SetNowMs(2799);
    r.Render(st, leds);
    Check(CountLit(leds) == 4,
          "setup-complete sweep returns to both player-edge centers before expiry");

    // A calm unavailable cue is sparse, neutral, and confined to the active
    // home edge. It is rendered by the same catalogue as every other edge
    // animation and can be layered over the current Turn indicator.
    st = modules::EdgeLedState{};
    st.mode = modules::EdgeUiMode::UnavailableNudge;
    st.activeSide = modules::UiOwner::P1;
    st.effectStartMs = 2000U;
    fxe::ManualMillis::SetNowMs(2000U);
    r.Render(st, leds);
    Check(CountLit(leds) == 4U &&
              AllOffOutsideRange(leds, 16U, 8U),
          "unavailable cue uses four pixels on the active home edge");
    Check(leds[16].r == leds[16].g &&
              leds[16].g == leds[16].b,
          "unavailable cue stays neutral instead of using prompt yellow");

    modules::EdgeLedCueController cueController;
    cueController.Show(modules::EdgeLedCueKind::Unavailable,
                       modules::UiOwner::P1,
                       100U);
    Check(cueController.Current(579U).IsActive(),
          "unavailable cue remains active through 479 ms");
    Check(!cueController.Current(580U).IsActive(),
          "unavailable cue expires at 480 ms");
    st = modules::EdgeLedState{};
    st.mode = modules::EdgeUiMode::OccupiedBoardWarning;
    st.effectStartMs = 1000U;
    fxe::ManualMillis::SetNowMs(1000U);
    r.Render(st, leds);
    Check(CountLit(leds) == modules::EDGE_LED_COUNT &&
              leds[0].r > leds[0].g && leds[0].g > leds[0].b,
          "occupied-board warning fills the status perimeter in amber");
    const auto occupiedWarningBright = leds[0];
    fxe::ManualMillis::SetNowMs(1200U);
    r.Render(st, leds);
    Check(leds[0].r < occupiedWarningBright.r &&
              leds[0].g < occupiedWarningBright.g,
          "occupied-board warning pulses without losing its amber meaning");
    cueController.Show(modules::EdgeLedCueKind::OccupiedBoardBlocked,
                       modules::UiOwner::None,
                       100U);
    Check(cueController.Current(1099U).IsActive() &&
              !cueController.Current(1100U).IsActive(),
          "occupied-board warning lasts exactly one second");
    cueController.Show(modules::EdgeLedCueKind::SetupComplete,
                       modules::UiOwner::None,
                       1000U);
    Check(cueController.Current(2799U).kind ==
              modules::EdgeLedCueKind::SetupComplete,
          "setup completion remains active through 1799 ms");
    Check(!cueController.Current(2800U).IsActive(),
          "setup completion expires at 1800 ms");
    Check(modules::EdgeLedCueController::SetupCompleteLayerAlpha255(1559U) ==
                  255U &&
              modules::EdgeLedCueController::SetupCompleteLayerAlpha255(1560U) ==
                  255U,
          "setup completion fully owns the perimeter until its handoff begins");
    Check(modules::EdgeLedCueController::SetupCompleteLayerAlpha255(1680U) ==
                  128U,
          "setup completion crossfades halfway into Turn at 1680 ms");
    Check(modules::EdgeLedCueController::SetupCompleteLayerAlpha255(1799U) ==
                  1U &&
              modules::EdgeLedCueController::SetupCompleteLayerAlpha255(1800U) ==
                  0U,
          "setup completion reaches Turn without a final-frame ownership snap");

    std::array<modules::Rgb, modules::EDGE_LED_COUNT> layered{};
    layered.fill(modules::Rgb{10U, 0U, 0U});
    modules::EdgeLedLayer overlay;
    overlay.mode = modules::EdgeLedLayerMode::Overlay;
    overlay.pixels[0] = modules::Rgb{0U, 20U, 0U};
    modules::ApplyEdgeLedLayer(overlay, layered);
    Check(layered[0].r == 10U && layered[0].g == 20U &&
              layered[1].r == 10U,
          "edge layer overlay preserves and augments the base frame");

    modules::EdgeLedLayer replacement;
    replacement.mode = modules::EdgeLedLayerMode::Replace;
    replacement.pixels[0] = modules::Rgb{0U, 0U, 30U};
    modules::ApplyEdgeLedLayer(replacement, layered);
    Check(layered[0].r == 0U && layered[0].g == 0U &&
              layered[0].b == 30U && !IsLit(layered[1]),
          "edge layer replacement can fully replace every base pixel");

    // Brightness preview fills a bar.
    st.mode = modules::EdgeUiMode::BrightnessPreview;
    st.owner = modules::UiOwner::P1;
    st.value01_8 = 3;
    fxe::ManualMillis::SetNowMs(0);
    r.Render(st, leds);
    Check(AnyLit(leds), "brightness preview emits light");
    Check(AllOffOutsideRange(leds, 8, 8), "brightness preview is confined to east edge (8..15)");
    Check(IsLit(leds[15]), "brightness preview top accent is lit");

    // Placement mode is intentionally quiet; board squares own move guidance.
    st.mode = modules::EdgeUiMode::Placement;
    st.activeSide = modules::UiOwner::P2;
    fxe::ManualMillis::SetNowMs(0);
    r.Render(st, leds);
    Check(!AnyLit(leds), "placement emits no light by default (home-edge prompt disabled)");

    // Beginner-mode badges wrap across the home-edge and side-edge corners without replacing normal modes.
    st.mode = modules::EdgeUiMode::Placement;
    st.activeSide = modules::UiOwner::P1;
    st.beginnerModeP1Enabled = true;
    st.beginnerModeP2Enabled = false;
    st.beginnerModeStingerOwner = modules::UiOwner::None;
    st.beginnerModeStingerEnabling = false;
    st.beginnerModeStingerStartMs = 0;
    fxe::ManualMillis::SetNowMs(500);
    r.Render(st, leds);
    Check(CountLit(leds) == 4, "beginner badge lights exactly four corner LEDs for one player");
    Check(IsLit(leds[16]) && IsLit(leds[23]), "beginner badge lights P1 home-edge corners");
    Check(IsLit(leds[15]) && IsLit(leds[24]), "beginner badge lights P1 adjacent side-edge corners");

    st.beginnerModeP2Enabled = true;
    fxe::ManualMillis::SetNowMs(950);
    r.Render(st, leds);
    Check(CountLit(leds) == 8, "beginner badges light both players' corner L-shapes");
    Check(IsLit(leds[0]) && IsLit(leds[7]), "beginner badge lights P2 home-edge corners");
    Check(IsLit(leds[8]) && IsLit(leds[31]), "beginner badge lights P2 adjacent side-edge corners");

    // Enable stinger splits outward from the home-edge center toward both corners.
    st.beginnerModeP1Enabled = false;
    st.beginnerModeP2Enabled = false;
    st.beginnerModeStingerOwner = modules::UiOwner::P1;
    st.beginnerModeStingerEnabling = true;
    st.beginnerModeStingerStartMs = 1000;
    fxe::ManualMillis::SetNowMs(1000);
    r.Render(st, leds);
    const uint8_t enableStartIndex = BrightestIndexInRange(leds, 16, 8);
    Check(enableStartIndex >= 19U && enableStartIndex <= 20U,
          "beginner enable stinger starts near the home-edge center");

    fxe::ManualMillis::SetNowMs(1660);
    r.Render(st, leds);
    const uint8_t enableLateIndex = BrightestIndexInRange(leds, 16, 8);
    Check(enableLateIndex <= 18U || enableLateIndex >= 21U,
          "beginner enable stinger moves out of the home-edge center band");
    const uint32_t enableLateIntensity = TotalIntensityInRange(leds, 16, 8);

    fxe::ManualMillis::SetNowMs(1710);
    r.Render(st, leds);
    Check(TotalIntensityInRange(leds, 16, 8) < enableLateIntensity,
          "beginner enable stinger fades out as it reaches the corners");

    // Disable stinger converges from both home-edge corners toward the center.
    st.beginnerModeStingerEnabling = false;
    fxe::ManualMillis::SetNowMs(1000);
    r.Render(st, leds);
    const uint8_t disableStartIndex = BrightestIndexInRange(leds, 16, 8);
    Check((disableStartIndex == 16U || disableStartIndex == 23U),
          "beginner disable stinger starts from the outer P1 corners");
    Check(IsLit(leds[16]) && IsLit(leds[23]),
          "beginner disable stinger lights both home-edge corners together at the start");

    fxe::ManualMillis::SetNowMs(1600);
    r.Render(st, leds);
    const uint8_t disableLateIndex = BrightestIndexInRange(leds, 16, 8);
    Check(disableLateIndex >= 19U && disableLateIndex <= 20U,
          "beginner disable stinger lands on the home-edge center as it completes");
    const uint32_t disableLateIntensity = TotalIntensityInRange(leds, 16, 8);

    fxe::ManualMillis::SetNowMs(1710);
    r.Render(st, leds);
    Check(TotalIntensityInRange(leds, 16, 8) < disableLateIntensity,
          "beginner disable stinger fades out as it reaches the center");

    if (g_failures == 0) {
        std::puts("[EDGE_LED] ok");
        return 0;
    }

    std::printf("[EDGE_LED] failures=%d\n", g_failures);
    return 1;
}
