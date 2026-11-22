#include <cstdio>
#include <initializer_list>

#include "bounded_hint_engine.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/correction_feedback.hpp"
#include "modules/orientation.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace {

constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t SOURCES[] = {52U, 12U, 51U, 11U}; // e2, e7, d2, d7
constexpr uint8_t DESTINATIONS[] = {36U, 28U, 43U, 19U}; // e4, e5, d3, d6
using Button = runtime::VirtualLitchPhysicalButton;
int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) return;
    ++g_failures;
    std::printf("[FAIL] NEW_GAME_SETUP_COLORS: %s\n", message);
}

bool Equal(const virtual_litch_color a, const virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}

bool Black(const virtual_litch_color color) {
    return color.r == 0U && color.g == 0U && color.b == 0U && color.w == 0U;
}

bool OwnerColor(const virtual_litch_color color, const bool blue) {
    return blue ? color.b > color.r && color.b > color.g
                : color.r > color.b && color.r > color.g;
}

bool Observe(runtime::VirtualLitchSession& session, const uint64_t logical) {
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    uint64_t physical = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((logical & (UINT64_C(1) << square)) != 0ULL) {
            physical |= UINT64_C(1) << modules::MapSquareToPhysical(square, rotate);
        }
    }
    return session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
        session.State().physical_occupied_mask == logical;
}

bool Set(runtime::VirtualLitchSession& session, const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return session.MutateSettingJson(450U, key, value, outcome) == VIRTUAL_LITCH_OK &&
        (outcome.status == modules::ControlMutationStatus::Applied ||
         outcome.status == modules::ControlMutationStatus::Noop);
}

bool ButtonDown(runtime::VirtualLitchSession& session, const Button button,
                const bool down) {
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    return session.SetButtonDown(rotate ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1,
                                 button, down) == VIRTUAL_LITCH_OK &&
        session.Advance(30U) == VIRTUAL_LITCH_OK;
}

bool Move(runtime::VirtualLitchSession& session, uint64_t& occupancy,
          const uint8_t from, const uint8_t to) {
    const uint8_t history = session.UndoHistoryMoveCount();
    occupancy &= ~(UINT64_C(1) << from);
    if (!Observe(session, occupancy)) return false;
    occupancy |= UINT64_C(1) << to;
    return Observe(session, occupancy) && session.Advance(2200U) == VIRTUAL_LITCH_OK &&
        session.UndoHistoryMoveCount() == history + 1U &&
        session.State().expected_occupied_mask == occupancy &&
        session.State().pending_action_token == 0U;
}

bool RedFault(const runtime::VirtualLitchSession& session) {
    for (const auto color : session.SemanticFrame().status) {
        if (color.r == 0U || color.r <= color.g || color.r <= color.b) return false;
    }
    return true;
}

uint8_t StatusPalettes(const runtime::VirtualLitchSession& session) {
    uint8_t palettes = 0U;
    for (const auto color : session.SemanticFrame().status) {
        if (OwnerColor(color, false)) palettes |= 1U;
        if (OwnerColor(color, true)) palettes |= 2U;
    }
    return palettes;
}

void Checkpoint(const runtime::VirtualLitchSession& session, const char* stage,
                const char* input, const char* result, const char* next,
                const char* expiry) {
    const auto& state = session.State();
    const auto home = session.HomeStateSnapshot();
    const auto display = session.DisplayText();
    std::printf("[NEW_GAME_SETUP_COLORS] swapped=%u stage=%s t=%u input=%s "
                "occupancy=%016llx expected=%016llx mode=%s phase=%s placement=%u "
                "action=%u token=%u history=%u result=%s display=\"%.*s\" "
                "candidate=%u fault=%u status_palettes=%u next=%s expiry=%s\n",
                session.FirstPlayerIsP2(), stage, state.now_ms, input,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                home.freshSetup ? "FreshSetup" : "Gameplay",
                runtime::ChessInteractionPhaseToString(
                    static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                home.placementMode, unsigned(home.action), state.pending_action_token,
                session.UndoHistoryMoveCount(), result,
                static_cast<int>(display.size()), display.empty() ? "" : display.data(),
                session.SetupDuckCandidateSquare(), RedFault(session), StatusPalettes(session),
                next, expiry);
    for (const uint8_t square : {12U, 52U, 19U, 28U, 36U, 43U}) {
        std::printf("[NEW_GAME_SETUP_LEDS] square=%u slots=", square);
        for (const auto color : session.SemanticFrame().board[square]) {
            std::printf("%u/%u/%u/%u;", color.r, color.g, color.b, color.w);
        }
        const bool rotate = modules::ComputeSquareRotate(
            session.BoardRotate(), session.FirstPlayerIsP2());
        const auto target = hardware::MapSemanticSquareLedTargets(
            hardware::BoardRevision::V3S3, square, hardware::SquareLedSlot::Square0, rotate);
        const auto physical = session.PhysicalFrame().pixels[target.indices[0U]];
        std::printf(" physical-left=%u:%u/%u/%u/%u\n", target.indices[0U],
                    physical.r, physical.g, physical.b, physical.w);
    }
}

void CheckSetupColors(const runtime::VirtualLitchSession& session) {
    bool guidance = true;
    bool detection = true;
    bool mappedGuidance = true;
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((OPENING & (UINT64_C(1) << square)) == 0ULL) continue;
        const bool blue = (square < 16U) != session.FirstPlayerIsP2();
        const bool occupied = (session.State().physical_occupied_mask &
                               (UINT64_C(1) << square)) != 0ULL;
        const auto& leds = session.SemanticFrame().board[square];
        if (occupied) {
            detection = detection && OwnerColor(leds[1U], blue);
            continue;
        }
        detection = detection && Black(leds[1U]);
        guidance = guidance && OwnerColor(leds[0U], blue) && Equal(leds[0U], leds[2U]);
        for (const auto slot : {hardware::SquareLedSlot::Square0,
                                hardware::SquareLedSlot::Square2}) {
            const auto targets = hardware::MapSemanticSquareLedTargets(
                hardware::BoardRevision::V3S3, square, slot, rotate);
            mappedGuidance = mappedGuidance && targets.count != 0U;
            for (uint8_t index = 0U; index < targets.count; ++index) {
                mappedGuidance = mappedGuidance && OwnerColor(
                    session.PhysicalFrame().pixels[targets.indices[index]], blue);
            }
        }
    }
    Check(guidance, "both starting sides keep owner-color placement pairs while leftovers remain");
    Check(detection, "occupied opening centers keep their owner and missing centers stay black");
    Check(mappedGuidance, "V3 physical side LEDs preserve both placement colors after orientation mapping");
}

void CheckExtras(const runtime::VirtualLitchSession& session) {
    bool removal = true;
    const uint64_t extras = session.State().physical_occupied_mask & ~OPENING;
    for (uint8_t square = 16U; square < 48U; ++square) {
        if ((extras & (UINT64_C(1) << square)) == 0ULL) continue;
        const auto& leds = session.SemanticFrame().board[square];
        if (square == session.SetupDuckCandidateSquare()) {
            removal = removal && leds[1U].r == 255U && leds[1U].g == 255U &&
                leds[1U].b == 0U;
        } else {
            removal = removal && !Equal(leds[0U], leds[2U]) && !Black(leds[1U]);
        }
    }
    Check(removal, "ordinary extras retain rainbow removal cues and the eligible Duck stays yellow");
}

void RunNewGameSetupColors(const bool swapped) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.first_player_is_p2 = swapped;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    uint64_t occupancy = OPENING;
    Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, occupancy) &&
              session.Advance(2200U) == VIRTUAL_LITCH_OK &&
              Set(session, "P2_END_PIECE_WHEEL", "85") &&
              Set(session, "P1_END_PIECE_WHEEL", "0") &&
              Set(session, "P2_END_PIECE_WHEEL", "170"),
          "ordinary opening reaches ready with distinct configured end palettes");
    for (uint8_t move = 0U; move < 4U; ++move) {
        Check(Move(session, occupancy, SOURCES[move], DESTINATIONS[move]),
              "real pickup/placement/settle/action acknowledgement commits each opening move once");
    }
    Checkpoint(session, "prior-game", "e2-e4,e7-e5,d2-d3,d7-d6", "four-moves-applied",
               "hold-same-owner-X+O", "none");
    Check(ButtonDown(session, Button::X, true) && ButtonDown(session, Button::O, true) &&
              session.Advance(500U) == VIRTUAL_LITCH_OK &&
              ButtonDown(session, Button::X, false) && ButtonDown(session, Button::O, false) &&
              session.UndoHistoryMoveCount() == 4U &&
              session.State().expected_occupied_mask == occupancy,
          "releasing New Game early cancels the hold without changing the live game");
    Checkpoint(session, "reset-canceled", "X+O-release-before-two-seconds", "canceled",
               "retry-New-Game-hold", "hold-canceled-on-release");
    Check(ButtonDown(session, Button::X, true) && ButtonDown(session, Button::O, true),
          "raw same-owner X+O starts production New Game hold");
    modules::ControlMutationOutcome outcome;
    bool admitted = false;
    bool applied = false;
    uint16_t requestId = 0U;
    for (uint32_t heldMs = 0U; heldMs < 2100U && !applied; ++heldMs) {
        if (session.Advance(1U) != VIRTUAL_LITCH_OK) break;
        if (!session.TryGetLastBoardControlOutcome(outcome) ||
            outcome.action != modules::ControlAction::NewGame) continue;
        if (outcome.status == modules::ControlMutationStatus::Accepted ||
            outcome.status == modules::ControlMutationStatus::Applied) {
            if (!admitted) {
                Checkpoint(session, "reset-acknowledgement", "X+O-held-two-seconds",
                           outcome.status == modules::ControlMutationStatus::Applied
                               ? "applied-synchronously" : "accepted-pending",
                           "arrange-opening", "one-control-request");
            }
            admitted = true;
            requestId = outcome.requestId;
        }
        applied = outcome.status == modules::ControlMutationStatus::Applied &&
            outcome.requestId == requestId && requestId != 0U;
    }
    Check(admitted,
          "board New Game is admitted through the real control owner");
    Check(applied &&
              session.UndoHistoryMoveCount() == 0U &&
              session.State().expected_occupied_mask == OPENING &&
              session.State().physical_occupied_mask == occupancy,
          "New Game acknowledgement resets logical opening and history while preserving physical pieces");
    Check(ButtonDown(session, Button::X, false) && ButtonDown(session, Button::O, false),
          "release reset chord after its acknowledged action");
    Checkpoint(session, "quiet-leftovers", "release-X+O", "setup-requires-arrangement",
               "restore-home-squares", "ordinary-extras-quiet-before-300ms");
    CheckSetupColors(session);
    Check(session.Advance(modules::CORRECTION_QUIET_MS) == VIRTUAL_LITCH_OK,
          "advance leftover feedback into its gentle stage");
    Checkpoint(session, "gentle-leftovers", "wait-300ms", "setup-requires-arrangement",
               "restore-home-squares", "ordinary-extras-gentle-before-1500ms");
    CheckSetupColors(session);
    Check(session.Advance(modules::CORRECTION_STRONG_MS) == VIRTUAL_LITCH_OK,
          "persistent leftovers reach strong correction");
    Checkpoint(session, "all-four-middle-rows", "New-Game-applied-with-pieces-still-on-d6,e5,e4,d3",
               "setup-requires-arrangement", "return-pieces-to-e2,e7,d2,d7", "correction-until-corrected");
    Check(session.HomeStateSnapshot().freshSetup && session.PlacementModeActive() &&
              session.DisplayText().empty() && RedFault(session),
          "Fresh Setup shows required extra correction without a text overlay");
    CheckSetupColors(session);
    CheckExtras(session);

    // A misplaced pawn must not commit a move or hide either opening side.
    occupancy = (occupancy & ~(UINT64_C(1) << 36U)) | (UINT64_C(1) << 34U);
    Check(Observe(session, occupancy) && session.Advance(1500U) == VIRTUAL_LITCH_OK &&
              session.UndoHistoryMoveCount() == 0U && session.State().pending_action_token == 0U &&
              session.PlacementModeActive(),
          "wrong-row replacement remains corrective setup without a chess action");
    Checkpoint(session, "wrong-row-replacement", "e4-to-c4", "requires-correction",
               "place-that-piece-on-e2", "correction-until-corrected");
    CheckSetupColors(session);
    CheckExtras(session);
    occupancy = (occupancy & ~(UINT64_C(1) << 34U)) | (UINT64_C(1) << 52U);
    Check(Observe(session, occupancy) && session.Advance(1500U) == VIRTUAL_LITCH_OK,
          "recover misplaced pawn to e2 while three middle rows remain occupied");
    Checkpoint(session, "one-pawn-restored", "c4-to-e2", "partial-setup-accepted",
               "restore-e7,d2,d7", "no-ready-sweep-while-incomplete");
    CheckSetupColors(session);
    CheckExtras(session);
    for (uint8_t piece = 2U; piece < 4U; ++piece) {
        occupancy = (occupancy & ~(UINT64_C(1) << DESTINATIONS[piece])) |
                    (UINT64_C(1) << SOURCES[piece]);
        Check(Observe(session, occupancy) && session.Advance(1500U) == VIRTUAL_LITCH_OK,
              "restore another pawn without clearing the remaining middle-row piece first");
        Checkpoint(session, piece == 2U ? "two-middle-rows" : "last-middle-row",
                   piece == 2U ? "d3-to-d2" : "d6-to-d7", "partial-setup-accepted",
                   "return-e5-to-e7", "no-ready-sweep-while-incomplete");
        CheckSetupColors(session);
        CheckExtras(session);
    }
    occupancy = OPENING;
    Check(Observe(session, occupancy) && session.Advance(199U) == VIRTUAL_LITCH_OK &&
              session.PlacementModeActive() && session.Advance(1U) == VIRTUAL_LITCH_OK &&
              !session.PlacementModeActive() && session.UndoHistoryMoveCount() == 0U,
          "clearing the final middle-row piece into its home square reaches ready at 200 ms");
    Checkpoint(session, "ready-acknowledgement", "e5-to-e7;stable-200ms", "ready",
               "play-or-adjust-setup", "two-player-sweep-1800ms");
    Check(StatusPalettes(session) == 3U,
          "first complete opening acknowledges both players on the perimeter");
    CheckSetupColors(session);
    Check(Observe(session, OPENING & ~(UINT64_C(1) << 52U)) &&
              session.Advance(1U) == VIRTUAL_LITCH_OK,
          "changing occupancy cancels the ready acknowledgement immediately");
    Checkpoint(session, "ready-canceled-by-pickup", "lift-e2", "pickup-accepted",
               "return-e2-or-play", "ready-sweep-canceled-on-board-change");
    Check(StatusPalettes(session) != 3U,
          "new pickup cancels the two-player acknowledgement without hiding its next action");
    Check(Observe(session, OPENING) && session.Advance(2200U) == VIRTUAL_LITCH_OK &&
              !session.PlacementModeActive() && !RedFault(session) &&
              StatusPalettes(session) == (swapped ? 2U : 1U),
          "returned pickup recovers calm ready after the bounded acknowledgement expires");
    Checkpoint(session, "acknowledgement-expired", "return-e2;wait-2200ms", "ready",
               "play-e2-e4", "sweep-expired-Turn-restored");
    Check(Move(session, occupancy, 52U, 36U),
          "recovered setup accepts the next normal move exactly once");
    Checkpoint(session, "next-game-playable", "e2-e4", "move-applied",
               "P2-to-move", "settle-expired");
}

} // namespace

int main() {
    RunNewGameSetupColors(false);
    RunNewGameSetupColors(true);
    std::printf("[NEW_GAME_SETUP_COLORS] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
