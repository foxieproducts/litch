#include "bounded_hint_engine.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <ArduinoJson.h>

#include "virtual_litch_frame_checks.hpp"
#include "virtual_litch_session.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/orientation.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/square_mapping.hpp"

namespace {

constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t DUCK_START = 24U;
int failures = 0;

void Check(bool condition, const char* run, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s: %s\n", run, message);
        ++failures;
    }
}

bool Rotated(const runtime::VirtualLitchSession& session) {
    return modules::ComputeSquareRotate(session.BoardRotate(), session.FirstPlayerIsP2());
}

bool Observe(runtime::VirtualLitchSession& session, uint64_t logical) {
    uint64_t physical = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((logical & (UINT64_C(1) << square)) != 0ULL) {
            physical |= UINT64_C(1) << runtime::MapSquareIndex(square, Rotated(session));
        }
    }
    return session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK;
}

modules::UiOwner PhysicalOwner(const runtime::VirtualLitchSession& session, bool black) {
    return modules::MapOwnerToPhysicalHomeEdge(
        black ? modules::UiOwner::P2 : modules::UiOwner::P1,
        modules::ComputeControlBankSwap(session.BoardRotate(), session.FirstPlayerIsP2(), false));
}

bool Button(runtime::VirtualLitchSession& session, modules::UiOwner owner,
            runtime::VirtualLitchPhysicalButton button, bool down) {
    return session.SetButtonDown(owner == modules::UiOwner::P2
                                     ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1,
                                 button, down) == VIRTUAL_LITCH_OK &&
           session.Advance(20U) == VIRTUAL_LITCH_OK;
}

bool Tap(runtime::VirtualLitchSession& session, modules::UiOwner owner,
         runtime::VirtualLitchPhysicalButton button) {
    return Button(session, owner, button, true) && Button(session, owner, button, false);
}

bool Coaching(runtime::VirtualLitchSession& session, modules::UiOwner owner, bool enabled) {
    bool ok = Button(session, owner, runtime::VirtualLitchPhysicalButton::O, true);
    for (uint8_t i = 0U; i < 11U; ++i) {
        ok = ok && session.Advance(100U) == VIRTUAL_LITCH_OK;
    }
    ok = ok && Button(session, owner, runtime::VirtualLitchPhysicalButton::O, false);
    modules::ControlMutationOutcome result;
    ok = ok && session.TryGetLastBoardControlOutcome(result) &&
         result.action == modules::ControlAction::Coaching &&
         result.status == modules::ControlMutationStatus::Applied;
    if (!ok || session.Advance(1000U) != VIRTUAL_LITCH_OK) return false;
    const auto& frame = session.PhysicalFrame();
    if (frame.led_count != 300U) {
        return virtual_litch_test::HasPhysicalCoachingBadge(frame, owner) == enabled;
    }
    // V3.3 has RGB emitters and inserted control LEDs. Read the same four
    // semantic badge positions through its production status-chain mapping.
    const std::array<uint8_t, 4> corners = owner == modules::UiOwner::P2
        ? std::array<uint8_t, 4>{0U, 7U, 8U, 31U}
        : std::array<uint8_t, 4>{16U, 23U, 24U, 15U};
    bool visible = true;
    for (const uint8_t corner : corners) {
        const auto color = frame.pixels[256U +
            hardware::V3_3PhysicalIndexForSemanticStatus(corner)];
        visible = visible && color.r != 0U && color.g != 0U && color.b != 0U;
    }
    return visible == enabled;
}

uint16_t Energy(virtual_litch_color color) {
    return static_cast<uint16_t>(color.r) + color.g + color.b + color.w;
}

bool Pair(const runtime::VirtualLitchSession& session, uint8_t logical) {
    if (logical >= 64U) {
        return false;
    }
    const auto revision = session.PhysicalFrame().led_count == 300U
        ? hardware::BoardRevision::V3_3S3 : hardware::BoardRevision::V2S3;
    const auto targets = hardware::MapSemanticSquareLedTargets(
        revision, logical, hardware::SquareLedSlot::Square0, Rotated(session));
    return Energy(session.SemanticFrame().board[logical][0]) != 0U &&
           Energy(session.PhysicalFrame().pixels[targets.indices[0U]]) != 0U;
}

bool CorrectionColor(virtual_litch_color color) {
    // Amber/red correction is distinct from a light square's gray background,
    // including after RGBW mapping extracts a white channel.
    return color.r > color.g && color.g >= color.b;
}

bool CorrectionPair(const runtime::VirtualLitchSession& session, uint8_t logical) {
    if (logical >= 64U) {
        return false;
    }
    const uint8_t physical = runtime::MapSquareIndex(logical, Rotated(session));
    return CorrectionColor(session.SemanticFrame().board[logical][0]) &&
           CorrectionColor(session.PhysicalFrame().pixels[hardware::LedIndexForSquare(
               physical, hardware::SquareLedSlot::Square0)]);
}

void CheckDuckYellow(const runtime::VirtualLitchSession& session,
                     const char* run, uint8_t logical) {
    const uint8_t physical = runtime::MapSquareIndex(logical, Rotated(session));
    const auto semantic = session.SemanticFrame().board[logical][0];
    const auto mapped = session.PhysicalFrame().pixels[hardware::LedIndexForSquare(
        physical, hardware::SquareLedSlot::Square0)];
    const auto yellow = [](virtual_litch_color color) {
        return color.r != 0U && color.r == color.g && color.b == 0U && color.w == 0U;
    };
    Check(yellow(semantic) && yellow(mapped), run,
          "selectable Duck Hint target is yellow in semantic and mapped RGBW output");
    std::printf("[AUTOMATIC_COACHING_DUCK_COLOR] run=%s square=%u physical=%u "
                "semantic=%u,%u,%u,%u mapped=%u,%u,%u,%u\n",
                run, logical, physical, semantic.r, semantic.g, semantic.b, semantic.w,
                mapped.r, mapped.g, mapped.b, mapped.w);
}

void Checkpoint(const runtime::VirtualLitchSession& session, const char* run,
                const char* stage, const char* next,
                modules::HomeStateNotificationPublisher& publisher,
                const bool visibleLandingExpected = false,
                const uint8_t directedFrom = 64U, const uint8_t directedTo = 64U) {
    uint64_t emptyLit = 0ULL;
    uint64_t pairs = 0ULL;
    const auto& state = session.State();
    for (uint8_t square = 0U; square < 64U; ++square) {
        const uint64_t bit = UINT64_C(1) << square;
        const auto revision = session.PhysicalFrame().led_count == 300U
            ? hardware::BoardRevision::V3_3S3 : hardware::BoardRevision::V2S3;
        const auto targets = hardware::MapSemanticSquareLedTargets(
            revision, square, hardware::SquareLedSlot::Piece, Rotated(session));
        uint16_t physicalEnergy = 0U;
        for (uint8_t i = 0U; i < targets.count; ++i) {
            physicalEnergy += Energy(session.PhysicalFrame().pixels[targets.indices[i]]);
        }
        if ((state.physical_occupied_mask & bit) == 0ULL &&
            (Energy(session.SemanticFrame().board[square][1]) != 0U ||
             physicalEnergy != 0U)) {
            emptyLit |= bit;
        }
        if (Pair(session, square)) {
            pairs |= bit;
        }
    }
    modules::HomeStateNotificationLine home;
    const bool published = session.TryPrepareHomePublication(publisher, true, home);
    const auto display = session.DisplayText();
    std::printf("[AUTOMATIC_COACHING_WHOLE_TURN] run=%s stage=%s t=%u "
                "occupied=%016llx expected=%016llx phase=%u action=%u token=%u "
                "hint=%u empty_centers=%016llx pairs=%016llx display=\"%.*s\" "
                "home_published=%u home=%s next=%s\n",
                run, stage, state.now_ms,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                state.interaction_phase, state.required_action, state.pending_action_token,
                session.HintDisplayRequested(), static_cast<unsigned long long>(emptyLit),
                static_cast<unsigned long long>(pairs), static_cast<int>(display.size()),
                display.empty() ? "" : display.data(), published, home.CStr(), next);
    uint64_t allowedLanding = 0ULL;
    const auto addLine = [&](const uint8_t source, const uint8_t target) {
        if (source >= 64U || target >= 64U) return;
        const int ranks = int(target / 8U) - source / 8U;
        const int files = int(target % 8U) - source % 8U;
        const int distance = std::max(std::abs(ranks), std::abs(files));
        for (int step = 0; step <= distance; ++step) {
            const int rank = source / 8U + (distance == 0 ? 0 :
                ((ranks > 0) - (ranks < 0)) * ((std::abs(ranks) * step + distance / 2) / distance));
            const int file = source % 8U + (distance == 0 ? 0 :
                ((files > 0) - (files < 0)) * ((std::abs(files) * step + distance / 2) / distance));
            allowedLanding |= UINT64_C(1) << (rank * 8 + file);
        }
    };
    const auto fixed = session.HomeStateSnapshot();
    const bool physicalMovement = state.required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::LiftPrimary) ||
        state.required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::PlacePrimary);
    if (physicalMovement && state.correction_mask == 0ULL) {
        if (fixed.duckRelocationOwner == 'u' || fixed.duckRelocationOwner == 'c')
            addLine(fixed.duckRelocationFromSquare, fixed.duckRelocationToSquare);
        if (session.IsUndoActionPending()) addLine(directedFrom, directedTo);
    }
    if (visibleLandingExpected) {
        const auto projection = session.HomeStateSnapshot();
        const auto assistance = projection.assistance;
        const auto presentation = assistance.presentation;
        const bool duckRelocation = presentation == modules::HomeAssistancePresentation::DuckRelocation;
        const uint8_t source = duckRelocation ? assistance.duckFromSquare : assistance.fromSquare;
        const uint8_t target = duckRelocation ? assistance.duckToSquare : assistance.toSquare;
        const bool visible = assistance.valid && assistance.hintRequested && target < 64U &&
            (presentation == modules::HomeAssistancePresentation::HintReady ||
             presentation == modules::HomeAssistancePresentation::HintRefreshing ||
             presentation == modules::HomeAssistancePresentation::DuckCompound ||
             presentation == modules::HomeAssistancePresentation::DuckRelocation ||
             presentation == modules::HomeAssistancePresentation::LegalGuide);
        Check(visible, run, "this named checkpoint owns a visible authoritative Hint landing");
        if (visible) {
            allowedLanding = UINT64_C(1) << target;
            const int dr = int(target / 8U) - source / 8U;
            const int df = int(target % 8U) - source % 8U;
            // Only these named, authoritatively visible checkpoints permit
            // transient all-four-LED route travel. Suppression checkpoints
            // retain no exception, even if a cached global tuple survives.
            if (source < 64U &&
                (dr == 0 || df == 0 || std::abs(dr) == std::abs(df) ||
                 presentation == modules::HomeAssistancePresentation::DuckRelocation)) {
                addLine(source, target);
            } else if (source < 64U &&
                       (state.physical_occupied_mask & (UINT64_C(1) << target)) == 0ULL &&
                       !(projection.physicalCaptureSquareValid && projection.physicalCaptureSquare < 64U &&
                         (state.physical_occupied_mask & (UINT64_C(1) << projection.physicalCaptureSquare)) != 0ULL) &&
                       ((std::abs(dr) == 2 && std::abs(df) == 1) ||
                        (std::abs(dr) == 1 && std::abs(df) == 2))) {
                // Only a currently visible, real legal knight tuple permits
                // both L legs. Suppressed/cached tuples permit nothing.
                const bool verticalFirst = std::abs(dr) == 2;
                const int step = verticalFirst ? ((dr > 0) - (dr < 0)) * 8
                                               : (df > 0) - (df < 0);
                const uint8_t elbow = static_cast<uint8_t>(source + 2 * step);
                allowedLanding |= UINT64_C(1) << elbow;
                allowedLanding |= (UINT64_C(1) << source) |
                    (UINT64_C(1) << (source + step));
            }
            if (presentation == modules::HomeAssistancePresentation::DuckCompound ||
                presentation == modules::HomeAssistancePresentation::LegalGuide)
                addLine(assistance.duckFromSquare, assistance.duckToSquare);
        }
    }
    Check((emptyLit & ~allowedLanding) == 0ULL, run,
          "only the named visible landing and its directional route may light empty Piece emitters; suppression and all ordinary cells stay dark");
}

bool Move(runtime::VirtualLitchSession& session, uint64_t& occupancy,
          uint8_t from, uint8_t to) {
    occupancy &= ~(UINT64_C(1) << from);
    if (!Observe(session, occupancy)) {
        return false;
    }
    occupancy |= UINT64_C(1) << to;
    return Observe(session, occupancy) && session.Advance(1U) == VIRTUAL_LITCH_OK;
}

bool Setting(runtime::VirtualLitchSession& session, const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return session.MutateSettingJson(100U, key, value, outcome) == VIRTUAL_LITCH_OK &&
           (outcome.status == modules::ControlMutationStatus::Applied ||
            outcome.status == modules::ControlMutationStatus::Noop);
}

bool WaitHint(runtime::VirtualLitchSession& session, uint8_t& from, uint8_t& to) {
    for (uint8_t attempt = 0U; attempt < 40U; ++attempt) {
        if (session.TryGetHintMoveSquares(from, to)) {
            return true;
        }
        if (session.Advance(100U) != VIRTUAL_LITCH_OK) {
            return false;
        }
    }
    return false;
}

void AutomaticCoachingWholeTurn(bool duck, bool firstP2, bool rotated,
                               bool chooseDifferentMove = false) {
    char run[96] = {};
    std::snprintf(run, sizeof(run), "AutomaticCoachingWholeTurn/%s/first-p%u/rotate-%u/%s",
                  duck ? "Duck" : "Classic", firstP2 ? 2U : 1U, rotated,
                  chooseDifferentMove ? "different-chess-move" : "follow-hint");
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    config.first_player_is_p2 = firstP2;
    config.board_rotate = rotated;
    test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
    modules::HomeStateNotificationPublisher publisher;
    const uint64_t initial = OPENING | (duck ? UINT64_C(1) << DUCK_START : 0ULL);
    uint64_t occupancy = initial;
    Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, occupancy) &&
              session.Advance(200U) == VIRTUAL_LITCH_OK,
          run, "raw opening occupancy enters production play with optional initial Duck");
    const auto whiteOwner = PhysicalOwner(session, false);
    const auto blackOwner = PhysicalOwner(session, true);
    Check(Coaching(session, whiteOwner, true), run, "raw O hold enables White's physical Coaching badge");
    uint8_t from = 64U;
    uint8_t to = 64U;
    if (!WaitHint(session, from, to)) {
        Check(false, run, "production engine returns a current-position Hint");
        return;
    }
    uint8_t duckFrom = 64U;
    uint8_t duckTo = 64U;
    Check(session.HintDisplayRequested() && Pair(session, from) && Pair(session, to),
          run, "Coaching reveals chess endpoints without a manual Hint press");
    if (duck) {
        Check(session.TryGetHintDuckMoveSquares(duckFrom, duckTo) &&
                  duckFrom == DUCK_START && Pair(session, duckFrom) && Pair(session, duckTo),
              run, "automatic Duck Coaching reveals the same compound placement pair");
    }
    Checkpoint(session, run, "automatic-ready", "lift-suggested-chess-piece", publisher, true);

    occupancy &= ~(UINT64_C(1) << from);
    Check(Observe(session, occupancy) && session.HintDisplayRequested() &&
              session.State().selected_square == from &&
              (session.State().valid_destination_mask & (UINT64_C(1) << to)) != 0ULL,
          run, "matching pickup retains automatic Hint target with the legal physical move guide");
    const uint64_t heldLegalDestinations = session.State().valid_destination_mask;
    const uint8_t physicalTarget = runtime::MapSquareIndex(to, Rotated(session));
    const uint8_t steadySlot = to % 8U < from % 8U ? 0U : 2U;
    const auto targetIndex = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V2S3, to,
        static_cast<hardware::SquareLedSlot>(steadySlot), Rotated(session)).indices[0U];
    const auto heldStart = session.PhysicalFrame().pixels[targetIndex];
    const auto heldBoard = session.SemanticFrame();
    Check(session.Advance(225U) == VIRTUAL_LITCH_OK, run, "sample the Hint target rail while its all-four route carries travel");
    const auto heldLater = session.PhysicalFrame().pixels[targetIndex];
    uint8_t retainedFrom = 64U;
    uint8_t retainedTo = 64U;
    Check(Energy(heldLater) >= Energy(heldStart) * 96U / 256U &&
              Energy(heldLater) != 0U &&
              session.TryGetHintMoveSquares(retainedFrom, retainedTo) &&
              retainedFrom == from && retainedTo == to &&
              session.State().selected_square == from &&
              session.State().valid_destination_mask == heldLegalDestinations,
          run, "real mapped target rail retains its visible floor without changing the selected piece, Hint or legal set");
    for (uint8_t square = 0U; square < 64U; ++square) {
        if (square != to && (heldLegalDestinations & (UINT64_C(1) << square)) != 0ULL) {
            const auto before = heldBoard.board[square][steadySlot];
            const auto after = session.SemanticFrame().board[square][steadySlot];
            Check(Energy(after) >= Energy(before) * 96U / 256U && Energy(after) > 0U,
                  run, "both route rails preserve the available-color floor for every other legal destination while all four LEDs carry travel");
        }
    }
    std::printf("[AUTOMATIC_COACHING_HELD_COLOR] run=%s from=%u to=%u physical=%u "
                "start=%u,%u,%u,%u later=%u,%u,%u,%u\n",
                run, from, to, physicalTarget,
                heldStart.r, heldStart.g, heldStart.b, heldStart.w,
                heldLater.r, heldLater.g, heldLater.b, heldLater.w);
    Checkpoint(session, run, "picked-up", "place-piece-or-return-it", publisher, true);
    occupancy |= UINT64_C(1) << from;
    Check(Observe(session, occupancy) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.HintDisplayRequested(), run, "returning the piece restores automatic guidance");
    Checkpoint(session, run, "pickup-canceled", "choose-a-move", publisher, true);

    Check(Button(session, whiteOwner, runtime::VirtualLitchPhysicalButton::X, true) &&
              session.Advance(1200U) == VIRTUAL_LITCH_OK &&
              Button(session, whiteOwner, runtime::VirtualLitchPhysicalButton::X, false) &&
              session.QuickSettingsActive() && !session.HintDisplayRequested(),
          run, "physical Quick Setup overlay suppresses automatic guidance");
    Check(Tap(session, whiteOwner, runtime::VirtualLitchPhysicalButton::X) &&
              !session.QuickSettingsActive() && session.HintDisplayRequested(),
          run, "closing Quick Setup restores the existing Hint without another selection");
    Checkpoint(session, run, "menu-closed", "choose-a-move", publisher, true);

    Check(Coaching(session, whiteOwner, false) && !session.HintDisplayRequested(),
          run, "disabling Coaching releases automatic display ownership");
    uint8_t shownFrom = 64U, shownTo = 64U, replayFrom = 64U, replayTo = 64U;
    Check(Tap(session, whiteOwner, runtime::VirtualLitchPhysicalButton::O) &&
              session.HintDisplayRequested() && session.TryGetHintMoveSquares(shownFrom, shownTo),
          run, "manual Hint reveals the current cached result with Coaching disabled");
    Check(Tap(session, whiteOwner, runtime::VirtualLitchPhysicalButton::O) &&
              !session.HintDisplayRequested(),
          run, "manual O retap hides guidance while Coaching is disabled");
    Check(Tap(session, whiteOwner, runtime::VirtualLitchPhysicalButton::O) &&
              session.HintDisplayRequested(),
          run, "manual O reveals retained analysis again without enabling Coaching");
    Check(Coaching(session, whiteOwner, true) && Coaching(session, blackOwner, true) &&
              session.TryGetHintMoveSquares(shownFrom, shownTo),
          run, "both player latches are enabled independently with current guidance");
    Check(Tap(session, whiteOwner, runtime::VirtualLitchPhysicalButton::O) &&
              session.HintDisplayRequested() && session.TryGetHintMoveSquares(replayFrom, replayTo) &&
              replayFrom == shownFrom && replayTo == shownTo,
          run, "Coaching O retap replays the exact shown result without hiding guidance");

    occupancy &= ~(UINT64_C(1) << 12U);
    Check(Observe(session, occupancy) && !session.HintDisplayRequested(),
          run, "wrong-owner removal immediately suppresses Coaching during quiet correction");
    Check(session.Advance(300U) == VIRTUAL_LITCH_OK && Pair(session, 12U),
          run, "persistent wrong-owner removal reveals a local restoration cue after quiet grace");
    Checkpoint(session, run, "correction", "restore-e7", publisher);
    occupancy |= UINT64_C(1) << 12U;
    Check(Observe(session, occupancy) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.HintDisplayRequested(), run, "restoration recovers automatic guidance");

    if (chooseDifferentMove) {
        from = from == 52U && to == 36U ? 51U : 52U;
        to = from == 51U ? 35U : 36U;
    }
    Check(Move(session, occupancy, from, to), run, "raw chess move reaches production acknowledgement");
    if (duck) {
        // Real early Duck pickup completes the chess-settle handoff, just as
        // the existing fast compound Hint journey does on the physical board.
        occupancy &= ~(UINT64_C(1) << DUCK_START);
        Check(Observe(session, occupancy), run, "early Duck pickup reaches the staged relocation");
        uint8_t relocationFrom = 64U;
        uint8_t relocationTo = 64U;
        Check(session.TryGetHintMoveSquares(relocationFrom, relocationTo) &&
                  relocationFrom == DUCK_START && relocationTo < 64U &&
                  (occupancy & (UINT64_C(1) << relocationTo)) == 0ULL &&
                  (chooseDifferentMove || relocationTo == duckTo),
              run, "same suggested chess move retains its Duck destination; other moves use staged placement");
        if (relocationTo >= 64U) {
            return;
        }
        duckTo = relocationTo;
        Check(session.HintDisplayRequested() && Pair(session, duckTo) &&
                  session.State().side_to_move_black == 0U,
              run, "automatic placement remains visible and the combined turn has not advanced");
        CheckDuckYellow(session, run, duckTo);
        Checkpoint(session, run, "duck-placement", "move-duck-to-yellow-destination", publisher, true);
        Check(Coaching(session, whiteOwner, false) && Coaching(session, whiteOwner, true) &&
                  session.HintDisplayRequested() && Pair(session, duckTo),
              run, "Coaching can be re-enabled during an authoritative Duck placement");
        CheckDuckYellow(session, run, duckTo);
        Check(Move(session, occupancy, DUCK_START, duckTo), run, "raw Duck placement completes the compound turn");
    }
    // Observe the actual acknowledgement boundary; 200 ms is shorter than
    // ordinary chess settling and does not establish turn completion.
    for (uint8_t sample = 0U; sample < 60U &&
         session.State().side_to_move_black == 0U; ++sample) {
        Check(session.Advance(100U) == VIRTUAL_LITCH_OK, run, "advance through production acknowledgement");
    }
    Check(session.State().side_to_move_black == 1U &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().expected_occupied_mask == occupancy &&
              session.State().pending_action_token == 0U,
          run, "completed turn has canonical occupancy, next player and no pending action");
    // With this fixture's zero calculation minimum, Classic already has its
    // next scored Hint. Keep the exact current landing/route envelope; Duck
    // remains in its thinking phase at this acknowledgement boundary.
    Checkpoint(session, run, "turn-complete", "black-selects-a-move", publisher, !duck);

    uint8_t earlyFrom = 64U;
    uint8_t earlyTo = 64U;
    // BoundedHintEngine disables the real calculation minimum. The production
    // mailbox timing journey owns the 500 ms first-reveal boundary; this one
    // checks physical pickup and recovery with whichever scored work is ready.
    occupancy &= ~(UINT64_C(1) << 9U); // Black b7 pawn on the next human turn.
    Check(Observe(session, occupancy) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::PieceLifted) &&
              !session.HintDisplayRequested(), run,
          "next-turn pickup remains responsive and keeps its physical legal guidance");
    Checkpoint(session, run, "early-next-turn-pickup", "place-b7-pawn-or-return-it", publisher);
    Check(session.Advance(500U) == VIRTUAL_LITCH_OK, run,
          "the human keeps holding the pawn without losing physical guidance");
    occupancy |= UINT64_C(1) << 9U;
    Check(Observe(session, occupancy) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
              WaitHint(session, earlyFrom, earlyTo) && session.HintDisplayRequested(), run,
          "returning the early pickup reveals retained analysis without another Hint gesture");
    Checkpoint(session, run, "early-pickup-recovered", "follow-current-Black-Hint", publisher, true);

    Check(Setting(session, "VS_CPU_PLAYER", "2") &&
              Setting(session, "VS_CPU_LEVEL", "1") &&
              session.Advance(100U) == VIRTUAL_LITCH_OK && !session.HintDisplayRequested(),
          run, "CPU turn suppresses automatic guidance without changing either Coaching latch");
    Check(!virtual_litch_test::HasPhysicalCoachingBadge(session.PhysicalFrame(), blackOwner) &&
              (session.HomeStateSnapshot().coachingMask & 2U) != 0U,
          run, "CPU inhibition hides the physical Coaching badge while retaining its saved preference");
    Checkpoint(session, run, "cpu-suppressed", "follow-computer-move-or-return-to-human-play", publisher);
    Check(Setting(session, "VS_CPU_LEVEL", "0") && session.Advance(200U) == VIRTUAL_LITCH_OK &&
              WaitHint(session, from, to) && session.HintDisplayRequested(),
          run, "returning the turn to a human restores automatic Coaching");
    Checkpoint(session, run, "human-reentry", "lift-suggested-black-piece", publisher, true);
}

std::string PublishedFen(const runtime::VirtualLitchSession& session) {
    const auto home = session.HomeStateLine();
    StaticJsonDocument<2048> document;
    if (deserializeJson(document, home.CStr())) {
        return {};
    }
    return document["fen"] | "";
}

void FixedDuckGuidance(runtime::VirtualLitchSession& session, const char* run,
                       const char* stage, uint8_t target, uint8_t guidedSquare,
                       runtime::RequiredPhysicalAction required,
                       const char* next,
                       modules::HomeStateNotificationPublisher& publisher) {
    const bool correctingWrongSquare = required == runtime::RequiredPhysicalAction::CorrectBoard;
    if (correctingWrongSquare) {
        Check(session.Advance(299U) == VIRTUAL_LITCH_OK && !CorrectionPair(session, guidedSquare),
              run, "wrong Duck placement retains quiet correction through 299 ms");
        Checkpoint(session, run, "wrong-duck-quiet", next, publisher);
        Check(session.Advance(1U) == VIRTUAL_LITCH_OK && CorrectionPair(session, guidedSquare),
              run, "wrong Duck placement reveals its local bracket at 300 ms");
        const auto amber = session.SemanticFrame().board[guidedSquare][0];
        Check(amber.r > amber.g && amber.g > 0U && amber.b == 0U,
              run, "gentle Duck correction uses the current amber bracket");
        Check(!session.HintDisplayRequested() &&
                  virtual_litch_test::HasPhysicalCoachingBadge(
                      session.PhysicalFrame(), PhysicalOwner(session, false)) &&
                  virtual_litch_test::HasPhysicalCoachingBadge(
                      session.PhysicalFrame(), PhysicalOwner(session, true)),
              run, "gentle correction keeps Hint suppressed and both Coaching badges visible");
        Checkpoint(session, run, "wrong-duck-amber", next, publisher);
        Check(session.Advance(1199U) == VIRTUAL_LITCH_OK && CorrectionPair(session, guidedSquare),
              run, "local Duck correction remains visible through 1499 ms");
        Check(session.Advance(1U) == VIRTUAL_LITCH_OK && CorrectionPair(session, guidedSquare),
              run, "persistent Duck correction starts strong feedback on-phase at 1500 ms");
        Checkpoint(session, run, "wrong-duck-strong", next, publisher);
    }
    bool previousLit = false;
    bool sawLit = false;
    bool sawDark = false;
    for (uint8_t sample = 0U; sample < 4U; ++sample) {
        Check(session.Advance(300U) == VIRTUAL_LITCH_OK, run,
              "repeated production ticks retain the fixed history action");
        Check(session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::DuckRelocation) &&
                  session.State().selected_square == target &&
                  session.State().required_action == static_cast<uint8_t>(required),
              run, "fixed Duck target and physical next action stay authoritative");
        const bool hidden = !session.HintDisplayRequested();
        const bool guided = correctingWrongSquare
            ? CorrectionPair(session, guidedSquare) : Pair(session, guidedSquare);
        const uint8_t physical = runtime::MapSquareIndex(guidedSquare, Rotated(session));
        const auto semantic = session.SemanticFrame().board[guidedSquare][0];
        const auto mapped = session.PhysicalFrame().pixels[hardware::LedIndexForSquare(
            physical, hardware::SquareLedSlot::Square0)];
        char detail[320] = {};
        std::snprintf(detail, sizeof(detail),
                      "fixed Duck pair stage=%s sample=%u t=%lu action=%u target=%u guided=%u "
                      "physical=%u hidden=%u semantic=%u,%u,%u,%u mapped=%u,%u,%u,%u",
                      stage, sample, static_cast<unsigned long>(session.State().now_ms),
                      static_cast<unsigned>(required), target, guidedSquare, physical, hidden,
                      semantic.r, semantic.g, semantic.b, semantic.w,
                      mapped.r, mapped.g, mapped.b, mapped.w);
        Check(hidden, run, "automatic Hint stays suppressed throughout fixed Duck history guidance");
        if (correctingWrongSquare) {
            // After 1500 ms, CorrectBoard owns a 300 ms wrong-square flash. Its dark
            // phase must not be mistaken for lost fixed Undo/Redo guidance.
            // Four 300 ms samples cover two full cycles without depending on
            // the flash phase at entry. Keep logical and mapped output aligned.
            const bool semanticLit = CorrectionColor(semantic);
            const bool physicalLit = CorrectionColor(mapped);
            Check(semanticLit == physicalLit, run, detail);
            Check(!semanticLit || guided, run, detail);
            Check((session.State().correction_mask & (UINT64_C(1) << guidedSquare)) != 0ULL,
                  run, "wrong-square feedback stays attached to the actual misplaced Duck");
            if (sample != 0U) { Check(semanticLit != previousLit, run, detail); }
            previousLit = semanticLit;
            sawLit = sawLit || semanticLit;
            sawDark = sawDark || !semanticLit;
        } else {
            Check(guided, run, detail);
        }
        const auto home = session.HomeStateLine();
        Check(std::strstr(home.CStr(), "\"co\":3") != nullptr &&
                  std::strstr(home.CStr(), "\"as\":\"0x\"") != nullptr,
              run, "Home retains both enabled latches and reports hidden assistance");
        Checkpoint(session, run, stage, next, publisher);
    }
    if (correctingWrongSquare) {
        Check(sawLit && sawDark, run,
              "wrong-square correction visibly flashes through both phases while its fixed destination remains authoritative");
    }
    // Hard correction owns the complete edge ring. Enabled latches above
    // remain authoritative; badges return when fixed placement resumes.
    // Other phases also allow an acknowledgement stinger to expire first.
    const bool badgesVisible =
        required != runtime::RequiredPhysicalAction::CorrectBoard;
    Check(virtual_litch_test::HasPhysicalCoachingBadge(
              session.PhysicalFrame(), PhysicalOwner(session, false)) == badgesVisible &&
              virtual_litch_test::HasPhysicalCoachingBadge(
                  session.PhysicalFrame(), PhysicalOwner(session, true)) == badgesVisible,
          run, badgesVisible
                   ? "both physical Coaching badges survive fixed history guidance"
                   : "hard correction owns the edge while both Coaching latches remain enabled");
}

void AutomaticCoachingDuckUndoAndCancelRedo(bool firstP2, bool rotated) {
    char run[96] = {};
    std::snprintf(run, sizeof(run),
                  "AutomaticCoachingDuckUndoAndCancelRedo/first-p%u/rotate-%u",
                  firstP2 ? 2U : 1U, rotated);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    config.first_player_is_p2 = firstP2;
    config.board_rotate = rotated;
    test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
    modules::HomeStateNotificationPublisher publisher;
    uint64_t occupancy = OPENING | (UINT64_C(1) << DUCK_START);
    const bool ready = session.Reset() == VIRTUAL_LITCH_OK &&
                       Observe(session, occupancy) &&
                       session.Advance(200U) == VIRTUAL_LITCH_OK;
    Check(ready, run, "raw opening Hall occupancy reaches Duck play");
    if (!ready) {
        return;
    }
    const auto whiteOwner = PhysicalOwner(session, false);
    const auto blackOwner = PhysicalOwner(session, true);
    Check(Coaching(session, whiteOwner, true) && Coaching(session, blackOwner, true),
          run, "raw O holds enable both physical Coaching owners");
    uint8_t from = 64U;
    uint8_t to = 64U;
    uint8_t duckFrom = 64U;
    uint8_t duckTo = 64U;
    const bool hinted = WaitHint(session, from, to) &&
                        session.TryGetHintDuckMoveSquares(duckFrom, duckTo) &&
                        from < 64U && to < 64U && duckFrom == DUCK_START &&
                        duckTo < 64U && duckTo != DUCK_START;
    Check(hinted && session.HintDisplayRequested(), run,
          "actual engine provides the automatic complete-turn suggestion");
    if (!hinted) {
        return;
    }
    Checkpoint(session, run, "automatic-ready", "follow-chess-and-Duck-Hint", publisher, true);
    Check(Move(session, occupancy, from, to), run,
          "raw chess pickup and placement reach production acknowledgement");
    occupancy &= ~(UINT64_C(1) << DUCK_START);
    Check(Observe(session, occupancy), run,
          "early Duck pickup enters the same pending compound turn");
    occupancy |= UINT64_C(1) << duckTo;
    Check(Observe(session, occupancy), run, "raw Duck placement completes the turn");
    for (uint8_t sample = 0U; sample < 60U &&
         session.State().side_to_move_black == 0U; ++sample) {
        Check(session.Advance(100U) == VIRTUAL_LITCH_OK, run,
              "wait for the actual complete-turn acknowledgement");
    }
    const std::string committedFen = PublishedFen(session);
    const uint64_t committedOccupancy = occupancy;
    const bool committed = !committedFen.empty() &&
                           session.State().side_to_move_black == 1U &&
                           session.State().expected_occupied_mask == occupancy &&
                           session.DuckSquare() == duckTo &&
                           !session.IsUndoActionPending();
    Check(committed, run, "one compound turn commits before requesting Undo");
    if (!committed) {
        return;
    }
    Checkpoint(session, run, "turn-complete", "tap-X-to-Undo", publisher);

    Check(Tap(session, blackOwner, runtime::VirtualLitchPhysicalButton::X),
          run, "raw X requests Undo through the production control owner");
    modules::ControlMutationOutcome outcome;
    Check(session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::Undo &&
              (outcome.status == modules::ControlMutationStatus::Accepted ||
               outcome.status == modules::ControlMutationStatus::Applied),
          run, "the board control owner acknowledges the Undo request");
    FixedDuckGuidance(session, run, "undo-fixed-return", DUCK_START, duckTo,
                      runtime::RequiredPhysicalAction::LiftPrimary,
                      "lift-Duck-for-fixed-return", publisher);
    Check(PublishedFen(session) == committedFen, run,
          "requesting Duck return does not prematurely undo the chess half");

    Check(Button(session, blackOwner, runtime::VirtualLitchPhysicalButton::O, true) &&
              session.Advance(1200U) == VIRTUAL_LITCH_OK &&
              Button(session, blackOwner, runtime::VirtualLitchPhysicalButton::O, false),
          run, "raw O hold is observed while fixed Undo owns the board");
    FixedDuckGuidance(session, run, "undo-O-hold-ignored", DUCK_START, duckTo,
                      runtime::RequiredPhysicalAction::LiftPrimary,
                      "lift-Duck-for-fixed-return", publisher);
    occupancy &= ~(UINT64_C(1) << duckTo);
    Check(Observe(session, occupancy), run, "raw Duck pickup advances the fixed return");
    FixedDuckGuidance(session, run, "undo-Duck-in-hand", DUCK_START, DUCK_START,
                      runtime::RequiredPhysicalAction::PlacePrimary,
                      "place-Duck-on-original-square", publisher);
    occupancy |= UINT64_C(1) << DUCK_START;
    Check(Observe(session, occupancy) && session.Advance(100U) == VIRTUAL_LITCH_OK,
          run, "returning Duck opens the actual chess inverse prompt");
    Check(session.IsUndoActionPending() && !session.HintDisplayRequested() &&
              session.State().side_to_move_black == 0U &&
              PublishedFen(session) != committedFen,
          run, "logical Undo waits for its physical chess inverse without automatic Hint");
    Checkpoint(session, run, "undo-chess-inverse", "tap-X-to-cancel-Undo", publisher, false, to, from);

    Check(Tap(session, whiteOwner, runtime::VirtualLitchPhysicalButton::X),
          run, "raw X cancels the pending physical chess inverse");
    Check(session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::Undo &&
              (outcome.status == modules::ControlMutationStatus::Accepted ||
               outcome.status == modules::ControlMutationStatus::Applied),
          run, "production control owner acknowledges Cancel Undo");
    FixedDuckGuidance(session, run, "cancel-redo-fixed", duckTo, DUCK_START,
                      runtime::RequiredPhysicalAction::LiftPrimary,
                      "lift-Duck-to-restore-canceled-turn", publisher);
    Check(PublishedFen(session) == committedFen, run,
          "Cancel Undo restores the original committed chess state");
    occupancy &= ~(UINT64_C(1) << DUCK_START);
    Check(Observe(session, occupancy), run, "raw Duck pickup begins fixed CancelRedo");
    FixedDuckGuidance(session, run, "cancel-redo-in-hand", duckTo, duckTo,
                      runtime::RequiredPhysicalAction::PlacePrimary,
                      "place-Duck-on-fixed-redo-square", publisher);
    uint8_t wrong = 0U;
    while (wrong < 64U && (wrong == duckTo || wrong == DUCK_START ||
           (occupancy & (UINT64_C(1) << wrong)) != 0ULL)) {
        ++wrong;
    }
    Check(wrong < 64U, run, "fixture has an empty incorrect Duck square");
    if (wrong >= 64U) {
        return;
    }
    occupancy |= UINT64_C(1) << wrong;
    Check(Observe(session, occupancy), run, "wrong Duck square enters production correction");
    FixedDuckGuidance(session, run, "cancel-redo-wrong-square", duckTo, wrong,
                      runtime::RequiredPhysicalAction::CorrectBoard,
                      "lift-Duck-from-incorrect-square", publisher);
    occupancy &= ~(UINT64_C(1) << wrong);
    Check(Observe(session, occupancy), run, "lifting incorrect Duck recovers the fixed redo");
    FixedDuckGuidance(session, run, "cancel-redo-recovered", duckTo, duckTo,
                      runtime::RequiredPhysicalAction::PlacePrimary,
                      "place-Duck-on-fixed-redo-square", publisher);
    occupancy |= UINT64_C(1) << duckTo;
    Check(Observe(session, occupancy) && session.Advance(300U) == VIRTUAL_LITCH_OK,
          run, "correct placement completes the actual CancelRedo action");
    Check(!session.IsUndoActionPending() &&
              session.State().pending_action_token == 0U &&
              session.State().side_to_move_black == 1U &&
              session.State().expected_occupied_mask == committedOccupancy &&
              session.State().physical_occupied_mask == committedOccupancy &&
              session.DuckSquare() == duckTo && PublishedFen(session) == committedFen,
          run, "CancelRedo restores exactly the completed turn without replay or history drift");
    Check(WaitHint(session, from, to) && session.HintDisplayRequested() &&
              session.Advance(300U) == VIRTUAL_LITCH_OK &&
              PublishedFen(session) == committedFen && !session.IsUndoActionPending(),
          run, "ordinary human play resumes automatic Coaching without reviving Undo");
    Checkpoint(session, run, "cancel-redo-complete", "choose-next-human-chess-move", publisher, true);
}

} // namespace

void DirectedV33HintAndCpuDestinationJourney(const bool limited) {
    const char* run = limited ? "DirectedDestination/V33-power-limited"
                              : "DirectedDestination/V33-near-dark";
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.brightness_scale_256 = limited ? 86U : 8U;
    config.max_led_current_ma = limited ? 80U : 0U;
    test_support::BoundedHintSession fixture(config); auto& session = fixture.session;
    const auto pixel = [&](const uint8_t square, const uint8_t slot) {
        return session.PhysicalFrame().pixels[4U * square + slot];
    };
    const auto checkpoint = [&](const char* stage, const uint8_t target, const char* next) {
        const auto& frame = session.PhysicalFrame();
        std::printf("[DIRECTED_DESTINATION_V33] run=%s stage=%s t=%u occupied=%016llx phase=%u action=%u target=%u "
                    "semantic_piece=%u,%u,%u top=%u,%u,%u bottom=%u,%u,%u scale=%u limited=%u current=%u next=%s\n",
            run, stage, session.State().now_ms, static_cast<unsigned long long>(session.State().physical_occupied_mask),
            session.State().interaction_phase, session.State().required_action, target,
            session.SemanticFrame().board[target][1U].r, session.SemanticFrame().board[target][1U].g,
            session.SemanticFrame().board[target][1U].b,
            pixel(target,1U).r,pixel(target,1U).g,pixel(target,1U).b,
            pixel(target,3U).r,pixel(target,3U).g,pixel(target,3U).b,
            frame.applied_brightness_scale_65535,frame.power_limited,frame.applied_led_current_ma,next);
    };
    const auto steadyMovement = [&](uint8_t source, uint8_t target, const char* stage) {
        Check(session.Advance(0U) == VIRTUAL_LITCH_OK, run, "render the current acknowledged movement prompt");
        const auto before = session.SemanticFrame();
        bool steady = true;
        for (uint8_t sample = 0U; sample < 45U; ++sample) {
            Check(session.Advance(20U) == VIRTUAL_LITCH_OK, run, "sample Minimal movement through the mover cadence");
            for (const auto square : {source, target}) for (uint8_t slot = 0U; slot < 3U; ++slot) {
                const auto a = before.board[square][slot];
                const auto b = session.SemanticFrame().board[square][slot];
                const bool same = a.r == b.r && a.g == b.g && a.b == b.b;
                if (steady && !same) std::printf("[MINIMAL_MOVEMENT_CHANGE] stage=%s square=%u slot=%u before=%u,%u,%u after=%u,%u,%u\n",
                    stage, square, slot, a.r, a.g, a.b, b.r, b.g, b.b);
                steady = steady && same;
            }
        }
        Check(steady && Energy(session.SemanticFrame().board[target][1U]) > 0U, run,
              "Minimal keeps the known source and landing steady with a visible Piece invitation");
        checkpoint(stage, target, "follow-steady-source-and-landing");
    };
    Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
              Setting(session,"WHITE_SQUARES_ON","false") && Coaching(session,PhysicalOwner(session,false),true),
          run,"real opening, acknowledged setting and O hold enable V3.3 Coaching");
    uint8_t from=64U,to=64U; const bool ready=WaitHint(session,from,to);
    Check(ready && from<64U && to<64U,run,"production search supplies the target");
    if (!ready || from>=64U || to>=64U) return;
    const uint8_t steady=to%8U<from%8U?0U:2U;
    const auto full=session.SemanticFrame().board[to][steady];
    const int dr=int(to/8U)-int(from/8U),df=int(to%8U)-int(from%8U);
    const bool knight=(std::abs(dr)==2&&std::abs(df)==1)||(std::abs(dr)==1&&std::abs(df)==2);
    uint64_t routeMask=(UINT64_C(1)<<from)|(UINT64_C(1)<<to);
    if(knight) {
        const int step=std::abs(dr)==2?((dr>0)-(dr<0))*8:(df>0)-(df<0);
        routeMask|=(UINT64_C(1)<<(from+step))|(UINT64_C(1)<<(from+2*step));
    }
    uint8_t nonroute=64U;
    for (const int8_t delta : {-1,1}) {
        const int square=int(to)+delta;
        if (square<0 || square>=64 || square/8!=to/8U || square==from ||
            (OPENING & (UINT64_C(1)<<square))!=0ULL || (routeMask & (UINT64_C(1)<<square))!=0ULL) continue;
        nonroute=static_cast<uint8_t>(square); break;
    }
    bool sourceOn=false,sourceOff=false,topBrighter=false,bottomBrighter=false;
    for (uint8_t sample=0U;sample<=24U;++sample) {
        const auto current=session.SemanticFrame().board[to][steady];
        sourceOn=sourceOn || Energy(session.SemanticFrame().board[from][1U])!=0U;
        sourceOff=sourceOff || Energy(session.SemanticFrame().board[from][1U])==0U;
        Check((knight ? Energy(current)>0U : current.r==full.r && current.g==full.g && current.b==full.b) &&
                  Energy(pixel(to,0U))>0U && Energy(pixel(to,2U))>0U &&
                  Energy(pixel(to,1U))>0U && Energy(pixel(to,3U))>0U,
              run,"axis-appropriate landing rails and BOTH actual Piece emitters survive quantization and current limiting");
        topBrighter=topBrighter || Energy(pixel(to,1U))>Energy(pixel(to,3U));
        bottomBrighter=bottomBrighter || Energy(pixel(to,3U))>Energy(pixel(to,1U));
        if (nonroute<64U) Check(Energy(pixel(nonroute,0U))==0U && Energy(pixel(nonroute,2U))==0U &&
                  Energy(pixel(nonroute,1U))==0U && Energy(pixel(nonroute,3U))==0U,
              run,"nonroute neighbor remains ordinary without a false destination cross");
        if (sample%6U==0U) checkpoint("Coaching-visible",to,"lift-blinking-source");
        if (sample!=24U) Check(session.Advance(50U)==VIRTUAL_LITCH_OK,run,"sample complete landing alternation and source blink");
    }
    Check(sourceOn && sourceOff && topBrighter && bottomBrighter,run,
          "source independently blinks and each actual target emitter takes the brighter role above floor");
    Check(!limited || (session.PhysicalFrame().power_limited && session.PhysicalFrame().applied_led_current_ma<=80U),
          run,"active landing remains inside the actual constrained current budget");
    if (!limited) {
        Check(Setting(session,"MINIMAL_ANIMATIONS","true"),run,"enable Minimal through acknowledged setting");
        steadyMovement(from,to,"Minimal-Coaching-visible");
    }
    Check(Observe(session,OPENING & ~(UINT64_C(1)<<from)),run,"Hall pickup follows Coaching");
    checkpoint("Coaching-held",to,"place-in-directed-landing-or-return-source");
    if (!limited) steadyMovement(from,to,"Minimal-Coaching-held");
    Check(Observe(session,OPENING) && Coaching(session,PhysicalOwner(session,false),false) && !session.HintDisplayRequested() &&
              Energy(pixel(to,1U))==0U && Energy(pixel(to,3U))==0U,
          run,"return and O hold cancel actual landing emitters without stale presence");
    Check(Setting(session,"VS_CPU_PLAYER","1") && Setting(session,"VS_CPU_LEVEL","1"),run,
          "acknowledged settings enable the production CPU on White");
    for (uint8_t attempt=0U;attempt<80U && session.HomeStateSnapshot().action!=modules::HomeStateAction::Cpu;++attempt)
        Check(session.Advance(100U)==VIRTUAL_LITCH_OK,run,"wait for the real CPU plan");
    const auto cpu=session.HomeStateSnapshot();
    Check(cpu.action==modules::HomeStateAction::Cpu && cpu.physicalMoveValid,run,
          "authoritative CPU plan names its physical source and destination");
    if (!cpu.physicalMoveValid) return;
    Check(session.Advance(700U)==VIRTUAL_LITCH_OK && Energy(pixel(cpu.physicalToSquare,0U))>0U &&
              Energy(pixel(cpu.physicalToSquare,2U))>0U && Energy(pixel(cpu.physicalToSquare,1U))>0U &&
              Energy(pixel(cpu.physicalToSquare,3U))>0U,run,
          "CPU opposite rails and BOTH actual landing emitters stay visible through mover cadence");
    checkpoint("CPU-directed",cpu.physicalToSquare,"lift-named-CPU-source");
    if (!limited) steadyMovement(cpu.physicalFromSquare,cpu.physicalToSquare,"Minimal-CPU-visible");
    uint64_t occupancy=OPENING & ~(UINT64_C(1)<<cpu.physicalFromSquare);
    Check(Observe(session,occupancy),run,"CPU lift reaches the production plan");
    checkpoint("CPU-held",cpu.physicalToSquare,"place-in-directed-CPU-landing");
    if (!limited) steadyMovement(cpu.physicalFromSquare,cpu.physicalToSquare,"Minimal-CPU-held");
    occupancy|=UINT64_C(1)<<cpu.physicalToSquare;
    Check(Observe(session,occupancy) && session.Advance(3000U)==VIRTUAL_LITCH_OK &&
              session.UndoHistoryMoveCount()==1U && session.State().correction_mask==0ULL,run,
          "CPU seating commits once and retires the destination invitation");
    checkpoint("CPU-complete",cpu.physicalToSquare,"play-Black-move");
    if (!limited) {
        Check(Setting(session,"VS_CPU_LEVEL","0") &&
                  Tap(session,PhysicalOwner(session,true),runtime::VirtualLitchPhysicalButton::X) &&
                  session.IsUndoActionPending(),run,"physical X opens the committed CPU move's inverse");
        steadyMovement(cpu.physicalToSquare,cpu.physicalFromSquare,"Minimal-Undo-visible");
        occupancy &= ~(UINT64_C(1)<<cpu.physicalToSquare);
        Check(Observe(session,occupancy),run,"Hall pickup follows the inverse source");
        steadyMovement(cpu.physicalToSquare,cpu.physicalFromSquare,"Minimal-Undo-held");
        const auto inverseBaseline = session.SemanticFrame();
        Check(Setting(session,"MINIMAL_ANIMATIONS","false"),run,"restore normal movement for inverse travel");
        bool transit = false, identities = true;
        for (uint16_t sample = 0U; sample < 100U; ++sample) {
            for (uint8_t square = 0U; square < 64U; ++square) {
                if (square == cpu.physicalFromSquare || square == cpu.physicalToSquare) continue;
                for (const auto slot : {0U,2U}) {
                    const auto before = inverseBaseline.board[square][slot];
                    const auto current = session.SemanticFrame().board[square][slot];
                    transit = transit || before.r != current.r || before.g != current.g || before.b != current.b;
                }
                if ((occupancy & (UINT64_C(1)<<square)) != 0ULL) {
                    const auto before = inverseBaseline.board[square][1U];
                    const auto current = session.SemanticFrame().board[square][1U];
                    identities = identities && before.r == current.r && before.g == current.g && before.b == current.b;
                }
            }
            Check(session.Advance(20U)==VIRTUAL_LITCH_OK,run,"observe a complete inverse travel pass");
        }
        Check(transit && identities,run,
              "Undo carries direction on transit rails while every occupied PiecePair retains its identity");
        checkpoint("Undo-travel",cpu.physicalFromSquare,"return-piece-to-original-square");
        occupancy |= UINT64_C(1)<<cpu.physicalFromSquare;
        Check(Observe(session,occupancy) && session.Advance(100U)==VIRTUAL_LITCH_OK &&
                  !session.IsUndoActionPending() && session.UndoHistoryMoveCount()==0U,run,
              "physical inverse completion retires shared guidance and restores the opening");
    }
}

void AssistancePreferencesPersistAndCpuGesturesJourney() {
    const char* run = "AssistancePreferencesPersistAndCpuGesturesJourney";
    const std::string path = "test_assistance_preferences_journey.json";
    auto cleanup = [&]() {
        for (const char* suffix : {"", ".tmp", ".bak"}) {
            std::error_code error;
            std::filesystem::remove(path + suffix, error);
        }
    };
    cleanup();
    {
        modules::SettingsStoreConfig settingsConfig;
        settingsConfig.path = path.c_str();
        modules::SettingsStore seed(settingsConfig);
        seed.Doc()["VS_CPU_LEVEL"] = 0U;
        seed.Doc()["TIMER"] = 0U;
        seed.Doc()["GAME_FIRST_PLAYER"] = 1U;
        seed.Doc()["BOARD_ROTATE"] = false;
        Check(seed.SaveNowBlocking(), run, "formatted store fixture exists before production boot load");
    }
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    {
        test_support::BoundedHintSession fixture(config, path.c_str());
        auto& session = fixture.session;
        Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
              Coaching(session, PhysicalOwner(session, false), true), run,
              "raw human O hold enables saved P1 Automatic Hints");
        Check(session.SettingsForPersistenceTest().SaveNowBlocking(), run,
              "production settings serializer flushes P1 on/P2 off");
    }
    {
        test_support::BoundedHintSession fixture(config, path.c_str());
        auto& session = fixture.session;
        Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.HomeStateSnapshot().coachingMask == 1U, run,
              "boot reload restores independently saved P1 on/P2 off");
        Check(Coaching(session, PhysicalOwner(session, false), false) &&
              Coaching(session, PhysicalOwner(session, true), true) &&
              session.SettingsForPersistenceTest().SaveNowBlocking(), run,
              "opposite raw owner holds save P1 off/P2 on through the same setting owner");
    }
    {
        test_support::BoundedHintSession fixture(config, path.c_str());
        auto& session = fixture.session;
        modules::HomeStateNotificationPublisher publisher;
        Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.HomeStateSnapshot().coachingMask == 2U, run,
              "next boot restores independently saved P1 off/P2 on");
        Check(Coaching(session, PhysicalOwner(session, false), true) &&
              Setting(session, "VS_CPU_PLAYER", "2") && Setting(session, "VS_CPU_LEVEL", "1"), run,
              "human turn with a CPU opponent retains both saved preferences");
        const uint32_t revision = session.SettingsForPersistenceTest().GetRevision();
        const auto cpuEnd = PhysicalOwner(session, true);
        Check(Button(session, cpuEnd, runtime::VirtualLitchPhysicalButton::O, true) &&
              session.Advance(1200U) == VIRTUAL_LITCH_OK &&
              Button(session, cpuEnd, runtime::VirtualLitchPhysicalButton::O, false) &&
              session.HomeStateSnapshot().coachingMask == 3U &&
              session.SettingsForPersistenceTest().GetRevision() == revision, run,
              "CPU bank O hold is consumed without toggling its saved preference or spending a revision");
        Checkpoint(session, run, "CPU-O-inhibited", "human-move-or-change-opponent", publisher, true);
        const auto humanEnd = PhysicalOwner(session, false);
        Check(Button(session, humanEnd, runtime::VirtualLitchPhysicalButton::O, true) &&
              session.Advance(400U) == VIRTUAL_LITCH_OK &&
              Setting(session, "VS_CPU_PLAYER", "1") && Setting(session, "VS_CPU_LEVEL", "0") &&
              session.Advance(800U) == VIRTUAL_LITCH_OK &&
              Button(session, humanEnd, runtime::VirtualLitchPhysicalButton::O, false) &&
              session.HomeStateSnapshot().coachingMask == 3U, run,
              "CPU role changes cancel an in-flight optional hold even when CPU turns off before release");
        Check(Coaching(session, humanEnd, false) && Coaching(session, humanEnd, true), run,
              "a fresh physical O hold works once after cancellation and release");
        Check(Setting(session, "MOVE_CONFIRM", "\"press_circle_to_confirm\"") &&
              Setting(session, "VS_CPU_PLAYER", "2") && Setting(session, "VS_CPU_LEVEL", "1"), run,
              "Circle confirmation remains configured with the opposite CPU bank");
        uint64_t occupancy = OPENING;
        Check(Move(session, occupancy, 52U, 36U) && session.Advance(1200U) == VIRTUAL_LITCH_OK &&
              session.UndoHistoryMoveCount() == 0U, run,
              "raw human placement waits for required Circle despite saved automatic guidance");
        Check(Tap(session, cpuEnd, runtime::VirtualLitchPhysicalButton::O) &&
              session.Advance(1100U) == VIRTUAL_LITCH_OK && session.UndoHistoryMoveCount() == 1U, run,
              "CPU bank O still performs required Circle confirmation");
        Checkpoint(session, run, "required-Circle-accepted", "follow-CPU-move", publisher);
    }
    cleanup();
}

void IndependentLegalMovesAndInwardCheckJourney() {
    const char* run = "IndependentLegalMovesAndInwardCheckJourney";
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
          Setting(session, "WHITE_SQUARES_ON", "false") &&
          Setting(session, "P1_LEGAL_MOVES", "false"), run, "opening has P1 legal destinations disabled");
    uint8_t hintFrom = 0U, hintTo = 0U;
    Check(WaitHint(session, hintFrom, hintTo) &&
          Tap(session, PhysicalOwner(session, false), runtime::VirtualLitchPhysicalButton::O) &&
          session.HintDisplayRequested(), run,
          "manual Hint remains available independently of Legal Moves preference");
    Check(session.TryGetHintMoveSquares(hintFrom, hintTo) &&
          Tap(session, PhysicalOwner(session, false), runtime::VirtualLitchPhysicalButton::O) &&
          !session.HintDisplayRequested(), run,
          "manual retap hides the Hint independently of disabled Legal Moves");
    Check(Tap(session, PhysicalOwner(session, false), runtime::VirtualLitchPhysicalButton::O) &&
          session.HintDisplayRequested() && session.TryGetHintMoveSquares(hintFrom, hintTo), run,
          "manual Hint can be revealed again while Legal Moves remains disabled");
    uint8_t replayFrom = 64U, replayTo = 64U;
    Check(Coaching(session, PhysicalOwner(session, false), true) &&
          session.TryGetHintMoveSquares(hintFrom, hintTo) &&
          Tap(session, PhysicalOwner(session, false), runtime::VirtualLitchPhysicalButton::O) &&
          session.HintDisplayRequested() && session.TryGetHintMoveSquares(replayFrom, replayTo) &&
          replayFrom == hintFrom && replayTo == hintTo, run,
          "Coaching retap preserves the exact shown Hint while Legal Moves remains disabled");
    uint64_t occupancy = OPENING & ~(UINT64_C(1) << 52U);
    Check(Observe(session, occupancy) &&
          (session.State().valid_destination_mask & (UINT64_C(1) << 36U)) != 0ULL, run,
          "raw pickup retains engine legality with P1 Legal Moves disabled");
    Check(session.Advance(150U) == VIRTUAL_LITCH_OK &&
          Energy(session.SemanticFrame().board[36U][0U]) == 0U &&
          Energy(session.SemanticFrame().board[36U][2U]) == 0U, run,
          "disabled P1 destination emitters stay dark past the e4 reveal window");
    occupancy |= UINT64_C(1) << 36U;
    Check(Observe(session, occupancy) && session.Advance(2200U) == VIRTUAL_LITCH_OK &&
          session.UndoHistoryMoveCount() == 1U, run, "legal move commits with Legal Moves off");
    occupancy &= ~(UINT64_C(1) << 13U);
    Check(Observe(session, occupancy) && session.State().selected_square == 13U &&
          (session.State().valid_destination_mask & (UINT64_C(1) << 21U)) != 0ULL, run,
          "P2 immediately owns its legal f6 destination");
    Check(session.Advance(100U) == VIRTUAL_LITCH_OK &&
          Energy(session.SemanticFrame().board[21U][0U]) != 0U &&
          Energy(session.SemanticFrame().board[21U][2U]) != 0U, run,
          "P2 independently reveals both rails of its normal f6 destination cue");
    occupancy |= UINT64_C(1) << 21U;
    Check(Observe(session, occupancy) && session.Advance(2200U) == VIRTUAL_LITCH_OK &&
          Move(session, occupancy, 59U, 31U) && session.Advance(2200U) == VIRTUAL_LITCH_OK, run,
          "raw e4/f6/Qh5 gives ordinary check without injecting FEN or renderer state");
    Check(Setting(session, "P2_CHECK_CUE", "false") &&
              Setting(session, "P1_CHECK_CUE", "false"), run,
          "checked player's preference owns both halves independently of the checker player's preference");
    const auto baseline = session.SemanticFrame();
    Check(Setting(session, "P2_CHECK_CUE", "true"), run, "checked player enables the linked white Check cue");
    bool kingCall = false;
    for (uint8_t sample = 0U; sample < 90U; ++sample) {
        const auto king = session.SemanticFrame().board[4U][1U];
        if (king.r == king.g && king.g == king.b && king.g > baseline.board[4U][1U].g) {
            kingCall = true;
            break;
        }
        Check(session.Advance(20U) == VIRTUAL_LITCH_OK, run, "wait within one bounded Check cycle for the king's white call");
    }
    const auto checked = session.SemanticFrame();
    const auto checkedPhysical = session.PhysicalFrame();
    Check(Setting(session, "P2_CHECK_CUE", "false"), run, "checked player can disable its optional cue");
    const auto plain = session.SemanticFrame();
    Check(kingCall && checked.board[4U][1U].g > plain.board[4U][1U].g &&
              std::memcmp(&checked.board[31U][1U], &plain.board[31U][1U], sizeof(virtual_litch_color)) == 0, run,
          "the king calls in white while the checking Queen keeps its ordinary color until the answer");
    for (uint8_t square = 0U; square < 64U; ++square) {
        for (uint8_t slot : {0U, 2U}) {
            Check(std::memcmp(&checked.board[square][slot], &plain.board[square][slot],
                              sizeof(virtual_litch_color)) == 0, run,
                  "Check Cue does not add an outward or neighboring halo");
        }
    }
    const uint8_t kingPhysical = runtime::MapSquareIndex(4U, Rotated(session));
    const auto kingTargets = hardware::MapV3SquareLedTargets(kingPhysical, hardware::SquareLedSlot::Piece, false);
    for (uint8_t index = 0U; index < kingTargets.count; ++index) {
        const auto target = kingTargets.indices[index];
        Check(std::memcmp(&checkedPhysical.pixels[target], &session.PhysicalFrame().pixels[target],
                          sizeof(virtual_litch_color)) != 0, run,
              "actual V3.3 mapped output changes on both inward king emitters");
        const auto on = checkedPhysical.pixels[target];
        const auto off = session.PhysicalFrame().pixels[target];
        std::printf("[ASSISTANCE_CHECK_MAPPED] index=%u on=%u,%u,%u,%u off=%u,%u,%u,%u\n",
            target, on.r, on.g, on.b, on.w, off.r, off.g, off.b, off.w);
    }
    std::printf("[ASSISTANCE_CHECK] king=4 on=%u,%u,%u off=%u,%u,%u; outward and neighbors unchanged\n",
        checked.board[4U][1U].r, checked.board[4U][1U].g, checked.board[4U][1U].b,
        plain.board[4U][1U].r, plain.board[4U][1U].g, plain.board[4U][1U].b);
    Check(Setting(session, "P2_CHECK_CUE", "true"), run, "resume the same Check episode for its answer");
    bool checkerAnswer = false;
    for (uint8_t sample = 0U; sample < 90U; ++sample) {
        const auto checker = session.SemanticFrame().board[31U][1U];
        if (checker.r == checker.g && checker.g == checker.b && checker.g > baseline.board[31U][1U].g) {
            checkerAnswer = true;
            break;
        }
        Check(session.Advance(20U) == VIRTUAL_LITCH_OK, run, "wait within the same cycle for the checking Queen's white answer");
    }
    const auto answer = session.SemanticFrame();
    const auto answerPhysical = session.PhysicalFrame();
    Check(Setting(session, "P2_CHECK_CUE", "false"), run, "disable both linked beats through the checked player's setting");
    const auto answerPlain = session.SemanticFrame();
    Check(checkerAnswer &&
              std::memcmp(&answer.board[4U][1U], &answerPlain.board[4U][1U], sizeof(virtual_litch_color)) == 0, run,
          "the actual checking Queen answers in white after the king returns to its ordinary player color");
    const auto checkerTargets = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3_3S3, 31U, hardware::SquareLedSlot::Piece, Rotated(session));
    for (uint8_t index = 0U; index < checkerTargets.count; ++index) {
        const auto target = checkerTargets.indices[index];
        Check(std::memcmp(&answerPhysical.pixels[target], &session.PhysicalFrame().pixels[target],
                          sizeof(virtual_litch_color)) != 0, run, "both real checking-Queen Piece LEDs deliver the white answer");
        const auto on = answerPhysical.pixels[target];
        const auto off = session.PhysicalFrame().pixels[target];
        std::printf("[ASSISTANCE_CHECK_ANSWER] king=4 checker=31 index=%u on=%u,%u,%u,%u off=%u,%u,%u,%u next=resolve-check\n",
            target, on.r, on.g, on.b, on.w, off.r, off.g, off.b, off.w);
    }
}

int main() {
    AssistancePreferencesPersistAndCpuGesturesJourney();
    IndependentLegalMovesAndInwardCheckJourney();
    DirectedV33HintAndCpuDestinationJourney(false);
    DirectedV33HintAndCpuDestinationJourney(true);
    for (bool duck : {false, true}) {
        for (bool firstP2 : {false, true}) {
            for (bool rotated : {false, true}) {
                AutomaticCoachingWholeTurn(duck, firstP2, rotated);
            }
        }
    }
    AutomaticCoachingWholeTurn(true, false, false, true);
    for (bool firstP2 : {false, true}) {
        for (bool rotated : {false, true}) {
            AutomaticCoachingDuckUndoAndCancelRedo(firstP2, rotated);
        }
    }
    std::printf("AutomaticCoachingWholeTurn failures=%d; engine/rules/input/render are production; "
                "native virtual time is not S3 latency or Elo evidence\n", failures);
    return failures == 0 ? 0 : 1;
}
