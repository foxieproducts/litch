#include "bounded_hint_engine.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

#include <ArduinoJson.h>

#include "virtual_litch_session.hpp"
#include "hardware/led_layout.hpp"
#include "modules/game_persistence.hpp"
#include "modules/orientation.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/game_clock.hpp"

namespace {

constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t DUCK_A5 = 24U;
constexpr uint8_t DUCK_B5 = 25U;
int g_failures = 0;

enum class CpuTurn { ReplyToWhite, ReplyToBlack, Opening };

void Check(const bool condition, const char* run, const char* message) {
    if (!condition) {
        std::printf("[FAIL] CPU_DUCK_UNDO %s: %s\n", run, message);
        ++g_failures;
    }
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

bool ReadHome(const runtime::VirtualLitchSession& session, JsonDocument& home) {
    const bool parsed = deserializeJson(home, session.HomeStateLine().CStr()) ==
                        DeserializationError::Ok;
    Check(parsed, "home", "canonical Home projection remains valid JSON");
    return parsed;
}

bool Set(runtime::VirtualLitchSession& session, const uint16_t id,
         const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return session.MutateSettingJson(id, key, value, outcome) == VIRTUAL_LITCH_OK &&
           (outcome.status == modules::ControlMutationStatus::Applied ||
            outcome.status == modules::ControlMutationStatus::Noop);
}

uint8_t Square(const char* text) {
    if (text == nullptr || text[0] < 'a' || text[0] > 'h' ||
        text[1] < '1' || text[1] > '8') {
        return 64U;
    }
    return static_cast<uint8_t>((8 - (text[1] - '0')) * 8 + text[0] - 'a');
}

bool Move(runtime::VirtualLitchSession& session, uint64_t& occupancy,
          const uint8_t from, const uint8_t to, const uint32_t settle = 1U) {
    if (from >= 64U || to >= 64U) return false;
    occupancy &= ~(UINT64_C(1) << from);
    if (!Observe(session, occupancy)) return false;
    occupancy |= UINT64_C(1) << to;
    return Observe(session, occupancy) &&
           session.Advance(settle) == VIRTUAL_LITCH_OK;
}

bool PersistedHistoryIs(runtime::VirtualLitchSession& session, const uint16_t moves) {
    modules::GamePersistenceState persisted;
    return modules::ReadGamePersistenceState(
               session.SettingsForPersistenceTest().Doc(), persisted) &&
           persisted.undoHistoryCount ==
               moves * modules::UNDO_HISTORY_FIELDS_PER_MOVE;
}

bool HistoryIs(const runtime::VirtualLitchSession& session, const uint16_t moves) {
    return session.UndoHistoryMoveCount() == moves;
}

bool AdvanceWithScans(runtime::VirtualLitchSession& session,
                      const uint64_t occupancy, uint32_t durationMs) {
    while (durationMs != 0U) {
        const uint32_t step = durationMs < 50U ? durationMs : 50U;
        if (!Observe(session, occupancy) ||
            session.Advance(step) != VIRTUAL_LITCH_OK) return false;
        durationMs -= step;
    }
    return true;
}

uint16_t RingEnergy(const runtime::VirtualLitchSession& session,
                    const uint8_t logicalSquare) {
    if (logicalSquare >= 64U) return 0U;
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    const uint8_t physical = modules::MapSquareToPhysical(logicalSquare, rotate);
    uint16_t energy = 0U;
    for (const auto slot : {hardware::SquareLedSlot::Square0,
                           hardware::SquareLedSlot::Square2}) {
        const auto color = session.PhysicalFrame().pixels[
            hardware::LedIndexForSquare(physical, slot)];
        energy += color.r + color.g + color.b + color.w;
    }
    return energy;
}

bool HasYellowRing(const runtime::VirtualLitchSession& session,
                   const uint8_t logicalSquare) {
    if (logicalSquare >= 64U) return false;
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    const uint8_t physical = modules::MapSquareToPhysical(logicalSquare, rotate);
    const auto color = session.PhysicalFrame().pixels[
        hardware::LedIndexForSquare(physical, hardware::SquareLedSlot::Square0)];
    return color.r > color.b && color.g > color.b;
}

bool Yellow(const virtual_litch_color color) {
    return color.r != 0U && color.r == color.g && color.b == 0U && color.w == 0U;
}

bool Equal(const virtual_litch_color a, const virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}

void ObserveDuckGuidance(runtime::VirtualLitchSession& session,
                         const uint8_t from, const uint8_t to,
                         const bool minimal, const char* run, const char* stage) {
    Check(session.Advance(0U) == VIRTUAL_LITCH_OK, run, "render the current Duck action");
    const auto initial = session.SemanticFrame();
    const auto action = session.State().required_action;
    const auto phase = session.State().interaction_phase;
    const auto history = session.UndoHistoryMoveCount();
    StaticJsonDocument<1536> home;
    if (!ReadHome(session, home)) return;
    const std::string plan = home["dr"] | "";
    const std::string fen = home["fen"] | "";
    const uint8_t distance = static_cast<uint8_t>(std::max(
        std::abs(int(to / 8U) - int(from / 8U)),
        std::abs(int(to % 8U) - int(from % 8U))));
    const uint32_t pass = std::max(900U, 480U * distance) + 240U;
    bool steady = true, landing = true, identities = true, targetChanged = false;
    uint64_t transit = 0ULL;
    for (uint32_t age = 0U; age <= pass; age += 20U) {
        const auto& frame = session.SemanticFrame();
        landing = landing && Yellow(frame.board[from][0U]) &&
            Yellow(frame.board[to][0U]) && Yellow(frame.board[to][1U]) &&
            Yellow(frame.board[to][2U]);
        for (uint8_t slot = 0U; slot < frame.leds_per_square; ++slot) {
            steady = steady && Equal(initial.board[from][slot], frame.board[from][slot]) &&
                Equal(initial.board[to][slot], frame.board[to][slot]);
            targetChanged = targetChanged || !Equal(initial.board[to][slot], frame.board[to][slot]);
        }
        for (uint8_t square = 0U; square < 64U; ++square) {
            if (square == from || square == to) continue;
            const bool occupied = (session.State().physical_occupied_mask &
                (UINT64_C(1) << square)) != 0ULL;
            identities = identities && (!occupied || !Yellow(frame.board[square][1U]));
            if (Yellow(frame.board[square][0U]) || Yellow(frame.board[square][2U])) {
                // Observe spatial progress, without reproducing the renderer's
                // discrete-line or emitter interpolation algorithm.
                const int rank = square / 8U, file = square % 8U;
                const bool between = rank >= std::min(from / 8U, to / 8U) &&
                    rank <= std::max(from / 8U, to / 8U) &&
                    file >= std::min(from % 8U, to % 8U) &&
                    file <= std::max(from % 8U, to % 8U);
                Check(between, run, "yellow Duck transit stays between its known endpoints");
                transit |= UINT64_C(1) << square;
            }
        }
        if (age + 20U <= pass) {
            Check(session.Advance(20U) == VIRTUAL_LITCH_OK, run,
                  "observe a complete production Duck route pass");
        }
    }
    Check(landing, run, "known Duck source and persistent landing Piece invitation remain yellow");
    Check(identities, run, "yellow travel never invites pickup of another occupied piece");
    Check(minimal ? steady && transit == 0ULL
                  : distance > 1U ? transit != 0ULL : targetChanged,
          run, minimal ? "Minimal Duck endpoints stay steady with no travel while this action is held"
                       : "fixed Duck guidance travels toward its landing rather than showing endpoints alone");
    Check(ReadHome(session, home) && plan == (home["dr"] | "") &&
              fen == (home["fen"] | "") && session.State().required_action == action &&
              session.State().interaction_phase == phase &&
              session.State().correction_mask == 0ULL &&
              session.UndoHistoryMoveCount() == history,
          run, "timed guidance preserves the authoritative Duck target and pending boundary");
    std::printf("[CPU_DUCK_ROUTE] run=%s stage=%s from=%u to=%u minimal=%u "
                "transit=%016llx target_changed=%u steady=%u landing=%u identities=%u next=%s\n",
                run, stage, from, to, minimal, static_cast<unsigned long long>(transit),
                targetChanged, steady, landing, identities,
                action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::LiftPrimary)
                    ? "lift-Duck-from-known-source" : "place-Duck-at-known-target");
}

void Checkpoint(const runtime::VirtualLitchSession& session, const char* run,
                const char* stage, const uint8_t from, const uint8_t to,
                const char* next) {
    const auto& state = session.State();
    const auto* clock = session.GameClock();
    std::printf(
        "[CPU_DUCK_UNDO_JOURNEY] run=%s stage=%s t=%u phase=%u required=%u "
        "physical=%016llx expected=%016llx correction=%016llx pending=%u "
        "display=%.*s from=%u to=%u ring_from=%u ring_to=%u "
        "white_ms=%u black_ms=%u next=%s home=%s\n",
        run, stage, state.now_ms, state.interaction_phase, state.required_action,
        static_cast<unsigned long long>(state.physical_occupied_mask),
        static_cast<unsigned long long>(state.expected_occupied_mask),
        static_cast<unsigned long long>(state.correction_mask),
        session.IsUndoActionPending(),
        static_cast<int>(session.DisplayText().size()), session.DisplayText().data(),
        from, to, RingEnergy(session, from), RingEnergy(session, to),
        clock != nullptr ? clock->GetRemainingMs(runtime::ClockSide::White) : 0U,
        clock != nullptr ? clock->GetRemainingMs(runtime::ClockSide::Black) : 0U,
        next, session.HomeStateLine().CStr());
    std::printf("[CPU_DUCK_UNDO_FRAME] run=%s stage=%s t=%u semantic_rgb=",
                run, stage, state.now_ms);
    for (uint8_t square = 0U; square < 64U; ++square) {
        for (uint8_t slot = 0U; slot < session.SemanticFrame().leds_per_square; ++slot) {
            const auto color = session.SemanticFrame().board[square][slot];
            std::printf("%02x%02x%02x,", color.r, color.g, color.b);
        }
    }
    std::printf(" status_rgb=");
    for (const auto color : session.SemanticFrame().status) {
        std::printf("%02x%02x%02x,", color.r, color.g, color.b);
    }
    std::printf("\n");
}

bool Undo(runtime::VirtualLitchSession& session, const uint16_t id,
          const modules::ControlMutationStatus expected, const char* run) {
    virtual_litch_action_request request{};
    virtual_litch_default_action_request(&request);
    request.request_id = id;
    request.action = VIRTUAL_LITCH_CONTROL_UNDO;
    request.undo_move_count = 1U;
    modules::ControlMutationOutcome outcome;
    const bool received =
        session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
        outcome.status == modules::ControlMutationStatus::Accepted &&
        session.Advance(1U) == VIRTUAL_LITCH_OK &&
        session.TryTakeControlCompletion(outcome) && outcome.requestId == id;
    Check(received && outcome.status == expected, run,
          "canonical Undo receives the expected correlated terminal result");
    std::printf("[CPU_DUCK_UNDO_CONTROL] run=%s id=%u received=%u status=%u "
                "reason=%u pending=%u\n", run, id, received,
                static_cast<unsigned>(outcome.status),
                static_cast<unsigned>(outcome.reason), session.IsUndoActionPending());
    return received && outcome.status == expected;
}

bool CommitHumanTurn(runtime::VirtualLitchSession& session, uint64_t& occupancy,
                     const uint8_t from, const uint8_t to,
                     const uint8_t duckFrom, const uint8_t duckTo) {
    if (!Move(session, occupancy, from, to, 2600U)) return false;
    StaticJsonDocument<1536> home;
    if (!ReadHome(session, home) || std::strcmp(home["a"] | "", "duck") != 0) {
        return false;
    }
    return Move(session, occupancy, duckFrom, duckTo, 2000U);
}

void RunCpuDuckUndo(const CpuTurn turn, const bool firstP2, const bool rotate,
                    const bool minimal = false) {
    const bool cpuBlack = turn == CpuTurn::ReplyToWhite;
    const bool opening = turn == CpuTurn::Opening;
    char run[80] = {};
    std::snprintf(run, sizeof(run), "cpu-%s/%s/first-p%u/rotate-%u/%s",
                  cpuBlack ? "black" : "white", opening ? "opening" : "reply",
                  firstP2 ? 2U : 1U, rotate, minimal ? "Minimal" : "Normal");
    const bool inspectGuidance = minimal ||
        (turn == CpuTurn::ReplyToWhite && !firstP2 && !rotate);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    config.first_player_is_p2 = firstP2;
    config.board_rotate = rotate;
    config.square_rotate_for_output = modules::ComputeSquareRotate(rotate, firstP2);
    test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
    uint64_t occupancy = OPENING | (UINT64_C(1) << DUCK_A5);
    const bool initialized = session.Reset() == VIRTUAL_LITCH_OK &&
        Set(session, 100U, "VS_CPU_LEVEL", "0") &&
        Set(session, 101U, "TIMER", "2") &&
        Set(session, 102U, "BOARD_SETUP_ASSISTANCE", "false") &&
        Set(session, 103U, "WHITE_SQUARES_ON", "false") &&
        Set(session, 104U, "MINIMAL_ANIMATIONS", minimal ? "true" : "false") &&
        Observe(session, occupancy) && session.Advance(1000U) == VIRTUAL_LITCH_OK;
    Check(initialized, run, "raw opening setup selects Duck with an enabled 3+2 clock");
    if (!initialized) return;
    const auto* clock = session.GameClock();
    Check(clock != nullptr && clock->IsEnabled(), run,
          "the production clock is enabled for increment assertions");
    if (clock == nullptr || !clock->IsEnabled()) return;

    StaticJsonDocument<1536> home;
    if (!ReadHome(session, home)) return;
    modules::FenString decisionFen;
    Check(decisionFen.Assign(home["fen"] | ""), run, "decision FEN fits bounded storage");
    uint64_t decisionOccupancy = occupancy;
    if (!opening) {
        const bool whiteCommitted = CommitHumanTurn(
            session, occupancy, 52U, 36U, DUCK_A5, DUCK_B5);
        Check(whiteCommitted && HistoryIs(session, 1U), run,
              "real e2e4 plus a5-b5 commits exactly one human compound turn");
        if (!whiteCommitted) return;
    }

    if (turn == CpuTurn::ReplyToBlack) {
        if (!ReadHome(session, home)) return;
        Check(decisionFen.Assign(home["fen"] | ""), run, "decision FEN fits bounded storage");
        decisionOccupancy = occupancy;
        const bool blackCommitted = CommitHumanTurn(
            session, occupancy, 12U, 28U, DUCK_B5, DUCK_A5);
        Check(blackCommitted && HistoryIs(session, 2U), run,
              "real e7e5 plus b5-a5 supplies the prior human Black turn");
        if (!blackCommitted) return;
    }
    const uint16_t historyBefore = opening ? 0U : cpuBlack ? 1U : 2U;
    const uint8_t humanFrom = cpuBlack ? 52U : 12U;
    const uint8_t humanTo = cpuBlack ? 36U : 28U;
    const uint8_t duckNow = cpuBlack ? DUCK_B5 : DUCK_A5;
    const uint8_t duckPrior = cpuBlack ? DUCK_A5 : DUCK_B5;
    const uint64_t preCpuOccupancy = occupancy;
    if (!ReadHome(session, home)) return;
    modules::FenString preCpuFen;
    Check(preCpuFen.Assign(home["fen"] | ""), run, "CPU boundary FEN fits bounded storage");
    Check(Set(session, 110U, "VS_CPU_PLAYER", cpuBlack ? "2" : "1") &&
              Set(session, 111U, "VS_CPU_LEVEL", "1"), run,
          "acknowledged settings give the next real turn to the CPU");

    bool cpuReady = false;
    for (uint16_t sample = 0U; sample < 120U; ++sample) {
        if (!ReadHome(session, home)) break;
        if (std::strcmp(home["a"] | "", "cpu") == 0 && home["c"].is<JsonArray>()) {
            cpuReady = true;
            break;
        }
        if (session.Advance(100U) != VIRTUAL_LITCH_OK) break;
    }
    const uint8_t cpuFrom = Square(home["c"][0U] | "--");
    const uint8_t cpuTo = Square(home["c"][1U] | "--");
    const bool quietCpuMove = cpuReady && cpuFrom < 64U && cpuTo < 64U &&
        std::strcmp(home["c"][2U] | "", "--") == 0;
    Check(quietCpuMove, run,
          "production Duck engine selects a real quiet CPU chess move");
    if (!quietCpuMove) return;
    const uint32_t whiteBeforeChess = clock->GetRemainingMs(runtime::ClockSide::White);
    const uint32_t blackBeforeChess = clock->GetRemainingMs(runtime::ClockSide::Black);
    Checkpoint(session, run, "cpu-chess-prompt", cpuFrom, cpuTo, "move-CPU-chess-piece");
    const bool cpuStaged = Move(session, occupancy, cpuFrom, cpuTo, 2600U) &&
        ReadHome(session, home) && std::strcmp(home["a"] | "", "duck") == 0 &&
        (home["dr"] | "")[0] == 'c';
    Check(cpuStaged && preCpuFen.Equals(home["fen"] | "") &&
              HistoryIs(session, historyBefore) &&
              clock->GetRemainingMs(runtime::ClockSide::White) <= whiteBeforeChess &&
              clock->GetRemainingMs(runtime::ClockSide::Black) <= blackBeforeChess, run,
          "CPU chess acknowledgement stages its fixed Duck plan without history or increment");
    if (!cpuStaged) return;
    const char* duckPlan = home["dr"] | "";
    const uint8_t cpuDuckTo = std::strlen(duckPlan) == 5U
                                ? Square(duckPlan + 3U) : 64U;
    Check(cpuDuckTo < 64U && cpuDuckTo != duckNow, run,
          "Home supplies the real fixed CPU Duck destination");
    if (cpuDuckTo >= 64U || cpuDuckTo == duckNow) return;
    Check(HasYellowRing(session, duckNow) && HasYellowRing(session, cpuDuckTo), run,
          "fixed CPU Duck source and destination have physical yellow guidance");
    if (inspectGuidance) ObserveDuckGuidance(session, duckNow, cpuDuckTo, minimal, run, "cpu-unheld");
    const uint32_t whiteBefore = clock->GetRemainingMs(runtime::ClockSide::White);
    const uint32_t blackBefore = clock->GetRemainingMs(runtime::ClockSide::Black);
    Checkpoint(session, run, "cpu-duck-pending", duckNow, cpuDuckTo, "Undo-before-moving-Duck");

    occupancy &= ~(UINT64_C(1) << duckNow);
    Check(Observe(session, occupancy), run, "raw Duck lift enters physical relocation");
    if (inspectGuidance) ObserveDuckGuidance(session, duckNow, cpuDuckTo, minimal, run, "cpu-held");
    if (!Undo(session, 120U, modules::ControlMutationStatus::Rejected, run)) return;
    Check(!session.IsUndoActionPending() && HistoryIs(session, historyBefore), run,
          "rejected in-hand Undo keeps the CPU turn and history intact");
    Checkpoint(session, run, "lifted-duck-rejected", duckNow, cpuDuckTo, "return-Duck-to-source");
    occupancy |= UINT64_C(1) << duckNow;
    Check(Observe(session, occupancy) && ReadHome(session, home) &&
              (home["dr"] | "")[0] == 'c' &&
              Square((home["dr"] | "") + 3U) == cpuDuckTo, run,
          "returning Duck to source preserves the original CPU destination");

    if (!Undo(session, 121U, modules::ControlMutationStatus::Applied, run)) return;
    Check(session.IsUndoActionPending() && ReadHome(session, home) &&
              (home["up"] | 0U) == 1U && preCpuFen.Equals(home["fen"] | "") &&
              HistoryIs(session, historyBefore), run,
          "untouched CPU Duck Undo opens only a physical inverse, with unchanged FEN/history");
    Checkpoint(session, run, "cpu-chess-inverse", cpuTo, cpuFrom, "return-CPU-chess-piece");
    occupancy &= ~(UINT64_C(1) << cpuTo);
    Check(Observe(session, occupancy) && session.Advance(120U) == VIRTUAL_LITCH_OK &&
              RingEnergy(session, cpuFrom) != 0U &&
              session.State().required_action == static_cast<uint8_t>(
                  runtime::RequiredPhysicalAction::PlacePrimary) &&
              session.State().correction_mask == 0ULL, run,
          "CPU inverse keeps a physical return-square cue while the piece is held");
    if (!Undo(session, 122U, modules::ControlMutationStatus::Rejected, run)) return;
    Check(session.Advance(380U) == VIRTUAL_LITCH_OK &&
              RingEnergy(session, cpuFrom) != 0U && session.IsUndoActionPending(), run,
          "rejected extra Undo cannot erase the retained history continuation");
    Checkpoint(session, run, "cpu-piece-held", cpuTo, cpuFrom, "place-CPU-piece-at-origin");
    occupancy |= UINT64_C(1) << cpuFrom;
    const bool cpuReturned = Observe(session, occupancy) &&
        session.Advance(1U) == VIRTUAL_LITCH_OK && occupancy == preCpuOccupancy &&
        ReadHome(session, home);
    if (opening) {
        // Returning to the opening rearms the existing setup lifecycle.
        // Hardware keeps scanning unchanged occupancy through Duck detection.
        Check(AdvanceWithScans(session, occupancy, 500U) && ReadHome(session, home),
              run, "opening inverse completes normal stable setup scans");
        Check(cpuReturned && HistoryIs(session, 0U) &&
                  decisionFen.Equals(home["fen"] | "") &&
                  session.State().expected_occupied_mask == decisionOccupancy &&
                  !session.IsUndoActionPending() && !home.containsKey("dr") &&
                  clock->GetRemainingMs(runtime::ClockSide::White) <= whiteBefore &&
                  clock->GetRemainingMs(runtime::ClockSide::Black) <= blackBefore, run,
              "CPU opening returns physically without inventing a human history entry");
        Checkpoint(session, run, "opening-returned", cpuFrom, cpuTo, "CPU-still-owns-opening");
        return;
    }
    Check(cpuReturned &&
              std::strcmp(home["a"] | "", "duck") == 0 &&
              (home["dr"] | "")[0] == 'u' &&
              Square((home["dr"] | "") + 1U) == duckNow &&
              Square((home["dr"] | "") + 3U) == duckPrior &&
              (home["up"] | 0U) == 1U && HistoryIs(session, historyBefore) &&
              HasYellowRing(session, duckNow) && HasYellowRing(session, duckPrior), run,
          "CPU return continues directly into the preceding human turn's fixed Duck inverse");
    Checkpoint(session, run, "human-duck-inverse", duckNow, duckPrior, "return-prior-human-Duck");
    if (inspectGuidance) ObserveDuckGuidance(session, duckNow, duckPrior, minimal, run, "Undo-unheld");

    occupancy &= ~(UINT64_C(1) << duckNow);
    Check(Observe(session, occupancy), run, "lift Duck for the fixed human inverse");
    if (inspectGuidance) ObserveDuckGuidance(session, duckNow, duckPrior, minimal, run, "Undo-held");
    occupancy |= UINT64_C(1) << duckPrior;
    Check(Observe(session, occupancy) && session.Advance(1U) == VIRTUAL_LITCH_OK && ReadHome(session, home) &&
              decisionFen.Equals(home["fen"] | "") &&
              HistoryIs(session, historyBefore - 1U) && session.IsUndoActionPending() &&
              clock->GetRemainingMs(runtime::ClockSide::White) <= whiteBefore &&
              clock->GetRemainingMs(runtime::ClockSide::Black) <= blackBefore, run,
          "only the completed human turn rewinds once, without an extra increment");
    Checkpoint(session, run, "human-chess-inverse", humanTo, humanFrom, "return-human-chess-piece");
    Check(Move(session, occupancy, humanTo, humanFrom, 1U) &&
              occupancy == decisionOccupancy && !session.IsUndoActionPending(), run,
          "physical human inverse finishes the original Undo decision cycle");
    Check(AdvanceWithScans(session, occupancy, 4000U) && ReadHome(session, home) &&
              decisionFen.Equals(home["fen"] | "") &&
              HistoryIs(session, historyBefore - 1U) &&
              session.State().expected_occupied_mask == decisionOccupancy &&
              std::strcmp(home["a"] | "", "idle") == 0 &&
              (home["up"] | 0U) == 0U && !home.containsKey("dr") &&
              clock->GetRemainingMs(runtime::ClockSide::White) <= whiteBefore &&
              clock->GetRemainingMs(runtime::ClockSide::Black) <= blackBefore &&
              session.State().side_to_move_black != cpuBlack, run,
          "stable human decision boundary neither replays the CPU half nor adds clock increment");
    Check(PersistedHistoryIs(session, historyBefore - 1U), run,
          "completed stable inverse persists the same authoritative history");
    bool duckRouteRetired = true;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if (square == duckPrior) continue;
        for (uint8_t slot = 0U; slot < session.SemanticFrame().leds_per_square; ++slot) {
            duckRouteRetired = duckRouteRetired && !Yellow(session.SemanticFrame().board[square][slot]);
        }
    }
    Check(duckRouteRetired, run, "completed Undo retires yellow travel and the vacated Duck prompt");
    Checkpoint(session, run, "human-decision-restored", humanFrom, humanTo, "choose-a-human-move");
}

} // namespace

int RunCpuDuckUndoVirtualLitchJourneys() {
    for (const CpuTurn turn : {CpuTurn::ReplyToWhite, CpuTurn::ReplyToBlack,
                              CpuTurn::Opening}) {
        for (const bool firstP2 : {false, true}) {
            for (const bool rotate : {false, true}) {
                RunCpuDuckUndo(turn, firstP2, rotate);
            }
        }
    }
    RunCpuDuckUndo(CpuTurn::ReplyToWhite, false, false, true);
    return g_failures;
}
