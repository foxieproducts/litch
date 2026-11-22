#include <array>
#include <cstdio>
#include <string_view>

#include <litch_max/chess_notation.hpp>

#include "bounded_hint_engine.hpp"
#include "modules/draw_rules.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/game_history_protocol.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "runtime/game_clock.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

#ifndef ARDUINO
void CheckDeadPositionTakeback(runtime::VirtualLitchSession& board, const modules::EngineDerivedState& derived) {
    // After Bxe7, bare Black king against king and bishop is a dead position.
    // Kxe7 would be available from this placement if the game were still live.
    constexpr uint64_t D6 = 1ULL << 19U, E7 = 1ULL << 12U;
    constexpr uint64_t ORIGINAL = (1ULL << 4U) | E7 | D6 | (1ULL << 56U);
    modules::ControlMutationOutcome grace;
    Check(board.MutateSettingJson(3U, "TAKEBACK_GRACE_SECONDS", "3", grace) == VIRTUAL_LITCH_OK &&
              grace.status == modules::ControlMutationStatus::Applied &&
              board.LoadExternalFen("4k3/4n3/3B4/8/8/8/8/K7 w - - 0 1") &&
              board.ObserveHallMask(ORIGINAL) == VIRTUAL_LITCH_OK && board.Advance(1000U) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(ORIGINAL & ~D6) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(ORIGINAL & ~D6 & ~E7) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(ORIGINAL & ~D6) == VIRTUAL_LITCH_OK && board.Advance(2000U) == VIRTUAL_LITCH_OK &&
              board.UndoHistoryMoveCount() == 1U && derived.drawReason == modules::DrawReason::InsufficientMaterial,
          "dead position: Bxe7 captures the knight and immediately draws by insufficient material");
    Check(board.ObserveHallMask(ORIGINAL & ~D6 & ~E7) == VIRTUAL_LITCH_OK && board.Advance(1U) == VIRTUAL_LITCH_OK &&
              board.IsUndoActionPending() && board.UndoHistoryMoveCount() == 0U && !derived.inDraw,
          "dead position: lifting the bishop during grace immediately starts the inverse capture");
    Check(board.ObserveHallMask(ORIGINAL & ~E7) == VIRTUAL_LITCH_OK && board.ObserveHallMask(ORIGINAL) == VIRTUAL_LITCH_OK &&
              board.Advance(1U) == VIRTUAL_LITCH_OK && !board.IsUndoActionPending() &&
              board.State().physical_occupied_mask == ORIGINAL && board.State().expected_occupied_mask == ORIGINAL,
          "dead position: returning the bishop to d6 and black knight to e7 restores play");
    std::printf("[DEAD_POSITION_TAKEBACK] Bxe7 lift=e7 return=bishop-d6/knight-e7 "
                "ply=%u draw=%u physical=%016llx expected=%016llx next=White-to-move\n",
                board.UndoHistoryMoveCount(), derived.inDraw,
                static_cast<unsigned long long>(board.State().physical_occupied_mask),
                static_cast<unsigned long long>(board.State().expected_occupied_mask));
}

void CheckTwoKnightTerminalJourney() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    // The two knight circuits repeat their joint position only every 60 plies.
    // Starting with Black, the 99th quiet ply leaves White Nc4 and Black Na7.
    constexpr const char* START = "k7/n1K5/8/8/8/8/3N4/8 b - - 0 1";
    constexpr std::array<const char*, 6> WHITE = {"d2", "c4", "b2", "d1", "f2", "e4"};
    constexpr std::array<const char*, 10> BLACK = {"a7", "c6", "a5", "b7", "c5",
                                                   "d7", "e5", "g6", "e7", "c8"};
    modules::ControlMutationOutcome timer;
    Check(board.Reset() == VIRTUAL_LITCH_OK &&
              board.MutateSettingJson(1U, "TIMER", "3", timer) == VIRTUAL_LITCH_OK &&
              timer.status == modules::ControlMutationStatus::Applied && board.LoadExternalFen(START) &&
              board.ObserveHallMask(board.State().expected_occupied_mask) == VIRTUAL_LITCH_OK &&
              board.Advance(1000U) == VIRTUAL_LITCH_OK,
          "knight mate journey: both knights are live and the matched position is ready for timed play");

    const modules::EngineDerivedState* facts = nullptr;
    board.RunControlIngress([](void* context, modules::SettingsStore&,
        modules::ControlMutationService&, modules::GamePersistenceCoordinator& coordinator) {
        *static_cast<const modules::EngineDerivedState**>(context) = &coordinator.GetEngineDerivedState();
    }, &facts);
    const auto& derived = *facts;
    const auto play = [&](const char* source, const char* destination) {
        uint8_t from = 0U, to = 0U;
        if (!litch_max::TryParseUciSquare(source, from) || !litch_max::TryParseUciSquare(destination, to)) {
            return false;
        }
        const uint64_t lifted = board.State().physical_occupied_mask & ~(1ULL << from);
        const uint64_t placed = lifted | (1ULL << to);
        const auto priorPlies = board.UndoHistoryMoveCount();
        return board.ObserveHallMask(lifted) == VIRTUAL_LITCH_OK &&
               (board.State().valid_destination_mask & (1ULL << to)) != 0ULL &&
               board.ObserveHallMask(placed) == VIRTUAL_LITCH_OK && board.Advance(2000U) == VIRTUAL_LITCH_OK &&
               board.UndoHistoryMoveCount() == priorPlies + 1U &&
               board.State().expected_occupied_mask == placed && board.State().physical_occupied_mask == placed;
    };
    const auto advanceKnight = [&](const auto& path, size_t turn) {
        return play(path[turn % path.size()], path[(turn + 1U) % path.size()]);
    };
    for (size_t turn = 0; turn < 50U; ++turn) {
        if (!advanceKnight(BLACK, turn) || (turn < 49U && !advanceKnight(WHITE, turn))) {
            std::printf("[KNIGHT_HISTORY] lost move at ply %u\n", board.UndoHistoryMoveCount() + 1U);
            Check(false, "knight mate journey: each quiet Hall move is legal, settles and commits exactly once");
            return;
        }
    }
    const uint64_t beforeMate = board.State().expected_occupied_mask;
    Check(derived.halfmoveClockPlies == 99U && !derived.inDraw && !derived.inCheckmate &&
              board.GameClock()->IsRunning() && board.UndoHistoryMoveCount() == 99U,
          "knight mate journey: the running game reaches halfmove 99 without an earlier draw");

    Check(play("c4", "b6"), "knight mate journey: raw Nc4-b6 settles and commits once");
    const auto litStatusMask = [&]() {
        uint32_t lit = 0U;
        for (uint8_t i = 0U; i < 32U; ++i) {
            const auto& color = board.SemanticFrame().status[i];
            if (color.r || color.g || color.b) lit |= 1U << i;
        }
        return lit;
    };
    constexpr uint32_t WHITE_HOME_EDGE = 0xffU << 16U;
    modules::HomeStateNotificationPublisher reconnect;
    modules::HomeStateNotificationLine home;
    Check(derived.halfmoveClockPlies == 100U && derived.inCheckmate && !derived.inDraw &&
              derived.drawReason == modules::DrawReason::None && !derived.drawStartValid &&
              !board.GameClock()->IsRunning() && !board.GameClock()->HasTimedOut() &&
              litStatusMask() == WHITE_HOME_EDGE && board.DisplayText().empty() &&
              board.TryPrepareHomePublication(reconnect, true, home) &&
              home.View().find("k7/n1K5/1N6/8/8/8/8/8 b - - ") != std::string_view::npos,
          "knight mate journey: halfmove 100 is White checkmate with a stopped clock, winner edge and reconnect FEN");
    std::printf("[TWO_KNIGHT_MATE_JOURNEY] ply=%u halfmove=%u mate=%u draw=%u reason=%u "
                "status=%08x clock_running=%u next=Undo-or-New-Game\n",
                board.UndoHistoryMoveCount(), derived.halfmoveClockPlies, derived.inCheckmate, derived.inDraw,
                static_cast<unsigned>(derived.drawReason), litStatusMask(), board.GameClock()->IsRunning());

    const auto tapUndo = [&]() {
        const auto x = runtime::VirtualLitchPhysicalButton::X;
        return board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, x, true) == VIRTUAL_LITCH_OK &&
               board.Advance(20U) == VIRTUAL_LITCH_OK &&
               board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, x, false) == VIRTUAL_LITCH_OK &&
               board.Advance(20U) == VIRTUAL_LITCH_OK && board.Advance(1U) == VIRTUAL_LITCH_OK;
    };
    Check(tapUndo() && board.IsUndoActionPending() && board.UndoHistoryMoveCount() == 99U &&
              !derived.inCheckmate && board.GameClock()->IsPaused(),
          "knight mate journey: tapping X rewinds the mate and asks for the physical return");
    const uint64_t withoutB6 = board.State().physical_occupied_mask & ~(1ULL << 17U);
    Check(board.ObserveHallMask(withoutB6) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(beforeMate) == VIRTUAL_LITCH_OK && board.Advance(1U) == VIRTUAL_LITCH_OK &&
              !board.IsUndoActionPending() && !derived.inDraw &&
              derived.halfmoveClockPlies == 99U && board.GameClock()->IsRunning() &&
              !board.GameClock()->IsPaused() && board.GameClock()->GetActiveSide() == runtime::ClockSide::White,
          "knight mate journey: physical Nb6-c4 restores the live game and White's clock at halfmove 99");
    const auto whiteTime = board.GameClock()->GetRemainingMs(runtime::ClockSide::White);
    const auto blackTime = board.GameClock()->GetRemainingMs(runtime::ClockSide::Black);
    Check(board.Advance(250U) == VIRTUAL_LITCH_OK &&
              board.GameClock()->GetRemainingMs(runtime::ClockSide::White) == whiteTime - 250U &&
              board.GameClock()->GetRemainingMs(runtime::ClockSide::Black) == blackTime,
          "knight mate journey: only White's time advances after the restored position is matched");
    std::printf("[TWO_KNIGHT_UNDO] ply=%u halfmove=%u clock_running=%u paused=%u next=White-to-move\n",
                board.UndoHistoryMoveCount(), derived.halfmoveClockPlies,
                board.GameClock()->IsRunning(), board.GameClock()->IsPaused());
    Check(play("c4", "e5"), "knight mate journey: White can choose a different quiet move after Undo");
    Check(derived.halfmoveClockPlies == 100U && !derived.inCheckmate && derived.inDraw &&
              derived.drawReason == modules::DrawReason::FiftyMoveRule && derived.drawStartValid &&
              litStatusMask() == UINT32_MAX &&
              !board.GameClock()->IsRunning() && !board.GameClock()->HasTimedOut(),
          "knight mate journey: nonmating Ne5 at the same boundary ends in the fifty-move draw");
    std::printf("[TWO_KNIGHT_ALTERNATE] ply=%u halfmove=%u mate=%u draw=%u reason=%u "
                "status=%08x clock_running=%u next=Undo-or-New-Game\n",
                board.UndoHistoryMoveCount(), derived.halfmoveClockPlies, derived.inCheckmate, derived.inDraw,
                static_cast<unsigned>(derived.drawReason), litStatusMask(), board.GameClock()->IsRunning());

    const uint64_t drawn = board.State().expected_occupied_mask;
    const uint64_t liftedA7 = drawn & ~(1ULL << 8U);
    const uint64_t knightAtC8 = liftedA7 | (1ULL << 2U);
    Check(board.ObserveHallMask(liftedA7) == VIRTUAL_LITCH_OK && board.State().valid_destination_mask == 0ULL &&
              board.ObserveHallMask(knightAtC8) == VIRTUAL_LITCH_OK && board.Advance(2000U) == VIRTUAL_LITCH_OK &&
              board.UndoHistoryMoveCount() == 100U && board.State().expected_occupied_mask == drawn &&
              board.ObserveHallMask(drawn) == VIRTUAL_LITCH_OK,
          "knight mate journey: the draw rejects Na7-c8 and accepts restoration of the drawn board");
    Check(board.ObserveHallMask(knightAtC8) == VIRTUAL_LITCH_OK && board.Advance(2000U) == VIRTUAL_LITCH_OK &&
              board.UndoHistoryMoveCount() == 100U && board.State().expected_occupied_mask == drawn &&
              board.ObserveHallMask(drawn) == VIRTUAL_LITCH_OK && board.Advance(1U) == VIRTUAL_LITCH_OK,
          "knight mate journey: a single combined Hall observation cannot bypass the terminal draw");
    Check(tapUndo() && board.IsUndoActionPending() && board.UndoHistoryMoveCount() == 99U &&
              !derived.inDraw && tapUndo() && !board.IsUndoActionPending() &&
              board.UndoHistoryMoveCount() == 100U && derived.inDraw &&
              derived.drawReason == modules::DrawReason::FiftyMoveRule &&
              !board.GameClock()->IsRunning() && !board.GameClock()->HasTimedOut(),
          "knight mate journey: X can undo the draw and Cancel Undo restores the stopped terminal result");

    virtual_litch_action_request newGame{};
    virtual_litch_default_action_request(&newGame);
    newGame.request_id = 2U;
    newGame.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    newGame.owner = VIRTUAL_LITCH_OWNER_P1;
    modules::ControlMutationOutcome outcome;
    Check(board.DispatchControlAction(newGame, outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted && board.Advance(1U) == VIRTUAL_LITCH_OK &&
              board.TryTakeControlCompletion(outcome) && outcome.requestId == newGame.request_id &&
              outcome.status == modules::ControlMutationStatus::Applied && board.UndoHistoryMoveCount() == 0U &&
              !derived.inDraw && !derived.inCheckmate,
          "knight mate journey: acknowledged New Game clears the terminal result and history");
    CheckDeadPositionTakeback(board, derived);
}
#endif

} // namespace

int main() {
    std::puts("[DRAW_CONDITIONS] start");

#ifndef ARDUINO
    // 1) Stalemate is treated as a forced derived.
    {
        modules::LitchMaxEngine engine;
        engine.Reset();

        static constexpr const char* BLACK_STALEMATE_FEN = "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1";
        Check(engine.SetFen(BLACK_STALEMATE_FEN), "SetFen black stalemate");

        modules::EngineDerivedState derived;
        derived.Reset();
        derived.RefreshFromEngine(engine);

        Check(derived.inStalemate, "derived: stalemate true");
        Check(derived.inDraw, "derived: inDraw true on stalemate");
        Check(derived.drawReason == modules::DrawReason::Stalemate, "derived: drawReason stalemate");
    }

    // 2) Threefold repetition ignores halfmove/fullmove counters.
    {
        static constexpr const char* FEN_A1 = "4k3/8/8/8/8/8/8/4K3 w - - 0 1";
        static constexpr const char* FEN_A2 = "4k3/8/8/8/8/8/8/4K3 w - - 99 123";
        static constexpr const char* FEN_A3 = "4k3/8/8/8/8/8/8/4K3 w - - 7 9";

        const char* history[] = {FEN_A1, FEN_A2, FEN_A3};
        const uint8_t count = modules::CountThreefoldRepetition(FEN_A1, history, 3);
        Check(count == 3U, "threefold: count==3 for same position key");

        static constexpr const char* FEN_OTHER_SIDE = "4k3/8/8/8/8/8/8/4K3 b - - 0 1";
        const char* history2[] = {FEN_A1, FEN_A2, FEN_OTHER_SIDE};
        const uint8_t count2 = modules::CountThreefoldRepetition(FEN_A1, history2, 3);
        Check(count2 == 2U, "threefold: side-to-move participates in key");

        static constexpr const char* FEN_EP_A3 = "4k3/8/8/8/P7/8/8/4K3 b - a3 0 1";
        static constexpr const char* FEN_NO_EP = "4k3/8/8/8/P7/8/8/4K3 b - - 0 1";
        const char* history3[] = {FEN_EP_A3};
        const uint8_t count3 = modules::CountThreefoldRepetition(FEN_NO_EP, history3, 1);
        Check(count3 == 1U, "threefold: unavailable en-passant does not split the same legal position");

        const char* legalEp[] = {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1"};
        Check(modules::CountThreefoldRepetition(
                  "4k3/8/8/3pP3/8/8/8/4K3 w - - 0 1", legalEp, 1U) == 0U,
              "threefold: a legal en-passant capture distinguishes positions");
        const char* pinnedEp[] = {"4r1k1/8/8/3pP3/8/8/8/4K3 w - d6 0 1"};
        Check(modules::CountThreefoldRepetition(
                  "4r1k1/8/8/3pP3/8/8/8/4K3 w - - 0 1", pinnedEp, 1U) == 1U,
              "threefold: pinned-only en-passant does not split the same legal position");
        const char* castle[] = {"4k3/8/8/8/8/8/8/4K2R w K - 0 1"};
        Check(modules::CountThreefoldRepetition(
                  "4k3/8/8/8/8/8/8/4K2R w - - 0 1", castle, 1U) == 0U,
              "threefold: castling rights remain part of legal identity");

        fxe::FixedString<96> rawEp, rawNoEp;
        Check(modules::TryBuildFenPositionKey4(FEN_EP_A3, rawEp) &&
                  modules::TryBuildFenPositionKey4(FEN_NO_EP, rawNoEp) &&
                  !rawEp.Equals(rawNoEp.View()),
              "raw FEN/cache key retains unavailable en-passant metadata");
        modules::game_history::Position packedEp, packedNoEp;
        Check(modules::game_history::EncodePosition(FEN_EP_A3,
                  modules::game_history::Variant::Classic, modules::game_history::INVALID_SQUARE, packedEp) &&
                  modules::game_history::EncodePosition(FEN_NO_EP,
                  modules::game_history::Variant::Classic, modules::game_history::INVALID_SQUARE, packedNoEp) &&
                  !modules::game_history::SamePositionAuthority(packedEp, packedNoEp),
              "raw packed protocol authority retains en-passant metadata");
    }

    // 3) 50-move rule halfmove clock (plies) advances only on non-pawn, non-capture moves.
    {
        static constexpr const char* KNIGHT_ONLY_FEN = "8/8/8/8/8/8/8/6N1 w - - 0 1";
        // g1 -> e2 (both empty except the knight at g1).
        static constexpr uint8_t FROM_G1 = 62U;
        static constexpr uint8_t TO_E2 = 52U;

        uint16_t halfmove = 0;
        for (uint16_t i = 0; i < 100U; ++i) {
            halfmove = modules::NextHalfmoveClockPlies(KNIGHT_ONLY_FEN, halfmove, FROM_G1, TO_E2, false, 0);
        }
        Check(halfmove == 100U, "50-move: halfmove clock reaches 100 plies");
        Check(modules::IsFiftyMoveDraw(halfmove), "50-move: draw triggers at 100 plies");

        static constexpr const char* PAWN_FEN = "8/8/8/8/8/8/P7/8 w - - 0 1";
        static constexpr uint8_t FROM_A2 = 48U;
        static constexpr uint8_t TO_A3 = 40U;
        const uint16_t afterPawn = modules::NextHalfmoveClockPlies(PAWN_FEN, 99U, FROM_A2, TO_A3, false, 0);
        Check(afterPawn == 0U, "50-move: pawn move resets clock");

        static constexpr const char* CAPTURE_FEN = "8/8/8/8/8/8/4p3/6N1 w - - 0 1";
        const uint16_t afterCapture = modules::NextHalfmoveClockPlies(CAPTURE_FEN, 99U, FROM_G1, TO_E2, false, 0);
        Check(afterCapture == 0U, "50-move: capture resets clock");

        const uint16_t afterCaptureSquare =
            modules::NextHalfmoveClockPlies(KNIGHT_ONLY_FEN, 99U, FROM_G1, TO_E2, true, 44U);
        Check(afterCaptureSquare == 0U, "50-move: captureSquareValid resets clock");
    }

    // 4) Insufficient material draws trigger immediately.
    {
        Check(modules::IsInsufficientMaterialDraw("7k/8/8/8/8/8/8/4K3 w - - 0 1"),
              "insufficient: bare kings");
        Check(modules::IsInsufficientMaterialDraw("7k/8/8/8/8/8/8/2B1K3 w - - 0 1"),
              "insufficient: bishop vs king");
        Check(modules::IsInsufficientMaterialDraw("7k/8/8/8/8/8/8/4K1N1 w - - 0 1"),
              "insufficient: knight vs king");
        Check(modules::IsInsufficientMaterialDraw("5b1k/8/8/8/8/8/8/2B1K3 w - - 0 1"),
              "insufficient: same-color bishops only");
        Check(!modules::IsInsufficientMaterialDraw("k7/n1K5/8/8/2N5/8/8/8 w - - 0 1"),
              "insufficient: knight vs knight permits Nc4-b6 mate");

        Check(!modules::IsInsufficientMaterialDraw("2b4k/8/8/8/8/8/8/2B1K3 w - - 0 1"),
              "insufficient: opposite-color bishops not auto-draw");
        Check(!modules::IsInsufficientMaterialDraw("7k/8/8/8/8/8/8/2B1K1N1 w - - 0 1"),
              "insufficient: bishop and knight can still mate");
    }

    CheckTwoKnightTerminalJourney();
#else
    std::puts("[DRAW_CONDITIONS] skipped (ARDUINO)");
#endif

    if (g_failures == 0) {
        std::puts("[DRAW_CONDITIONS] ok");
        return 0;
    }

    std::printf("[DRAW_CONDITIONS] failures=%d\n", g_failures);
    return 1;
}
