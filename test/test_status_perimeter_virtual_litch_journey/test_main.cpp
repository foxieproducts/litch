#include "bounded_hint_engine.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include <ArduinoJson.h>

#include "virtual_litch_session.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/correction_feedback.hpp"
#include "modules/rgb_optical_calibration.hpp"
#include "modules/rgbw_auto_map.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace {

constexpr uint64_t INITIAL = UINT64_C(0xFFFF00000000FFFF);
// Independent oracle from hardware_platform.md, LEDs section: clockwise from
// top left, 0=a8 .. 63=h1. Do not derive these targets from the adapter under test.
constexpr std::array<uint8_t, 32> RING_SQUARES{{
    0, 1, 2, 3, 4, 5, 6, 7,
    7, 15, 23, 31, 39, 47, 55, 63,
    63, 62, 61, 60, 59, 58, 57, 56,
    56, 48, 40, 32, 24, 16, 8, 0,
}};
int g_failures = 0;

void Check(bool condition, const char* stage, const char* message) {
    if (!condition) {
        std::printf("[FAIL] StatusPerimeterParity/%s: %s\n", stage, message);
        ++g_failures;
    }
}

modules::Rgb Rgb(virtual_litch_color c) { return {c.r, c.g, c.b}; }

bool Same(virtual_litch_color a, virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}

bool Visible(virtual_litch_color c) { return c.r || c.g || c.b || c.w; }

uint16_t RingLed(uint8_t index) {
    // Horizontal/vertical sides have separate square roles at each corner.
    return static_cast<uint16_t>(RING_SQUARES[index] * 3U +
                                ((index / 8U) % 2U == 0U ? 0U : 2U));
}

bool IsSharedRingLed(uint16_t led) {
    for (uint8_t index = 0U; index < RING_SQUARES.size(); ++index) {
        if (RingLed(index) == led) return true;
    }
    return false;
}

bool Idle(const runtime::VirtualLitchSession& session) {
    const auto& s = session.State();
    return s.interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
           s.selected_square == runtime::CHESS_INTERACTION_INVALID_SQUARE && s.required_action == 0U &&
           s.correction_mask == 0ULL && s.valid_destination_mask == 0ULL;
}

bool Observe(runtime::VirtualLitchSession& session, uint64_t logical, bool rotate) {
    uint64_t physical = 0ULL;
    for (uint8_t square = 0; square < 64U; ++square) {
        if (logical & (UINT64_C(1) << square))
            physical |= UINT64_C(1) << (rotate ? 63U - square : square);
    }
    return session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
           session.State().physical_occupied_mask == logical;
}

bool Set(runtime::VirtualLitchSession& session, const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    const bool accepted = session.MutateSettingJson(101U, key, value, outcome) == VIRTUAL_LITCH_OK &&
        (outcome.status == modules::ControlMutationStatus::Applied ||
         outcome.status == modules::ControlMutationStatus::Noop);
    std::printf("[STATUS_PERIMETER_ACK] key=%s value=%s accepted=%u\n", key, value, accepted);
    return accepted;
}

bool Down(runtime::VirtualLitchSession& session,
          runtime::VirtualLitchPhysicalButton button, bool down) {
    // The same fitted physical P1 bank exists on both boards, including when
    // rotated. The production adapter alone chooses its logical owner.
    return session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, button, down) == VIRTUAL_LITCH_OK &&
           session.Advance(20U) == VIRTUAL_LITCH_OK;
}

bool Tap(runtime::VirtualLitchSession& session, runtime::VirtualLitchPhysicalButton button) {
    return Down(session, button, true) && Down(session, button, false);
}

struct Checkpoint {
    virtual_litch_state state{};
    virtual_litch_semantic_frame semantic{};
};

// This checks position through the real compositor, status adapter, RGB/RGBW
// mapping and current limiter. Reuse the existing blend/color policy because
// only explicitly owned king brackets take priority. The captured scale accounts for genuine
// target current differences; it is not an optical brightness parity assertion.
void CheckPhysical(const runtime::VirtualLitchSession& session, uint8_t hardware,
                   bool rotate, const char* stage) {
    const auto& semantic = session.SemanticFrame();
    const auto& physical = session.PhysicalFrame();
    std::array<modules::Rgb, 224> expected{};
    for (uint8_t square = 0; square < 64U; ++square) {
        for (uint8_t slot = 0; slot < 3U; ++slot) {
            const uint16_t led = (rotate ? 63U - square : square) * 3U +
                                 (rotate ? 2U - slot : slot);
            expected[led] = Rgb(semantic.board[square][slot]);
        }
    }
    for (uint8_t semanticIndex = 0; semanticIndex < 32U; ++semanticIndex) {
        const uint8_t ringIndex = (semanticIndex + (rotate ? 16U : 0U)) % 32U;
        const uint16_t led = hardware == VIRTUAL_LITCH_HARDWARE_C3
                                 ? RingLed(ringIndex) : 192U + ringIndex;
        const auto status = Rgb(semantic.status[semanticIndex]);
        const uint8_t composerSquare = rotate
            ? static_cast<uint8_t>(63U - RING_SQUARES[ringIndex])
            : RING_SQUARES[ringIndex];
        if (hardware == VIRTUAL_LITCH_HARDWARE_C3 &&
            (session.SidePrioritySquareMask() & (UINT64_C(1) << composerSquare)) != 0ULL) {
            continue;
        }
        expected[led] = hardware == VIRTUAL_LITCH_HARDWARE_C3
            ? modules::status_led_frame_adapter::BlendVirtualStatus(expected[led], status)
            : status;
    }
    const auto scale = [&](uint8_t channel) {
        return static_cast<uint8_t>((static_cast<uint32_t>(channel) *
            physical.applied_brightness_scale_65535 + 32767U) / 65535U);
    };
    unsigned mismatches = 0U;
    for (uint16_t led = 0; led < physical.led_count; ++led) {
        // This journey owns the status-perimeter contract. Quick Setup's grid
        // foreground is relocated after the semantic capture so its interior
        // C3 pixels cannot be reconstructed from `semantic.board`; they belong
        // to the independently covered menu-orientation journey. Keep this
        // oracle on the dedicated S3 perimeter or the shared C3 clockwise ring.
        const bool statusSurface = hardware == VIRTUAL_LITCH_HARDWARE_S3
            ? led >= 192U
            : IsSharedRingLed(led);
        if (!statusSurface) continue;
        const modules::Rgb scaled{scale(expected[led].r), scale(expected[led].g), scale(expected[led].b)};
        const auto channels = hardware == VIRTUAL_LITCH_HARDWARE_S3
            ? modules::AutoMapRgbToRgbw(
                  modules::rgb_optical_calibration::PresentForS3(scaled), 32U)
            : modules::RgbwChannels{scaled.r, scaled.g, scaled.b, 0U};
        const auto actual = physical.pixels[led];
        // Captured scale is rounded to uint16; permit one RGB quantization step.
        const auto near = [](int a, int b) { return a >= b - 1 && a <= b + 1; };
        if (!near(actual.r, channels.r) || !near(actual.g, channels.g) ||
            !near(actual.b, channels.b) || !near(actual.w, channels.w)) {
            if (mismatches < 4U)
                std::printf("[STATUS_PERIMETER_MISMATCH] hw=%u rotate=%u stage=%s led=%u "
                            "actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n", hardware, rotate,
                            stage, led, actual.r, actual.g, actual.b, actual.w,
                            channels.r, channels.g, channels.b, channels.w);
            ++mismatches;
        }
    }
    Check(mismatches == 0U, stage, "calibrated final output follows the physical clockwise ring and preserves square/piece roles");
}

void Capture(const runtime::VirtualLitchSession& session, uint8_t hardware, bool rotate,
             const char* stage, const char* nextAction, std::vector<Checkpoint>& out) {
    std::printf("[STATUS_OWNER_MASK] stage=%s mask=%016llx\n", stage,
        static_cast<unsigned long long>(session.SidePrioritySquareMask()));
    const auto& state = session.State();
    const auto display = session.DisplayText();
    std::printf("[STATUS_PERIMETER_JOURNEY] hw=%u rotate=%u stage=%s t=%u "
                "occupancy=%016llx phase=%u required=%u selected=%u correction=%016llx "
                "quick_setup=%u item=%u display=\"%.*s\" next=%s home=%s\n",
                hardware, rotate, stage, state.now_ms,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                state.interaction_phase, state.required_action, state.selected_square,
                static_cast<unsigned long long>(state.correction_mask), session.QuickSettingsActive(),
                session.QuickSettingsSelectedIndex(), static_cast<int>(display.size()),
                display.empty() ? "" : display.data(), nextAction, session.HomeStateLine().CStr());
    for (uint8_t index : {0U, 7U, 8U, 15U, 16U, 23U, 24U, 31U}) {
        const uint8_t ringIndex = (index + (rotate ? 16U : 0U)) % 32U;
        const uint16_t led = hardware == VIRTUAL_LITCH_HARDWARE_C3 ? RingLed(ringIndex) : 192U + ringIndex;
        const auto intent = session.SemanticFrame().status[index];
        const auto pixel = session.PhysicalFrame().pixels[led];
        std::printf("[STATUS_PERIMETER_LED] stage=%s semantic=%u ring=%u square=%u led=%u "
                    "intent=%u,%u,%u physical=%u,%u,%u,%u\n", stage, index, ringIndex,
                    RING_SQUARES[ringIndex], led, intent.r, intent.g, intent.b,
                    pixel.r, pixel.g, pixel.b, pixel.w);
    }
    // Literal expected ownership comes from this named journey: only the two
    // opening king brackets may reserve shared side LEDs. Menu/correction,
    // a real pickup and explicit assistance dismissal release those slots.
    const bool noAssist = std::strcmp(stage, "menu-item-0") == 0 ||
        std::strcmp(stage, "menu-item-1") == 0 ||
        std::strcmp(stage, "wrong-pickup") == 0 ||
        std::strcmp(stage, "fresh-pickup") == 0 ||
        std::strcmp(stage, "assistance-off") == 0 ||
        std::strcmp(stage, "dismissed") == 0 ||
        std::strcmp(stage, "dismissed-late") == 0;
    const uint64_t expectedMask = noAssist ? 0ULL : UINT64_C(0x1000000000000010);
    Check(session.SidePrioritySquareMask() == expectedMask, stage,
          "visible owner is exactly the opening king pair or explicitly released");
    CheckPhysical(session, hardware, rotate, stage);
    out.push_back({state, session.SemanticFrame()});
}

std::vector<Checkpoint> Journey(uint8_t hardware, bool rotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardware;
    config.board_rotate = rotate;
    test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
    std::vector<Checkpoint> checkpoints;
    if (hardware == VIRTUAL_LITCH_HARDWARE_C3) {
        Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P2,
                                    runtime::VirtualLitchPhysicalButton::X, true) ==
                  VIRTUAL_LITCH_OUT_OF_RANGE,
              "missing-bank", "C3 still rejects an unfitted physical P2 bank in either orientation");
    }
    const bool ready = session.Reset() == VIRTUAL_LITCH_OK && Observe(session, INITIAL, rotate) &&
        session.Advance(200U) == VIRTUAL_LITCH_OK && Set(session, "WHITE_SQUARES_ON", "false") &&
        Set(session, "TURN_ANIM_STYLE", "\"active_end_pingpong_sides\"") &&
        session.Advance(1U) == VIRTUAL_LITCH_OK;
    StaticJsonDocument<1024> home;
    const bool play = deserializeJson(home, session.HomeStateLine().CStr()) == DeserializationError::Ok &&
        home["pm"].as<int>() == 0 && std::strcmp(home["a"] | "", "idle") == 0;
    Check(ready && play && Idle(session), "ready", "stable raw opening and acknowledged settings reach Play ready");
    if (!ready || !play || !Idle(session)) return checkpoints;
    // Ready first starts the 1.8-second setup acknowledgement. Let it expire
    // before sampling the ordinary idle Turn animation.
    Check(session.Advance(2000U) == VIRTUAL_LITCH_OK && Idle(session),
          "ready", "setup acknowledgement expires before idle Turn sampling");
    Capture(session, hardware, rotate, "ready", "lift-a-piece-or-hold-X", checkpoints);
    for (uint8_t sample = 0; sample < 4U; ++sample) {
        Check(session.Advance(137U) == VIRTUAL_LITCH_OK, "idle-turn", "timed idle render advances");
        Capture(session, hardware, rotate, "idle-turn", "lift-a-piece-or-hold-X", checkpoints);
    }
    Check(Down(session, runtime::VirtualLitchPhysicalButton::X, true) &&
              session.Advance(350U) == VIRTUAL_LITCH_OK, "hold-X", "raw hold reaches progress preview");
    Capture(session, hardware, rotate, "hold-X", "keep-holding-to-open-Quick-Setup", checkpoints);
    Check(session.Advance(650U) == VIRTUAL_LITCH_OK &&
              Down(session, runtime::VirtualLitchPhysicalButton::X, false) &&
              session.QuickSettingsActive(), "menu", "one-second raw hold opens Quick Setup");
    Capture(session, hardware, rotate, "menu-item-0", "press-Right", checkpoints);
    Check(Down(session, runtime::VirtualLitchPhysicalButton::Right, true) &&
              session.QuickSettingsSelectedIndex() == 1U &&
              Down(session, runtime::VirtualLitchPhysicalButton::Right, false) &&
              session.QuickSettingsSelectedIndex() == 1U, "menu-item-1", "Right Press immediately moves selection one place");
    Capture(session, hardware, rotate, "menu-item-1", "tap-X-to-exit", checkpoints);
    Check(Tap(session, runtime::VirtualLitchPhysicalButton::X) && !session.QuickSettingsActive() &&
              session.Advance(1200U) == VIRTUAL_LITCH_OK && Idle(session),
          "menu-exit", "exit cancels menu ownership and timed feedback");
    Capture(session, hardware, rotate, "menu-exit", "lift-opponent-a8-rook", checkpoints);
    Check(Observe(session, INITIAL & ~UINT64_C(1), rotate) &&
              session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Correction),
          "wrong-pickup", "opponent corner pickup is rejected into physical correction");
    Capture(session, hardware, rotate, "wrong-pickup", "return-a8-rook", checkpoints);
    Check(session.Advance(modules::CORRECTION_QUIET_MS) == VIRTUAL_LITCH_OK,
          "wrong-pickup", "sustained mismatch reaches local amber guidance");
    const auto revision = hardware == VIRTUAL_LITCH_HARDWARE_C3
        ? hardware::BoardRevision::V1C3
        : hardware::BoardRevision::V2S3;
    bool physicalCornerVisible = false;
    for (const auto slot : {hardware::SquareLedSlot::Square0,
                            hardware::SquareLedSlot::Square2}) {
        const auto cornerTargets = hardware::MapSemanticSquareLedTargets(
            revision, 0U, slot, rotate);
        for (uint8_t index = 0U; index < cornerTargets.count; ++index) {
            physicalCornerVisible = physicalCornerVisible || Visible(
                session.PhysicalFrame().pixels[cornerTargets.indices[index]]);
        }
    }
    Check(Visible(session.SemanticFrame().board[0][0]) &&
              physicalCornerVisible,
          "wrong-pickup", "gentle corner return cue remains visible on shared square LEDs");
    Check(Observe(session, INITIAL, rotate) && Idle(session) &&
              session.Advance(1200U) == VIRTUAL_LITCH_OK && Idle(session),
          "recovered", "correct return cancels correction and expired feedback cannot revive it");
    Capture(session, hardware, rotate, "recovered", "lift-a2-pawn", checkpoints);
    Check(Observe(session, INITIAL & ~(UINT64_C(1) << 48U), rotate) &&
              session.State().selected_square == 48U && session.State().valid_destination_mask != 0ULL,
          "fresh-pickup", "fresh legal pickup has usable move guidance after recovery");
    Capture(session, hardware, rotate, "fresh-pickup", "move-to-a3-or-a4-or-return-a2", checkpoints);
    Check(Observe(session, INITIAL, rotate) && Idle(session) && session.UndoHistoryMoveCount() == 0U,
          "ready-again", "return to opening preserves game without adding history");
    Capture(session, hardware, rotate, "ready-again", "choose-next-move", checkpoints);
    Check(Set(session, "BOARD_SETUP_ASSISTANCE", "false") &&
              session.Advance(1U) == VIRTUAL_LITCH_OK,
          "assistance-off", "acknowledged assistance disable releases the pair");
    Capture(session, hardware, rotate, "assistance-off", "choose-next-move", checkpoints);
    Check(Set(session, "BOARD_SETUP_ASSISTANCE", "true") &&
              session.Advance(1U) == VIRTUAL_LITCH_OK,
          "assistance-restored", "enabled assistance resumes the armed pair");
    Capture(session, hardware, rotate, "assistance-restored", "tap-X-to-dismiss", checkpoints);
    Check(Tap(session, runtime::VirtualLitchPhysicalButton::X) && Idle(session),
          "dismissed", "raw X dismisses optional king assistance without changing the game");
    Capture(session, hardware, rotate, "dismissed", "choose-next-move", checkpoints);
    Check(session.Advance(2000U) == VIRTUAL_LITCH_OK && Idle(session) &&
              session.UndoHistoryMoveCount() == 0U,
          "dismissed-late", "expiry cannot restore dismissed guidance or add a move");
    Capture(session, hardware, rotate, "dismissed-late", "choose-next-move", checkpoints);
    return checkpoints;
}

void Compare(bool rotate) {
    // Execute separately: the production engine has a single shared native lane.
    const auto dedicated = Journey(VIRTUAL_LITCH_HARDWARE_S3, rotate);
    const auto virtualRing = Journey(VIRTUAL_LITCH_HARDWARE_C3, rotate);
    Check(dedicated.size() == 17U && virtualRing.size() == dedicated.size(), "paired", "both targets complete every named checkpoint");
    for (size_t i = 0; i < std::min(dedicated.size(), virtualRing.size()); ++i) {
        const auto& a = dedicated[i];
        const auto& b = virtualRing[i];
        Check(a.state.now_ms == b.state.now_ms && a.state.interaction_phase == b.state.interaction_phase &&
                  a.state.required_action == b.state.required_action && a.state.correction_mask == b.state.correction_mask &&
                  a.state.valid_destination_mask == b.state.valid_destination_mask,
              "paired", "same raw journey preserves authoritative timing, precedence and next action");
        bool same = true;
        for (uint8_t index = 0; index < 32U; ++index)
            same = same && Same(a.semantic.status[index], b.semantic.status[index]);
        for (uint8_t square = 0; square < 64U; ++square)
            for (uint8_t slot = 0; slot < 3U; ++slot)
                same = same && Same(a.semantic.board[square][slot], b.semantic.board[square][slot]);
        Check(same, "paired", "C3 and V2 share identical semantic status and square frames");
    }
}

void CheckDirectC3InputRejection() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_C3;
    virtual_litch* board = nullptr;
    Check(virtual_litch_create(&config, &board) == VIRTUAL_LITCH_OK && board,
          "direct-input", "direct C API board creation succeeds");
    if (!board) return;
    for (uint8_t owner : {VIRTUAL_LITCH_OWNER_NONE, VIRTUAL_LITCH_OWNER_P1}) {
        for (uint8_t button : {VIRTUAL_LITCH_BUTTON_P2_X, VIRTUAL_LITCH_BUTTON_P2_O}) {
            Check(virtual_litch_dispatch_button(board, owner, button,
                      VIRTUAL_LITCH_BUTTON_PRESS_EVENT) == VIRTUAL_LITCH_OUT_OF_RANGE,
                  "direct-input", "direct P2-specific buttons stay rejected for any owner tag");
        }
    }
    Check(virtual_litch_dispatch_button(board, VIRTUAL_LITCH_OWNER_P2,
              VIRTUAL_LITCH_BUTTON_LEFT, VIRTUAL_LITCH_BUTTON_PRESS_EVENT) ==
              VIRTUAL_LITCH_OUT_OF_RANGE,
          "direct-input", "direct unfitted P2 owner stays rejected");
    Check(virtual_litch_dispatch_button(board, 255U,
              VIRTUAL_LITCH_BUTTON_LEFT, VIRTUAL_LITCH_BUTTON_PRESS_EVENT) ==
              VIRTUAL_LITCH_INVALID_ENUM &&
          virtual_litch_dispatch_button(board, VIRTUAL_LITCH_OWNER_P1,
              255U, VIRTUAL_LITCH_BUTTON_PRESS_EVENT) == VIRTUAL_LITCH_INVALID_ENUM &&
          virtual_litch_dispatch_button(board, VIRTUAL_LITCH_OWNER_P1,
              VIRTUAL_LITCH_BUTTON_LEFT, 255U) == VIRTUAL_LITCH_INVALID_ENUM,
          "direct-input", "invalid direct owner/button/event enums remain rejected");
    virtual_litch_destroy(board);
}

void CheckV3_3Frame(const runtime::VirtualLitchSession& session,
                   const bool rotate, const char* stage,
                   const char* nextAction) {
    const auto& state = session.State();
    const auto& semantic = session.SemanticFrame();
    const auto& physical = session.PhysicalFrame();
    std::printf("[V3_3_STATUS_JOURNEY] rotate=%u stage=%s t=%u occupancy=%016llx "
                "phase=%u required=%u selected=%u correction=%016llx "
                "display=\"%.*s\" next=%s leds=%u channels=%u output_scale=%u\n",
                rotate, stage, state.now_ms,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                state.interaction_phase, state.required_action,
                state.selected_square,
                static_cast<unsigned long long>(state.correction_mask),
                static_cast<int>(session.DisplayText().size()),
                session.DisplayText().data(), nextAction,
                physical.led_count, physical.channels_per_led,
                physical.applied_brightness_scale_65535);
    Check(physical.led_count == 300U && physical.channels_per_led == 3U,
          stage, "V3.3 journey renders all 300 physical RGB outputs");
    Check(physical.applied_brightness_scale_65535 == 32768U,
          stage, "V3.3 applies its provisional half-output gain to real guidance");
    const auto scale = [&](uint8_t channel) {
        return static_cast<int>((static_cast<uint32_t>(channel) *
            physical.applied_brightness_scale_65535 + 32767U) / 65535U);
    };
    const auto near = [](int a, int b) { return a >= b - 1 && a <= b + 1; };
    uint8_t visiblePerimeter = 0U;
    for (uint8_t semanticIndex = 0U; semanticIndex < 32U; ++semanticIndex) {
        const uint8_t ring = static_cast<uint8_t>(
            (semanticIndex + (rotate ? 16U : 0U)) % 32U);
        // Independent physical-chain oracle: first six controls follow four
        // perimeter LEDs, second six follow twenty perimeter LEDs.
        const uint8_t chain = ring < 4U ? ring
            : static_cast<uint8_t>(ring < 20U ? ring + 6U : ring + 12U);
        const auto expected = semantic.status[semanticIndex];
        const auto actual = physical.pixels[256U + chain];
        visiblePerimeter += Visible(actual) ? 1U : 0U;
        Check(near(actual.r, scale(expected.r)) &&
                  near(actual.g, scale(expected.g)) &&
                  near(actual.b, scale(expected.b)) && actual.w == 0U,
              stage, "rotated perimeter cue reaches its inserted physical position");
    }
    if (std::strcmp(stage, "v3.3-invalid") == 0) {
        Check(visiblePerimeter > 0U, stage,
              "correction has visible physical perimeter guidance");
    }
    if (std::strcmp(stage, "v3.3-pickup") == 0) {
        bool destinationVisible = false;
        for (uint8_t square = 0U; square < 64U; ++square) {
            if ((state.valid_destination_mask & (UINT64_C(1) << square)) == 0ULL)
                continue;
            const uint8_t mapped = rotate ? static_cast<uint8_t>(63U - square) : square;
            for (uint8_t slot = 0U; slot < 4U; ++slot) {
                destinationVisible |= Visible(physical.pixels[4U * mapped + slot]);
            }
        }
        Check(destinationVisible, stage,
              "legal pickup has visible physical destination guidance");
    }
    for (const uint8_t first : {4U, 26U}) {
        for (uint8_t role = 0U; role < 6U; ++role) {
            Check(!Visible(physical.pixels[256U + first + role]), stage,
                  "unassigned O/direction/X control emitters stay dark");
        }
    }
}

void V3_3Journey(const bool rotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.brightness_scale_256 = 256U;
    config.max_led_current_ma = 20000U;
    config.board_rotate = rotate ? 1U : 0U;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    const bool ready = session.Reset() == VIRTUAL_LITCH_OK &&
        Observe(session, INITIAL, rotate) &&
        session.Advance(2200U) == VIRTUAL_LITCH_OK && Idle(session);
    Check(ready, "v3.3-ready", "opening occupancy reaches Play ready");
    if (!ready) return;
    CheckV3_3Frame(session, rotate, "v3.3-ready", "lift-a2-pawn");

    const bool invalid = Observe(session, INITIAL & ~UINT64_C(1), rotate) &&
        session.State().interaction_phase ==
            static_cast<uint8_t>(runtime::ChessInteractionPhase::Correction);
    Check(invalid, "v3.3-invalid", "opponent rook pickup is rejected");
    if (!invalid) return;
    CheckV3_3Frame(session, rotate, "v3.3-invalid", "return-a8-rook");
    Check(Observe(session, INITIAL, rotate) && Idle(session) &&
              session.Advance(1200U) == VIRTUAL_LITCH_OK,
          "v3.3-recovered", "returning rook clears correction and expiry");
    CheckV3_3Frame(session, rotate, "v3.3-recovered", "lift-a2-pawn");

    Check(Observe(session, INITIAL & ~(UINT64_C(1) << 48U), rotate) &&
              session.State().selected_square == 48U &&
              session.State().valid_destination_mask != 0ULL,
          "v3.3-pickup", "legal pickup produces physical next-action guidance");
    CheckV3_3Frame(session, rotate, "v3.3-pickup",
                   "move-to-a3-or-a4-or-return-a2");
    Check(Observe(session, INITIAL, rotate) && Idle(session),
          "v3.3-return", "returning the pawn restores ready state");
}

} // namespace

int main() {
    CheckDirectC3InputRejection();
    Compare(false);
    Compare(true);
    V3_3Journey(false);
    V3_3Journey(true);
    std::printf("[STATUS_PERIMETER_JOURNEY] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
