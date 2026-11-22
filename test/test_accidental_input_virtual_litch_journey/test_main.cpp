#include "bounded_hint_engine.hpp"

#include <cstdio>
#include <string>

#include <ArduinoJson.h>

#include "hardware/led_mapping.hpp"
#include "modules/correction_feedback.hpp"
#include "modules/orientation.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace {

constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t A2 = 48U;
constexpr uint8_t A3 = 40U;
constexpr uint8_t D6 = 19U;
constexpr uint8_t E4 = 36U;
constexpr uint8_t G8 = 6U;
constexpr uint64_t BASE = (UINT64_C(1) << G8) | (UINT64_C(1) << D6) |
    (UINT64_C(1) << E4) | (UINT64_C(1) << A2) | (UINT64_C(1) << 60U);
constexpr const char* FEN = "6k1/8/3p4/8/4Q3/8/P7/4K3 w - - 0 1";
int failures = 0;

void Check(const bool okay, const char* journey, const char* message) {
    if (!okay) {
        ++failures;
        std::printf("[FAIL] %s: %s\n", journey, message);
    }
}

virtual_litch_config Config() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    return config;
}

bool Observe(runtime::VirtualLitchSession& board, const uint64_t logical) {
    const bool rotate = modules::ComputeSquareRotate(
        board.BoardRotate(), board.FirstPlayerIsP2());
    uint64_t physical = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((logical & (UINT64_C(1) << square)) != 0ULL) {
            physical |= UINT64_C(1) << modules::MapSquareToPhysical(square, rotate);
        }
    }
    return board.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
        board.State().physical_occupied_mask == logical;
}

bool Set(runtime::VirtualLitchSession& board, const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return board.MutateSettingJson(900U, key, value, outcome) == VIRTUAL_LITCH_OK &&
        (outcome.status == modules::ControlMutationStatus::Applied ||
         outcome.status == modules::ControlMutationStatus::Noop);
}

std::string Fen(const runtime::VirtualLitchSession& board) {
    StaticJsonDocument<1024> home;
    if (deserializeJson(home, board.HomeStateLine().CStr())) return {};
    return home["fen"] | "";
}

bool Ready(runtime::VirtualLitchSession& board) {
    return board.Reset() == VIRTUAL_LITCH_OK && Observe(board, OPENING) &&
        board.Advance(200U) == VIRTUAL_LITCH_OK &&
        Set(board, "MOVE_CONFIRM", "\"press_circle_to_confirm\"") &&
        board.LoadExternalFen(FEN) && Observe(board, BASE) &&
        board.Advance(200U) == VIRTUAL_LITCH_OK &&
        board.State().interaction_phase ==
            static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle);
}

bool Equal(const virtual_litch_color a, const virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}

bool Rainbow(const runtime::VirtualLitchSession& board, const uint8_t square) {
    return !Equal(board.SemanticFrame().board[square][0],
                  board.SemanticFrame().board[square][2]);
}

bool RedFault(const runtime::VirtualLitchSession& board) {
    for (const auto color : board.SemanticFrame().status) {
        if (color.r == 0U || color.r <= color.g || color.r <= color.b) return false;
    }
    return true;
}

bool Amber(const runtime::VirtualLitchSession& board, const uint8_t square) {
    const auto color = board.SemanticFrame().board[square][0];
    const bool rotate = modules::ComputeSquareRotate(
        board.BoardRotate(), board.FirstPlayerIsP2());
    const uint16_t led = hardware::MapLedIndexForSquareAndEdgeRotate(
        hardware::LedIndexForSquare(square, hardware::SquareLedSlot::Square0),
        rotate, board.BoardRotate());
    const auto physical = board.PhysicalFrame().pixels[led];
    return color.r > color.g && color.g > 0U && color.b == 0U &&
        physical.r > physical.g && physical.g > 0U && physical.b == 0U &&
        !Rainbow(board, square);
}

void Log(const runtime::VirtualLitchSession& board, const char* journey,
         const char* action, const uint8_t square, const char* next) {
    const auto& state = board.State();
    const auto left = board.SemanticFrame().board[square][0];
    const auto right = board.SemanticFrame().board[square][2];
    const auto text = board.DisplayText();
    std::printf("[ACCIDENTAL_INPUT_JOURNEY] %s t=%u input=%s occupancy=%016llx "
                "expected=%016llx phase=%u correction=%016llx history=%u "
                "text=\"%.*s\" square=%u left=%u/%u/%u right=%u/%u/%u "
                "red_fault=%u next=%s\n", journey, state.now_ms, action,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                state.interaction_phase,
                static_cast<unsigned long long>(state.correction_mask),
                board.UndoHistoryMoveCount(), static_cast<int>(text.size()),
                text.empty() ? "" : text.data(), square,
                left.r, left.g, left.b, right.r, right.g, right.b,
                RedFault(board) ? 1U : 0U, next);
}

void RapidReplace() {
    constexpr const char* JOURNEY = "AccidentalInput/RapidOpponentBump";
    test_support::BoundedHintSession fixture(Config());
    auto& board = fixture.session;
    Check(Ready(board), JOURNEY, "production startup/import completes");
    Check(Observe(board, BASE & ~(UINT64_C(1) << D6)), JOURNEY, "wrong pickup observed");
    Check(board.State().correction_mask != 0ULL && Fen(board) == FEN,
          JOURNEY, "validation remains immediate; wrong pickup cannot move the engine");
    Check(!Rainbow(board, D6) && !RedFault(board), JOURNEY,
          "first bump frame does not shout rainbow or red fault");
    Log(board, JOURNEY, "lift-d6", D6, "replace-d6");
    Check(board.Advance(200U) == VIRTUAL_LITCH_OK && Observe(board, BASE),
          JOURNEY, "brief bump replaces within grace");
    Check(board.State().correction_mask == 0ULL && !Rainbow(board, D6) &&
              !RedFault(board) && Fen(board) == FEN && board.UndoHistoryMoveCount() == 0U,
          JOURNEY, "replacement immediately clears feedback without game/history changes");
    Check(board.Advance(1600U) == VIRTUAL_LITCH_OK && !RedFault(board), JOURNEY,
          "cancelled grace cannot produce a delayed angry-board flash");
    Log(board, JOURNEY, "replace+1600ms", D6, "lift-White-piece");
    Check(Observe(board, BASE & ~(UINT64_C(1) << A2)) &&
              board.State().selected_square == A2 &&
              (board.State().valid_destination_mask & (UINT64_C(1) << A3)) != 0ULL,
          JOURNEY, "the next intentional pickup gets legal targets immediately");
}

void SustainedMismatch(const bool extra) {
    const char* journey = extra ? "AccidentalInput/SustainedExtra"
                                : "AccidentalInput/SustainedOpponentLift";
    test_support::BoundedHintSession fixture(Config());
    auto& board = fixture.session;
    Check(Ready(board), journey, "production startup/import completes");
    const uint8_t square = extra ? A3 : D6;
    const uint64_t mismatch = extra ? BASE | (UINT64_C(1) << square)
                                    : BASE & ~(UINT64_C(1) << square);
    Check(Observe(board, mismatch), journey, "unexplained occupancy enters validation");
    Check(board.Advance(299U) == VIRTUAL_LITCH_OK && !Rainbow(board, square) &&
              !RedFault(board), journey, "quiet grace lasts through 299ms");
    Log(board, journey, "mismatch+299ms", square, "restore-square");
    Check(board.Advance(1U) == VIRTUAL_LITCH_OK && Amber(board, square) &&
              !RedFault(board), journey, "300ms starts a local steady amber cue, not global fault");
    Log(board, journey, "mismatch+300ms", square, "restore-square");
    Check(board.Advance(1199U) == VIRTUAL_LITCH_OK && Amber(board, square) &&
              !RedFault(board), journey, "gentle cue remains through 1499ms");
    Check(board.Advance(1U) == VIRTUAL_LITCH_OK && Rainbow(board, square) &&
              RedFault(board),
          journey, "sustained mismatch escalates at 1500ms");
    Log(board, journey, "mismatch+1500ms", square, "restore-square");
    Check(Observe(board, BASE) && board.State().correction_mask == 0ULL &&
              !Rainbow(board, square) && !RedFault(board) && Fen(board) == FEN,
          journey, "strong correction clears immediately on restoration");
}

void BumpDuringAnimation() {
    constexpr const char* JOURNEY = "AccidentalInput/BumpDuringPendingMoveAnimation";
    test_support::BoundedHintSession fixture(Config());
    auto& board = fixture.session;
    Check(Ready(board), JOURNEY, "production startup/import completes");
    const uint64_t lifted = BASE & ~(UINT64_C(1) << A2);
    const uint64_t placed = lifted | (UINT64_C(1) << A3);
    Check(Observe(board, lifted) && Observe(board, placed) &&
              board.Advance(220U) == VIRTUAL_LITCH_OK &&
              board.UndoHistoryMoveCount() == 0U, JOURNEY,
          "intentional move promptly reaches pending confirmation and animation");
    Check(Observe(board, placed & ~(UINT64_C(1) << G8)) &&
              !Rainbow(board, G8) && !RedFault(board), JOURNEY,
          "brief unrelated bump during animation remains locally quiet");
    Log(board, JOURNEY, "bump-g8-during-animation", G8, "replace-g8");
    Check(board.Advance(180U) == VIRTUAL_LITCH_OK && Observe(board, placed) &&
              board.State().human_illegal_mask == 0ULL && Fen(board) == FEN &&
              board.UndoHistoryMoveCount() == 0U && !RedFault(board), JOURNEY,
          "replacement recovers without accepting or losing the pending move");
    Log(board, JOURNEY, "replace-g8", G8, "Circle-or-relift-a3");
}

void FreshSetupExtra() {
    constexpr const char* JOURNEY = "AccidentalInput/FreshSetupExtra";
    test_support::BoundedHintSession fixture(Config());
    auto& board = fixture.session;
    const uint64_t incomplete = OPENING & ~(UINT64_C(1) << 52U);
    constexpr uint8_t EXTRA = 41U;
    const uint64_t withDuckCandidate = incomplete | (UINT64_C(1) << A3);
    Check(board.Reset() == VIRTUAL_LITCH_OK && Observe(board, incomplete) &&
              board.Advance(200U) == VIRTUAL_LITCH_OK, JOURNEY,
          "fresh setup retains its ordinary missing-piece guidance");
    // A single middle-board extra is deliberately a provisional Duck, even
    // before all chess pieces are placed. Keep that identity; bump a second.
    Check(Observe(board, withDuckCandidate) &&
              board.Advance(200U) == VIRTUAL_LITCH_OK && !RedFault(board),
          JOURNEY, "the provisional Duck is not mislabeled as correction");
    Check(Observe(board, withDuckCandidate | (UINT64_C(1) << EXTRA)) &&
              !Rainbow(board, EXTRA) && !RedFault(board), JOURNEY,
          "an accidental extra during setup does not immediately flash a fault");
    Log(board, JOURNEY, "extra-b3", EXTRA, "remove-b3-then-place-e2");
    Check(board.Advance(300U) == VIRTUAL_LITCH_OK && Amber(board, EXTRA) &&
              !RedFault(board), JOURNEY, "setup extra gets the same gentle local cue");
    Check(board.Advance(1200U) == VIRTUAL_LITCH_OK && Rainbow(board, EXTRA) &&
              RedFault(board), JOURNEY, "persistent setup extra gets clear strong correction");
    Check(Observe(board, withDuckCandidate) && !RedFault(board) &&
              board.UndoHistoryMoveCount() == 0U, JOURNEY,
          "removing the extra restores calm missing-piece setup, not a game move");
    Log(board, JOURNEY, "remove-b3", EXTRA, "place-e2");
}

} // namespace

int main() {
    using modules::CorrectionFeedback;
    Check(modules::CorrectionFeedbackForElapsed(299U) == CorrectionFeedback::Quiet &&
              modules::CorrectionFeedbackForElapsed(300U) == CorrectionFeedback::Gentle &&
              modules::CorrectionFeedbackForElapsed(1499U) == CorrectionFeedback::Gentle &&
              modules::CorrectionFeedbackForElapsed(1500U) == CorrectionFeedback::Strong,
          "AccidentalInput/TimingPolicy", "exact presentation boundaries");
    RapidReplace();
    SustainedMismatch(false);
    SustainedMismatch(true);
    BumpDuringAnimation();
    FreshSetupExtra();
    std::printf("[ACCIDENTAL_INPUT_JOURNEY] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
