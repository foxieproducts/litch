#include <array>
#include <cstdio>
#include <cstring>

#include "bounded_hint_engine.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "virtual_litch_session.hpp"

namespace {

constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t E2 = 52U, E4 = 36U, E5 = 28U;
constexpr uint8_t D2 = 51U, D5 = 27U, D6 = 19U, A3 = 40U;
constexpr const char* OPENING_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
constexpr const char* CIRCLE = "\"press_circle_to_confirm\"";
int g_failures = 0;

constexpr uint64_t Bit(const uint8_t square) { return UINT64_C(1) << square; }
constexpr uint64_t E4_PLACED = (OPENING & ~Bit(E2)) | Bit(E4);

bool SameOwnerHue(const virtual_litch_color a, const virtual_litch_color b) {
    const uint8_t left[]{a.r, a.g, a.b}, right[]{b.r, b.g, b.b};
    if ((a.r | a.g | a.b) == 0U || (b.r | b.g | b.b) == 0U) return false;
    for (uint8_t i = 0U; i < 3U; ++i) {
        if ((left[i] == 0U) != (right[i] == 0U)) return false;
        for (uint8_t j = 0U; j < 3U; ++j) {
            const int delta = static_cast<int>(left[i]) * right[j] - static_cast<int>(left[j]) * right[i];
            if (delta < -510 || delta > 510) return false;
        }
    }
    return true;
}

bool Equal(const virtual_litch_color a, const virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}

bool WarmRemovalCue(const virtual_litch_semantic_frame& frame, const uint8_t square) {
    const auto& left = frame.board[square][0U];
    return Equal(left, frame.board[square][2U]) && left.r > 0U && left.r >= left.g &&
           left.b == 0U && left.w == 0U && Equal(frame.board[square][1U], {});
}

virtual_litch_config Config() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    return config;
}

bool SetPolicy(runtime::VirtualLitchSession& board, const uint16_t request,
               const char* value) {
    modules::ControlMutationOutcome outcome;
    return board.MutateSettingJson(request, "MOVE_CONFIRM", value, outcome) ==
               VIRTUAL_LITCH_OK &&
           (outcome.status == modules::ControlMutationStatus::Applied ||
            outcome.status == modules::ControlMutationStatus::Noop) &&
           outcome.reason == modules::ControlMutationReason::None &&
           // Deliver the acknowledged preference on the next normal runtime pass,
           // before any journey time jump starts or samples its fresh deadline.
           board.Advance(0U) == VIRTUAL_LITCH_OK;
}

bool FenEquals(const test_support::BoundedHintSession& fixture, const char* expected) {
    char fen[128]{};
    return fixture.engine.GetFen(fen, sizeof(fen)) && std::strcmp(fen, expected) == 0;
}

bool Ready(runtime::VirtualLitchSession& board) {
    return board.Reset() == VIRTUAL_LITCH_OK &&
           board.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
           board.Advance(200U) == VIRTUAL_LITCH_OK &&
           board.State().expected_occupied_mask == OPENING &&
           board.State().interaction_phase ==
               static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle);
}

bool Move(runtime::VirtualLitchSession& board, uint64_t& occupancy,
          const uint8_t from, const uint8_t to) {
    occupancy &= ~Bit(from);
    const bool lifted = board.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK;
    occupancy |= Bit(to);
    return lifted && board.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK;
}

bool Pending(const test_support::BoundedHintSession& fixture, const uint64_t before,
             const uint8_t history, const char* fen) {
    const auto& board = fixture.session;
    return board.State().expected_occupied_mask == before &&
           board.State().pending_action_token == 0U &&
           board.UndoHistoryMoveCount() == history && FenEquals(fixture, fen);
}

bool OpeningPending(const test_support::BoundedHintSession& fixture) {
    return Pending(fixture, OPENING, 0U, OPENING_FEN) &&
           fixture.session.State().side_to_move_black == 0U;
}

void Checkpoint(const runtime::VirtualLitchSession& board, const char* journey,
                const char* input, const char* next, const bool accepted) {
    if (!accepted) {
        ++g_failures;
        std::printf("[FAIL] %s: %s\n", journey, input);
    }
    const auto& state = board.State();
    const auto home = board.HomeStateLine();
    std::printf("[O_CONFIRM_JOURNEY] %s t=%lu input=%s accepted=%u "
                "occupancy=%016llx expected=%016llx phase=%s required=%u "
                "action=%lu side=%s history=%u correction=%016llx illegal=%016llx "
                "text=%.*s next=%s leds=",
                journey, static_cast<unsigned long>(state.now_ms), input,
                accepted ? 1U : 0U,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                runtime::ChessInteractionPhaseToString(
                    static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                state.required_action, static_cast<unsigned long>(state.pending_action_token),
                state.side_to_move_black != 0U ? "Black" : "White",
                board.UndoHistoryMoveCount(),
                static_cast<unsigned long long>(state.correction_mask),
                static_cast<unsigned long long>(state.human_illegal_mask),
                static_cast<int>(board.DisplayText().size()), board.DisplayText().data(), next);
    constexpr std::array<uint8_t, 9> SQUARES{{E2, E4, D2, A3, D5, D6, 60U, 62U, 61U}};
    constexpr std::array<const char*, 9> NAMES{{"e2", "e4", "d2", "a3", "d5", "d6",
                                             "e1", "g1", "f1"}};
    for (size_t index = 0U; index < SQUARES.size(); ++index) {
        const auto& leds = board.SemanticFrame().board[SQUARES[index]];
        std::printf("%s%s:s%u/%u/%u:p%u/%u/%u", index == 0U ? "" : ",",
                    NAMES[index], leds[0].r, leds[0].g, leds[0].b,
                    leds[1].r, leds[1].g, leds[1].b);
    }
    std::printf(" home=%s\n", home.CStr());
}

bool CircleLevel(runtime::VirtualLitchSession& board, const bool down) {
    return board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                              runtime::VirtualLitchPhysicalButton::O, down) ==
               VIRTUAL_LITCH_OK && board.Advance(20U) == VIRTUAL_LITCH_OK;
}

void ConfirmOnce(test_support::BoundedHintSession& fixture, const char* journey,
                 const uint64_t before, const uint64_t after, const uint8_t history,
                 const char* beforeFen) {
    auto& board = fixture.session;
    const bool hintBefore = board.HintDisplayRequested();
    const bool pressed = CircleLevel(board, true);
    const bool released = CircleLevel(board, false);
    // Debounced release resolves the unmatched O press. The full finalize
    // interval starts when gameplay accepts it, not at its buffered edge.
    const uint32_t confirmedAt = board.State().now_ms;
    const bool waiting = board.Advance(999U) == VIRTUAL_LITCH_OK;
    Checkpoint(board, journey, "accepted-O+999ms", "wait-final-1ms",
               pressed && released && waiting && board.State().now_ms == confirmedAt + 999U &&
               Pending(fixture, before, history, beforeFen));
    const bool completed = board.Advance(1U) == VIRTUAL_LITCH_OK &&
        board.State().expected_occupied_mask == after &&
        board.State().side_to_move_black == 1U &&
        board.UndoHistoryMoveCount() == history + 1U &&
        board.HintDisplayRequested() == hintBefore;
    Checkpoint(board, journey, "accepted-O+1000ms", "lift-Black-piece", completed);
    Checkpoint(board, journey, "advance-5000ms-after-commit", "lift-Black-piece",
               board.Advance(5000U) == VIRTUAL_LITCH_OK &&
               board.State().expected_occupied_mask == after &&
               board.UndoHistoryMoveCount() == history + 1U &&
               board.State().side_to_move_black == 1U);
}

void EnPassantCircleJourney() {
    constexpr const char* NAME = "A1-mover-first-en-passant-waits-for-O";
    test_support::BoundedHintSession fixture(Config());
    auto& board = fixture.session;
    uint64_t occupancy = OPENING;
    bool ready = Ready(board) && SetPolicy(board, 710U, "\"settle_2s\"");
    constexpr std::array<std::array<uint8_t, 2>, 4> SETUP{{
        {{E2, E4}}, {{8U, 16U}}, {{E4, E5}}, {{11U, D5}},
    }};
    for (const auto& move : SETUP) {
        ready = ready && Move(board, occupancy, move[0], move[1]) &&
            board.Advance(2000U) == VIRTUAL_LITCH_OK &&
            board.Advance(600U) == VIRTUAL_LITCH_OK;
    }
    char beforeFen[128]{};
    ready = ready && fixture.engine.GetFen(beforeFen, sizeof(beforeFen)) &&
        board.UndoHistoryMoveCount() == 4U && SetPolicy(board, 711U, CIRCLE);
    Checkpoint(board, NAME, "e2e4-a7a6-e4e5-d7d5-then-app-O-policy", "lift-e5", ready);
    if (!ready) { return; }
    const uint64_t before = occupancy;
    const auto ownerColor = board.SemanticFrame().board[E5][1];
    const bool placed = Move(board, occupancy, E5, D6);
    Checkpoint(board, NAME, "lift-e5-place-d6-leave-pawn-d5", "remove-pawn-d5",
               placed && Pending(fixture, before, 4U, beforeFen) &&
               board.State().required_action ==
                   static_cast<uint8_t>(runtime::RequiredPhysicalAction::RemoveCapture) &&
               Equal(ownerColor, board.SemanticFrame().board[D6][1]));
    bool sawStrike = false, sawRest = false, heldCorrectly = true;
    for (uint8_t sample = 0U; sample < 12U; ++sample) {
        const bool advanced = board.Advance(100U) == VIRTUAL_LITCH_OK;
        heldCorrectly = advanced && heldCorrectly &&
            Pending(fixture, before, 4U, beforeFen) &&
            Equal(ownerColor, board.SemanticFrame().board[D6][1]);
        const auto rail = board.SemanticFrame().board[D5][0U];
        heldCorrectly = heldCorrectly && WarmRemovalCue(board.SemanticFrame(), D5);
        sawStrike = sawStrike || Equal(rail, {255U, 101U, 0U, 0U});
        sawRest = sawRest || Equal(rail, {64U, 25U, 0U, 0U});
        if (sample == 2U || sample == 7U) {
            Checkpoint(board, NAME, "sample-capture-removal-flash", "remove-pawn-d5",
                       heldCorrectly);
        }
    }
    Checkpoint(board, NAME, "early-O-while-d5-occupied+1000ms", "remove-pawn-d5-then-press-O-again",
               CircleLevel(board, true) && CircleLevel(board, false) &&
               board.Advance(1000U) == VIRTUAL_LITCH_OK &&
               Pending(fixture, before, 4U, beforeFen));
    const bool waitedForRemoval = board.Advance(4000U) == VIRTUAL_LITCH_OK;
    Checkpoint(board, NAME, "captured-pawn-still-present+4000ms", "remove-pawn-d5",
               waitedForRemoval && heldCorrectly && sawStrike && sawRest &&
               Pending(fixture, before, 4U, beforeFen));
    occupancy &= ~Bit(D5);
    Checkpoint(board, NAME, "remove-captured-pawn-d5", "press-O-or-relift-d6",
               board.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
               Pending(fixture, before, 4U, beforeFen) &&
               board.State().interaction_phase ==
                   static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
               !WarmRemovalCue(board.SemanticFrame(), D5) &&
               Equal(ownerColor, board.SemanticFrame().board[D6][1]));
    Checkpoint(board, NAME, "capture-cleared+2001ms", "press-O-or-relift-d6",
               board.Advance(2001U) == VIRTUAL_LITCH_OK &&
               Pending(fixture, before, 4U, beforeFen));
    Checkpoint(board, NAME, "capture-cleared+5001ms", "press-O-or-relift-d6",
               board.Advance(3000U) == VIRTUAL_LITCH_OK &&
               Pending(fixture, before, 4U, beforeFen));
    ConfirmOnce(fixture, NAME, before, occupancy, 4U, beforeFen);
}

enum class ReturnOrder { SourceFirst, OtherFirst, Together };

void CorrectAndCancelJourney(const ReturnOrder order) {
    const char* name = order == ReturnOrder::SourceFirst ? "A2-O-cancel-return-e2-then-d2" :
        order == ReturnOrder::OtherFirst ? "A2-O-cancel-return-d2-then-e2" :
                                         "A2-accepted-O-cancel-return-e2-d2-together";
    test_support::BoundedHintSession fixture(Config());
    auto& board = fixture.session;
    uint64_t occupancy = OPENING;
    const bool ready = Ready(board) && SetPolicy(board, 720U, CIRCLE) &&
        Move(board, occupancy, E2, E4);
    Checkpoint(board, name, "app-O-policy-lift-e2-place-e4", "press-O-or-relift-e4",
               ready && OpeningPending(fixture));
    if (!ready) { return; }
    occupancy |= Bit(A3);
    Checkpoint(board, name, "place-extra-piece-a3", "remove-extra-a3",
               board.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
               board.Advance(5001U) == VIRTUAL_LITCH_OK && OpeningPending(fixture) &&
               (board.State().human_illegal_mask & Bit(A3)) != 0ULL);
    Checkpoint(board, name, "raw-O-with-extra-a3+1000ms", "remove-extra-a3-then-confirm-again",
               CircleLevel(board, true) && CircleLevel(board, false) &&
               board.Advance(1000U) == VIRTUAL_LITCH_OK && OpeningPending(fixture));
    occupancy &= ~Bit(A3);
    Checkpoint(board, name, "remove-extra-piece-a3", "press-O-or-relift-e4",
               board.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
               OpeningPending(fixture) && board.State().human_illegal_mask == 0ULL &&
               board.State().correction_mask == 0ULL);
    Checkpoint(board, name, "extra-removed+1200ms-no-banked-O", "press-fresh-O-or-relift-e4",
               board.Advance(1200U) == VIRTUAL_LITCH_OK && OpeningPending(fixture));
    if (order == ReturnOrder::Together) {
        const bool pressed = CircleLevel(board, true);
        const bool released = CircleLevel(board, false);
        const bool advanced = board.Advance(300U) == VIRTUAL_LITCH_OK;
        Checkpoint(board, name, "fresh-valid-O-accepted+300ms", "relift-e4-to-cancel-armed-confirmation",
                   pressed && released && advanced && OpeningPending(fixture));
    }
    occupancy &= ~Bit(D2);
    Checkpoint(board, name, "lift-unrelated-pawn-d2", "return-d2-or-relift-e4-to-cancel",
               board.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
               OpeningPending(fixture) &&
               ((board.State().correction_mask | board.State().human_illegal_mask) & Bit(D2)) != 0ULL);
    occupancy &= ~Bit(E4);
    Checkpoint(board, name, "relift-e4-while-d2-missing", "restore-e2-and-d2",
               board.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK && OpeningPending(fixture));
    if (order == ReturnOrder::Together) {
        const bool advanced = board.Advance(1000U) == VIRTUAL_LITCH_OK;
        Checkpoint(board, name, "relift-cancels-armed-O-past-original-deadline", "restore-e2-and-d2",
                   advanced && OpeningPending(fixture));
    }
    if (order != ReturnOrder::Together) {
        const bool sourceFirst = order == ReturnOrder::SourceFirst;
        occupancy |= Bit(sourceFirst ? E2 : D2);
        Checkpoint(board, name, sourceFirst ? "return-e2-first" : "return-d2-first",
                   sourceFirst ? "return-d2" : "return-e2",
                   board.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
                   board.Advance(2200U) == VIRTUAL_LITCH_OK && OpeningPending(fixture));
    }
    occupancy |= Bit(E2) | Bit(D2);
    Checkpoint(board, name, "finish-return-e2-and-d2", "lift-White-piece",
               board.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
               board.Advance(560U) == VIRTUAL_LITCH_OK && OpeningPending(fixture) &&
               board.State().interaction_phase ==
                   static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
               board.State().human_illegal_mask == 0ULL && board.State().correction_mask == 0ULL);
    Checkpoint(board, name, "cancelled-position+5000ms", "lift-White-piece",
               board.Advance(5000U) == VIRTUAL_LITCH_OK && OpeningPending(fixture));
    Checkpoint(board, name, "next-valid-lift-e2-place-e4", "press-O",
               Move(board, occupancy, E2, E4) && OpeningPending(fixture));
    ConfirmOnce(fixture, name, OPENING, E4_PLACED, 0U, OPENING_FEN);
}


void CircleToTimedJourney() {
    constexpr const char* NAME = "A3-live-O-to-2s-starts-full-interval";
    test_support::BoundedHintSession fixture(Config());
    auto& board = fixture.session;
    uint64_t occupancy = OPENING;
    const bool ready = Ready(board) && SetPolicy(board, 730U, CIRCLE) &&
        Move(board, occupancy, E2, E4) && board.Advance(5001U) == VIRTUAL_LITCH_OK;
    Checkpoint(board, NAME, "O-confirm-placement+5001ms", "press-O-or-change-preference",
               ready && OpeningPending(fixture));
    if (!ready) { return; }
    Checkpoint(board, NAME, "app-acknowledged-policy-2s", "wait-new-full-2s",
               SetPolicy(board, 731U, "\"settle_2s\"") && OpeningPending(fixture));
    const uint32_t changedAt = board.State().now_ms;
    Checkpoint(board, NAME, "policy-change+1000ms-repeat-identical-2s", "wait-remaining-1000ms",
               board.Advance(1000U) == VIRTUAL_LITCH_OK &&
               SetPolicy(board, 732U, "\"settle_2s\"") && OpeningPending(fixture));
    Checkpoint(board, NAME, "policy-change+1999ms", "wait-final-1ms",
               board.Advance(999U) == VIRTUAL_LITCH_OK && OpeningPending(fixture));
    Checkpoint(board, NAME, "policy-change+2000ms", "lift-Black-piece",
               board.Advance(1U) == VIRTUAL_LITCH_OK &&
               board.State().now_ms == changedAt + 2000U &&
               board.State().expected_occupied_mask == E4_PLACED &&
               board.State().side_to_move_black == 1U && board.UndoHistoryMoveCount() == 1U);
    Checkpoint(board, NAME, "completed-change+5000ms", "lift-Black-piece",
               board.Advance(5000U) == VIRTUAL_LITCH_OK &&
               board.State().expected_occupied_mask == E4_PLACED && board.UndoHistoryMoveCount() == 1U);
}

void TimedToCircleJourney() {
    constexpr const char* NAME = "A3-live-2s-to-O-stops-auto-confirmation";
    test_support::BoundedHintSession fixture(Config());
    auto& board = fixture.session;
    uint64_t occupancy = OPENING;
    const bool ready = Ready(board) && SetPolicy(board, 740U, "\"settle_2s\"") &&
        Move(board, occupancy, E2, E4) && board.Advance(1500U) == VIRTUAL_LITCH_OK;
    Checkpoint(board, NAME, "timed-placement+1500ms", "wait-or-select-O-confirmation",
               ready && OpeningPending(fixture));
    if (!ready) { return; }
    Checkpoint(board, NAME, "app-acknowledged-policy-O", "press-O-or-relift-e4",
               SetPolicy(board, 741U, CIRCLE) && OpeningPending(fixture));
    Checkpoint(board, NAME, "past-old-deadline-and-new-policy+5001ms", "press-O-or-relift-e4",
               board.Advance(5001U) == VIRTUAL_LITCH_OK && OpeningPending(fixture));
    ConfirmOnce(fixture, NAME, OPENING, E4_PLACED, 0U, OPENING_FEN);
}

void AcceptedCircleWindowJourney(const uint32_t holdMs = 0U) {
    const char* name = holdMs == 0U
        ? "A3-accepted-O-keeps-one-second-deadline"
        : "A3-200ms-O-tap-keeps-full-accepted-deadline";
    test_support::BoundedHintSession fixture(Config());
    auto& board = fixture.session;
    uint64_t occupancy = OPENING;
    const bool ready = Ready(board) && SetPolicy(board, 750U, CIRCLE) &&
        Move(board, occupancy, E2, E4) && CircleLevel(board, true) &&
        board.Advance(holdMs) == VIRTUAL_LITCH_OK && OpeningPending(fixture) &&
        CircleLevel(board, false);
    const uint32_t confirmedAt = board.State().now_ms;
    Checkpoint(board, name, "O-release-accepted", "wait-existing-1s",
               ready && OpeningPending(fixture));
    if (!ready) { return; }
    Checkpoint(board, name, "O-release+300ms-then-app-policy-5s", "wait-remaining-700ms",
               board.Advance(300U) == VIRTUAL_LITCH_OK &&
               board.State().now_ms == confirmedAt + 300U &&
               SetPolicy(board, 751U, "\"settle_5s\"") && OpeningPending(fixture));
    Checkpoint(board, name, "accepted-O+999ms", "wait-final-1ms",
               board.Advance(699U) == VIRTUAL_LITCH_OK && OpeningPending(fixture));
    Checkpoint(board, name, "accepted-O+1000ms", "lift-Black-piece",
               board.Advance(1U) == VIRTUAL_LITCH_OK &&
               board.State().now_ms == confirmedAt + 1000U &&
               board.State().expected_occupied_mask == E4_PLACED &&
               board.State().side_to_move_black == 1U && board.UndoHistoryMoveCount() == 1U);
    Checkpoint(board, name, "changed-preference+5000ms-after-commit", "lift-Black-piece",
               board.Advance(5000U) == VIRTUAL_LITCH_OK &&
               board.State().expected_occupied_mask == E4_PLACED && board.UndoHistoryMoveCount() == 1U);
}


void AcceptedCircleCorrectionJourney() {
    constexpr const char* NAME = "A3-accepted-O-correction-preserves-commit-deadline";
    test_support::BoundedHintSession fixture(Config());
    auto& board = fixture.session;
    uint64_t occupancy = OPENING;
    const bool ready = Ready(board) && SetPolicy(board, 760U, CIRCLE) &&
        Move(board, occupancy, E2, E4) && CircleLevel(board, true) &&
        CircleLevel(board, false);
    const uint32_t confirmedAt = board.State().now_ms;
    Checkpoint(board, NAME, "O-release-accepted", "wait-existing-1s",
               ready && OpeningPending(fixture));
    if (!ready) { return; }
    const bool progressed = board.Advance(300U) == VIRTUAL_LITCH_OK;
    occupancy |= Bit(A3);
    Checkpoint(board, NAME, "accepted-O+300ms-place-extra-a3", "remove-extra-a3",
               progressed && board.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
               board.State().now_ms == confirmedAt + 300U && OpeningPending(fixture) &&
               (board.State().human_illegal_mask & Bit(A3)) != 0ULL);
    Checkpoint(board, NAME, "accepted-O+1000ms-extra-a3-still-present", "remove-extra-a3",
               board.Advance(700U) == VIRTUAL_LITCH_OK && OpeningPending(fixture));
    occupancy &= ~Bit(A3);
    Checkpoint(board, NAME, "remove-extra-a3-after-accepted-deadline", "next-runtime-tick-completes-confirmed-move",
               board.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
               OpeningPending(fixture) &&
               board.State().human_illegal_mask == 0ULL && board.State().correction_mask == 0ULL);
    // Firmware polls Hall after Tick. Run the next normal pass without adding
    // time or another O press: the accepted deadline has already expired.
    const uint32_t restoredAt = board.State().now_ms;
    const bool ticked = board.Advance(0U) == VIRTUAL_LITCH_OK;
    Checkpoint(board, NAME, "restored-board-next-runtime-tick-no-time-or-new-O", "lift-Black-piece",
               ticked && board.State().now_ms == restoredAt &&
               restoredAt == confirmedAt + 1000U &&
               board.State().expected_occupied_mask == E4_PLACED &&
               board.State().side_to_move_black == 1U && board.UndoHistoryMoveCount() == 1U &&
               board.State().human_illegal_mask == 0ULL && board.State().correction_mask == 0ULL);
    Checkpoint(board, NAME, "restored-confirmed-position+5000ms", "lift-Black-piece",
               board.Advance(5000U) == VIRTUAL_LITCH_OK &&
               board.State().expected_occupied_mask == E4_PLACED && board.UndoHistoryMoveCount() == 1U);
}

void CastleRetainedCircleJourney() {
    constexpr const char* NAME = "O-castle-commits-once-retained-settle-finishes";
    auto config = Config();
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    uint64_t occupancy = OPENING;
    bool ready = Ready(board) && SetPolicy(board, 770U, "\"settle_2s\"");
    constexpr std::array<std::array<uint8_t, 2>, 6> SETUP{{
        {{E2, E4}}, {{8U, 16U}}, {{62U, 45U}},
        {{16U, 24U}}, {{61U, E2}}, {{9U, 17U}},
    }};
    for (const auto& move : SETUP) {
        ready = ready && Move(board, occupancy, move[0], move[1]) &&
            board.Advance(2000U) == VIRTUAL_LITCH_OK &&
            board.Advance(600U) == VIRTUAL_LITCH_OK;
    }
    ready = ready && board.UndoHistoryMoveCount() == 6U && SetPolicy(board, 771U, CIRCLE);
    Checkpoint(board, NAME, "e2e4-a7a6-g1f3-a6a5-f1e2-b7b6-then-app-O-policy", "castle-e1g1",
               ready);
    if (!ready) { return; }
    const uint64_t before = occupancy;
    const auto ownerColor = board.SemanticFrame().board[60U][1];
    Checkpoint(board, NAME, "lift-king-e1-place-g1", "move-rook-h1-to-f1",
               Move(board, occupancy, 60U, 62U) &&
               board.State().expected_occupied_mask == before &&
               board.UndoHistoryMoveCount() == 6U &&
               board.State().required_action ==
                   static_cast<uint8_t>(runtime::RequiredPhysicalAction::LiftSecondary) &&
               Equal(ownerColor, board.SemanticFrame().board[62U][1]));
    Checkpoint(board, NAME, "lift-rook-h1-place-f1-composite-acknowledged", "press-O-to-finish-rook-feedback",
               Move(board, occupancy, 63U, 61U) &&
               board.State().interaction_phase ==
                   static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
               board.State().expected_occupied_mask == occupancy &&
               board.State().side_to_move_black == 1U && board.UndoHistoryMoveCount() == 7U &&
               std::strstr(board.HomeStateLine().CStr(), "\"a\":\"settle\"") != nullptr &&
               SameOwnerHue(ownerColor, board.SemanticFrame().board[61U][1]));
    char castleFen[128]{};
    const bool gotCastleFen = fixture.engine.GetFen(castleFen, sizeof(castleFen));
    const bool waited = board.Advance(5001U) == VIRTUAL_LITCH_OK;
    Checkpoint(board, NAME, "committed-castle+5001ms-without-O", "press-O-to-finish-rook-feedback",
               gotCastleFen && waited && Pending(fixture, occupancy, 7U, castleFen) &&
               std::strstr(board.HomeStateLine().CStr(), "\"a\":\"settle\"") != nullptr);
    const bool pressed = CircleLevel(board, true);
    const bool released = CircleLevel(board, false);
    const uint32_t confirmedAt = board.State().now_ms;
    const bool almostFinished = board.Advance(999U) == VIRTUAL_LITCH_OK;
    Checkpoint(board, NAME, "accepted-O+999ms-no-next-piece-input", "wait-final-1ms",
               pressed && released && almostFinished &&
               board.State().now_ms == confirmedAt + 999U &&
               Pending(fixture, occupancy, 7U, castleFen) &&
               std::strstr(board.HomeStateLine().CStr(), "\"a\":\"settle\"") != nullptr);
    const bool finished = board.Advance(1U) == VIRTUAL_LITCH_OK;
    Checkpoint(board, NAME, "accepted-O+1000ms-no-next-piece-input", "lift-Black-piece",
               finished && board.State().now_ms == confirmedAt + 1000U &&
               Pending(fixture, occupancy, 7U, castleFen) &&
               board.State().side_to_move_black == 1U &&
               std::strstr(board.HomeStateLine().CStr(), "\"a\":\"idle\"") != nullptr);
    const bool expired = board.Advance(5000U) == VIRTUAL_LITCH_OK;
    Checkpoint(board, NAME, "castle-feedback-expired+5000ms", "lift-Black-piece",
               expired && Pending(fixture, occupancy, 7U, castleFen) &&
               board.State().side_to_move_black == 1U &&
               std::strstr(board.HomeStateLine().CStr(), "\"a\":\"idle\"") != nullptr);
}

} // namespace

int RunOConfirmVirtualLitchJourneys() {
    EnPassantCircleJourney();
    CorrectAndCancelJourney(ReturnOrder::SourceFirst);
    CorrectAndCancelJourney(ReturnOrder::OtherFirst);
    CorrectAndCancelJourney(ReturnOrder::Together);
    CircleToTimedJourney();
    TimedToCircleJourney();
    AcceptedCircleWindowJourney();
    AcceptedCircleWindowJourney(200U);
    AcceptedCircleCorrectionJourney();
    CastleRetainedCircleJourney();
    return g_failures;
}
