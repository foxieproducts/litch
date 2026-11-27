#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "bounded_hint_engine.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/user_input_timing.hpp"
#include "modules/player_end_palette.hpp"
#include "modules/ui_color_keys.hpp"
#include "runtime/virtual_litch_session.h"
#include "virtual_litch_session.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

uint8_t P1Wheel(runtime::VirtualLitchSession& session) {
    return static_cast<uint8_t>(
        modules::player_end_palette::ReadSettingUInt32(
            session.SettingsForPersistenceTest(),
            modules::ui_color_keys::P1_END_PIECE_WHEEL, 0U));
}

void CheckHeldPaletteRepeat(const uint8_t hardware, const char* revision) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardware;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;

    Check(session.Reset() == VIRTUAL_LITCH_OK,
          "palette-repeat journey resets the production session");
    auto& settings = session.SettingsForPersistenceTest();
    settings.Doc()[modules::ui_color_keys::P1_END_PIECE_WHEEL] = 0U;
    settings.Doc()[modules::ui_color_keys::P2_END_PIECE_WHEEL] = 128U;
    modules::player_end_palette::SynchronizeLegacyAliases(settings.Doc());
    Check(session.ObserveHallMask(UINT64_C(0xFFFF00000000FFFF)) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK,
          "palette-repeat journey reaches idle starting position");

    Check(session.SetButtonDown(
              VIRTUAL_LITCH_OWNER_P1,
              runtime::VirtualLitchPhysicalButton::Up, true) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              P1Wheel(session) == 1U,
          "held physical Up applies its initial palette step after debounce");

    Check(session.Advance(
              hardware::user_input_timing::DPAD_BEFORE_REPEAT_MS - 1U) ==
              VIRTUAL_LITCH_OK &&
              P1Wheel(session) == 1U,
          "held physical Up does not repeat before the shared delay");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
              P1Wheel(session) == 3U,
          "held physical Up repeats at the shared delay boundary");

    for (uint8_t repeat = 0U; repeat < 4U; ++repeat) {
        Check(session.Advance(
                  hardware::user_input_timing::DPAD_VALUE_REPEAT_RATE_MS) ==
                  VIRTUAL_LITCH_OK,
              "held physical Up accepts the next repeat interval");
    }
    Check(P1Wheel(session) == 11U,
          "held physical Up changes player color at the accelerated cadence");
    Check(session.SetButtonDown(
              VIRTUAL_LITCH_OWNER_P1,
              runtime::VirtualLitchPhysicalButton::Up, false) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK,
          "palette-repeat journey releases the physical control");

    std::printf(
        "[PALETTE_REPEAT_CHECKPOINT] revision=%s first_ms=%u repeat_ms=%u "
        "tap_steps=1 repeat_steps=2 total_steps=11 final_wheel=%u next=release\n",
        revision,
        static_cast<unsigned>(
            hardware::user_input_timing::DPAD_BEFORE_REPEAT_MS),
        static_cast<unsigned>(
            hardware::user_input_timing::DPAD_VALUE_REPEAT_RATE_MS),
        static_cast<unsigned>(P1Wheel(session)));
}

unsigned Peak(const virtual_litch_color color) {
    return std::max({color.r, color.g, color.b, color.w});
}

void CheckCoachingHintThroughPaletteHold() {
    constexpr auto REVISION = hardware::BoardRevision::V3_3S3;
    constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
    using Button = runtime::VirtualLitchPhysicalButton;
    using namespace hardware::user_input_timing;

    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    auto& settings = session.SettingsForPersistenceTest();
    settings.Doc()[modules::ui_color_keys::P1_END_PIECE_WHEEL] = 0U;
    settings.Doc()[modules::ui_color_keys::P2_END_PIECE_WHEEL] = 128U;
    modules::player_end_palette::SynchronizeLegacyAliases(settings.Doc());
    modules::ControlMutationOutcome coaching;
    const bool ready = session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
        session.Advance(3000U) == VIRTUAL_LITCH_OK &&
        session.MutateSettingJson(1U, "P1_AUTO_HINTS", "true", coaching) ==
            VIRTUAL_LITCH_OK &&
        coaching.status == modules::ControlMutationStatus::Applied &&
        session.Advance(2000U) == VIRTUAL_LITCH_OK;
    const auto initial = session.HomeStateSnapshot().assistance;
    const auto from = initial.fromSquare;
    const auto to = initial.toSquare;
    const bool shown = ready && initial.coachingForced && initial.hintRequested &&
        from < 64U && to < 64U && (OPENING & (UINT64_C(1) << from)) != 0U &&
        (OPENING & (UINT64_C(1) << to)) == 0U;
    Check(shown, "V3.3 Coaching reveals a real opening Hint before the palette edit");
    if (!shown) return;

    constexpr auto PIECE = hardware::SquareLedSlot::Piece;
    const auto source = hardware::MapSemanticSquareLedTargets(REVISION, from, PIECE, false);
    const auto landing = hardware::MapSemanticSquareLedTargets(REVISION, to, PIECE, false);
    bool topLead = false;
    bool bottomLead = false;
    bool centerPairTogether = true;
    const int ranks = int(to / 8U) - from / 8U;
    const int files = int(to % 8U) - from % 8U;
    const bool knight = (std::abs(ranks) == 2 && std::abs(files) == 1) ||
                        (std::abs(ranks) == 1 && std::abs(files) == 2);
    const bool horizontalLanding = knight ? std::abs(ranks) == 2 : ranks == 0;
    bool uninterrupted = true;
    const auto sample = [&](const char* stage = nullptr) {
        const auto& state = session.State();
        const auto hint = session.HomeStateSnapshot().assistance;
        const auto& semantic = session.SemanticFrame();
        const auto& mapped = session.PhysicalFrame();
        const auto fromPiece = Peak(semantic.board[from][static_cast<uint8_t>(PIECE)]);
        const auto toPiece = Peak(semantic.board[to][static_cast<uint8_t>(PIECE)]);
        const auto fromTop = Peak(mapped.pixels[source.indices[0]]);
        const auto fromBottom = Peak(mapped.pixels[source.indices[1]]);
        const auto toTop = Peak(mapped.pixels[landing.indices[0]]);
        const auto toBottom = Peak(mapped.pixels[landing.indices[1]]);
        const auto topColor = mapped.pixels[landing.indices[0]];
        const auto bottomColor = mapped.pixels[landing.indices[1]];
        centerPairTogether = centerPairTogether && topColor.r == bottomColor.r &&
            topColor.g == bottomColor.g && topColor.b == bottomColor.b && topColor.w == bottomColor.w;
        const bool blinkOn = state.now_ms % 800U < 600U;
        const bool steady = state.physical_occupied_mask == OPENING &&
            state.interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
            hint.coachingForced && hint.hintRequested &&
            hint.fromSquare == from && hint.toSquare == to &&
            (fromPiece > 0U) == blinkOn && (fromTop > 0U) == blinkOn &&
            (fromBottom > 0U) == blinkOn && toPiece > 0U && toTop > 0U && toBottom > 0U;
        if (stage != nullptr || (!steady && uninterrupted)) {
            const auto display = session.DisplayText();
            std::printf("[PALETTE_HINT] stage=%s result=%s t=%u occupancy=%016llx "
                        "phase=%u display=\"%.*s\" hint=%u>%u wheel=%u "
                        "source=%u/%u/%u landing=%u/%u/%u next=consider-Hint\n",
                        stage != nullptr ? stage : "repeat", steady ? "visible" : "interrupted",
                        state.now_ms, static_cast<unsigned long long>(state.physical_occupied_mask),
                        state.interaction_phase, static_cast<int>(display.size()),
                        display.empty() ? "" : display.data(), hint.fromSquare, hint.toSquare,
                        P1Wheel(session), fromPiece, fromTop, fromBottom, toPiece, toTop, toBottom);
        }
        uninterrupted = uninterrupted && steady;
        return toTop > toBottom ? 1 : toBottom > toTop ? -1 : 0;
    };

    sample("before-edit");
    Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, Button::Up, true) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(BUTTON_DEBOUNCE_MS) == VIRTUAL_LITCH_OK &&
              P1Wheel(session) == 1U,
          "V3.3 physical Up applies the palette step while Coaching is visible");
    sample("debounced-press");
    Check(session.Advance(DPAD_BEFORE_REPEAT_MS) == VIRTUAL_LITCH_OK &&
              P1Wheel(session) == 3U,
          "V3.3 held Up reaches the first accelerated palette step");
    for (unsigned repeat = 0U; repeat <= 90U; ++repeat) {
        const int lead = sample(repeat == 0U ? "first-repeat" : nullptr);
        topLead = topLead || lead > 0;
        bottomLead = bottomLead || lead < 0;
        if (repeat != 90U) {
            Check(session.Advance(DPAD_VALUE_REPEAT_RATE_MS) == VIRTUAL_LITCH_OK,
                  "V3.3 samples the board on the actual repeat cadence");
        }
    }
    const auto heldWheel = P1Wheel(session);
    Check(heldWheel != 3U &&
              (horizontalLanding ? centerPairTogether : topLead && bottomLead),
          "the mapped Hint retains the final horizontal center pair or vertical emitter order while the palette changes");
    sample("end-of-hold");
    Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, Button::Up, false) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(BUTTON_DEBOUNCE_MS) == VIRTUAL_LITCH_OK &&
              session.Advance(DPAD_BEFORE_REPEAT_MS) == VIRTUAL_LITCH_OK &&
              P1Wheel(session) == heldWheel,
          "release retires palette repeat and keeps the edit");
    sample("released-repeat-expired");
    Check(uninterrupted,
          "the same Coaching Hint keeps its semantic and mapped source blink and landing through the palette hold");
}

}  // namespace

int main() {
    std::puts("[PALETTE_REPEAT_VIRTUAL_LITCH_JOURNEY] start");
    CheckHeldPaletteRepeat(VIRTUAL_LITCH_HARDWARE_S3, "V2S3");
    CheckHeldPaletteRepeat(VIRTUAL_LITCH_HARDWARE_V3, "V3S3");
    CheckCoachingHintThroughPaletteHold();

    if (g_failures == 0) {
        std::puts("[PALETTE_REPEAT_VIRTUAL_LITCH_JOURNEY] ok");
        return 0;
    }
    std::printf("[PALETTE_REPEAT_VIRTUAL_LITCH_JOURNEY] failures=%d\n",
                g_failures);
    return 1;
}
