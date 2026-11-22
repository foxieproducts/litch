#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "modules/orientation.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/game_clock.hpp"
#include "virtual_litch_session.hpp"

namespace {
constexpr uint64_t OPENING = UINT64_C(0xffff00000000ffff);
int g_failures = 0;

void Check(bool condition, const char* journey, const char* message) {
    if (!condition) {
        ++g_failures;
        std::printf("[FAIL] RAPID_SETTLE %s: %s\n", journey, message);
    }
}

bool Set(runtime::VirtualLitchSession& board, uint16_t id,
         const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return board.MutateSettingJson(id, key, value, outcome) == VIRTUAL_LITCH_OK &&
           (outcome.status == modules::ControlMutationStatus::Applied ||
            outcome.status == modules::ControlMutationStatus::Noop);
}

bool Observe(runtime::VirtualLitchSession& board, uint64_t logical) {
    const bool rotate = modules::ComputeSquareRotate(
        board.BoardRotate(), board.FirstPlayerIsP2());
    uint64_t physical = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((logical & (UINT64_C(1) << square)) != 0ULL) {
            physical |= UINT64_C(1) << modules::MapSquareToPhysical(square, rotate);
        }
    }
    return board.ObserveHallMask(physical) == VIRTUAL_LITCH_OK;
}

void Log(const runtime::VirtualLitchSession& board, const char* journey,
         const char* input, const char* next) {
    const auto& state = board.State();
    const auto* clock = board.GameClock();
    const uint8_t target = state.selected_square == 7U ? 15U :
        state.selected_square == 11U ? 27U :
        state.selected_square == 62U ? 45U : state.selected_square == 6U ? 21U : 36U;
    const auto cue = board.SemanticFrame().board[target][0];
    std::printf("[RAPID_SETTLE_JOURNEY] %s t=%lu input=%s occupancy=%016llx "
                "expected=%016llx phase=%s selected=%u side=%u history=%u "
                "illegal=%016llx correction=%016llx clock_side=%u "
                "running=%u paused=%u white_ms=%lu black_ms=%lu "
                "target=%u target_rgb=%u/%u/%u text=%.*s next=%s\n", journey,
                static_cast<unsigned long>(state.now_ms), input,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                runtime::ChessInteractionPhaseToString(
                    static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                state.selected_square, state.side_to_move_black,
                board.UndoHistoryMoveCount(),
                static_cast<unsigned long long>(state.human_illegal_mask),
                static_cast<unsigned long long>(state.correction_mask),
                clock ? static_cast<unsigned>(clock->GetActiveSide()) : 99U,
                clock && clock->IsRunning(), clock && clock->IsPaused(),
                clock ? static_cast<unsigned long>(clock->GetRemainingMs(runtime::ClockSide::White)) : 0UL,
                clock ? static_cast<unsigned long>(clock->GetRemainingMs(runtime::ClockSide::Black)) : 0UL,
                target, cue.r, cue.g, cue.b,
                static_cast<int>(board.DisplayText().size()), board.DisplayText().data(), next);
}

void RapidAlternatingPickup(const char* policy, uint32_t duration) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    const bool ready = board.Reset() == VIRTUAL_LITCH_OK &&
        Set(board, 720U, "MOVE_CONFIRM", policy) &&
        Set(board, 721U, "TIMER", "2") && Observe(board, OPENING) &&
        board.Advance(1000U) == VIRTUAL_LITCH_OK;
    Check(ready, policy, "opening and 3+2 clock are armed through production settings");
    if (!ready) return;
    constexpr uint8_t sources[] = {52U, 11U, 62U, 6U};
    constexpr uint8_t targets[] = {36U, 27U, 45U, 21U};
    uint64_t occupancy = OPENING;
    for (uint8_t move = 0U; move < 4U; ++move) {
        const uint64_t committed = occupancy;
        const auto previousSide = move % 2U == 0U ? runtime::ClockSide::Black : runtime::ClockSide::White;
        const auto nextSide = move % 2U == 0U ? runtime::ClockSide::White : runtime::ClockSide::Black;
        const auto* clock = board.GameClock();
        const uint32_t previousMs = clock->GetRemainingMs(previousSide);
        const uint32_t nextMs = clock->GetRemainingMs(nextSide);
        occupancy &= ~(UINT64_C(1) << sources[move]);
        Check(Observe(board, occupancy), policy, "next player's raw Hall pickup is admitted");
        Log(board, policy, "next-player-pickup", "place-selected-piece");
        Check(board.State().expected_occupied_mask == committed &&
                  board.UndoHistoryMoveCount() == move &&
                  board.State().side_to_move_black == move % 2U &&
                  board.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
                  board.State().selected_square == sources[move] &&
                  (board.State().valid_destination_mask & (UINT64_C(1) << targets[move])) != 0ULL &&
                  board.State().human_illegal_mask == 0ULL &&
                  board.State().correction_mask == 0ULL,
              policy, "each alternating pickup commits the prior move once and immediately owns the legal destinations");
        Check(clock->IsRunning() && !clock->IsPaused() && clock->GetActiveSide() == nextSide,
              policy, "clock starts/switches to the player who just picked up");
        if (move != 0U) {
            Check(clock->GetRemainingMs(previousSide) == previousMs + 2000U &&
                      clock->GetRemainingMs(nextSide) == nextMs,
                  policy, "handoff awards exactly one increment without charging the next side early");
        }
        const uint32_t heldMs = clock->GetRemainingMs(nextSide);
        Check(board.Advance(20U) == VIRTUAL_LITCH_OK, policy, "held piece advances physical time");
        Check(clock->GetRemainingMs(nextSide) + 20U == heldMs,
              policy, "only the new active side pays time while its piece is held");
        occupancy |= UINT64_C(1) << targets[move];
        Check(Observe(board, occupancy) &&
                  board.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
                  board.UndoHistoryMoveCount() == move,
              policy, "placement starts the new side's own settle without double commit");
        Log(board, policy, "place+20ms", move < 3U ? "opponent-pickup-before-expiry" : "wait-final-settle");
        Check(board.Advance(20U) == VIRTUAL_LITCH_OK, policy, "handoff stays inside either timed policy");
    }
    Check(board.Advance(duration) == VIRTUAL_LITCH_OK &&
              board.UndoHistoryMoveCount() == 4U &&
              board.State().expected_occupied_mask == occupancy &&
              board.State().side_to_move_black == 0U &&
              board.GameClock()->GetActiveSide() == runtime::ClockSide::White,
          policy, "final settle expires once and restores White's turn and clock");
    Log(board, policy, "final-expiry", "White-next-move");
}

void CoalescedAlternatingPickup(const char* policy) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    Check(board.Reset() == VIRTUAL_LITCH_OK &&
              Set(board, 722U, "MOVE_CONFIRM", policy) &&
              Set(board, 723U, "TIMER", "2") && Observe(board, OPENING) &&
              board.Advance(1000U) == VIRTUAL_LITCH_OK,
          policy, "coalesced journey is armed through the same production controls");
    constexpr uint8_t sources[] = {52U, 11U, 62U, 6U};
    constexpr uint8_t targets[] = {36U, 27U, 45U, 21U};
    uint64_t occupancy = OPENING & ~(UINT64_C(1) << sources[0]);
    Check(Observe(board, occupancy), policy, "first pickup starts White's clock");
    for (uint8_t move = 0U; move < 3U; ++move) {
        Check(board.Advance(20U) == VIRTUAL_LITCH_OK, policy, "physical input interval advances");
        occupancy |= UINT64_C(1) << targets[move];
        occupancy &= ~(UINT64_C(1) << sources[move + 1U]);
        Check(Observe(board, occupancy), policy, "one Hall sample contains placement and opponent pickup");
        Log(board, policy, "coalesced-place-and-opponent-pickup", "place-next-selected-piece");
        const auto nextSide = move % 2U == 0U ? runtime::ClockSide::Black : runtime::ClockSide::White;
        Check(board.UndoHistoryMoveCount() == move + 1U &&
                  board.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
                  board.State().selected_square == sources[move + 1U] &&
                  (board.State().valid_destination_mask & (UINT64_C(1) << targets[move + 1U])) != 0ULL &&
                  board.State().human_illegal_mask == 0ULL &&
                  board.State().correction_mask == 0ULL &&
                  board.GameClock()->GetActiveSide() == nextSide &&
                  !board.GameClock()->IsPaused(),
              policy, "every coalesced handoff advances one move, legal selection and running clock");
    }
}

void CircleRequiresConfirmation() {
    constexpr const char* journey = "circle-requires-confirmation-before-next-turn";
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    Check(board.Reset() == VIRTUAL_LITCH_OK &&
              Set(board, 724U, "MOVE_CONFIRM", "\"press_circle_to_confirm\"") &&
              Set(board, 725U, "TIMER", "2") && Observe(board, OPENING) &&
              board.Advance(1000U) == VIRTUAL_LITCH_OK,
          journey, "Circle mode and clock are armed");
    uint64_t occupancy = OPENING & ~(UINT64_C(1) << 52U);
    Check(Observe(board, occupancy), journey, "White picks up e2");
    occupancy |= UINT64_C(1) << 36U;
    Check(Observe(board, occupancy) && board.Advance(20U) == VIRTUAL_LITCH_OK,
          journey, "White places e4 without Circle");
    occupancy &= ~(UINT64_C(1) << 11U);
    Check(Observe(board, occupancy) && board.UndoHistoryMoveCount() == 0U &&
              board.State().side_to_move_black == 0U &&
              board.GameClock()->GetActiveSide() == runtime::ClockSide::White &&
              board.GameClock()->IsPaused(),
          journey, "opponent pickup cannot confirm the move or transfer turn/clock without Circle");
    Log(board, journey, "opponent-pickup-before-Circle", "restore-opponent-piece-then-Circle");
    occupancy |= UINT64_C(1) << 11U;
    Check(Observe(board, occupancy) && board.Advance(5000U) == VIRTUAL_LITCH_OK &&
              board.UndoHistoryMoveCount() == 0U &&
              !board.GameClock()->IsPaused(),
          journey, "restoration recovers White's clock while confirmation still waits");
    Check(board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.Advance(1000U) == VIRTUAL_LITCH_OK &&
              board.UndoHistoryMoveCount() == 1U &&
              board.GameClock()->GetActiveSide() == runtime::ClockSide::Black &&
              !board.GameClock()->IsPaused(),
          journey, "Circle and its confirmation interval finalize once and hand off to Black");
    Log(board, journey, "Circle-confirmation-expired", "Black-pickup");
}
void AutomaticPromotionPickup(const uint8_t hardware, const bool atomic,
                              const bool requireO, const bool pickupBeforeO) {
    const char* journey = pickupBeforeO ? "promotion-pickup-before-Circle" :
        requireO ? "promotion-pickup-after-Circle" :
        atomic ? "promotion-atomic-next-pickup" : "promotion-timed-next-pickup";
    std::printf("[AUTOMATIC_PROMOTION_PICKUP] journey=%s hardware=%u atomic=%u requireO=%u beforeO=%u\n",
                journey, hardware, atomic, requireO, pickupBeforeO);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardware;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    constexpr uint64_t BEFORE = (UINT64_C(1) << 7U) | (UINT64_C(1) << 8U) |
        (UINT64_C(1) << 50U) | (UINT64_C(1) << 63U);
    uint64_t occupancy = BEFORE;
    const auto tapO = [&]() {
        return board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                   runtime::VirtualLitchPhysicalButton::O, true) == VIRTUAL_LITCH_OK &&
            board.Advance(20U) == VIRTUAL_LITCH_OK &&
            board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                   runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
            board.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    Check(board.Reset() == VIRTUAL_LITCH_OK && Observe(board, OPENING) &&
              Set(board, 750U, "TIMER", "2") &&
              (!requireO || Set(board, 751U, "MOVE_CONFIRM", "\"press_circle_to_confirm\"")) &&
              board.LoadExternalFen("7k/P7/8/8/8/8/2P5/7K w - - 0 1") &&
              Observe(board, occupancy) && board.Advance(200U) == VIRTUAL_LITCH_OK,
          journey, "real settings, FEN seed and Hall scan initialize automatic Queen");
    occupancy &= ~(UINT64_C(1) << 8U);
    Check(Observe(board, occupancy), journey, "raw a7 pawn pickup");
    occupancy |= UINT64_C(1);
    if (!atomic) {
        Check(Observe(board, occupancy) && board.Advance(100U) == VIRTUAL_LITCH_OK,
              journey, "a8 placement enters ordinary pre-commit settling");
    }
    if (requireO && !pickupBeforeO) {
        Check(tapO(), journey, "normal Circle is accepted before the next pickup");
    }
    occupancy &= ~(UINT64_C(1) << 7U);
    Check(Observe(board, occupancy), journey, "raw next-side king pickup accompanies or interrupts promotion");
    if (pickupBeforeO) {
        Check(board.Advance(2200U) == VIRTUAL_LITCH_OK &&
                  board.UndoHistoryMoveCount() == 0U && !board.PromotionActive() &&
                  board.State().side_to_move_black == 0U,
              journey, "pickup before Circle cannot accept automatic promotion");
        Log(board, journey, "next-pickup-before-Circle", "restore-h8-then-confirm");
        occupancy |= UINT64_C(1) << 7U;
        Check(Observe(board, occupancy) && tapO() &&
                  board.Advance(1100U) == VIRTUAL_LITCH_OK,
              journey, "restoration and fresh Circle recover through the ordinary move gate");
        occupancy &= ~(UINT64_C(1) << 7U);
        Check(Observe(board, occupancy), journey, "next pickup after confirmed recovery");
    }
    Log(board, journey, "promotion-and-next-pickup", "place-black-king-h7-or-return-h8");
    Check(board.UndoHistoryMoveCount() == 1U && !board.PromotionActive() &&
              std::strstr(board.HomeStateLine().CStr(), "Q6k/") != nullptr &&
              board.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
              board.State().selected_square == 7U &&
              (board.State().valid_destination_mask & (UINT64_C(1) << 15U)) != 0ULL &&
              board.State().human_illegal_mask == 0ULL && board.State().correction_mask == 0ULL &&
              board.GameClock()->GetActiveSide() == runtime::ClockSide::Black &&
              !board.GameClock()->IsPaused(),
          journey, "promotion acknowledges once and replays the next pickup with its legal mask and clock owner");
    occupancy |= UINT64_C(1) << 7U;
    Check(Observe(board, occupancy) && board.Advance(200U) == VIRTUAL_LITCH_OK &&
              board.UndoHistoryMoveCount() == 1U &&
              board.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle),
          journey, "returning the next piece recovers without a stranded claimed action");
    occupancy &= ~(UINT64_C(1) << 7U);
    Check(Observe(board, occupancy), journey, "a fresh king pickup remains actionable");
    occupancy |= UINT64_C(1) << 15U;
    Check(Observe(board, occupancy) && (!requireO || tapO()) &&
              board.Advance(2200U) == VIRTUAL_LITCH_OK &&
              board.UndoHistoryMoveCount() == 2U &&
              std::strstr(board.HomeStateLine().CStr(), "Q7/7k/") != nullptr,
          journey, "the next legal move commits once after promotion and recovery");
    Log(board, journey, "reply-acknowledged", "continue-White-turn");
}
} // namespace

int RunRapidSettleHandoffVirtualLitchJourneys() {
    g_failures = 0;
    RapidAlternatingPickup("\"settle_2s\"", 2000U);
    RapidAlternatingPickup("\"settle_5s\"", 5000U);
    CoalescedAlternatingPickup("\"settle_2s\"");
    CoalescedAlternatingPickup("\"settle_5s\"");
    CircleRequiresConfirmation();
    for (const auto hardware : {VIRTUAL_LITCH_HARDWARE_V3, VIRTUAL_LITCH_HARDWARE_V3_3}) {
        AutomaticPromotionPickup(hardware, true, false, false);
        AutomaticPromotionPickup(hardware, false, false, false);
        AutomaticPromotionPickup(hardware, false, true, false);
        AutomaticPromotionPickup(hardware, true, true, true);
    }
    return g_failures;
}
