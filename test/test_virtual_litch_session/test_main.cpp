#include "bounded_hint_engine.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include "virtual_litch_session.hpp"
#include "virtual_litch_session_internal.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/led_mapping.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/led_render_policy.hpp"
#include "virtual_litch_frame_checks.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/correction_feedback.hpp"
#include "modules/game_variant.hpp"
#include "modules/promotion_piece_cue.hpp"
#include "modules/home_state_wire_codec.hpp"
#include "modules/settle_animation_setting.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/virtual_litch_session.h"
#include "runtime/interaction_domain_types.hpp"

static_assert(sizeof(virtual_litch_config) == 28U,
              "append-only control ABI must not resize legacy config");
static_assert(sizeof(virtual_litch_sleep_lifecycle) == 32U,
              "sleep lifecycle ABI has an explicit fixed v1 layout");
static_assert(sizeof(virtual_litch_runtime_health_fact) == 16U,
              "runtime-health fact ABI has an explicit fixed v1 layout");
static_assert(sizeof(virtual_litch_runtime_health) == 28U,
              "runtime-health snapshot ABI has an explicit fixed v1 layout");

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

virtual_litch* CreateBoard(virtual_litch_config config) {
    virtual_litch* board = nullptr;
    Check(virtual_litch_create(&config, &board) == VIRTUAL_LITCH_OK,
          "session creation succeeds");
    Check(board != nullptr, "session creation returns a handle");
    return board;
}

virtual_litch_config ConfigFor(const uint8_t hardware,
                                  const uint16_t historyCapacity = 8U) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardware;
    config.history_capacity = historyCapacity;
    return config;
}

virtual_litch_sleep_lifecycle GetSleep(const virtual_litch* board) {
    virtual_litch_sleep_lifecycle lifecycle{};
    virtual_litch_default_sleep_lifecycle(&lifecycle);
    Check(virtual_litch_get_sleep_lifecycle(board, &lifecycle) ==
              VIRTUAL_LITCH_OK,
          "sleep lifecycle query succeeds");
    return lifecycle;
}

virtual_litch_runtime_health GetRuntimeHealth(
    const virtual_litch* board) {
    virtual_litch_runtime_health health{};
    virtual_litch_default_runtime_health(&health);
    Check(virtual_litch_get_runtime_health(board, &health) ==
              VIRTUAL_LITCH_OK,
          "runtime-health query succeeds");
    return health;
}

bool AnyWhiteChannel(const virtual_litch_physical_frame& frame) {
    for (uint16_t index = 0U; index < frame.led_count; ++index) {
        if (frame.pixels[index].w != 0U) {
            return true;
        }
    }
    return false;
}

uint16_t ColorEnergy(const virtual_litch_color color) {
    return static_cast<uint16_t>(color.r) + color.g + color.b + color.w;
}

bool SameColor(const virtual_litch_color left,
               const virtual_litch_color right) {
    return left.r == right.r && left.g == right.g && left.b == right.b &&
           left.w == right.w;
}

uint16_t ChannelDistance(const uint8_t left, const uint8_t right) {
    return left > right ? static_cast<uint16_t>(left - right)
                        : static_cast<uint16_t>(right - left);
}

uint32_t StatusFrameDistance(
    const virtual_litch_semantic_frame& left,
    const virtual_litch_semantic_frame& right) {
    uint32_t distance = 0U;
    for (uint8_t index = 0U; index < VIRTUAL_LITCH_STATUS_LED_COUNT;
         ++index) {
        distance += ChannelDistance(left.status[index].r, right.status[index].r);
        distance += ChannelDistance(left.status[index].g, right.status[index].g);
        distance += ChannelDistance(left.status[index].b, right.status[index].b);
        distance += ChannelDistance(left.status[index].w, right.status[index].w);
    }
    return distance;
}

uint32_t DedicatedPhysicalStatusDistance(
    const virtual_litch_physical_frame& left,
    const virtual_litch_physical_frame& right) {
    if (left.led_count != right.led_count ||
        left.led_count < hardware::BOARD_LED_COUNT +
                             VIRTUAL_LITCH_STATUS_LED_COUNT) {
        return UINT32_MAX;
    }
    uint32_t distance = 0U;
    for (uint8_t index = 0U; index < VIRTUAL_LITCH_STATUS_LED_COUNT;
         ++index) {
        const uint16_t physicalIndex =
            static_cast<uint16_t>(hardware::BOARD_LED_COUNT + index);
        distance += ChannelDistance(left.pixels[physicalIndex].r,
                                    right.pixels[physicalIndex].r);
        distance += ChannelDistance(left.pixels[physicalIndex].g,
                                    right.pixels[physicalIndex].g);
        distance += ChannelDistance(left.pixels[physicalIndex].b,
                                    right.pixels[physicalIndex].b);
        distance += ChannelDistance(left.pixels[physicalIndex].w,
                                    right.pixels[physicalIndex].w);
    }
    return distance;
}

bool IsRedDominant(const virtual_litch_color color) {
    return color.r != 0U && color.r > color.g && color.r > color.b;
}

bool IsBlueDominant(const virtual_litch_color color) {
    return color.b != 0U && color.b > color.r && color.b > color.g;
}

bool IsExactYellow(const virtual_litch_color color) {
    return color.r == 255U && color.g == 255U && color.b == 0U &&
           color.w == 0U;
}

size_t CountExactYellowPieceCenters(
    const virtual_litch_semantic_frame& frame) {
    size_t count = 0U;
    for (uint8_t square = 0U; square < 64U; ++square) {
        count += IsExactYellow(
                     frame.board[square][static_cast<uint8_t>(
                         hardware::SquareLedSlot::Piece)])
                     ? 1U
                     : 0U;
    }
    return count;
}

using JourneySession = runtime::virtual_litch_internal::Session;

void PrintJourneyCheckpoint(const JourneySession& session,
                            const char* step,
                            const char* input,
                            const char* next);

size_t LitStatusPixelCount(const virtual_litch_semantic_frame& frame) {
    size_t count = 0U;
    for (const virtual_litch_color color : frame.status) {
        count += ColorEnergy(color) != 0U ? 1U : 0U;
    }
    return count;
}

uint32_t LitStatusPixelMask(
    const virtual_litch_semantic_frame& frame) {
    uint32_t mask = 0U;
    for (uint8_t index = 0U; index < VIRTUAL_LITCH_STATUS_LED_COUNT;
         ++index) {
        if (ColorEnergy(frame.status[index]) != 0U) {
            mask |= UINT32_C(1) << index;
        }
    }
    return mask;
}

uint32_t LitDedicatedPhysicalStatusPixelMask(
    const virtual_litch_physical_frame& physical) {
    uint32_t mask = 0U;
    for (uint8_t index = 0U; index < VIRTUAL_LITCH_STATUS_LED_COUNT;
         ++index) {
        const uint16_t physicalIndex =
            static_cast<uint16_t>(hardware::BOARD_LED_COUNT + index);
        if (physicalIndex < physical.led_count &&
            ColorEnergy(physical.pixels[physicalIndex]) != 0U) {
            mask |= UINT32_C(1) << index;
        }
    }
    return mask;
}

uint32_t LitDedicatedPhysicalStatusPixelMask(
    const JourneySession& session) {
    return LitDedicatedPhysicalStatusPixelMask(session.PhysicalFrame());
}

uint64_t HighIntensityPhysicalBoardSquareMask(
    const virtual_litch_physical_frame& frame) {
    uint64_t mask = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        const uint16_t base = static_cast<uint16_t>(
            square * hardware::LEDS_PER_SQUARE);
        for (uint8_t slot = 0U; slot < hardware::LEDS_PER_SQUARE;
             ++slot) {
            if (base + slot < frame.led_count &&
                ColorEnergy(frame.pixels[base + slot]) >= 128U) {
                mask |= UINT64_C(1) << square;
                break;
            }
        }
    }
    return mask;
}

uint64_t HighIntensitySemanticBoardSurfaceMask(
    const virtual_litch_semantic_frame& frame,
    const uint16_t threshold = 96U) {
    uint64_t mask = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        const uint16_t square0 = ColorEnergy(
            frame.board[square][static_cast<uint8_t>(
                hardware::SquareLedSlot::Square0)]);
        const uint16_t square2 = ColorEnergy(
            frame.board[square][static_cast<uint8_t>(
                hardware::SquareLedSlot::Square2)]);
        if (square0 >= threshold || square2 >= threshold) {
            mask |= UINT64_C(1) << square;
        }
    }
    return mask;
}

uint64_t HighIntensityPhysicalBoardSurfaceMask(
    const virtual_litch_physical_frame& frame,
    const uint16_t threshold = 64U) {
    uint64_t mask = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        const uint16_t square0 = ColorEnergy(frame.pixels[
            hardware::LedIndexForSquare(
                square, hardware::SquareLedSlot::Square0)]);
        const uint16_t square2 = ColorEnergy(frame.pixels[
            hardware::LedIndexForSquare(
                square, hardware::SquareLedSlot::Square2)]);
        if (square0 >= threshold || square2 >= threshold) {
            mask |= UINT64_C(1) << square;
        }
    }
    return mask;
}

uint64_t RotateSquareMask180(const uint64_t mask) {
    uint64_t rotated = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((mask & (UINT64_C(1) << square)) != 0ULL) {
            rotated |= UINT64_C(1) << (63U - square);
        }
    }
    return rotated;
}

bool StatusIsTurnIndicatorForOwner(
    const virtual_litch_semantic_frame& frame,
    const bool p2) {
    const uint8_t first = p2 ? 0U : 16U;
    bool found = false;
    for (uint8_t index = 0U; index < VIRTUAL_LITCH_STATUS_LED_COUNT;
         ++index) {
        if (ColorEnergy(frame.status[index]) == 0U) {
            continue;
        }
        found = true;
        if (index < first || index >= first + 8U) {
            return false;
        }
    }
    return found;
}

virtual_litch_color BrightestTurnIndicatorPixel(
    const virtual_litch_semantic_frame& frame,
    const bool p2) {
    const uint8_t first = p2 ? 0U : 16U;
    virtual_litch_color brightest{};
    for (uint8_t index = first; index < first + 8U; ++index) {
        if (ColorEnergy(frame.status[index]) > ColorEnergy(brightest)) {
            brightest = frame.status[index];
        }
    }
    return brightest;
}

uint32_t NormalizedColorDistanceSq(const virtual_litch_color left,
                                   const virtual_litch_color right) {
    const auto maximum = [](const virtual_litch_color color) {
        return std::max(color.r, std::max(color.g, color.b));
    };
    const uint8_t leftMax = maximum(left);
    const uint8_t rightMax = maximum(right);
    if (leftMax == 0U || rightMax == 0U) return UINT32_MAX;
    const auto normalize = [](const uint8_t channel, const uint8_t max) {
        return static_cast<int32_t>(
            (static_cast<uint32_t>(channel) * 255U + max / 2U) / max);
    };
    const int32_t red = normalize(left.r, leftMax) -
                        normalize(right.r, rightMax);
    const int32_t green = normalize(left.g, leftMax) -
                          normalize(right.g, rightMax);
    const int32_t blue = normalize(left.b, leftMax) -
                         normalize(right.b, rightMax);
    return static_cast<uint32_t>(red * red + green * green + blue * blue);
}

bool StatusIsTurnIndicatorForOwnerWithUsbHealth(
    const virtual_litch_semantic_frame& frame,
    const bool p2) {
    virtual_litch_semantic_frame withoutHealth = frame;
    withoutHealth.status[27U] = {};
    withoutHealth.status[28U] = {};
    return StatusIsTurnIndicatorForOwner(withoutHealth, p2);
}

bool IsAmberHealthMarker(const virtual_litch_color color) {
    return color.r != 0U && color.g != 0U && color.r > color.b &&
           color.g > color.b;
}

bool StatusHasP1AmberHealthBookends(
    const virtual_litch_semantic_frame& frame) {
    return IsAmberHealthMarker(frame.status[16U]) &&
           IsAmberHealthMarker(frame.status[23U]);
}

bool PhysicalHasP1AmberHealthBookends(
    const virtual_litch_physical_frame& frame) {
    const uint16_t first = hardware::BOARD_LED_COUNT + 16U;
    const uint16_t last = hardware::BOARD_LED_COUNT + 23U;
    return last < frame.led_count &&
           IsAmberHealthMarker(frame.pixels[first]) &&
           IsAmberHealthMarker(frame.pixels[last]);
}

bool StatusHasUsbSideYellowHealthMarker(
    const virtual_litch_semantic_frame& frame) {
    return IsAmberHealthMarker(frame.status[27U]) &&
           !IsAmberHealthMarker(frame.status[28U]);
}

bool PhysicalHasUsbSideYellowHealthMarker(
    const virtual_litch_physical_frame& frame) {
    const uint16_t marker = hardware::BOARD_LED_COUNT + 27U;
    const uint16_t alternate = hardware::BOARD_LED_COUNT + 28U;
    return alternate < frame.led_count &&
           IsAmberHealthMarker(frame.pixels[marker]) &&
           !IsAmberHealthMarker(frame.pixels[alternate]);
}

bool StatusIsFullRedFault(const virtual_litch_semantic_frame& frame) {
    for (const virtual_litch_color color : frame.status) {
        if (ColorEnergy(color) == 0U || color.r <= color.g ||
            color.r <= color.b) {
            return false;
        }
    }
    return true;
}

bool StatusHasSetupCompletionSweep(
    const virtual_litch_semantic_frame& frame) {
    const auto isRedDominant = [](const virtual_litch_color color) {
        return color.r != 0U && color.r > color.g && color.r > color.b;
    };
    const auto isBlueDominant = [](const virtual_litch_color color) {
        return color.b != 0U && color.b > color.r && color.b > color.g;
    };
    const bool northIsRed = isRedDominant(frame.status[3U]) &&
                            isRedDominant(frame.status[4U]);
    const bool northIsBlue = isBlueDominant(frame.status[3U]) &&
                             isBlueDominant(frame.status[4U]);
    const bool southIsRed = isRedDominant(frame.status[19U]) &&
                            isRedDominant(frame.status[20U]);
    const bool southIsBlue = isBlueDominant(frame.status[19U]) &&
                             isBlueDominant(frame.status[20U]);
    return (northIsRed && southIsBlue) ||
           (northIsBlue && southIsRed);
}

bool LitStatusPixelsAreBlueDominantOnHomeEdge(
    const virtual_litch_semantic_frame& frame,
    const bool north) {
    const uint8_t first = north ? 0U : 16U;
    bool found = false;
    for (uint8_t index = 0U; index < VIRTUAL_LITCH_STATUS_LED_COUNT;
         ++index) {
        const virtual_litch_color color = frame.status[index];
        if (ColorEnergy(color) == 0U) {
            continue;
        }
        if (index < first || index >= first + 8U ||
            color.b <= color.r || color.b <= color.g) {
            return false;
        }
        found = true;
    }
    return found;
}

bool SameDedicatedPhysicalStatusPixels(
    const virtual_litch_physical_frame& left,
    const virtual_litch_physical_frame& right) {
    if (left.led_count != right.led_count ||
        left.led_count < hardware::BOARD_LED_COUNT) {
        return false;
    }
    for (uint16_t index = hardware::BOARD_LED_COUNT;
         index < left.led_count;
         ++index) {
        if (!SameColor(left.pixels[index], right.pixels[index])) {
            return false;
        }
    }
    return true;
}

size_t LitDedicatedPhysicalStatusPixelCount(const JourneySession& session) {
    const auto& physical = session.PhysicalFrame();
    size_t count = 0U;
    for (uint16_t index = hardware::BOARD_LED_COUNT;
         index < physical.led_count;
         ++index) {
        if (ColorEnergy(physical.pixels[index]) != 0U) {
            ++count;
        }
    }
    return count;
}

bool DedicatedStatusTopologyMatches(const JourneySession& session) {
    const auto& semantic = session.SemanticFrame();
    const auto& physical = session.PhysicalFrame();
    if (physical.led_count != hardware::BOARD_LED_COUNT +
                                  VIRTUAL_LITCH_STATUS_LED_COUNT) {
        return false;
    }
    for (uint8_t index = 0U; index < VIRTUAL_LITCH_STATUS_LED_COUNT;
         ++index) {
        const bool semanticLit = ColorEnergy(semantic.status[index]) != 0U;
        const bool physicalLit =
            ColorEnergy(physical.pixels[hardware::BOARD_LED_COUNT + index]) !=
            0U;
        if (semanticLit != physicalLit) {
            return false;
        }
    }
    return true;
}

bool DedicatedStatusTopologyMatchesExceptUsbHealth(
    const JourneySession& session) {
    const auto& semantic = session.SemanticFrame();
    const auto& physical = session.PhysicalFrame();
    if (physical.led_count != hardware::BOARD_LED_COUNT +
                                  VIRTUAL_LITCH_STATUS_LED_COUNT) {
        return false;
    }
    for (uint8_t index = 0U; index < VIRTUAL_LITCH_STATUS_LED_COUNT;
         ++index) {
        if (index == 27U || index == 28U) {
            continue;
        }
        const bool semanticLit = ColorEnergy(semantic.status[index]) != 0U;
        const bool physicalLit =
            ColorEnergy(physical.pixels[hardware::BOARD_LED_COUNT + index]) !=
            0U;
        if (semanticLit != physicalLit) {
            return false;
        }
    }
    return true;
}

const virtual_litch_color& BoardLed(
    const virtual_litch_semantic_frame& frame,
    const uint8_t square,
    const hardware::SquareLedSlot slot) {
    return frame.board[square][static_cast<uint8_t>(slot)];
}

size_t BrightNeutralSquareFaceCount(
    const virtual_litch_semantic_frame& frame) {
    size_t count = 0U;
    for (uint8_t square = 0U; square < 64U; ++square) {
        for (const hardware::SquareLedSlot slot : {
                 hardware::SquareLedSlot::Square0,
                 hardware::SquareLedSlot::Square2}) {
            const virtual_litch_color color = BoardLed(frame, square, slot);
            if (color.r >= 200U && color.g >= 200U && color.b >= 200U) {
                ++count;
            }
        }
    }
    return count;
}

const virtual_litch_color& PhysicalBoardLed(
    const virtual_litch_physical_frame& frame,
    const uint8_t square,
    const hardware::SquareLedSlot slot) {
    const hardware::BoardRevision revision = frame.led_count == 300U
        ? hardware::BoardRevision::V3_3S3
        : frame.led_count == 288U ? hardware::BoardRevision::V3S3
                                 : hardware::BoardRevision::V2S3;
    const auto targets = hardware::MapSemanticSquareLedTargets(
        revision, square, slot, false);
    return frame.pixels[targets.indices[0]];
}

bool SameBoardPixels(const virtual_litch_semantic_frame& left,
                     const virtual_litch_semantic_frame& right) {
    for (uint8_t square = 0U; square < 64U; ++square) {
        for (uint8_t slot = 0U; slot < hardware::LEDS_PER_SQUARE;
             ++slot) {
            if (!SameColor(left.board[square][slot],
                           right.board[square][slot])) {
                return false;
            }
        }
    }
    return true;
}

bool SameBoardPixelsOutsideMask(
    const virtual_litch_semantic_frame& left,
    const virtual_litch_semantic_frame& right,
    const uint64_t allowedDifferenceMask) {
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((allowedDifferenceMask & (UINT64_C(1) << square)) != 0ULL) {
            continue;
        }
        for (uint8_t slot = 0U; slot < hardware::LEDS_PER_SQUARE;
             ++slot) {
            if (!SameColor(left.board[square][slot],
                           right.board[square][slot])) {
                return false;
            }
        }
    }
    return true;
}

const char* StatusShapeName(const virtual_litch_semantic_frame& frame) {
    if (LitStatusPixelCount(frame) == 0U) {
        return "off";
    }
    if (StatusIsTurnIndicatorForOwner(frame, false)) {
        return "P1-turn-indicator";
    }
    if (StatusIsTurnIndicatorForOwner(frame, true)) {
        return "P2-turn-indicator";
    }
    if (StatusIsFullRedFault(frame)) {
        return "full-red";
    }
    return "mixed";
}

virtual_litch_result DispatchJourneySnapshot(
    JourneySession& session,
    const uint64_t occupiedMask) {
    return session.DispatchBoardObservation(
        session.PhysicalOccupancyMask() ^ occupiedMask,
        occupiedMask);
}

bool TapP1X(JourneySession& session) {
    const auto tap = [&](const uint8_t event) {
        return session.DispatchButton(VIRTUAL_LITCH_OWNER_P1,
                                      VIRTUAL_LITCH_BUTTON_P1_X,
                                      event) == VIRTUAL_LITCH_OK;
    };
    return tap(VIRTUAL_LITCH_BUTTON_PRESS_EVENT) &&
           tap(VIRTUAL_LITCH_BUTTON_RELEASE_EVENT);
}

bool TapP1O(JourneySession& session) {
    const auto tap = [&](const uint8_t event) {
        return session.DispatchButton(VIRTUAL_LITCH_OWNER_P1,
                                      VIRTUAL_LITCH_BUTTON_P1_O,
                                      event) == VIRTUAL_LITCH_OK;
    };
    return tap(VIRTUAL_LITCH_BUTTON_PRESS_EVENT) &&
           tap(VIRTUAL_LITCH_BUTTON_RELEASE_EVENT);
}

bool TapP2O(JourneySession& session) {
    const auto tap = [&](const uint8_t event) {
        return session.DispatchButton(VIRTUAL_LITCH_OWNER_P2,
                                      VIRTUAL_LITCH_BUTTON_P2_O,
                                      event) == VIRTUAL_LITCH_OK;
    };
    return tap(VIRTUAL_LITCH_BUTTON_PRESS_EVENT) &&
           tap(VIRTUAL_LITCH_BUTTON_RELEASE_EVENT);
}

bool MutateJourneySetupAssistance(JourneySession& session,
                                  const uint16_t requestId,
                                  const bool enabled) {
    virtual_litch_setting_request request{};
    virtual_litch_default_setting_request(&request);
    request.request_id = requestId;
    std::snprintf(request.key, sizeof(request.key), "%s",
                  "BOARD_SETUP_ASSISTANCE");
    std::snprintf(request.value_json, sizeof(request.value_json), "%s",
                  enabled ? "true" : "false");
    modules::ControlMutationOutcome outcome;
    return session.MutateSetting(request, outcome) == VIRTUAL_LITCH_OK &&
           outcome.status == modules::ControlMutationStatus::Applied &&
           outcome.reason == modules::ControlMutationReason::None;
}

bool MutateJourneyU32(JourneySession& session,
                      const uint16_t requestId,
                      const char* key,
                      const uint32_t value) {
    virtual_litch_setting_request request{};
    virtual_litch_default_setting_request(&request);
    request.request_id = requestId;
    std::snprintf(request.key, sizeof(request.key), "%s", key);
    std::snprintf(request.value_json, sizeof(request.value_json), "%u",
                  static_cast<unsigned>(value));
    modules::ControlMutationOutcome outcome;
    return session.MutateSetting(request, outcome) == VIRTUAL_LITCH_OK &&
           outcome.status == modules::ControlMutationStatus::Applied &&
           outcome.reason == modules::ControlMutationReason::None;
}

bool MutateJourneyBool(JourneySession& session,
                       const uint16_t requestId,
                       const char* key,
                       const bool value) {
    virtual_litch_setting_request request{};
    virtual_litch_default_setting_request(&request);
    request.request_id = requestId;
    std::snprintf(request.key, sizeof(request.key), "%s", key);
    std::snprintf(request.value_json, sizeof(request.value_json), "%s",
                  value ? "true" : "false");
    modules::ControlMutationOutcome outcome;
    return session.MutateSetting(request, outcome) == VIRTUAL_LITCH_OK &&
           outcome.status == modules::ControlMutationStatus::Applied &&
           outcome.reason == modules::ControlMutationReason::None;
}

void PrintRawFrame(const runtime::VirtualLitchSession& session,
                   const char* step, const char* next);

void TestFreshSetupKingLiftHomeJourney() {
    constexpr uint64_t INITIAL = UINT64_C(0xFFFF00000000FFFF);
    constexpr uint8_t KING = 60U;
    test_support::BoundedHintSession fixture(ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& session = fixture.session;
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(INITIAL) == VIRTUAL_LITCH_OK &&
              session.Advance(2000U) == VIRTUAL_LITCH_OK,
          "king-lift Home journey reaches opening with flashing king");
    Check(!session.HomeStateSnapshot().kingQueenSwapPromptActive,
          "opening king flash does not own a K/Q prompt");
    PrintRawFrame(session, "king-lift opening; input=initial pieces; wait 2s",
                  "lift flashing P1 king");
    Check(session.ObserveHallMask(INITIAL & ~(UINT64_C(1) << KING)) == VIRTUAL_LITCH_OK &&
              session.Advance(1U) == VIRTUAL_LITCH_OK,
          "king-lift Home journey accepts physical king lift");
    const auto lifted = session.HomeStateSnapshot();
    Check(lifted.freshSetup && lifted.action == modules::HomeStateAction::Lift &&
              session.State().selected_square == KING &&
              session.State().valid_destination_mask == 0ULL &&
              session.State().human_illegal_mask == 0ULL &&
              !lifted.kingQueenSwapPromptActive && !session.KingQueenSwapActive() &&
              lifted.occupiedMask == (INITIAL & ~(UINT64_C(1) << KING)),
          "blocked own-king pickup keeps ordinary source guidance, never correction or K/Q authority");
    Check(session.HomeStateLine().View().find("\"kq\":1") == std::string_view::npos,
          "production Home publication omits the inactive K/Q flag");
    PrintRawFrame(session, "king lifted; input=Hall e1 empty; Home kq=0",
                  "replace P1 king on e1");
    Check(session.ObserveHallMask(INITIAL) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              !session.HomeStateSnapshot().kingQueenSwapPromptActive,
          "replacing king recovers without fabricated K/Q guidance");
    Check(session.ObserveHallMask(INITIAL | (UINT64_C(1) << 36U)) == VIRTUAL_LITCH_OK &&
              !session.HomeStateSnapshot().kingQueenSwapPromptActive &&
              session.ObserveHallMask(INITIAL) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK,
          "ordinary invalid extra and its recovery never create K/Q authority");
    PrintRawFrame(session, "king-lift recovery; input=replace e1; clear extra; Home kq=0",
                  "play normally");
}

void TestPlayerEndSetupJourneyPlayback() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr std::array<uint8_t, 4> KQ_SQUARES{3U, 4U, 59U, 60U};
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E3 = 44U;
    constexpr uint8_t E4 = 36U;
    constexpr uint8_t E7 = 12U;
    constexpr uint64_t E2_LEGAL_DESTINATIONS =
        (1ULL << E3) | (1ULL << E4);
    test_support::BoundedHintEngine journeyEngine;
    JourneySession session(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3),
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
        nullptr, &journeyEngine);
    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "player-end journey initializes");
    Check(DispatchJourneySnapshot(
              session, INITIAL_OCCUPANCY & ~(1ULL << E2)) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK,
          "player-end journey enters ordinary Fresh Setup correction");
    Check(StatusIsTurnIndicatorForOwner(session.SemanticFrame(), false),
          "player-end journey initially points the Turn indicator south");

    Check(MutateJourneyU32(session, 740U, "GAME_FIRST_PLAYER", 2U),
          "player-end journey accepts placement change during Fresh Setup");
    Check(StatusIsTurnIndicatorForOwner(session.SemanticFrame(), false),
          "player-end journey keeps semantic P1 Turn canonical");

    Check(session.DispatchSensorDelta(E2, true) == VIRTUAL_LITCH_OK &&
              session.Advance(199U) == VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive() &&
              session.KingQueenSwapActive(),
          "player-end journey keeps the K/Q correction active during the 199 ms opening handoff");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.KingQueenSwapActive() &&
              session.State().physical_occupied_mask == INITIAL_OCCUPANCY &&
              session.State().expected_occupied_mask == INITIAL_OCCUPANCY &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().correction_mask == 0ULL,
          "player-end journey starts K/Q correction once the opening is stable");
    Check(session.HomeStateSnapshot().kingQueenSwapPromptActive,
          "Home announces genuine K/Q prompt before any physical lift");
    Check(session.DispatchSensorDelta(KQ_SQUARES.front(), false) ==
                  VIRTUAL_LITCH_OK &&
              session.KingQueenSwapActive(),
          "player-end journey records the first K/Q lift");
    virtual_litch_setting_request playerEndDuringPrompt{};
    virtual_litch_default_setting_request(&playerEndDuringPrompt);
    playerEndDuringPrompt.request_id = 741U;
    std::snprintf(playerEndDuringPrompt.key,
                  sizeof(playerEndDuringPrompt.key), "%s",
                  "GAME_FIRST_PLAYER");
    std::snprintf(playerEndDuringPrompt.value_json,
                  sizeof(playerEndDuringPrompt.value_json), "%s", "1");
    modules::ControlMutationOutcome playerEndDuringPromptOutcome;
    Check(session.MutateSetting(playerEndDuringPrompt,
                                playerEndDuringPromptOutcome) ==
                  VIRTUAL_LITCH_OK &&
              playerEndDuringPromptOutcome.status ==
                  modules::ControlMutationStatus::Rejected &&
              playerEndDuringPromptOutcome.reason ==
                  modules::ControlMutationReason::Busy &&
              session.FirstPlayerIsP2() && session.KingQueenSwapActive(),
          "app player-end reversal stays busy after the first K/Q lift");
    std::printf(
        "[PLAYER_END_JOURNEY] Post-lift app reversal rejected | "
        "input=lift-d8;GAME_FIRST_PLAYER=1 | outcome=Rejected/Busy "
        "| player_end=2 correction=active | next=finish-current-swap\n");
    Check(session.DispatchSensorDelta(KQ_SQUARES.front(), true) ==
              VIRTUAL_LITCH_OK,
          "player-end journey replaces the first K/Q piece");
    for (std::size_t index = 1U; index < KQ_SQUARES.size(); ++index) {
        const uint8_t square = KQ_SQUARES[index];
        Check(session.DispatchSensorDelta(square, false) ==
                      VIRTUAL_LITCH_OK &&
                  session.DispatchSensorDelta(square, true) ==
                      VIRTUAL_LITCH_OK,
              "player-end journey advances the K/Q correction");
    }
    Check(session.HomeStateSnapshot().kingQueenSwapPromptActive &&
              session.Advance(199U) == VIRTUAL_LITCH_OK &&
              session.HomeStateSnapshot().kingQueenSwapPromptActive,
          "Home retains genuine K/Q authority through completion dwell");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
              !session.KingQueenSwapActive() &&
              !session.BoardOverlayActive() &&
              StatusIsTurnIndicatorForOwner(session.SemanticFrame(), false),
          "player-end journey completes K/Q correction and restores the P1 Turn indicator north");
    Check(!session.HomeStateSnapshot().kingQueenSwapPromptActive,
          "Home clears K/Q authority after genuine completion");
    std::printf(
        "[PLAYER_END_JOURNEY] Orientation corrected | input=place-final-e2; "
        "swap four prompted K/Q pieces "
        "| player_end=2 rotate=0 | authoritative=Idle expected=initial "
        "physical=initial | board_overlay=off | perimeter=P1-turn-indicator "
        "| next=play\n");
    Check(session.Advance(601U) == VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive() &&
              StatusIsTurnIndicatorForOwner(session.SemanticFrame(), false),
          "player-end journey keeps the P1 Turn indicator after K/Q correction");
    Check(NormalizedColorDistanceSq(
              BrightestTurnIndicatorPixel(session.SemanticFrame(), false),
              BoardLed(session.SemanticFrame(), E2,
                       hardware::SquareLedSlot::Piece)) <= 9U,
          "active-side animation uses P1's direct player color without a hue offset");
    PrintJourneyCheckpoint(
        session, "direct-color active-side animation",
        "finish player-end correction; sample the moving Turn indicator",
        "lift the P1 pawn");

    Check(session.DispatchSensorDelta(E2, false) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::PieceLifted) &&
              session.State().valid_destination_mask ==
                  E2_LEGAL_DESTINATIONS &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL &&
              ColorEnergy(BoardLed(
                  session.SemanticFrame(), E2,
                  hardware::SquareLedSlot::Square0)) != 0U,
          "P1/White pickup after player-end setup immediately selects e2 and its legal mask");
    // The mask is authoritative on pickup. The visible wave reaches e4 at 100ms
    // and spends another 50ms fading it in.
    Check(session.Advance(150U) == VIRTUAL_LITCH_OK &&
              ColorEnergy(BoardLed(
                  session.SemanticFrame(), E3,
                  hardware::SquareLedSlot::Square0)) != 0U &&
              ColorEnergy(BoardLed(
                  session.SemanticFrame(), E3,
                  hardware::SquareLedSlot::Square2)) != 0U &&
              ColorEnergy(BoardLed(
                  session.SemanticFrame(), E4,
                  hardware::SquareLedSlot::Square0)) != 0U &&
              ColorEnergy(BoardLed(
                  session.SemanticFrame(), E4,
                  hardware::SquareLedSlot::Square2)) != 0U,
          "P1/White pickup after player-end setup visibly guides e3/e4 after the outward reveal");
    std::printf(
        "[PLAYER_END_JOURNEY] White pickup | input=lift-e2 | "
        "authoritative=PieceLifted valid=%016llx invalid=0 | "
        "board=origin-plus-e3/e4 | next=return-e2\n",
        static_cast<unsigned long long>(
            session.State().valid_destination_mask));

    Check(session.DispatchSensorDelta(E2, true) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle),
          "returning the P1/White pawn restores ordinary Idle play");
    const bool p2PickupDispatched =
        session.DispatchSensorDelta(E7, false) == VIRTUAL_LITCH_OK &&
        session.Advance(modules::CORRECTION_STRONG_MS) == VIRTUAL_LITCH_OK;
    const virtual_litch_color invalidSquare0 = BoardLed(
        session.SemanticFrame(), E7, hardware::SquareLedSlot::Square0);
    const virtual_litch_color invalidPiece = BoardLed(
        session.SemanticFrame(), E7, hardware::SquareLedSlot::Piece);
    const virtual_litch_color invalidSquare2 = BoardLed(
        session.SemanticFrame(), E7, hardware::SquareLedSlot::Square2);
    Check(p2PickupDispatched &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Correction) &&
              session.State().human_illegal_mask == (1ULL << E7) &&
              session.State().correction_mask == (1ULL << E7) &&
              StatusIsFullRedFault(session.SemanticFrame()) &&
              ColorEnergy(invalidSquare0) != 0U &&
              ColorEnergy(invalidPiece) == 0U &&
              ColorEnergy(invalidSquare2) != 0U &&
              !SameColor(invalidSquare0, invalidSquare2),
          "P2/Black pickup keeps the empty center dark with rainbow side-pair and red perimeter feedback");
    std::printf(
        "[PLAYER_END_JOURNEY] Black invalid-owner pickup | input=lift-e7 | "
        "authoritative=Correction invalid=%016llx | "
        "board=mismatch-rainbow-pair empty-center=dark status=%s | "
        "next=return-e7\n",
        static_cast<unsigned long long>(
            session.State().human_illegal_mask),
        StatusShapeName(session.SemanticFrame()));

    Check(session.DispatchSensorDelta(E7, true) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL,
          "returning the P2/Black pawn recovers ordinary playable Idle state");
}

void TestBoardTimerQuickSettingsJourney() {
    test_support::BoundedHintEngine journeyEngine;
    JourneySession session(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3),
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
        nullptr, &journeyEngine);
    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "board TIMER journey initializes");

    Check(session.DispatchButton(VIRTUAL_LITCH_OWNER_P1,
                                 VIRTUAL_LITCH_BUTTON_P1_X,
                                 VIRTUAL_LITCH_BUTTON_PRESS_EVENT) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(1000U) == VIRTUAL_LITCH_OK &&
              session.BoardOverlayActive(),
          "holding X opens the production Quick Settings surface");
    if (!session.BoardOverlayActive()) {
        return;
    }
    // Supporting normalized-event journey: model the complete gesture, not
    // repeated press edges without releases. Raw journeys cover GPIO debounce.
    const auto menuTap = [&](const uint8_t button) {
        if (button == VIRTUAL_LITCH_BUTTON_LEFT || button == VIRTUAL_LITCH_BUTTON_RIGHT) {
            Check(session.Advance(250U) == VIRTUAL_LITCH_OK,
                  "wait out the vertical guard before normalized side Press");
        }
        return session.DispatchButton(
                   VIRTUAL_LITCH_OWNER_P1, button,
                   VIRTUAL_LITCH_BUTTON_PRESS_EVENT) == VIRTUAL_LITCH_OK &&
               session.DispatchButton(
                   VIRTUAL_LITCH_OWNER_P1, button,
                   VIRTUAL_LITCH_BUTTON_RELEASE_EVENT) == VIRTUAL_LITCH_OK;
    };

    Check(session.DispatchButton(VIRTUAL_LITCH_OWNER_P1,
                                 VIRTUAL_LITCH_BUTTON_P1_X,
                                 VIRTUAL_LITCH_BUTTON_RELEASE_EVENT) ==
              VIRTUAL_LITCH_OK,
          "releasing the opening hold is consumed by Quick Settings");

    for (uint8_t item = 0U; item < 6U; ++item) {
        Check(menuTap(VIRTUAL_LITCH_BUTTON_RIGHT),
              "right navigates to the board TIMER item");
    }
    const uint64_t offBoardHash = session.SemanticFrame().board_hash;
    const uint64_t timerSelectionStatusHash =
        session.SemanticFrame().status_hash;
    Check(session.DisplayText().empty(),
          "TIMER selection starts with its non-letter clock icon");

    Check(menuTap(VIRTUAL_LITCH_BUTTON_UP),
          "up selects the next TIMER preset");
    modules::ControlMutationOutcome outcome;
    Check(session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.reason == modules::ControlMutationReason::None &&
              session.BoardOverlayActive() &&
              session.SemanticFrame().board_hash != offBoardHash &&
              session.SemanticFrame().status_hash ==
                  timerSelectionStatusHash,
          "TIMER adjustment is acknowledged while the selected-item perimeter remains stable");

    Check(session.DispatchButton(VIRTUAL_LITCH_OWNER_P1,
                                 VIRTUAL_LITCH_BUTTON_P1_X,
                                 VIRTUAL_LITCH_BUTTON_PRESS_EVENT) ==
                  VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive(),
          "X closes Quick Settings after TIMER selection");

    virtual_litch_action_request newGame{};
    virtual_litch_default_action_request(&newGame);
    newGame.request_id = 750U;
    newGame.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    newGame.owner = VIRTUAL_LITCH_OWNER_P1;
    Check(session.DispatchControlAction(newGame, outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "New Game applies the selected board TIMER preset");
    const runtime::GameClock* clock = session.GameClock();
    Check(clock != nullptr && clock->IsEnabled() &&
              clock->GetRemainingMs(runtime::ClockSide::White) == 60000U &&
              clock->GetRemainingMs(runtime::ClockSide::Black) == 60000U,
          "board-selected TIMER=1 starts both casual clocks at one minute");

    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;
    Check(DispatchJourneySnapshot(session, INITIAL_OCCUPANCY) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              clock != nullptr && !clock->IsRunning() && !clock->IsPaused(),
          "board-selected clock waits while the opening setup stabilizes");
    Check(session.DispatchSensorDelta(E2, false) == VIRTUAL_LITCH_OK &&
              session.DispatchSensorDelta(E4, true) == VIRTUAL_LITCH_OK &&
              session.Advance(2000U) == VIRTUAL_LITCH_OK &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              clock != nullptr && clock->IsRunning() && !clock->IsPaused() &&
              clock->GetActiveSide() == runtime::ClockSide::Black,
          "first legal pickup starts time and e2-e4 switches to P2");
    Check(session.DispatchButton(VIRTUAL_LITCH_OWNER_P1,
                                 VIRTUAL_LITCH_BUTTON_P1_X,
                                 VIRTUAL_LITCH_BUTTON_PRESS_EVENT) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(1000U) == VIRTUAL_LITCH_OK &&
              session.BoardOverlayActive(),
          "running play can reopen Quick Settings");
    Check(session.Advance(1021U) == VIRTUAL_LITCH_OK &&
              session.DisplayText() == "1",
          "reopened TIMER shows its numeric value after the identity icon");
    const uint32_t pausedRemaining =
        clock == nullptr
            ? 0U
            : clock->GetRemainingMs(runtime::ClockSide::Black);

    const uint64_t timerValueBoardHash = session.SemanticFrame().board_hash;
    const uint64_t timerValueStatusHash = session.SemanticFrame().status_hash;
    Check(menuTap(VIRTUAL_LITCH_BUTTON_UP) &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason == modules::ControlMutationReason::NotAtStart,
          "midgame TIMER adjustment is rejected as not-at-start");
    Check(session.EdgeCueIntent().kind ==
                  modules::EdgeLedCueKind::OccupiedBoardBlocked &&
              session.EdgeCueIntent().owner ==
                  static_cast<modules::UiOwner>(
                      session.QuickSettingsOwner()) &&
              session.DisplayText() == "1" &&
              session.SemanticFrame().status_hash != timerValueStatusHash,
          "midgame TIMER rejection uses the occupied-pieces amber warning");
    Check(session.SemanticFrame().board_hash != timerValueBoardHash,
          "midgame TIMER rejection changes the visible board frame");
    Check(clock != nullptr && clock->IsPaused() &&
              clock->GetRemainingMs(runtime::ClockSide::Black) ==
                  pausedRemaining,
          "midgame TIMER rejection leaves the paused clock unchanged");
    Check(session.Advance(1000U) == VIRTUAL_LITCH_OK &&
              session.EdgeCueIntent().kind == modules::EdgeLedCueKind::None &&
              clock != nullptr && clock->IsPaused() &&
              clock->GetRemainingMs(runtime::ClockSide::Black) ==
                  pausedRemaining,
          "board Quick Settings pauses the casual clock while it is open");
    Check(menuTap(VIRTUAL_LITCH_BUTTON_LEFT),
          "left navigates from TIMER to board rotation");
    const bool rotateBeforeBlock = session.BoardRotate();
    Check(menuTap(VIRTUAL_LITCH_BUTTON_UP) &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason == modules::ControlMutationReason::NotAtStart &&
              session.BoardRotate() == rotateBeforeBlock &&
              session.EdgeCueIntent().kind ==
                  modules::EdgeLedCueKind::OccupiedBoardBlocked,
          "midgame board rotation remains unchanged and repeats the occupied-piece amber warning");
    Check(session.Advance(1000U) == VIRTUAL_LITCH_OK,
          "rotation warning expires before the next blocked action");
    for (uint8_t item = 0U; item < 3U; ++item) {
        Check(menuTap(VIRTUAL_LITCH_BUTTON_LEFT),
              "left navigates from rotation to P1/P2 end placement");
    }
    const bool firstPlayerBeforeBlock = session.FirstPlayerIsP2();
    Check(menuTap(VIRTUAL_LITCH_BUTTON_UP) &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason == modules::ControlMutationReason::NotAtStart &&
              session.FirstPlayerIsP2() == firstPlayerBeforeBlock &&
              session.EdgeCueIntent().kind ==
                  modules::EdgeLedCueKind::OccupiedBoardBlocked,
          "midgame P1/P2 end placement remains unchanged and repeats the occupied-piece amber warning");
    Check(session.Advance(1000U) == VIRTUAL_LITCH_OK &&
              !session.EdgeCueIntent().IsActive(),
          "P1/P2-end warning restores the Quick Settings surface after one second");
    Check(session.DispatchButton(VIRTUAL_LITCH_OWNER_P1,
                                 VIRTUAL_LITCH_BUTTON_P1_X,
                                 VIRTUAL_LITCH_BUTTON_PRESS_EVENT) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              clock != nullptr && !clock->IsPaused() &&
              clock->GetRemainingMs(runtime::ClockSide::Black) ==
                  pausedRemaining &&
              session.Advance(100U) == VIRTUAL_LITCH_OK &&
              clock->GetRemainingMs(runtime::ClockSide::Black) <
                  pausedRemaining,
          "closing Quick Settings resumes the same casual clock");

    std::printf(
        "[OCCUPIED_BOARD_ACTION_BLOCK_JOURNEY] input=midgame-TIMER/rotate/"
        "P1-P2-ends authoritative=rejected:not_at_start settings=unchanged "
        "pieces=pulse status=amber duration_ms=1000 recovery=menu-restored "
        "next=new-game-for-success\n");
    std::printf(
        "[BOARD_TIMER_JOURNEY] X-hold; right x6; up; X | "
        "display=0->1 status=%016llx outcome=applied/none | "
        "clock=60000/60000 then midgame=amber-perimeter+"
        "all-piece-warning; value-unchanged; cue-expired; clock-paused/resumed | "
        "next=play\n",
        static_cast<unsigned long long>(timerSelectionStatusHash));
}

void TestAppOccupiedSettingFeedbackJourney() {
    test_support::BoundedHintEngine engine;
    JourneySession session(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_V3),
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
        nullptr, &engine);
    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "app occupied settings journey initializes");
    constexpr uint64_t OPENING = 0xFFFF00000000FFFFULL;
    DispatchJourneySnapshot(session, OPENING);
    session.Advance(2500U);
    session.DispatchSensorDelta(52U, false);
    session.DispatchSensorDelta(36U, true);
    session.Advance(2500U);
    const auto before = session.State();
    uint16_t requestId = 790U;
    for (const auto& mutation : std::array<std::pair<const char*, const char*>, 3>{
             {{"BOARD_ROTATE", "true"}, {"GAME_FIRST_PLAYER", "2"},
              {"TIMER", "1"}}}) {
        modules::ControlMutationOutcome outcome;
        virtual_litch_setting_request request{};
        virtual_litch_default_setting_request(&request);
        request.request_id = requestId++;
        std::snprintf(request.key, sizeof(request.key), "%s", mutation.first);
        std::snprintf(request.value_json, sizeof(request.value_json), "%s",
                      mutation.second);
        Check(session.MutateSetting(request, outcome) ==
                      VIRTUAL_LITCH_OK &&
                  outcome.status == modules::ControlMutationStatus::Rejected &&
                  outcome.reason == modules::ControlMutationReason::NotAtStart,
              "app start-only setting rejects through the production mutation service");
        Check(session.EdgeCueIntent().kind ==
                  modules::EdgeLedCueKind::OccupiedBoardBlocked,
              "app start-only rejection shows the same occupied-board warning");
        const auto warningStarted = session.EdgeCueIntent().startMs;
        for (uint8_t read = 0U; read < 5U; ++read) {
            (void)session.HomeStateLine();
            session.Advance(100U);
            Check(session.EdgeCueIntent().startMs == warningStarted,
                  "HomeState clock-gate reads cannot restart occupied feedback");
        }
        Check(session.State().physical_occupied_mask == before.physical_occupied_mask &&
                  session.State().side_to_move_black == before.side_to_move_black &&
                  !session.BoardRotate() && !session.FirstPlayerIsP2(),
              "app occupied setting rejection preserves game and orientation");
        session.Advance(1001U);
        Check(!session.EdgeCueIntent().IsActive(),
              "app occupied setting warning expires after one second");
        for (uint8_t read = 0U; read < 5U; ++read) {
            (void)session.HomeStateLine();
            session.Advance(100U);
            Check(!session.EdgeCueIntent().IsActive(),
                  "HomeState polling cannot resurrect an expired warning");
        }
    }
    std::puts("[APP_OCCUPIED_SETTINGS_JOURNEY] input=e2-e4; "
              "app=rotate/P1-P2/TIMER authoritative=midgame "
              "result=rejected:not_at_start game=unchanged pieces=pulse "
              "status=amber expiry=1000ms next=new-game");
}

void TestClockTimeoutPhysicalHintRecoveryJourney() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    test_support::BoundedHintEngine journeyEngine;
    JourneySession session(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3),
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
        nullptr, &journeyEngine);
    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "timeout Hint journey initializes");
    Check(MutateJourneyU32(session, 760U, "TIMER", 1U),
          "timeout Hint journey selects the one-minute clock");

    virtual_litch_action_request newGame{};
    virtual_litch_default_action_request(&newGame);
    newGame.request_id = 761U;
    newGame.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    newGame.owner = VIRTUAL_LITCH_OWNER_P1;
    modules::ControlMutationOutcome outcome;
    Check(session.DispatchControlAction(newGame, outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "timeout Hint journey starts a timed game");
    Check(DispatchJourneySnapshot(session, INITIAL_OCCUPANCY) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK,
          "timeout Hint journey accepts canonical initial occupancy");

    const runtime::GameClock* clock = session.GameClock();
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;
    Check(clock != nullptr && !clock->IsRunning() && !clock->IsPaused() &&
              session.DispatchSensorDelta(E2, false) ==
                  VIRTUAL_LITCH_OK &&
              session.DispatchSensorDelta(E4, true) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(2001U) == VIRTUAL_LITCH_OK &&
              clock->IsRunning() &&
              clock->GetActiveSide() == runtime::ClockSide::Black,
          "timeout Hint journey starts from the first legal pickup");
    Check(session.Advance(60001U) == VIRTUAL_LITCH_OK &&
              clock != nullptr && clock->HasTimedOut() &&
              clock->GetTimedOutSide() == runtime::ClockSide::Black &&
              !clock->IsRunning(),
          "timeout Hint journey latches Black's flag fall");
    const virtual_litch_semantic_frame terminalFrame =
        session.SemanticFrame();

    Check(TapP2O(session) &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::Hint &&
              outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason == modules::ControlMutationReason::GameOver,
          "physical active-side O reaches the board-origin Hint rejection after timeout");
    Check(!session.BoardOverlayActive() &&
              SameBoardPixels(session.SemanticFrame(), terminalFrame) &&
              session.EdgeCueIntent().kind ==
                  modules::EdgeLedCueKind::Unavailable,
          "terminal physical Hint keeps the board text-free and selects the calm unavailable cue");
    Check(session.Advance(
              modules::EdgeLedCueController::UNAVAILABLE_DURATION_MS + 1U) ==
                  VIRTUAL_LITCH_OK &&
              !session.EdgeCueIntent().IsActive() &&
              !session.BoardOverlayActive(),
          "terminal physical Hint cue expires without leaving an overlay");

    newGame.request_id = 762U;
    Check(session.DispatchControlAction(newGame, outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied &&
              clock != nullptr && !clock->HasTimedOut(),
          "New Game clears the timeout authority");
    Check(DispatchJourneySnapshot(session, INITIAL_OCCUPANCY) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              TapP1O(session) &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::Hint &&
              outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.reason == modules::ControlMutationReason::None,
          "physical Hint succeeds again after New Game recovery");

    std::printf(
        "[TIMEOUT_HINT_JOURNEY] input=TIMER=1,new-game,e2-e4-settled,+60001ms,P1-O | "
        "authority=black-timeout outcome=hint-rejected/game-over | "
        "board=text-free cue=unavailable expiry=481ms | "
        "recovery=new-game,hint-applied next=play\n");
}

void PrintJourneyCheckpoint(const JourneySession& session,
                            const char* step,
                            const char* input,
                            const char* next) {
    const auto& state = session.State();
    const auto& semantic = session.SemanticFrame();
    static constexpr uint8_t BLACK_KING = 4U;
    static constexpr uint8_t WHITE_KING = 60U;
    modules::ControlMutationOutcome outcome;
    const bool hasOutcome = session.TryGetLastBoardControlOutcome(outcome);
    std::printf(
        "[SETUP_JOURNEY] %s | t=%u input=\"%s\" | phase=%s "
        "mismatch=%016llx | status=%s pixels=%zu/%zu | "
        "king-pairs=W:%u/%u/%u B:%u/%u/%u | "
        "hashes=%016llx/%016llx | last_outcome=%s/%s | next=\"%s\"\n",
        step,
        static_cast<unsigned>(state.now_ms),
        input,
        runtime::ChessInteractionPhaseToString(
            static_cast<runtime::ChessInteractionPhase>(
                state.interaction_phase)),
        static_cast<unsigned long long>(state.physical_occupied_mask ^
                                        state.expected_occupied_mask),
        StatusShapeName(semantic),
        LitStatusPixelCount(semantic),
        LitDedicatedPhysicalStatusPixelCount(session),
        static_cast<unsigned>(ColorEnergy(BoardLed(
            semantic, WHITE_KING, hardware::SquareLedSlot::Square0))),
        static_cast<unsigned>(ColorEnergy(BoardLed(
            semantic, WHITE_KING, hardware::SquareLedSlot::Piece))),
        static_cast<unsigned>(ColorEnergy(BoardLed(
            semantic, WHITE_KING, hardware::SquareLedSlot::Square2))),
        static_cast<unsigned>(ColorEnergy(BoardLed(
            semantic, BLACK_KING, hardware::SquareLedSlot::Square0))),
        static_cast<unsigned>(ColorEnergy(BoardLed(
            semantic, BLACK_KING, hardware::SquareLedSlot::Piece))),
        static_cast<unsigned>(ColorEnergy(BoardLed(
            semantic, BLACK_KING, hardware::SquareLedSlot::Square2))),
        static_cast<unsigned long long>(semantic.board_hash),
        static_cast<unsigned long long>(semantic.status_hash),
        hasOutcome ? modules::ControlMutationStatusToString(outcome.status)
                   : "none",
        hasOutcome ? modules::ControlMutationReasonToString(outcome.reason)
                   : "none",
        next);
}

const char* RuntimeHealthStateName(const uint8_t state) {
    switch (state) {
        case VIRTUAL_LITCH_RUNTIME_HEALTH_HEALTHY:
            return "Healthy";
        case VIRTUAL_LITCH_RUNTIME_HEALTH_DEGRADED_USABLE:
            return "DegradedUsable";
        case VIRTUAL_LITCH_RUNTIME_HEALTH_INPUT_PAUSED:
            return "InputPaused";
        case VIRTUAL_LITCH_RUNTIME_HEALTH_PROTECTION_LATCHED:
            return "ProtectionLatched";
        default:
            return "Unknown";
    }
}

void PrintRuntimeHealthCheckpoint(const JourneySession& session,
                                  const char* step,
                                  const char* input,
                                  const char* outcome,
                                  const char* next) {
    const auto& health = session.RuntimeHealth();
    const auto& state = session.State();
    const auto& semantic = session.SemanticFrame();
    const auto& physical = session.PhysicalFrame();
    const runtime::GameClock* clock = session.GameClock();
    const bool clockPresent = clock != nullptr;
    const bool blackActive = clockPresent &&
                             clock->GetActiveSide() == runtime::ClockSide::Black;
    const uint32_t remainingMs = clockPresent
                                     ? clock->GetRemainingMs(clock->GetActiveSide())
                                     : 0U;
    std::printf(
        "[RUNTIME_HEALTH_JOURNEY] %s | t=%u input=\"%s\" | "
        "health=%s rev=%u causes=0x%04x advice=%u/%u/%u | "
        "phase=%s obs=%u occupancy=%016llx | "
        "clock=%s/%s side=%s remaining=%u | "
        "display=none status_mask=%08x/%08x pixels=%zu/%zu "
        "hashes=%016llx/%016llx/%016llx | "
        "outcome=\"%s\" next=\"%s\"\n",
        step,
        static_cast<unsigned>(health.now_ms),
        input,
        RuntimeHealthStateName(health.state),
        static_cast<unsigned>(health.revision),
        static_cast<unsigned>(health.causes),
        static_cast<unsigned>(health.board_input_allowed),
        static_cast<unsigned>(health.move_acceptance_allowed),
        static_cast<unsigned>(health.clocks_allowed),
        runtime::ChessInteractionPhaseToString(
            static_cast<runtime::ChessInteractionPhase>(
                state.interaction_phase)),
        static_cast<unsigned>(state.board_observation_sequence),
        static_cast<unsigned long long>(state.physical_occupied_mask),
        clockPresent && clock->IsRunning() ? "running" : "stopped",
        clockPresent && clock->IsPaused() ? "paused" : "advancing",
        blackActive ? "black" : "white",
        static_cast<unsigned>(remainingMs),
        static_cast<unsigned>(LitStatusPixelMask(semantic)),
        static_cast<unsigned>(LitDedicatedPhysicalStatusPixelMask(session)),
        LitStatusPixelCount(semantic),
        LitDedicatedPhysicalStatusPixelCount(session),
        static_cast<unsigned long long>(semantic.board_hash),
        static_cast<unsigned long long>(semantic.status_hash),
        static_cast<unsigned long long>(physical.frame_hash),
        outcome,
        next);
}

uint16_t PhysicalSquareLedIndex(const virtual_litch_config& config,
                                const uint8_t square,
                                const hardware::SquareLedSlot slot) {
    const uint16_t semanticIndex = hardware::LedIndexForSquare(square, slot);
    return hardware::MapLedIndexForSquareAndEdgeRotate(
        semanticIndex,
        config.square_rotate_for_output != 0U,
        config.board_rotate != 0U);
}

struct LargerSettingRequest {
    virtual_litch_setting_request base;
    uint32_t future_flags = 0U;
};

struct LargerActionRequest {
    virtual_litch_action_request base;
    uint32_t future_flags = 0U;
};

struct LargerRuntimeHealthFact {
    virtual_litch_runtime_health_fact base;
    uint32_t future_flags = 0U;
};

struct LargerRuntimeHealth {
    virtual_litch_runtime_health base;
    uint32_t future_flags = 0U;
};

void TestAbiValidation() {
    Check(virtual_litch_get_abi_version() ==
              VIRTUAL_LITCH_ABI_VERSION,
          "ABI version is externally queryable");
    Check(virtual_litch_create(nullptr, nullptr) ==
              VIRTUAL_LITCH_NULL_ARGUMENT,
          "create rejects a null output pointer");

    virtual_litch_config config = ConfigFor(
        VIRTUAL_LITCH_HARDWARE_S3);
    config.abi_version += 1U;
    virtual_litch* board = nullptr;
    Check(virtual_litch_create(&config, &board) ==
              VIRTUAL_LITCH_ABI_MISMATCH,
          "create rejects an incompatible ABI version");
    Check(board == nullptr, "failed create leaves the handle null");

    virtual_litch_position defaultPosition{};
    virtual_litch_default_position(&defaultPosition);
    Check(defaultPosition.en_passant_target_square == UINT8_MAX &&
              defaultPosition.en_passant_capture_square == UINT8_MAX,
          "default position marks optional squares invalid");

    config = ConfigFor(VIRTUAL_LITCH_HARDWARE_S3);
    config.brightness_scale_256 = 257U;
    Check(virtual_litch_create(&config, &board) ==
              VIRTUAL_LITCH_INVALID_CONFIG,
          "create rejects invalid deterministic policy values");

    virtual_litch_sleep_lifecycle lifecycle{};
    virtual_litch_default_sleep_lifecycle(&lifecycle);
    Check(lifecycle.struct_size == sizeof(lifecycle) &&
              lifecycle.abi_version ==
                  VIRTUAL_LITCH_SLEEP_LIFECYCLE_ABI_VERSION &&
              lifecycle.state == VIRTUAL_LITCH_SLEEP_AWAKE &&
              lifecycle.origin == VIRTUAL_LITCH_SLEEP_ORIGIN_NONE,
          "sleep lifecycle default initializes its independent ABI prefix");
    board = CreateBoard(ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    virtual_litch_sleep_lifecycle badLifecycle{};
    Check(virtual_litch_get_sleep_lifecycle(board, &badLifecycle) ==
              VIRTUAL_LITCH_ABI_MISMATCH,
          "sleep lifecycle query validates the caller ABI prefix");
    virtual_litch_default_sleep_lifecycle(&badLifecycle);
    badLifecycle.struct_size -= 1U;
    Check(virtual_litch_get_sleep_lifecycle(board, &badLifecycle) ==
              VIRTUAL_LITCH_ABI_MISMATCH,
          "sleep lifecycle query rejects undersized caller storage");
    virtual_litch_destroy(board);
}

void TestRuntimeHealthAbiAndValidation() {
    virtual_litch_runtime_health_fact fact{};
    virtual_litch_default_runtime_health_fact(&fact);
    Check(fact.struct_size == sizeof(fact) &&
              fact.abi_version ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_ABI_VERSION &&
              fact.kind == VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BOARD_SCAN &&
              fact.board_scan ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_STABLE &&
              fact.ambient_sensor_count == 2U &&
              fact.source ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_SOURCE_STABLE &&
              fact.power_envelope_percent == 100U &&
              fact.brownout_detected == 0U,
          "runtime-health fact default initializes a safe v1 prefix");

    virtual_litch_runtime_health health{};
    virtual_litch_default_runtime_health(&health);
    Check(health.struct_size == sizeof(health) &&
              health.abi_version ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_ABI_VERSION &&
              health.state == VIRTUAL_LITCH_RUNTIME_HEALTH_HEALTHY &&
              health.revision == 0U && health.causes == 0U &&
              health.board_input_allowed != 0U &&
              health.move_acceptance_allowed != 0U &&
              health.clocks_allowed != 0U,
          "runtime-health snapshot default initializes a healthy v1 prefix");

    virtual_litch* board = CreateBoard(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    health = GetRuntimeHealth(board);
    Check(health.state == VIRTUAL_LITCH_RUNTIME_HEALTH_HEALTHY &&
              health.revision == 0U && health.causes == 0U,
          "new VirtualLitch handles expose authoritative healthy revision zero");
    Check(virtual_litch_dispatch_runtime_health_fact(nullptr, &fact) ==
                  VIRTUAL_LITCH_NULL_ARGUMENT &&
              virtual_litch_dispatch_runtime_health_fact(board, nullptr) ==
                  VIRTUAL_LITCH_NULL_ARGUMENT &&
              virtual_litch_get_runtime_health(nullptr, &health) ==
                  VIRTUAL_LITCH_NULL_ARGUMENT &&
              virtual_litch_get_runtime_health(board, nullptr) ==
                  VIRTUAL_LITCH_NULL_ARGUMENT,
          "runtime-health ABI rejects null handles and values");

    virtual_litch_runtime_health uninitializedHealth{};
    Check(virtual_litch_get_runtime_health(board, &uninitializedHealth) ==
              VIRTUAL_LITCH_ABI_MISMATCH,
          "runtime-health query requires an initialized ABI prefix");
    LargerRuntimeHealth largerHealth{};
    virtual_litch_default_runtime_health(&largerHealth.base);
    largerHealth.base.struct_size = sizeof(largerHealth);
    Check(virtual_litch_get_runtime_health(
              board,
              reinterpret_cast<virtual_litch_runtime_health*>(
                  &largerHealth)) == VIRTUAL_LITCH_OK &&
              largerHealth.base.revision == 0U,
          "runtime-health query accepts larger future caller storage");

    virtual_litch_default_runtime_health_fact(&fact);
    fact.abi_version += 1U;
    Check(virtual_litch_dispatch_runtime_health_fact(board, &fact) ==
              VIRTUAL_LITCH_ABI_MISMATCH,
          "runtime-health fact rejects a wrong subsystem ABI");
    virtual_litch_default_runtime_health_fact(&fact);
    fact.struct_size -= 1U;
    Check(virtual_litch_dispatch_runtime_health_fact(board, &fact) ==
              VIRTUAL_LITCH_ABI_MISMATCH,
          "runtime-health fact rejects an undersized prefix");
    virtual_litch_default_runtime_health_fact(&fact);
    fact.kind = 255U;
    Check(virtual_litch_dispatch_runtime_health_fact(board, &fact) ==
              VIRTUAL_LITCH_INVALID_ENUM,
          "runtime-health fact rejects an unknown kind");
    virtual_litch_default_runtime_health_fact(&fact);
    fact.board_scan = 2U;
    Check(virtual_litch_dispatch_runtime_health_fact(board, &fact) ==
              VIRTUAL_LITCH_INVALID_ENUM,
          "runtime-health fact rejects an unknown board-scan value");
    virtual_litch_default_runtime_health_fact(&fact);
    fact.source = 2U;
    Check(virtual_litch_dispatch_runtime_health_fact(board, &fact) ==
              VIRTUAL_LITCH_INVALID_ENUM,
          "runtime-health fact rejects an unknown source value");
    virtual_litch_default_runtime_health_fact(&fact);
    fact.ambient_sensor_count = 3U;
    Check(virtual_litch_dispatch_runtime_health_fact(board, &fact) ==
              VIRTUAL_LITCH_INVALID_CONFIG,
          "runtime-health fact rejects an impossible ambient count");
    virtual_litch_default_runtime_health_fact(&fact);
    fact.power_envelope_percent = 101U;
    Check(virtual_litch_dispatch_runtime_health_fact(board, &fact) ==
              VIRTUAL_LITCH_INVALID_CONFIG,
          "runtime-health fact rejects an impossible power percentage");
    virtual_litch_default_runtime_health_fact(&fact);
    fact.brownout_detected = 2U;
    Check(virtual_litch_dispatch_runtime_health_fact(board, &fact) ==
              VIRTUAL_LITCH_INVALID_CONFIG,
          "runtime-health fact rejects a non-boolean brownout value");
    virtual_litch_default_runtime_health_fact(&fact);
    fact.reserved[0] = 1U;
    Check(virtual_litch_dispatch_runtime_health_fact(board, &fact) ==
              VIRTUAL_LITCH_INVALID_CONFIG,
          "runtime-health fact rejects nonzero reserved bytes");

    LargerRuntimeHealthFact largerFact{};
    virtual_litch_default_runtime_health_fact(&largerFact.base);
    largerFact.base.struct_size = sizeof(largerFact);
    largerFact.base.kind =
        VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_AMBIENT_SENSOR_COUNT;
    largerFact.base.ambient_sensor_count = 1U;
    Check(virtual_litch_dispatch_runtime_health_fact(
              board,
              reinterpret_cast<const virtual_litch_runtime_health_fact*>(
                  &largerFact)) == VIRTUAL_LITCH_OK,
          "runtime-health fact accepts a larger future request prefix");
    health = GetRuntimeHealth(board);
    Check(health.state ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_DEGRADED_USABLE &&
              health.revision == 1U &&
              health.causes ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_ONE_AMBIENT_SENSOR,
          "runtime-health fact reaches the instance-owned production reducer");
    Check(virtual_litch_dispatch_runtime_health_fact(
              board, &largerFact.base) == VIRTUAL_LITCH_OK &&
              GetRuntimeHealth(board).revision == 1U,
          "repeated no-op health evidence does not bump the revision");
    Check(virtual_litch_reset(board) == VIRTUAL_LITCH_OK,
          "VirtualLitch reset accepts an active health cause");
    health = GetRuntimeHealth(board);
    Check(health.state == VIRTUAL_LITCH_RUNTIME_HEALTH_HEALTHY &&
              health.revision == 0U && health.causes == 0U,
          "VirtualLitch reset restores deterministic health revision zero");
    virtual_litch_destroy(board);
}

void TestSleepLifecycle() {
    virtual_litch* board = CreateBoard(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    virtual_litch_semantic_frame baseline{};
    Check(virtual_litch_get_semantic_frame(board, &baseline) ==
              VIRTUAL_LITCH_OK,
          "baseline semantic frame is queryable before sleep");

    Check(virtual_litch_request_sleep(
              board, VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE) ==
              VIRTUAL_LITCH_OK,
          "Idle starts VirtualLitch sleep countdown");
    virtual_litch_sleep_lifecycle lifecycle = GetSleep(board);
    Check(lifecycle.state == VIRTUAL_LITCH_SLEEP_COUNTDOWN &&
              lifecycle.origin == VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE &&
              lifecycle.remaining_countdown_ms == 3000U,
          "sleep countdown records state, first origin, and deadline");
    Check(virtual_litch_request_sleep(
              board, VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE) ==
              VIRTUAL_LITCH_OK,
          "a second interruptible request is satisfied without restarting countdown");
    lifecycle = GetSleep(board);
    Check(lifecycle.origin == VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE &&
              lifecycle.remaining_countdown_ms == 3000U,
          "first origin and original deadline win across duplicate requests");
    Check(virtual_litch_request_sleep(
              board, VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE) ==
              VIRTUAL_LITCH_OK,
          "duplicate idle request preserves the first countdown");
    Check(virtual_litch_request_sleep(board, 99U) ==
              VIRTUAL_LITCH_INVALID_ENUM,
          "invalid sleep origin remains an enum error");

    Check(virtual_litch_step_until(board, 2999U) == VIRTUAL_LITCH_OK,
          "countdown reaches the final millisecond deterministically");
    virtual_litch_semantic_frame finalCountdown{};
    Check(virtual_litch_get_semantic_frame(board, &finalCountdown) ==
              VIRTUAL_LITCH_OK,
          "final countdown semantic frame is queryable");
    lifecycle = GetSleep(board);
    Check(lifecycle.state == VIRTUAL_LITCH_SLEEP_COUNTDOWN &&
              lifecycle.remaining_countdown_ms == 1U,
          "2999ms remains in countdown with one millisecond left");
    Check(finalCountdown.board_hash == baseline.board_hash,
          "sleep cue leaves board semantic pixels unchanged");
    uint8_t litStatus = 0U;
    for (const auto& color : finalCountdown.status) {
        litStatus += ColorEnergy(color) != 0U ? 1U : 0U;
    }
    Check(litStatus == VIRTUAL_LITCH_STATUS_LED_COUNT,
          "countdown gathers the perimeter cue from three LEDs through the full ring");
    Check(virtual_litch_step_until(board, 3500U) == VIRTUAL_LITCH_OK,
          "countdown reaches preflight boundary");
    lifecycle = GetSleep(board);
    Check(lifecycle.state == VIRTUAL_LITCH_SLEEP_PREPARING &&
              lifecycle.remaining_countdown_ms == 0U,
          "3000ms countdown plus 500ms fade enters Preparing");
    Check(virtual_litch_resolve_sleep_preflights(board, 0U, 1U) ==
              VIRTUAL_LITCH_POLICY_REJECTED,
          "failed simulated persistence preflight rejects sleep");
    lifecycle = GetSleep(board);
    Check(lifecycle.state == VIRTUAL_LITCH_SLEEP_AWAKE &&
              lifecycle.cancel_cause == VIRTUAL_LITCH_SLEEP_CANCEL_PREFLIGHT_FAILURE,
          "failed preflight returns to Awake with its typed cause");

    Check(virtual_litch_request_sleep(
              board, VIRTUAL_LITCH_SLEEP_ORIGIN_CONSOLE) ==
                  VIRTUAL_LITCH_OK &&
              virtual_litch_advance(board, 3500U) == VIRTUAL_LITCH_OK,
          "console request reaches preparation through the same fake clock");
    Check(virtual_litch_resolve_sleep_preflights(board, 1U, 1U) ==
              VIRTUAL_LITCH_OK,
          "successful simulated preflights commit fake sleep");
    lifecycle = GetSleep(board);
    Check(lifecycle.state == VIRTUAL_LITCH_SLEEP_SLEEPING &&
              lifecycle.persistence_preflight_complete != 0U &&
              lifecycle.wake_source_preflight_complete != 0U &&
              lifecycle.sleep_authorization_pending == 0U,
          "fake sleep consumes authorization without invoking host sleep");
    Check(virtual_litch_simulate_sleep_wake(board) == VIRTUAL_LITCH_OK &&
              virtual_litch_report_wake_buttons_released(board) ==
                  VIRTUAL_LITCH_OK,
          "simulated wake enters the held-input guard");
    Check(virtual_litch_advance(board, 199U) == VIRTUAL_LITCH_OK &&
              GetSleep(board).state == VIRTUAL_LITCH_SLEEP_WAKE_GUARD,
          "WakeGuard holds through 199ms after button release");
    Check(virtual_litch_advance(board, 1U) == VIRTUAL_LITCH_OK &&
              GetSleep(board).state == VIRTUAL_LITCH_SLEEP_AWAKE,
          "WakeGuard exits at the 200ms boundary");
    virtual_litch_destroy(board);
}

// Named SharedSleepActivityOption journey: existing app-origin ingress,
// production SleepTransition/compositor/mapping/brightness/current limiter and
// engine-backed authority. Persistence outcomes and validated tap classification
// are injected only at their existing platform boundaries; no real sleep/I2C.
void TestAppSleepCountUpFadeWakeEventImmunityJourney() {
    constexpr uint64_t INITIAL_OCCUPANCY = UINT64_C(0xFFFF00000000FFFF);
    test_support::BoundedHintEngine engine;
    JourneySession session(ConfigFor(VIRTUAL_LITCH_HARDWARE_V3_3),
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
        nullptr, &engine);
    Check(DispatchJourneySnapshot(session, INITIAL_OCCUPANCY) == VIRTUAL_LITCH_OK &&
              session.Advance(1000U) == VIRTUAL_LITCH_OK,
          "sleep journey starts from observed opening occupancy");
    const auto baseline = session.State();
    const char* initialFen = session.HomeStateSnapshot().fen;
    const std::string committedFen = initialFen != nullptr ? initialFen : "";
    Check(!committedFen.empty(), "sleep journey retains an authoritative starting FEN");
    const auto countOrange = [&]() {
        unsigned count = 0U;
        for (const auto color : session.SemanticFrame().status)
            count += color.r > color.g && color.g > 0U && color.b == 0U;
        return count;
    };
    const auto physicalRing = [&]() {
        // Independent V3.3 wiring oracle from hardware_platform.md:
        // 256 board pixels, then 44 status pixels; controls occupy 4..9/26..31.
        unsigned count = 0U;
        const auto& frame = session.PhysicalFrame();
        for (unsigned chain = 0U; chain < 44U; ++chain) {
            if ((chain >= 4U && chain <= 9U) || (chain >= 26U && chain <= 31U)) continue;
            const auto color = frame.pixels[256U + chain];
            count += color.r > color.g && color.g > 0U && color.b == 0U;
        }
        return count;
    };
    const auto energy = [&]() {
        uint32_t value = 0U;
        const auto& frame = session.PhysicalFrame();
        for (uint16_t led = 0U; led < frame.led_count; ++led)
            value += ColorEnergy(frame.pixels[led]);
        return value;
    };
    const auto checkpoint = [&](const char* stage, const char* input,
                                 const char* result, const char* next) {
        const auto& lifecycle = session.SleepLifecycle();
        const auto& state = session.State();
        const auto text = session.DisplayText();
        std::printf("[APP_SLEEP_JOURNEY] stage=%s t=%u input=%s result=%s "
            "occupancy=%016llx expected=%016llx phase=%u action=%u sleep=%u "
            "remaining=%u text=%.*s orange=%u/32 physical_ring=%u/32 physical_energy=%u "
            "cancel=%u next=%s\n", stage, session.NowMs(), input, result,
            static_cast<unsigned long long>(state.physical_occupied_mask),
            static_cast<unsigned long long>(state.expected_occupied_mask),
            state.interaction_phase, state.required_action, lifecycle.state,
            lifecycle.remaining_countdown_ms, static_cast<int>(text.size()),
            text.data(), countOrange(), physicalRing(), energy(), lifecycle.cancel_cause, next);
    };
    const auto unchanged = [&]() {
        const auto& state = session.State();
        return state.physical_occupied_mask == baseline.physical_occupied_mask &&
            state.expected_occupied_mask == baseline.expected_occupied_mask &&
            state.pending_action_token == baseline.pending_action_token &&
            state.side_to_move_black == baseline.side_to_move_black &&
            state.interaction_phase == baseline.interaction_phase &&
            state.board_observation_sequence == baseline.board_observation_sequence;
    };
    const auto finishFade = [&](const uint32_t started, const uint32_t countdownMs) {
        Check(session.StepUntil(started + countdownMs) == VIRTUAL_LITCH_OK &&
                  session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_FADING &&
                  session.SleepLifecycle().remaining_countdown_ms == 0U &&
                  (countdownMs == 0U ? countOrange() < 32U && physicalRing() < 32U
                                    : countOrange() == 32U && physicalRing() == 32U),
              "Idle completes its mapped orange count-up; App enters the same fade immediately");
        uint32_t previousEnergy = energy();
        Check(previousEnergy > 0U, "fade begins with visible physical output");
        checkpoint("fade begins", countdownMs == 0U ? "App entry" : "three-second cue elapsed",
                   "fading", "wait for the half-second fade");
        for (const uint32_t elapsed : {100U, 250U, 400U}) {
            Check(session.StepUntil(started + countdownMs + elapsed) == VIRTUAL_LITCH_OK &&
                      session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_FADING &&
                      energy() > 0U && energy() < previousEnergy,
                  "production RGBW output visibly decreases through intermediate fade frames");
            previousEnergy = energy();
            Check(session.ResolveSleepPreflights(true, true) == VIRTUAL_LITCH_POLICY_REJECTED &&
                      session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_FADING,
                  "neither policy can authorize teardown during its fade");
            checkpoint("fade progress", "cooperative render tick", "fading", "wait for black before save");
        }
        Check(session.StepUntil(started + countdownMs + 500U) == VIRTUAL_LITCH_OK &&
                  session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_PREPARING && energy() == 0U,
              "both policies reach black preparation only after the complete half-second fade");
        checkpoint("fade complete", "500ms fade elapsed", "preparing", "wait for verified save");
    };
    auto started = session.NowMs();
    Check(session.RequestSleep(VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE) == VIRTUAL_LITCH_OK &&
              session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_COUNTDOWN &&
              session.SleepLifecycle().remaining_countdown_ms == 3000U && countOrange() == 3U,
          "true uses the existing informative three-second count-up");
    checkpoint("interruptible count-up", "Idle request / true", "accepted", "wait or use normal activity to cancel");
    Check(session.StepUntil(started + 1500U) == VIRTUAL_LITCH_OK && countOrange() > 3U && countOrange() < 32U,
          "true count-up advances through the production mapped perimeter");
    finishFade(started, 3000U);
    Check(session.DispatchValidatedMotionOrTap() == VIRTUAL_LITCH_OK &&
              session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_AWAKE && unchanged(),
          "true configured motion cancels without changing the game");
    checkpoint("interruptible cancellation", "configured tap", "cancelled", "continue current game");
    (void)session.DispatchButton(VIRTUAL_LITCH_OWNER_P1, VIRTUAL_LITCH_BUTTON_P1_X,
                                 VIRTUAL_LITCH_BUTTON_PRESS_EVENT);
    started = session.NowMs();
    (void)session.RequestSleep(VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE);
    (void)session.Advance(100U);
    started = session.NowMs();
    Check(session.RequestSleep(VIRTUAL_LITCH_SLEEP_ORIGIN_APP) == VIRTUAL_LITCH_OK &&
              session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_FADING &&
              session.SleepLifecycle().remaining_countdown_ms == 0U &&
              session.SleepLifecycle().origin == VIRTUAL_LITCH_SLEEP_ORIGIN_APP && energy() > 0U,
          "App takeover starts the immediate half-second fade without a count-up");
    checkpoint("manual takeover", "App request / false during Idle", "accepted", "wait for the half-second fade");
    Check(session.DispatchValidatedMotionOrTap() == VIRTUAL_LITCH_POLICY_REJECTED &&
              session.DispatchButton(VIRTUAL_LITCH_OWNER_P1, VIRTUAL_LITCH_BUTTON_UP,
                                    VIRTUAL_LITCH_BUTTON_PRESS_EVENT) == VIRTUAL_LITCH_OK &&
              session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_FADING && unchanged(),
          "false ignores motion and consumes the physical control before menu/gameplay dispatch");
    checkpoint("normal activity ignored", "tap plus Up Press", "consumed", "wait for verified save");
    finishFade(started, 0U);
    (void)session.Advance(2100U);
    for (const auto event : {VIRTUAL_LITCH_BUTTON_REPEAT_EVENT,
                             VIRTUAL_LITCH_BUTTON_LONG_HOLD_EVENT,
                             VIRTUAL_LITCH_BUTTON_RELEASE_EVENT}) {
        Check(session.DispatchButton(VIRTUAL_LITCH_OWNER_P1, VIRTUAL_LITCH_BUTTON_P1_X, event) == VIRTUAL_LITCH_OK &&
                  session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_PREPARING &&
                  !session.QuickSettingsActive(),
              "false retires preexisting holds and consumes repeat/hold/release without opening a menu");
    }
    checkpoint("prior hold retired", "preexisting X hold and later events", "consumed", "wait for verified save");
    constexpr uint8_t E2 = 52U;
    constexpr uint64_t E2_MASK = UINT64_C(1) << E2;
    Check(session.DispatchBoardObservation(E2_MASK, INITIAL_OCCUPANCY & ~E2_MASK) == VIRTUAL_LITCH_OK &&
              session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_PREPARING &&
              session.State().physical_occupied_mask == (INITIAL_OCCUPANCY & ~E2_MASK) &&
              committedFen == session.HomeStateSnapshot().fen,
          "false preserves authoritative Hall sampling and persistence context without cancelling entry");
    checkpoint("Hall still authoritative", "e2 lifted", "observed", "wait for verified save");
    Check(session.ResolveSleepPreflights(false, true) == VIRTUAL_LITCH_POLICY_REJECTED &&
              session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_AWAKE && energy() > 0U,
          "false retains common persistence failure recovery");
    checkpoint("save failure", "persistence failure", "rejected", "follow current piece guidance");
    Check(session.DispatchSensorDelta(E2, true) == VIRTUAL_LITCH_OK &&
              session.State().physical_occupied_mask == INITIAL_OCCUPANCY,
          "Hall recovery returns to the authoritative board before retry");
    started = session.NowMs();
    (void)session.RequestSleep(VIRTUAL_LITCH_SLEEP_ORIGIN_APP);
    finishFade(started, 0U);
    Check(session.ResolveSleepPreflights(true, false) == VIRTUAL_LITCH_POLICY_REJECTED &&
              session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_AWAKE && energy() > 0U,
          "false retains the same wake-source failure guard");
    checkpoint("wake preparation failure", "wake-source unavailable", "rejected", "release controls and retry");
    started = session.NowMs();
    (void)session.RequestSleep(VIRTUAL_LITCH_SLEEP_ORIGIN_APP);
    finishFade(started, 0U);
    Check(session.ResolveSleepPreflights(true, true) == VIRTUAL_LITCH_OK &&
              session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_SLEEPING && energy() == 0U,
          "false authorizes sleep only after the common verified preflights");
    checkpoint("sleep authorized", "verified common preflights", "accepted", "use the same configured wake inputs");
    Check(session.SimulateSleepWake() == VIRTUAL_LITCH_OK &&
              session.ReportWakeButtonsReleased() == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_AWAKE,
          "same simulated wake release guard returns to play");
    checkpoint("wake guard complete", "simulated wake plus release +200ms", "accepted", "continue current game");
}

void TestSleepCancellationAndLiveState() {
    const auto checkConsumed = [](const char* message,
                                  virtual_litch_result (*dispatch)(
                                      virtual_litch*)) {
        virtual_litch* board = CreateBoard(
            ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
        virtual_litch_state before{};
        (void)virtual_litch_get_state(board, &before);
        (void)virtual_litch_request_sleep(
            board, VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE);
        Check(dispatch(board) == VIRTUAL_LITCH_OK, message);
        const virtual_litch_sleep_lifecycle lifecycle = GetSleep(board);
        virtual_litch_state after{};
        (void)virtual_litch_get_state(board, &after);
        Check(lifecycle.state == VIRTUAL_LITCH_SLEEP_AWAKE &&
                  after.physical_occupied_mask == before.physical_occupied_mask &&
                  after.board_observation_sequence == before.board_observation_sequence,
              "sleep activity cancellation is consumed before reducer dispatch");
        virtual_litch_destroy(board);
    };
    const auto button = [](virtual_litch* board) {
        return virtual_litch_dispatch_button(
            board, VIRTUAL_LITCH_OWNER_P1, VIRTUAL_LITCH_BUTTON_UP,
            VIRTUAL_LITCH_BUTTON_PRESS_EVENT);
    };
    const auto observation = [](virtual_litch* board) {
        return virtual_litch_dispatch_board_observation(board, 1ULL, 1ULL);
    };
    const auto sensor = [](virtual_litch* board) {
        return virtual_litch_dispatch_sensor_delta(board, 0U, 1U);
    };
    checkConsumed("button press cancels and is consumed", button);
    for (const auto dispatch : {+observation, +sensor}) {
        virtual_litch* changedBoard = CreateBoard(ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
        (void)virtual_litch_request_sleep(changedBoard, VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE);
        Check(dispatch(changedBoard) == VIRTUAL_LITCH_OK &&
                  GetSleep(changedBoard).state == VIRTUAL_LITCH_SLEEP_AWAKE,
              "accepted Hall input cancels sleep after ordinary observation dispatch");
        virtual_litch_state observed{};
        (void)virtual_litch_get_state(changedBoard, &observed);
        Check(observed.physical_occupied_mask == 1ULL && observed.board_observation_sequence != 0U,
              "Hall cancellation retains actual occupancy instead of discarding the accepted observation");
        virtual_litch_destroy(changedBoard);
    }
    checkConsumed("validated motion cancels and is consumed",
                  virtual_litch_dispatch_validated_motion_or_tap);

    virtual_litch* board = CreateBoard(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_C3));
    (void)virtual_litch_request_sleep(board,
                                         VIRTUAL_LITCH_SLEEP_ORIGIN_MENU);
    Check(virtual_litch_reset(board) == VIRTUAL_LITCH_OK &&
              GetSleep(board).state == VIRTUAL_LITCH_SLEEP_AWAKE &&
              GetSleep(board).origin == VIRTUAL_LITCH_SLEEP_ORIGIN_NONE,
          "reset clears the live fake sleep state");
    virtual_litch_destroy(board);
}

void TestSleepCuePhysicalPipeline() {
    const auto checkHardware = [](const uint8_t hardwareType,
                                  const char* name) {
        virtual_litch_config config = ConfigFor(hardwareType);
        virtual_litch* board = CreateBoard(config);
        virtual_litch_physical_frame before{};
        (void)virtual_litch_get_physical_frame(board, &before);
        (void)virtual_litch_request_sleep(
            board, VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE);
        virtual_litch_semantic_frame semantic{};
        virtual_litch_physical_frame physical{};
        Check(virtual_litch_get_semantic_frame(board, &semantic) ==
                      VIRTUAL_LITCH_OK &&
                  virtual_litch_get_physical_frame(board, &physical) ==
                      VIRTUAL_LITCH_OK,
              "sleep cue captures semantic and final physical output");
        const uint8_t physicalStatus =
            modules::status_led_frame_adapter::PhysicalIndexForSemanticIndex(
                0U, false);
        const uint16_t target = hardwareType == VIRTUAL_LITCH_HARDWARE_C3
                                    ? modules::status_led_frame_adapter::
                                          VirtualLedIndexForPhysicalStatusIndex(
                                              physicalStatus)
                                    : static_cast<uint16_t>(
                                          hardware::BOARD_LED_COUNT +
                                          physicalStatus);
        Check(ColorEnergy(semantic.status[0]) != 0U &&
                  target < physical.led_count &&
                  ColorEnergy(physical.pixels[target]) != 0U &&
                  before.frame_hash != physical.frame_hash,
              name);
        virtual_litch_destroy(board);
    };
    checkHardware(VIRTUAL_LITCH_HARDWARE_C3,
                  "C3 maps the fake sleep cue through virtual perimeter output");
    checkHardware(VIRTUAL_LITCH_HARDWARE_S3,
                  "S3 maps the fake sleep cue through dedicated RGBW perimeter output");

    virtual_litch* board = CreateBoard(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    Check(virtual_litch_request_sleep(
              board, VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE) ==
              VIRTUAL_LITCH_OK,
          "sleep countdown starts before the health-priority intersection");
    virtual_litch_runtime_health_fact uncertain{};
    virtual_litch_default_runtime_health_fact(&uncertain);
    uncertain.kind = VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BOARD_SCAN;
    uncertain.board_scan =
        VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN;
    Check(virtual_litch_dispatch_runtime_health_fact(board, &uncertain) ==
                  VIRTUAL_LITCH_OK &&
              virtual_litch_advance(board, 100U) == VIRTUAL_LITCH_OK &&
              virtual_litch_dispatch_runtime_health_fact(
                  board, &uncertain) == VIRTUAL_LITCH_OK,
          "sustained uncertainty enters paused health during countdown");
    virtual_litch_semantic_frame healthSemantic{};
    virtual_litch_physical_frame healthPhysical{};
    Check(virtual_litch_get_semantic_frame(
              board, &healthSemantic) == VIRTUAL_LITCH_OK &&
              virtual_litch_get_physical_frame(
                  board, &healthPhysical) == VIRTUAL_LITCH_OK &&
              LitStatusPixelMask(healthSemantic) ==
                  UINT32_C(0x33333333) &&
              LitDedicatedPhysicalStatusPixelMask(healthPhysical) ==
                  UINT32_C(0x33333333),
          "paused health replaces sleep countdown through semantic and physical output");
    virtual_litch_destroy(board);
}

void TestSemanticAndHardwareMappings() {
    virtual_litch* c3 = CreateBoard(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_C3));
    virtual_litch* s3 = CreateBoard(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));

    virtual_litch_semantic_frame c3Semantic{};
    virtual_litch_semantic_frame s3Semantic{};
    virtual_litch_physical_frame c3Physical{};
    virtual_litch_physical_frame s3Physical{};
    Check(virtual_litch_get_semantic_frame(c3, &c3Semantic) ==
              VIRTUAL_LITCH_OK,
          "C3 semantic frame query succeeds");
    Check(virtual_litch_get_semantic_frame(s3, &s3Semantic) ==
              VIRTUAL_LITCH_OK,
          "S3 semantic frame query succeeds");
    Check(c3Semantic.combined_hash == s3Semantic.combined_hash,
          "semantic output is independent of the selected hardware mapper");
    Check(c3Semantic.leds_per_square == 3U,
          "semantic output exposes the current production slot count");

    Check(virtual_litch_get_physical_frame(c3, &c3Physical) ==
              VIRTUAL_LITCH_OK,
          "C3 physical frame query succeeds");
    Check(virtual_litch_get_physical_frame(s3, &s3Physical) ==
              VIRTUAL_LITCH_OK,
          "S3 physical frame query succeeds");
    Check(c3Physical.led_count == 192U &&
              c3Physical.channels_per_led == 3U,
          "C3 maps status onto a 192-pixel RGB surface");
    Check(s3Physical.led_count == 224U &&
              s3Physical.channels_per_led == 4U,
          "S3 maps status onto a dedicated 32-pixel RGBW chain");
    Check(!AnyWhiteChannel(c3Physical),
          "C3 physical output never exposes a white channel");
    Check(AnyWhiteChannel(s3Physical),
          "S3 physical output applies the production RGBW auto-map");
    Check(c3Physical.frame_hash != s3Physical.frame_hash,
          "hardware mapping changes the post-budget physical hash");

    virtual_litch* defaultBoard = nullptr;
    Check(virtual_litch_create(nullptr, &defaultBoard) == VIRTUAL_LITCH_OK,
          "default config creates existing S3 profile");
    virtual_litch_physical_frame defaultPhysical{};
    (void)virtual_litch_get_physical_frame(defaultBoard, &defaultPhysical);
    Check(defaultPhysical.frame_hash == s3Physical.frame_hash &&
              defaultPhysical.led_count == s3Physical.led_count,
          "default physical bytes remain identical to explicit V2 profile");
    virtual_litch_destroy(defaultBoard);
    virtual_litch_destroy(c3);
    virtual_litch_destroy(s3);
}


void CheckV3ProductionFrame(
    const runtime::virtual_litch_internal::Session& session,
    const virtual_litch_config& config, const bool boardRotate,
    const bool firstPlayerIsP2) {
    const auto& semantic = session.SemanticFrame();
    const auto& actual = session.PhysicalFrame();
    Check(semantic.leds_per_square == 3U && actual.led_count == 288U &&
              actual.channels_per_led == 4U,
          "V3 adds physical topology without changing semantic ABI");
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    for (uint8_t square = 0U; square < 64U; ++square) {
        for (uint8_t slot = 0U; slot < 3U; ++slot) {
            const auto color = semantic.board[square][slot];
            board[square * 3U + slot] = {color.r, color.g, color.b};
        }
        Check(ColorEnergy(semantic.board[square][3U]) == 0U,
              "reserved fourth semantic slot is not reinterpreted");
    }
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        const auto color = semantic.status[index];
        status[index] = {color.r, color.g, color.b};
    }
    const bool sleepCountdownActive =
        session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_COUNTDOWN;
    const auto physical = hardware::MapSemanticLedFrame(
        {hardware::BoardRevision::V3S3,
         modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2),
         sleepCountdownActive
             ? boardRotate
             : modules::ComputeControlBankSwap(boardRotate, firstPlayerIsP2, false),
         session.SidePrioritySquareMask()}, board, status);
    const auto policyConfig = modules::led_render_policy::MakePowerBudgetPolicyConfig(
        {true, config.recovery_power_limit != 0U});
    modules::PowerBudgetPolicy policy(policyConfig);
    runtime::virtual_litch_internal::CaptureLedRenderer renderer(288U, true);
    modules::BudgetAwareLedPipeline pipeline(renderer, policy,
        modules::led_render_policy::MakePipelineConfig(true, 32U));
    pipeline.SetBrightnessScale01(static_cast<float>(config.brightness_scale_256) / 256.0f);
    pipeline.RenderFrame(physical.data(), 288U);
    for (uint16_t index = 0U; index < 288U; ++index) {
        const auto& a = actual.pixels[index];
        const auto& b = renderer.Pixels()[index];
        Check(a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w,
              "V3 journey emits exact shared production mapper/pipeline bytes");
    }
    const auto stats = pipeline.GetLastStats();
    Check(actual.applied_led_current_ma == stats.appliedLedCurrentMa &&
              actual.estimated_led_current_ma == stats.estimatedLedCurrentMa &&
              actual.power_limited == (stats.powerLimited ? 1U : 0U) &&
              static_cast<uint32_t>(actual.applied_led_current_ma) +
                  policyConfig.fixedSystemCurrentMa <= policyConfig.maxTotalCurrentMa,
          "V3 current and limiting evidence include all 288 RGBW emitters");
    for (uint8_t square = 0U; square < 64U; ++square) {
        const auto targets = hardware::MapSemanticSquareLedTargets(
            hardware::BoardRevision::V3S3, square, hardware::SquareLedSlot::Piece,
            modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2));
        const auto a = actual.pixels[targets.indices[0]];
        const auto b = actual.pixels[targets.indices[1]];
        Check(a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w,
              "V3 transport preserves existing equal Top/Bottom piece output");
    }
}

void TestV3ProfileAndProductionJourney() {
    static_assert(VIRTUAL_LITCH_HARDWARE_C3 == 1 && VIRTUAL_LITCH_HARDWARE_S3 == 2 &&
                      VIRTUAL_LITCH_HARDWARE_V3 == 3 &&
                      VIRTUAL_LITCH_HARDWARE_V3_3 == 4 &&
                      VIRTUAL_LITCH_ABI_VERSION == 1U,
                  "hardware profiles are append-only with existing ABI");
    auto config = ConfigFor(VIRTUAL_LITCH_HARDWARE_V3);
    virtual_litch* handle = nullptr;
    config.hardware = 5U;
    Check(virtual_litch_create(&config, &handle) == VIRTUAL_LITCH_INVALID_CONFIG && handle == nullptr,
          "unknown profile remains rejected instead of becoming V3");
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.max_led_current_ma = UINT16_MAX;
    Check(virtual_litch_create(&config, &handle) == VIRTUAL_LITCH_INVALID_CONFIG && handle == nullptr,
          "V3 current override reserves fixed-system integer headroom");
    config.max_led_current_ma = 0U;
    handle = CreateBoard(config);
    virtual_litch_destroy(handle);
    virtual_litch_default_config(&config);
    Check(config.hardware == VIRTUAL_LITCH_HARDWARE_S3,
          "default remains deployed V2/S3");

    using runtime::virtual_litch_internal::Session;
    using runtime::virtual_litch_internal::SessionMode;
    constexpr uint64_t opening = UINT64_C(0xFFFF00000000FFFF);
    for (uint8_t envelope = 0U; envelope < 3U; ++envelope) {
        for (const bool boardRotate : {false, true}) {
            for (const bool firstPlayerIsP2 : {false, true}) {
                config = ConfigFor(VIRTUAL_LITCH_HARDWARE_V3);
                config.brightness_scale_256 = envelope == 2U ? 8U : 256U;
                config.strong_power_source = envelope == 0U ? 1U : 0U;
                config.recovery_power_limit = envelope == 0U ? 0U : 1U;
                test_support::BoundedHintEngine engine;
                Session session(config, SessionMode::EngineBackedJourney, nullptr, &engine);
                session.SettingsForPersistenceTest().Doc()["BOARD_ROTATE"] = boardRotate;
                session.SettingsForPersistenceTest().Doc()["GAME_FIRST_PLAYER"] = firstPlayerIsP2 ? 2U : 1U;
                virtual_litch_setting_request request{};
                virtual_litch_default_setting_request(&request);
                request.request_id = 71U;
                std::snprintf(request.key, sizeof(request.key), "BOARD_SETUP_ASSISTANCE");
                std::snprintf(request.value_json, sizeof(request.value_json), "false");
                modules::ControlMutationOutcome outcome;
                Check(session.MutateSetting(request, outcome) == VIRTUAL_LITCH_OK &&
                          outcome.status == modules::ControlMutationStatus::Applied,
                      "V3 uses production acknowledged setting mutation");
                Check(session.DispatchBoardObservation(UINT64_MAX, opening) == VIRTUAL_LITCH_OK,
                      "V3 production engine receives actual Hall occupancy");
                CheckV3ProductionFrame(session, config, boardRotate, firstPlayerIsP2);
                Check(session.DispatchBoardObservation(1ULL << 52U, opening & ~(1ULL << 52U)) == VIRTUAL_LITCH_OK &&
                          session.State().valid_destination_mask != 0ULL,
                      "V3 physical pickup enters real legal destination journey");
                CheckV3ProductionFrame(session, config, boardRotate, firstPlayerIsP2);
                Check(session.DispatchBoardObservation(1ULL << 52U, opening) == VIRTUAL_LITCH_OK,
                      "V3 physical return restores ordinary position");
                Check(session.RequestSleep(VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE) == VIRTUAL_LITCH_OK &&
                          session.SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_COUNTDOWN,
                      "V3 existing sleep owner admits countdown");
                CheckV3ProductionFrame(session, config, boardRotate, firstPlayerIsP2);
                std::printf("[V3_PROFILE] envelope=%u rotate=%u p2=%u pixels=%u current=%u status_start=256\n",
                    envelope, boardRotate, firstPlayerIsP2,
                    session.PhysicalFrame().led_count, session.PhysicalFrame().applied_led_current_ma);
            }
        }
    }
    config = ConfigFor(VIRTUAL_LITCH_HARDWARE_V3);
    test_support::BoundedHintEngine startupEngine;
    Session startup(config, SessionMode::EngineBackedJourney, nullptr, &startupEngine);
    startup.CaptureStartupFrame(100U, 256U);
    const auto& startupFrame = startup.PhysicalFrame();
    Check(startupFrame.led_count == 288U && startupFrame.channels_per_led == 4U &&
              startupFrame.applied_led_current_ma + modules::S3_FIXED_SYSTEM_CURRENT_MA <=
                  modules::S3_CONSERVATIVE_TOTAL_CURRENT_MA,
          "V3 startup grid uses RGBW transport and conservative production cap");
    for (uint16_t index = 256U; index < 288U; ++index) {
        Check(ColorEnergy(startupFrame.pixels[index]) == 0U,
              "V3 pre-services startup status remains dark at offset256");
    }
    virtual_litch_physical_frame badge{};
    for (const uint16_t offset : {0U, 7U, 8U, 31U}) { badge.pixels[256U + offset].w = 1U; }
    Check(virtual_litch_test::HasPhysicalCoachingBadge(badge, modules::UiOwner::P2,
              hardware::BoardRevision::V3S3) &&
              !virtual_litch_test::HasPhysicalCoachingBadge(badge, modules::UiOwner::P2),
          "V3 status helper reads offset256 while legacy callers retain192");
}

void TestBoardObservationsAndCastlingIntent() {
    virtual_litch* board = CreateBoard(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    const uint64_t king = 1ULL << 60U;
    const uint64_t rook = 1ULL << 63U;
    const uint64_t occupied = king | rook;
    virtual_litch_position position{};
    virtual_litch_default_position(&position);
    position.occupied_mask = occupied;
    position.king_mask = king;
    position.rook_mask = rook;
    position.mover_occupied_mask = occupied;
    position.side_to_move_valid = 1U;
    position.legal_castling_targets_valid = 1U;
    position.legal_castling_king_targets_mask = 1ULL << 62U;
    Check(virtual_litch_dispatch_position(board, &position) ==
              VIRTUAL_LITCH_OK,
          "authoritative position dispatch succeeds");
    Check(virtual_litch_dispatch_board_snapshot(board, occupied) ==
              VIRTUAL_LITCH_OK,
          "full physical snapshot dispatch succeeds");
    Check(virtual_litch_dispatch_board_snapshot(board, rook) ==
              VIRTUAL_LITCH_OK,
          "king pickup dispatch succeeds");

    virtual_litch_state state{};
    Check(virtual_litch_get_state(board, &state) ==
              VIRTUAL_LITCH_OK,
          "state query succeeds after observations");
    Check(state.interaction_phase == static_cast<uint8_t>(
                                         runtime::ChessInteractionPhase::PieceLifted),
          "production interaction reducer sees the king pickup");
    Check(state.selected_square == 60U,
          "state query exposes the selected king square");
    Check(state.castling_king_destination_mask == (1ULL << 62U),
          "state query preserves explicit castling intent");
    Check(state.castling_rook_source_mask == rook,
          "state query exposes the coordinated castling rook");

    Check(virtual_litch_dispatch_sensor_delta(board, 63U, 0U) ==
              VIRTUAL_LITCH_OK,
          "single-square delta dispatch succeeds");
    Check(virtual_litch_get_state(board, &state) ==
                  VIRTUAL_LITCH_OK &&
              state.physical_occupied_mask == 0ULL,
          "single-square delta updates the same production model");
    Check(virtual_litch_reset(board) == VIRTUAL_LITCH_OK,
          "session reset succeeds");
    Check(virtual_litch_dispatch_sensor_delta(board, 4U, 1U) ==
              VIRTUAL_LITCH_OK,
          "delta dispatch succeeds after reset");
    (void)virtual_litch_get_state(board, &state);
    Check(state.board_observation_sequence == 1U,
          "reset restores deterministic event sequencing");
    virtual_litch_destroy(board);
}

void TestClockAndHistoryRing() {
    virtual_litch_config config = ConfigFor(
        VIRTUAL_LITCH_HARDWARE_S3, 3U);
    virtual_litch* board = CreateBoard(config);
    Check(virtual_litch_advance(board, 10U) == VIRTUAL_LITCH_OK,
          "relative time advance succeeds");
    Check(virtual_litch_step_until(board, 20U) == VIRTUAL_LITCH_OK,
          "absolute time step succeeds");
    Check(virtual_litch_step_until(board, 30U) == VIRTUAL_LITCH_OK,
          "later absolute time step succeeds");
    Check(virtual_litch_step_until(board, 29U) ==
              VIRTUAL_LITCH_TIME_REVERSED,
          "session rejects time reversal");
    Check(virtual_litch_get_history_count(board) == 3U,
          "history is bounded by the configured ring capacity");

    virtual_litch_history_frame history{};
    Check(virtual_litch_get_history_frame(board, 0U, &history) ==
                  VIRTUAL_LITCH_OK &&
              history.state.now_ms == 10U,
          "history index zero is the oldest retained frame");
    Check(virtual_litch_get_history_frame(board, 2U, &history) ==
                  VIRTUAL_LITCH_OK &&
              history.state.now_ms == 30U,
          "history retains chronological order after wrapping");
    Check(virtual_litch_get_history_frame(board, 3U, &history) ==
              VIRTUAL_LITCH_OUT_OF_RANGE,
          "history rejects an unavailable index");
    virtual_litch_destroy(board);

    config = ConfigFor(VIRTUAL_LITCH_HARDWARE_C3, 0U);
    config.initial_time_ms = UINT32_MAX - 1U;
    board = CreateBoard(config);
    Check(virtual_litch_advance(board, 2U) ==
              VIRTUAL_LITCH_TIME_OVERFLOW,
          "relative step rejects uint32 clock overflow");
    Check(virtual_litch_get_history_count(board) == 0U,
          "zero-capacity history performs no hidden allocation");
    virtual_litch_destroy(board);
}

void TestInterleavedSessionOwnership() {
    virtual_litch_config aConfig = ConfigFor(
        VIRTUAL_LITCH_HARDWARE_S3);
    virtual_litch_config bConfig = ConfigFor(
        VIRTUAL_LITCH_HARDWARE_C3);
    aConfig.initial_time_ms = 100U;
    bConfig.initial_time_ms = 1000U;
    virtual_litch* a = CreateBoard(aConfig);
    virtual_litch* b = CreateBoard(bConfig);

    Check(virtual_litch_dispatch_board_snapshot(a, 1ULL << 1U) ==
              VIRTUAL_LITCH_OK,
          "first session accepts its board state");
    Check(virtual_litch_dispatch_board_snapshot(b, 1ULL << 62U) ==
              VIRTUAL_LITCH_OK,
          "second session accepts a different board state");
    Check(virtual_litch_advance(a, 25U) == VIRTUAL_LITCH_OK,
          "first session advances independently");
    Check(virtual_litch_advance(b, 50U) == VIRTUAL_LITCH_OK,
          "second session advances independently");

    virtual_litch_state aState{};
    virtual_litch_state bState{};
    (void)virtual_litch_get_state(a, &aState);
    (void)virtual_litch_get_state(b, &bState);
    Check(aState.now_ms == 125U && bState.now_ms == 1050U,
          "interleaved handles retain instance-owned clocks");
    Check(aState.physical_occupied_mask == (1ULL << 1U) &&
              bState.physical_occupied_mask == (1ULL << 62U),
          "interleaved handles retain instance-owned board models");

    Check(virtual_litch_dispatch_button(
              a, VIRTUAL_LITCH_OWNER_P2,
              VIRTUAL_LITCH_BUTTON_LEFT,
              VIRTUAL_LITCH_BUTTON_PRESS_EVENT) ==
              VIRTUAL_LITCH_OK,
          "S3 accepts a P2 control event");
    Check(virtual_litch_dispatch_button(
              b, VIRTUAL_LITCH_OWNER_P2,
              VIRTUAL_LITCH_BUTTON_LEFT,
              VIRTUAL_LITCH_BUTTON_PRESS_EVENT) ==
              VIRTUAL_LITCH_OUT_OF_RANGE,
          "C3 rejects a physically unavailable P2 control event");

    virtual_litch_destroy(a);
    virtual_litch_destroy(b);
}

void TestPowerBudgetCapture() {
    virtual_litch_config config = ConfigFor(
        VIRTUAL_LITCH_HARDWARE_S3);
    config.max_led_current_ma = 1U;
    virtual_litch* board = CreateBoard(config);
    virtual_litch_physical_frame physical{};
    Check(virtual_litch_get_physical_frame(board, &physical) ==
              VIRTUAL_LITCH_OK,
          "post-budget frame remains queryable under a tight limit");
    Check(physical.power_limited != 0U,
          "physical frame reports production current limiting");
    Check(physical.applied_led_current_ma <= 1U,
          "post-budget physical current respects the configured ceiling");
    virtual_litch_destroy(board);
}

void CheckNewGameKingCue(const virtual_litch_config& config,
                         const char* hardwareName,
                         const bool expectPowerLimited) {
    virtual_litch* board = CreateBoard(config);
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint64_t INITIAL_WHITE_OCCUPANCY = 0xFFFF000000000000ULL;
    constexpr uint64_t INITIAL_BLACK_OCCUPANCY = 0x000000000000FFFFULL;
    constexpr uint64_t INITIAL_KING_MASK = (1ULL << 4U) | (1ULL << 60U);
    constexpr uint64_t INITIAL_ROOK_MASK = (1ULL << 0U) | (1ULL << 7U) |
                                           (1ULL << 56U) | (1ULL << 63U);
    constexpr uint64_t INITIAL_PAWN_MASK =
        (0xFFULL << 8U) | (0xFFULL << 48U);
    virtual_litch_position opening{};
    virtual_litch_default_position(&opening);
    opening.occupied_mask = INITIAL_OCCUPANCY;
    opening.king_mask = INITIAL_KING_MASK;
    opening.rook_mask = INITIAL_ROOK_MASK;
    opening.pawn_mask = INITIAL_PAWN_MASK;
    opening.mover_occupied_mask = INITIAL_WHITE_OCCUPANCY;
    opening.opponent_occupied_mask = INITIAL_BLACK_OCCUPANCY;
    opening.side_to_move_valid = 1U;
    Check(virtual_litch_dispatch_position(board, &opening) ==
              VIRTUAL_LITCH_OK,
          "VirtualLitch opening position seeds New Game observation ownership");
    Check(virtual_litch_dispatch_board_snapshot(board, INITIAL_OCCUPANCY) ==
              VIRTUAL_LITCH_OK,
          "VirtualLitch opening observation is present before New Game");

    virtual_litch_action_request action{};
    virtual_litch_default_action_request(&action);
    action.request_id = 901U;
    action.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    virtual_litch_control_outcome outcome{};
    Check(virtual_litch_dispatch_control_action(board, &action, &outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == 0U,
          "VirtualLitch New Game is accepted before king-cue capture");
    Check(virtual_litch_advance(board, 1U) == VIRTUAL_LITCH_OK &&
              virtual_litch_take_control_completion(board, &outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == 1U,
          "VirtualLitch New Game completes before king-cue capture");

    virtual_litch_semantic_frame semantic{};
    virtual_litch_physical_frame physical{};
    Check(virtual_litch_get_semantic_frame(board, &semantic) ==
              VIRTUAL_LITCH_OK,
          "New Game semantic king cue is queryable");
    Check(virtual_litch_get_physical_frame(board, &physical) ==
              VIRTUAL_LITCH_OK,
          "New Game post-budget king cue is queryable");
    virtual_litch_state state{};
    Check(virtual_litch_get_state(board, &state) == VIRTUAL_LITCH_OK &&
              state.physical_occupied_mask == INITIAL_OCCUPANCY,
          "New Game preserves the latest physical board observation");

    constexpr uint8_t BLACK_KING = 4U;
    constexpr uint8_t BLACK_QUEEN = 3U;
    constexpr uint8_t WHITE_QUEEN = 59U;
    constexpr uint8_t WHITE_KING = 60U;
    const auto checkSquare = [&](const uint8_t king,
                                 const uint8_t neighbor,
                                 const char* color,
                                 const char* phase,
                                 const bool flashOn) {
        const virtual_litch_color semanticSide0 =
            semantic.board[king][static_cast<uint8_t>(
                hardware::SquareLedSlot::Square0)];
        const virtual_litch_color semanticSide2 =
            semantic.board[king][static_cast<uint8_t>(
                hardware::SquareLedSlot::Square2)];
        const uint16_t semanticBracket = ColorEnergy(semanticSide0);
        const uint16_t semanticPiece =
            ColorEnergy(semantic.board[king][static_cast<uint8_t>(
                hardware::SquareLedSlot::Piece)]);
        const virtual_litch_color semanticNeighbor =
            semantic.board[neighbor][static_cast<uint8_t>(
                hardware::SquareLedSlot::Square0)];
        Check(semanticBracket != 0U &&
                  SameColor(semanticSide0, semanticSide2),
              "New Game king identity uses two matching semantic side LEDs");
        Check(flashOn ? semanticBracket > semanticPiece
                      : semanticBracket < semanticPiece,
              flashOn ? "New Game flash bracket exceeds the steady detected center"
                      : "New Game lower rest bracket sits below the steady detected center");
        Check(!SameColor(semanticSide0, semanticNeighbor),
              "New Game semantic king pair remains distinct from its neighbor");

        const uint16_t bracketIndex = PhysicalSquareLedIndex(
            config, king, hardware::SquareLedSlot::Square0);
        const uint16_t pieceIndex = PhysicalSquareLedIndex(
            config, king, hardware::SquareLedSlot::Piece);
        const uint16_t secondBracketIndex = PhysicalSquareLedIndex(
            config, king, hardware::SquareLedSlot::Square2);
        const uint16_t neighborIndex = PhysicalSquareLedIndex(
            config, neighbor, hardware::SquareLedSlot::Square0);
        Check(bracketIndex < physical.led_count &&
                  pieceIndex < physical.led_count &&
                  secondBracketIndex < physical.led_count &&
                  neighborIndex < physical.led_count,
              "New Game king-cue targets fit the physical topology");
        const virtual_litch_color bracket = physical.pixels[bracketIndex];
        const virtual_litch_color secondBracket =
            physical.pixels[secondBracketIndex];
        const virtual_litch_color piece = physical.pixels[pieceIndex];
        const virtual_litch_color neighborPixel =
            physical.pixels[neighborIndex];
        Check(ColorEnergy(bracket) != 0U && SameColor(bracket, secondBracket),
              "post-budget New Game cue keeps two persistent matching side LEDs");
        if (!expectPowerLimited || flashOn) {
            Check(!SameColor(bracket, piece),
                  "post-budget New Game pair differs from its center code");
            Check(!SameColor(bracket, neighborPixel),
                  "post-budget New Game pair differs from its neighbor code");
        }
        std::printf("[KING_CUE] hardware=%s phase=%s color=%s bracket=%u piece=%u neighbor=%u limited=%u current=%u/%u\n",
                    hardwareName,
                    phase,
                    color,
                    static_cast<unsigned>(ColorEnergy(bracket)),
                    static_cast<unsigned>(ColorEnergy(piece)),
                    static_cast<unsigned>(ColorEnergy(neighborPixel)),
                    static_cast<unsigned>(physical.power_limited),
                    static_cast<unsigned>(physical.applied_led_current_ma),
                    static_cast<unsigned>(physical.estimated_led_current_ma));
    };
    Check((physical.power_limited != 0U) == expectPowerLimited,
          "New Game characterization reaches the requested power-limit state");
    checkSquare(WHITE_KING, WHITE_QUEEN, "white", "full", true);
    checkSquare(BLACK_KING, BLACK_QUEEN, "black", "full", true);
    const std::array<virtual_litch_color, 2> fullPairs{
        semantic.board[WHITE_KING][static_cast<uint8_t>(
            hardware::SquareLedSlot::Square0)],
        semantic.board[BLACK_KING][static_cast<uint8_t>(
            hardware::SquareLedSlot::Square0)]};
    const std::array<virtual_litch_color, 2> steadyCenters{
        semantic.board[WHITE_KING][static_cast<uint8_t>(
            hardware::SquareLedSlot::Piece)],
        semantic.board[BLACK_KING][static_cast<uint8_t>(
            hardware::SquareLedSlot::Piece)]};

    Check(virtual_litch_advance(board, 1000U) == VIRTUAL_LITCH_OK,
          "New Game king cue advances to the 1000 ms rest phase");
    Check(virtual_litch_get_semantic_frame(board, &semantic) ==
                  VIRTUAL_LITCH_OK &&
              virtual_litch_get_physical_frame(board, &physical) ==
                  VIRTUAL_LITCH_OK,
          "minimum-phase New Game cue remains queryable");
    Check((physical.power_limited != 0U) == expectPowerLimited,
          "rest-phase cue retains the requested power-limit state");
    checkSquare(WHITE_KING, WHITE_QUEEN, "white", "rest", false);
    checkSquare(BLACK_KING, BLACK_QUEEN, "black", "rest", false);
    Check(!SameColor(
              semantic.board[WHITE_KING][static_cast<uint8_t>(
                  hardware::SquareLedSlot::Square0)],
              fullPairs[0]) &&
              !SameColor(
                  semantic.board[BLACK_KING][static_cast<uint8_t>(
                      hardware::SquareLedSlot::Square0)],
                  fullPairs[1]) &&
              SameColor(
                  semantic.board[WHITE_KING][static_cast<uint8_t>(
                      hardware::SquareLedSlot::Piece)],
                  steadyCenters[0]) &&
              SameColor(
                  semantic.board[BLACK_KING][static_cast<uint8_t>(
                      hardware::SquareLedSlot::Piece)],
                  steadyCenters[1]),
          "rest changes only the semantic pair, not either king center");

    Check(virtual_litch_advance(board, 600U) == VIRTUAL_LITCH_OK &&
              virtual_litch_get_semantic_frame(board, &semantic) ==
                  VIRTUAL_LITCH_OK &&
              virtual_litch_get_physical_frame(board, &physical) ==
                  VIRTUAL_LITCH_OK,
          "New Game king cue reaches the exact 1600 ms repeat boundary");
    Check(SameColor(
              semantic.board[WHITE_KING][static_cast<uint8_t>(
                  hardware::SquareLedSlot::Square0)],
              fullPairs[0]) &&
              SameColor(
                  semantic.board[BLACK_KING][static_cast<uint8_t>(
                      hardware::SquareLedSlot::Square2)],
                  fullPairs[1]) &&
              SameColor(
                  semantic.board[WHITE_KING][static_cast<uint8_t>(
                      hardware::SquareLedSlot::Piece)],
                  steadyCenters[0]) &&
              SameColor(
                  semantic.board[BLACK_KING][static_cast<uint8_t>(
                      hardware::SquareLedSlot::Piece)],
                  steadyCenters[1]),
          "1600 ms repeats both full pairs while both centers remain steady");
    Check((physical.power_limited != 0U) == expectPowerLimited,
          "repeat phase retains the requested power-limit state");
    checkSquare(WHITE_KING, WHITE_QUEEN, "white", "repeat", true);
    checkSquare(BLACK_KING, BLACK_QUEEN, "black", "repeat", true);
    virtual_litch_destroy(board);
}

void TestNewGameKingCueAtRestrainedBrightness() {
    virtual_litch_config c3 = ConfigFor(VIRTUAL_LITCH_HARDWARE_C3);
    c3.brightness_scale_256 = 12U;
    CheckNewGameKingCue(c3, "c3", false);

    virtual_litch_config c3Limited = c3;
    c3Limited.max_led_current_ma = 5U;
    CheckNewGameKingCue(c3Limited, "c3-limited", true);

    virtual_litch_config s3 = ConfigFor(VIRTUAL_LITCH_HARDWARE_S3);
    s3.brightness_scale_256 = 12U;
    s3.board_rotate = 1U;
    s3.square_rotate_for_output = 1U;
    s3.first_player_is_p2 = 1U;
    CheckNewGameKingCue(s3, "s3-rotated-p2", false);

    virtual_litch_config s3Limited = s3;
    s3Limited.max_led_current_ma = 9U;
    CheckNewGameKingCue(s3Limited, "s3-rotated-p2-limited", true);
}

void TestSetupUnavailableUndoUsesPerimeterCue() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t E2 = 52U;
    test_support::BoundedHintEngine journeyEngine;
    JourneySession session(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3),
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
        nullptr, &journeyEngine);
    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "setup unavailable-undo journey initializes");

    virtual_litch_action_request request{};
    virtual_litch_default_action_request(&request);
    request.request_id = 699U;
    request.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    request.owner = VIRTUAL_LITCH_OWNER_P1;
    modules::ControlMutationOutcome outcome;
    Check(session.DispatchControlAction(request, outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "setup unavailable-undo journey starts a fresh game");

    const uint64_t openingWithoutE2 =
        INITIAL_OCCUPANCY & ~(1ULL << E2);
    Check(DispatchJourneySnapshot(session, openingWithoutE2) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false),
          "31-piece setup begins with the P1 Turn indicator");

    Check(TapP1X(session) &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "first setup X dismisses the king assist");
    const virtual_litch_semantic_frame baseline =
        session.SemanticFrame();

    Check(TapP1X(session) &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason == modules::ControlMutationReason::Busy,
          "second setup X reports unavailable Undo without inventing history");
    Check(SameBoardPixels(session.SemanticFrame(), baseline) &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              session.SemanticFrame().status_hash != baseline.status_hash,
          "unavailable setup Undo adds a subtle perimeter cue without board text");

    Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
              SameBoardPixels(session.SemanticFrame(), baseline) &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false),
          "unavailable Undo never renders a clipped BUS board message");
    Check(session.Advance(600U) == VIRTUAL_LITCH_OK &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false),
          "unavailable Undo cue expires back into the Turn indicator");
}

void TestSecondPhysicalUndoCancelsPendingCorrectionOnPress() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;
    const uint64_t e2e4Occupancy =
        (INITIAL_OCCUPANCY & ~(UINT64_C(1) << E2)) |
        (UINT64_C(1) << E4);
    test_support::BoundedHintEngine journeyEngine;
    JourneySession session(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3),
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
        nullptr, &journeyEngine);
    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "physical cancel-Undo journey initializes");

    virtual_litch_action_request newGame{};
    virtual_litch_default_action_request(&newGame);
    newGame.request_id = 710U;
    newGame.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    newGame.owner = VIRTUAL_LITCH_OWNER_P1;
    modules::ControlMutationOutcome outcome;
    Check(session.DispatchControlAction(newGame, outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "physical cancel-Undo journey starts a fresh game");
    Check(DispatchJourneySnapshot(session, INITIAL_OCCUPANCY) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.Advance(1801U) == VIRTUAL_LITCH_OK,
          "physical cancel-Undo journey reaches Play Ready");
    Check(session.DispatchSensorDelta(E2, false) == VIRTUAL_LITCH_OK &&
              session.DispatchSensorDelta(E4, true) == VIRTUAL_LITCH_OK &&
              session.Advance(2000U) == VIRTUAL_LITCH_OK &&
              session.State().physical_occupied_mask == e2e4Occupancy &&
              session.State().expected_occupied_mask == e2e4Occupancy &&
              session.State().side_to_move_black != 0U,
          "physical cancel-Undo journey commits e2-e4");

    const bool firstUndoTapped = TapP1X(session);
    const bool firstUndoOutcome =
        session.TryGetLastBoardControlOutcome(outcome);
    Check(firstUndoTapped && firstUndoOutcome &&
              outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.actionState ==
                  modules::ControlActionState::UndoPending &&
              session.State().physical_occupied_mask == e2e4Occupancy &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::AwaitingAction) &&
              !StatusIsFullRedFault(session.SemanticFrame()),
          "first physical X applies canonical Undo and opens calm inverse correction on release");

    Check(session.DispatchButton(VIRTUAL_LITCH_OWNER_P1,
                                 VIRTUAL_LITCH_BUTTON_P1_X,
                                 VIRTUAL_LITCH_BUTTON_PRESS_EVENT) ==
                  VIRTUAL_LITCH_OK &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.reason == modules::ControlMutationReason::None &&
              outcome.actionState == modules::ControlActionState::Idle &&
              session.State().physical_occupied_mask == e2e4Occupancy &&
              session.State().expected_occupied_mask == e2e4Occupancy &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().side_to_move_black != 0U &&
              StatusIsTurnIndicatorForOwner(session.SemanticFrame(), true) &&
              !StatusIsFullRedFault(session.SemanticFrame()),
          "second physical X applies Cancel Undo on press without release delay and restores calm play");
    const uint16_t cancelRequestId = outcome.requestId;
    Check(session.DispatchButton(VIRTUAL_LITCH_OWNER_P1,
                                 VIRTUAL_LITCH_BUTTON_P1_X,
                                 VIRTUAL_LITCH_BUTTON_RELEASE_EVENT) ==
                  VIRTUAL_LITCH_OK &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.requestId == cancelRequestId,
          "release after press-owned Cancel Undo is consumed without a duplicate action");

    std::printf(
        "[UNDO_CANCEL_JOURNEY] input=e2-e4; X tap; X press | "
        "authoritative=UndoPending->Idle | outcome=applied/none | "
        "board=physical/expected:%016llx status=P2-turn-indicator | "
        "cancel_latency=same-input-tick release=consumed\n",
        static_cast<unsigned long long>(e2e4Occupancy));
}

void TestProductionSetupJourneyPlayback() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint64_t OBSERVED_PRIOR_GAME_OCCUPANCY =
        0x8386040308882780ULL;
    constexpr uint8_t OBSERVED_EXTRA = 42U;
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E3 = 44U;
    constexpr uint8_t E4 = 36U;
    constexpr uint8_t BLACK_KING = 4U;
    constexpr uint8_t WHITE_KING = 60U;
    constexpr uint8_t BLACK_KING_BACKGROUND_REFERENCE = 0U;
    constexpr uint8_t WHITE_KING_BACKGROUND_REFERENCE = 62U;
    constexpr uint8_t MALFORMED_EXTRA_A = 35U;
    constexpr uint8_t MALFORMED_EXTRA_B = 42U;
    constexpr std::array<uint8_t, 4> EXTRAS{18U, 20U, 27U, 29U};
    constexpr uint64_t FOUR_EXTRA_MASK =
        (1ULL << EXTRAS[0]) | (1ULL << EXTRAS[1]) |
        (1ULL << EXTRAS[2]) | (1ULL << EXTRAS[3]);
    constexpr uint64_t LEGAL_E2_DESTINATIONS =
        (1ULL << E3) | (1ULL << E4);

    const virtual_litch_config journeyConfig =
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3);
    test_support::BoundedHintEngine journeyEngine;
    JourneySession session(
        journeyConfig,
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
        nullptr, &journeyEngine);
    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "private production-engine journey initializes");

    const auto applyNewGame = [&](const uint16_t requestId) {
        virtual_litch_action_request request{};
        virtual_litch_default_action_request(&request);
        request.request_id = requestId;
        request.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
        request.owner = VIRTUAL_LITCH_OWNER_P1;
        modules::ControlMutationOutcome outcome;
        return session.DispatchControlAction(request, outcome) ==
                   VIRTUAL_LITCH_OK &&
               outcome.status == modules::ControlMutationStatus::Accepted &&
               session.Advance(1U) == VIRTUAL_LITCH_OK &&
               session.TryTakeControlCompletion(outcome) &&
               outcome.status == modules::ControlMutationStatus::Applied;
    };
    const auto readyFor = [&](const bool p2) {
        return session.State().interaction_phase == static_cast<uint8_t>(
                                                        runtime::ChessInteractionPhase::Idle) &&
               session.State().correction_mask == 0ULL &&
               StatusIsTurnIndicatorForOwner(
                   session.SemanticFrame(), p2) &&
               DedicatedStatusTopologyMatches(session);
    };
    const auto setupCompleteFor = [&] {
        return session.State().interaction_phase == static_cast<uint8_t>(
                                                        runtime::ChessInteractionPhase::Idle) &&
               session.State().correction_mask == 0ULL &&
               !session.BoardOverlayActive() &&
               StatusHasSetupCompletionSweep(session.SemanticFrame()) &&
               DedicatedStatusTopologyMatches(session);
    };
    const auto retainedDuckAndLaterExtrasUseRainbow = [&](
                                                          const auto& frame, const uint8_t extraCount) {
        if (!IsExactYellow(BoardLed(
                frame, EXTRAS[0], hardware::SquareLedSlot::Piece)) ||
            !SameColor(
                BoardLed(frame, EXTRAS[0],
                         hardware::SquareLedSlot::Square0),
                BoardLed(frame, EXTRAS[0],
                         hardware::SquareLedSlot::Square2))) {
            return false;
        }
        for (uint8_t index = 1U; index < extraCount; ++index) {
            const uint8_t square = EXTRAS[index];
            const virtual_litch_color square0 = BoardLed(
                frame, square, hardware::SquareLedSlot::Square0);
            const virtual_litch_color piece = BoardLed(
                frame, square, hardware::SquareLedSlot::Piece);
            const virtual_litch_color square2 = BoardLed(
                frame, square, hardware::SquareLedSlot::Square2);
            if (SameColor(square0, piece) || SameColor(piece, square2) ||
                SameColor(square0, square2)) {
                return false;
            }
        }
        return true;
    };

    Check(DispatchJourneySnapshot(session, OBSERVED_PRIOR_GAME_OCCUPANCY) ==
              VIRTUAL_LITCH_OK,
          "prior-game occupancy reaches the production board-observation seam");
    Check(applyNewGame(700U),
          "New Game completes through the real coordinator");
    Check(session.Advance(modules::CORRECTION_STRONG_MS) == VIRTUAL_LITCH_OK,
          "persistent New Game leftovers reach strong correction timing");
    Check(session.State().physical_occupied_mask ==
                  OBSERVED_PRIOR_GAME_OCCUPANCY &&
              session.State().expected_occupied_mask == INITIAL_OCCUPANCY,
          "New Game preserves the observed board while resetting its target");
    Check(StatusIsFullRedFault(session.SemanticFrame()),
          "New Game leftover removal keeps the established correction ring");
    Check(ColorEnergy(BoardLed(
              session.SemanticFrame(), E2,
              hardware::SquareLedSlot::Piece)) == 0U &&
              ColorEnergy(BoardLed(session.SemanticFrame(), E2,
                                   hardware::SquareLedSlot::Square0)) > 0U &&
              SameColor(BoardLed(session.SemanticFrame(), E2,
                                 hardware::SquareLedSlot::Square0),
                        BoardLed(session.SemanticFrame(), E2,
                                 hardware::SquareLedSlot::Square2)),
          "New Game keeps missing home-square guidance and black empty centers while leftovers remain");
    Check(!SameColor(
              BoardLed(session.SemanticFrame(), OBSERVED_EXTRA,
                       hardware::SquareLedSlot::Square0),
              BoardLed(session.SemanticFrame(), OBSERVED_EXTRA,
                       hardware::SquareLedSlot::Square2)),
          "New Game guides each leftover piece for removal");
    PrintJourneyCheckpoint(
        session, "New Game leftover-piece stage",
        "observe prior-game occupancy; request New Game",
        "return leftovers to their colored home squares; leave e2 missing");
    const uint64_t openingWithoutE2 =
        INITIAL_OCCUPANCY & ~(1ULL << E2);
    Check(DispatchJourneySnapshot(session, openingWithoutE2) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.State().physical_occupied_mask == openingWithoutE2 &&
              session.State().expected_occupied_mask == INITIAL_OCCUPANCY &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              DedicatedStatusTopologyMatches(session),
          "stable 31-piece setup keeps the P1 Turn indicator");
    const virtual_litch_semantic_frame arrangingSemantic =
        session.SemanticFrame();
    const virtual_litch_physical_frame arrangingPhysical =
        session.PhysicalFrame();
    PrintJourneyCheckpoint(session, "31-piece setup frame A",
                           "New Game; e2 missing; wait 200 ms",
                           "advance one turn-animation frame");

    Check(session.Advance(80U) == VIRTUAL_LITCH_OK &&
              session.State().physical_occupied_mask == openingWithoutE2 &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              arrangingSemantic.status_hash !=
                  session.SemanticFrame().status_hash &&
              !SameDedicatedPhysicalStatusPixels(
                  arrangingPhysical, session.PhysicalFrame()) &&
              DedicatedStatusTopologyMatches(session),
          "31-piece P1 Turn indicator animates through semantic and physical output");
    PrintJourneyCheckpoint(session, "31-piece setup frame B",
                           "advance 80 ms", "place e2");

    Check(session.DispatchSensorDelta(E2, true) == VIRTUAL_LITCH_OK &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              DedicatedStatusTopologyMatches(session),
          "exact opening begins its stability handoff with the P1 Turn indicator");
    const virtual_litch_semantic_frame handoffSemantic =
        session.SemanticFrame();
    const virtual_litch_physical_frame handoffPhysical =
        session.PhysicalFrame();
    PrintJourneyCheckpoint(session, "exact-board handoff frame A",
                           "place e2; wait 0 ms",
                           "advance one turn-animation frame");
    Check(session.Advance(80U) == VIRTUAL_LITCH_OK &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              handoffSemantic.status_hash !=
                  session.SemanticFrame().status_hash &&
              !SameDedicatedPhysicalStatusPixels(
                  handoffPhysical, session.PhysicalFrame()) &&
              DedicatedStatusTopologyMatches(session),
          "P1 Turn indicator animates during the exact-board stability handoff");
    PrintJourneyCheckpoint(session, "exact-board handoff frame B",
                           "advance 80 ms", "advance to 199 ms");
    Check(session.Advance(119U) == VIRTUAL_LITCH_OK &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              DedicatedStatusTopologyMatches(session),
          "exact opening keeps the P1 Turn indicator through 199 ms");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK && setupCompleteFor() &&
              LitStatusPixelCount(session.SemanticFrame()) == 4U &&
              LitDedicatedPhysicalStatusPixelCount(session) == 4U &&
              !StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              session.State().physical_occupied_mask == INITIAL_OCCUPANCY &&
              session.State().expected_occupied_mask == INITIAL_OCCUPANCY,
          "exact opening gives setup acknowledgement sole perimeter ownership at 200 ms");
    const virtual_litch_semantic_frame firstReadySemantic =
        session.SemanticFrame();
    const virtual_litch_physical_frame firstReadyPhysical =
        session.PhysicalFrame();
    PrintJourneyCheckpoint(session, "opening Play Ready",
                           "place e2; wait 200 ms", "add four extras");
    Check(session.Advance(80U) == VIRTUAL_LITCH_OK && setupCompleteFor() &&
              firstReadySemantic.status_hash !=
                  session.SemanticFrame().status_hash &&
              !SameDedicatedPhysicalStatusPixels(
                  firstReadyPhysical, session.PhysicalFrame()),
          "setup completion expands symmetrically through semantic and physical perimeter output");
    Check(session.Advance(820U) == VIRTUAL_LITCH_OK &&
              LitStatusPixelCount(session.SemanticFrame()) ==
                  VIRTUAL_LITCH_STATUS_LED_COUNT &&
              LitDedicatedPhysicalStatusPixelCount(session) ==
                  VIRTUAL_LITCH_STATUS_LED_COUNT,
          "setup completion meets across the full semantic and physical perimeter");
    Check(session.Advance(660U) == VIRTUAL_LITCH_OK &&
              setupCompleteFor(),
          "setup completion returns toward both players before its handoff");
    Check(session.Advance(120U) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().correction_mask == 0ULL &&
              DedicatedStatusTopologyMatches(session),
          "setup completion blends into the P1 Turn indicator during its final 240 ms");
    Check(session.Advance(119U) == VIRTUAL_LITCH_OK &&
              DedicatedStatusTopologyMatches(session),
          "setup completion reaches its final blended frame at 1799 ms");
    const virtual_litch_semantic_frame finalHandoffSemantic =
        session.SemanticFrame();
    const virtual_litch_physical_frame finalHandoffPhysical =
        session.PhysicalFrame();
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK && readyFor(false) &&
              StatusFrameDistance(finalHandoffSemantic,
                                  session.SemanticFrame()) <= 128U &&
              DedicatedPhysicalStatusDistance(finalHandoffPhysical,
                                              session.PhysicalFrame()) <=
                  128U,
          "Play Ready hands off smoothly to Turn at 1800 ms without a dark or ownership gap");

    const uint64_t malformedExtraMask =
        (1ULL << MALFORMED_EXTRA_A) | (1ULL << MALFORMED_EXTRA_B);
    Check(session.DispatchSensorDelta(MALFORMED_EXTRA_A, true) ==
                  VIRTUAL_LITCH_OK &&
              session.SetupDuckCandidateSquare() == MALFORMED_EXTRA_A,
          "one rank-4 setup extra is provisionally Vex");
    Check(session.DispatchSensorDelta(MALFORMED_EXTRA_B, true) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(modules::CORRECTION_STRONG_MS) ==
                  VIRTUAL_LITCH_OK &&
              (session.State().physical_occupied_mask ^
               session.State().expected_occupied_mask) ==
                  malformedExtraMask &&
              session.SetupDuckCandidateSquare() == MALFORMED_EXTRA_A &&
              session.State().human_illegal_mask ==
                  (1ULL << MALFORMED_EXTRA_B) &&
              StatusIsFullRedFault(session.SemanticFrame()) &&
              IsExactYellow(BoardLed(
                  session.SemanticFrame(), MALFORMED_EXTRA_A,
                  hardware::SquareLedSlot::Piece)) &&
              !IsExactYellow(BoardLed(
                  session.SemanticFrame(), MALFORMED_EXTRA_B,
                  hardware::SquareLedSlot::Piece)),
          "second opening extra uses rainbow/red correction while retaining provisional Vex");
    PrintJourneyCheckpoint(session, "malformed setup extras",
                           "add d4 and c3 extras",
                           "remove both extras; add c6 Vex");
    Check(session.DispatchSensorDelta(MALFORMED_EXTRA_B, false) ==
                  VIRTUAL_LITCH_OK &&
              session.DispatchSensorDelta(MALFORMED_EXTRA_A, false) ==
                  VIRTUAL_LITCH_OK &&
              session.State().physical_occupied_mask == INITIAL_OCCUPANCY,
          "removing malformed setup extras recovers the exact opening");

    const uint64_t firstExtraMask = 1ULL << EXTRAS[0];
    Check(session.DispatchSensorDelta(EXTRAS[0], true) ==
                  VIRTUAL_LITCH_OK &&
              (session.State().physical_occupied_mask ^
               session.State().expected_occupied_mask) == firstExtraMask &&
              session.State().correction_mask == 0ULL &&
              IsExactYellow(BoardLed(
                  session.SemanticFrame(), EXTRAS[0],
                  hardware::SquareLedSlot::Piece)) &&
              CountExactYellowPieceCenters(session.SemanticFrame()) == 1U &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              DedicatedStatusTopologyMatches(session),
          "first eligible rank-6 extra is the sole yellow Duck cue without blanking the Turn indicator");
    const virtual_litch_semantic_frame duckValidationSemantic =
        session.SemanticFrame();
    const virtual_litch_physical_frame duckValidationPhysical =
        session.PhysicalFrame();
    PrintJourneyCheckpoint(session, "Duck validation frame A",
                           "add eligible c6 candidate; wait 0 ms",
                           "advance one turn-animation frame");
    Check(session.Advance(80U) == VIRTUAL_LITCH_OK &&
              (session.State().physical_occupied_mask ^
               session.State().expected_occupied_mask) == firstExtraMask &&
              IsExactYellow(BoardLed(
                  session.SemanticFrame(), EXTRAS[0],
                  hardware::SquareLedSlot::Piece)) &&
              CountExactYellowPieceCenters(session.SemanticFrame()) == 1U &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              duckValidationSemantic.status_hash !=
                  session.SemanticFrame().status_hash &&
              !SameDedicatedPhysicalStatusPixels(
                  duckValidationPhysical, session.PhysicalFrame()) &&
              DedicatedStatusTopologyMatches(session),
          "Duck validation keeps calm board semantics while the P1 Turn indicator animates");
    PrintJourneyCheckpoint(session, "Duck validation frame B",
                           "advance 80 ms", "add second extra");

    const uint64_t secondExtraMask =
        firstExtraMask | (1ULL << EXTRAS[1]);
    const bool secondExtraApplied =
        session.DispatchSensorDelta(EXTRAS[1], true) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(modules::CORRECTION_STRONG_MS) == VIRTUAL_LITCH_OK;
    Check(secondExtraApplied &&
              (session.State().physical_occupied_mask ^
               session.State().expected_occupied_mask) == secondExtraMask &&
              StatusIsFullRedFault(session.SemanticFrame()) &&
              LitStatusPixelCount(session.SemanticFrame()) ==
                  VIRTUAL_LITCH_STATUS_LED_COUNT &&
              LitDedicatedPhysicalStatusPixelCount(session) ==
                  VIRTUAL_LITCH_STATUS_LED_COUNT &&
              DedicatedStatusTopologyMatches(session) &&
              retainedDuckAndLaterExtrasUseRainbow(
                  session.SemanticFrame(), 2U) &&
              CountExactYellowPieceCenters(session.SemanticFrame()) == 1U,
          "second eligible extra leaves the retained Duck calm and applies bounded correction only to itself");

    uint64_t accumulatedExtraMask = secondExtraMask;
    for (uint8_t index = 2U; index < EXTRAS.size(); ++index) {
        accumulatedExtraMask |= 1ULL << EXTRAS[index];
        const bool extraApplied =
            session.DispatchSensorDelta(EXTRAS[index], true) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(modules::CORRECTION_STRONG_MS) == VIRTUAL_LITCH_OK;
        Check(extraApplied &&
                  (session.State().physical_occupied_mask ^
                   session.State().expected_occupied_mask) ==
                      accumulatedExtraMask &&
                  StatusIsFullRedFault(session.SemanticFrame()) &&
                  LitStatusPixelCount(session.SemanticFrame()) ==
                      VIRTUAL_LITCH_STATUS_LED_COUNT &&
                  LitDedicatedPhysicalStatusPixelCount(session) ==
                      VIRTUAL_LITCH_STATUS_LED_COUNT &&
                  DedicatedStatusTopologyMatches(session) &&
                  retainedDuckAndLaterExtrasUseRainbow(
                      session.SemanticFrame(),
                      static_cast<uint8_t>(index + 1U)) &&
                  CountExactYellowPieceCenters(session.SemanticFrame()) == 1U,
              index == 2U
                  ? "third extra remains bounded correction beside the retained Duck"
                  : "fourth extra remains bounded correction beside the retained Duck");
    }
    const virtual_litch_semantic_frame hardCorrection =
        session.SemanticFrame();
    Check((session.State().physical_occupied_mask ^
           session.State().expected_occupied_mask) == FOUR_EXTRA_MASK &&
              StatusIsFullRedFault(hardCorrection) &&
              DedicatedStatusTopologyMatches(session) &&
              retainedDuckAndLaterExtrasUseRainbow(hardCorrection, 4U),
          "four extras retain one Duck and render only later extras as rainbow correction");
    const virtual_litch_color kingBracket0 = BoardLed(
        hardCorrection, WHITE_KING, hardware::SquareLedSlot::Square0);
    const virtual_litch_color kingBracket2 = BoardLed(
        hardCorrection, WHITE_KING, hardware::SquareLedSlot::Square2);
    Check(ColorEnergy(kingBracket0) == 0U &&
              ColorEnergy(kingBracket2) == 0U,
          "required leftover removal suppresses the optional king assist");
    PrintJourneyCheckpoint(session, "four-extra correction",
                           "add c6, e6, d5, f5",
                           "disable setup assistance");

    Check(MutateJourneySetupAssistance(session, 702U, false),
          "private journey disables setup assistance through production mutation");
    const virtual_litch_semantic_frame assistanceOff =
        session.SemanticFrame();
    Check((session.State().physical_occupied_mask ^
           session.State().expected_occupied_mask) == FOUR_EXTRA_MASK &&
              StatusIsFullRedFault(assistanceOff) &&
              DedicatedStatusTopologyMatches(session) &&
              retainedDuckAndLaterExtrasUseRainbow(assistanceOff, 4U) &&
              ColorEnergy(BoardLed(
                  assistanceOff, WHITE_KING,
                  hardware::SquareLedSlot::Square0)) == 0U &&
              ColorEnergy(BoardLed(
                  assistanceOff, WHITE_KING,
                  hardware::SquareLedSlot::Square2)) == 0U &&
              ColorEnergy(BoardLed(
                  assistanceOff, WHITE_KING,
                  hardware::SquareLedSlot::Piece)) != 0U,
          "setting off leaves the required leftover-removal stage unchanged");
    PrintJourneyCheckpoint(session, "assistance off in correction",
                           "set BOARD_SETUP_ASSISTANCE=false",
                           "tap P1 X while disabled");

    Check(TapP1X(session), "setting-off X uses physical button ingress");
    modules::ControlMutationOutcome settingOffX;
    Check(session.TryGetLastBoardControlOutcome(settingOffX) &&
              settingOffX.status == modules::ControlMutationStatus::Rejected &&
              settingOffX.reason == modules::ControlMutationReason::NoHistory,
          "setting-off X reaches correction-owned semantic Undo, not assist dismissal");
    PrintJourneyCheckpoint(session, "disabled X rejected",
                           "tap P1 X while setting is off",
                           "let NO expire; enable assistance");

    Check(session.Advance(2240U) == VIRTUAL_LITCH_OK &&
              MutateJourneySetupAssistance(session, 703U, true),
          "private journey reenables assistance after BUS expiry");
    const virtual_litch_semantic_frame assistanceRestored =
        session.SemanticFrame();
    Check((session.State().physical_occupied_mask ^
           session.State().expected_occupied_mask) == FOUR_EXTRA_MASK &&
              StatusIsFullRedFault(assistanceRestored) &&
              DedicatedStatusTopologyMatches(session) &&
              retainedDuckAndLaterExtrasUseRainbow(assistanceRestored, 4U) &&
              ColorEnergy(BoardLed(
                  assistanceRestored, WHITE_KING,
                  hardware::SquareLedSlot::Square0)) == 0U &&
              ColorEnergy(BoardLed(
                  assistanceRestored, WHITE_KING,
                  hardware::SquareLedSlot::Square2)) == 0U,
          "reenabling keeps the optional pair hidden behind required removal");
    PrintJourneyCheckpoint(session, "assistance restored in correction",
                           "wait 2240 ms; set BOARD_SETUP_ASSISTANCE=true",
                           "restore opening");

    Check(DispatchJourneySnapshot(session, INITIAL_OCCUPANCY) ==
                  VIRTUAL_LITCH_OK &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              DedicatedStatusTopologyMatches(session) &&
              ColorEnergy(BoardLed(
                  session.SemanticFrame(), WHITE_KING,
                  hardware::SquareLedSlot::Square0)) != 0U &&
              SameColor(
                  BoardLed(session.SemanticFrame(), WHITE_KING,
                           hardware::SquareLedSlot::Square0),
                  BoardLed(session.SemanticFrame(), WHITE_KING,
                           hardware::SquareLedSlot::Square2)),
          "removing leftovers reveals the optional king assist and Turn indicator");
    PrintJourneyCheckpoint(session, "correction recovery with assist",
                           "restore exact opening; wait 0 ms",
                           "tap P1 X to dismiss the visible assist");

    Check(TapP1X(session), "visible-assist X uses physical button ingress");
    modules::ControlMutationOutcome firstX;
    const virtual_litch_semantic_frame afterDismissal =
        session.SemanticFrame();
    Check(session.TryGetLastBoardControlOutcome(firstX) &&
              firstX.status == modules::ControlMutationStatus::Applied &&
              firstX.reason == modules::ControlMutationReason::None &&
              session.State().physical_occupied_mask == INITIAL_OCCUPANCY &&
              StatusIsTurnIndicatorForOwner(afterDismissal, false) &&
              DedicatedStatusTopologyMatches(session) &&
              !session.BoardOverlayActive(),
          "X dismisses only the now-visible assist during setup handoff");
    Check(ColorEnergy(BoardLed(afterDismissal, WHITE_KING,
                               hardware::SquareLedSlot::Square0)) == 0U &&
              ColorEnergy(BoardLed(afterDismissal, WHITE_KING,
                                   hardware::SquareLedSlot::Square2)) == 0U &&
              ColorEnergy(BoardLed(afterDismissal, WHITE_KING,
                                   hardware::SquareLedSlot::Piece)) != 0U,
          "assist dismissal removes both dark-square brackets, not the king");
    PrintJourneyCheckpoint(session, "assist dismissed",
                           "tap P1 X while assist owns X",
                           "advance one turn-animation frame");

    const virtual_litch_semantic_frame recoverySemantic = afterDismissal;
    const virtual_litch_physical_frame recoveryPhysical =
        session.PhysicalFrame();
    PrintJourneyCheckpoint(session, "correction recovery frame A",
                           "restore exact opening; wait 0 ms",
                           "advance one turn-animation frame");
    Check(session.Advance(80U) == VIRTUAL_LITCH_OK &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              recoverySemantic.status_hash !=
                  session.SemanticFrame().status_hash &&
              !SameDedicatedPhysicalStatusPixels(
                  recoveryPhysical, session.PhysicalFrame()) &&
              DedicatedStatusTopologyMatches(session),
          "recovered P1 Turn indicator animates during the stability handoff");
    PrintJourneyCheckpoint(session, "correction recovery frame B",
                           "advance 80 ms", "advance to 199 ms");
    Check(session.Advance(119U) == VIRTUAL_LITCH_OK &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              DedicatedStatusTopologyMatches(session),
          "correction recovery keeps the P1 Turn indicator through 199 ms");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
              readyFor(false),
          "correction recovery reaches P1 Play Ready at 200 ms");
    const virtual_litch_semantic_frame readyBackground =
        session.SemanticFrame();

    Check(TapP1X(session), "second X uses physical button ingress");
    modules::ControlMutationOutcome secondX;
    Check(session.TryGetLastBoardControlOutcome(secondX) &&
              secondX.status == modules::ControlMutationStatus::Rejected &&
              secondX.reason == modules::ControlMutationReason::NoHistory,
          "second X returns a no-history rejection");
    const virtual_litch_semantic_frame unavailableCue =
        session.SemanticFrame();
    Check(SameBoardPixels(unavailableCue, readyBackground) &&
              StatusIsTurnIndicatorForOwner(unavailableCue, false) &&
              unavailableCue.status_hash != readyBackground.status_hash &&
              DedicatedStatusTopologyMatches(session),
          "no-history Undo overlays a sparse perimeter cue without board text");
    PrintJourneyCheckpoint(session, "Undo unavailable cue",
                           "tap P1 X", "advance to 479 ms");

    Check(session.Advance(479U) == VIRTUAL_LITCH_OK &&
              SameBoardPixels(session.SemanticFrame(), readyBackground) &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              DedicatedStatusTopologyMatches(session),
          "unavailable cue remains perimeter-only through 479 ms");
    PrintJourneyCheckpoint(session, "Undo unavailable at 479 ms",
                           "advance 479 ms", "advance final 1 ms");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
              SameBoardPixels(session.SemanticFrame(), readyBackground) &&
              readyFor(false),
          "unavailable cue expires into ordinary Play Ready at 480 ms");
    PrintJourneyCheckpoint(session, "Undo unavailable expired",
                           "advance final 1 ms", "rearm; lift e2");

    Check(applyNewGame(701U) && setupCompleteFor() &&
              session.Advance(300U) == VIRTUAL_LITCH_OK &&
              setupCompleteFor() &&
              session.DispatchSensorDelta(E2, false) ==
                  VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::PieceLifted) &&
              session.State().valid_destination_mask ==
                  LEGAL_E2_DESTINATIONS &&
              !StatusHasSetupCompletionSweep(session.SemanticFrame()) &&
              LitStatusPixelCount(session.SemanticFrame()) == 0U,
          "legal pickup interrupts the active setup acknowledgement immediately");
    Check(session.DispatchSensorDelta(E2, true) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              readyFor(false) &&
              applyNewGame(702U) &&
              setupCompleteFor() &&
              session.Advance(1801U) == VIRTUAL_LITCH_OK &&
              readyFor(false),
          "returning the piece restores Turn and New Game rearms a complete acknowledgement");
    const virtual_litch_semantic_frame rearmedReady =
        session.SemanticFrame();
    const virtual_litch_physical_frame rearmedReadyPhysical =
        session.PhysicalFrame();
    Check(session.DispatchSensorDelta(E2, false) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::PieceLifted) &&
              session.State().valid_destination_mask ==
                  LEGAL_E2_DESTINATIONS &&
              LitStatusPixelCount(session.SemanticFrame()) == 0U,
          "first pickup hides the armed assist and exposes semantic Undo");
    const auto destinationIsGuided = [&](const uint8_t square) {
        const auto& semantic = session.SemanticFrame();
        const auto& physical = session.PhysicalFrame();
        const uint16_t side0Index = PhysicalSquareLedIndex(
            journeyConfig, square, hardware::SquareLedSlot::Square0);
        const uint16_t side2Index = PhysicalSquareLedIndex(
            journeyConfig, square, hardware::SquareLedSlot::Square2);
        return ColorEnergy(BoardLed(
                   semantic, square,
                   hardware::SquareLedSlot::Square0)) != 0U &&
               ColorEnergy(BoardLed(
                   semantic, square,
                   hardware::SquareLedSlot::Square2)) != 0U &&
               !SameColor(
                   BoardLed(semantic, square,
                            hardware::SquareLedSlot::Square0),
                   BoardLed(rearmedReady, square,
                            hardware::SquareLedSlot::Square0)) &&
               !SameColor(
                   BoardLed(semantic, square,
                            hardware::SquareLedSlot::Square2),
                   BoardLed(rearmedReady, square,
                            hardware::SquareLedSlot::Square2)) &&
               side0Index < physical.led_count &&
               side2Index < physical.led_count &&
               ColorEnergy(physical.pixels[side0Index]) != 0U &&
               ColorEnergy(physical.pixels[side2Index]) != 0U &&
               !SameColor(physical.pixels[side0Index],
                          rearmedReadyPhysical.pixels[side0Index]) &&
               !SameColor(physical.pixels[side2Index],
                          rearmedReadyPhysical.pixels[side2Index]);
    };
    Check(session.Advance(150U) == VIRTUAL_LITCH_OK &&
              destinationIsGuided(E3) && destinationIsGuided(E4),
          "e2 lift visibly guides e3/e4 through semantic and mapped physical LEDs after the reveal");
    Check(TapP1X(session), "hidden-assist X uses physical button ingress");
    modules::ControlMutationOutcome pickupX;
    Check(session.TryGetLastBoardControlOutcome(pickupX) &&
              pickupX.status == modules::ControlMutationStatus::Applied &&
              pickupX.reason == modules::ControlMutationReason::None &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              (session.State().physical_occupied_mask ^
               session.State().expected_occupied_mask) == (1ULL << E2) &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              DedicatedStatusTopologyMatches(session) &&
              SameColor(BoardLed(session.SemanticFrame(), WHITE_KING,
                                 hardware::SquareLedSlot::Square0),
                        BoardLed(rearmedReady, WHITE_KING,
                                 hardware::SquareLedSlot::Square0)),
          "semantic Undo cancels the pickup and reveals the assist with the P1 Turn indicator");
    PrintJourneyCheckpoint(session, "hidden assist preserved",
                           "lift e2; wait for e3/e4 reveal; tap P1 X",
                           "return e2; play e2-e4");

    Check(session.DispatchSensorDelta(E2, true) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              readyFor(false) &&
              session.DispatchSensorDelta(E2, false) == VIRTUAL_LITCH_OK &&
              session.State().valid_destination_mask ==
                  LEGAL_E2_DESTINATIONS &&
              session.DispatchSensorDelta(E4, true) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Settling),
          "real engine accepts e2-e4 and starts settling");
    Check(session.Advance(1999U) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Settling),
          "e2-e4 remains Settling through 1999 ms");
    const uint64_t e2e4Occupancy =
        (INITIAL_OCCUPANCY & ~(1ULL << E2)) | (1ULL << E4);
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK && readyFor(true) &&
              session.State().physical_occupied_mask == e2e4Occupancy &&
              session.State().expected_occupied_mask == e2e4Occupancy &&
              session.State().side_to_move_black != 0U,
          "e2-e4 becomes P2 Play Ready at 2000 ms");
    const virtual_litch_semantic_frame p2ReadySemantic =
        session.SemanticFrame();
    const virtual_litch_physical_frame p2ReadyPhysical =
        session.PhysicalFrame();
    Check(session.Advance(80U) == VIRTUAL_LITCH_OK && readyFor(true) &&
              p2ReadySemantic.status_hash !=
                  session.SemanticFrame().status_hash &&
              !SameDedicatedPhysicalStatusPixels(
                  p2ReadyPhysical, session.PhysicalFrame()),
          "P2 Play Ready animates through semantic and physical perimeter output");
    const virtual_litch_semantic_frame settledWithAssistanceEnabled =
        session.SemanticFrame();
    const bool settledMutationApplied =
        MutateJourneySetupAssistance(session, 704U, false);
    const virtual_litch_semantic_frame settledWithAssistanceDisabled =
        session.SemanticFrame();
    const auto kingPairUnchanged = [&](const uint8_t kingSquare) {
        return SameColor(
                   BoardLed(settledWithAssistanceEnabled, kingSquare,
                            hardware::SquareLedSlot::Square0),
                   BoardLed(settledWithAssistanceDisabled, kingSquare,
                            hardware::SquareLedSlot::Square0)) &&
               SameColor(
                   BoardLed(settledWithAssistanceEnabled, kingSquare,
                            hardware::SquareLedSlot::Square2),
                   BoardLed(settledWithAssistanceDisabled, kingSquare,
                            hardware::SquareLedSlot::Square2));
    };
    const auto kingPairMatchesOrdinaryBackground = [&](const uint8_t kingSquare, const uint8_t referenceSquare) {
        return SameColor(
                   BoardLed(settledWithAssistanceEnabled, kingSquare,
                            hardware::SquareLedSlot::Square0),
                   BoardLed(settledWithAssistanceEnabled, referenceSquare,
                            hardware::SquareLedSlot::Square0)) &&
               SameColor(
                   BoardLed(settledWithAssistanceEnabled, kingSquare,
                            hardware::SquareLedSlot::Square2),
                   BoardLed(settledWithAssistanceEnabled, referenceSquare,
                            hardware::SquareLedSlot::Square2));
    };
    Check(settledMutationApplied &&
              kingPairUnchanged(WHITE_KING) &&
              kingPairUnchanged(BLACK_KING) &&
              kingPairMatchesOrdinaryBackground(
                  WHITE_KING, WHITE_KING_BACKGROUND_REFERENCE) &&
              kingPairMatchesOrdinaryBackground(
                  BLACK_KING, BLACK_KING_BACKGROUND_REFERENCE),
          "acknowledged settled e2-e4 no longer renders the king setup cue");
    PrintJourneyCheckpoint(session, "e2-e4 settled",
                           "place e4; wait 2000 ms", "journey complete");
}

void TestDuckBeforeAndAfterSetupPresentationJourney() {
    constexpr uint64_t INITIAL = UINT64_C(0xFFFF00000000FFFF);
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E3 = 44U;
    constexpr uint8_t E4 = 36U;
    constexpr uint8_t E7 = 12U;
    constexpr uint8_t DUCK_C6 = 18U;
    constexpr uint8_t EXTRA_D5 = 27U;
    constexpr uint64_t E2_LEGAL_DESTINATIONS =
        (1ULL << E3) | (1ULL << E4);
    test_support::BoundedHintEngine journeyEngine;
    JourneySession session(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3),
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
        nullptr, &journeyEngine);
    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "Duck setup presentation journey initializes");

    const auto calmDuckCue = [&](const uint8_t square) {
        return IsExactYellow(BoardLed(
                   session.SemanticFrame(), square,
                   hardware::SquareLedSlot::Piece)) &&
               SameColor(BoardLed(
                             session.SemanticFrame(), square,
                             hardware::SquareLedSlot::Square0),
                         BoardLed(
                             session.SemanticFrame(), square,
                             hardware::SquareLedSlot::Square2));
    };

    const uint64_t duckBefore32 =
        (INITIAL & ~(1ULL << E2)) | (1ULL << DUCK_C6);
    Check(DispatchJourneySnapshot(session, duckBefore32) ==
                  VIRTUAL_LITCH_OK &&
              session.GameVariant() == modules::GameVariant::Undecided &&
              session.SetupDuckCandidateSquare() == DUCK_C6 &&
              !session.BoardOverlayActive() &&
              calmDuckCue(DUCK_C6) &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false),
          "first eligible extra immediately owns calm Duck identity without premature presentation");
    Check(session.DispatchSensorDelta(EXTRA_D5, true) ==
                  VIRTUAL_LITCH_OK &&
              session.SetupDuckCandidateSquare() == DUCK_C6 &&
              !session.BoardOverlayActive() &&
              calmDuckCue(DUCK_C6) &&
              !SameColor(
                  BoardLed(session.SemanticFrame(), EXTRA_D5,
                           hardware::SquareLedSlot::Square0),
                  BoardLed(session.SemanticFrame(), EXTRA_D5,
                           hardware::SquareLedSlot::Piece)),
          "second extra before 200 ms leaves only that second square in rainbow correction");
    Check(session.DispatchSensorDelta(EXTRA_D5, false) ==
                  VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive() &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive() && calmDuckCue(DUCK_C6) &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false),
          "removing the early second extra permits Duck classification without a board icon");
    PrintJourneyCheckpoint(session, "Duck-before-32 activated",
                           "place c6 Duck with e2 missing; wait 200 ms",
                           "keep arranging the 32 orthodox pieces");
    Check(session.Advance(100U) == VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive() &&
              IsExactYellow(BoardLed(
                  session.SemanticFrame(), DUCK_C6,
                  hardware::SquareLedSlot::Piece)) &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false),
          "Duck classification stays board-quiet while setup continues");

    Check(session.DispatchSensorDelta(DUCK_C6, false) ==
                  VIRTUAL_LITCH_OK &&
              session.DispatchSensorDelta(DUCK_C6, true) ==
                  VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive() &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive() && calmDuckCue(DUCK_C6),
          "Duck removal and replacement restores the sole yellow identity without an icon");
    Check(session.DispatchSensorDelta(EXTRA_D5, true) ==
                  VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive() &&
              calmDuckCue(DUCK_C6) &&
              !SameColor(
                  BoardLed(session.SemanticFrame(), EXTRA_D5,
                           hardware::SquareLedSlot::Square0),
                  BoardLed(session.SemanticFrame(), EXTRA_D5,
                           hardware::SquareLedSlot::Piece)) &&
              session.DispatchSensorDelta(EXTRA_D5, false) ==
                  VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive(),
          "changed observation cancels Duck presentation immediately and it does not restart");

    Check(session.DispatchSensorDelta(E2, true) == VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive() &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.GameVariant() == modules::GameVariant::Duck &&
              session.DuckSquare() == DUCK_C6 &&
              !session.BoardOverlayActive() &&
              StatusHasSetupCompletionSweep(session.SemanticFrame()),
          "playable 32nd-piece setup acknowledges completion only after 200 ms stability");
    Check(session.Advance(1801U) == VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive() &&
              session.DispatchSensorDelta(E2, false) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::PieceLifted) &&
              session.State().valid_destination_mask ==
                  E2_LEGAL_DESTINATIONS &&
              session.State().correction_mask == 0ULL &&
              session.State().human_illegal_mask == 0ULL,
          "Duck-before-32 setup hands off to playable P1 legal guidance");
    PrintJourneyCheckpoint(session, "Duck opening P1 pickup",
                           "Ready expired; lift e2",
                           "return e2; lift e7");
    Check(session.DispatchSensorDelta(E2, true) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              !session.BoardOverlayActive(),
          "returning the P1 pawn restores the playable Duck opening");
    const bool p2PickupDispatched =
        session.DispatchSensorDelta(E7, false) == VIRTUAL_LITCH_OK &&
        session.Advance(modules::CORRECTION_STRONG_MS) == VIRTUAL_LITCH_OK;
    const virtual_litch_color p2InvalidSquare0 = BoardLed(
        session.SemanticFrame(), E7, hardware::SquareLedSlot::Square0);
    const virtual_litch_color p2InvalidPiece = BoardLed(
        session.SemanticFrame(), E7, hardware::SquareLedSlot::Piece);
    const virtual_litch_color p2InvalidSquare2 = BoardLed(
        session.SemanticFrame(), E7, hardware::SquareLedSlot::Square2);
    Check(p2PickupDispatched &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Correction) &&
              session.State().human_illegal_mask == (1ULL << E7) &&
              session.State().correction_mask == (1ULL << E7) &&
              StatusIsFullRedFault(session.SemanticFrame()) &&
              !SameColor(p2InvalidSquare0, p2InvalidPiece) &&
              !SameColor(p2InvalidPiece, p2InvalidSquare2) &&
              !SameColor(p2InvalidSquare0, p2InvalidSquare2),
          "Duck opening rejects a P2 pickup with shared rainbow/red feedback");
    PrintJourneyCheckpoint(session, "Duck opening P2 rejected",
                           "return e2; lift e7",
                           "return e7; add d5 extra");
    Check(session.DispatchSensorDelta(E7, true) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL,
          "returning the P2 pawn recovers the playable Duck opening");

    const bool extraApplied =
        session.DispatchSensorDelta(EXTRA_D5, true) == VIRTUAL_LITCH_OK &&
        session.Advance(modules::CORRECTION_STRONG_MS) == VIRTUAL_LITCH_OK;
    const virtual_litch_color extraSquare0 = BoardLed(
        session.SemanticFrame(), EXTRA_D5,
        hardware::SquareLedSlot::Square0);
    const virtual_litch_color extraPiece = BoardLed(
        session.SemanticFrame(), EXTRA_D5,
        hardware::SquareLedSlot::Piece);
    const virtual_litch_color extraSquare2 = BoardLed(
        session.SemanticFrame(), EXTRA_D5,
        hardware::SquareLedSlot::Square2);
    Check(extraApplied && calmDuckCue(DUCK_C6) &&
              CountExactYellowPieceCenters(session.SemanticFrame()) == 1U &&
              session.State().human_illegal_mask == (1ULL << EXTRA_D5) &&
              StatusIsFullRedFault(session.SemanticFrame()) &&
              !SameColor(extraSquare0, extraPiece) &&
              !SameColor(extraPiece, extraSquare2) &&
              !SameColor(extraSquare0, extraSquare2),
          "34th piece alone is rainbow while the established Duck stays yellow");
    PrintJourneyCheckpoint(session, "Duck plus 34th-piece correction",
                           "add d5 beside retained c6 Duck",
                           "advance rainbow; remove c6 Duck");
    const uint64_t firstExtraBoardHash =
        session.SemanticFrame().board_hash;
    Check(session.Advance(240U) == VIRTUAL_LITCH_OK &&
              session.SemanticFrame().board_hash != firstExtraBoardHash &&
              calmDuckCue(DUCK_C6) &&
              CountExactYellowPieceCenters(session.SemanticFrame()) == 1U &&
              session.State().human_illegal_mask == (1ULL << EXTRA_D5) &&
              !SameColor(
                  BoardLed(session.SemanticFrame(), EXTRA_D5,
                           hardware::SquareLedSlot::Square0),
                  extraSquare0) &&
              !SameColor(
                  BoardLed(session.SemanticFrame(), EXTRA_D5,
                           hardware::SquareLedSlot::Piece),
                  extraPiece) &&
              !SameColor(
                  BoardLed(session.SemanticFrame(), EXTRA_D5,
                           hardware::SquareLedSlot::Square2),
                  extraSquare2),
          "34th-piece rainbow advances without changing Duck or invalid ownership");
    PrintJourneyCheckpoint(session, "Duck plus animated correction",
                           "advance 34th-piece cue 240 ms",
                           "remove c6 Duck");

    Check(session.DispatchSensorDelta(DUCK_C6, false) ==
                  VIRTUAL_LITCH_OK &&
              session.GameVariant() == modules::GameVariant::Undecided &&
              session.DuckSquare() == modules::INVALID_DUCK_SQUARE,
          "removing the untouched Duck clears its identity even while the 34th remains");
    Check(session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.GameVariant() == modules::GameVariant::Duck &&
              session.DuckSquare() == EXTRA_D5 &&
              !session.BoardOverlayActive() &&
              calmDuckCue(EXTRA_D5),
          "remaining eligible extra becomes the replacement Duck without an icon");
    PrintJourneyCheckpoint(session, "Duck-after-32 replacement",
                           "leave d5 extra; remove c6 Duck; wait 200 ms",
                           "journey complete");
}

void TestRuntimeHealthJourneyPlayback() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;
    constexpr uint8_t E7 = 12U;
    constexpr uint64_t PLAYED_OCCUPANCY =
        (INITIAL_OCCUPANCY & ~(UINT64_C(1) << E2)) |
        (UINT64_C(1) << E4);
    constexpr uint32_t PAUSED_STATUS_MASK = UINT32_C(0x33333333);
    const virtual_litch_config journeyConfig =
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3);
    test_support::BoundedHintEngine journeyEngine;
    JourneySession session(
        journeyConfig,
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
        nullptr, &journeyEngine);
    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "runtime-health journey initializes the private production engine");

    virtual_litch_setting_request timerRequest{};
    virtual_litch_default_setting_request(&timerRequest);
    timerRequest.request_id = 900U;
    std::snprintf(timerRequest.key, sizeof(timerRequest.key), "%s", "TIMER");
    std::snprintf(timerRequest.value_json,
                  sizeof(timerRequest.value_json), "%s", "1");
    modules::ControlMutationOutcome timerOutcome;
    Check(session.MutateSetting(timerRequest, timerOutcome) ==
                  VIRTUAL_LITCH_OK &&
              timerOutcome.status == modules::ControlMutationStatus::Applied,
          "runtime-health journey enables a real clock through settings policy");

    virtual_litch_action_request newGame{};
    virtual_litch_default_action_request(&newGame);
    newGame.request_id = 901U;
    newGame.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    newGame.owner = VIRTUAL_LITCH_OWNER_P1;
    modules::ControlMutationOutcome newGameOutcome;
    Check(session.DispatchControlAction(newGame, newGameOutcome) ==
                  VIRTUAL_LITCH_OK &&
              newGameOutcome.status ==
                  modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(newGameOutcome) &&
              newGameOutcome.status ==
                  modules::ControlMutationStatus::Applied,
          "runtime-health journey starts through the production New Game path");

    Check(DispatchJourneySnapshot(session, INITIAL_OCCUPANCY) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              StatusHasSetupCompletionSweep(session.SemanticFrame()) &&
              session.Advance(1801U) == VIRTUAL_LITCH_OK,
          "runtime-health journey reaches a stable opening after setup acknowledgement");

    const runtime::GameClock* clock = session.GameClock();
    Check(clock != nullptr && clock->IsEnabled() && !clock->IsRunning() &&
              !clock->IsPaused() &&
              session.DispatchSensorDelta(E2, false) ==
                  VIRTUAL_LITCH_OK &&
              session.DispatchSensorDelta(E4, true) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(2001U) == VIRTUAL_LITCH_OK &&
              clock->IsRunning() && !clock->IsPaused() &&
              clock->GetActiveSide() == runtime::ClockSide::Black,
          "healthy clock waits for and then starts after the first settled move");
    const runtime::ClockSide initialClockSide =
        clock == nullptr ? runtime::ClockSide::White
                         : clock->GetActiveSide();
    const virtual_litch_runtime_health initialHealth =
        session.RuntimeHealth();
    Check(initialHealth.state ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_HEALTHY &&
              initialHealth.revision == 0U && initialHealth.causes == 0U &&
              initialHealth.board_input_allowed != 0U &&
              initialHealth.move_acceptance_allowed != 0U &&
              initialHealth.clocks_allowed != 0U &&
              StatusIsTurnIndicatorForOwnerWithUsbHealth(
                  session.SemanticFrame(), true) &&
              DedicatedStatusTopologyMatches(session),
          "healthy journey baseline exposes authority and normal board output");
    PrintRuntimeHealthCheckpoint(
        session, "healthy baseline", "opening matched; TIMER=1; e2-e4 settled",
        "play and clock advancing", "publish one ambient sensor");

    const auto dispatchAmbient = [&](const uint8_t count) {
        virtual_litch_runtime_health_fact fact{};
        virtual_litch_default_runtime_health_fact(&fact);
        fact.kind =
            VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_AMBIENT_SENSOR_COUNT;
        fact.ambient_sensor_count = count;
        return session.DispatchRuntimeHealthFact(fact);
    };
    const auto dispatchBoardScan = [&](const uint8_t scan) {
        virtual_litch_runtime_health_fact fact{};
        virtual_litch_default_runtime_health_fact(&fact);
        fact.kind = VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BOARD_SCAN;
        fact.board_scan = scan;
        return session.DispatchRuntimeHealthFact(fact);
    };

    const uint64_t healthyBoardHash = session.SemanticFrame().board_hash;
    const uint32_t healthyRemaining = clock == nullptr
                                          ? 0U
                                          : clock->GetRemainingMs(initialClockSide);
    Check(dispatchAmbient(1U) == VIRTUAL_LITCH_OK,
          "one-sensor fact reaches the VirtualLitch health seam");
    const virtual_litch_runtime_health degraded = session.RuntimeHealth();
    Check(degraded.state ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_DEGRADED_USABLE &&
              degraded.revision == 1U &&
              degraded.now_ms == session.State().now_ms &&
              degraded.frame_sequence == session.State().frame_sequence &&
              degraded.frame_sequence ==
                  session.SemanticFrame().frame_sequence &&
              degraded.frame_sequence ==
                  session.PhysicalFrame().frame_sequence &&
              degraded.causes ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_ONE_AMBIENT_SENSOR &&
              degraded.board_input_allowed != 0U &&
              degraded.move_acceptance_allowed != 0U &&
              degraded.clocks_allowed != 0U &&
              StatusIsTurnIndicatorForOwnerWithUsbHealth(
                  session.SemanticFrame(), true) &&
              LitStatusPixelCount(session.SemanticFrame()) > 2U &&
              LitDedicatedPhysicalStatusPixelCount(session) ==
                  LitStatusPixelCount(session.SemanticFrame()) &&
              DedicatedStatusTopologyMatches(session) &&
              session.SemanticFrame().board_hash == healthyBoardHash &&
              clock != nullptr && clock->IsRunning() && !clock->IsPaused(),
          "usable degradation keeps play live and uses the sparse production cue");
    const uint32_t degradedObservationSequence =
        session.State().board_observation_sequence;
    Check(session.DispatchSensorDelta(E7, false) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::PieceLifted) &&
              session.DispatchSensorDelta(E7, true) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().physical_occupied_mask == PLAYED_OCCUPANCY &&
              session.State().board_observation_sequence >
                  degradedObservationSequence &&
              session.RuntimeHealth().revision == 1U &&
              !clock->IsPaused() &&
              clock->GetActiveSide() == initialClockSide &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), true),
          "usable degradation accepts and reverses a production board pickup");
    PrintRuntimeHealthCheckpoint(
        session, "degraded but playable", "ambient_sensor_count=1",
        "two-marker overlay; Turn indicator and play remain active",
        "publish sustained uncertain board scans");
    Check(session.Advance(20U) == VIRTUAL_LITCH_OK &&
              clock->GetRemainingMs(initialClockSide) < healthyRemaining,
          "usable degradation leaves clock accounting active");

    Check(dispatchBoardScan(
              VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN) ==
                  VIRTUAL_LITCH_OK &&
              session.RuntimeHealth().revision == 1U &&
              session.Advance(99U) == VIRTUAL_LITCH_OK &&
              dispatchBoardScan(
                  VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN) ==
                  VIRTUAL_LITCH_OK &&
              session.RuntimeHealth().revision == 1U,
          "uncertain scans remain below the 100 ms pause boundary");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK,
          "runtime-health journey reaches the exact pause boundary");
    const uint64_t boardHashAtPauseBoundary =
        session.SemanticFrame().board_hash;
    Check(dispatchBoardScan(
              VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN) ==
              VIRTUAL_LITCH_OK,
          "uncertain evidence at 100 ms enters the pause");
    const virtual_litch_runtime_health paused = session.RuntimeHealth();
    const uint32_t pausedRemaining =
        clock->GetRemainingMs(initialClockSide);
    const uint64_t pausedStatusHash = session.SemanticFrame().status_hash;
    const virtual_litch_physical_frame pausedPhysical =
        session.PhysicalFrame();
    const virtual_litch_state pausedState = session.State();
    Check(paused.state == VIRTUAL_LITCH_RUNTIME_HEALTH_INPUT_PAUSED &&
              paused.revision == 2U &&
              paused.causes ==
                  (VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_ONE_AMBIENT_SENSOR |
                   VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_BOARD_SCAN_UNCERTAIN) &&
              paused.board_input_allowed == 0U &&
              paused.move_acceptance_allowed == 0U &&
              paused.clocks_allowed == 0U &&
              clock->IsRunning() && clock->IsPaused() &&
              clock->GetActiveSide() == initialClockSide &&
              LitStatusPixelCount(session.SemanticFrame()) == 16U &&
              LitDedicatedPhysicalStatusPixelCount(session) == 16U &&
              LitStatusPixelMask(session.SemanticFrame()) ==
                  PAUSED_STATUS_MASK &&
              LitDedicatedPhysicalStatusPixelMask(session) ==
                  PAUSED_STATUS_MASK &&
              DedicatedStatusTopologyMatches(session) &&
              session.SemanticFrame().board_hash == boardHashAtPauseBoundary,
          "sustained uncertainty pauses real gameplay and shows dashed feedback");
    Check(session.DispatchSensorDelta(E7, false) ==
                  VIRTUAL_LITCH_POLICY_REJECTED &&
              session.State().physical_occupied_mask ==
                  pausedState.physical_occupied_mask &&
              session.State().board_observation_sequence ==
                  pausedState.board_observation_sequence &&
              session.State().interaction_phase ==
                  pausedState.interaction_phase,
          "paused health rejects board ingress before reducer mutation");
    Check(session.Advance(500U) == VIRTUAL_LITCH_OK &&
              clock->GetRemainingMs(initialClockSide) == pausedRemaining &&
              clock->GetActiveSide() == initialClockSide &&
              LitStatusPixelCount(session.SemanticFrame()) == 16U &&
              session.SemanticFrame().status_hash != pausedStatusHash &&
              !SameDedicatedPhysicalStatusPixels(
                  pausedPhysical, session.PhysicalFrame()),
          "paused clock stays frozen while the production cue breathes visibly");
    PrintRuntimeHealthCheckpoint(
        session, "paused and visible",
        "uncertain scans for 100 ms; reject e2 lift; advance 500 ms",
        "input rejected; clock frozen; 16-pixel cue remains visible",
        "publish fresh stable scans for 100 ms");

    Check(dispatchBoardScan(
              VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_STABLE) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(99U) == VIRTUAL_LITCH_OK &&
              dispatchBoardScan(
                  VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_STABLE) ==
                  VIRTUAL_LITCH_OK &&
              session.RuntimeHealth().state ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_INPUT_PAUSED &&
              session.RuntimeHealth().revision == 2U &&
              clock->IsPaused(),
          "fresh success remains paused below the 100 ms recovery boundary");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
              dispatchBoardScan(
                  VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_STABLE) ==
                  VIRTUAL_LITCH_OK,
          "fresh success at 100 ms clears only board uncertainty");
    const virtual_litch_runtime_health stabilized =
        session.RuntimeHealth();
    Check(stabilized.state ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_DEGRADED_USABLE &&
              stabilized.revision == 3U &&
              stabilized.causes ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_ONE_AMBIENT_SENSOR &&
              stabilized.board_input_allowed != 0U &&
              stabilized.move_acceptance_allowed != 0U &&
              stabilized.clocks_allowed != 0U &&
              clock->IsPaused() &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), true) &&
              LitStatusPixelCount(session.SemanticFrame()) > 2U &&
              LitDedicatedPhysicalStatusPixelCount(session) ==
                  LitStatusPixelCount(session.SemanticFrame()) &&
              DedicatedStatusTopologyMatches(session),
          "bounded recovery reveals the still-valid degraded cause and cue");
    PrintRuntimeHealthCheckpoint(
        session, "stabilized recovery", "fresh stable scans for 100 ms",
        "board ingress allowed; clock awaits normal coordinator tick",
        "advance coordinator and restore second ambient sensor");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
              clock->IsRunning() && !clock->IsPaused() &&
              clock->GetActiveSide() == initialClockSide &&
              clock->GetRemainingMs(initialClockSide) == pausedRemaining,
          "normal coordinator reconciliation resumes the same frozen clock");
    Check(session.Advance(20U) == VIRTUAL_LITCH_OK &&
              clock->GetRemainingMs(initialClockSide) < pausedRemaining &&
              clock->GetRemainingMs(initialClockSide) >=
                  pausedRemaining - 20U,
          "resumed clock advances without reset, increment, or side switch");

    const uint64_t degradedBoardHash = session.SemanticFrame().board_hash;
    Check(dispatchAmbient(2U) == VIRTUAL_LITCH_OK,
          "two-sensor fact clears the final degraded cause");
    const virtual_litch_runtime_health recovered =
        session.RuntimeHealth();
    Check(recovered.state == VIRTUAL_LITCH_RUNTIME_HEALTH_HEALTHY &&
              recovered.revision == 4U && recovered.causes == 0U &&
              recovered.board_input_allowed != 0U &&
              recovered.move_acceptance_allowed != 0U &&
              recovered.clocks_allowed != 0U &&
              clock->IsRunning() && !clock->IsPaused() &&
              clock->GetActiveSide() == initialClockSide &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), true) &&
              DedicatedStatusTopologyMatches(session) &&
              session.SemanticFrame().board_hash == degradedBoardHash,
          "final validated recovery restores healthy normal presentation");
    PrintRuntimeHealthCheckpoint(
        session, "healthy recovery", "ambient_sensor_count=2",
        "normal Play Ready output and same clock restored",
        "publish boot-owned persisted brownout fact");

    Check(MutateJourneyBool(session, 903U, "MINIMAL_ANIMATIONS", true),
          "health mapping comparison uses stationary production presentation");
    const uint32_t healthPhase = session.State().now_ms % 5000U;
    const uint32_t untilHealthPulse =
        healthPhase == 0U ? 0U : 5000U - healthPhase;
    Check(session.Advance(untilHealthPulse) == VIRTUAL_LITCH_OK,
          "healthy reference reaches the same five-second presentation phase");
    const virtual_litch_physical_frame healthyRotatedPhysical =
        session.PhysicalFrame();
    const uint64_t boardHashBeforePowerLatch = session.SemanticFrame().board_hash;
    const uint32_t remainingBeforePowerLatch =
        clock->GetRemainingMs(initialClockSide);
    virtual_litch_runtime_health_fact brownout{};
    virtual_litch_default_runtime_health_fact(&brownout);
    brownout.kind = VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BROWNOUT;
    brownout.brownout_detected = 1U;
    Check(session.DispatchRuntimeHealthFact(brownout) ==
              VIRTUAL_LITCH_OK,
          "boot-owned persisted brownout fact reaches the normalized health seam");
    const virtual_litch_runtime_health powerLimited =
        session.RuntimeHealth();
    const bool boardUnchangedAtPowerFact =
        session.SemanticFrame().board_hash == boardHashBeforePowerLatch;
    bool onlyUsbPairChanged = true;
    const uint16_t usbFirst = hardware::BOARD_LED_COUNT + 27U;
    const uint16_t usbSecond = hardware::BOARD_LED_COUNT + 28U;
    for (uint16_t index = 0U; index < session.PhysicalFrame().led_count; ++index) {
        if (index == usbFirst || index == usbSecond) continue;
        const auto before = healthyRotatedPhysical.pixels[index];
        const auto after = session.PhysicalFrame().pixels[index];
        onlyUsbPairChanged = onlyUsbPairChanged &&
            before.r == after.r && before.g == after.g &&
            before.b == after.b && before.w == after.w;
    }
    std::printf(
        "[RUNTIME_HEALTH] usb-pulse semantic27=%u,%u,%u semantic28=%u,%u,%u "
        "physical27=%u,%u,%u physical28=%u,%u,%u t=%u "
        "board_unchanged=%u turn_retained=%u topology=%u\n",
        session.SemanticFrame().status[27U].r,
        session.SemanticFrame().status[27U].g,
        session.SemanticFrame().status[27U].b,
        session.SemanticFrame().status[28U].r,
        session.SemanticFrame().status[28U].g,
        session.SemanticFrame().status[28U].b,
        session.PhysicalFrame().pixels[hardware::BOARD_LED_COUNT + 27U].r,
        session.PhysicalFrame().pixels[hardware::BOARD_LED_COUNT + 27U].g,
        session.PhysicalFrame().pixels[hardware::BOARD_LED_COUNT + 27U].b,
        session.PhysicalFrame().pixels[hardware::BOARD_LED_COUNT + 28U].r,
        session.PhysicalFrame().pixels[hardware::BOARD_LED_COUNT + 28U].g,
        session.PhysicalFrame().pixels[hardware::BOARD_LED_COUNT + 28U].b,
        session.State().now_ms,
        boardUnchangedAtPowerFact ? 1U : 0U,
        StatusIsTurnIndicatorForOwnerWithUsbHealth(
            session.SemanticFrame(), true) ? 1U : 0U,
        DedicatedStatusTopologyMatchesExceptUsbHealth(session) ? 1U : 0U);
    Check(powerLimited.state ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_DEGRADED_USABLE &&
              powerLimited.revision == 5U &&
              powerLimited.causes ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_BROWNOUT_LATCHED &&
              powerLimited.board_input_allowed != 0U &&
              powerLimited.move_acceptance_allowed != 0U &&
              powerLimited.clocks_allowed != 0U &&
              clock->IsRunning() && !clock->IsPaused() &&
              clock->GetActiveSide() == initialClockSide &&
              boardUnchangedAtPowerFact &&
              onlyUsbPairChanged &&
              StatusIsTurnIndicatorForOwnerWithUsbHealth(
                  session.SemanticFrame(), true) &&
              PhysicalHasUsbSideYellowHealthMarker(session.PhysicalFrame()) &&
              DedicatedStatusTopologyMatchesExceptUsbHealth(session),
          "latched power truth adds a fixed physical USB-side yellow marker without pausing play");
    Check(session.Advance(250U) == VIRTUAL_LITCH_OK &&
              session.RuntimeHealth().revision == 5U &&
              IsAmberHealthMarker(session.PhysicalFrame().pixels[usbSecond]) &&
              !IsAmberHealthMarker(session.PhysicalFrame().pixels[usbFirst]) &&
              clock->GetRemainingMs(initialClockSide) <
                  remainingBeforePowerLatch,
          "power cue blinks to the adjacent USB-side orange marker while the clock advances");
    const uint64_t boardHashWhilePowerLimited =
        session.SemanticFrame().board_hash;
    PrintRuntimeHealthCheckpoint(
        session, "power limited but playable",
        "normalized boot adapter republishes persisted brownout latch",
        "fixed USB-side yellow/orange pulse visible; play and clock remain active",
        "validated firmware owner supplies authoritative power-only reset");

    virtual_litch_runtime_health_fact resetPowerProtection{};
    virtual_litch_default_runtime_health_fact(&resetPowerProtection);
    resetPowerProtection.kind =
        VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_RESET_LATCHED_PROTECTION;
    Check(session.DispatchRuntimeHealthFact(resetPowerProtection) ==
              VIRTUAL_LITCH_OK,
          "normalized authoritative clear reaches the production reducer path");
    const virtual_litch_runtime_health powerRecovered =
        session.RuntimeHealth();
    Check(powerRecovered.state ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_HEALTHY &&
              powerRecovered.revision == 6U &&
              powerRecovered.causes == 0U &&
              powerRecovered.board_input_allowed != 0U &&
              powerRecovered.move_acceptance_allowed != 0U &&
              powerRecovered.clocks_allowed != 0U,
          "authoritative power clear restores healthy advice");
    Check(clock->IsRunning() && !clock->IsPaused() &&
              clock->GetActiveSide() == initialClockSide,
          "authoritative power clear preserves the live clock");
    Check(session.SemanticFrame().board_hash == boardHashWhilePowerLimited,
          "authoritative power clear leaves board guidance unchanged");
    Check(StatusIsTurnIndicatorForOwner(
              session.SemanticFrame(), true) &&
              !StatusHasUsbSideYellowHealthMarker(session.SemanticFrame()) &&
              DedicatedStatusTopologyMatches(session),
          "authoritative power clear removes the USB-side warning from the active-side Turn cue");
    PrintRuntimeHealthCheckpoint(
        session, "power health recovered",
        "normalized reset after firmware fresh-CC and persistence owner",
        "USB-side warning removed; same game and clock continue",
        "lift e7 to resume play");
    Check(session.DispatchSensorDelta(E7, false) == VIRTUAL_LITCH_OK &&
              session.State().physical_occupied_mask ==
                  (PLAYED_OCCUPANCY & ~(1ULL << E7)) &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::PieceLifted),
          "recovered health admits production board gameplay again");
}

void TestPowerLimitedFreshSetupKeepsTurnIndicator() {
    test_support::BoundedHintEngine journeyEngine;
    JourneySession session(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3),
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
        nullptr, &journeyEngine);
    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "power-limited setup journey initializes");

    virtual_litch_action_request newGame{};
    virtual_litch_default_action_request(&newGame);
    newGame.request_id = 902U;
    newGame.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    newGame.owner = VIRTUAL_LITCH_OWNER_P1;
    modules::ControlMutationOutcome newGameOutcome;
    Check(session.DispatchControlAction(newGame, newGameOutcome) ==
                  VIRTUAL_LITCH_OK &&
              newGameOutcome.status ==
                  modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(newGameOutcome) &&
              newGameOutcome.status ==
                  modules::ControlMutationStatus::Applied,
          "power-limited setup journey starts through New Game");
    const uint64_t healthySetupStatusHash =
        session.SemanticFrame().status_hash;

    virtual_litch_runtime_health_fact brownout{};
    virtual_litch_default_runtime_health_fact(&brownout);
    brownout.kind = VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BROWNOUT;
    brownout.brownout_detected = 1U;
    Check(session.DispatchRuntimeHealthFact(brownout) ==
                  VIRTUAL_LITCH_OK &&
              session.RuntimeHealth().state ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_DEGRADED_USABLE &&
              session.RuntimeHealth().causes ==
                  VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_BROWNOUT_LATCHED &&
              session.RuntimeHealth().board_input_allowed != 0U &&
              session.RuntimeHealth().move_acceptance_allowed != 0U &&
              session.RuntimeHealth().clocks_allowed != 0U &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              session.SemanticFrame().status_hash == healthySetupStatusHash,
          "latched power limiting keeps Fresh Setup playable with only the P1 Turn indicator");
    const uint64_t limitedSetupStatusHash =
        session.SemanticFrame().status_hash;
    Check(session.Advance(200U) == VIRTUAL_LITCH_OK &&
              StatusIsTurnIndicatorForOwner(
                  session.SemanticFrame(), false) &&
              session.SemanticFrame().status_hash !=
                  limitedSetupStatusHash,
          "power-limited Fresh Setup keeps the Turn indicator animated");
}

void TestAppendOnlyControlMutationAbi() {
    virtual_litch* a = CreateBoard(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    virtual_litch* b = CreateBoard(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));

    virtual_litch_setting_request setting{};
    virtual_litch_default_setting_request(&setting);
    setting.request_id = 51U;
    std::snprintf(setting.key, sizeof(setting.key), "%s", "BRIGHTNESS");
    std::snprintf(setting.value_json, sizeof(setting.value_json), "%s", "6");
    virtual_litch_control_outcome outcome{};
    Check(virtual_litch_mutate_setting(a, &setting, &outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.request_id == 51U && outcome.status == 1U &&
              outcome.settings_revision == 1U,
          "fake setting mutation uses correlated production policy outcome");
    Check(virtual_litch_mutate_setting(a, &setting, &outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == 3U && outcome.settings_revision == 1U,
          "fake setting mutation reports Noop without a revision bump");

    virtual_litch_action_request action{};
    virtual_litch_default_action_request(&action);
    action.request_id = 77U;
    action.action = VIRTUAL_LITCH_CONTROL_COACHING;
    action.owner = VIRTUAL_LITCH_OWNER_P1;
    Check(virtual_litch_dispatch_control_action(a, &action, &outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == 1U && outcome.request_id == 77U,
          "owner-local coaching applies synchronously");
    action.coaching_mutation = 1U;
    Check(virtual_litch_dispatch_control_action(b, &action, &outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == 3U,
          "coaching state is owned independently by each fake session");

    action = virtual_litch_action_request{};
    virtual_litch_default_action_request(&action);
    action.request_id = 65535U;
    action.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    Check(virtual_litch_dispatch_control_action(a, &action, &outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == 0U && outcome.request_id == 65535U,
          "new game is first acknowledged as Accepted");
    Check(virtual_litch_dispatch_control_action(a, &action, &outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == 2U &&
              outcome.reason ==
                  static_cast<uint8_t>(
                      modules::ControlMutationReason::Busy) &&
              outcome.request_id == 65535U,
          "duplicate pending VirtualLitch control ID reports busy");
    Check(virtual_litch_advance(a, 1U) == VIRTUAL_LITCH_OK &&
              virtual_litch_take_control_completion(a, &outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == 1U && outcome.request_id == 65535U,
          "new game later completes with the same bounded request ID");

    action = virtual_litch_action_request{};
    virtual_litch_default_action_request(&action);
    action.request_id = 144U;
    action.action = VIRTUAL_LITCH_CONTROL_UNDO;
    action.undo_move_count = 1U;
    Check(virtual_litch_dispatch_control_action(a, &action, &outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.status == 0U && outcome.request_id == 144U,
          "VirtualLitch undo is first acknowledged through the shared action path");
    Check(virtual_litch_advance(a, 1U) == VIRTUAL_LITCH_OK &&
              virtual_litch_take_control_completion(a, &outcome) ==
                  VIRTUAL_LITCH_OK &&
              outcome.request_id == 144U && outcome.status == 2U &&
              outcome.reason ==
                  static_cast<uint8_t>(
                      modules::ControlMutationReason::Unsupported),
          "VirtualLitch undo completes as unsupported");

    LargerSettingRequest largerSetting{};
    virtual_litch_default_setting_request(&largerSetting.base);
    largerSetting.base.struct_size = sizeof(largerSetting);
    largerSetting.base.request_id = 88U;
    std::snprintf(largerSetting.base.key, sizeof(largerSetting.base.key), "%s",
                  "BRIGHTNESS");
    std::snprintf(largerSetting.base.value_json,
                  sizeof(largerSetting.base.value_json), "%s", "7");
    Check(virtual_litch_mutate_setting(
              a, reinterpret_cast<const virtual_litch_setting_request*>(&largerSetting),
              &outcome) == VIRTUAL_LITCH_OK &&
              outcome.request_id == 88U,
          "larger future setting request is accepted via prefix ABI");

    virtual_litch_setting_request undersizedSetting{};
    virtual_litch_default_setting_request(&undersizedSetting);
    undersizedSetting.struct_size =
        static_cast<uint32_t>(sizeof(undersizedSetting) - 1U);
    std::snprintf(undersizedSetting.key, sizeof(undersizedSetting.key), "%s",
                  "BRIGHTNESS");
    std::snprintf(undersizedSetting.value_json,
                  sizeof(undersizedSetting.value_json), "%s", "7");
    Check(virtual_litch_mutate_setting(a, &undersizedSetting, &outcome) ==
              VIRTUAL_LITCH_ABI_MISMATCH,
          "undersized setting request is rejected");

    LargerActionRequest largerAction{};
    virtual_litch_default_action_request(&largerAction.base);
    largerAction.base.struct_size = sizeof(largerAction);
    largerAction.base.request_id = 99U;
    largerAction.base.action = VIRTUAL_LITCH_CONTROL_COACHING;
    largerAction.base.owner = VIRTUAL_LITCH_OWNER_P2;
    Check(virtual_litch_dispatch_control_action(
              a, reinterpret_cast<const virtual_litch_action_request*>(&largerAction),
              &outcome) == VIRTUAL_LITCH_OK &&
              outcome.request_id == 99U,
          "larger future action request is accepted via prefix ABI");

    virtual_litch_action_request undersizedAction{};
    virtual_litch_default_action_request(&undersizedAction);
    undersizedAction.struct_size =
        static_cast<uint32_t>(sizeof(undersizedAction) - 1U);
    undersizedAction.action = VIRTUAL_LITCH_CONTROL_COACHING;
    undersizedAction.owner = VIRTUAL_LITCH_OWNER_P1;
    Check(virtual_litch_dispatch_control_action(
              a, &undersizedAction, &outcome) ==
              VIRTUAL_LITCH_ABI_MISMATCH,
          "undersized action request is rejected");

    virtual_litch_destroy(a);
    virtual_litch_destroy(b);
}

void TestConcurrentHandleClockIsolation() {
    constexpr uint32_t ITERATIONS = 2000U;
    constexpr uint32_t FIRST_START_MS = 1000U;
    constexpr uint32_t SECOND_START_MS = 500000U;
    constexpr uint32_t FIRST_STEP_MS = 1U;
    constexpr uint32_t SECOND_STEP_MS = 7U;

    virtual_litch_config firstConfig =
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3);
    firstConfig.initial_time_ms = FIRST_START_MS;
    virtual_litch_config secondConfig =
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3);
    secondConfig.initial_time_ms = SECOND_START_MS;
    virtual_litch* first = CreateBoard(firstConfig);
    virtual_litch* second = CreateBoard(secondConfig);

    std::atomic<bool> start{false};
    std::atomic<bool> firstOk{true};
    std::atomic<bool> secondOk{true};
    const auto exercise = [&](virtual_litch* board,
                              const uint32_t initialMs,
                              const uint32_t stepMs,
                              std::atomic<bool>& ok) {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (uint32_t iteration = 1U; iteration <= ITERATIONS; ++iteration) {
            virtual_litch_state state{};
            virtual_litch_semantic_frame semantic{};
            virtual_litch_physical_frame physical{};
            const uint32_t expectedNowMs = initialMs + iteration * stepMs;
            const bool iterationOk =
                virtual_litch_advance(board, stepMs) ==
                    VIRTUAL_LITCH_OK &&
                virtual_litch_get_state(board, &state) ==
                    VIRTUAL_LITCH_OK &&
                virtual_litch_get_semantic_frame(board, &semantic) ==
                    VIRTUAL_LITCH_OK &&
                virtual_litch_get_physical_frame(board, &physical) ==
                    VIRTUAL_LITCH_OK &&
                state.now_ms == expectedNowMs &&
                semantic.now_ms == expectedNowMs &&
                physical.now_ms == expectedNowMs;
            if (!iterationOk) {
                ok.store(false, std::memory_order_relaxed);
                return;
            }
        }
    };

    std::thread firstThread(
        exercise, first, FIRST_START_MS, FIRST_STEP_MS, std::ref(firstOk));
    std::thread secondThread(
        exercise, second, SECOND_START_MS, SECOND_STEP_MS, std::ref(secondOk));
    start.store(true, std::memory_order_release);
    firstThread.join();
    secondThread.join();

    Check(firstOk.load(std::memory_order_relaxed) &&
              secondOk.load(std::memory_order_relaxed),
          "concurrent VirtualLitch handles retain independent clocks and frames");
    virtual_litch_destroy(first);
    virtual_litch_destroy(second);
}

void PrintRawFrame(const runtime::VirtualLitchSession& session,
                   const char* step,
                   const char* next);
void CheckRawJourney(bool condition,
                     const char* message,
                     const runtime::VirtualLitchSession& session);

void TestRawPlayerEndQuickSettingJourney() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t LOGICAL_E2 = 52U;
    constexpr uint8_t PHYSICAL_E2_FROM_P2_END = 11U;
    constexpr std::array<uint8_t, 4> KQ_SQUARES{3U, 4U, 59U, 60U};
    constexpr uint64_t E2_DESTINATIONS =
        (1ULL << 44U) | (1ULL << 36U);

    test_support::BoundedHintSession sessionFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& session = sessionFixture.session;
    CheckRawJourney(
        session.Reset() == VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(200U) == VIRTUAL_LITCH_OK,
        "raw player-end journey reaches the exact opening", session);
    CheckRawJourney(
        IsBlueDominant(PhysicalBoardLed(
            session.PhysicalFrame(), LOGICAL_E2,
            hardware::SquareLedSlot::Piece)) &&
            IsRedDominant(PhysicalBoardLed(
                session.PhysicalFrame(), PHYSICAL_E2_FROM_P2_END,
                hardware::SquareLedSlot::Piece)),
        "raw player-end journey starts with distinct logical-player palettes",
        session);

    const auto setDown = [&](const runtime::VirtualLitchPhysicalButton button,
                             const bool down) {
        return session.SetButtonDown(
                   VIRTUAL_LITCH_OWNER_P1, button, down) ==
                   VIRTUAL_LITCH_OK &&
               session.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    const auto tap = [&](const runtime::VirtualLitchPhysicalButton button) {
        if (session.QuickSettingsActive() &&
            (button == runtime::VirtualLitchPhysicalButton::Left ||
             button == runtime::VirtualLitchPhysicalButton::Right) &&
            session.Advance(250U) != VIRTUAL_LITCH_OK) return false;
        return setDown(button, true) && setDown(button, false);
    };

    CheckRawJourney(
        setDown(runtime::VirtualLitchPhysicalButton::X, true) &&
            session.Advance(1000U) == VIRTUAL_LITCH_OK &&
            session.QuickSettingsActive() &&
            setDown(runtime::VirtualLitchPhysicalButton::X, false),
        "raw debounced P1 X hold opens production Quick Setup", session);
    Check(tap(runtime::VirtualLitchPhysicalButton::Up),
          "raw player-end journey enables VS CPU level 1");
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Right) &&
            session.QuickSettingsSelectedIndex() ==
                modules::quick_settings_items::CPU_PLAYER &&
            session.DisplayText().empty(),
        "CPU player selection starts with a non-letter identity icon",
        session);
    const uint64_t cpuPlayerIdentityHash =
        session.SemanticFrame().board_hash;
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Right) &&
            session.QuickSettingsSelectedIndex() ==
                modules::quick_settings_items::FIRST_PLAYER &&
            session.DisplayText().empty() &&
            session.SemanticFrame().board_hash != cpuPlayerIdentityHash,
        "raw P1 directions select player-side placement", session);
    PrintRawFrame(session, "Quick Setup player-side selected",
                  "press Up to move P1/White to the opposite player end");
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Up) &&
            session.FirstPlayerIsP2() &&
            session.DisplayText() == "2" &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.KingQueenSwapActive(),
        "GAME_FIRST_PLAYER moves P1 state and starts K/Q correction",
        session);
    PrintRawFrame(session, "P1 moved to opposite player end",
                  "press Down before lifting a prompted piece to cancel");
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Down) &&
            !session.FirstPlayerIsP2() &&
            session.DisplayText() == "1" &&
            session.Advance(50U) == VIRTUAL_LITCH_OK &&
            !session.KingQueenSwapActive(),
        "reverting player end before a K/Q lift cancels without requeue",
        session);
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Up) &&
            session.FirstPlayerIsP2() &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.KingQueenSwapActive(),
        "player-end journey restarts one K/Q correction", session);
    uint64_t physical = INITIAL_OCCUPANCY;
    physical &= ~(UINT64_C(1) << KQ_SQUARES.front());
    CheckRawJourney(
        session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
            session.KingQueenSwapActive(),
        "raw Player ends correction accepts the first K/Q lift", session);
    const virtual_litch_semantic_frame promptBeforeBusy =
        session.SemanticFrame();
    const std::string displayBeforeBusy(session.DisplayText());
    const auto kqPieceCuesPreserved = [&]() {
        for (const uint8_t square : KQ_SQUARES) {
            const auto actual = BoardLed(session.SemanticFrame(), square,
                                         hardware::SquareLedSlot::Piece);
            // Busy feedback must preserve the source's identity and its
            // intentional off phase, rather than freezing its animation.
            if (ColorEnergy(actual) != 0U && !SameColor(
                    actual, BoardLed(promptBeforeBusy, square,
                                     hardware::SquareLedSlot::Piece))) {
                return false;
            }
        }
        return true;
    };
    std::size_t darkKqPieceCount = 0U;
    bool kqPieceCuesMatchOccupancy = true;
    for (const uint8_t square : KQ_SQUARES) {
        const bool occupied =
            (session.State().physical_occupied_mask &
             (UINT64_C(1) << square)) != 0ULL;
        const bool lit = ColorEnergy(BoardLed(
            promptBeforeBusy, square, hardware::SquareLedSlot::Piece)) != 0U;
        darkKqPieceCount += lit ? 0U : 1U;
        kqPieceCuesMatchOccupancy =
            kqPieceCuesMatchOccupancy && lit == occupied;
    }
    CheckRawJourney(
        displayBeforeBusy == "2" &&
            kqPieceCuesMatchOccupancy && darkKqPieceCount == 1U,
        "lifted K/Q correction retains the selected value and occupied-piece cues",
        session);
    modules::ControlMutationOutcome postLiftBoardOutcome;
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Down) &&
            session.TryGetLastBoardControlOutcome(postLiftBoardOutcome) &&
            postLiftBoardOutcome.status ==
                modules::ControlMutationStatus::Rejected &&
            postLiftBoardOutcome.reason ==
                modules::ControlMutationReason::Busy,
        "raw Player ends reversal reports a typed busy rejection", session);
    CheckRawJourney(
        session.EdgeCueIntent().kind ==
                modules::EdgeLedCueKind::Unavailable &&
            session.EdgeCueIntent().owner ==
                static_cast<modules::UiOwner>(session.QuickSettingsOwner()),
        "busy reversal routes a non-text perimeter cue to Quick Setup owner",
        session);
    CheckRawJourney(
        session.DisplayText() == displayBeforeBusy &&
            kqPieceCuesPreserved(),
        "busy reversal preserves selected value and K/Q piece cues",
        session);
    CheckRawJourney(
        SameBoardPixelsOutsideMask(
            session.SemanticFrame(),
            promptBeforeBusy,
            (UINT64_C(1) << 3U) | (UINT64_C(1) << 4U) |
                (UINT64_C(1) << 59U) | (UINT64_C(1) << 60U)),
        "busy perimeter feedback preserves board pixels outside K/Q prompts",
        session);
    CheckRawJourney(
        session.SemanticFrame().status_hash != promptBeforeBusy.status_hash,
        "busy perimeter feedback is visibly present during K/Q correction",
        session);
    CheckRawJourney(
        session.FirstPlayerIsP2() && session.KingQueenSwapActive(),
        "raw Player ends reversal stays busy after the first K/Q lift", session);
    PrintRawFrame(session, "post-lift Player ends reversal rejected",
                  "replace the lifted piece and finish the current swap");
    const modules::EdgeLedCueIntent busyCue = session.EdgeCueIntent();
    const uint32_t busyCueElapsedMs =
        static_cast<uint32_t>(session.State().now_ms - busyCue.startMs);
    const uint32_t busyCueRemainingMs =
        busyCueElapsedMs < modules::EdgeLedCueController::UNAVAILABLE_DURATION_MS
            ? modules::EdgeLedCueController::UNAVAILABLE_DURATION_MS -
                  busyCueElapsedMs
            : 0U;
    CheckRawJourney(
        busyCueRemainingMs > 0U &&
            session.Advance(busyCueRemainingMs - 1U) ==
                VIRTUAL_LITCH_OK &&
            session.EdgeCueIntent().kind ==
                modules::EdgeLedCueKind::Unavailable &&
            session.DisplayText() == displayBeforeBusy &&
            kqPieceCuesPreserved() &&
            session.KingQueenSwapActive(),
        "busy perimeter cue preserves selected value and K/Q cues through its penultimate ms",
        session);
    CheckRawJourney(
        session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.EdgeCueIntent().kind == modules::EdgeLedCueKind::None &&
            session.DisplayText() == displayBeforeBusy &&
            kqPieceCuesPreserved() &&
            session.KingQueenSwapActive(),
        "busy perimeter cue expires with selected value and K/Q cues intact", session);
    physical |= UINT64_C(1) << KQ_SQUARES.front();
    Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
          "raw player-end journey replaces the first K/Q piece");

    const bool returnedToCpuDifficulty =
        tap(runtime::VirtualLitchPhysicalButton::Left) &&
        tap(runtime::VirtualLitchPhysicalButton::Left) &&
        session.QuickSettingsSelectedIndex() ==
            modules::quick_settings_items::CPU_DIFFICULTY;
    CheckRawJourney(
        returnedToCpuDifficulty && session.DisplayText().empty() &&
            session.Advance(1021U) == VIRTUAL_LITCH_OK &&
            session.DisplayText() == "1",
        "CPU difficulty remains readable after its identity icon",
        session);
    Check(tap(runtime::VirtualLitchPhysicalButton::Right) &&
              tap(runtime::VirtualLitchPhysicalButton::Right),
          "raw P1 directions return to player-side placement");
    Check(tap(runtime::VirtualLitchPhysicalButton::X) &&
              !session.QuickSettingsActive(),
          "player-end journey closes Quick Setup");
    // Raw d8 already completed logical e1 after the player-end swap. Inspect
    // the untouched logical d8/e8 pair on the opposite hardware edge.
    const uint16_t d8OuterAtEntry = ColorEnergy(BoardLed(
        session.SemanticFrame(), 3U, hardware::SquareLedSlot::Square0));
    const uint16_t d8SeamAtEntry = ColorEnergy(BoardLed(
        session.SemanticFrame(), 3U, hardware::SquareLedSlot::Square2));
    const uint16_t e8SeamAtEntry = ColorEnergy(BoardLed(
        session.SemanticFrame(), 4U, hardware::SquareLedSlot::Square0));
    const uint16_t e8OuterAtEntry = ColorEnergy(BoardLed(
        session.SemanticFrame(), 4U, hardware::SquareLedSlot::Square2));
    CheckRawJourney(
        d8OuterAtEntry != 0U && d8SeamAtEntry != 0U &&
            e8SeamAtEntry != 0U && e8OuterAtEntry != 0U &&
            session.Advance(320U) == VIRTUAL_LITCH_OK &&
            (ColorEnergy(BoardLed(
                 session.SemanticFrame(), 3U,
                 hardware::SquareLedSlot::Square0)) != d8OuterAtEntry ||
             ColorEnergy(BoardLed(
                 session.SemanticFrame(), 3U,
                 hardware::SquareLedSlot::Square2)) != d8SeamAtEntry ||
             ColorEnergy(BoardLed(
                 session.SemanticFrame(), 4U,
                 hardware::SquareLedSlot::Square0)) != e8SeamAtEntry ||
             ColorEnergy(BoardLed(
                 session.SemanticFrame(), 4U,
                 hardware::SquareLedSlot::Square2)) != e8OuterAtEntry),
        "visible K/Q cue shuttles strongly across the adjacent d/e seam",
        session);
    PrintRawFrame(session, "K/Q reciprocal pair shuttle",
                  "swap each adjacent king and queen");

    for (std::size_t index = 1U; index < KQ_SQUARES.size(); ++index) {
        const uint8_t square = KQ_SQUARES[index];
        physical &= ~(UINT64_C(1) << square);
        Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
              "player-end K/Q lift advances correction");
        if (index == 1U) {
            CheckRawJourney(
                session.Advance(300U) == VIRTUAL_LITCH_OK &&
                    session.KingQueenSwapActive() &&
                    ColorEnergy(BoardLed(
                        session.SemanticFrame(), square,
                        hardware::SquareLedSlot::Square0)) != 0U &&
                    ColorEnergy(BoardLed(
                        session.SemanticFrame(), square,
                        hardware::SquareLedSlot::Square2)) != 0U,
                "first vacancy becomes a strong two-sided landing socket",
                session);
            PrintRawFrame(session, "K/Q first vacancy",
                          "place into the bright socket; its neighbor points inward");
        }
        physical |= UINT64_C(1) << square;
        Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
              "player-end K/Q replacement advances correction");
    }
    CheckRawJourney(
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
            !session.KingQueenSwapActive() &&
            StatusIsTurnIndicatorForOwner(session.SemanticFrame(), false) &&
            IsRedDominant(PhysicalBoardLed(
                session.PhysicalFrame(), PHYSICAL_E2_FROM_P2_END,
                hardware::SquareLedSlot::Piece)) &&
            IsBlueDominant(PhysicalBoardLed(
                session.PhysicalFrame(), LOGICAL_E2,
                hardware::SquareLedSlot::Piece)),
        "completed player-end correction keeps each palette on its physical end",
        session);
    PrintRawFrame(session, "player-end K/Q correction complete",
                  "active-side animation resumes on the selected end");

    const virtual_litch_color p1EndBeforeShortcut = PhysicalBoardLed(
        session.PhysicalFrame(), LOGICAL_E2,
        hardware::SquareLedSlot::Piece);
    const virtual_litch_color p2EndBeforeShortcut = PhysicalBoardLed(
        session.PhysicalFrame(), PHYSICAL_E2_FROM_P2_END,
        hardware::SquareLedSlot::Piece);
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Up) &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            !SameColor(
                PhysicalBoardLed(
                    session.PhysicalFrame(), LOGICAL_E2,
                    hardware::SquareLedSlot::Piece),
                p1EndBeforeShortcut) &&
            SameColor(
                PhysicalBoardLed(
                    session.PhysicalFrame(), PHYSICAL_E2_FROM_P2_END,
                    hardware::SquareLedSlot::Piece),
                p2EndBeforeShortcut),
        "near raw Up edits its assigned logical P2 palette after P1 moves to the far end",
        session);
    PrintRawFrame(session, "physical P1 color shortcut after end swap",
                  "P1-controls color changes here; the opposite end stays unchanged");

    CheckRawJourney(
        session.ObserveHallMask(
            INITIAL_OCCUPANCY &
            ~(UINT64_C(1) << PHYSICAL_E2_FROM_P2_END)) ==
                VIRTUAL_LITCH_OK &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::PieceLifted) &&
            session.State().selected_square == LOGICAL_E2 &&
            session.State().valid_destination_mask == E2_DESTINATIONS,
        "far P1 pawn lift maps to logical e2 with its normal move guidance",
        session);
}

void TestRawP2QuickSettingsOwnership() {
    test_support::BoundedHintSession sessionFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& session = sessionFixture.session;
    CheckRawJourney(
        session.Reset() == VIRTUAL_LITCH_OK &&
            session.SetButtonDown(
                VIRTUAL_LITCH_OWNER_P2,
                runtime::VirtualLitchPhysicalButton::X,
                true) == VIRTUAL_LITCH_OK &&
            session.Advance(20U) == VIRTUAL_LITCH_OK &&
            session.Advance(1000U) == VIRTUAL_LITCH_OK &&
            session.QuickSettingsActive() &&
            session.QuickSettingsOwner() == VIRTUAL_LITCH_OWNER_P2,
        "raw P2 timing preserves owner metadata through Quick Setup", session);
}

void TestRawPhysicalPaletteShortcutQuadrants() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    struct Quadrant {
        bool firstPlayerUsesP2End;
        bool boardRotate;
        const char* p1Message;
        const char* p2Message;
    };
    constexpr std::array<Quadrant, 4> QUADRANTS{{
        {false, false,
         "P1 shortcut edits its physical end at default placement/unrotated",
         "P2 shortcut edits its physical end at default placement/unrotated"},
        {true, false,
         "P1 shortcut edits its physical end at swapped placement/unrotated",
         "P2 shortcut edits its physical end at swapped placement/unrotated"},
        {false, true,
         "P1 shortcut edits its physical end at default placement/rotated",
         "P2 shortcut edits its physical end at default placement/rotated"},
        {true, true,
         "P1 shortcut edits its physical end at swapped placement/rotated",
         "P2 shortcut edits its physical end at swapped placement/rotated"},
    }};

    for (const Quadrant& quadrant : QUADRANTS) {
        // A raw control bank edits the palette of the pieces at that same raw
        // hardware edge, regardless of which logical player is assigned there.
        const uint8_t P1_END_PAWN = 52U;
        const uint8_t P2_END_PAWN = 11U;
        test_support::BoundedHintSession sessionFixture(
            ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& session = sessionFixture.session;
        Check(session.Reset() == VIRTUAL_LITCH_OK &&
                  session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                      VIRTUAL_LITCH_OK &&
                  session.Advance(200U) == VIRTUAL_LITCH_OK,
              "physical palette quadrant reaches the opening");
        const auto setDown = [&session](
                                 const uint8_t owner,
                                 const runtime::VirtualLitchPhysicalButton button,
                                 const bool down) {
            return session.SetButtonDown(owner, button, down) ==
                       VIRTUAL_LITCH_OK &&
                   session.Advance(20U) == VIRTUAL_LITCH_OK;
        };
        const auto tap = [&](const uint8_t owner,
                             const runtime::VirtualLitchPhysicalButton button) {
            if (session.QuickSettingsActive() &&
                (button == runtime::VirtualLitchPhysicalButton::Left ||
                 button == runtime::VirtualLitchPhysicalButton::Right) &&
                session.Advance(250U) != VIRTUAL_LITCH_OK) return false;
            return setDown(owner, button, true) && setDown(owner, button, false);
        };
        Check(setDown(VIRTUAL_LITCH_OWNER_P1,
                      runtime::VirtualLitchPhysicalButton::X,
                      true) &&
                  session.Advance(1000U) == VIRTUAL_LITCH_OK &&
                  setDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X,
                          false) &&
                  session.QuickSettingsActive(),
              "physical palette quadrant opens Quick Setup");
        Check(tap(VIRTUAL_LITCH_OWNER_P1,
                  runtime::VirtualLitchPhysicalButton::Right) &&
                  tap(VIRTUAL_LITCH_OWNER_P1,
                      runtime::VirtualLitchPhysicalButton::Right) &&
                  session.QuickSettingsSelectedIndex() ==
                      modules::quick_settings_items::FIRST_PLAYER,
              "physical palette quadrant selects player placement");
        if (quadrant.firstPlayerUsesP2End) {
            Check(tap(VIRTUAL_LITCH_OWNER_P1,
                      runtime::VirtualLitchPhysicalButton::Up) &&
                      session.FirstPlayerIsP2(),
                  "physical palette quadrant swaps player placement");
        }
        Check(tap(VIRTUAL_LITCH_OWNER_P1,
                  runtime::VirtualLitchPhysicalButton::Right) &&
                  tap(VIRTUAL_LITCH_OWNER_P1,
                      runtime::VirtualLitchPhysicalButton::Right) &&
                  tap(VIRTUAL_LITCH_OWNER_P1,
                      runtime::VirtualLitchPhysicalButton::Right) &&
                  session.QuickSettingsSelectedIndex() ==
                      modules::quick_settings_items::ROTATE,
              "physical palette quadrant selects literal rotation");
        if (quadrant.boardRotate) {
            Check(tap(VIRTUAL_LITCH_OWNER_P1,
                      runtime::VirtualLitchPhysicalButton::Up) &&
                      session.BoardRotate(),
                  "physical palette quadrant rotates board I/O");
        }
        Check(tap(VIRTUAL_LITCH_OWNER_P1,
                  runtime::VirtualLitchPhysicalButton::X) &&
                  !session.QuickSettingsActive() &&
                  session.Advance(1U) == VIRTUAL_LITCH_OK,
              "physical palette quadrant closes Quick Setup");

        const virtual_litch_color p1Before = PhysicalBoardLed(
            session.PhysicalFrame(),
            P1_END_PAWN,
            hardware::SquareLedSlot::Piece);
        const virtual_litch_color p2Before = PhysicalBoardLed(
            session.PhysicalFrame(),
            P2_END_PAWN,
            hardware::SquareLedSlot::Piece);
        Check(tap(VIRTUAL_LITCH_OWNER_P1,
                  runtime::VirtualLitchPhysicalButton::Up),
              "physical P1 palette shortcut tap is accepted");
        const virtual_litch_color p1After = PhysicalBoardLed(
            session.PhysicalFrame(),
            P1_END_PAWN,
            hardware::SquareLedSlot::Piece);
        const virtual_litch_color p2AfterP1 = PhysicalBoardLed(
            session.PhysicalFrame(),
            P2_END_PAWN,
            hardware::SquareLedSlot::Piece);
        Check(!SameColor(p1After, p1Before) &&
                  SameColor(p2AfterP1, p2Before),
              quadrant.p1Message);

        Check(tap(VIRTUAL_LITCH_OWNER_P2,
                  runtime::VirtualLitchPhysicalButton::Up),
              "physical P2 palette shortcut tap is accepted");
        const virtual_litch_color p1AfterP2 = PhysicalBoardLed(
            session.PhysicalFrame(),
            P1_END_PAWN,
            hardware::SquareLedSlot::Piece);
        const virtual_litch_color p2After = PhysicalBoardLed(
            session.PhysicalFrame(),
            P2_END_PAWN,
            hardware::SquareLedSlot::Piece);
        Check(SameColor(p1AfterP2, p1After) &&
                  !SameColor(p2After, p2AfterP1),
              quadrant.p2Message);
    }
}

void TestRawBoardRotateQuickSettingJourney() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr std::array<uint8_t, 4> KQ_SQUARES{3U, 4U, 59U, 60U};
    test_support::BoundedHintSession sessionFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& session = sessionFixture.session;
    CheckRawJourney(
        session.Reset() == VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(200U) == VIRTUAL_LITCH_OK,
        "raw rotate journey reaches the exact opening", session);

    const auto setDown = [&](const runtime::VirtualLitchPhysicalButton button,
                             const bool down) {
        return session.SetButtonDown(
                   VIRTUAL_LITCH_OWNER_P1, button, down) ==
                   VIRTUAL_LITCH_OK &&
               session.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    const auto tap = [&](const runtime::VirtualLitchPhysicalButton button) {
        if (session.QuickSettingsActive() &&
            (button == runtime::VirtualLitchPhysicalButton::Left ||
             button == runtime::VirtualLitchPhysicalButton::Right) &&
            session.Advance(250U) != VIRTUAL_LITCH_OK) return false;
        return setDown(button, true) && setDown(button, false);
    };
    CheckRawJourney(
        setDown(runtime::VirtualLitchPhysicalButton::X, true) &&
            session.Advance(1000U) == VIRTUAL_LITCH_OK &&
            setDown(runtime::VirtualLitchPhysicalButton::X, false) &&
            session.QuickSettingsActive(),
        "raw rotate journey opens Quick Setup", session);
    const uint64_t unrotatedMenuMask =
        HighIntensityPhysicalBoardSquareMask(session.PhysicalFrame());
    for (uint8_t item = 0U; item < 5U; ++item) {
        Check(tap(runtime::VirtualLitchPhysicalButton::Right),
              "raw right advances toward literal rotation");
    }
    CheckRawJourney(
        session.QuickSettingsSelectedIndex() ==
                modules::quick_settings_items::ROTATE &&
            tap(runtime::VirtualLitchPhysicalButton::Up) &&
            session.BoardRotate() && !session.FirstPlayerIsP2() &&
            session.DisplayText() == "1" &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.KingQueenSwapActive(),
        "BOARD_ROTATE keeps its distinct literal K/Q correction semantics",
        session);
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Down) &&
            !session.BoardRotate() &&
            session.Advance(50U) == VIRTUAL_LITCH_OK &&
            !session.KingQueenSwapActive(),
        "untouched literal rotation can be switched off without a busy lock",
        session);
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Up) &&
            session.BoardRotate() &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.KingQueenSwapActive(),
        "literal rotation starts one fresh K/Q correction after cancellation",
        session);
    PrintRawFrame(session, "literal board rotation enabled",
                  "return to first item and close Quick Setup");

    for (uint8_t item = 0U; item < 5U; ++item) {
        Check(tap(runtime::VirtualLitchPhysicalButton::Left),
              "raw left returns to the initial Quick Setup item");
    }
    const uint64_t rotatedMenuMask =
        HighIntensityPhysicalBoardSquareMask(session.PhysicalFrame());
    CheckRawJourney(
        session.QuickSettingsSelectedIndex() ==
                modules::quick_settings_items::CPU_DIFFICULTY &&
            unrotatedMenuMask != 0ULL &&
            unrotatedMenuMask != RotateSquareMask180(unrotatedMenuMask) &&
            rotatedMenuMask == RotateSquareMask180(unrotatedMenuMask),
        "BOARD_ROTATE alone rotates the same Quick Setup value 180 degrees",
        session);

    Check(tap(runtime::VirtualLitchPhysicalButton::X) &&
              !session.QuickSettingsActive(),
          "raw remapped X closes literal-rotation Quick Setup");
    CheckRawJourney(session.HomeStateSnapshot().kingQueenSwapPromptActive,
                    "raw rotation Home announces K/Q before the first lift", session);
    uint64_t physical = INITIAL_OCCUPANCY;
    for (const uint8_t square : KQ_SQUARES) {
        physical &= ~(UINT64_C(1) << square);
        Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
              "raw Hall lift advances K/Q correction");
        physical |= UINT64_C(1) << square;
        Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
              "raw Hall replacement advances K/Q correction");
    }
    CheckRawJourney(
        session.Advance(199U) == VIRTUAL_LITCH_OK &&
            session.HomeStateSnapshot().kingQueenSwapPromptActive,
        "raw rotation Home keeps K/Q active through completion dwell", session);
    PrintRawFrame(session, "K/Q completion dwell; Home kq=1",
                  "keep pieces still for the final millisecond");
    CheckRawJourney(
        session.Advance(1U) == VIRTUAL_LITCH_OK &&
            !session.HomeStateSnapshot().kingQueenSwapPromptActive &&
            !session.KingQueenSwapActive() &&
            StatusIsTurnIndicatorForOwner(session.SemanticFrame(), false),
        "raw Hall K/Q choreography returns literal rotation to Fresh Setup",
        session);

    const auto setDownFromP2 = [&](const runtime::VirtualLitchPhysicalButton button,
                                   const bool down) {
        return session.SetButtonDown(
                   VIRTUAL_LITCH_OWNER_P2, button, down) ==
                   VIRTUAL_LITCH_OK &&
               session.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    const auto tapFromP2 = [&](const runtime::VirtualLitchPhysicalButton button) {
        if (session.QuickSettingsActive() &&
            (button == runtime::VirtualLitchPhysicalButton::Left ||
             button == runtime::VirtualLitchPhysicalButton::Right) &&
            session.Advance(250U) != VIRTUAL_LITCH_OK) return false;
        return setDownFromP2(button, true) && setDownFromP2(button, false);
    };
    CheckRawJourney(
        setDownFromP2(runtime::VirtualLitchPhysicalButton::X, true) &&
            session.Advance(1000U) == VIRTUAL_LITCH_OK &&
            setDownFromP2(runtime::VirtualLitchPhysicalButton::X, false) &&
            session.QuickSettingsActive() &&
            session.QuickSettingsOwner() == VIRTUAL_LITCH_OWNER_P1,
        "literal rotation remaps P1 Quick Setup controls to the opposite physical end",
        session);
    const uint64_t rotateOnlyMenuMask =
        HighIntensityPhysicalBoardSquareMask(session.PhysicalFrame());
    Check(tapFromP2(runtime::VirtualLitchPhysicalButton::Right) &&
              tapFromP2(runtime::VirtualLitchPhysicalButton::Right) &&
              session.QuickSettingsSelectedIndex() ==
                  modules::quick_settings_items::FIRST_PLAYER &&
              tapFromP2(runtime::VirtualLitchPhysicalButton::Up) &&
              session.FirstPlayerIsP2() && session.BoardRotate(),
          "combined quadrant swaps player ends while literal rotation remains enabled");
    Check(tap(runtime::VirtualLitchPhysicalButton::Left) &&
              tap(runtime::VirtualLitchPhysicalButton::Left) &&
              session.QuickSettingsSelectedIndex() ==
                  modules::quick_settings_items::CPU_DIFFICULTY,
          "end assignment remaps logical P1 controls without remapping board squares");
    const uint64_t combinedMenuMask =
        HighIntensityPhysicalBoardSquareMask(session.PhysicalFrame());
    CheckRawJourney(
        rotateOnlyMenuMask == rotatedMenuMask &&
            combinedMenuMask == unrotatedMenuMask &&
            rotateOnlyMenuMask == RotateSquareMask180(combinedMenuMask),
        "all player-end and literal-rotation quadrants keep the menu upright for its physical control-bank user",
        session);
    Check(tap(runtime::VirtualLitchPhysicalButton::X) &&
              !session.QuickSettingsActive(),
          "combined quadrant closes Quick Setup from P1's remapped physical controls");
}

void TestAppBoardRotateMutationStartsKingQueenJourney() {
    constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
    constexpr std::array<uint8_t, 4> KQ{3U, 4U, 59U, 60U};
    auto config = ConfigFor(VIRTUAL_LITCH_HARDWARE_V3_3);
    config.brightness_scale_256 = 8U;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    const auto rotateBoard = [&](const uint16_t request, const bool enabled) {
        modules::ControlMutationOutcome outcome;
        return session.MutateSettingJson(request, "BOARD_ROTATE",
                   enabled ? "true" : "false", outcome) == VIRTUAL_LITCH_OK &&
               outcome.status == modules::ControlMutationStatus::Applied;
    };
    const auto physicalCue = [&](const uint8_t logical, const hardware::SquareLedSlot slot) {
        return PhysicalBoardLed(session.PhysicalFrame(),
            runtime::MapSquareIndex(logical, session.BoardRotate()), slot);
    };
    const auto checkpoint = [&](const char* stage, const char* next) {
        const auto source = physicalCue(60U, hardware::SquareLedSlot::Piece);
        const auto rail = physicalCue(60U, hardware::SquareLedSlot::Square0);
        std::printf("[KQ_V33_JOURNEY] stage=%s t=%u occupied=%016llx kq=%u "
            "phase=%u action=%u source=%u,%u,%u rail=%u,%u,%u "
            "scale=%u limited=%u current=%u display=\"%.*s\" next=%s\n",
            stage, session.State().now_ms,
            static_cast<unsigned long long>(session.State().physical_occupied_mask),
            session.KingQueenSwapActive(), session.State().interaction_phase,
            session.State().required_action, source.r, source.g, source.b,
            rail.r, rail.g, rail.b,
            session.PhysicalFrame().applied_brightness_scale_65535,
            session.PhysicalFrame().power_limited,
            session.PhysicalFrame().applied_led_current_ma,
            static_cast<int>(session.DisplayText().size()), session.DisplayText().data(), next);
    };
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              rotateBoard(91U, true) &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.KingQueenSwapActive(),
          "acknowledged app BOARD_ROTATE starts V3.3 K/Q correction");
    Check(ColorEnergy(physicalCue(60U, hardware::SquareLedSlot::Piece)) != 0U,
          "new K/Q prompt starts with an informative source-on phase");
    checkpoint("app-Applied-entry", "lift-the-four-flashing-K/Q-pieces");
    Check(session.Advance(700U) == VIRTUAL_LITCH_OK &&
              ColorEnergy(physicalCue(60U, hardware::SquareLedSlot::Piece)) == 0U &&
              ColorEnergy(physicalCue(60U, hardware::SquareLedSlot::Square0)) != 0U &&
              ColorEnergy(physicalCue(60U, hardware::SquareLedSlot::Square2)) != 0U,
          "near-dark V3.3 output preserves the bracket during the source-off phase");
    checkpoint("source-off-bracket-present", "swap-each-adjacent-K/Q-pair");
    Check(rotateBoard(92U, false) &&
              session.Advance(1U) == VIRTUAL_LITCH_OK && !session.KingQueenSwapActive(),
          "return to baseline cancels untouched V3.3 K/Q correction");
    checkpoint("baseline-canceled", "change-orientation-again-if-needed");
    Check(rotateBoard(93U, true) &&
              session.Advance(1U) == VIRTUAL_LITCH_OK && session.KingQueenSwapActive(),
          "one new setting mutation restarts one K/Q prompt");

    uint64_t physical = OPENING & ~(UINT64_C(1) << 8U);
    Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
              session.KingQueenSwapActive() &&
              session.ObserveHallMask(physical | (UINT64_C(1) << 35U)) == VIRTUAL_LITCH_OK &&
              session.KingQueenSwapActive() && session.UndoHistoryMoveCount() == 0U &&
              session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK,
          "unrelated lift/wrong placement cannot complete K/Q; restoration recovers the invitation");
    checkpoint("unrelated-placement-recovered", "lift-a-flashing-K/Q-piece");
    physical = OPENING;
    for (const uint8_t square : KQ) {
        physical &= ~(UINT64_C(1) << square);
        Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
              "real Hall lift advances the V3.3 K/Q modal");
        if (square == KQ.front()) {
            const uint8_t logicalVacancy = static_cast<uint8_t>(63U - square);
            const auto socket = physicalCue(logicalVacancy, hardware::SquareLedSlot::Square0);
            Check(session.Advance(700U) == VIRTUAL_LITCH_OK &&
                      SameColor(socket, physicalCue(logicalVacancy, hardware::SquareLedSlot::Square0)) &&
                      ColorEnergy(socket) != 0U &&
                      ColorEnergy(physicalCue(logicalVacancy, hardware::SquareLedSlot::Piece)) == 0U,
                  "V3.3 partial-swap socket stays steady while the occupied source blinks");
            checkpoint("partial-swap-source-off", "place-into-the-steady-empty-socket");
        }
        physical |= UINT64_C(1) << square;
        Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
              "real Hall return clears each completed K/Q square");
    }
    Check(session.Advance(199U) == VIRTUAL_LITCH_OK && session.KingQueenSwapActive() &&
              ColorEnergy(physicalCue(60U, hardware::SquareLedSlot::Square0)) == 0U,
          "zero-pending V3.3 completion dwell stays clear through 199 ms");
    checkpoint("completion-dwell", "keep-pieces-still-one-more-millisecond");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK && !session.KingQueenSwapActive() &&
              StatusIsTurnIndicatorForOwner(session.SemanticFrame(), false),
          "stable V3.3 completion returns to the ordinary Turn owner");
    checkpoint("complete", "play-the-first-move");
}

void TestV33CastlingSteadyRookDestinationJourney() {
    auto config = ConfigFor(VIRTUAL_LITCH_HARDWARE_V3_3);
    config.brightness_scale_256 = 8U;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    constexpr uint64_t POSITION = (UINT64_C(1) << 4U) |
        (UINT64_C(1) << 60U) | (UINT64_C(1) << 63U);
    modules::ControlMutationOutcome outcome;
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(UINT64_C(0xFFFF00000000FFFF)) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.LoadExternalFen("4k3/8/8/8/8/8/8/4K2R w K - 0 1") &&
              session.ObserveHallMask(POSITION) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.MutateSettingJson(94U, "WHITE_SQUARES_ON", "false", outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "V3.3 castle journey loads a legal rules fixture and real matching Hall occupancy");
    uint64_t physical = POSITION & ~(UINT64_C(1) << 60U);
    Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(physical | (UINT64_C(1) << 62U)) == VIRTUAL_LITCH_OK &&
              session.State().required_action ==
                  static_cast<uint8_t>(runtime::RequiredPhysicalAction::LiftSecondary),
          "raw king lift/place hands castling to the authoritative rook prompt");
    physical |= UINT64_C(1) << 62U;
    const auto socket = PhysicalBoardLed(session.PhysicalFrame(), 61U,
                                        hardware::SquareLedSlot::Square0);
    const auto seatedKing = PhysicalBoardLed(session.PhysicalFrame(), 62U,
                                            hardware::SquareLedSlot::Piece);
    const uint32_t untilSourceOff = (1500U - session.State().now_ms % 800U) % 800U;
    Check(session.Advance(untilSourceOff) == VIRTUAL_LITCH_OK && ColorEnergy(socket) != 0U &&
              SameColor(socket, PhysicalBoardLed(session.PhysicalFrame(), 61U,
                                                hardware::SquareLedSlot::Square0)) &&
              SameColor(seatedKing, PhysicalBoardLed(session.PhysicalFrame(), 62U,
                                                    hardware::SquareLedSlot::Piece)) &&
              ColorEnergy(PhysicalBoardLed(session.PhysicalFrame(), 63U,
                                          hardware::SquareLedSlot::Piece)) == 0U,
          "near-dark V3.3 rook destination stays steady while only the waiting rook blinks");
    PrintRawFrame(session, "V3.3 king seated; rook destination steady",
                  "lift-h1-rook-and-place-in-steady-f1-socket");
    physical &= ~(UINT64_C(1) << 63U);
    Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
              session.State().required_action ==
                  static_cast<uint8_t>(runtime::RequiredPhysicalAction::PlaceSecondary) &&
              ColorEnergy(PhysicalBoardLed(session.PhysicalFrame(), 61U,
                                          hardware::SquareLedSlot::Square0)) != 0U,
          "raw rook lift keeps the same castling landing socket");
    physical |= UINT64_C(1) << 61U;
    Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
              session.Advance(3000U) == VIRTUAL_LITCH_OK &&
              session.UndoHistoryMoveCount() == 1U &&
              session.State().correction_mask == 0ULL,
          "raw rook seating acknowledges one complete castle and clears its invitation");
    PrintRawFrame(session, "V3.3 castle complete", "play-Black-move");
}

void TestLiteralPhysicalTurnKeepsPlayerColorJourney() {
    constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
    constexpr std::array<std::array<uint8_t, 2>, 2> KQ_PAIRS{{
        {{3U, 4U}}, {{59U, 60U}},
    }};
    // These are hardware coordinates, independent of the production mapping.
    // Turning the board in the room moves a player's square to index 63-sq.
    for (const bool oppositePlacement : {false, true}) {
        for (const bool initiallyRotated : {false, true}) {
            test_support::BoundedHintSession sessionFixture(
                ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& session = sessionFixture.session;
            CheckRawJourney(session.Reset() == VIRTUAL_LITCH_OK &&
                                session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
                                session.Advance(2200U) == VIRTUAL_LITCH_OK,
                            "literal-turn color journey starts playable", session);
            const auto setDown = [&](const uint8_t bank,
                                     const runtime::VirtualLitchPhysicalButton button,
                                     const bool down) {
                return session.SetButtonDown(bank, button, down) == VIRTUAL_LITCH_OK &&
                       session.Advance(20U) == VIRTUAL_LITCH_OK;
            };
            const auto tap = [&](const uint8_t bank,
                                 const runtime::VirtualLitchPhysicalButton button) {
                if (session.QuickSettingsActive() &&
                    (button == runtime::VirtualLitchPhysicalButton::Left ||
                     button == runtime::VirtualLitchPhysicalButton::Right) &&
                    session.Advance(250U) != VIRTUAL_LITCH_OK) return false;
                return setDown(bank, button, true) && setDown(bank, button, false);
            };
            const auto setQuickChoice = [&](const uint8_t item, const bool enabled) {
                // Open at the bank currently owned by logical P1. The existing
                // raw adapter supplies production debounce and owner transforms.
                const uint8_t bank = session.BoardRotate() != session.FirstPlayerIsP2()
                                         ? VIRTUAL_LITCH_OWNER_P2
                                         : VIRTUAL_LITCH_OWNER_P1;
                bool ok = setDown(bank, runtime::VirtualLitchPhysicalButton::X, true) &&
                          session.Advance(1000U) == VIRTUAL_LITCH_OK &&
                          setDown(bank, runtime::VirtualLitchPhysicalButton::X, false) &&
                          session.QuickSettingsActive();
                for (uint8_t step = 0U;
                     ok && session.QuickSettingsSelectedIndex() != item && step < 12U;
                     ++step) {
                    ok = tap(bank, runtime::VirtualLitchPhysicalButton::Right);
                }
                ok = ok && session.QuickSettingsSelectedIndex() == item &&
                     tap(bank, enabled ? runtime::VirtualLitchPhysicalButton::Up
                                       : runtime::VirtualLitchPhysicalButton::Down);
                modules::ControlMutationOutcome outcome;
                ok = ok && session.TryGetLastBoardControlOutcome(outcome) &&
                     outcome.status == modules::ControlMutationStatus::Applied &&
                     outcome.reason == modules::ControlMutationReason::None;
                return ok && tap(bank, runtime::VirtualLitchPhysicalButton::X) &&
                       !session.QuickSettingsActive() &&
                       session.Advance(1U) == VIRTUAL_LITCH_OK;
            };
            const auto completeKingQueen = [&]() {
                bool ok = session.KingQueenSwapActive();
                uint64_t hall = OPENING;
                for (const auto& pair : KQ_PAIRS) {
                    for (const uint8_t square : pair) {
                        hall &= ~(UINT64_C(1) << square);
                        ok = session.ObserveHallMask(hall) == VIRTUAL_LITCH_OK && ok;
                    }
                    // Exchange the two pieces while both adjacent holes are empty.
                    for (const uint8_t square : pair) {
                        hall |= UINT64_C(1) << square;
                        ok = session.ObserveHallMask(hall) == VIRTUAL_LITCH_OK && ok;
                    }
                }
                return session.Advance(2200U) == VIRTUAL_LITCH_OK && ok &&
                       !session.KingQueenSwapActive() &&
                       session.State().interaction_phase == static_cast<uint8_t>(
                           runtime::ChessInteractionPhase::Idle) &&
                       session.State().correction_mask == 0ULL;
            };
            if (oppositePlacement) {
                CheckRawJourney(setQuickChoice(modules::quick_settings_items::FIRST_PLAYER, true) &&
                                    completeKingQueen(),
                                "literal-turn fixture completes opposite player placement", session);
            }
            if (initiallyRotated) {
                CheckRawJourney(setQuickChoice(modules::quick_settings_items::ROTATE, true) &&
                                    completeKingQueen(),
                                "literal-turn reverse fixture completes initial rotation", session);
            }
            const bool beforeOpposite = initiallyRotated != oppositePlacement;
            const uint8_t p1BeforeSquare = beforeOpposite ? 11U : 52U;
            const uint8_t p2BeforeSquare = static_cast<uint8_t>(63U - p1BeforeSquare);
            const auto p1Before = PhysicalBoardLed(session.PhysicalFrame(), p1BeforeSquare,
                                                    hardware::SquareLedSlot::Piece);
            const auto p2Before = PhysicalBoardLed(session.PhysicalFrame(), p2BeforeSquare,
                                                    hardware::SquareLedSlot::Piece);
            CheckRawJourney((IsBlueDominant(p1Before) && IsRedDominant(p2Before)) ||
                                (IsRedDominant(p1Before) && IsBlueDominant(p2Before)),
                            "literal-turn baseline has distinguishable player hues", session);
            CheckRawJourney(setQuickChoice(modules::quick_settings_items::ROTATE, !initiallyRotated) &&
                                session.KingQueenSwapActive() && completeKingQueen(),
                            "literal180 turn acknowledges rotation and completes K/Q exchange", session);
            const uint8_t p1AfterSquare = p2BeforeSquare;
            const uint8_t p2AfterSquare = p1BeforeSquare;
            const auto p1After = PhysicalBoardLed(session.PhysicalFrame(), p1AfterSquare,
                                                   hardware::SquareLedSlot::Piece);
            const auto p2After = PhysicalBoardLed(session.PhysicalFrame(), p2AfterSquare,
                                                   hardware::SquareLedSlot::Piece);
            const auto keepsHue = [](const virtual_litch_color before,
                                     const virtual_litch_color after) {
                return (IsBlueDominant(before) && IsBlueDominant(after)) ||
                       (IsRedDominant(before) && IsRedDominant(after));
            };
            CheckRawJourney(keepsHue(p1Before, p1After) && keepsHue(p2Before, p2After),
                            "literal180 turn keeps both players' colors at their new hardware sides", session);
            bool turnKeepsHue = false;
            const bool p1FarAfter = (!initiallyRotated) != oppositePlacement;
            const uint8_t firstEdgePixel = p1FarAfter ? 0U : 16U;
            for (uint8_t tick = 0U; tick < 8U; ++tick) {
                Check(session.Advance(50U) == VIRTUAL_LITCH_OK,
                      "literal-turn physical Turn indicator advances");
                for (uint8_t index = firstEdgePixel; index < firstEdgePixel + 8U; ++index) {
                    turnKeepsHue = turnKeepsHue || keepsHue(
                        p1Before, session.PhysicalFrame().pixels[hardware::BOARD_LED_COUNT + index]);
                }
            }
            CheckRawJourney(turnKeepsHue,
                            "literal180 turn keeps active player's hue on their new physical home edge", session);
            const uint8_t physicalE4 = p1AfterSquare == 52U ? 36U : 27U;
            uint64_t hall = OPENING & ~(UINT64_C(1) << p1AfterSquare);
            CheckRawJourney(session.ObserveHallMask(hall) == VIRTUAL_LITCH_OK &&
                                session.State().valid_destination_mask ==
                                    ((UINT64_C(1) << 44U) | (UINT64_C(1) << 36U)),
                            "literal-turn player can lift e2 from their new hardware side", session);
            hall |= UINT64_C(1) << physicalE4;
            CheckRawJourney(session.ObserveHallMask(hall) == VIRTUAL_LITCH_OK &&
                                session.Advance(2100U) == VIRTUAL_LITCH_OK &&
                                session.State().expected_occupied_mask ==
                                    ((OPENING & ~(UINT64_C(1) << 52U)) | (UINT64_C(1) << 36U)) &&
                                session.State().correction_mask == 0ULL,
                            "literal-turn K/Q exit reaches a settled playable e2e4 destination", session);
            std::printf("[LITERAL_ROTATION_COLOR] first_player=%u rotate=%u->%u "
                        "P1hardware=%u->%u before_rgb=%u/%u/%u after_rgb=%u/%u/%u "
                        "turn_hue_preserved=%u KQ=completed next=e2e4\n",
                        oppositePlacement ? 2U : 1U, unsigned(initiallyRotated),
                        unsigned(!initiallyRotated), p1BeforeSquare, p1AfterSquare,
                        p1Before.r, p1Before.g, p1Before.b, p1After.r, p1After.g, p1After.b,
                        unsigned(turnKeepsHue));
        }
    }
}

void PrintRawFrame(const runtime::VirtualLitchSession& session,
                   const char* step,
                   const char* next) {
    const auto& state = session.State();
    const auto& semantic = session.SemanticFrame();
    const auto& physical = session.PhysicalFrame();
    modules::ControlMutationOutcome outcome;
    const bool hasOutcome = session.TryGetLastBoardControlOutcome(outcome);
    const uint64_t highIntensityMask =
        HighIntensityPhysicalBoardSquareMask(physical);
    std::printf(
        "[RAW_UX_JOURNEY] %s | t=%u phase=%s display=\"%.*s\" "
        "quick=%u owner=%u item=%u player_end=%u rotate=%u "
        "outcome=%s/%s "
        "physical=%016llx expected=%016llx valid=%016llx "
        "correction=%016llx high=%016llx status=%s | board_leds=",
        step,
        static_cast<unsigned>(state.now_ms),
        runtime::ChessInteractionPhaseToString(
            static_cast<runtime::ChessInteractionPhase>(
                state.interaction_phase)),
        static_cast<int>(session.DisplayText().size()),
        session.DisplayText().data(),
        static_cast<unsigned>(session.QuickSettingsActive() ? 1U : 0U),
        static_cast<unsigned>(session.QuickSettingsOwner()),
        static_cast<unsigned>(session.QuickSettingsSelectedIndex()),
        static_cast<unsigned>(session.FirstPlayerIsP2() ? 2U : 1U),
        static_cast<unsigned>(session.BoardRotate() ? 1U : 0U),
        hasOutcome ? modules::ControlMutationStatusToString(outcome.status)
                   : "none",
        hasOutcome ? modules::ControlMutationReasonToString(outcome.reason)
                   : "none",
        static_cast<unsigned long long>(state.physical_occupied_mask),
        static_cast<unsigned long long>(state.expected_occupied_mask),
        static_cast<unsigned long long>(state.valid_destination_mask),
        static_cast<unsigned long long>(state.correction_mask),
        static_cast<unsigned long long>(highIntensityMask),
        StatusShapeName(semantic));
    uint64_t focusMask =
        (state.physical_occupied_mask ^ state.expected_occupied_mask) |
        state.valid_destination_mask | state.correction_mask |
        state.human_illegal_mask | state.cpu_illegal_mask;
    if (state.selected_square < 64U) {
        focusMask |= UINT64_C(1) << state.selected_square;
    }
    if (session.SetupDuckCandidateSquare() < 64U) {
        focusMask |= UINT64_C(1) << session.SetupDuckCandidateSquare();
    }
    if (session.DuckSquare() < 64U) {
        focusMask |= UINT64_C(1) << session.DuckSquare();
    }
    if (focusMask == 0ULL) {
        focusMask = (UINT64_C(1) << 4U) | (UINT64_C(1) << 60U);
    }
    bool first = true;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((focusMask & (UINT64_C(1) << square)) == 0ULL) {
            continue;
        }
        const auto a = BoardLed(
            semantic, square, hardware::SquareLedSlot::Square0);
        const auto b = BoardLed(
            semantic, square, hardware::SquareLedSlot::Piece);
        const auto c = BoardLed(
            semantic, square, hardware::SquareLedSlot::Square2);
        const auto physicalA = physical.pixels[hardware::LedIndexForSquare(
            square, hardware::SquareLedSlot::Square0)];
        const auto physicalB = physical.pixels[hardware::LedIndexForSquare(
            square, hardware::SquareLedSlot::Piece)];
        const auto physicalC = physical.pixels[hardware::LedIndexForSquare(
            square, hardware::SquareLedSlot::Square2)];
        std::printf(
            "%s%u:%02x%02x%02x/%02x%02x%02x/%02x%02x%02x>"
            "%02x%02x%02x%02x/%02x%02x%02x%02x/"
            "%02x%02x%02x%02x",
            first ? "" : ",",
            static_cast<unsigned>(square),
            a.r, a.g, a.b,
            b.r, b.g, b.b,
            c.r, c.g, c.b,
            physicalA.r, physicalA.g, physicalA.b, physicalA.w,
            physicalB.r, physicalB.g, physicalB.b, physicalB.w,
            physicalC.r, physicalC.g, physicalC.b, physicalC.w);
        first = false;
    }
    std::printf(" | status_leds=");
    first = true;
    for (uint8_t index = 0U;
         index < VIRTUAL_LITCH_STATUS_LED_COUNT;
         ++index) {
        const auto color = semantic.status[index];
        if (ColorEnergy(color) == 0U) {
            continue;
        }
        const auto physicalColor =
            physical.pixels[hardware::BOARD_LED_COUNT + index];
        std::printf(
            "%s%u:%02x%02x%02x>%02x%02x%02x%02x",
            first ? "" : ",",
            static_cast<unsigned>(index),
            color.r, color.g, color.b,
            physicalColor.r, physicalColor.g,
            physicalColor.b, physicalColor.w);
        first = false;
    }
    std::printf(" | next=\"%s\"\n", next);
}

void CheckRawJourney(const bool condition,
                     const char* message,
                     const runtime::VirtualLitchSession& session) {
    if (!condition) {
        PrintRawFrame(session, message, "inspect failed frame");
    }
    Check(condition, message);
}

void TestRawSetupFailureRegressionJourney() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E3 = 44U;
    constexpr uint8_t E4 = 36U;
    constexpr uint8_t E7 = 12U;
    constexpr uint8_t ORDINARY_EXTRA_D4 = 35U;
    constexpr uint8_t ORDINARY_EXTRA_C3 = 42U;
    constexpr uint8_t DUCK_C6 = 18U;
    constexpr uint64_t E2_DESTINATIONS =
        (UINT64_C(1) << E3) | (UINT64_C(1) << E4);
    test_support::BoundedHintSession sessionFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& session = sessionFixture.session;
    CheckRawJourney(
        session.Reset() == VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(
                INITIAL_OCCUPANCY & ~(UINT64_C(1) << E2)) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(200U) == VIRTUAL_LITCH_OK &&
            StatusIsTurnIndicatorForOwner(
                session.SemanticFrame(), false),
        "raw 31-piece Fresh Setup points toward physical White owner",
        session);
    const virtual_litch_semantic_frame arranging =
        session.SemanticFrame();
    CheckRawJourney(
        session.Advance(80U) == VIRTUAL_LITCH_OK &&
            session.SemanticFrame().status_hash != arranging.status_hash &&
            StatusIsTurnIndicatorForOwner(
                session.SemanticFrame(), false),
        "raw Fresh Setup keeps the White-owner perimeter motion smooth",
        session);
    PrintRawFrame(session, "31-piece arranging", "place physical e2");

    CheckRawJourney(
        session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(199U) == VIRTUAL_LITCH_OK &&
            session.DisplayText().empty() &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.DisplayText().empty() &&
            StatusHasSetupCompletionSweep(session.SemanticFrame()),
        "ordinary setup completion acknowledges both ends without a board icon",
        session);
    PrintRawFrame(session, "ordinary setup complete", "wait for Ready expiry");
    CheckRawJourney(
        session.Advance(1801U) == VIRTUAL_LITCH_OK &&
            session.DisplayText().empty() &&
            StatusIsTurnIndicatorForOwner(
                session.SemanticFrame(), false),
        "setup acknowledgement expires into ordinary play", session);

    const auto setXDown = [&](const bool down) {
        return session.SetButtonDown(
                   VIRTUAL_LITCH_OWNER_P1,
                   runtime::VirtualLitchPhysicalButton::X,
                   down) == VIRTUAL_LITCH_OK &&
               session.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    modules::ControlMutationOutcome outcome;
    CheckRawJourney(
        setXDown(true) && setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Applied,
        "first raw X tap dismisses only the setup assist", session);
    const virtual_litch_semantic_frame beforeUnavailable =
        session.SemanticFrame();
    CheckRawJourney(
        setXDown(true) && setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Rejected &&
            outcome.reason == modules::ControlMutationReason::NoHistory &&
            session.DisplayText().empty() &&
            SameBoardPixels(
                session.SemanticFrame(), beforeUnavailable) &&
            StatusIsTurnIndicatorForOwner(
                session.SemanticFrame(), false) &&
            session.SemanticFrame().status_hash !=
                beforeUnavailable.status_hash,
        "raw no-history X shows only the subtle unavailable cue, never BU/BUSY",
        session);
    PrintRawFrame(session, "Undo unavailable", "wait 480 ms");
    CheckRawJourney(
        session.Advance(480U) == VIRTUAL_LITCH_OK &&
            session.DisplayText().empty() &&
            StatusIsTurnIndicatorForOwner(
                session.SemanticFrame(), false),
        "raw unavailable cue expires back to Turn", session);

    CheckRawJourney(
        session.ObserveHallMask(
            INITIAL_OCCUPANCY & ~(UINT64_C(1) << E2)) ==
                VIRTUAL_LITCH_OK &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::PieceLifted) &&
            session.State().valid_destination_mask == E2_DESTINATIONS,
        "raw White lift reaches production valid-move guidance", session);
    PrintRawFrame(session, "White e2 lifted", "return e2; lift Black e7");
    CheckRawJourney(
        session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(
                INITIAL_OCCUPANCY & ~(UINT64_C(1) << E7)) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(modules::CORRECTION_STRONG_MS) == VIRTUAL_LITCH_OK &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::Correction) &&
            session.State().correction_mask == (UINT64_C(1) << E7) &&
            StatusIsFullRedFault(session.SemanticFrame()),
        "raw wrong-side lift reaches production correction", session);
    PrintRawFrame(session, "Black e7 rejected", "return e7; add ordinary d4 extra");

    CheckRawJourney(
        session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(
                INITIAL_OCCUPANCY |
                (UINT64_C(1) << ORDINARY_EXTRA_D4) |
                (UINT64_C(1) << ORDINARY_EXTRA_C3)) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(modules::CORRECTION_STRONG_MS) == VIRTUAL_LITCH_OK &&
            session.SetupDuckCandidateSquare() ==
                modules::INVALID_DUCK_SQUARE &&
            session.State().human_illegal_mask ==
                ((UINT64_C(1) << ORDINARY_EXTRA_D4) |
                 (UINT64_C(1) << ORDINARY_EXTRA_C3)) &&
            StatusIsFullRedFault(session.SemanticFrame()),
        "raw multiple extras are rainbow correction and cannot masquerade as Vex",
        session);
    PrintRawFrame(session, "ordinary d4/c3 extras", "remove both; add c6 Vex candidate");

    CheckRawJourney(
        session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(
                INITIAL_OCCUPANCY |
                (UINT64_C(1) << DUCK_C6)) ==
                VIRTUAL_LITCH_OK &&
            session.SetupDuckCandidateSquare() == DUCK_C6 &&
            session.GameVariant() == modules::GameVariant::Undecided &&
            IsExactYellow(BoardLed(
                session.SemanticFrame(), DUCK_C6,
                hardware::SquareLedSlot::Piece)) &&
            session.Advance(200U) == VIRTUAL_LITCH_OK &&
            session.GameVariant() == modules::GameVariant::Duck &&
            session.DuckSquare() == DUCK_C6 &&
            session.DisplayText().empty() &&
            IsExactYellow(BoardLed(
                session.SemanticFrame(), DUCK_C6,
                hardware::SquareLedSlot::Piece)) &&
            StatusIsTurnIndicatorForOwner(
                session.SemanticFrame(), false),
        "raw eligible Duck candidate commits without adding a board icon",
        session);
    PrintRawFrame(session, "Duck committed", "remove c6 before first move");
    CheckRawJourney(
        session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            session.GameVariant() == modules::GameVariant::Undecided &&
            session.DuckSquare() == modules::INVALID_DUCK_SQUARE &&
            session.SetupDuckCandidateSquare() ==
                modules::INVALID_DUCK_SQUARE &&
            session.DisplayText().empty() &&
            StatusIsTurnIndicatorForOwner(
                session.SemanticFrame(), false),
        "raw pre-move Duck removal restores undecided Classic-ready setup",
        session);
    PrintRawFrame(session, "Duck removed", "journey complete");
}

void TestRawPhysicalUndoCompletesJourney() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t A2 = 48U;
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;
    const uint64_t e2Lifted =
        INITIAL_OCCUPANCY & ~(UINT64_C(1) << E2);
    const uint64_t e2e4Occupancy =
        e2Lifted | (UINT64_C(1) << E4);

    test_support::BoundedHintSession sessionFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& session = sessionFixture.session;
    CheckRawJourney(
        session.Reset() == VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(200U) == VIRTUAL_LITCH_OK &&
            session.Advance(1801U) == VIRTUAL_LITCH_OK,
        "raw physical-Undo journey reaches Play Ready", session);

    CheckRawJourney(
        session.ObserveHallMask(e2Lifted) == VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(e2e4Occupancy) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(2000U) == VIRTUAL_LITCH_OK &&
            session.State().physical_occupied_mask == e2e4Occupancy &&
            session.State().expected_occupied_mask == e2e4Occupancy &&
            session.State().side_to_move_black != 0U,
        "raw physical-Undo journey commits e2-e4", session);

    const auto setXDown = [&](const bool down) {
        return session.SetButtonDown(
                   VIRTUAL_LITCH_OWNER_P1,
                   runtime::VirtualLitchPhysicalButton::X,
                   down) == VIRTUAL_LITCH_OK &&
               session.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    modules::ControlMutationOutcome outcome;
    CheckRawJourney(
        setXDown(true) && setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Applied &&
            outcome.actionState ==
                modules::ControlActionState::UndoPending &&
            session.State().physical_occupied_mask == e2e4Occupancy &&
            session.State().expected_occupied_mask == e2e4Occupancy &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::AwaitingAction),
        "raw physical X starts the inverse e4-e2 Undo plan", session);

    CheckRawJourney(
        session.DisplayText().empty() &&
            BrightNeutralSquareFaceCount(session.SemanticFrame()) == 0U &&
            ColorEnergy(BoardLed(
                session.SemanticFrame(), E2,
                hardware::SquareLedSlot::Square0)) > 0U &&
            ColorEnergy(BoardLed(
                session.SemanticFrame(), E4,
                hardware::SquareLedSlot::Piece)) > 0U &&
            !StatusIsFullRedFault(session.SemanticFrame()),
        "raw Undo presents return/current guidance without a board letter",
        session);
    std::printf(
        "[UNDO_SPATIAL_JOURNEY] input=P1-X after e2-e4 | "
        "authoritative=UndoPending | board=return:e2,current:e4,"
        "neutral_letter_pixels=%zu,status=P1-turn | outcome=accepted | "
        "next=lift-e4/place-e2\n",
        BrightNeutralSquareFaceCount(session.SemanticFrame()));

    const uint64_t unrelatedA2Lifted =
        e2e4Occupancy & ~(UINT64_C(1) << A2);
    CheckRawJourney(
        session.ObserveHallMask(unrelatedA2Lifted) == VIRTUAL_LITCH_OK &&
            session.IsUndoActionPending() &&
            IsRedDominant(BoardLed(
                session.SemanticFrame(), A2,
                hardware::SquareLedSlot::Square0)) &&
            IsRedDominant(BoardLed(
                session.SemanticFrame(), A2,
                hardware::SquareLedSlot::Square2)) &&
            BrightNeutralSquareFaceCount(session.SemanticFrame()) == 0U &&
            !StatusIsFullRedFault(session.SemanticFrame()),
        "raw Undo rejects an unrelated lift locally without a whole-board fault",
        session);
    std::printf(
        "[UNDO_SPATIAL_JOURNEY] input=lift-unrelated-a2 | "
        "authoritative=UndoPending | board=local-red:a2,"
        "neutral_letter_pixels=%zu,status=Undo-owner | outcome=rejected | "
        "next=replace-a2\n",
        BrightNeutralSquareFaceCount(session.SemanticFrame()));

    CheckRawJourney(
        session.ObserveHallMask(e2e4Occupancy) == VIRTUAL_LITCH_OK &&
            session.IsUndoActionPending() &&
            !IsRedDominant(BoardLed(
                session.SemanticFrame(), A2,
                hardware::SquareLedSlot::Square0)) &&
            BrightNeutralSquareFaceCount(session.SemanticFrame()) == 0U &&
            !StatusIsFullRedFault(session.SemanticFrame()),
        "raw Undo recovers after the unrelated piece is replaced",
        session);
    std::printf(
        "[UNDO_SPATIAL_JOURNEY] input=replace-a2 | "
        "authoritative=UndoPending | board=return:e2,current:e4,"
        "neutral_letter_pixels=%zu,status=Undo-owner | outcome=recovered | "
        "next=cancel-or-lift-e4\n",
        BrightNeutralSquareFaceCount(session.SemanticFrame()));

    const bool cancelPressed =
        setXDown(true) && session.Advance(1U) == VIRTUAL_LITCH_OK;
    const bool cancelOutcomeRead =
        session.TryGetLastBoardControlOutcome(outcome);
    CheckRawJourney(
        cancelPressed && cancelOutcomeRead &&
            outcome.status == modules::ControlMutationStatus::Applied &&
            !session.IsUndoActionPending() &&
            session.State().expected_occupied_mask == e2e4Occupancy &&
            !StatusIsFullRedFault(session.SemanticFrame()),
        "raw second X press cancels Undo immediately and restores forward play",
        session);
    std::printf(
        "[UNDO_SPATIAL_JOURNEY] input=P1-X press | "
        "authoritative=UndoPending->Idle | board=forward:e2-e4,status=P2-turn | "
        "outcome=cancelled | next=release-X\n");

    CheckRawJourney(
        setXDown(false) && setXDown(true) && setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Applied &&
            outcome.actionState == modules::ControlActionState::UndoPending &&
            session.IsUndoActionPending() &&
            BrightNeutralSquareFaceCount(session.SemanticFrame()) == 0U &&
            !StatusIsFullRedFault(session.SemanticFrame()),
        "raw physical X can restart Undo after cancellation",
        session);

    CheckRawJourney(
        session.ObserveHallMask(e2Lifted) == VIRTUAL_LITCH_OK &&
            session.IsUndoActionPending() &&
            BrightNeutralSquareFaceCount(session.SemanticFrame()) == 0U &&
            ColorEnergy(BoardLed(
                session.SemanticFrame(), E2,
                hardware::SquareLedSlot::Square0)) > 0U &&
            !StatusIsFullRedFault(session.SemanticFrame()),
        "raw Undo keeps its spatial destination calm while the piece is held",
        session);
    std::printf(
        "[UNDO_SPATIAL_JOURNEY] input=lift-e4 | "
        "authoritative=UndoPending | board=return:e2,"
        "neutral_letter_pixels=%zu,status=no-red | outcome=in-progress | "
        "next=place-e2\n",
        BrightNeutralSquareFaceCount(session.SemanticFrame()));

    CheckRawJourney(
        session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.State().physical_occupied_mask == INITIAL_OCCUPANCY &&
            session.State().expected_occupied_mask == INITIAL_OCCUPANCY &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::Idle) &&
            session.State().side_to_move_black == 0U &&
            StatusIsTurnIndicatorForOwner(
                session.SemanticFrame(), false) &&
            !StatusIsFullRedFault(session.SemanticFrame()),
        "raw physical e4-e2 completes Undo and restores White to move",
        session);

    std::printf(
        "[UNDO_COMPLETE_JOURNEY] input=e2-e4; X tap; e4-e2 | "
        "authoritative=UndoPending->Idle | outcome=completed | "
        "board=physical/expected:%016llx status=P1-turn-indicator | "
        "expiry=none cancellation=not-required\n",
        static_cast<unsigned long long>(INITIAL_OCCUPANCY));
}

void TestRawPlacementAndRestoreRippleJourney() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t F2 = 53U;
    constexpr uint8_t E4 = 36U;
    constexpr uint8_t F4 = 37U;
    const uint64_t e2Lifted =
        INITIAL_OCCUPANCY & ~(UINT64_C(1) << E2);
    const uint64_t e2e4Occupancy =
        e2Lifted | (UINT64_C(1) << E4);

    test_support::BoundedHintSession normalFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& normal = normalFixture.session;
    test_support::BoundedHintSession minimalFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& minimal = minimalFixture.session;
    const auto reachReady = [&](runtime::VirtualLitchSession& session) {
        return session.Reset() == VIRTUAL_LITCH_OK &&
               session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                   VIRTUAL_LITCH_OK &&
               session.Advance(200U) == VIRTUAL_LITCH_OK &&
               session.Advance(1801U) == VIRTUAL_LITCH_OK;
    };
    CheckRawJourney(reachReady(normal),
                    "normal ripple journey reaches Play Ready",
                    normal);
    CheckRawJourney(reachReady(minimal),
                    "Minimal ripple journey reaches Play Ready",
                    minimal);

    for (auto* session : {&normal, &minimal}) {
        CheckRawJourney(
            modules::settle_animation_setting::Read(
                session->SettingsForPersistenceTest()) ==
                modules::SettleAnimationStyle::Ripple,
            "fresh sessions use Ripple before any Placement effect write",
            *session);
    }

    modules::ControlMutationOutcome minimalOutcome;
    CheckRawJourney(
        minimal.MutateSettingJson(
            901U,
            "MINIMAL_ANIMATIONS",
            "true",
            minimalOutcome) == VIRTUAL_LITCH_OK &&
            minimalOutcome.status ==
                modules::ControlMutationStatus::Applied,
        "Minimal Animations is applied through the acknowledged settings path",
        minimal);

    const auto placeE4 = [&](runtime::VirtualLitchSession& session) {
        return session.ObserveHallMask(e2Lifted) == VIRTUAL_LITCH_OK &&
               session.ObserveHallMask(e2e4Occupancy) ==
                   VIRTUAL_LITCH_OK &&
               session.Advance(220U) == VIRTUAL_LITCH_OK &&
               session.State().interaction_phase == static_cast<uint8_t>(
                   runtime::ChessInteractionPhase::Settling);
    };
    CheckRawJourney(placeE4(normal),
                    "normal e2-e4 placement reaches the ripple frame",
                    normal);
    CheckRawJourney(placeE4(minimal),
                    "Minimal e2-e4 placement reaches its still acknowledgement",
                    minimal);

    Check(!SameColor(BoardLed(normal.SemanticFrame(), F4,
                              hardware::SquareLedSlot::Square0),
                     BoardLed(minimal.SemanticFrame(), F4,
                              hardware::SquareLedSlot::Square0)) &&
              !SameColor(PhysicalBoardLed(normal.PhysicalFrame(), F4,
                                          hardware::SquareLedSlot::Square0),
                         PhysicalBoardLed(minimal.PhysicalFrame(), F4,
                                          hardware::SquareLedSlot::Square0)),
          "placement ripple reaches a neighboring square through semantic and physical output");

    const auto restoreE2 = [&](runtime::VirtualLitchSession& session) {
        return session.ObserveHallMask(e2Lifted) == VIRTUAL_LITCH_OK &&
               session.ObserveHallMask(INITIAL_OCCUPANCY) ==
                   VIRTUAL_LITCH_OK &&
               session.Advance(220U) == VIRTUAL_LITCH_OK &&
               session.State().interaction_phase == static_cast<uint8_t>(
                   runtime::ChessInteractionPhase::Idle) &&
               session.State().physical_occupied_mask == INITIAL_OCCUPANCY;
    };
    CheckRawJourney(restoreE2(normal),
                    "normal source restore reaches the ripple frame",
                    normal);
    CheckRawJourney(restoreE2(minimal),
                    "Minimal source restore reaches its still acknowledgement",
                    minimal);
    Check(!SameColor(BoardLed(normal.SemanticFrame(), F2,
                              hardware::SquareLedSlot::Square0),
                     BoardLed(minimal.SemanticFrame(), F2,
                              hardware::SquareLedSlot::Square0)) &&
              !SameColor(PhysicalBoardLed(normal.PhysicalFrame(), F2,
                                          hardware::SquareLedSlot::Square0),
                         PhysicalBoardLed(minimal.PhysicalFrame(), F2,
                                          hardware::SquareLedSlot::Square0)),
          "restored piece ripple reaches a neighboring square through semantic and physical output");

    CheckRawJourney(
        normal.Advance(400U) == VIRTUAL_LITCH_OK &&
            minimal.Advance(400U) == VIRTUAL_LITCH_OK &&
            SameColor(BoardLed(normal.SemanticFrame(), F2,
                               hardware::SquareLedSlot::Square0),
                      BoardLed(minimal.SemanticFrame(), F2,
                               hardware::SquareLedSlot::Square0)),
        "placement and restore acknowledgements expire back to the base board",
        normal);

    std::printf(
        "[PLACEMENT_RESTORE_RIPPLE_JOURNEY] "
        "input=e2-e4; e4-e2 | authoritative=Settling->PieceLifted->Idle | "
        "accepted=normal radial bloom; Minimal static local acknowledgement | "
        "board=semantic+mapped-physical | expiry=560ms cancellation=move-restored\n");
}

void TestImmediateNextPlayerPickupAcrossSettleJourney() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;
    constexpr uint8_t E7 = 12U;
    constexpr uint8_t E6 = 20U;
    constexpr uint8_t E5 = 28U;
    constexpr uint64_t E7_DESTINATIONS =
        (UINT64_C(1) << E6) | (UINT64_C(1) << E5);
    const uint64_t e2Lifted =
        INITIAL_OCCUPANCY & ~(UINT64_C(1) << E2);
    const uint64_t e2e4 = e2Lifted | (UINT64_C(1) << E4);
    const uint64_t e2e4WithE7Lifted =
        e2e4 & ~(UINT64_C(1) << E7);

    test_support::BoundedHintSession sequentialFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& sequential = sequentialFixture.session;
    CheckRawJourney(
        sequential.Reset() == VIRTUAL_LITCH_OK &&
            sequential.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            sequential.Advance(200U) == VIRTUAL_LITCH_OK &&
            sequential.ObserveHallMask(e2Lifted) == VIRTUAL_LITCH_OK &&
            sequential.ObserveHallMask(e2e4) == VIRTUAL_LITCH_OK &&
            sequential.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::Settling),
        "next-pickup journey reaches human settling through production ingress",
        sequential);
    CheckRawJourney(
        sequential.ObserveHallMask(e2e4WithE7Lifted) ==
                VIRTUAL_LITCH_OK &&
            sequential.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::PieceLifted) &&
            sequential.State().selected_square == E7 &&
            sequential.State().side_to_move_black != 0U &&
            sequential.State().expected_occupied_mask == e2e4 &&
            sequential.State().physical_occupied_mask ==
                e2e4WithE7Lifted &&
            sequential.State().valid_destination_mask ==
                E7_DESTINATIONS &&
            sequential.State().correction_mask == 0ULL &&
            sequential.State().human_illegal_mask == 0ULL &&
            sequential.UndoHistoryMoveCount() == 1U &&
            !StatusIsFullRedFault(sequential.SemanticFrame()),
        "next-side pickup interrupts settling and becomes authoritative exactly once",
        sequential);
    CheckRawJourney(
        sequential.ObserveHallMask(e2e4) == VIRTUAL_LITCH_OK &&
            sequential.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::Idle) &&
            sequential.State().correction_mask == 0ULL &&
            sequential.State().human_illegal_mask == 0ULL &&
            sequential.UndoHistoryMoveCount() == 1U,
        "returning the immediate next source recovers to the committed position",
        sequential);

    test_support::BoundedHintSession coalescedFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& coalesced = coalescedFixture.session;
    CheckRawJourney(
        coalesced.Reset() == VIRTUAL_LITCH_OK &&
            coalesced.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            coalesced.Advance(200U) == VIRTUAL_LITCH_OK &&
            coalesced.Advance(1801U) == VIRTUAL_LITCH_OK &&
            coalesced.ObserveHallMask(e2Lifted) == VIRTUAL_LITCH_OK,
        "coalesced next-pickup journey reaches the first legal source",
        coalesced);
    CheckRawJourney(
        coalesced.ObserveHallMask(e2e4WithE7Lifted) ==
                VIRTUAL_LITCH_OK &&
            coalesced.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::PieceLifted) &&
            coalesced.State().selected_square == E7 &&
            coalesced.State().side_to_move_black != 0U &&
            coalesced.State().expected_occupied_mask == e2e4 &&
            coalesced.State().physical_occupied_mask ==
                e2e4WithE7Lifted &&
            coalesced.State().valid_destination_mask ==
                E7_DESTINATIONS &&
            coalesced.State().correction_mask == 0ULL &&
            coalesced.State().human_illegal_mask == 0ULL &&
            coalesced.UndoHistoryMoveCount() == 1U &&
            LitStatusPixelCount(coalesced.SemanticFrame()) == 0U &&
            !StatusIsFullRedFault(coalesced.SemanticFrame()) &&
            ColorEnergy(BoardLed(
                coalesced.SemanticFrame(), E7,
                hardware::SquareLedSlot::Square0)) != 0U &&
            ColorEnergy(BoardLed(
                coalesced.SemanticFrame(), E7,
                hardware::SquareLedSlot::Piece)) == 0U &&
            ColorEnergy(BoardLed(
                coalesced.SemanticFrame(), E7,
                hardware::SquareLedSlot::Square2)) != 0U,
        "one Hall snapshot can finish e2-e4 and start e7 without correction",
        coalesced);
    CheckRawJourney(
        coalesced.Advance(150U) == VIRTUAL_LITCH_OK &&
            ColorEnergy(BoardLed(
                coalesced.SemanticFrame(), E6,
                hardware::SquareLedSlot::Square0)) != 0U &&
            ColorEnergy(BoardLed(
                coalesced.SemanticFrame(), E6,
                hardware::SquareLedSlot::Square2)) != 0U &&
            ColorEnergy(BoardLed(
                coalesced.SemanticFrame(), E5,
                hardware::SquareLedSlot::Square0)) != 0U &&
            ColorEnergy(BoardLed(
                coalesced.SemanticFrame(), E5,
                hardware::SquareLedSlot::Square2)) != 0U,
        "the coalesced e7 pickup guides e6/e5 after the outward reveal",
        coalesced);
    PrintRawFrame(coalesced,
                  "coalesced e2-e4 completion and legal e7 pickup",
                  "place e7 on e6/e5 or return e7");

    test_support::BoundedHintSession oConfirmFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& oConfirm = oConfirmFixture.session;
    modules::ControlMutationOutcome oConfirmOutcome;
    CheckRawJourney(
        oConfirm.Reset() == VIRTUAL_LITCH_OK &&
            oConfirm.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            oConfirm.Advance(200U) == VIRTUAL_LITCH_OK &&
            oConfirm.Advance(1801U) == VIRTUAL_LITCH_OK &&
            oConfirm.MutateSettingJson(
                902U, "MOVE_CONFIRM",
                "\"press_circle_to_confirm\"", oConfirmOutcome) ==
                VIRTUAL_LITCH_OK &&
            oConfirmOutcome.status ==
                modules::ControlMutationStatus::Applied &&
            oConfirm.ObserveHallMask(e2Lifted) == VIRTUAL_LITCH_OK &&
            oConfirm.ObserveHallMask(e2e4WithE7Lifted) ==
                VIRTUAL_LITCH_OK &&
            oConfirm.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::Settling) &&
            oConfirm.State().side_to_move_black == 0U &&
            oConfirm.State().human_illegal_mask ==
                (UINT64_C(1) << E7) &&
            oConfirm.UndoHistoryMoveCount() == 0U,
        "coalesced next-side pickup cannot bypass Circle confirmation",
        oConfirm);
    PrintRawFrame(oConfirm,
                  "pre-Circle coalesced e7 pickup blocked",
                  "return e7, then press Circle");
    CheckRawJourney(
        oConfirm.ObserveHallMask(e2e4) == VIRTUAL_LITCH_OK &&
            oConfirm.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::Settling) &&
            oConfirm.State().human_illegal_mask == 0ULL &&
            oConfirm.UndoHistoryMoveCount() == 0U &&
            oConfirm.Advance(5000U) == VIRTUAL_LITCH_OK &&
            oConfirm.UndoHistoryMoveCount() == 0U &&
            oConfirm.SetButtonDown(
                VIRTUAL_LITCH_OWNER_P1,
                runtime::VirtualLitchPhysicalButton::O, true) ==
                VIRTUAL_LITCH_OK &&
            oConfirm.Advance(20U) == VIRTUAL_LITCH_OK &&
            oConfirm.SetButtonDown(
                VIRTUAL_LITCH_OWNER_P1,
                runtime::VirtualLitchPhysicalButton::O, false) ==
                VIRTUAL_LITCH_OK &&
            oConfirm.Advance(20U) == VIRTUAL_LITCH_OK &&
            oConfirm.Advance(1000U) == VIRTUAL_LITCH_OK &&
            oConfirm.State().side_to_move_black != 0U &&
            oConfirm.UndoHistoryMoveCount() == 1U,
        "returning e7 preserves the pending move and Circle remains required",
        oConfirm);

    test_support::BoundedHintSession expiryFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& nearExpiry = expiryFixture.session;
    CheckRawJourney(
        nearExpiry.Reset() == VIRTUAL_LITCH_OK &&
            nearExpiry.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            nearExpiry.Advance(200U) == VIRTUAL_LITCH_OK &&
            nearExpiry.ObserveHallMask(e2Lifted) == VIRTUAL_LITCH_OK &&
            nearExpiry.ObserveHallMask(e2e4) == VIRTUAL_LITCH_OK &&
            nearExpiry.Advance(1999U) == VIRTUAL_LITCH_OK &&
            nearExpiry.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::Settling) &&
            nearExpiry.ObserveHallMask(e2e4WithE7Lifted) ==
                VIRTUAL_LITCH_OK &&
            nearExpiry.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::PieceLifted) &&
            nearExpiry.State().selected_square == E7 &&
            nearExpiry.State().valid_destination_mask ==
                E7_DESTINATIONS &&
            nearExpiry.UndoHistoryMoveCount() == 1U &&
            !StatusIsFullRedFault(nearExpiry.SemanticFrame()),
        "next-side pickup just before the settle deadline replays without an upset state",
        nearExpiry);

    test_support::BoundedHintSession afterglowFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& afterglow = afterglowFixture.session;
    CheckRawJourney(
        afterglow.Reset() == VIRTUAL_LITCH_OK &&
            afterglow.ObserveHallMask(INITIAL_OCCUPANCY) ==
                VIRTUAL_LITCH_OK &&
            afterglow.Advance(200U) == VIRTUAL_LITCH_OK &&
            afterglow.ObserveHallMask(e2Lifted) == VIRTUAL_LITCH_OK &&
            afterglow.ObserveHallMask(e2e4) == VIRTUAL_LITCH_OK &&
            afterglow.Advance(2000U) == VIRTUAL_LITCH_OK &&
            afterglow.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::Idle) &&
            afterglow.UndoHistoryMoveCount() == 1U &&
            afterglow.ObserveHallMask(e2e4WithE7Lifted) ==
                VIRTUAL_LITCH_OK &&
            afterglow.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::PieceLifted) &&
            afterglow.State().selected_square == E7 &&
            afterglow.State().valid_destination_mask ==
                E7_DESTINATIONS &&
            afterglow.State().correction_mask == 0ULL &&
            afterglow.State().human_illegal_mask == 0ULL &&
            !StatusIsFullRedFault(afterglow.SemanticFrame()),
        "post-commit confirmation animation never blocks the next legal pickup",
        afterglow);

    constexpr uint8_t D2 = 51U;
    const uint64_t e2e4WithD2Lifted =
        e2e4 & ~(UINT64_C(1) << D2);
    CheckRawJourney(
        afterglow.ObserveHallMask(e2e4) == VIRTUAL_LITCH_OK &&
            afterglow.ObserveHallMask(e2e4WithD2Lifted) ==
                VIRTUAL_LITCH_OK &&
            afterglow.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::Correction) &&
            afterglow.State().correction_mask ==
                (UINT64_C(1) << D2),
        "a wrong-side pickup enters canonical correction",
        afterglow);
    CheckRawJourney(
        afterglow.Advance(modules::CORRECTION_STRONG_MS) ==
                VIRTUAL_LITCH_OK &&
            StatusIsFullRedFault(afterglow.SemanticFrame()),
        "a wrong-side pickup reaches strong correction feedback",
        afterglow);
    PrintRawFrame(afterglow,
                  "wrong-side d2 pickup strong correction",
                  "return d2");
    CheckRawJourney(
        afterglow.ObserveHallMask(e2e4) == VIRTUAL_LITCH_OK &&
            afterglow.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::Idle) &&
            afterglow.State().correction_mask == 0ULL &&
            afterglow.UndoHistoryMoveCount() == 1U &&
            StatusIsTurnIndicatorForOwner(
                afterglow.SemanticFrame(), true),
        "a wrong-side pickup corrects and recovers without recommit",
        afterglow);
    PrintRawFrame(afterglow,
                  "wrong-side d2 pickup recovered",
                  "lift a Black piece");
    std::printf(
        "[IMMEDIATE_NEXT_PLAYER_PICKUP_JOURNEY] "
        "input=e2 lift; e4 place + immediate e7 lift | "
        "authoritative=PieceLifted(e7),side=P2,valid=e6/e5 | "
        "accepted=e2-e4 exactly once | correction=none for legal pickup; "
        "invalid=d2 red correction | recovery=return source\n");
}

void TestRawPromotionBoardFirstJourney() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t A2 = 48U;
    constexpr uint8_t A4 = 32U;
    constexpr uint8_t A5 = 24U;
    constexpr uint8_t A6 = 16U;
    constexpr uint8_t B7 = 9U;
    constexpr uint8_t B8 = 1U;
    constexpr uint8_t C2 = 50U;
    constexpr uint8_t C6 = 18U;
    constexpr uint8_t H7 = 15U;
    constexpr uint8_t H6 = 23U;
    constexpr uint8_t H5 = 31U;
    constexpr uint8_t H4 = 39U;
    uint64_t promotionIconAreaMask = 0ULL;
    for (uint8_t row = 0U;
         row < modules::promotion_piece_cue::ICON_HEIGHT; ++row) {
        for (uint8_t column = 0U;
             column < modules::promotion_piece_cue::ICON_WIDTH; ++column) {
            const uint8_t square = static_cast<uint8_t>(
                (modules::promotion_piece_cue::ICON_ORIGIN_Y + row) * 8U +
                modules::promotion_piece_cue::ICON_ORIGIN_X + column);
            promotionIconAreaMask |= UINT64_C(1) << square;
        }
    }

    test_support::BoundedHintSession sessionFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& session = sessionFixture.session;
    uint64_t physical = INITIAL_OCCUPANCY;
    CheckRawJourney(
        session.Reset() == VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
            session.Advance(200U) == VIRTUAL_LITCH_OK &&
            session.Advance(1801U) == VIRTUAL_LITCH_OK,
        "raw promotion journey reaches Play Ready", session);

    modules::ControlMutationOutcome promotionOutcome;
    Check(session.MutateSettingJson(789U, "PROMOTION_MODE", "\"choose\"", promotionOutcome) == VIRTUAL_LITCH_OK,
          "raw manual promotion fixture opts into Choose");
    const auto move = [&](const uint8_t from,
                          const uint8_t to,
                          const bool capture = false) {
        physical &= ~(UINT64_C(1) << from);
        if (session.ObserveHallMask(physical) != VIRTUAL_LITCH_OK) {
            return false;
        }
        if (capture) {
            physical &= ~(UINT64_C(1) << to);
            if (session.ObserveHallMask(physical) != VIRTUAL_LITCH_OK) {
                return false;
            }
        }
        physical |= UINT64_C(1) << to;
        return session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
               session.Advance(2000U) == VIRTUAL_LITCH_OK;
    };
    const auto setDown = [&](const runtime::VirtualLitchPhysicalButton button,
                             const bool down) {
        return session.SetButtonDown(
                   VIRTUAL_LITCH_OWNER_P1, button, down) ==
                   VIRTUAL_LITCH_OK &&
               session.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    const auto tap = [&](const runtime::VirtualLitchPhysicalButton button) {
        return setDown(button, true) && setDown(button, false);
    };
    const auto promotionCueVisible = [&](
                                         const modules::PromotionPiece piece) {
        const uint64_t expectedIcon =
            modules::promotion_piece_cue::BoardMask(piece);
        return (HighIntensitySemanticBoardSurfaceMask(
                    session.SemanticFrame()) &
                promotionIconAreaMask) == expectedIcon &&
               (HighIntensityPhysicalBoardSurfaceMask(
                    session.PhysicalFrame()) &
                promotionIconAreaMask) == expectedIcon &&
               LitStatusPixelMask(session.SemanticFrame()) ==
                   modules::promotion_piece_cue::
                       HUMAN_LEFT_RIGHT_EDGE_MASK &&
               LitDedicatedPhysicalStatusPixelMask(
                   session.PhysicalFrame()) ==
                   modules::promotion_piece_cue::
                       HUMAN_LEFT_RIGHT_EDGE_MASK;
    };

    CheckRawJourney(
        move(A2, A4) && move(B8, C6) && move(A4, A5) &&
            move(H7, H6) && move(A5, A6) && move(H6, H5) &&
            move(A6, B7, true) && move(H5, H4) && move(B7, B8) &&
            session.PromotionActive() &&
            session.PromotionSelection() == modules::PromotionPiece::Queen &&
            session.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::AwaitingAction) &&
            promotionCueVisible(modules::PromotionPiece::Queen),
        "physical last-rank move opens a board-readable promotion selector",
        session);
    std::printf(
        "[PROMOTION_BOARD_JOURNEY] input=a2-a4,b8-c6,a4-a5,h7-h6,"
        "a5-a6,h6-h5,a6xb7,h5-h4,b7-b8 | "
        "authoritative=AwaitingAction/Promotion choice=Queen | "
        "board=queen-silhouette edge=restrained-left-right-guidance | "
        "next=Left/Right then center-or-Circle; X cancels\n");

    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::X) &&
            !session.PromotionActive() &&
            session.State().physical_occupied_mask !=
                session.State().expected_occupied_mask &&
            session.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::AwaitingAction),
        "short physical X cancels only the provisional promotion", session);
    physical &= ~(UINT64_C(1) << B8);
    Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
          "promotion recovery lifts the provisional last-rank pawn");
    physical |= UINT64_C(1) << B7;
    CheckRawJourney(
        session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.State().correction_mask == 0ULL &&
            session.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::Idle),
        "returning the pawn recovers the pre-promotion board", session);

    CheckRawJourney(
        move(B7, B8) && session.PromotionActive() &&
            promotionCueVisible(modules::PromotionPiece::Queen),
        "the recovered last-rank move reopens the selector", session);
    physical &= ~(UINT64_C(1) << C2);
    CheckRawJourney(
        session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
            session.Advance(2000U) == VIRTUAL_LITCH_OK &&
            session.PromotionActive() &&
            session.PromotionSelection() == modules::PromotionPiece::Queen &&
            session.State().physical_occupied_mask == physical &&
            StatusIsFullRedFault(session.SemanticFrame()) &&
            (HighIntensitySemanticBoardSurfaceMask(
                 session.SemanticFrame()) &
             modules::promotion_piece_cue::BoardMask(
                 modules::PromotionPiece::Queen)) !=
                modules::promotion_piece_cue::BoardMask(
                    modules::PromotionPiece::Queen),
        "an unrelated lift fully preempts the selector with correction",
        session);
    physical |= UINT64_C(1) << C2;
    CheckRawJourney(
            session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
            session.Advance(2000U) == VIRTUAL_LITCH_OK &&
            session.State().physical_occupied_mask == physical &&
            session.PromotionActive() &&
            session.PromotionSelection() == modules::PromotionPiece::Queen &&
            promotionCueVisible(modules::PromotionPiece::Queen),
        "restoring the unrelated piece resumes the same selector",
        session);
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Up) &&
            session.PromotionSelection() == modules::PromotionPiece::Queen,
        "Up is consumed by promotion without changing board color settings",
        session);
    CheckRawJourney(
        setDown(runtime::VirtualLitchPhysicalButton::X, true) &&
            session.Advance(500U) == VIRTUAL_LITCH_OK &&
            promotionCueVisible(modules::PromotionPiece::Queen) &&
            session.Advance(500U) == VIRTUAL_LITCH_OK &&
            !session.QuickSettingsActive() && session.PromotionActive() &&
            setDown(runtime::VirtualLitchPhysicalButton::X, false) &&
            !session.QuickSettingsActive() && session.PromotionActive(),
        "held X cannot open Quick Setup or cancel promotion on release",
        session);
    CheckRawJourney(
        session.Advance(60000U) == VIRTUAL_LITCH_OK &&
            session.PromotionActive() &&
            session.PromotionSelection() == modules::PromotionPiece::Queen,
        "board waits without silently auto-selecting when the app is absent",
        session);
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Right) &&
            session.PromotionSelection() == modules::PromotionPiece::Rook &&
            promotionCueVisible(modules::PromotionPiece::Rook),
        "physical Right selects the Rook silhouette", session);
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Right) &&
            session.PromotionSelection() == modules::PromotionPiece::Bishop &&
            promotionCueVisible(modules::PromotionPiece::Bishop),
        "physical Right selects the Bishop silhouette", session);
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::Right) &&
            session.PromotionSelection() == modules::PromotionPiece::Knight &&
            promotionCueVisible(modules::PromotionPiece::Knight),
        "physical Right selects the Knight silhouette", session);
    CheckRawJourney(
        tap(runtime::VirtualLitchPhysicalButton::O) &&
            !session.PromotionActive() &&
            session.State().physical_occupied_mask == physical &&
            session.State().expected_occupied_mask == physical &&
            session.State().side_to_move_black != 0U &&
            session.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::Idle) &&
            StatusIsTurnIndicatorForOwner(session.SemanticFrame(), true),
        "occupied-board Circle confirms the underpromotion without opening Litchcraft",
        session);
    std::printf(
        "[PROMOTION_BOARD_JOURNEY] input=X cancel; b8-b7 recovery; b7-b8; "
        "c2 correction/recovery; Up ignored; X hold blocked; wait 60s; "
        "Rightx3; Circle | "
        "authoritative=Promotion(Knight)->Idle/Black | outcome=confirmed-knight | "
        "board=physical/expected:%016llx status=P2-turn-indicator | "
        "expiry=none app=not-required\n",
        static_cast<unsigned long long>(physical));
}

void TestRawPromotionCorrectionConfirmJourney(const uint8_t missingSquare,
                                             const virtual_litch_config config = ConfigFor(VIRTUAL_LITCH_HARDWARE_S3)) {
    constexpr uint64_t INITIAL = UINT64_C(0xFFFF00000000FFFF);
    constexpr uint8_t B7 = 9U;
    constexpr uint8_t B8 = 1U;
    constexpr uint8_t C2 = 50U;
    const char* const branch = missingSquare == C2 ? "missing-c2" : "lifted-b8";
    const uint8_t hardware = config.hardware;
    const bool squareRotate = modules::ComputeSquareRotate(config.board_rotate != 0U, config.first_player_is_p2 != 0U);
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    modules::ControlMutationOutcome outcome;
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              session.MutateSettingJson(790U, "TIMER", "1", outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "promotion correction fixture selects a real running clock");
    Check(session.MutateSettingJson(789U, "PROMOTION_MODE", "\"choose\"", outcome) == VIRTUAL_LITCH_OK,
          "raw correction promotion fixture opts into Choose");
    virtual_litch_action_request newGame{};
    virtual_litch_default_action_request(&newGame);
    newGame.request_id = 791U;
    newGame.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    newGame.owner = VIRTUAL_LITCH_OWNER_P1;
    Check(session.DispatchControlAction(newGame, outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "promotion correction fixture starts the timed game");
    uint64_t physical = INITIAL;
    const auto observe = [&](const uint64_t mask) {
        return session.ObserveHallMask(squareRotate ? RotateSquareMask180(mask) : mask);
    };
    CheckRawJourney(observe(physical) == VIRTUAL_LITCH_OK &&
                        session.Advance(200U) == VIRTUAL_LITCH_OK &&
                        session.Advance(1801U) == VIRTUAL_LITCH_OK,
                    "promotion correction journey admits the physical opening", session);
    const auto move = [&](const uint8_t from, const uint8_t to,
                          const bool capture = false) {
        physical &= ~(UINT64_C(1) << from);
        if (observe(physical) != VIRTUAL_LITCH_OK) return false;
        if (capture) {
            physical &= ~(UINT64_C(1) << to);
            if (observe(physical) != VIRTUAL_LITCH_OK) return false;
        }
        physical |= UINT64_C(1) << to;
        return observe(physical) == VIRTUAL_LITCH_OK &&
               session.Advance(2000U) == VIRTUAL_LITCH_OK;
    };
    const auto tap = [&](const runtime::VirtualLitchPhysicalButton button) {
        return session.SetButtonDown(squareRotate ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1, button, true) == VIRTUAL_LITCH_OK &&
               session.Advance(20U) == VIRTUAL_LITCH_OK &&
               session.SetButtonDown(squareRotate ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1, button, false) == VIRTUAL_LITCH_OK &&
               session.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    const auto engineFen = [&]() {
        char fen[litch_max::FEN_BUFFER_CAPACITY]{};
        Check(fixture.engine.GetFen(fen, sizeof(fen)), "promotion journey reads the real engine FEN");
        return std::string(fen);
    };
    const auto checkpoint = [&](const char* step, const char* next) {
        if (hardware == VIRTUAL_LITCH_HARDWARE_S3) PrintRawFrame(session, step, next);
        else {
            const auto& semantic = session.SemanticFrame();
            const auto& physicalFrame = session.PhysicalFrame();
            bool mappedSidesAgree = physicalFrame.led_count == 300U;
            for (uint8_t square = 0U; square < 64U; ++square) {
                for (const auto slot : {hardware::SquareLedSlot::Square0, hardware::SquareLedSlot::Square2}) {
                    const auto targets = hardware::MapSemanticSquareLedTargets(
                        hardware::BoardRevision::V3_3S3, square, slot, squareRotate);
                    const auto energy = ColorEnergy(semantic.board[square][static_cast<uint8_t>(slot)]);
                    const auto actual = ColorEnergy(physicalFrame.pixels[targets.indices[0]]);
                    mappedSidesAgree &= energy == 0U ? actual == 0U : energy < 64U || actual != 0U;
                }
            }
            Check(mappedSidesAgree, "exact V3.3 promotion maps real semantic board sides at the configured orientation");
            std::printf("[PROMOTION_V33_FRAME] step=%s first_end=%u rotate=%u mapped_sides=%u physical_leds=%u "
                        "frame_hash=%016llx status=%s next=%s pixels=",
                step, config.first_player_is_p2 ? 2U : 1U, config.board_rotate, mappedSidesAgree,
                physicalFrame.led_count, static_cast<unsigned long long>(physicalFrame.frame_hash),
                StatusShapeName(semantic), next);
            for (uint16_t pixel = 0U; pixel < physicalFrame.led_count; ++pixel) {
                const auto color = physicalFrame.pixels[pixel];
                std::printf("%02x%02x%02x%02x", color.r, color.g, color.b, color.w);
            }
            std::puts("");
        }
        const auto home = session.HomeStateSnapshot();
        const auto line = session.HomeStateLine();
        modules::home_state_wire::Payload payload;
        Check(modules::home_state_wire::Encode(home, payload), "production promotion Home encodes unchanged binary layout");
        Check(!line.Empty(), "production promotion Home JSON retains required physical plan");
        if (session.PromotionActive()) {
            Check(home.physicalMoveValid && home.physicalFromSquare == B7 &&
                      home.physicalToSquare == B8 && !home.physicalCaptureSquareValid,
                  "actual pending promotion publishes only its exact completed physical plan");
        }
        const auto* clock = session.GameClock();
        std::printf("[PROMOTION_CORRECTION_CONFIRM_JOURNEY] branch=%s step=%s "
                    "choice=%u active=%u token=%u history=%u fen=\"%s\" "
                    "clock=%u/%u/%u side=%u white_ms=%u black_ms=%u "
                    "home_action=%s home_move=%u:%u-%u home_fen=\"%s\" "
                    "home_line=%s | next=%s\n", branch, step,
                    static_cast<unsigned>(session.PromotionSelection()),
                    session.PromotionActive() ? 1U : 0U,
                    session.State().pending_action_token, session.UndoHistoryMoveCount(),
                    engineFen().c_str(), clock->IsEnabled(), clock->IsRunning(), clock->IsPaused(),
                    static_cast<unsigned>(clock->GetActiveSide()),
                    clock->GetRemainingMs(runtime::ClockSide::White),
                    clock->GetRemainingMs(runtime::ClockSide::Black),
                    modules::HomeStateActionToToken(home.action), home.physicalMoveValid,
                    home.physicalFromSquare, home.physicalToSquare, home.fen, line.CStr(), next);
    };
    CheckRawJourney(move(48U, 32U) && move(B8, 18U) && move(32U, 24U) &&
                        move(15U, 23U) && move(24U, 16U) && move(23U, 31U) &&
                        move(16U, B7, true) && move(31U, 39U) && move(B7, B8) &&
                        tap(runtime::VirtualLitchPhysicalButton::Right) &&
                        tap(runtime::VirtualLitchPhysicalButton::Right) &&
                        tap(runtime::VirtualLitchPhysicalButton::Right) &&
                        session.PromotionActive() &&
                        session.PromotionSelection() == modules::PromotionPiece::Knight,
                    "raw opening moves retain an explicit Knight choice before correction", session);
    const std::string preFen = engineFen();
    const auto pending = session.State();
    const uint8_t history = session.UndoHistoryMoveCount();
    const uint64_t completedPhysical = physical;
    const auto* clock = session.GameClock();
    const auto activeSide = clock->GetActiveSide();
    const uint32_t blackRemaining = clock->GetRemainingMs(runtime::ClockSide::Black);
    Check(clock->IsEnabled() && clock->IsRunning() && activeSide == runtime::ClockSide::White,
          "promotion modal still owns White's running clock before commit");
    checkpoint("Knight selected", "lift the named piece without confirming");
    physical &= ~(UINT64_C(1) << missingSquare);
    CheckRawJourney(observe(physical) == VIRTUAL_LITCH_OK &&
                        session.Advance(2000U) == VIRTUAL_LITCH_OK &&
                        session.PromotionActive() &&
                        session.State().physical_occupied_mask == physical &&
                        (missingSquare != C2 || StatusIsFullRedFault(session.SemanticFrame())),
                    "raw Hall lift retains promotion; unrelated c2 owns correction", session);
    checkpoint("correction before O", missingSquare == C2 ? "return c2; leave pawn on b8" : "replace b8; do not return pawn to b7");
    CheckRawJourney(missingSquare != C2 ||
                        session.HomeStateSnapshot().action == modules::HomeStateAction::Correction,
                    "production Home must request correction while the board hides promotion", session);
    const bool acceptedRawO = tap(runtime::VirtualLitchPhysicalButton::O);
    checkpoint("physical O during correction", "restore only the missing square; no implicit confirmation");
    CheckRawJourney(acceptedRawO && session.PromotionActive() &&
                        session.PromotionSelection() == modules::PromotionPiece::Knight &&
                        session.State().pending_action_token == pending.pending_action_token &&
                        engineFen() == preFen && session.UndoHistoryMoveCount() == history &&
                        session.State().side_to_move_black == 0U &&
                        clock->GetActiveSide() == activeSide &&
                        clock->GetRemainingMs(runtime::ClockSide::Black) == blackRemaining &&
                        (missingSquare != C2 || StatusIsFullRedFault(session.SemanticFrame())),
                    "physical O with a missing piece must not commit choice, history or clock", session);
    physical |= UINT64_C(1) << missingSquare;
    CheckRawJourney(observe(physical) == VIRTUAL_LITCH_OK &&
                        session.Advance(2000U) == VIRTUAL_LITCH_OK &&
                        physical == completedPhysical &&
                        (physical & (UINT64_C(1) << B7)) == 0ULL &&
                        (physical & (UINT64_C(1) << B8)) != 0ULL &&
                        session.PromotionActive() &&
                        session.PromotionSelection() == modules::PromotionPiece::Knight &&
                        session.State().pending_action_token == pending.pending_action_token &&
                        engineFen() == preFen && session.UndoHistoryMoveCount() == history &&
                        session.HomeStateSnapshot().action == modules::HomeStateAction::Promote &&
                        !StatusIsFullRedFault(session.SemanticFrame()),
                    "restoration resumes the same Knight selector and never autoaccepts old O", session);
    checkpoint("restored without a new O", "fresh O confirms Knight exactly once");
    const bool needsFreshO = session.PromotionActive() && engineFen() == preFen &&
                             session.UndoHistoryMoveCount() == history;
    CheckRawJourney(tap(runtime::VirtualLitchPhysicalButton::O) && needsFreshO &&
                        !session.PromotionActive() && session.UndoHistoryMoveCount() == history + 1U &&
                        engineFen() == "rNbqkbnr/p1ppppp1/2n5/8/7p/8/1PPPPPPP/RNBQKBNR b KQkq - 0 1" &&
                        session.State().expected_occupied_mask == completedPhysical &&
                        session.State().side_to_move_black != 0U &&
                        clock->GetActiveSide() == runtime::ClockSide::Black &&
                        StatusIsTurnIndicatorForOwner(session.SemanticFrame(), true),
                    "only a fresh visible O commits exactly the selected Knight and hands off", session);
    checkpoint("fresh O after restoration", "Black may make the next legal move");
}

void TestRawDuckWholeTurnUndoCompletesJourney() {
    constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;
    constexpr uint8_t DUCK_E5 = 28U;
    constexpr uint8_t DUCK_D5 = 27U;
    const uint64_t openingWithDuck =
        INITIAL_OCCUPANCY | (UINT64_C(1) << DUCK_E5);
    const uint64_t e2LiftedWithDuck =
        openingWithDuck & ~(UINT64_C(1) << E2);
    const uint64_t chessMovedWithDuck =
        e2LiftedWithDuck | (UINT64_C(1) << E4);
    const uint64_t duckLifted =
        chessMovedWithDuck & ~(UINT64_C(1) << DUCK_E5);
    const uint64_t completedTurn =
        duckLifted | (UINT64_C(1) << DUCK_D5);

    test_support::BoundedHintSession sessionFixture(
        ConfigFor(VIRTUAL_LITCH_HARDWARE_S3));
    auto& session = sessionFixture.session;
    CheckRawJourney(
        session.Reset() == VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(openingWithDuck) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(200U) == VIRTUAL_LITCH_OK &&
            session.Advance(1801U) == VIRTUAL_LITCH_OK &&
            session.DuckSquare() == DUCK_E5,
        "raw Duck Undo journey reaches Play Ready with Duck on e5",
        session);

    CheckRawJourney(
        session.ObserveHallMask(e2LiftedWithDuck) ==
                VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(chessMovedWithDuck) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(10U) == VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(duckLifted) == VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(completedTurn) == VIRTUAL_LITCH_OK &&
            session.UndoHistoryMoveCount() == 0U &&
            session.Advance(1999U) == VIRTUAL_LITCH_OK &&
            session.UndoHistoryMoveCount() == 0U &&
            session.State().side_to_move_black == 0U,
        "raw Duck Undo fixture waits for its full human Duck settle", session);
    CheckRawJourney(
        session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.UndoHistoryMoveCount() == 1U &&
            session.State().physical_occupied_mask == completedTurn &&
            session.State().expected_occupied_mask == completedTurn &&
            session.DuckSquare() == DUCK_D5 &&
            session.State().side_to_move_black != 0U,
        "raw Duck Undo journey commits e2-e4 plus e5-d5",
        session);

    const auto setXDown = [&](const bool down) {
        return session.SetButtonDown(
                   VIRTUAL_LITCH_OWNER_P1,
                   runtime::VirtualLitchPhysicalButton::X,
                   down) == VIRTUAL_LITCH_OK &&
               session.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    modules::ControlMutationOutcome outcome;
    CheckRawJourney(
        setXDown(true) && setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Applied &&
            outcome.actionState ==
                modules::ControlActionState::UndoPending &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::DuckRelocation),
        "raw Duck Undo starts with the indivisible Duck return", session);

    CheckRawJourney(
        setXDown(true) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Applied &&
            outcome.reason == modules::ControlMutationReason::None &&
            outcome.actionState == modules::ControlActionState::Idle &&
            session.State().physical_occupied_mask == completedTurn &&
            session.State().expected_occupied_mask == completedTurn &&
            session.DuckSquare() == DUCK_D5 &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::Idle) &&
            session.State().side_to_move_black != 0U &&
            StatusIsTurnIndicatorForOwner(session.SemanticFrame(), true) &&
            !StatusIsFullRedFault(session.SemanticFrame()),
        "second physical X cancels an untouched Duck return on press",
        session);
    const uint16_t cancelRequestId = outcome.requestId;
    CheckRawJourney(
        setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.requestId == cancelRequestId &&
            session.State().physical_occupied_mask == completedTurn &&
            session.State().expected_occupied_mask == completedTurn,
        "release after Duck Cancel Undo is consumed without a duplicate action",
        session);

    std::printf(
        "[DUCK_UNDO_CANCEL_JOURNEY] input=e2-e4,e5-d5; X tap; X press | "
        "authoritative=DuckRelocation->Idle | outcome=applied/none | "
        "board=physical/expected:%016llx duck=d5 "
        "status=P2-turn-indicator | "
        "cancel_latency=same-input-tick release=consumed\n",
        static_cast<unsigned long long>(completedTurn));

    CheckRawJourney(
        setXDown(true) && setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Applied &&
            outcome.actionState ==
                modules::ControlActionState::UndoPending &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::DuckRelocation),
        "raw Duck Undo restarts after the cancellation journey", session);

    const uint64_t undoDuckLifted =
        completedTurn & ~(UINT64_C(1) << DUCK_D5);
    const uint64_t duckReturned =
        undoDuckLifted | (UINT64_C(1) << DUCK_E5);
    CheckRawJourney(
        session.ObserveHallMask(undoDuckLifted) == VIRTUAL_LITCH_OK &&
            session.IsUndoActionPending() &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::DuckRelocation),
        "raw Duck return stays pending while the Duck is in hand", session);
    CheckRawJourney(
        setXDown(true) && setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Rejected &&
            outcome.reason == modules::ControlMutationReason::Busy &&
            outcome.actionState ==
                modules::ControlActionState::UndoPending &&
            session.IsUndoActionPending() &&
            session.DisplayText().empty() &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::DuckRelocation),
        "raw mid-return Cancel Undo is rejected without dropping pending state",
        session);
    CheckRawJourney(
        session.ObserveHallMask(completedTurn) == VIRTUAL_LITCH_OK &&
            session.IsUndoActionPending() &&
            setXDown(true) && setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Applied &&
            outcome.actionState == modules::ControlActionState::Idle &&
            !session.IsUndoActionPending() &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::Idle),
        "raw restored Duck source makes Cancel Undo safe again", session);
    CheckRawJourney(
        session.Advance(481U) == VIRTUAL_LITCH_OK &&
            session.DisplayText().empty() &&
            StatusIsTurnIndicatorForOwner(session.SemanticFrame(), true),
        "raw mid-return unavailable cue expires into the forward turn",
        session);

    std::printf(
        "[DUCK_UNDO_CANCEL_DURING_RETURN_JOURNEY] "
        "input=X tap; lift d5; X tap rejected; return d5; X tap | "
        "authoritative=UndoPending throughout touched return -> Idle | "
        "outcome=calm-unavailable-then-cancelled | board=forward-restored | "
        "recovery=return Duck to lit source; cue expires at 481ms\n");

    CheckRawJourney(
        setXDown(true) && setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Applied &&
            outcome.actionState ==
                modules::ControlActionState::UndoPending &&
            session.IsUndoActionPending(),
        "raw Duck Undo restarts for after-return cancellation", session);
    CheckRawJourney(
        session.ObserveHallMask(undoDuckLifted) == VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(duckReturned) == VIRTUAL_LITCH_OK &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.IsUndoActionPending() &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::ExecutingPlan),
        "raw Duck Undo advances from Duck return to chess inverse",
        session);

    CheckRawJourney(
        setXDown(true) && session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Applied &&
            outcome.actionState ==
                modules::ControlActionState::UndoPending &&
            session.IsUndoActionPending() &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::DuckRelocation),
        "raw Cancel Undo after the Duck return starts a pending Duck redo",
        session);
    const uint16_t redoRequestId = outcome.requestId;
    CheckRawJourney(
        setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.requestId == redoRequestId &&
            session.IsUndoActionPending(),
        "release after cancellation is consumed while the redo stays pending",
        session);

    const uint64_t redoDuckLifted =
        duckReturned & ~(UINT64_C(1) << DUCK_E5);
    CheckRawJourney(
        session.ObserveHallMask(redoDuckLifted) == VIRTUAL_LITCH_OK &&
            session.IsUndoActionPending() &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::DuckRelocation),
        "raw cancel-undo stays pending while the redo Duck is in hand",
        session);
    CheckRawJourney(
        session.ObserveHallMask(completedTurn) == VIRTUAL_LITCH_OK &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            !session.IsUndoActionPending() &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::Idle) &&
            session.State().physical_occupied_mask == completedTurn &&
            session.State().expected_occupied_mask == completedTurn &&
            session.DuckSquare() == DUCK_D5,
        "raw Duck redo clears pending only after the whole turn is restored",
        session);

    std::printf(
        "[DUCK_UNDO_CANCEL_AFTER_RETURN_JOURNEY] "
        "input=X tap; d5-e5; X tap; e5-d5 | "
        "authoritative=UndoPending through opposite fixed Duck redo -> Idle | "
        "outcome=cancelled | board=forward-restored | expiry=none\n");

    CheckRawJourney(
        setXDown(true) && setXDown(false) &&
            session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Applied &&
            outcome.actionState ==
                modules::ControlActionState::UndoPending &&
            session.IsUndoActionPending(),
        "raw Duck Undo restarts for complete inverse", session);
    CheckRawJourney(
        session.ObserveHallMask(undoDuckLifted) == VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(duckReturned) == VIRTUAL_LITCH_OK &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.IsUndoActionPending() &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::ExecutingPlan),
        "raw final Duck return advances to the chess inverse", session);

    const uint64_t undoChessLifted =
        duckReturned & ~(UINT64_C(1) << E4);
    CheckRawJourney(
        session.ObserveHallMask(undoChessLifted) ==
                VIRTUAL_LITCH_OK &&
            session.ObserveHallMask(openingWithDuck) ==
                VIRTUAL_LITCH_OK &&
            session.Advance(1U) == VIRTUAL_LITCH_OK &&
            session.State().physical_occupied_mask == openingWithDuck &&
            session.State().expected_occupied_mask == INITIAL_OCCUPANCY &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::Idle) &&
            session.State().side_to_move_black == 0U &&
            session.GameVariant() == modules::GameVariant::Undecided &&
            session.SetupDuckCandidateSquare() == DUCK_E5 &&
            !session.IsUndoActionPending() &&
            StatusIsTurnIndicatorForOwner(
                session.SemanticFrame(), false) &&
            !StatusIsFullRedFault(session.SemanticFrame()),
        "raw Duck chess inverse completes the whole-turn Undo", session);

    std::printf(
        "[DUCK_UNDO_COMPLETE_JOURNEY] "
        "input=e2-e4,e5-d5; X tap; d5-e5,e4-e2 | "
        "authoritative=DuckRelocation->UndoPending->Idle | "
        "outcome=completed | board=physical:%016llx expected:%016llx "
        "provisional_duck=e5 | "
        "expiry=none cancellation=not-required\n",
        static_cast<unsigned long long>(openingWithDuck),
        static_cast<unsigned long long>(INITIAL_OCCUPANCY));
}

} // namespace

int main() {
    std::puts("[VIRTUAL_LITCH_SESSION] start");
    TestAbiValidation();
    TestRuntimeHealthAbiAndValidation();
    TestSleepLifecycle();
    TestAppSleepCountUpFadeWakeEventImmunityJourney();
    TestSleepCancellationAndLiveState();
    TestSleepCuePhysicalPipeline();
    TestSemanticAndHardwareMappings();
    TestV3ProfileAndProductionJourney();
    TestBoardObservationsAndCastlingIntent();
    TestClockAndHistoryRing();
    TestInterleavedSessionOwnership();
    TestPowerBudgetCapture();
    TestNewGameKingCueAtRestrainedBrightness();
    TestSetupUnavailableUndoUsesPerimeterCue();
    TestSecondPhysicalUndoCancelsPendingCorrectionOnPress();
    TestFreshSetupKingLiftHomeJourney();
    TestPlayerEndSetupJourneyPlayback();
    TestBoardTimerQuickSettingsJourney();
    TestAppOccupiedSettingFeedbackJourney();
    TestClockTimeoutPhysicalHintRecoveryJourney();
    TestProductionSetupJourneyPlayback();
    TestDuckBeforeAndAfterSetupPresentationJourney();
    TestRuntimeHealthJourneyPlayback();
    TestPowerLimitedFreshSetupKeepsTurnIndicator();
    TestAppendOnlyControlMutationAbi();
    TestConcurrentHandleClockIsolation();
    TestRawPlayerEndQuickSettingJourney();
    TestRawP2QuickSettingsOwnership();
    TestRawPhysicalPaletteShortcutQuadrants();
    TestRawBoardRotateQuickSettingJourney();
    TestAppBoardRotateMutationStartsKingQueenJourney();
    TestV33CastlingSteadyRookDestinationJourney();
    TestLiteralPhysicalTurnKeepsPlayerColorJourney();
    TestRawSetupFailureRegressionJourney();
    TestRawPlacementAndRestoreRippleJourney();
    TestImmediateNextPlayerPickupAcrossSettleJourney();
    TestRawPhysicalUndoCompletesJourney();
    TestRawPromotionBoardFirstJourney();
    TestRawPromotionCorrectionConfirmJourney(50U);
    TestRawPromotionCorrectionConfirmJourney(1U);
    for (const bool firstPlayerIsP2 : {false, true}) {
        for (const bool boardRotate : {false, true}) {
            auto config = ConfigFor(VIRTUAL_LITCH_HARDWARE_V3_3);
            config.first_player_is_p2 = firstPlayerIsP2;
            config.board_rotate = boardRotate;
            config.square_rotate_for_output = modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2);
            TestRawPromotionCorrectionConfirmJourney(50U, config);
            TestRawPromotionCorrectionConfirmJourney(1U, config);
        }
    }
    TestRawDuckWholeTurnUndoCompletesJourney();

    if (g_failures == 0) {
        std::puts("[VIRTUAL_LITCH_SESSION] ok");
        return 0;
    }
    std::printf("[VIRTUAL_LITCH_SESSION] failures=%d\n", g_failures);
    return 1;
}
