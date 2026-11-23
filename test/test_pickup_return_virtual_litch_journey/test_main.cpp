#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

#include <ArduinoJson.h>

#include "virtual_litch_session.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/orientation.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/game_clock.hpp"

namespace {

constexpr uint64_t INITIAL_OCCUPANCY = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t A2 = 48U;
constexpr uint8_t A7 = 8U;
constexpr uint8_t A3 = 40U;
constexpr uint8_t E4 = 36U;
constexpr uint8_t E5 = 28U;
constexpr uint8_t F6 = 21U;
constexpr uint8_t D6 = 19U;
constexpr uint8_t C5 = 26U;
constexpr uint8_t D5 = 27U;
constexpr uint8_t D1 = 59U;
constexpr uint8_t H3 = 47U;
// Keep the original a2/e4/d6 trigger, with the non-moving king on g8
// so the initial position is legal for the production engine.
constexpr const char* POSITION =
    "6k1/8/3p4/8/4Q3/8/P7/4K3 w - - 0 1";
constexpr const char* CAPTURE_POSITION =
    "6k1/8/8/3p4/4Q3/8/P7/4K3 w - - 0 1";
constexpr uint64_t BASE = (UINT64_C(1) << 6U) | (UINT64_C(1) << D6) |
    (UINT64_C(1) << E4) | (UINT64_C(1) << A2) | (UINT64_C(1) << 60U);
int failures = 0;

void Check(bool condition, const char* run, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s: %s\n", run, message);
        ++failures;
    }
}

bool Observe(runtime::VirtualLitchSession& session, uint64_t logical) {
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
    return session.MutateSettingJson(100U, key, value, outcome) == VIRTUAL_LITCH_OK &&
           (outcome.status == modules::ControlMutationStatus::Applied ||
            outcome.status == modules::ControlMutationStatus::Noop);
}

std::string Fen(const runtime::VirtualLitchSession& session) {
    // The optional physical-move tuple must fit while capture removal is shown.
    StaticJsonDocument<2048> home;
    if (deserializeJson(home, session.HomeStateLine().CStr()) !=
        DeserializationError::Ok) {
        return {};
    }
    return home["fen"] | "";
}

bool Idle(const runtime::VirtualLitchSession& session) {
    const auto& state = session.State();
    return state.interaction_phase ==
               static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
           state.selected_square == runtime::CHESS_INTERACTION_INVALID_SQUARE &&
           state.required_action ==
               static_cast<uint8_t>(runtime::RequiredPhysicalAction::None) &&
           state.valid_destination_mask == 0ULL &&
           state.correction_mask == 0ULL && state.human_illegal_mask == 0ULL &&
           state.cpu_illegal_mask == 0ULL && state.pending_action_token == 0U;
}

uint16_t PhysicalCueIndex(
    const runtime::VirtualLitchSession& session,
    uint8_t square,
    hardware::SquareLedSlot slot = hardware::SquareLedSlot::Square0) {
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    const auto revision = session.PhysicalFrame().led_count == 300U
        ? hardware::BoardRevision::V3_3S3
        : session.PhysicalFrame().led_count == 288U
            ? hardware::BoardRevision::V3S3 : hardware::BoardRevision::V2S3;
    return hardware::MapSemanticSquareLedTargets(revision, square, slot, rotate)
        .indices[0U];
}

uint8_t DominantColorChannel(const virtual_litch_color color) {
    if (color.r > color.g && color.r > color.b) {
        return 0U;
    }
    if (color.g > color.r && color.g > color.b) {
        return 1U;
    }
    if (color.b > color.r && color.b > color.g) {
        return 2U;
    }
    return 3U;
}

bool CaptureDestinationUsesMoverColor(
    const runtime::VirtualLitchSession& session,
    const uint8_t whiteKing = 60U,
    const uint8_t blackKing = 6U) {
    const uint8_t pieceSlot =
        static_cast<uint8_t>(hardware::SquareLedSlot::Piece);
    const auto destination = session.SemanticFrame().board[D5][pieceSlot];
    const auto mover = session.SemanticFrame().board[whiteKing][pieceSlot];
    const auto opponent = session.SemanticFrame().board[blackKing][pieceSlot];
    const auto physicalDestination = session.PhysicalFrame().pixels[
        PhysicalCueIndex(session, D5, hardware::SquareLedSlot::Piece)];
    const auto physicalMover = session.PhysicalFrame().pixels[
        PhysicalCueIndex(session, whiteKing, hardware::SquareLedSlot::Piece)];
    const auto physicalOpponent = session.PhysicalFrame().pixels[
        PhysicalCueIndex(session, blackKing, hardware::SquareLedSlot::Piece)];
    const uint8_t destinationChannel = DominantColorChannel(destination);
    const uint8_t moverChannel = DominantColorChannel(mover);
    const uint8_t opponentChannel = DominantColorChannel(opponent);
    const uint8_t physicalDestinationChannel =
        DominantColorChannel(physicalDestination);
    const uint8_t physicalMoverChannel = DominantColorChannel(physicalMover);
    const uint8_t physicalOpponentChannel =
        DominantColorChannel(physicalOpponent);
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    const auto revision = session.PhysicalFrame().led_count == 300U
        ? hardware::BoardRevision::V3_3S3
        : session.PhysicalFrame().led_count == 288U
            ? hardware::BoardRevision::V3S3 : hardware::BoardRevision::V2S3;
    const auto pieces = hardware::MapSemanticSquareLedTargets(
        revision, D5, hardware::SquareLedSlot::Piece, rotate);
    bool allPieceEmittersMatch = true;
    for (uint8_t i = 0U; i < pieces.count; ++i) {
        allPieceEmittersMatch = allPieceEmittersMatch &&
            DominantColorChannel(session.PhysicalFrame().pixels[pieces.indices[i]]) ==
                physicalMoverChannel;
    }
    return allPieceEmittersMatch && destinationChannel < 3U && destinationChannel == moverChannel &&
           destinationChannel != opponentChannel &&
           physicalDestinationChannel < 3U &&
           physicalDestinationChannel == physicalMoverChannel &&
           physicalDestinationChannel != physicalOpponentChannel;
}

bool YellowReturnCue(const runtime::VirtualLitchSession& session, uint8_t square) {
    const auto semantic = session.SemanticFrame().board[square][0];
    const auto mapped = session.PhysicalFrame().pixels[PhysicalCueIndex(session, square)];
    const auto yellow = [](virtual_litch_color color) {
        return color.r != 0U && color.r == color.g && color.b == 0U && color.w == 0U;
    };
    return yellow(semantic) && yellow(mapped);
}

void Checkpoint(const runtime::VirtualLitchSession& session, const char* run,
                const char* stage, const char* nextAction) {
    const auto& state = session.State();
    const auto display = session.DisplayText();
    std::printf("[PICKUP_RETURN_JOURNEY] run=%s stage=%s t=%u occupancy=%016llx "
                "expected=%016llx phase=%u selected=%u required=%u valid=%016llx "
                "correction=%016llx action=%u history=%u display=\"%.*s\" "
                "next=%s home=%s\n",
                run, stage, state.now_ms,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                state.interaction_phase, state.selected_square, state.required_action,
                static_cast<unsigned long long>(state.valid_destination_mask),
                static_cast<unsigned long long>(state.correction_mask),
                state.pending_action_token, session.UndoHistoryMoveCount(),
                static_cast<int>(display.size()), display.empty() ? "" : display.data(),
                nextAction, session.HomeStateLine().CStr());
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    for (const uint8_t square : {A2, A3, E4, D6, C5, D5, D1, E5, F6, A7, H3}) {
        const uint8_t physical = modules::MapSquareToPhysical(square, rotate);
        const auto semantic = session.SemanticFrame().board[square][0];
        const uint16_t physicalLed = PhysicalCueIndex(session, square);
        const auto mapped = session.PhysicalFrame().pixels[physicalLed];
        const auto piece = session.SemanticFrame().board[square][1U];
        const auto physicalPiece = session.PhysicalFrame().pixels[
            PhysicalCueIndex(session, square, hardware::SquareLedSlot::Piece)];
        std::printf("[PICKUP_RETURN_LED] run=%s stage=%s square=%u physical=%u led=%u "
                    "semantic=%u,%u,%u,%u mapped=%u,%u,%u,%u "
                    "piece=%u,%u,%u,%u piece_mapped=%u,%u,%u,%u\n",
                    run, stage, square, physical, physicalLed,
                    semantic.r, semantic.g, semantic.b, semantic.w,
                    mapped.r, mapped.g, mapped.b, mapped.w,
                    piece.r, piece.g, piece.b, piece.w,
                    physicalPiece.r, physicalPiece.g, physicalPiece.b, physicalPiece.w);
    }
}

bool Prepare(runtime::VirtualLitchSession& session, const char* run,
             const char* fen = POSITION, uint64_t occupancy = BASE) {
    // Complete production startup and the 200 ms placement stability interval
    // before importing, then let the imported physical position stabilize too.
    // Every pickup, return and capture below is a complete raw Hall snapshot.
    const bool initialized = session.Reset() == VIRTUAL_LITCH_OK &&
        Observe(session, INITIAL_OCCUPANCY) &&
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
        Set(session, "TIMER", "1") && Set(session, "HINT_LEVEL", "4") &&
        session.LoadExternalFen(fen) && Observe(session, occupancy) &&
        session.Advance(200U) == VIRTUAL_LITCH_OK;
    StaticJsonDocument<1024> home;
    const bool playReady = deserializeJson(home, session.HomeStateLine().CStr()) ==
            DeserializationError::Ok && home["pm"].is<int>() &&
        home["pm"].as<int>() == 0 && std::strcmp(home["a"] | "", "idle") == 0;
    const bool ready = initialized && playReady && Idle(session) && Fen(session) == fen &&
        session.GameClock() != nullptr && session.GameClock()->IsEnabled();
    Check(ready, run,
          "production import, settings and stable raw occupancy establish an idle timed game outside placement");
    if (!ready) Checkpoint(session, run, "setup-not-ready", "complete-production-setup");
    return ready;
}

bool TapHint(runtime::VirtualLitchSession& session) {
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    const modules::UiOwner owner = modules::MapOwnerToPhysicalHomeEdge(
        modules::UiOwner::P1, rotate);
    const uint8_t physicalOwner = owner == modules::UiOwner::P2
                                     ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
    return session.SetButtonDown(physicalOwner, runtime::VirtualLitchPhysicalButton::O,
                                 true) == VIRTUAL_LITCH_OK &&
           session.Advance(20U) == VIRTUAL_LITCH_OK &&
           session.SetButtonDown(physicalOwner, runtime::VirtualLitchPhysicalButton::O,
                                 false) == VIRTUAL_LITCH_OK &&
           session.Advance(20U) == VIRTUAL_LITCH_OK;
}

enum class ReturnOrder { Together, ExtraFirst, SelectedFirst };

void PickupReturnJourney(ReturnOrder order, bool rotate) {
    const char* orderName = order == ReturnOrder::Together ? "together" :
        order == ReturnOrder::ExtraFirst ? "extra-first" : "selected-first";
    char run[96] = {};
    std::snprintf(run, sizeof(run), "HeldA2ExtraE4Return/%s/rotate-%u", orderName, rotate);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    config.board_rotate = rotate;
    test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
    if (!Prepare(session, run)) return;
    const auto* clock = session.GameClock();
    const uint8_t history = session.UndoHistoryMoveCount();
    const uint32_t white = clock->GetRemainingMs(runtime::ClockSide::White);
    const uint32_t black = clock->GetRemainingMs(runtime::ClockSide::Black);
    const auto activeSide = clock->GetActiveSide();
    const uint64_t held = BASE & ~(UINT64_C(1) << A2);
    const uint64_t bothHeld = held & ~(UINT64_C(1) << E4);
    Checkpoint(session, run, "idle", "lift-a2");
    Check(Observe(session, held) && session.State().selected_square == A2 &&
              (session.State().valid_destination_mask & (UINT64_C(1) << A3)) != 0ULL,
          run, "a2 pickup obtains current legal pawn guidance");
    Check(Observe(session, bothHeld) && session.State().interaction_phase ==
              static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
              session.State().selected_square == A2 && YellowReturnCue(session, E4),
          run, "extra friendly e4 pickup retains the original selected source");
    Checkpoint(session, run, "extra-friendly-held", "return-e4-and-cancel-a2");
    if (order != ReturnOrder::Together) {
        const uint64_t intermediate = order == ReturnOrder::ExtraFirst
            ? held : BASE & ~(UINT64_C(1) << E4);
        Check(Observe(session, intermediate) && session.State().selected_square == A2 &&
                  session.UndoHistoryMoveCount() == history && Fen(session) == POSITION,
              run, "a partial return preserves unresolved physical recovery without committing");
        Checkpoint(session, run, "partial-return", order == ReturnOrder::ExtraFirst
                       ? "return-a2" : "return-e4");
    }
    Check(Observe(session, BASE) && Idle(session) && !YellowReturnCue(session, E4), run,
          "returning the complete board cancels to Idle without phantom selection or legal targets");
    Check(Fen(session) == POSITION && session.UndoHistoryMoveCount() == history &&
              clock->GetActiveSide() == activeSide &&
              clock->GetRemainingMs(runtime::ClockSide::White) == white &&
              clock->GetRemainingMs(runtime::ClockSide::Black) == black,
          run, "same-time pickup cancellation changes neither FEN, history, clock side nor balances");
    Checkpoint(session, run, "returned", "choose-a-piece-or-Hint");
    Check(session.Advance(1200U) == VIRTUAL_LITCH_OK && Idle(session) &&
              !YellowReturnCue(session, E4), run,
          "ordinary ticks expire return feedback without reviving phantom selection");
    Checkpoint(session, run, "return-feedback-expired", "lift-e4-queen");
    Check(Observe(session, BASE & ~(UINT64_C(1) << E4)) &&
              session.State().selected_square == E4 &&
              (session.State().valid_destination_mask & (UINT64_C(1) << D5)) != 0ULL,
          run, "a different fresh pickup receives queen guidance instead of phantom pawn guidance");
    Checkpoint(session, run, "fresh-pickup", "return-e4-then-tap-O");
    Check(Observe(session, BASE) && Idle(session) && TapHint(session), run,
          "fresh pickup cancellation permits the real debounced Hint control");
    uint8_t from = 64U;
    uint8_t to = 64U;
    bool hintReady = false;
    for (uint8_t attempt = 0U; attempt < 40U; ++attempt) {
        hintReady = session.TryGetHintMoveSquares(from, to);
        if (hintReady) break;
        if (session.Advance(100U) != VIRTUAL_LITCH_OK) break;
    }
    modules::ControlMutationOutcome outcome;
    Check(hintReady && from < 64U && to < 64U && from != to &&
              session.HintDisplayRequested() &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::Hint &&
              (outcome.status == modules::ControlMutationStatus::Applied ||
               outcome.status == modules::ControlMutationStatus::Accepted) &&
              Fen(session) == POSITION && session.UndoHistoryMoveCount() == history &&
              clock->GetActiveSide() == activeSide &&
              clock->GetRemainingMs(runtime::ClockSide::White) <= white &&
              clock->GetRemainingMs(runtime::ClockSide::Black) <= black,
          run, "accepted Hint produces current guidance without a move, increment or side switch");
    Checkpoint(session, run, "Hint-ready", "follow-Hint-or-select-another-move");
}

void GenuineCorrectionAndCaptureControls() {
    constexpr const char* RUN = "PickupReturn/GenuineCorrectionAndCapture";
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    {
        test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
        if (!Prepare(session, RUN)) return;
        const uint8_t history = session.UndoHistoryMoveCount();
        Check(Observe(session, BASE & ~(UINT64_C(1) << D6)) &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::Correction),
              RUN, "a non-capturable opponent pickup still requires real Correction");
        Checkpoint(session, RUN, "wrong-owner-pickup", "return-d6");
        Check(Observe(session, BASE) && Idle(session) && Fen(session) == POSITION &&
                  session.UndoHistoryMoveCount() == history,
              RUN, "restoring real Correction preserves the game and returns Idle");
    }
    {
        test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
        const uint64_t occupancy = (BASE & ~(UINT64_C(1) << D6)) |
                                   (UINT64_C(1) << D5);
        if (!Prepare(session, RUN, CAPTURE_POSITION, occupancy)) return;
        const uint8_t history = session.UndoHistoryMoveCount();
        const uint64_t targetRemoved = occupancy & ~(UINT64_C(1) << D5);
        Check(Observe(session, targetRemoved) && session.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::CaptureStaged),
              RUN, "legal target-first removal remains a staged capture");
        Check(Observe(session, targetRemoved & ~(UINT64_C(1) << E4)) &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::ExecutingPlan),
              RUN, "lifting the legal capturer retains the existing execution plan");
        Checkpoint(session, RUN, "capture-in-hand", "place-e4-queen-on-d5");
        const uint64_t captured = occupancy & ~(UINT64_C(1) << E4);
        Check(Observe(session, captured) && session.UndoHistoryMoveCount() == history &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling),
              RUN, "real capture reaches settle without premature commit or cancellation");
        Check(CaptureDestinationUsesMoverColor(session), RUN,
              "target-first capture destination has the mover's semantic and mapped physical piece color");
        bool captureColorStable = true;
        bool captureAdvanceOk = true;
        for (uint16_t elapsed = 0U; elapsed < 2600U; ++elapsed) {
            captureAdvanceOk = captureAdvanceOk &&
                               session.Advance(1U) == VIRTUAL_LITCH_OK;
            captureColorStable = captureColorStable &&
                                 CaptureDestinationUsesMoverColor(session);
        }
        Check(captureAdvanceOk && captureColorStable &&
                  session.State().expected_occupied_mask == captured &&
                  session.State().side_to_move_black == 1U &&
                  session.UndoHistoryMoveCount() == history + 1U,
              RUN, "every capture settle and commit frame keeps the mover color and receives one authoritative history");
        Check(CaptureDestinationUsesMoverColor(session), RUN,
              "authoritative capture acknowledgement preserves the mover's piece color");
        Checkpoint(session, RUN, "capture-committed", "Black-to-move");
    }
}

void RookCaptureAtomicJourney(const bool rotate) {
    char run[96] = {};
    std::snprintf(run, sizeof(run), "RookD1D5/observed-removal/rotate-%u", rotate);
    constexpr const char* position = "6k1/8/8/3p4/8/8/P7/3RK3 w - - 0 1";
    constexpr uint64_t source = UINT64_C(1) << D1;
    constexpr uint64_t victim = UINT64_C(1) << D5;
    constexpr uint64_t extra = UINT64_C(1) << A2;
    constexpr uint64_t occupancy = source | victim | extra |
        (UINT64_C(1) << 6U) | (UINT64_C(1) << 60U);
    constexpr uint64_t removed = occupancy & ~victim;
    constexpr uint64_t captured = occupancy & ~source;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.board_rotate = rotate;
    {
        test_support::BoundedHintSession fixture(config);
        auto& session = fixture.session;
        if (!Prepare(session, run, position, occupancy)) return;
        Check(Observe(session, removed) && Observe(session, occupancy) &&
                  Idle(session) && Fen(session) == position,
              run, "victim return without mover departure still cancels staging");
        Checkpoint(session, run, "victim-return-cancels", "remove-D5-victim");

        Check(Observe(session, removed) && session.Advance(100U) == VIRTUAL_LITCH_OK &&
                  Observe(session, captured) && session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
                  session.UndoHistoryMoveCount() == 0U,
              run, "observed vacancy then atomic D1-off/D5-on reaches unconfirmed settle");
        Checkpoint(session, run, "fused-settle-cancelable", "wait-or-return-rook-D1");
        Check(Observe(session, occupancy) && Idle(session) &&
                  session.Advance(2600U) == VIRTUAL_LITCH_OK && Idle(session) &&
                  Fen(session) == position && session.UndoHistoryMoveCount() == 0U,
              run, "source restoration cancels the indistinguishable victim-return/mover-held interpretation without a later commit");
        Checkpoint(session, run, "source-restored", "remove-D5-victim");

        Check(Observe(session, removed) && Observe(session, captured & ~extra) &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::Correction) &&
                  session.UndoHistoryMoveCount() == 0U,
              run, "extra removal in the fused batch requires correction without a capture");
        Checkpoint(session, run, "extra-removal-rejected", "restore-D1-and-A2");
        Check(Observe(session, occupancy) && Idle(session) && Fen(session) == position,
              run, "restoring the whole board recovers extra-removal correction");
        Check(Observe(session, removed) &&
                  Observe(session, captured | (UINT64_C(1) << 51U)) &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::Correction) &&
                  session.UndoHistoryMoveCount() == 0U,
              run, "extra D2 placement cannot reinterpret the fused capture as a quiet move");
        Checkpoint(session, run, "extra-placement-rejected", "clear-D2-and-restore-D1");
        Check(Observe(session, occupancy) && Idle(session) && Fen(session) == position,
              run, "clearing the extra placement recovers the original game");

        Check(Observe(session, removed) && Observe(session, occupancy & ~extra) &&
                  session.Advance(2600U) == VIRTUAL_LITCH_OK &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
                  session.State().selected_square == A2 &&
                  session.State().valid_destination_mask ==
                      ((UINT64_C(1) << A3) | (UINT64_C(1) << 32U)) &&
                  (session.State().valid_destination_mask & victim) == 0ULL &&
                  session.State().expected_occupied_mask == occupancy &&
                  session.State().side_to_move_black == 0U &&
                  session.State().pending_action_token == 0U &&
                  Fen(session) == position && session.UndoHistoryMoveCount() == 0U,
              run, "D5 return with A2 departure remains an ordinary A2 pickup with only A3/A4 legal and no capture committed");
        Checkpoint(session, run, "victim-return-A2-held", "return-A2-piece-or-place-A3-A4");
        Check(Observe(session, occupancy) && Idle(session) &&
                  session.State().expected_occupied_mask == occupancy &&
                  session.State().side_to_move_black == 0U &&
                  Fen(session) == position && session.UndoHistoryMoveCount() == 0U,
              run, "returning A2 cancels the ambiguous pickup and recovers without a move");

        Check(Observe(session, removed) && session.Advance(100U) == VIRTUAL_LITCH_OK &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::CaptureStaged),
              run, "complete raw Hall scan records the D5 victim vacancy");
        Checkpoint(session, run, "victim-observed-empty", "move-rook-D1-to-D5");
        Check(Observe(session, captured) && session.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
                  session.State().pending_action_token == 0U &&
                  session.State().expected_occupied_mask == occupancy &&
                  CaptureDestinationUsesMoverColor(session),
              run, "atomic mover departure and landing retain removal evidence and show mover settle color");
        Checkpoint(session, run, "atomic-capture-settling", "wait-for-confirmation");
        Check(session.Advance(1000U) == VIRTUAL_LITCH_OK &&
                  session.UndoHistoryMoveCount() == 0U,
              run, "fused capture retains the ordinary confirmation deadline");
        Check(session.Advance(1600U) == VIRTUAL_LITCH_OK && Idle(session) &&
                  session.State().expected_occupied_mask == captured &&
                  session.State().side_to_move_black == 1U &&
                  session.UndoHistoryMoveCount() == 1U &&
                  CaptureDestinationUsesMoverColor(session),
              run, "engine acknowledgement commits D1-to-D5 once and retires capture guidance");
        Checkpoint(session, run, "capture-acknowledged", "Black-to-move");
        Check(session.Advance(2000U) == VIRTUAL_LITCH_OK && Idle(session) &&
                  session.UndoHistoryMoveCount() == 1U,
              run, "expired capture presentation cannot replay the move");
    }
    {
        test_support::BoundedHintSession fixture(config);
        auto& session = fixture.session;
        if (!Prepare(session, run, position, occupancy)) return;
        Check(Observe(session, captured) && session.Advance(5000U) == VIRTUAL_LITCH_OK &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
                  session.State().selected_square == D1 &&
                  session.UndoHistoryMoveCount() == 0U && Fen(session) == position,
              run, "never-observed D5 vacancy stays pending even after the confirmation deadline");
        Checkpoint(session, run, "vacancy-never-observed", "lift-rook-off-D5");
        Check(Observe(session, captured & ~victim) &&
                  session.Advance(100U) == VIRTUAL_LITCH_OK &&
                  session.State().required_action ==
                      static_cast<uint8_t>(runtime::RequiredPhysicalAction::PlacePrimary),
              run, "recovery lift supplies actual vacancy and requests D5 placement");
        Checkpoint(session, run, "recovery-D5-empty", "reseat-rook-D5");
        Check(Observe(session, captured) && session.Advance(2600U) == VIRTUAL_LITCH_OK &&
                  Idle(session) && session.State().expected_occupied_mask == captured &&
                  session.State().side_to_move_black == 1U &&
                  session.UndoHistoryMoveCount() == 1U,
              run, "reseating the rook completes the pending capture through the production engine");
        Checkpoint(session, run, "recovery-acknowledged", "Black-to-move");
    }
}


void ReturnedSourceCaptureHandoffJourney(const bool rotate,
                                        const bool invalidTargetFirst) {
    constexpr const char* position = "6k1/8/8/2pp4/8/8/P7/3RK3 w - - 0 1";
    constexpr const char* completed = "6k1/8/8/2pR4/8/8/P7/4K3 b - - 0 1";
    constexpr uint64_t selected = UINT64_C(1) << A2;
    constexpr uint64_t rook = UINT64_C(1) << D1;
    constexpr uint64_t victim = UINT64_C(1) << D5;
    constexpr uint64_t invalidVictim = UINT64_C(1) << C5;
    constexpr uint64_t before = selected | rook | victim | invalidVictim |
        (UINT64_C(1) << 6U) | (UINT64_C(1) << 60U);
    constexpr uint64_t after = before & ~rook;
    char run[112] = {};
    std::snprintf(run, sizeof(run), "ReturnedA2AndVictimSameScan/%s/rotate-%u",
        invalidTargetFirst ? "noncapturable-C5-recovery" : "legal-D5", rotate);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.board_rotate = rotate;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    if (!Prepare(session, run, position, before)) return;
    Check(Set(session, "MOVE_CONFIRM", "\"settle_2s\"") &&
              Set(session, "TAKEBACK_GRACE_SECONDS", "0"), run,
          "production settings select ordinary two-second capture confirmation");
    const auto advance = [&](uint32_t duration) {
        while (duration != 0U) {
            const uint32_t step = duration > 10U ? 10U : duration;
            if (session.Advance(step) != VIRTUAL_LITCH_OK) return false;
            duration -= step;
        }
        return true;
    };
    const auto unchanged = [&]() {
        return Fen(session) == position && session.UndoHistoryMoveCount() == 0U &&
            session.State().expected_occupied_mask == before &&
            session.State().side_to_move_black == 0U &&
            session.State().pending_action_token == 0U;
    };
    const auto staged = [&]() {
        return session.State().interaction_phase ==
                static_cast<uint8_t>(runtime::ChessInteractionPhase::CaptureStaged) &&
            session.State().selected_square == D5 &&
            session.State().required_action ==
                static_cast<uint8_t>(runtime::RequiredPhysicalAction::LiftPrimary) &&
            session.State().valid_destination_mask == 0ULL &&
            session.State().correction_mask == 0ULL &&
            session.State().human_illegal_mask == 0ULL &&
            session.State().cpu_illegal_mask == 0ULL;
    };
    const auto step = [&](const uint64_t occupied, const char* message) {
        Check(advance(250U) && Observe(session, occupied), run, message);
    };
    Checkpoint(session, run, "stable-position", "lift-A2");
    if (invalidTargetFirst) {
        step(before & ~selected, "select A2 before the noncapturable victim handoff");
        step(before & ~invalidVictim, "one Hall scan returns A2 and removes noncapturable C5");
        Check(unchanged() && session.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::Correction) &&
                  session.State().required_action ==
                      static_cast<uint8_t>(runtime::RequiredPhysicalAction::CorrectBoard) &&
                  session.State().correction_mask == invalidVictim &&
                  session.State().valid_destination_mask == 0ULL,
              run, "noncapturable removal cancels A2 and requests only the actual C5 correction");
        Check(advance(350U) && unchanged(), run,
              "noncapturable handoff remains uncommitted while local correction appears");
        const auto semantic = session.SemanticFrame().board[C5][0U];
        const auto physical = session.PhysicalFrame().pixels[PhysicalCueIndex(session, C5)];
        Check(semantic.r > semantic.g && semantic.g > 0U && semantic.b == 0U &&
                  physical.r > physical.g && physical.g > 0U && physical.b == 0U,
              run, "noncapturable victim receives mapped amber correction instead of an A2 capture prompt");
        Checkpoint(session, run, "noncapturable-victim-rejected", "return-C5");
        Check(advance(2250U) && unchanged(), run,
              "a stale confirmation deadline cannot commit the noncapturable handoff");
        step(before, "restore C5 after the rejected handoff");
        Check(Idle(session) && unchanged(), run,
              "canonical restoration retires the returned pawn's selection and every action");
        Checkpoint(session, run, "invalid-handoff-restored", "lift-A2-then-return-it-while-removing-D5");
    }

    step(before & ~selected, "lift A2 to begin an ordinary pawn selection");
    Check(unchanged() && session.State().interaction_phase ==
              static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
              session.State().selected_square == A2 &&
              session.State().valid_destination_mask != 0ULL,
          run, "A2 owns legal pawn guidance before its physical return");
    step(before & ~victim, "one Hall scan returns A2 and removes the legal D5 victim");
    Check(unchanged() && staged(), run,
          "atomic A2 return cancels pawn selection before staging D5 for its legal capturer");
    Checkpoint(session, run, "A2-returned-D5-staged", "lift-D1-rook");

    const auto firstPiece = session.PhysicalFrame().pixels[
        PhysicalCueIndex(session, D1, hardware::SquareLedSlot::Piece)];
    bool capturerAnimated = false;
    bool capturerVisible = false;
    bool targetCueVisible = true;
    bool stagingRetained = true;
    // One complete capturer pulse proves guidance belongs to the real D1 rook;
    // the returned pawn has no selected-source destination mask or plan.
    for (uint8_t sample = 0U; sample < 90U; ++sample) {
        const bool advanced = advance(10U);
        stagingRetained = stagingRetained && advanced && staged() && unchanged();
        const auto piece = session.PhysicalFrame().pixels[
            PhysicalCueIndex(session, D1, hardware::SquareLedSlot::Piece)];
        capturerVisible = capturerVisible || piece.r != 0U || piece.g != 0U ||
            piece.b != 0U || piece.w != 0U;
        capturerAnimated = capturerAnimated || piece.r != firstPiece.r ||
            piece.g != firstPiece.g || piece.b != firstPiece.b || piece.w != firstPiece.w;
        const auto landing = session.PhysicalFrame().pixels[PhysicalCueIndex(session, D5)];
        const auto emptyPiece = session.PhysicalFrame().pixels[
            PhysicalCueIndex(session, D5, hardware::SquareLedSlot::Piece)];
        targetCueVisible = targetCueVisible &&
            (landing.r != 0U || landing.g != 0U || landing.b != 0U || landing.w != 0U) &&
            emptyPiece.r == 0U && emptyPiece.g == 0U && emptyPiece.b == 0U && emptyPiece.w == 0U;
    }
    Check(stagingRetained && capturerVisible && capturerAnimated && targetCueVisible,
          run, "physical D1 capturer pulses while empty D5 retains landing guidance with dark piece LEDs");
    Check(advance(1400U) && unchanged() && staged(), run,
          "waiting beyond a confirmation interval cannot revive A2 or commit target-first staging");
    Checkpoint(session, run, "staged-without-ghost-deadline", "lift-D1-and-place-on-D5");
    step(before & ~victim & ~rook, "lift the actual D1 capturer after returning A2");
    Check(unchanged() && session.State().interaction_phase ==
              static_cast<uint8_t>(runtime::ChessInteractionPhase::ExecutingPlan) &&
              session.State().selected_square != A2 &&
              session.State().required_action ==
                  static_cast<uint8_t>(runtime::RequiredPhysicalAction::PlacePrimary),
          run, "the new rook pickup owns placement without a ghost A2 capture");
    Checkpoint(session, run, "D1-capture-in-hand", "place-rook-D5");
    step(after, "place the D1 rook on the observed-empty D5 victim square");
    Check(unchanged() && session.State().interaction_phase ==
              static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
              CaptureDestinationUsesMoverColor(session),
          run, "rook landing begins ordinary unconfirmed mover-colored settling");
    Checkpoint(session, run, "rook-capture-settling", "wait-two-seconds");
    Check(advance(1990U) && unchanged(), run,
          "the rook capture retains its full new confirmation interval");
    Check(advance(20U) && Idle(session) && Fen(session) == completed &&
              session.State().expected_occupied_mask == after &&
              session.State().physical_occupied_mask == after &&
              session.State().side_to_move_black == 1U &&
              session.UndoHistoryMoveCount() == 1U,
          run, "production acknowledgement commits only D1-D5 and leaves A2 home");
    Checkpoint(session, run, "rook-capture-acknowledged", "Black-to-move");
    Check(advance(2500U) && Idle(session) && Fen(session) == completed &&
              session.UndoHistoryMoveCount() == 1U &&
              session.State().pending_action_token == 0U,
          run, "expired staging and settle cannot resurrect the pawn or repeat the rook move");
    Checkpoint(session, run, "expired-handoff-retains-one-move", "Black-to-move");
}

void PawnCaptureNearEnPassantJourney(const bool rotate) {
    constexpr const char* position = "6k1/8/5b2/3pP3/8/8/P7/4K3 w - d6 0 1";
    constexpr uint64_t source = UINT64_C(1) << E5;
    constexpr uint64_t epVictim = UINT64_C(1) << D5;
    constexpr uint64_t ordinaryVictim = UINT64_C(1) << F6;
    constexpr uint64_t occupancy = source | epVictim | ordinaryVictim |
        (UINT64_C(1) << A2) | (UINT64_C(1) << 6U) | (UINT64_C(1) << 60U);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.board_rotate = rotate;
    for (const bool enPassant : {false, true}) {
        char run[96] = {};
        std::snprintf(run, sizeof(run), "PawnE5/EP-metadata/%s/rotate-%u",
                      enPassant ? "en-passant" : "ordinary-f6", rotate);
        test_support::BoundedHintSession fixture(config);
        auto& session = fixture.session;
        if (!Prepare(session, run, position, occupancy)) return;
        if (enPassant) {
            Check(Observe(session, occupancy & ~epVictim) &&
                      Observe(session, occupancy & ~source) &&
                      session.Advance(2600U) == VIRTUAL_LITCH_OK &&
                      session.State().interaction_phase ==
                          static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
                      session.State().selected_square == E5 &&
                      session.UndoHistoryMoveCount() == 0U && Fen(session) == position,
                  run, "restoring the EP victim with source pickup cannot become an ordinary capture");
            Checkpoint(session, run, "EP-victim-return-cancels", "remove-D5-victim-then-place-E5-pawn-on-D6");
            Check(Observe(session, occupancy) && Idle(session), run,
                  "returning the pawn after EP cancellation restores the original game");
        }
        const uint8_t landing = enPassant ? D6 : F6;
        const uint64_t victim = enPassant ? epVictim : ordinaryVictim;
        const uint64_t captured = (occupancy & ~source & ~victim) |
                                 (UINT64_C(1) << landing);
        Check(Observe(session, occupancy & ~victim) &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::CaptureStaged),
              run, "the actual capture victim is observed empty first");
        Checkpoint(session, run, "victim-observed-empty", enPassant ? "move-E5-pawn-to-D6" : "move-E5-pawn-to-F6");
        if (enPassant) {
            const auto checkEpRoute = [&](const char* message) {
                bool sourceVisible = false, landingVisible = false, victimDark = true;
                const bool mappedRotate = modules::ComputeSquareRotate(
                    session.BoardRotate(), session.FirstPlayerIsP2());
                const auto victimPieces = hardware::MapSemanticSquareLedTargets(
                    hardware::BoardRevision::V3_3S3, D5,
                    hardware::SquareLedSlot::Piece, mappedRotate);
                for (unsigned sample = 0U; sample < 60U; ++sample) {
                    Check(session.Advance(30U) == VIRTUAL_LITCH_OK, run, "advance EP route playback");
                    for (const auto slot : {hardware::SquareLedSlot::Square0,
                                            hardware::SquareLedSlot::Square2}) {
                        const auto sourcePixel = session.PhysicalFrame().pixels[PhysicalCueIndex(session, E5, slot)];
                        const auto landingPixel = session.PhysicalFrame().pixels[PhysicalCueIndex(session, D6, slot)];
                        sourceVisible = sourceVisible || sourcePixel.r > sourcePixel.g + 3U;
                        landingVisible = landingVisible || landingPixel.r > landingPixel.g + 3U;
                    }
                    for (uint8_t i = 0U; i < victimPieces.count; ++i) {
                        const auto pixel = session.PhysicalFrame().pixels[victimPieces.indices[i]];
                        victimDark = victimDark && (pixel.r | pixel.g | pixel.b | pixel.w) == 0U;
                    }
                }
                Check(sourceVisible && landingVisible && victimDark && victimPieces.count == 2U,
                      run, message);
            };
            checkEpRoute("staged EP previews E5 to actual D6 landing while both empty D5 Piece emitters stay dark");
            Check(Observe(session, occupancy & ~victim & ~source) &&
                      session.State().required_action ==
                          static_cast<uint8_t>(runtime::RequiredPhysicalAction::PlacePrimary),
                  run, "lifting E5 hands staged EP to the physical placement action");
            checkEpRoute("held EP continues E5-to-D6 guidance without illuminating empty D5 Piece emitters");
        }
        Check(Observe(session, captured) && session.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
                  session.UndoHistoryMoveCount() == 0U,
              run, "atomic pawn departure and landing preserve the chosen capture's vacancy");
        Checkpoint(session, run, "atomic-pawn-capture-settling", "wait-for-confirmation");
        Check(session.Advance(2600U) == VIRTUAL_LITCH_OK && Idle(session) &&
                  session.State().expected_occupied_mask == captured &&
                  session.State().side_to_move_black == 1U &&
                  session.UndoHistoryMoveCount() == 1U,
              run, "the production engine acknowledges exactly one pawn capture");
        Check(Fen(session) == (enPassant ?
                  "6k1/8/3P1b2/8/8/8/P7/4K3 b - - 0 1" :
                  "6k1/8/5P2/3p4/8/8/P7/4K3 b - - 0 1"),
              run, "ordinary capture preserves d5 while actual EP clears d5 and lands d6");
        Checkpoint(session, run, "pawn-capture-acknowledged", "Black-to-move");
    }
}

void EnPassantLandingWithUnrelatedPickupJourney() {
    constexpr const char* position = "7k/p7/8/3pP3/8/8/8/7K w - d6 0 1";
    constexpr const char* completed = "7k/p7/3P4/8/8/8/8/7K b - - 0 1";
    constexpr uint64_t source = UINT64_C(1) << E5;
    constexpr uint64_t target = UINT64_C(1) << D6;
    constexpr uint64_t victim = UINT64_C(1) << D5;
    constexpr uint64_t extra = UINT64_C(1) << A7;
    constexpr uint64_t before = source | victim | extra |
        (UINT64_C(1) << 7U) | (UINT64_C(1) << 63U);
    constexpr uint64_t wrong = (before & ~source & ~extra) | target;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    for (const bool sourceAlreadyLifted : {false, true}) {
        const char* run = sourceAlreadyLifted
            ? "EnPassant/held-source/landing-plus-A7-pickup"
            : "EnPassant/atomic-source/landing-plus-A7-pickup";
        test_support::BoundedHintSession fixture(config);
        auto& session = fixture.session;
        if (!Prepare(session, run, position, before)) return;
        // Service the production timers throughout each pause, not only at
        // its endpoint. Physical changes remain at least 250 ms apart.
        const auto advance = [&](uint32_t duration) {
            while (duration != 0U) {
                const uint32_t step = duration > 10U ? 10U : duration;
                if (session.Advance(step) != VIRTUAL_LITCH_OK) return false;
                duration -= step;
            }
            return true;
        };
        const auto unchanged = [&]() {
            return Fen(session) == position &&
                session.UndoHistoryMoveCount() == 0U &&
                session.State().side_to_move_black == 0U &&
                session.State().expected_occupied_mask == before &&
                session.State().pending_action_token == 0U;
        };
        if (sourceAlreadyLifted) {
            Check(advance(250U) && Observe(session, before & ~source), run,
                  "lift e5 before the mixed landing");
        }
        Check(advance(250U) && Observe(session, wrong) && unchanged() &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::Correction) &&
                  session.State().required_action ==
                      static_cast<uint8_t>(runtime::RequiredPhysicalAction::CorrectBoard) &&
                  session.State().correction_mask == (source | target | extra),
              run, "mixed EP landing requires canonical correction without claiming a move");
        Checkpoint(session, run, "mixed-landing-rejected", "return-A7-and-E5-remove-D6");
        Check(advance(350U) && unchanged(), run,
              "correction keeps the game unchanged while its local cues appear");
        for (const uint8_t square : {E5, D6, A7}) {
            const auto cue = session.SemanticFrame().board[square][0U];
            const auto physical = session.PhysicalFrame().pixels[
                PhysicalCueIndex(session, square)];
            Check(cue.r > cue.g && cue.g > 0U && cue.b == 0U &&
                      physical.r > physical.g && physical.g > 0U && physical.b == 0U,
                  run, "each missing or extra square has a local amber correction bracket");
        }
        Checkpoint(session, run, "local-correction-cues", "restore-A7-and-return-pawn-D6-to-E5");
        Check(advance(1250U) && unchanged(), run,
              "strong correction retains the unchanged game");
        Checkpoint(session, run, "strong-correction-cues", "restore-A7-and-return-pawn-D6-to-E5");
        Check(advance(1750U) && unchanged() &&
                  session.State().correction_mask == (source | target | extra),
              run, "confirmation deadline cannot commit incomplete EP behind correction");
        Checkpoint(session, run, "deadline-passed-no-move", "restore-the-three-marked-squares");
        Check(advance(250U) && Observe(session, wrong | extra) &&
                  advance(250U) && Observe(session, before & ~source) &&
                  advance(250U) && Observe(session, before) &&
                  Idle(session) && unchanged(),
              run, "restoring the canonical board clears correction without a move");
        Checkpoint(session, run, "canonical-board-restored", "retry-E5-to-D6-then-remove-D5");

        Check(advance(250U) && Observe(session, before & ~source) &&
                  advance(250U) && Observe(session, (before & ~source) | target) &&
                  session.State().required_action ==
                      static_cast<uint8_t>(runtime::RequiredPhysicalAction::RemoveCapture) &&
                  advance(3000U) && unchanged(),
              run, "valid EP landing still waits for its actual captured pawn past the deadline");
        Checkpoint(session, run, "valid-EP-waits-for-D5", "remove-the-black-pawn-on-D5");
        bool sawBright = false, sawDim = false;
        for (uint8_t sample = 0U; sample < 13U; ++sample) {
            Check(advance(100U) && unchanged(), run,
                  "directed EP removal does not advance the game");
            const auto& victimLeds = session.SemanticFrame().board[D5];
            const auto& left = victimLeds[0U];
            const auto& right = victimLeds[2U];
            const auto& piece = victimLeds[1U];
            Check(left.r > 0U && left.r >= left.g && left.b == 0U && left.w == 0U &&
                      right.r == left.r && right.g == left.g && right.b == left.b && right.w == left.w &&
                      (piece.r | piece.g | piece.b | piece.w) == 0U,
                  run, "after correction both rails on the actual EP victim flash together with its Piece LEDs dark");
            sawBright = sawBright || left.r == 255U;
            sawDim = sawDim || (left.r == 64U && left.g == 25U);
        }
        Check(sawBright && sawDim, run,
              "the recovered EP removal alternates amber strikes with a dim rest");
        const uint64_t after = (before & ~source & ~victim) | target;
        Check(advance(250U) && Observe(session, after) && advance(2200U) &&
                  Idle(session) && Fen(session) == completed &&
                  session.State().expected_occupied_mask == after &&
                  session.State().side_to_move_black == 1U &&
                  session.UndoHistoryMoveCount() == 1U,
              run, "removing D5 after recovery commits exactly one legal EP move");
        Checkpoint(session, run, "valid-EP-acknowledged", "Black-to-move");
        Check(advance(2200U) && session.UndoHistoryMoveCount() == 1U, run,
              "expired correction and settle cannot duplicate the recovered move");
    }
}

void CaptureDuringCorrectionJourney() {
    constexpr const char* position =
        "r2q3r/2pp1Qpp/1p1k4/p2bN3/2B2B2/2P4P/P1P3P1/3R2KR w - - 0 1";
    constexpr const char* completed =
        "r2q3r/2pp1Qpp/1p1k4/p2RN3/2B2B2/2P4P/P1P3P1/6KR b - - 0 1";
    constexpr uint64_t before = UINT64_C(0xC8458424190AEC89);
    constexpr uint64_t source = UINT64_C(1) << D1;
    constexpr uint64_t victim = UINT64_C(1) << D5;
    constexpr uint64_t extra = UINT64_C(1) << H3;
    enum class Path { Complete, Circle, ExtraFirst, SourceFirst, VictimFirst,
                      Together, VacancyUnseen, SourceExtraVictim, VictimExtraSource };
    const char* names[] = {"complete", "fresh-O", "extra-restored-early",
                          "source-restored-first", "victim-restored-first",
                          "both-restored", "vacancy-unseen",
                          "return-source-extra-victim", "return-victim-extra-source"};
    for (const Path path : {Path::Complete, Path::Circle, Path::ExtraFirst,
                           Path::SourceFirst, Path::VictimFirst, Path::Together,
                           Path::VacancyUnseen, Path::SourceExtraVictim,
                           Path::VictimExtraSource}) {
        char run[96];
        std::snprintf(run, sizeof(run), "CaptureCorrection/D1-D5-H3/%s",
                      names[static_cast<unsigned>(path)]);
        virtual_litch_config config{};
        virtual_litch_default_config(&config);
        config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
        test_support::BoundedHintSession fixture(config);
        auto& session = fixture.session;
        const auto advance = [&](uint32_t duration) {
            while (duration != 0U) {
                const uint32_t step = duration > 10U ? 10U : duration;
                if (session.Advance(step) != VIRTUAL_LITCH_OK) return false;
                duration -= step;
            }
            return true;
        };
        const bool ready = session.Reset() == VIRTUAL_LITCH_OK &&
            Observe(session, INITIAL_OCCUPANCY) && advance(200U) &&
            Set(session, "TIMER", "0") && Set(session, "HINT_LEVEL", "8") &&
            Set(session, "TAKEBACK_GRACE_SECONDS", "0") &&
            Set(session, "P1_PIECE_WHEEL", "138") &&
            Set(session, "P2_PIECE_WHEEL", "95") &&
            Set(session, "MOVE_CONFIRM", path == Path::Circle
                ? "\"press_circle_to_confirm\"" : "\"settle_2s\"") &&
            Set(session, "SETTLE_ANIM_STYLE", "\"mini_orbit\"") &&
            session.LoadExternalFen(position) && Observe(session, before) &&
            advance(200U) && Idle(session) && TapHint(session) && advance(500U);
        uint8_t hintFrom = 255U, hintTo = 255U;
        Check(ready && session.TryGetHintMoveSquares(hintFrom, hintTo) &&
                  hintFrom == D1 && hintTo == D5, run,
              "exact support position reveals the production D1-D5 Hint");
        if (!ready) continue;
        const auto unchanged = [&]() {
            return Fen(session) == position && session.UndoHistoryMoveCount() == 0U &&
                session.State().pending_action_token == 0U &&
                session.State().side_to_move_black == 0U;
        };
        const auto step = [&](uint64_t occupied, const char* message) {
            Check(advance(250U) && Observe(session, occupied), run, message);
        };
        step(before & ~source, "lift D1");
        Check(session.HintDisplayRequested() &&
                  session.State().valid_destination_mask == UINT64_C(0x3708080808000000),
              run, "selected rook retains the support position's legal guidance");
        if (path == Path::SourceFirst || path == Path::VictimFirst ||
            path == Path::Together || path == Path::SourceExtraVictim ||
            path == Path::VictimExtraSource) {
            step(before & ~source & ~victim, "observe D5 vacancy before correction");
            step(before & ~source & ~victim & ~extra, "lift unrelated H3");
            if (path == Path::SourceExtraVictim || path == Path::VictimExtraSource) {
                const bool sourceFirst = path == Path::SourceExtraVictim;
                step(before & ~(sourceFirst ? victim : source) & ~extra,
                     "return the first capture piece while H3 remains absent");
                step(before & ~(sourceFirst ? victim : source),
                     "restore H3 before the last capture piece");
                Check(advance(250U) && unchanged(), run,
                      "partial restoration neither commits nor invents capture evidence");
                Checkpoint(session, run, "partial-return-correction-cleared",
                           sourceFirst ? "return-D5-victim" : "return-D1-rook");
                step(before, "return the final capture piece");
                Check(Idle(session) && unchanged(), run,
                      "interleaved full restoration clears every selection and prompt");
                Checkpoint(session, run, "interleaved-return-canceled", "fresh-D1-pickup");
            } else if (path == Path::SourceFirst) {
                step(before & ~victim & ~extra, "return D1 while H3 and D5 remain absent");
                step(before & ~extra, "return D5 while H3 remains absent");
            } else if (path == Path::VictimFirst) {
                step(before & ~source & ~extra, "return D5 while D1 remains held");
                step(before & ~extra, "return D1 while H3 remains absent");
            } else {
                step(before & ~extra, "restore D1 and D5 in one observation");
            }
            Check(unchanged(), run, "restoration invalidates the capture without changing the game");
            if (path == Path::SourceExtraVictim || path == Path::VictimExtraSource) {
                step(before & ~source, "lift D1 again without a fresh D5 vacancy");
            } else {
                step(before & ~source & ~extra, "lift D1 again without a fresh D5 vacancy");
                step(before & ~source, "restore H3 after the new pickup");
            }
            Check(advance(2500U) && unchanged() &&
                      session.State().interaction_phase ==
                          static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted),
                  run, "the later pickup cannot reuse removal proof from the canceled capture");
            Checkpoint(session, run, "stale-proof-rejected", "remove-D5-before-placing-the-rook");
            step(before, "return D1 to finish cancellation");
            Check(Idle(session) && unchanged(), run, "full restoration cancels the selected source");
            step(before & ~source, "fresh D1 pickup after cancellation");
        } else {
            step(before & ~source & ~extra, "lift unrelated H3 while D1 is held");
            Check(YellowReturnCue(session, H3), run, "H3 return remains the physical next action");
            if (path == Path::ExtraFirst) {
                step(before & ~source, "restore H3 before D5 removal");
            } else if (path == Path::VacancyUnseen) {
                step(before & ~source, "restore H3 without any D5-empty observation");
                Check(advance(2500U) && unchanged() &&
                          session.State().interaction_phase ==
                              static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted),
                      run, "correction cannot invent an unobserved victim vacancy");
                Checkpoint(session, run, "vacancy-not-invented", "lift-rook-from-D5-until-empty-then-reseat");
            } else {
                step(before & ~source & ~victim & ~extra, "remove D5 during correction");
                step(before & ~source & ~extra, "land the rook on D5 during correction");
                Check(advance(3000U) && TapHint(session) && advance(1200U) &&
                          unchanged(), run,
                      "deadline and O cannot commit while H3 remains absent");
                Checkpoint(session, run, "capture-complete-H3-still-wrong", "return-H3");
                step(before & ~source, "restore H3 after the physical capture");
            }
        }
        if (path != Path::Complete && path != Path::Circle) {
            step(before & ~source & ~victim, "fresh D5 vacancy supplies current capture proof");
            step(before & ~source, "land rook on D5");
        }
        Check(unchanged() && session.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
                  CaptureDestinationUsesMoverColor(session, 62U, D6),
              run, "restored correction starts ordinary mover-colored capture confirmation");
        Checkpoint(session, run, "correction-cleared-capture-settling",
                   path == Path::Circle ? "press-a-fresh-O" : "wait-for-confirmation");
        if (path == Path::Circle) {
            Check(advance(2500U) && unchanged() && TapHint(session) &&
                      advance(990U) && unchanged() && advance(20U),
                  run, "blocked O was discarded; only a fresh O starts confirmation");
        } else {
            Check(advance(1990U) && unchanged() && advance(20U), run,
                  "the full confirmation interval starts after correction clears");
        }
        Check(Idle(session) && Fen(session) == completed &&
                  session.UndoHistoryMoveCount() == 1U &&
                  session.State().side_to_move_black == 1U &&
                  fixture.engine.IsInCheck(modules::SideToMove::Black),
              run, "the recovered Rxd5 check is acknowledged exactly once");
        Checkpoint(session, run, "capture-acknowledged", "Black-to-move-in-check");
        Check(advance(2500U) && session.UndoHistoryMoveCount() == 1U &&
                  !YellowReturnCue(session, H3), run,
              "expired correction feedback cannot revive a prompt or duplicate the move");
    }
}

} // namespace

int main() {
    std::puts("[PICKUP_RETURN_VIRTUAL_LITCH] start");
    for (const bool rotate : {false, true}) {
        for (const ReturnOrder order : {ReturnOrder::Together, ReturnOrder::ExtraFirst,
                                       ReturnOrder::SelectedFirst}) {
            PickupReturnJourney(order, rotate);
        }
    }
    GenuineCorrectionAndCaptureControls();
    RookCaptureAtomicJourney(false);
    RookCaptureAtomicJourney(true);
    for (const bool rotate : {false, true}) {
        ReturnedSourceCaptureHandoffJourney(rotate, false);
        ReturnedSourceCaptureHandoffJourney(rotate, true);
    }
    PawnCaptureNearEnPassantJourney(false);
    PawnCaptureNearEnPassantJourney(true);
    EnPassantLandingWithUnrelatedPickupJourney();
    CaptureDuringCorrectionJourney();
    if (failures == 0) {
        std::puts("[PICKUP_RETURN_VIRTUAL_LITCH] ok");
        return 0;
    }
    std::printf("[PICKUP_RETURN_VIRTUAL_LITCH] failures=%d\n", failures);
    return 1;
}
