#include "bounded_hint_engine.hpp"
#include <array>
#include <cstdio>
#include <cstring>

#include "virtual_litch_session.hpp"
#include "modules/player_end_palette.hpp"
#include "modules/rgb.hpp"
#include "modules/settle_animation_setting.hpp"
#include "modules/ui_color_wheel.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace {

constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t E2 = 52U;
constexpr uint8_t E4 = 36U;
constexpr uint8_t D4 = 35U;
constexpr uint8_t D2 = 51U;
constexpr uint64_t LIFTED = OPENING & ~(UINT64_C(1) << E2);
constexpr uint64_t PLACED = LIFTED | (UINT64_C(1) << E4);
static_assert(static_cast<uint8_t>(modules::SettleAnimationStyle::LandingFlash) == 0U &&
              static_cast<uint8_t>(modules::SettleAnimationStyle::MiniOrbit) == 1U &&
              static_cast<uint8_t>(modules::SettleAnimationStyle::Ripple) == 2U &&
              static_cast<uint8_t>(modules::SettleAnimationStyle::None) == 3U,
              "None appends without renumbering deployed placement styles");
int g_failures = 0;

void Check(const bool ok, const char* journey, const char* message) {
    if (!ok) {
        ++g_failures;
        std::printf("[FAIL] %s: %s\n", journey, message);
    }
}

bool Equal(const virtual_litch_color a, const virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}

bool SameHue(const virtual_litch_color expected, const virtual_litch_color actual) {
    const uint8_t a[]{expected.r, expected.g, expected.b};
    const uint8_t b[]{actual.r, actual.g, actual.b};
    for (uint8_t i = 0U; i < 3U; ++i) {
        if ((a[i] == 0U) != (b[i] == 0U)) return false;
        for (uint8_t j = 0U; j < 3U; ++j) {
            const int difference = static_cast<int>(a[i]) * b[j] - static_cast<int>(a[j]) * b[i];
            if (difference < -510 || difference > 510) return false;
        }
    }
    return actual.r != 0U || actual.g != 0U || actual.b != 0U;
}

bool Yellow(const virtual_litch_color color) {
    return color.r != 0U && color.r == color.g && color.b == 0U &&
           color.w == 0U;
}

virtual_litch_config Config() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    return config;
}

bool Set(runtime::VirtualLitchSession& board, const uint16_t id,
         const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return board.MutateSettingJson(id, key, value, outcome) == VIRTUAL_LITCH_OK &&
           (outcome.status == modules::ControlMutationStatus::Applied ||
            outcome.status == modules::ControlMutationStatus::Noop) &&
           outcome.reason == modules::ControlMutationReason::None;
}

bool Ready(runtime::VirtualLitchSession& board) {
    return board.Reset() == VIRTUAL_LITCH_OK &&
           board.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
           board.Advance(200U) == VIRTUAL_LITCH_OK &&
           board.State().expected_occupied_mask == OPENING &&
           board.State().interaction_phase ==
               static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle);
}

bool Place(runtime::VirtualLitchSession& board) {
    return board.ObserveHallMask(LIFTED) == VIRTUAL_LITCH_OK &&
           board.ObserveHallMask(PLACED) == VIRTUAL_LITCH_OK;
}

bool Pending(const runtime::VirtualLitchSession& board) {
    const auto& state = board.State();
    return state.physical_occupied_mask == PLACED &&
           state.expected_occupied_mask == OPENING &&
           state.side_to_move_black == 0U && board.UndoHistoryMoveCount() == 0U;
}

void Log(const runtime::VirtualLitchSession& board, const char* journey,
         const char* input, const char* next) {
    const auto& state = board.State();
    const auto& destination = board.SemanticFrame().board[E4];
    const auto& neighbor = board.SemanticFrame().board[D4];
    const auto home = board.HomeStateLine();
    std::printf("[SETTLE_JOURNEY] %s t=%lu input=%s occupancy=%016llx "
                "expected=%016llx phase=%s quick_setup=%u history=%u "
                "text=%.*s e4_square=%u/%u/%u e4_piece=%u/%u/%u "
                "d4_square=%u/%u/%u next=%s home=%s\n",
                journey, static_cast<unsigned long>(state.now_ms), input,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                runtime::ChessInteractionPhaseToString(
                    static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                board.QuickSettingsActive() ? 1U : 0U, board.UndoHistoryMoveCount(),
                static_cast<int>(board.DisplayText().size()), board.DisplayText().data(),
                destination[0].r, destination[0].g, destination[0].b,
                destination[1].r, destination[1].g, destination[1].b,
                neighbor[0].r, neighbor[0].g, neighbor[0].b, next, home.CStr());
}

struct EffectSamples {
    virtual_litch_color destination80{};
    virtual_litch_color destination340{};
    virtual_litch_color neighbor220{};
    virtual_litch_color restoreNeighbor220{};
};

EffectSamples PlacementEffect(const char* style, const bool minimal) {
    char journey[64]{};
    std::snprintf(journey, sizeof(journey), "placement-%s-minimal-%u",
                  style == nullptr ? "default" : style, minimal ? 1U : 0U);
    test_support::BoundedHintSession boardFixture(Config());
    auto& board = boardFixture.session;
    bool initialized = Ready(board) &&
        Set(board, 580U, "MOVE_CONFIRM", "\"press_circle_to_confirm\"") &&
        Set(board, 581U, "MINIMAL_ANIMATIONS", minimal ? "true" : "false");
    if (style != nullptr) {
        char json[16]{};
        std::snprintf(json, sizeof(json), "\"%s\"", style);
        initialized = initialized && Set(board, 582U, "SETTLE_ANIM_STYLE", json);
    }
    Check(initialized, journey, "enumerated settings are acknowledged before physical input");
    if (!initialized) { return {}; }
    const auto ownerColor = board.SemanticFrame().board[E2][1];
    const auto sourceBase = board.SemanticFrame().board[E2][0];
    const auto neighborBase = board.SemanticFrame().board[D4][0];
    const auto sourceNeighborBase = board.SemanticFrame().board[D2][0];
    const bool placedImmediately = Place(board);
    Check(placedImmediately &&
              SameHue(ownerColor, board.SemanticFrame().board[E4][1]) &&
              !Yellow(board.SemanticFrame().board[E4][1]),
          journey,
          "the first rendered destination frame stays the mover color and can never be a provisional Duck");
    const bool placed = placedImmediately &&
                        board.Advance(80U) == VIRTUAL_LITCH_OK;
    Check(placed && Pending(board), journey, "destination feedback cannot commit an O-confirm move");
    Check(SameHue(ownerColor, board.SemanticFrame().board[E4][1]), journey,
          "settled piece keeps the mover's occupied-piece color");
    EffectSamples samples;
    samples.destination80 = board.SemanticFrame().board[E4][0];
    Check(board.Advance(140U) == VIRTUAL_LITCH_OK && Pending(board), journey,
          "placement event cooperates with baseline progression through 220ms");
    samples.neighbor220 = board.SemanticFrame().board[D4][0];
    if (minimal || style == nullptr || std::strcmp(style, "ripple") != 0) {
        Check(Equal(neighborBase, samples.neighbor220), journey,
              "local placement feedback leaves the adjacent square unchanged");
    }

    Log(board, journey, "place-e4+220ms", "Circle-or-relift-e4");
    Check(board.Advance(120U) == VIRTUAL_LITCH_OK && Pending(board), journey,
          "visual samples leave confirmation authority unchanged");
    samples.destination340 = board.SemanticFrame().board[E4][0];
    Check(SameHue(ownerColor, board.SemanticFrame().board[E4][1]), journey,
          "animation never pulses the occupied piece off");

    modules::ControlMutationOutcome rejected;
    Check(board.MutateSettingJson(583U, "SETTLE_ANIM_STYLE", "\"splash\"", rejected) ==
                  VIRTUAL_LITCH_OK &&
              rejected.status == modules::ControlMutationStatus::Rejected &&
              rejected.reason == modules::ControlMutationReason::InvalidEnumValue && Pending(board),
          journey, "invalid effect is rejected without ending the pending move");

    Check(board.ObserveHallMask(LIFTED) == VIRTUAL_LITCH_OK &&
              board.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
              board.State().selected_square == E2 &&
              (board.State().valid_destination_mask & (UINT64_C(1) << E4)) != 0ULL,
          journey, "destination relift cancels back to the original legal placement");
    const auto legalTarget = board.SemanticFrame().board[E4][0];
    Check(legalTarget.b > legalTarget.r && legalTarget.b > legalTarget.g, journey,
          "legal blue destination guidance takes priority after relift");
    Log(board, journey, "relift-e4", "return-e2-or-place-legal-destination");
    Check(board.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
              board.Advance(220U) == VIRTUAL_LITCH_OK &&
              board.State().expected_occupied_mask == OPENING &&
              board.UndoHistoryMoveCount() == 0U &&
              board.State().human_illegal_mask == 0ULL &&
              board.State().correction_mask == 0ULL,
          journey, "source return recovers without history or correction debt");
    samples.restoreNeighbor220 = board.SemanticFrame().board[D2][0];
    Log(board, journey, "return-e2+220ms", "lift-White-piece");
    Check(board.Advance(340U) == VIRTUAL_LITCH_OK &&
              Equal(sourceBase, board.SemanticFrame().board[E2][0]) &&
              Equal(sourceNeighborBase, board.SemanticFrame().board[D2][0]),
          journey, "restore acknowledgement expires by the existing 560ms boundary");
    Log(board, journey, "restore+560ms", "lift-White-piece");
    Check(Place(board) && board.Advance(80U) == VIRTUAL_LITCH_OK && Pending(board) &&
              Equal(samples.destination80, board.SemanticFrame().board[E4][0]),
          journey, "a new physical placement starts one fresh event and fresh settling progression");
    return samples;
}

void AutomaticSettleNextPickup() {
    constexpr const char* journey = "automatic-settle-next-player-pickup";
    constexpr uint8_t D7 = 11U;
    constexpr uint8_t D5 = 27U;
    const uint64_t blackLifted = PLACED & ~(UINT64_C(1) << D7);
    test_support::BoundedHintSession fixture(Config());
    auto& board = fixture.session;
    const bool ready = Ready(board) &&
        Set(board, 598U, "MOVE_CONFIRM", "\"settle_2s\"");
    const auto white = board.SemanticFrame().board[E2][1];
    const auto destinationBackground = board.SemanticFrame().board[D5][0U];
    Check(ready && Place(board) &&
              board.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
              SameHue(board.SemanticFrame().board[E4][1], white) &&
              !Yellow(board.SemanticFrame().board[E4][1]),
          journey,
          "E2-E4 enters ordinary settle with a white piece and no Duck-yellow frame");
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(blackLifted) == VIRTUAL_LITCH_OK &&
              board.State().expected_occupied_mask == PLACED &&
              board.State().side_to_move_black == 1U &&
              board.UndoHistoryMoveCount() == 1U &&
              board.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
              board.State().selected_square == D7 &&
              board.State().human_illegal_mask == 0ULL &&
              board.State().correction_mask == 0ULL,
          journey,
          "the next legal pickup accepts E2-E4 once and owns guidance without correction");
    const auto expectedRgb = modules::ScaleRgbSaturatingBy256(
        modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(
            modules::player_end_palette::P2_END_DEFAULT_WHEEL + 16U)),
        320U);
    const virtual_litch_color expectedTarget{
        expectedRgb.r, expectedRgb.g, expectedRgb.b, 0U};
    const auto& target = board.SemanticFrame().board[D5];
    Check(Equal(target[0U], destinationBackground) && Equal(target[0U], target[2U]) &&
              !Yellow(target[0U]) &&
              Equal(target[1U], virtual_litch_color{}),
          journey,
          "the next player's legal d5 retains its background before the reveal without Duck guidance");
    Check(board.Advance(220U) == VIRTUAL_LITCH_OK &&
              Equal(board.SemanticFrame().board[D5][0U], expectedTarget) &&
              Equal(board.SemanticFrame().board[D5][2U], expectedTarget),
          journey, "the held destination reaches the exact configured P2 color after the reveal");
}

bool ButtonLevel(runtime::VirtualLitchSession& board, const uint8_t owner,
                 const runtime::VirtualLitchPhysicalButton button, const bool down) {
    // The existing raw native adapter debounces ordinary buttons at 20ms.
    return board.SetButtonDown(owner, button, down) == VIRTUAL_LITCH_OK &&
           board.Advance(20U) == VIRTUAL_LITCH_OK;
}

void CircleConfirmation(const uint8_t owner) {
    const char* journey = owner == VIRTUAL_LITCH_OWNER_P1
        ? "Center-Litchcraft-X-Circle-P1" : "Center-Litchcraft-X-Circle-P2";
    test_support::BoundedHintSession boardFixture(Config());
    auto& board = boardFixture.session;
    Check(Ready(board) &&
              Set(board, 590U, "MOVE_CONFIRM", "\"press_circle_to_confirm\"") &&
              Place(board) && board.Advance(2200U) == VIRTUAL_LITCH_OK && Pending(board),
          journey, "O-confirm remains pending beyond the ordinary two-second policy");
    Check(ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::Press, true) &&
              ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::Press, false) &&
              !board.QuickSettingsActive() && !board.Litchcraft().IsActive() &&
              Pending(board),
          journey, "Center tap cannot enter Litchcraft or confirm an in-progress move");
    Log(board, journey, "Center-press-release", "X-hold-for-Quick-Setup");
    Check(ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::X, true) &&
              board.Advance(1000U) == VIRTUAL_LITCH_OK &&
              board.QuickSettingsActive() && Pending(board),
          journey, "X hold remains the Quick Setup entry without consuming the pending move");
    Check(ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::X, false) &&
              board.QuickSettingsActive() && Pending(board),
          journey, "releasing the X hold leaves Quick Setup open for adjustment");
    Check(ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::X, true) &&
              !board.QuickSettingsActive() &&
              ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::X, false) &&
              Pending(board) &&
              !board.IsUndoActionPending(),
          journey, "X tap closes Quick Setup and preserves the pending move");
    Log(board, journey, "X-hold-release-tap", "Circle-to-confirm");
    const bool hintBefore = board.HintDisplayRequested();
    Check(ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::O, true),
          journey, "raw Circle press is admitted");
    Check(ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::O, false),
          journey, "Circle release resolves arbitration and accepts confirmation");
    const uint32_t confirmAt = board.State().now_ms;
    Check(board.Advance(999U) == VIRTUAL_LITCH_OK && Pending(board) &&
              board.State().now_ms == confirmAt + 999U,
          journey, "Circle starts the existing 1000ms window and cannot finalize early");
    Log(board, journey, "accepted-Circle+999ms", "wait-1ms");
    Check(board.Advance(1U) == VIRTUAL_LITCH_OK &&
              board.State().expected_occupied_mask == PLACED &&
              board.State().side_to_move_black == 1U &&
              board.UndoHistoryMoveCount() == 1U &&
              !board.QuickSettingsActive() && board.HintDisplayRequested() == hintBefore,
          journey, "Circle finalizes exactly once at 1000ms without invoking Hint");
    Log(board, journey, "accepted-Circle+1000ms", "lift-Black-piece");
}

void CircleAnimationNextPickup(const uint8_t owner, const uint32_t pickupDelayMs) {
    char journey[64]{};
    std::snprintf(journey, sizeof(journey), "Circle-animation-next-pickup-P%u-%lums",
                  static_cast<unsigned>(owner), static_cast<unsigned long>(pickupDelayMs));
    constexpr uint8_t D7 = 11U;
    constexpr uint8_t D6 = 19U;
    constexpr uint8_t D5 = 27U;
    const uint64_t blackLifted = PLACED & ~(UINT64_C(1) << D7);
    const uint64_t blackPlaced = blackLifted | (UINT64_C(1) << D5);
    test_support::BoundedHintSession boardFixture(Config());
    auto& board = boardFixture.session;
    Check(Ready(board) &&
              Set(board, 595U, "MOVE_CONFIRM", "\"press_circle_to_confirm\"") &&
              Place(board) &&
              ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::O, true) &&
              ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::O, false),
          journey, "Circle accepts the first move before the next pickup");
    const uint32_t confirmedAt = board.State().now_ms;
    Check(board.Advance(pickupDelayMs) == VIRTUAL_LITCH_OK &&
              Pending(board) && board.State().now_ms == confirmedAt + pickupDelayMs,
          journey, "first move remains in its accepted animation until physical input");
    Check(board.ObserveHallMask(blackLifted) == VIRTUAL_LITCH_OK &&
              board.State().expected_occupied_mask == PLACED &&
              board.State().side_to_move_black == 1U &&
              board.UndoHistoryMoveCount() == 1U &&
              board.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
              board.State().selected_square == D7 &&
              (board.State().valid_destination_mask &
                  ((UINT64_C(1) << D6) | (UINT64_C(1) << D5))) ==
                  ((UINT64_C(1) << D6) | (UINT64_C(1) << D5)) &&
              board.State().human_illegal_mask == 0ULL &&
              board.State().correction_mask == 0ULL,
          journey, "next pickup finalizes the accepted move once and immediately owns legal guidance");
    const auto destination = board.SemanticFrame().board[D5][0];
    std::printf("[CIRCLE_NEXT_PICKUP] %s selected=%u legal=%016llx "
                "illegal=%016llx correction=%016llx d5=%u/%u/%u\n",
                journey, board.State().selected_square,
                static_cast<unsigned long long>(board.State().valid_destination_mask),
                static_cast<unsigned long long>(board.State().human_illegal_mask),
                static_cast<unsigned long long>(board.State().correction_mask),
                destination.r, destination.g, destination.b);
    Log(board, journey, "Circle+pickup-d7", "place-d5-or-d6-or-return-d7");
    Check(board.Advance(1200U) == VIRTUAL_LITCH_OK &&
              board.State().selected_square == D7 &&
              board.UndoHistoryMoveCount() == 1U &&
              board.State().human_illegal_mask == 0ULL &&
              board.State().correction_mask == 0ULL,
          journey, "old animation expiry cannot disturb the next held piece");
    Check(board.ObserveHallMask(blackPlaced) == VIRTUAL_LITCH_OK &&
              board.Advance(1200U) == VIRTUAL_LITCH_OK &&
              board.State().expected_occupied_mask == PLACED &&
              board.UndoHistoryMoveCount() == 1U,
          journey, "the next move still requires its own Circle confirmation");
    Check(ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::O, true) &&
              ButtonLevel(board, owner, runtime::VirtualLitchPhysicalButton::O, false) &&
              board.Advance(1000U) == VIRTUAL_LITCH_OK &&
              board.State().expected_occupied_mask == blackPlaced &&
              board.State().side_to_move_black == 0U &&
              board.UndoHistoryMoveCount() == 2U &&
              board.State().human_illegal_mask == 0ULL &&
              board.State().correction_mask == 0ULL,
          journey, "both legal moves complete exactly once without correction debt");
    Log(board, journey, "confirm-d7d5+1000ms", "lift-White-piece");

    test_support::BoundedHintSession baselineFixture(Config());
    auto& baseline = baselineFixture.session;
    Check(Ready(baseline) &&
              Set(baseline, 596U, "MOVE_CONFIRM", "\"press_circle_to_confirm\"") &&
              Place(baseline) &&
              ButtonLevel(baseline, owner, runtime::VirtualLitchPhysicalButton::O, true) &&
              ButtonLevel(baseline, owner, runtime::VirtualLitchPhysicalButton::O, false) &&
              baseline.Advance(1500U) == VIRTUAL_LITCH_OK &&
              baseline.ObserveHallMask(blackLifted) == VIRTUAL_LITCH_OK &&
              (destination.r != 0U || destination.g != 0U || destination.b != 0U) &&
              Equal(destination, baseline.SemanticFrame().board[D5][0]),
          journey, "immediate next destination matches ordinary Black pickup guidance after animation expiry");
}

void CircleAnimationRejectsOtherChanges() {
    constexpr uint64_t D7_MASK = UINT64_C(1) << 11U;
    constexpr uint64_t C7_MASK = UINT64_C(1) << 10U;
    constexpr uint64_t D2_MASK = UINT64_C(1) << D2;
    constexpr uint64_t A3_MASK = UINT64_C(1) << 40U;
    for (const uint64_t changedOccupancy : {
             PLACED & ~D2_MASK, PLACED | A3_MASK,
             PLACED & ~(D7_MASK | C7_MASK),
             (PLACED & ~D7_MASK) | A3_MASK}) {
        const char* journey = "Circle-animation-rejects-non-pickup-transition";
        test_support::BoundedHintSession fixture(Config());
        auto& board = fixture.session;
        Check(Ready(board) &&
                  Set(board, 597U, "MOVE_CONFIRM", "\"press_circle_to_confirm\"") &&
                  Place(board) &&
                  ButtonLevel(board, VIRTUAL_LITCH_OWNER_P1,
                              runtime::VirtualLitchPhysicalButton::O, true) &&
                  ButtonLevel(board, VIRTUAL_LITCH_OWNER_P1,
                              runtime::VirtualLitchPhysicalButton::O, false) &&
                  board.ObserveHallMask(changedOccupancy) == VIRTUAL_LITCH_OK &&
                  board.Advance(1200U) == VIRTUAL_LITCH_OK &&
                  board.State().expected_occupied_mask == OPENING &&
                  board.State().side_to_move_black == 0U &&
                  board.UndoHistoryMoveCount() == 0U &&
                  board.State().human_illegal_mask != 0ULL,
              journey, "same-side lifts, placements and multi-square changes require correction before commit");
        Check(board.ObserveHallMask(PLACED) == VIRTUAL_LITCH_OK &&
                  board.Advance(1U) == VIRTUAL_LITCH_OK &&
                  board.State().expected_occupied_mask == PLACED &&
                  board.State().side_to_move_black == 1U &&
                  board.UndoHistoryMoveCount() == 1U &&
                  board.State().human_illegal_mask == 0ULL &&
                  board.State().correction_mask == 0ULL,
              journey, "restoring the accepted move completes once after its elapsed deadline");
    }
}

constexpr std::array<uint32_t, 11> PLACEMENT_SAMPLE_MS{
    0U, 80U, 220U, 340U, 560U, 1000U, 1500U, 2080U, 2220U, 2560U, 4999U};
constexpr std::array<uint32_t, 11> TWO_SECOND_SAMPLE_MS{
    0U, 80U, 220U, 340U, 560U, 800U, 1000U, 1250U, 1500U, 1800U, 1999U};
// Pre-coupling 520 -> 80ms pulse and quadratic solid blend, stretched over
// the selected interval and followed by solid (never a restarted progression).
constexpr std::array<uint8_t, 11> TWO_SECOND_RED{
    0U, 100U, 255U, 158U, 255U, 225U, 233U, 154U, 220U, 255U, 255U};
constexpr std::array<uint8_t, 11> FIVE_SECOND_RED{
    0U, 98U, 255U, 198U, 120U, 205U, 113U, 136U, 161U, 249U, 255U};
constexpr std::array<uint8_t, 11> CIRCLE_RED{
    0U, 100U, 255U, 158U, 255U, 233U, 220U, 255U, 255U, 255U, 255U};

struct PlacementFrames {
    std::array<virtual_litch_semantic_frame, PLACEMENT_SAMPLE_MS.size()> semantic{};
    std::array<virtual_litch_physical_frame, PLACEMENT_SAMPLE_MS.size()> physical{};
};

PlacementFrames PlacementSeparation(const char* style, const uint8_t outputContext,
                                    const uint8_t confirmation) {
    const bool circle = confirmation == 2U;
    const bool twoSeconds = confirmation == 0U;
    const char* policy = circle ? "Circle" : twoSeconds ? "2s" : "5s";
    char journey[80]{};
    std::snprintf(journey, sizeof(journey), "placement-separate-%s-output-%u-%s",
                  style, outputContext, policy);
    auto config = Config();
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.brightness_scale_256 = outputContext == 1U ? 8U : 86U;
    config.max_led_current_ma = outputContext == 2U ? 80U : 0U;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    char json[16]{};
    std::snprintf(json, sizeof(json), "\"%s\"", style);
    Check(Ready(board) &&
              Set(board, 620U, "MOVE_CONFIRM", circle ? "\"press_circle_to_confirm\"" : twoSeconds ? "\"settle_2s\"" : "\"settle_5s\"") &&
              Set(board, 621U, "SETTLE_ANIM_STYLE", json) &&
              Set(board, 622U, "WHITE_SQUARES_ON", "false") &&
              Set(board, 624U, "P2_END_PIECE_WHEEL", "85") &&
              Set(board, 623U, "P1_END_PIECE_WHEEL", "0") &&
              Place(board),
          journey, "acknowledged settings and raw Hall placement enter pending confirmation");
    const auto piece = board.SemanticFrame().board[E4][1];
    const auto& samples = twoSeconds ? TWO_SECOND_SAMPLE_MS : PLACEMENT_SAMPLE_MS;
    const auto& expectedRed = circle ? CIRCLE_RED : twoSeconds ? TWO_SECOND_RED : FIVE_SECOND_RED;
    PlacementFrames result;
    uint32_t age = 0U;
    for (size_t i = 0U; i < samples.size(); ++i) {
        const uint32_t sample = samples[i];
        Check(board.Advance(sample - age) == VIRTUAL_LITCH_OK && Pending(board) &&
                  board.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
                  Equal(piece, board.SemanticFrame().board[E4][1]),
              journey, "presentation preserves piece identity and cannot confirm the move");
        age = sample;
        result.semantic[i] = board.SemanticFrame();
        result.physical[i] = board.PhysicalFrame();
        Check(Equal(board.SemanticFrame().board[E4][0], board.SemanticFrame().board[E4][2]) &&
                  Equal(board.PhysicalFrame().pixels[4U * E4], board.PhysicalFrame().pixels[4U * E4 + 2U]),
              journey, "both mapped side rails retain the same cooperating placement and settling progression");
        Check(outputContext != 2U || board.PhysicalFrame().applied_led_current_ma <= 80U,
              journey, "placement and settling remain inside the mapped current budget");
        if (std::strcmp(style, "none") == 0) {
            const auto side = board.SemanticFrame().board[E4][0];
            Check(side.r == expectedRed[i] && side.g == 0U && side.b == 0U,
                  journey, "None preserves the exact progressive baseline from first placement to deadline");
        }
        Log(board, journey, "timed-placement-frame", "wait-for-deadline-or-Circle; relift-cancels");
        const auto pixel = board.PhysicalFrame().pixels[4U * E4];
        std::printf("[PLACEMENT_SEPARATION] style=%s policy=%s age=%u output=%u mapped_e4=%u/%u/%u\n",
                    style, policy, sample, outputContext, pixel.r, pixel.g, pixel.b);
    }
    if (circle) {
        Check(board.Advance(1501U) == VIRTUAL_LITCH_OK && Pending(board) &&
                  board.SemanticFrame().board[E4][0].r == 255U &&
                  ButtonLevel(board, VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true) &&
                  ButtonLevel(board, VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false) &&
                  board.Advance(80U) == VIRTUAL_LITCH_OK && Pending(board) &&
                  board.SemanticFrame().board[E4][0].r == 255U &&
                  board.SemanticFrame().board[D4][0].r == 0U &&
                  board.Advance(919U) == VIRTUAL_LITCH_OK && Pending(board),
              journey, "late Circle keeps solid settling and cannot replay the local or radial placement event");
    }
    Check(board.Advance(1U) == VIRTUAL_LITCH_OK && board.UndoHistoryMoveCount() == 1U &&
              board.State().expected_occupied_mask == PLACED,
          journey, "the unchanged confirmation deadline commits once");
    return result;
}

void EarlyCirclePreservesPlacement(const char* style, const PlacementFrames& reference) {
    const char* journey = "early-Circle-preserves-placement-event-age";
    auto config = Config();
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.brightness_scale_256 = 86U;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    char json[16]{};
    std::snprintf(json, sizeof(json), "\"%s\"", style);
    Check(Ready(board) && Set(board, 630U, "MOVE_CONFIRM", "\"press_circle_to_confirm\"") &&
              Set(board, 631U, "SETTLE_ANIM_STYLE", json) &&
              Set(board, 632U, "WHITE_SQUARES_ON", "false") &&
              Set(board, 633U, "P2_END_PIECE_WHEEL", "85") &&
              Set(board, 634U, "P1_END_PIECE_WHEEL", "0") && Place(board) &&
              board.Advance(80U) == VIRTUAL_LITCH_OK &&
              ButtonLevel(board, VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true) &&
              ButtonLevel(board, VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false),
          journey, "raw Circle is accepted while the initial placement event is still running");
    uint32_t age = 120U;
    for (size_t i = 2U; i <= 5U; ++i) {
        const uint32_t sample = PLACEMENT_SAMPLE_MS[i];
        Check(board.Advance(sample - age) == VIRTUAL_LITCH_OK && Pending(board),
              journey, "early Circle retains its full final second");
        age = sample;
        bool same = true;
        for (uint8_t square = 0U; square < 64U; ++square) {
            for (uint8_t slot = 0U; slot < 4U; ++slot) {
                same = same && Equal(board.SemanticFrame().board[square][slot], reference.semantic[i].board[square][slot]) &&
                    Equal(board.PhysicalFrame().pixels[4U * square + slot], reference.physical[i].pixels[4U * square + slot]);
            }
        }
        Check(same, journey, "Circle never restarts or replaces either the placement event or the settling progression");
        Log(board, journey, style, "wait-for-original-Circle-deadline");
    }
    Check(board.Advance(119U) == VIRTUAL_LITCH_OK && Pending(board) &&
              board.Advance(1U) == VIRTUAL_LITCH_OK && board.UndoHistoryMoveCount() == 1U &&
              board.Advance(220U) == VIRTUAL_LITCH_OK && board.SemanticFrame().board[D4][0].r == 0U,
          journey, "the accepted move commits once and completion cannot emit another placement effect");
}

void IndependentPlacementAndSettling() {
    for (const uint8_t outputContext : {0U, 1U, 2U}) {
        for (const uint8_t confirmation : {0U, 1U, 2U}) {
            const auto baseline = PlacementSeparation("none", outputContext, confirmation);
            const auto ripple = PlacementSeparation("ripple", outputContext, confirmation);
            const char* journey = "independent-placement-and-settling";
            if (confirmation == 2U && outputContext == 0U) {
                EarlyCirclePreservesPlacement("none", baseline);
                EarlyCirclePreservesPlacement("ripple", ripple);
            }
            Check(!Equal(ripple.semantic[2].board[D4][0], baseline.semantic[2].board[D4][0]) &&
                  !Equal(ripple.physical[2].pixels[4U * D4], baseline.physical[2].pixels[4U * D4]),
                  journey, "the retained Ripple recipe remains distinct from baseline in semantic and mapped output");
            for (size_t i = 4U; i < PLACEMENT_SAMPLE_MS.size(); ++i) {
                bool same = true;
                for (uint8_t square = 0U; square < 64U; ++square) {
                    for (uint8_t slot = 0U; slot < 4U; ++slot) {
                        const auto expected = baseline.semantic[i].board[square][slot];
                        const auto physicalExpected = baseline.physical[i].pixels[4U * square + slot];
                        same = same && Equal(ripple.semantic[i].board[square][slot], expected) &&
                            Equal(ripple.physical[i].pixels[4U * square + slot], physicalExpected);
                    }
                }
                Check(same, journey, "after 560ms every style is exactly baseline settling, without event replay");
            }
        }
    }
}

void V33PendingPlacement(const char* style, const bool circle,
                         const bool minimal = false) {
    char journey[96]{};
    std::snprintf(journey, sizeof(journey), "V33-pending-%s-%s-minimal-%u",
                  style, circle ? "Circle" : "automatic", minimal);
    auto config = Config();
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    const bool nearDark = minimal || std::strcmp(style, "mini_orbit") == 0 || std::strcmp(style, "rainbow_orbit") == 0;
    const bool none = std::strcmp(style, "none") == 0;
    const bool limited = std::strcmp(style, "ripple") == 0 && !minimal;
    config.brightness_scale_256 = nearDark ? 8U : 86U;
    config.max_led_current_ma = limited ? 80U : 0U;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    char json[16]{};
    std::snprintf(json, sizeof(json), "\"%s\"", style);
    const bool ready = Ready(board) &&
              Set(board, 610U, "MOVE_CONFIRM", circle ? "\"press_circle_to_confirm\"" : "\"settle_2s\"") &&
              Set(board, 611U, "SETTLE_ANIM_STYLE", json) &&
              Set(board, 612U, "MINIMAL_ANIMATIONS", minimal ? "true" : "false") &&
              Set(board, 613U, "WHITE_SQUARES_ON", "false") &&
              (!none || !minimal || Set(board, 614U, "BRIGHTNESS", "0"));
    Check(ready,
          journey, "acknowledged preferences precede raw V3.3 placement");
    if (!ready) { return; }
    Check(std::strcmp(board.SettingsForPersistenceTest().ReadValue(
              modules::settle_animation_setting::KEY,
              modules::settle_animation_setting::DEFAULT_VALUE), style) == 0 &&
              modules::settle_animation_setting::Read(board.SettingsForPersistenceTest()) ==
                  modules::settle_animation_setting::Parse(style),
          journey, "acknowledged placement style reads back from the production settings owner");
    const auto owner = board.SemanticFrame().board[E2][1];
    const auto physical = [&](const uint8_t square, const uint8_t slot) {
        return board.PhysicalFrame().pixels[4U * square + slot];
    };
    const auto maximum = [](const virtual_litch_color color) {
        return color.r > color.g ? (color.r > color.b ? color.r : color.b)
                                 : (color.g > color.b ? color.g : color.b);
    };
    const auto checkpoint = [&](const char* stage, const char* next) {
        Log(board, journey, stage, next);
        const auto rail = physical(E4, 0U);
        const auto center = physical(E4, 1U);
        const auto& frame = board.PhysicalFrame();
        std::printf("[V33_SETTLE_OUTPUT] run=%s stage=%s physical_rail=%u,%u,%u "
                    "physical_piece=%u,%u,%u scale=%u current=%u limited=%u\n",
                    journey, stage, rail.r, rail.g, rail.b, center.r, center.g, center.b,
                    frame.applied_brightness_scale_65535, frame.applied_led_current_ma,
                    frame.power_limited);
    };
    Check(Place(board), journey, "raw e2 pickup and e4 placement enter settling");
    if (circle && (none || std::strcmp(style, "mini_orbit") == 0) && !minimal) {
        const bool relifted = board.ObserveHallMask(LIFTED) == VIRTUAL_LITCH_OK;
        Check(relifted &&
                  board.State().valid_destination_mask != 0ULL &&
                  maximum(physical(E4, 0U)) != 0U,
              journey, "destination relift immediately restores required legal placement guidance");
        constexpr uint8_t F4 = 37U;
        constexpr uint64_t WRONG = UINT64_C(1) << F4;
        Check(board.ObserveHallMask(LIFTED | WRONG) == VIRTUAL_LITCH_OK &&
                  (board.State().human_illegal_mask & WRONG) != 0ULL &&
                  board.UndoHistoryMoveCount() == 0U,
              journey, "wrong placement is rejected immediately without committing the move");
        Check(board.Advance(299U) == VIRTUAL_LITCH_OK &&
                  maximum(physical(F4, 0U)) == 0U && maximum(physical(F4, 2U)) == 0U &&
                  (board.State().human_illegal_mask & WRONG) != 0ULL,
              journey, "rejected wrong placement retains the ordinary 299ms quiet correction interval");
        Check(board.Advance(1U) == VIRTUAL_LITCH_OK &&
                  maximum(physical(F4, 0U)) != 0U && maximum(physical(F4, 2U)) != 0U &&
                  board.UndoHistoryMoveCount() == 0U,
              journey, "required mapped correction bracket appears at 300ms even with placement effect None");
        const auto wrongLeft = physical(F4, 0U);
        const auto wrongRight = physical(F4, 2U);
        std::printf("[SETTLE_RECOVERY] run=%s t=%lu wrong_f4_rails=%u/%u/%u,%u/%u/%u "
                    "illegal=%016llx history=%u next=remove-f4-then-return-e2\n",
                    journey, static_cast<unsigned long>(board.State().now_ms),
                    wrongLeft.r, wrongLeft.g, wrongLeft.b, wrongRight.r, wrongRight.g, wrongRight.b,
                    static_cast<unsigned long long>(board.State().human_illegal_mask), board.UndoHistoryMoveCount());
        checkpoint("wrong-f4-plus-300ms", "remove-f4-then-return-e2");
        Check(board.ObserveHallMask(LIFTED) == VIRTUAL_LITCH_OK,
              journey, "removing the wrong placement reaches raw Hall recovery");
        Check(board.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK,
              journey, "returning the source reaches raw Hall recovery");
        Check(board.Advance(560U) == VIRTUAL_LITCH_OK &&
                  board.State().physical_occupied_mask == OPENING &&
                  board.State().expected_occupied_mask == OPENING &&
                  board.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
                  board.State().human_illegal_mask == 0ULL && board.State().correction_mask == 0ULL &&
                  board.UndoHistoryMoveCount() == 0U,
              journey, "relift and wrong placement recover through source return without committing");
        Check(Place(board) && Pending(board), journey, "recovered raw e2-e4 placement restarts pending confirmation");
        if (!Pending(board)) { return; }
    }
    uint32_t age = 0U;
    for (const uint32_t sample : {0U, 80U, 220U, 340U, 560U, 1000U, 1500U, 1999U}) {
        Check(board.Advance(sample - age) == VIRTUAL_LITCH_OK && Pending(board),
              journey, "visual feedback cannot advance automatic or Circle confirmation early");
        age = sample;
        const auto side = board.SemanticFrame().board[E4][0];
        Check(SameHue(owner, board.SemanticFrame().board[E4][1]) &&
                  SameHue(owner, physical(E4, 1U)) && SameHue(owner, physical(E4, 3U)),
              journey, "every placement style retains both occupied piece emitters in their owner hue");
        Check(sample == 0U ||
                  (maximum(side) != 0U && maximum(physical(E4, 0U)) != 0U &&
                   maximum(physical(E4, 2U)) != 0U),
              journey, "all styles retain baseline settling through semantic and mapped output");
        Check(!limited || board.PhysicalFrame().applied_led_current_ma <= 80U,
              journey, "strong settling feedback respects the physical current budget");
        if (minimal || std::strcmp(style, "landing_flash") == 0 || std::strcmp(style, "none") == 0) {
            Check(maximum(physical(D4, 0U)) == 0U, journey,
                  "Landing flash, None and Minimal stay local during settling");
        }
        if (sample == 0U || sample == 220U || sample == 1999U) checkpoint("pending", "wait-or-relift");
    }
    if (circle) {
        Check(board.Advance(501U) == VIRTUAL_LITCH_OK && Pending(board) &&
                  ButtonLevel(board, VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true) &&
                  ButtonLevel(board, VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false) &&
                  board.Advance(999U) == VIRTUAL_LITCH_OK && Pending(board),
              journey, "untimed placement waits for real Circle and its full final second");
        checkpoint("Circle-plus-999ms", "wait-one-millisecond");
    }
    Check(board.Advance(1U) == VIRTUAL_LITCH_OK && board.UndoHistoryMoveCount() == 1U &&
              board.State().expected_occupied_mask == PLACED && board.State().side_to_move_black != 0U,
          journey, "deadline commits exactly once and hands over the turn");
    age = 0U;
    for (const uint32_t sample : {0U, 80U, 220U, 340U, 560U}) {
        Check(board.Advance(sample - age) == VIRTUAL_LITCH_OK &&
                  maximum(physical(D4, 0U)) == 0U && maximum(physical(D4, 2U)) == 0U &&
                  board.UndoHistoryMoveCount() == 1U,
              journey, "completed settling never starts a second ripple on its neighboring square");
        age = sample;
        if (sample == 220U) checkpoint("completed-plus-220ms", "play-Black-move");
    }
    constexpr uint8_t D7 = 11U;
    const uint32_t restorationMs = modules::settle_animation_setting::DurationMs(
        modules::settle_animation_setting::Parse(style), minimal);
    Check(board.ObserveHallMask(PLACED & ~(UINT64_C(1) << D7)) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(PLACED) == VIRTUAL_LITCH_OK &&
              board.Advance(80U) == VIRTUAL_LITCH_OK &&
              (maximum(board.SemanticFrame().board[D7][0]) != 0U) == !none &&
              maximum(physical(D7, 1U)) != 0U &&
              board.UndoHistoryMoveCount() == 1U && board.State().human_illegal_mask == 0ULL,
          journey, "source return acknowledges the selected style while preserving piece identity and move authority");
    Check(board.Advance(restorationMs > 80U ? restorationMs - 80U : 0U) == VIRTUAL_LITCH_OK &&
              maximum(board.SemanticFrame().board[D7][0]) == 0U && maximum(physical(D7, 1U)) != 0U &&
              board.UndoHistoryMoveCount() == 1U && board.State().human_illegal_mask == 0ULL,
          journey, "source-return feedback expires at its selected style lifetime without changing the next move");
}

} // namespace

int RunSettleEffectsVirtualLitchJourneys() {
    IndependentPlacementAndSettling();
    for (const char* style : {"landing_flash", "mini_orbit", "rainbow_orbit", "ripple"}) {
        V33PendingPlacement(style, false);
        V33PendingPlacement(style, true);
    }
    V33PendingPlacement("ripple", true, true);
    V33PendingPlacement("none", false);
    V33PendingPlacement("none", true);
    V33PendingPlacement("none", true, true);
    const auto minimalGlow = PlacementEffect("landing_flash", true);
    const auto minimalPulse = PlacementEffect("mini_orbit", true);
    const auto minimalRipple = PlacementEffect("ripple", true);
    Check(Equal(minimalGlow.destination340, minimalPulse.destination340) &&
              Equal(minimalGlow.destination80, minimalPulse.destination80) &&
              Equal(minimalGlow.destination80, minimalRipple.destination80) &&
              Equal(minimalGlow.neighbor220, minimalRipple.neighbor220) &&
              Equal(minimalGlow.restoreNeighbor220, minimalRipple.restoreNeighbor220),
          "placement-effects", "Minimal shares still local feedback and the same independent settling baseline");
    CircleConfirmation(VIRTUAL_LITCH_OWNER_P1);
    CircleConfirmation(VIRTUAL_LITCH_OWNER_P2);
    for (const uint8_t owner : {VIRTUAL_LITCH_OWNER_P1, VIRTUAL_LITCH_OWNER_P2}) {
        CircleAnimationNextPickup(owner, 20U);
        CircleAnimationNextPickup(owner, 999U);
    }
    AutomaticSettleNextPickup();
    CircleAnimationRejectsOtherChanges();
    return g_failures;
}
