#include "bounded_hint_engine.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>

#include "virtual_litch_session.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/led_mapping.hpp"
#include "modules/orientation.hpp"
#include "modules/edge_led_constants.hpp"
#include "modules/rgb_optical_calibration.hpp"
#include "modules/rgbw_auto_map.hpp"
#include "modules/ui_color_wheel.hpp"
#include "runtime/chess_interaction_model.hpp"

bool RunMultiPieceCaptureAudit();

namespace {

constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t E2 = 52U;
constexpr uint8_t E4 = 36U;
constexpr uint8_t E7 = 12U;
constexpr uint8_t D4 = 35U;
int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] EMPTY_PIECE_LED_JOURNEY: %s\n", message);
        ++g_failures;
    }
}

uint16_t Energy(const virtual_litch_color color) {
    return static_cast<uint16_t>(color.r) + color.g + color.b + color.w;
}

const virtual_litch_color& Led(const runtime::VirtualLitchSession& session,
                                 const uint8_t square,
                                 const hardware::SquareLedSlot slot) {
    return session.SemanticFrame().board[square][static_cast<uint8_t>(slot)];
}

std::array<uint16_t, 2> OutputEnergy(const runtime::VirtualLitchSession& session,
                                     const uint8_t square,
                                     const hardware::SquareLedSlot slot = hardware::SquareLedSlot::Piece) {
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    const auto targets = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3S3, square,
        slot, rotate);
    uint16_t physicalEnergy = 0U;
    for (uint8_t index = 0U; index < targets.count; ++index) {
        physicalEnergy = std::max<uint16_t>(
            physicalEnergy,
            Energy(session.PhysicalFrame().pixels[targets.indices[index]]));
    }
    return {Energy(Led(session, square, slot)),
            physicalEnergy};
}

bool IsRedFault(const runtime::VirtualLitchSession& session) {
    for (const auto color : session.SemanticFrame().status) {
        if (color.r == 0U || color.r < color.g || color.r < color.b) {
            return false;
        }
    }
    return true;
}

bool PairLit(const runtime::VirtualLitchSession& session, const uint8_t square) {
    return Energy(Led(session, square, hardware::SquareLedSlot::Square0)) > 0U &&
           Energy(Led(session, square, hardware::SquareLedSlot::Square2)) > 0U;
}

bool Observe(runtime::VirtualLitchSession& session, const uint64_t logicalMask) {
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    uint64_t physicalMask = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((logicalMask & (UINT64_C(1) << square)) != 0ULL) {
            physicalMask |= UINT64_C(1)
                            << modules::MapSquareToPhysical(square, rotate);
        }
    }
    return session.ObserveHallMask(physicalMask) == VIRTUAL_LITCH_OK &&
           session.State().physical_occupied_mask == logicalMask;
}

bool Set(runtime::VirtualLitchSession& session,
         const uint16_t requestId,
         const char* key,
         const char* value) {
    modules::ControlMutationOutcome outcome;
    return session.MutateSettingJson(requestId, key, value, outcome) ==
               VIRTUAL_LITCH_OK &&
           outcome.status == modules::ControlMutationStatus::Applied &&
           outcome.reason == modules::ControlMutationReason::None;
}

void Checkpoint(const runtime::VirtualLitchSession& session,
                const char* stage,
                const char* outcome,
                const char* nextAction,
                const uint64_t expectedEmptyCenters = 0ULL) {
    const auto& state = session.State();
    uint64_t litEmptyCenters = 0ULL;
    uint64_t litEmptyCenterPairs = 0ULL;
    uint64_t litOccupiedCenters = 0ULL;
    uint64_t litSquarePairs = 0ULL;
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    for (uint8_t square = 0U; square < 64U; ++square) {
        const uint64_t bit = UINT64_C(1) << square;
        const auto targets = hardware::MapSemanticSquareLedTargets(
            hardware::BoardRevision::V3S3, square,
            hardware::SquareLedSlot::Piece, rotate);
        const bool semanticLit = Energy(Led(
            session, square, hardware::SquareLedSlot::Piece)) > 0U;
        bool physicalLit = false;
        bool physicalPairLit = targets.count == 2U;
        for (uint8_t index = 0U; index < targets.count; ++index) {
            const bool lit = Energy(session.PhysicalFrame().pixels[targets.indices[index]]) > 0U;
            physicalLit = physicalLit || lit;
            physicalPairLit = physicalPairLit && lit;
        }
        if ((state.physical_occupied_mask & bit) == 0ULL) {
            if (semanticLit || physicalLit) {
                litEmptyCenters |= bit;
            }
            if (semanticLit && physicalPairLit) {
                litEmptyCenterPairs |= bit;
            }
        } else if (semanticLit && physicalLit) {
            litOccupiedCenters |= bit;
        }
        if (PairLit(session, square)) {
            litSquarePairs |= bit;
        }
    }
    const auto home = session.HomeStateLine();
    const auto display = session.DisplayText();
    std::printf(
        "[EMPTY_PIECE_LED_JOURNEY] stage=%s rotate=%u first_p2=%u "
        "t=%u physical=%016llx expected=%016llx phase=%u correction=%016llx "
        "outcome=%s display=\"%.*s\" home=%s empty_centers=%016llx expected_empty_centers=%016llx "
        "occupied_centers=%016llx square_pairs=%016llx next=%s\n",
        stage, static_cast<unsigned>(session.BoardRotate()),
        static_cast<unsigned>(session.FirstPlayerIsP2()), state.now_ms,
        static_cast<unsigned long long>(state.physical_occupied_mask),
        static_cast<unsigned long long>(state.expected_occupied_mask),
        static_cast<unsigned>(state.interaction_phase),
        static_cast<unsigned long long>(state.correction_mask), outcome,
        static_cast<int>(display.size()), display.empty() ? "" : display.data(),
        home.CStr(),
        static_cast<unsigned long long>(litEmptyCenters),
        static_cast<unsigned long long>(expectedEmptyCenters),
        static_cast<unsigned long long>(litOccupiedCenters),
        static_cast<unsigned long long>(litSquarePairs), nextAction);
    Check(litEmptyCenters == expectedEmptyCenters && litEmptyCenterPairs == expectedEmptyCenters,
          "only the named movement landing or timed route has lit semantic and both mapped empty Piece LEDs");
    const auto pawn = OutputEnergy(session, E2);
    const auto king = OutputEnergy(session, 60U);
    std::printf("[SETUP_DETECTION] stage=%s t=%u "
                "e2_semantic=%u e2_mapped=%u e1_semantic=%u e1_mapped=%u\n",
                stage, state.now_ms, pawn[0], pawn[1], king[0], king[1]);
    const auto pawnSquare = OutputEnergy(session, E2, hardware::SquareLedSlot::Square0);
    const auto oppositePawnSquare = OutputEnergy(session, E7, hardware::SquareLedSlot::Square0);
    const auto whiteSquareColor = Led(session, E2, hardware::SquareLedSlot::Square0);
    const auto blackSquareColor = Led(session, E7, hardware::SquareLedSlot::Square0);
    std::printf("[SETUP_SQUARE_GUIDANCE] stage=%s e2_rgb=%u/%u/%u e2_semantic=%u e2_mapped=%u "
                "e7_rgb=%u/%u/%u e7_semantic=%u e7_mapped=%u\n",
                stage, whiteSquareColor.r, whiteSquareColor.g, whiteSquareColor.b,
                pawnSquare[0], pawnSquare[1], blackSquareColor.r, blackSquareColor.g,
                blackSquareColor.b, oppositePawnSquare[0], oppositePawnSquare[1]);
    const auto whiteBracket = OutputEnergy(session, 60U, hardware::SquareLedSlot::Square0);
    const auto blackBracket = OutputEnergy(session, 4U, hardware::SquareLedSlot::Square0);
    std::printf("[KING_LOCATION] stage=%s e1_semantic=%u e1_mapped=%u "
                "e8_semantic=%u e8_mapped=%u side_owner=%016llx history=%u\n",
                stage, whiteBracket[0], whiteBracket[1], blackBracket[0], blackBracket[1],
                static_cast<unsigned long long>(session.SidePrioritySquareMask()),
                static_cast<unsigned>(session.UndoHistoryMoveCount()));
    Check(session.DisplayText().empty(),
          "ordinary setup and correction preserve the square-led next action");
}

void CheckMissingSetupGuidance(const runtime::VirtualLitchSession& session) {
    const uint64_t missing = OPENING & ~session.State().physical_occupied_mask;
    const bool removingExtras = IsRedFault(session);
    constexpr uint64_t KINGS = (UINT64_C(1) << 60U) | (UINT64_C(1) << 4U);
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((missing & (UINT64_C(1) << square)) == 0ULL) continue;
        const auto center = OutputEnergy(session, square);
        Check(center[0] == 0U && center[1] == 0U,
              "undetected setup pieces stay black in semantic and physical output");
        if (removingExtras) continue;
        for (const auto slot : {hardware::SquareLedSlot::Square0,
                                hardware::SquareLedSlot::Square2}) {
            const auto output = OutputEnergy(session, square, slot);
            Check(output[0] > 0U && output[1] > 0U,
                  "missing opening square retains owner-color placement guidance in semantic and physical output");
        }
        if ((KINGS & (UINT64_C(1) << square)) != 0ULL) continue;
        const auto color = Led(session, square, hardware::SquareLedSlot::Square0);
        const bool p1Owner = (square >= 48U) != session.FirstPlayerIsP2();
        Check(p1Owner ? color.r > color.g && color.r > color.b
                      : color.g > color.r && color.b > color.r,
              "missing opening square uses its owning player's selected palette");
    }
}

void CheckKingPulseStrength(runtime::VirtualLitchSession& session,
                            const char* stage) {
    constexpr std::array<uint8_t, 2> KINGS{60U, 4U};
    const auto scale = [](const modules::Rgb rgb, const uint16_t gain) {
        return modules::ScaleRgbSaturatingBy256(
            modules::ScaleRgbSaturatingBy256(rgb, gain), 320U);
    };
    const auto equal = [](const virtual_litch_color actual, const modules::Rgb expected) {
        return actual.r == expected.r && actual.g == expected.g &&
               actual.b == expected.b && actual.w == 0U;
    };
    const auto& settings = session.SettingsForPersistenceTest();
    const std::array<modules::Rgb, 2> palettes{
        modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(
            settings.ReadValue("P1_PIECE_WHEEL", 0U))),
        modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(
            settings.ReadValue("P2_PIECE_WHEEL", 128U)))};
    const std::array<virtual_litch_color, 2> centers{
        Led(session, KINGS[0], hardware::SquareLedSlot::Piece),
        Led(session, KINGS[1], hardware::SquareLedSlot::Piece)};
    unsigned flashes = 0U;
    for (unsigned tick = 0U; tick < 32U; ++tick) {
        Check(session.Advance(50U) == VIRTUAL_LITCH_OK,
              "king pulse advances through production timed rendering");
        bool whiteFlash = false;
        for (size_t king = 0U; king < KINGS.size(); ++king) {
            const size_t owner = session.FirstPlayerIsP2() ? 1U - king : king;
            const auto peak = scale(palettes[owner], 512U);
            const auto rest = scale(palettes[owner], 72U);
            const auto oldPeak = scale(palettes[owner], 256U);
            const auto left = Led(session, KINGS[king], hardware::SquareLedSlot::Square0);
            const auto right = Led(session, KINGS[king], hardware::SquareLedSlot::Square2);
            const bool flash = equal(left, peak);
            Check((flash || equal(left, rest)) && equal(right, flash ? peak : rest),
                  "both king side LEDs use doubled flash gain or halved rest gain");
            Check(peak.g == 2U * oldPeak.g &&
                      (peak.r == 255U || peak.b == 255U),
                  "mixed palettes double an unsaturated channel while the dominant channel saturates");
            Check(equal(Led(session, KINGS[king], hardware::SquareLedSlot::Piece),
                        modules::Rgb{centers[king].r, centers[king].g, centers[king].b}),
                  "king center remains steady and confirms only actual occupancy");
            if (king == 0U) whiteFlash = flash;
            else Check(flash == whiteFlash, "both king owners share one cadence");
            const auto output = OutputEnergy(session, KINGS[king], hardware::SquareLedSlot::Square0);
            std::printf("[KING_PULSE_STRENGTH] stage=%s rotate=%u first_p2=%u "
                        "t=%u square=%u occupied=%u phase=%s rgb=%u,%u,%u "
                        "mapped_energy=%u applied_current_ma=%u power_limited=%u "
                        "next=place-remaining-pieces cadence_expiry=1600ms-loop\n",
                        stage, unsigned(session.BoardRotate()), unsigned(session.FirstPlayerIsP2()),
                        session.State().now_ms, unsigned(KINGS[king]),
                        unsigned((session.State().physical_occupied_mask >> KINGS[king]) & 1ULL),
                        flash ? "flash" : "rest", unsigned(left.r), unsigned(left.g), unsigned(left.b),
                        output[1], unsigned(session.PhysicalFrame().applied_led_current_ma),
                        unsigned(session.PhysicalFrame().power_limited));
        }
        if (whiteFlash) ++flashes;
    }
    Check(flashes == 12U, "a full 1600 ms loop retains exactly three 200 ms flashes");
}

void RunSetupCenterOccupancyJourney(const bool boardRotate, const bool firstPlayerIsP2) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.board_rotate = boardRotate ? 1U : 0U;
    config.first_player_is_p2 = firstPlayerIsP2 ? 1U : 0U;
    test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
    Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, 0ULL),
          "raw empty Hall observation reaches the production session");
    virtual_litch_action_request request{};
    virtual_litch_default_action_request(&request);
    request.request_id = 800U;
    request.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    request.owner = VIRTUAL_LITCH_OWNER_P1;
    modules::ControlMutationOutcome outcome;
    Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "acknowledged production New Game enters empty setup");
    Check(Set(session, 804U, "P1_PIECE_WHEEL", "0") &&
              Set(session, 805U, "P2_PIECE_WHEEL", "128"),
          "acknowledged settings choose distinct owning-player palettes");
    Check(Set(session, 804U, "P1_PIECE_WHEEL", "16") &&
              Set(session, 805U, "P2_PIECE_WHEEL", "154"),
          "acknowledged mixed palettes expose unsaturated and saturated flash channels");
    // A real piece without a magnet produces the same raw Hall observation
    // as this empty square. No occupancy is inferred from expected opening FEN.
    Check(session.Advance(2400U - session.State().now_ms % 2400U) == VIRTUAL_LITCH_OK &&
              std::string_view(session.HomeStateLine().CStr()).find("\"pm\":1") !=
                  std::string_view::npos,
          "empty setup has authoritative placement ownership before handling pieces");
    Check(session.State().expected_occupied_mask == OPENING &&
              PairLit(session, E2) && PairLit(session, 60U) && !IsRedFault(session),
          "missing opening squares guide placement while undetected centers stay black");
    CheckMissingSetupGuidance(session);
    Checkpoint(session, "empty-setup", "not-detected", "place-opening-pieces-or-check-undetected-magnet");
    CheckKingPulseStrength(session, "missing-kings");
    for (const uint32_t step : std::array<uint32_t, 6>{200U, 200U, 600U, 200U, 400U, 800U}) {
        Check(session.Advance(step) == VIRTUAL_LITCH_OK,
              "sample the existing king cadence with undetected pieces held in place");
        Check(PairLit(session, E2) && PairLit(session, 60U),
              "missing pawn square guidance survives every king flash/rest phase");
        CheckMissingSetupGuidance(session);
        Checkpoint(session, "undetected-held", "not-detected", "check-magnet-and-place-pieces");
    }

    Check(Set(session, 801U, "BOARD_SETUP_ASSISTANCE", "false") &&
              PairLit(session, 60U) && PairLit(session, E2),
          "disabling optional assistance retains ordinary king and pawn square guidance");
    Checkpoint(session, "assist-disabled", "Applied", "place-detectable-pieces");
    Check(Set(session, 802U, "BOARD_SETUP_ASSISTANCE", "true") && PairLit(session, 60U),
          "re-enabling assistance restores the stronger king bracket");
    Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                               runtime::VirtualLitchPhysicalButton::X, true) == VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                                   runtime::VirtualLitchPhysicalButton::X, false) == VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.reason == modules::ControlMutationReason::None &&
              PairLit(session, 60U),
          "raw physical X dismisses the optional bracket while retaining ordinary square guidance");
    Checkpoint(session, "assist-dismissed", "Applied", "place-detectable-pieces");
    request.request_id = 803U;
    Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied &&
              session.Advance(2400U) == VIRTUAL_LITCH_OK && PairLit(session, 60U),
          "acknowledged New Game rearms the existing king-location owner");
    Checkpoint(session, "assist-rearmed", "Applied-ack-expired", "place-detectable-pieces");

    const uint64_t partial = (UINT64_C(1) << 4U) | (UINT64_C(1) << 56U) | (UINT64_C(1) << 1U) |
                             (UINT64_C(1) << E2) | (UINT64_C(1) << 60U);
    Check(Observe(session, partial) && PairLit(session, E7),
          "real Hall detection lights occupied centers while other missing squares guide placement");
    Check(Energy(Led(session, 56U, hardware::SquareLedSlot::Piece)) > 0U &&
              Energy(Led(session, 1U, hardware::SquareLedSlot::Piece)) > 0U,
          "correct occupied pieces keep both player centers lit");
    const auto whiteCenter = Led(session, 56U, hardware::SquareLedSlot::Piece);
    const auto blackCenter = Led(session, 1U, hardware::SquareLedSlot::Piece);
    const auto placedPawn = OutputEnergy(session, E2);
    const auto placedKing = OutputEnergy(session, 60U);
    for (uint8_t output = 0U; output < 2U; ++output) {
        Check(placedPawn[output] > 0U && placedKing[output] > 0U,
              "actual pawn and king detection changes both center outputs from black to lit");
    }
    CheckMissingSetupGuidance(session);
    Checkpoint(session, "real-detection", "detected", "place-remaining-pieces");
    CheckKingPulseStrength(session, "present-kings");

    const uint64_t removed = partial & ~((UINT64_C(1) << E2) | (UINT64_C(1) << 60U));
    Check(Observe(session, removed), "raw Hall removal clears authoritative detection");
    for (uint8_t output = 0U; output < 2U; ++output) {
        Check(OutputEnergy(session, E2)[output] == 0U &&
                  OutputEnergy(session, 60U)[output] == 0U,
              "raw removal immediately returns pawn and king centers to black");
    }
    Check(PairLit(session, E2) && PairLit(session, 60U),
          "removal restores pawn square guidance and the stronger king bracket");
    CheckMissingSetupGuidance(session);
    Checkpoint(session, "setup-pieces-removed", "not-detected", "replace-pawn-and-king");
    Check(Observe(session, partial), "replacement restores detected centers");

    Check(Observe(session, partial | (UINT64_C(1) << D4)) &&
              session.SetupDuckCandidateSquare() == D4 &&
              !PairLit(session, D4) &&
              Energy(Led(session, D4, hardware::SquareLedSlot::Piece)) > 0U,
          "single row-4 extra becomes the sole provisional Vex center");
    Checkpoint(session, "partial-vex", "provisional-vex", "place-remaining-pieces-or-remove-vex");
    constexpr uint8_t EXTRA = 27U;
    Check(Observe(session, partial | (UINT64_C(1) << D4) | (UINT64_C(1) << EXTRA)) &&
              session.Advance(1500U) == VIRTUAL_LITCH_OK &&
              IsRedFault(session) && !PairLit(session, 60U),
          "second extra requires removal and suppresses optional king assistance");
    CheckMissingSetupGuidance(session);
    Checkpoint(session, "invalid-extra", "rejected-strong-correction", "remove-d5");
    Check(Observe(session, partial | (UINT64_C(1) << D4)) &&
              !IsRedFault(session) && PairLit(session, 60U),
          "removing the invalid extra restores the enabled king bracket");
    Checkpoint(session, "invalid-extra-removed", "recovered", "place-remaining-pieces-or-remove-vex");
    Check(Observe(session, partial) && PairLit(session, E7),
          "removing provisional Vex restores missing opening square guidance");
    Checkpoint(session, "extra-removed", "recovered", "place-remaining-pieces");

    Check(Observe(session, OPENING) &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.EdgeCueIntent().kind == modules::EdgeLedCueKind::SetupComplete,
          "complete setup reaches acknowledged playable boundary");
    Checkpoint(session, "complete-setup", "setup-complete", "observe-acknowledgement-expiry-play-already-enabled");
    Check(session.Advance(1801U) == VIRTUAL_LITCH_OK &&
              session.EdgeCueIntent().kind == modules::EdgeLedCueKind::None,
          "setup completion acknowledgement expires");
    Check(Energy(Led(session, 56U, hardware::SquareLedSlot::Piece)) == Energy(whiteCenter) &&
              Energy(Led(session, 1U, hardware::SquareLedSlot::Piece)) == Energy(blackCenter),
          "completion preserves ordinary occupied-piece intensity");
    Checkpoint(session, "ready", "play-ready", "lift-e2");

    const uint64_t lifted = OPENING & ~(UINT64_C(1) << E2);
    Check(Observe(session, lifted) &&
              session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::PieceLifted) &&
              (session.State().valid_destination_mask & (UINT64_C(1) << E4)) != 0ULL &&
              PairLit(session, E4),
          "legal pickup offers real engine destinations on square pairs");
    Checkpoint(session, "e2-held", "legal-pickup", "place-e4-or-return-e2");
    Check(session.Advance(220U) == VIRTUAL_LITCH_OK,
          "sample held-piece prompt");
    Checkpoint(session, "e2-held-later", "in-progress", "return-e2");
    Check(Observe(session, OPENING) &&
              session.Advance(560U) == VIRTUAL_LITCH_OK,
          "source return completes and its acknowledgement expires");
    Checkpoint(session, "pickup-cancelled", "recovered", "play-white");

    Check(Observe(session, OPENING & ~(UINT64_C(1) << E7)) &&
              session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::Correction) &&
              session.State().correction_mask == (UINT64_C(1) << E7) &&
              !IsRedFault(session),
          "wrong-side pickup is rejected immediately without a first-frame fault");
    Checkpoint(session, "wrong-side-e7", "quiet-correction", "replace-e7");
    Check(session.Advance(1500U) == VIRTUAL_LITCH_OK &&
              PairLit(session, E7) && IsRedFault(session),
          "sustained wrong-side pickup escalates to missing-square correction");
    Check(session.Advance(240U) == VIRTUAL_LITCH_OK && PairLit(session, E7),
          "missing-square rainbow remains actionable as it rotates");
    Checkpoint(session, "correction-held", "rejected-correction", "replace-e7");
    Check(Observe(session, OPENING) &&
              session.Advance(560U) == VIRTUAL_LITCH_OK &&
              session.State().correction_mask == 0ULL && !IsRedFault(session) &&
              Energy(Led(session, E7, hardware::SquareLedSlot::Piece)) > 0U,
          "replacing rejected piece clears correction and restores its center");
    Checkpoint(session, "correction-recovered", "play-ready", "play-white");

    const uint64_t moved = lifted | (UINT64_C(1) << E4);
    Check(Observe(session, lifted) && Observe(session, moved) &&
              session.Advance(2000U) == VIRTUAL_LITCH_OK &&
              session.UndoHistoryMoveCount() == 1U &&
              session.State().expected_occupied_mask == moved &&
              (session.SidePrioritySquareMask() &
               ((UINT64_C(1) << 60U) | (UINT64_C(1) << 4U))) == 0ULL,
          "first real e2-e4 commits once and retires the optional setup bracket");
    Checkpoint(session, "first-move", "committed-once-assist-cancelled", "play-black");

    Check(Set(session, 901U, "BOARD_SETUP_ASSISTANCE", "false") &&
              Observe(session, 0ULL),
          "acknowledged assist setting does not exempt emptied board centers");
    Checkpoint(session, "board-cleared", "correction", "replace-opening");
}

void RunUnrelatedOpponentLiftJourney() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    const auto bit = [](uint8_t square) { return UINT64_C(1) << square; };
    Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) &&
              session.Advance(2400U) == VIRTUAL_LITCH_OK,
          "unrelated lift journey reaches a playable opening through Hall input");
    const std::string acknowledged(session.HomeStateSnapshot().fen);
    const auto ownColor = Led(session, E2, hardware::SquareLedSlot::Piece);
    const auto opposingColor = Led(session, E7, hardware::SquareLedSlot::Piece);
    const auto checkpoint = [&](const char* stage, uint32_t age, const char* next) {
        const auto& state = session.State();
        const auto center = Led(session, E7, hardware::SquareLedSlot::Piece);
        const auto left = Led(session, E7, hardware::SquareLedSlot::Square0);
        const auto right = Led(session, E7, hardware::SquareLedSlot::Square2);
        const auto mapped = OutputEnergy(session, E7);
        const auto display = session.DisplayText();
        std::printf("[UNRELATED_OPPONENT_LIFT] stage=%s age=%u mask=%016llx expected=%016llx "
                    "phase=%u action=%u correction=%016llx history=%u display=\"%.*s\" "
                    "white=%u/%u/%u black=%u/%u/%u e7_piece=%u/%u/%u mapped_energy=%u "
                    "e7_rails=%u/%u/%u,%u/%u/%u next=%s\n",
                    stage, age, static_cast<unsigned long long>(state.physical_occupied_mask),
                    static_cast<unsigned long long>(state.expected_occupied_mask),
                    state.interaction_phase, state.required_action,
                    static_cast<unsigned long long>(state.correction_mask), session.UndoHistoryMoveCount(),
                    static_cast<int>(display.size()), display.empty() ? "" : display.data(),
                    ownColor.r, ownColor.g, ownColor.b, opposingColor.r, opposingColor.g, opposingColor.b,
                    center.r, center.g, center.b, mapped[1U],
                    left.r, left.g, left.b, right.r, right.g, right.b, next);
    };
    for (bool returnOpponentFirst : {false, true}) {
        const uint64_t ownHeld = OPENING & ~bit(E2);
        const uint64_t bothHeld = ownHeld & ~bit(E7);
        Check(Observe(session, ownHeld) &&
                  (session.State().valid_destination_mask & bit(E4)) != 0ULL &&
                  (session.State().valid_destination_mask & bit(E7)) == 0ULL,
              "e2 is lifted with a real legal e4 destination and no e7 capture");
        Check(Observe(session, bothHeld), "an unrelated e7 removal follows the held e2 pawn");
        const uint32_t start = session.State().now_ms;
        bool opponentSquareWasDark = true;
        bool noCapturePlacement = true;
        for (const uint32_t age : {0U, 20U, 299U, 300U, 600U, 1500U}) {
            Check(session.Advance(start + age - session.State().now_ms) == VIRTUAL_LITCH_OK,
                  "sample first frame, early correction, and sustained correction");
            const auto energy = OutputEnergy(session, E7);
            opponentSquareWasDark &= energy[0U] == 0U && energy[1U] == 0U;
            noCapturePlacement &= session.State().required_action != static_cast<uint8_t>(
                runtime::RequiredPhysicalAction::PlacePrimary);
            if (age == 0U) {
                const auto redReturnRail = [&](hardware::SquareLedSlot slot) {
                    const auto color = Led(session, E7, slot);
                    return color.r > color.g && color.r > color.b &&
                           OutputEnergy(session, E7, slot)[1U] != 0U;
                };
                Check(opposingColor.r > opposingColor.g && opposingColor.r > opposingColor.b &&
                          redReturnRail(hardware::SquareLedSlot::Square0) &&
                          redReturnRail(hardware::SquareLedSlot::Square2),
                      "the first return flash uses Black's palette on both mapped rails");
            }
            checkpoint("e7-removed-while-e2-held", age, "return-e7-and-e2");
        }
        Check(opponentSquareWasDark && noCapturePlacement,
              "unrelated missing opponent stays dark on both Piece LEDs and never asks for a capture landing");
        const uint64_t firstReturn = bothHeld | bit(returnOpponentFirst ? E7 : E2);
        Check(Observe(session, firstReturn) && session.Advance(2500U) == VIRTUAL_LITCH_OK &&
                  session.UndoHistoryMoveCount() == 0U &&
                  acknowledged == session.HomeStateSnapshot().fen,
              "either first return is correction and cannot commit a move after the settle deadline");
        checkpoint(returnOpponentFirst ? "opponent-returned-first" : "mover-returned-first", 0U,
                   returnOpponentFirst ? "return-e2" : "return-e7");
        Check(Observe(session, OPENING) && session.Advance(600U) == VIRTUAL_LITCH_OK &&
                  session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
                  session.State().correction_mask == 0ULL &&
                  session.State().valid_destination_mask == 0ULL &&
                  session.State().expected_occupied_mask == OPENING &&
                  session.UndoHistoryMoveCount() == 0U,
              "restoring both pieces cancels the pickup and returns to the same game");
        checkpoint("opening-restored", 0U, "choose-a-legal-White-move");
    }
    const uint64_t e4 = (OPENING & ~bit(E2)) | bit(E4);
    Check(Observe(session, OPENING & ~bit(E2)) && Observe(session, e4) &&
              session.Advance(2400U) == VIRTUAL_LITCH_OK && session.UndoHistoryMoveCount() == 1U &&
              session.State().expected_occupied_mask == e4,
          "the subsequent legal e2-e4 move commits normally");
}

void RunSetupPulseCurrentLimitJourney() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.max_led_current_ma = 50U;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) &&
              Set(session, 806U, "BRIGHTNESS", "8") &&
              session.Advance(2400U) == VIRTUAL_LITCH_OK,
          "raw occupied setup and acknowledged maximum brightness reach the output limiter");
    for (unsigned tick = 0U; tick < 32U; ++tick) {
        Check(session.Advance(50U) == VIRTUAL_LITCH_OK &&
                  session.PhysicalFrame().power_limited != 0U &&
                  session.PhysicalFrame().applied_led_current_ma <= 50U,
              "stronger king pulse respects an actively constrained physical output budget");
    }
    std::printf("[KING_PULSE_CURRENT_LIMIT] raw_hall=%016llx brightness=8 "
                "sampled_ms=1600 cap_ma=50 applied_ma=%u limited=%u "
                "next=play-white expiry=existing-cadence\n",
                static_cast<unsigned long long>(session.State().physical_occupied_mask),
                unsigned(session.PhysicalFrame().applied_led_current_ma),
                unsigned(session.PhysicalFrame().power_limited));
}

void RunCheckmateJourney() {
    // Reach Fool's Mate through the same raw Hall, settle, engine-action and
    // acknowledgement owners as a physical game. No final FEN or render state
    // is injected: merely importing a mate can leave placement recovery active.
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              Observe(session, OPENING) &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              Set(session, 910U, "MINIMAL_ANIMATIONS", "true"),
          "mate journey starts at the playable opening with stationary effects");

    uint64_t occupancy = OPENING;
    constexpr std::array<std::array<uint8_t, 2>, 4> MOVES{{
        {53U, 45U}, // f2-f3
        {12U, 28U}, // e7-e5
        {54U, 38U}, // g2-g4
        {3U, 39U},  // Qd8-h4#
    }};
    constexpr std::array<const char*, 4> MOVE_NAMES{
        "f2-f3", "e7-e5", "g2-g4", "Qd8-h4-mate"};
    for (size_t index = 0U; index < MOVES.size(); ++index) {
        const auto move = MOVES[index];
        const uint64_t before = occupancy;
        occupancy &= ~(UINT64_C(1) << move[0]);
        const bool liftAccepted = Observe(session, occupancy) &&
            session.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::PieceLifted) &&
            (session.State().valid_destination_mask &
             (UINT64_C(1) << move[1])) != 0ULL;
        occupancy |= UINT64_C(1) << move[1];
        const bool placed = liftAccepted && Observe(session, occupancy) &&
            session.State().expected_occupied_mask == before;
        const bool committed = placed &&
            session.Advance(2000U) == VIRTUAL_LITCH_OK &&
            session.State().expected_occupied_mask == occupancy &&
            session.State().pending_action_token == 0U &&
            session.State().correction_mask == 0ULL &&
            session.State().side_to_move_black == (index % 2U == 0U ? 1U : 0U);
        Check(committed, "raw Fool's Mate move settles and is acknowledged");
        if (!committed) {
            Checkpoint(session, "mate-move-not-committed", "failed", "inspect-move-acknowledgement");
            return;
        }
        Checkpoint(session, MOVE_NAMES[index], "Hall-move-acknowledged",
                   index + 1U < MOVES.size() ? MOVE_NAMES[index + 1U]
                                            : "inspect-terminal-result");
    }

    const auto homeHasMatePosition = [&]() {
        const auto line = session.HomeStateLine();
        const std::string_view home(line.CStr());
        // Classic Home publishes the authoritative FEN. The compact out/win
        // fields describe Duck outcomes, so Classic checkmate leaves them n.
        // This exact piece placement, side and rights are Fool's Mate; FEN
        // counters do not decide whether White has any legal check evasion.
        // The checkmate conclusion here is known-position inference from Home,
        // not an exposed coordinator-derived inCheckmate field.
        return home.find(
                   "\"fen\":\"rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - ") !=
                   std::string_view::npos &&
               home.find("\"g\":\"c\"") != std::string_view::npos &&
               home.find("\"out\":\"n\"") != std::string_view::npos &&
               home.find("\"win\":\"n\"") != std::string_view::npos;
    };
    const auto homeLine = session.HomeStateLine();
    const std::string_view home(homeLine.CStr());
    const bool mateAcknowledged = homeHasMatePosition() &&
        home.find("\"a\":\"idle\"") != std::string_view::npos &&
        home.find("\"pm\":0") != std::string_view::npos &&
        home.find("\"fs\":1") == std::string_view::npos &&
        session.State().interaction_phase == static_cast<uint8_t>(
            runtime::ChessInteractionPhase::Idle);
    Check(mateAcknowledged,
          "authoritative Home publishes the acknowledged Classic mate outside setup/correction");
    if (!mateAcknowledged) {
        Checkpoint(session, "mate-authority-not-confirmed", "failed", "inspect-Home-position");
        return;
    }

    const auto onlyWinnerEdgeLit = [&]() {
        for (uint8_t index = 0U; index < 32U; ++index) {
            if ((Energy(session.SemanticFrame().status[index]) > 0U) !=
                (index < 8U)) return false;
        }
        return true;
    };
    const auto mappedStatusCheckpoint = [&](const char* stage) {
        const auto& frame = session.PhysicalFrame();
        const bool rotate = modules::ComputeControlBankSwap(
            session.BoardRotate(), session.FirstPlayerIsP2(), false);
        const auto topology = hardware::BoardLedTopologyFor(hardware::BoardRevision::V3S3);
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> semantic{};
        for (uint8_t index = 0U; index < 32U; ++index) {
            const auto color = session.SemanticFrame().status[index];
            semantic[index] = {color.r, color.g, color.b};
        }
        hardware::PhysicalLedFrame mapped{};
        hardware::ApplySemanticStatusFrame(
            {hardware::BoardRevision::V3S3, rotate, rotate}, semantic, mapped);
        Check(frame.led_count == 288U && frame.channels_per_led == 4U,
              "terminal journey uses the existing V3 RGBW physical chain");
        std::printf("[TERMINAL_STATUS_MAPPING] stage=%s t=%u applied_scale_65535=%u",
                    stage, session.State().now_ms, frame.applied_brightness_scale_65535);
        for (uint8_t index = 0U; index < 32U; ++index) {
            const uint16_t physical = topology.physicalBoardLedCount +
                modules::status_led_frame_adapter::PhysicalIndexForSemanticIndex(index, rotate);
            const auto scale = [&](const uint8_t value) {
                return static_cast<uint8_t>((static_cast<uint32_t>(value) *
                    frame.applied_brightness_scale_65535 + 32767U) / 65535U);
            };
            const auto rgb = mapped[physical];
            const auto expected = modules::AutoMapRgbToRgbw(
                modules::rgb_optical_calibration::PresentForS3(
                    {scale(rgb.r), scale(rgb.g), scale(rgb.b)}), 32U);
            const auto actual = frame.pixels[physical];
            // The public applied scale is quantized to u16; channel rounding
            // may differ by one. A semantic black pixel must remain exact black.
            const auto near = [](const uint8_t lhs, const uint8_t rhs) {
                return lhs > rhs ? lhs - rhs <= 1U : rhs - lhs <= 1U;
            };
            Check(Energy(session.SemanticFrame().status[index]) == 0U
                      ? Energy(actual) == 0U
                      : near(actual.r, expected.r) && near(actual.g, expected.g) &&
                        near(actual.b, expected.b) && near(actual.w, expected.w),
                  "all 32 status pixels reach the existing mapped V3 RGBW output");
            std::printf(" %u:%u/%u/%u->%u:%u/%u/%u/%u", index,
                        semantic[index].r, semantic[index].g, semantic[index].b,
                        physical, actual.r, actual.g, actual.b, actual.w);
        }
        std::printf("\n");
    };
    const auto terminalCheckpoint = [&](const char* stage) {
        Checkpoint(session, stage, "acknowledged-known-mate-position", "X-plus-O-hold-or-Undo");
        const auto& status = session.SemanticFrame().status;
        uint32_t lit = 0U;
        for (uint8_t index = 0U; index < 32U; ++index) {
            if (Energy(status[index]) > 0U) lit |= UINT32_C(1) << index;
        }
        std::printf("[TERMINAL_EDGE_JOURNEY] stage=%s t=%u winner=P2 "
                    "status_lit=%08x F8=%u/%u/%u C8=%u/%u/%u "
                    "C1=%u/%u/%u F1=%u/%u/%u next=optional-X-plus-O-hold-or-Undo\n",
                    stage, session.State().now_ms, lit,
                    status[2U].r, status[2U].g, status[2U].b,
                    status[5U].r, status[5U].g, status[5U].b,
                    status[18U].r, status[18U].g, status[18U].b,
                    status[21U].r, status[21U].g, status[21U].b);
        mappedStatusCheckpoint(stage);
    };
    const auto terminalFrame = session.SemanticFrame();
    for (const uint32_t advance : {0U, 500U, 500U}) {
        Check(session.Advance(advance) == VIRTUAL_LITCH_OK &&
                  onlyWinnerEdgeLit() &&
                  session.SemanticFrame().status_hash == terminalFrame.status_hash,
              "acknowledged Minimal mate keeps only the stationary winner edge through former blink cycles");
        terminalCheckpoint("mate-minimal-no-reset-cue");
    }
    Check(Set(session, 911U, "MINIMAL_ANIMATIONS", "false"),
          "ordinary winner animation can resume without a reset-cue setting");
    const auto animatedFrame = session.SemanticFrame();
    Check(session.Advance(550U) == VIRTUAL_LITCH_OK && onlyWinnerEdgeLit() &&
              session.SemanticFrame().status_hash != animatedFrame.status_hash,
          "ordinary mate sparks animate only the winner edge");
    terminalCheckpoint("mate-ordinary-no-reset-cue");
    bool defaultKingLit = false;
    bool defaultImpactSeen = false;
    for (uint8_t frame = 0U; frame <= 20U; ++frame) {
        const auto king = OutputEnergy(session, 60U);
        if (king[0U] > 0U && king[1U] > 0U && !defaultKingLit) {
            defaultKingLit = true;
            Checkpoint(session, "default-Fire-Lob-king-before-impact", "acknowledged-mate", "watch-contributor-fire-travel");
        }
        if (king[0U] == 0U && king[1U] == 0U && !defaultImpactSeen) {
            defaultImpactSeen = true;
            Checkpoint(session, "default-Fire-Lob-impact", "acknowledged-mate", "king-returns-after-impact");
        }
        if (frame != 20U) Check(session.Advance(100U) == VIRTUAL_LITCH_OK,
                               "sample the default Fire Lob through one complete effect period");
    }
    Check(defaultKingLit && defaultImpactSeen && homeHasMatePosition(),
          "absent style selects Fire Lob in the real raw-move mate journey and preserves terminal authority");
    Checkpoint(session, "mate-pieces-present", "acknowledged-known-mate-position", "lift-e1-and-h4");

    constexpr uint8_t LOSING_KING = 60U;
    constexpr uint8_t MATING_QUEEN = 39U;
    const uint64_t lifted = occupancy &
        ~((UINT64_C(1) << LOSING_KING) | (UINT64_C(1) << MATING_QUEEN));
    Check(Observe(session, lifted) && homeHasMatePosition() &&
              session.State().expected_occupied_mask == occupancy,
          "terminal Hall lift preserves the acknowledged mating position");
    Checkpoint(session, "mate-anchors-lifted", "known-mate-position-retained", "replace-e1-and-h4-or-new-game");
    Check(Observe(session, occupancy) && homeHasMatePosition() &&
              Energy(Led(session, LOSING_KING, hardware::SquareLedSlot::Piece)) > 0U &&
              Energy(Led(session, MATING_QUEEN, hardware::SquareLedSlot::Piece)) > 0U,
          "stationary terminal centers recover when pieces return");
    Checkpoint(session, "mate-anchors-restored", "known-mate-position-retained", "new-game");

    const auto litStatusCount = [&]() {
        uint8_t count = 0U;
        for (const auto color : session.SemanticFrame().status) {
            if (Energy(color) > 0U) ++count;
        }
        return count;
    };
    Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                               runtime::VirtualLitchPhysicalButton::O, true) == VIRTUAL_LITCH_OK &&
              session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                                   runtime::VirtualLitchPhysicalButton::X, true) == VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              session.Advance(750U) == VIRTUAL_LITCH_OK && homeHasMatePosition(),
          "raw X+O chord starts destructive hold progress without changing the mating game");
    const uint8_t earlyProgress = litStatusCount();
    Check(session.Advance(750U) == VIRTUAL_LITCH_OK && homeHasMatePosition() &&
              litStatusCount() > earlyProgress && litStatusCount() > 8U &&
              session.UndoHistoryMoveCount() == 4U,
          "destructive reset hold still grows across the full edge before its two-second threshold");
    std::printf("[TERMINAL_EDGE_JOURNEY] stage=mate-reset-hold t=%u "
                "input=P1-O-plus-X held1500ms outcome=pending "
                "history=%u status_lit_count=%u early_status_lit_count=%u "
                "next=release-to-cancel-or-continue-to-threshold\n",
                session.State().now_ms, session.UndoHistoryMoveCount(),
                litStatusCount(), earlyProgress);
    mappedStatusCheckpoint("mate-reset-hold");
    Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                               runtime::VirtualLitchPhysicalButton::X, false) == VIRTUAL_LITCH_OK &&
              session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                                   runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              homeHasMatePosition() && onlyWinnerEdgeLit() &&
              session.UndoHistoryMoveCount() == 4U,
          "releasing the incomplete reset hold cancels progress and restores only the winner edge");
    terminalCheckpoint("mate-reset-hold-cancelled");

    const auto homeBeforeMate = [&]() {
        const auto line = session.HomeStateLine();
        return std::string_view(line.CStr()).find(
            "rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq ") !=
            std::string_view::npos;
    };
    const auto resetCueAbsent = [&]() {
        for (const uint8_t index : {2U, 5U, 18U, 21U}) {
            const auto color = session.SemanticFrame().status[index];
            if (color.r == 0U && color.g > 0U && color.b == 0U) return false;
        }
        return true;
    };
    modules::ControlMutationOutcome undoOutcome;
    Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                               runtime::VirtualLitchPhysicalButton::X, true) == VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                                   runtime::VirtualLitchPhysicalButton::X, false) == VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              session.TryGetLastBoardControlOutcome(undoOutcome) &&
              undoOutcome.status == modules::ControlMutationStatus::Applied &&
              undoOutcome.actionState == modules::ControlActionState::UndoPending &&
              session.IsUndoActionPending() && homeHasMatePosition() &&
              session.UndoHistoryMoveCount() == 4U && resetCueAbsent(),
          "raw X acknowledges UndoPending while the current game waits for coordinator handoff");
    Checkpoint(session, "mate-Undo-accepted", "Applied-UndoPending-awaiting-handoff", "wait-for-return-plan");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK && homeBeforeMate() &&
              session.UndoHistoryMoveCount() == 3U &&
              session.IsUndoActionPending() && PairLit(session, 3U) &&
              OutputEnergy(session, 3U, hardware::SquareLedSlot::Square0)[1] > 0U &&
              resetCueAbsent(),
          "the next production tick hands off Undo and publishes the authoritative three-ply return plan");
    Checkpoint(session, "mate-Undo-plan", "authoritative-three-ply-UndoPending", "lift-h4-place-d8",
               UINT64_C(1) << 3U);
    mappedStatusCheckpoint("mate-Undo-plan");
    Check(session.Advance(500U) == VIRTUAL_LITCH_OK &&
              session.IsUndoActionPending() && homeBeforeMate() &&
              PairLit(session, 3U) && resetCueAbsent(),
          "Undo return guidance remains until physical restoration without terminal reset cues");
    std::printf("[TERMINAL_UNDO_EXIT] t=%u input=P1-X "
                "authoritative=three-ply UndoPending outcome=Applied "
                "next=lift-h4-place-d8 expiry=none-until-physical-restoration\n",
                session.State().now_ms);
    const uint64_t queenLifted = occupancy & ~(UINT64_C(1) << MATING_QUEEN);
    const uint64_t beforeMateOccupancy = queenLifted | (UINT64_C(1) << 3U);
    Check(Observe(session, queenLifted) && session.IsUndoActionPending() &&
              PairLit(session, 3U) && resetCueAbsent(),
          "physical mating-queen lift retains the Undo return destination");
    Checkpoint(session, "mate-Undo-queen-lifted", "UndoPending", "place-d8",
               (UINT64_C(1) << 3U) | (UINT64_C(1) << 30U));
    Check(Observe(session, beforeMateOccupancy) &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              !session.IsUndoActionPending() && homeBeforeMate() &&
              session.UndoHistoryMoveCount() == 3U &&
              session.State().physical_occupied_mask == beforeMateOccupancy &&
              session.State().expected_occupied_mask == beforeMateOccupancy &&
              session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
              session.State().side_to_move_black == 1U && resetCueAbsent(),
          "physical Qh4-d8 restores play-ready Black to move and expires terminal presentation");
    Checkpoint(session, "mate-Undo-complete", "Idle-three-ply-play-ready", "play-black");
    mappedStatusCheckpoint("mate-Undo-complete");
    Check(session.Advance(1000U) == VIRTUAL_LITCH_OK && homeBeforeMate() &&
              !session.IsUndoActionPending() && resetCueAbsent(),
          "terminal reset cues do not return after Undo completion");
    Check(Observe(session, beforeMateOccupancy & ~(UINT64_C(1) << 6U)) &&
              session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
              session.State().valid_destination_mask != 0ULL &&
              Observe(session, beforeMateOccupancy) &&
              session.UndoHistoryMoveCount() == 3U,
          "restored play accepts the next ordinary knight pickup without a new game");
    Checkpoint(session, "mate-Undo-next-pickup-restored", "play-ready-current-game-retained", "play-black");
    std::printf("[TERMINAL_UNDO_EXIT] t=%u input=Qh4-d8 then g8-lift-and-return "
                "authoritative=Idle-three-ply outcome=completed "
                "next=play-black terminal-cue-expiry=on-three-ply-authority Undo-guidance-expiry=on-Qd8-restoration\n",
                session.State().now_ms);
}

} // namespace

void AssembleSetupForCoachingJourney(runtime::VirtualLitchSession& session, const bool coach,
                                    const bool invalidRecovery = false) {
    Check(Observe(session, 0ULL) && session.Advance(200U) == VIRTUAL_LITCH_OK,
          "SETUP_COACH: raw empty board enters Fresh Setup");
    if (coach) Check(Set(session, 950U, "P1_AUTO_HINTS", "true"),
                     "SETUP_COACH: saved Automatic Hints is enabled through the production setting owner");
    uint64_t mask = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((OPENING & (UINT64_C(1) << square)) == 0ULL || square == E2) continue;
        mask |= UINT64_C(1) << square;
        Check(Observe(session, mask) && session.Advance(60U) == VIRTUAL_LITCH_OK,
              "SETUP_COACH: each opening piece follows the real Hall/setup lifecycle");
    }
    if (invalidRecovery)
        Check(Observe(session, OPENING | (UINT64_C(1) << 17U) | (UINT64_C(1) << 18U)) &&
                  session.Advance(200U) == VIRTUAL_LITCH_OK &&
                  session.EdgeCueIntent().kind != modules::EdgeLedCueKind::SetupComplete,
              "SETUP_COACH: two ordinary extras cannot acknowledge completed setup");
    Check(Observe(session, OPENING) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.EdgeCueIntent().kind == modules::EdgeLedCueKind::SetupComplete,
          "SETUP_COACH: the clean final piece or recovered exact board admits one real Ready episode");
}

void SetupCompleteBeforeAutomaticCoachingJourney(const bool rotate, const bool invalidRecovery) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.board_rotate = rotate;
    test_support::BoundedHintSession automatic(config), ordinary(config);
    AssembleSetupForCoachingJourney(automatic.session, true, invalidRecovery);
    AssembleSetupForCoachingJourney(ordinary.session, false, invalidRecovery);
    auto& session = automatic.session;
    auto& control = ordinary.session;
    const uint32_t started = session.State().now_ms;
    for (const uint32_t age : {0U, 120U, 500U, 900U, 1560U, 1680U, 1799U}) {
        Check(session.Advance(started + age - session.State().now_ms) == VIRTUAL_LITCH_OK &&
                  control.Advance(started + age - control.State().now_ms) == VIRTUAL_LITCH_OK,
              "SETUP_COACH: sample the original finite Ready clock");
        Check(!session.HintDisplayRequested(),
              "SETUP_COACH: automatic Hint visuals stay hidden from the first Ready frame through its fade");
        for (uint8_t square = 0U; square < 64U; ++square) {
            if ((OPENING & (UINT64_C(1) << square)) == 0ULL)
                Check(Energy(Led(session, square, hardware::SquareLedSlot::Piece)) == 0U,
                      "SETUP_COACH: automatic source/landing guidance cannot leak through Ready");
        }
        if (age < 1560U) {
            for (uint8_t index = 0U; index < 32U; ++index) {
                const auto a = session.SemanticFrame().status[index];
                const auto b = control.SemanticFrame().status[index];
                Check(a.r == b.r && a.g == b.g && a.b == b.b &&
                          Energy(session.PhysicalFrame().pixels[256U + hardware::V3_3PhysicalIndexForSemanticStatus(index)]) ==
                              Energy(control.PhysicalFrame().pixels[256U + hardware::V3_3PhysicalIndexForSemanticStatus(index)]),
                      "SETUP_COACH: Coaching on and off retain the same semantic and mapped Ready sweep");
            }
        }
        std::printf("[SETUP_COACH_READY] trigger=%s rotate=%u age=%u occupancy=%016llx phase=%u hint_requested=%u cue=%u history=%u next=play-now-or-observe-Ready-expiry\n",
                    invalidRecovery ? "invalid-recovery" : "clean-32nd-piece", rotate, age, static_cast<unsigned long long>(session.State().physical_occupied_mask),
                    session.State().interaction_phase, session.HintDisplayRequested(),
                    static_cast<unsigned>(session.EdgeCueIntent().kind), session.UndoHistoryMoveCount());
    }
    modules::ThinkStats stats;
    Check(automatic.engine.GetLastThinkStats(stats) && stats.nodeCount > 0U,
          "SETUP_COACH: real engine analysis advances while automatic presentation is hidden");
    Check(session.Advance(2U) == VIRTUAL_LITCH_OK &&
              session.EdgeCueIntent().kind == modules::EdgeLedCueKind::None &&
              session.HintDisplayRequested(),
          "SETUP_COACH: Ready expiry releases automatic Hint without restarting Ready");
    uint8_t from = 64U, to = 64U;
    Check(session.TryGetHintMoveSquares(from, to) && from < 64U && to < 64U &&
              Energy(Led(session, to, hardware::SquareLedSlot::Piece)) > 0U,
          "SETUP_COACH: expiry reveals the already calculated legal Hint");
    std::printf("[SETUP_COACH_EXPIRED] trigger=%s rotate=%u age=%u nodes=%u hint=%u:%u history=%u next=follow-Hint-or-move-any-legal-piece\n",
                invalidRecovery ? "invalid-recovery" : "clean-32nd-piece", rotate, session.State().now_ms - started, stats.nodeCount, from, to,
                session.UndoHistoryMoveCount());
}

void SetupCoachingInterruptionJourney(const uint8_t interruption) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    AssembleSetupForCoachingJourney(session, true);
    const uint32_t readyStarted = session.State().now_ms;
    Check(session.Advance(600U) == VIRTUAL_LITCH_OK && !session.HintDisplayRequested(),
          "SETUP_COACH: an interruption starts during the real Ready episode");
    if (interruption == 0U) {
        Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true) == VIRTUAL_LITCH_OK &&
                  session.Advance(20U) == VIRTUAL_LITCH_OK &&
                  session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
                  session.Advance(20U) == VIRTUAL_LITCH_OK && session.HintDisplayRequested(),
              "SETUP_COACH: a fresh explicit raw O Hint interrupts Ready immediately");
        uint8_t from = 64U, to = 64U;
        Check(session.TryGetHintMoveSquares(from, to) && from < 64U && to < 64U &&
                  Energy(Led(session, to, hardware::SquareLedSlot::Piece)) > 0U,
              "SETUP_COACH: explicit Hint immediately uses the calculation retained behind Ready");
        const auto corner = session.SemanticFrame().status[
            modules::PlayerCornerLedIndices(modules::UiOwner::P1)[0U]];
        Check(corner.r > 0U && corner.g > 0U && corner.b > 0U,
              "SETUP_COACH: explicit Hint attention, rather than the Ready sweep, owns the perimeter");
    } else if (interruption == 1U || interruption == 2U) {
        const uint8_t lifted = interruption == 1U ? E2 : E7;
        Check(Observe(session, OPENING & ~(UINT64_C(1) << lifted)) && session.Advance(20U) == VIRTUAL_LITCH_OK &&
                  session.EdgeCueIntent().kind != modules::EdgeLedCueKind::SetupComplete,
              "SETUP_COACH: early valid pickup and wrong-side correction both dismiss Ready without delaying input");
        if (interruption == 1U)
            Check(session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
                      (session.State().valid_destination_mask & (UINT64_C(1) << E4)) != 0ULL && PairLit(session, E4),
                  "SETUP_COACH: early pickup retains ordinary legal next-action guidance");
        else Check(session.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::CorrectBoard),
                   "SETUP_COACH: wrong-side pickup retains required correction authority");
        Check(Observe(session, OPENING) && session.Advance(100U) == VIRTUAL_LITCH_OK &&
                  session.EdgeCueIntent().kind != modules::EdgeLedCueKind::SetupComplete,
              "SETUP_COACH: physical recovery does not replay the dismissed Ready episode");
    } else {
        virtual_litch_runtime_health_fact scan{};
        virtual_litch_default_runtime_health_fact(&scan);
        scan.kind = VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BOARD_SCAN;
        scan.board_scan = VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN;
        Check(session.DispatchRuntimeHealthFact(scan) == VIRTUAL_LITCH_OK &&
                  session.Advance(100U) == VIRTUAL_LITCH_OK &&
                  session.DispatchRuntimeHealthFact(scan) == VIRTUAL_LITCH_OK,
              "SETUP_COACH: real health facts interrupt Ready with paused-input authority");
        unsigned lit = 0U;
        for (const auto pixel : session.SemanticFrame().status) lit += Energy(pixel) > 0U;
        Check(lit == 16U && !session.HintDisplayRequested(),
              "SETUP_COACH: required health presentation suppresses automatic Hint and Ready");
        Check(session.Advance(1801U) == VIRTUAL_LITCH_OK,
              "SETUP_COACH: the original Ready clock continues to expire beneath safety");
        scan.board_scan = VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_STABLE;
        Check(session.DispatchRuntimeHealthFact(scan) == VIRTUAL_LITCH_OK &&
                  session.Advance(100U) == VIRTUAL_LITCH_OK &&
                  session.DispatchRuntimeHealthFact(scan) == VIRTUAL_LITCH_OK &&
                  session.EdgeCueIntent().kind != modules::EdgeLedCueKind::SetupComplete,
              "SETUP_COACH: safety recovery cannot restart the expired Ready episode");
    }
    Check(session.UndoHistoryMoveCount() == 0U && session.State().expected_occupied_mask == OPENING,
          "SETUP_COACH: every interruption preserves engine/history/confirmation authority");
    std::printf("[SETUP_COACH_INTERRUPTION] kind=%u age=%u phase=%u required=%u hint_requested=%u cue=%u history=%u next=follow-current-authoritative-guidance\n",
                interruption, session.State().now_ms - readyStarted, session.State().interaction_phase,
                session.State().required_action, session.HintDisplayRequested(),
                static_cast<unsigned>(session.EdgeCueIntent().kind), session.UndoHistoryMoveCount());
}

int main() {
    for (const bool rotate : {false, true})
        for (const bool invalidRecovery : {false, true})
            SetupCompleteBeforeAutomaticCoachingJourney(rotate, invalidRecovery);
    for (uint8_t interruption = 0U; interruption < 4U; ++interruption) SetupCoachingInterruptionJourney(interruption);
    for (const bool rotate : std::array<bool, 2>{false, true}) {
        for (const bool firstP2 : std::array<bool, 2>{false, true}) {
            RunSetupCenterOccupancyJourney(rotate, firstP2);
        }
    }
    const int failuresBeforeTwoLifts = g_failures;
    RunUnrelatedOpponentLiftJourney();
    if (g_failures == failuresBeforeTwoLifts) {
        Check(RunMultiPieceCaptureAudit(), "bounded multi-piece color and recovery audit");
    }
    RunSetupPulseCurrentLimitJourney();
    RunCheckmateJourney();
    std::printf("[EMPTY_PIECE_LED_JOURNEY] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
