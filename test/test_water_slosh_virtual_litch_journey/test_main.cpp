#include <array>
#include <cstdio>

#include "bounded_hint_engine.hpp"
#include "hardware/lis2dw12.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "virtual_litch_session.hpp"
#include "virtual_litch_session_internal.hpp"

namespace {

int g_failures = 0;
constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
void Check(const bool condition, const char* message) {
    if (!condition) { std::printf("[FAIL] %s\n", message); ++g_failures; }
}

hardware::Lis2dw12Sample Tilt() { return {0, 0, 0, 29, -646, 741}; }
hardware::Lis2dw12Sample Level() { return {0, 0, 0, 29, 11, 1023}; }

uint32_t Energy(const virtual_litch_color pixel) {
    return uint32_t(pixel.r) + pixel.g + pixel.b + pixel.w;
}

bool SameColor(const virtual_litch_color lhs, const virtual_litch_color rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.w == rhs.w;
}

uint32_t StatusBankEnergy(const virtual_litch_physical_frame& frame,
                          const uint8_t bank) {
    uint32_t energy = 0U;
    for (uint8_t offset = 0U; offset < 8U; ++offset) {
        energy += Energy(frame.pixels[256U + bank * 8U + offset]);
    }
    return energy;
}

uint32_t BoardBankEnergy(const virtual_litch_physical_frame& frame,
                         const uint8_t bank) {
    uint32_t energy = 0U;
    for (uint8_t offset = 0U; offset < 8U; ++offset) {
        const uint8_t square = bank == 0U ? offset :
            bank == 1U ? uint8_t(7U + 8U * offset) :
            bank == 2U ? uint8_t(63U - offset) :
                         uint8_t(56U - 8U * offset);
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            energy += Energy(frame.pixels[square * 4U + slot]);
        }
    }
    return energy;
}

template <typename Session>
void Feed(Session& session, const hardware::Lis2dw12Sample& sample,
          const uint32_t duration) {
    for (uint32_t t = 0U; t < duration; t += 40U) {
        session.ObserveMotionSample(sample);
        Check(session.Advance(40U) == VIRTUAL_LITCH_OK, "motion journey advances at 25 Hz sampling");
    }
}

void Checkpoint(const runtime::VirtualLitchSession& session,
                const char* stage, const char* next) {
    const auto& state = session.State();
    const auto text = session.DisplayText();
    const auto home = session.HomeStateSnapshot();
    const auto homeLine = session.HomeStateLine();
    std::printf("[WATER_JOURNEY] stage=%s t=%u occupancy=%016llx phase=%s "
                "required=%u fresh=%u placement=%u hint=%u water=%u "
                "display=\"%.*s\" next=%s home=%s\n", stage, state.now_ms,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                runtime::ChessInteractionPhaseToString(
                    static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                unsigned(state.required_action), unsigned(home.freshSetup),
                unsigned(home.placementMode), unsigned(session.HintDisplayRequested()),
                session.WaterSloshActive() ? 1U : 0U,
                int(text.size()), text.empty() ? "" : text.data(), next, homeLine.CStr());
}

void PhysicalJourney(const bool rotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.board_rotate = rotate ? 1U : 0U;
    runtime::VirtualLitchSession session(config);
    Check(session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK,
          "empty board enters through production Hall observation");
    Feed(session, Level(), 500U);
    Check(!session.WaterSloshActive(), "stationary empty setup stays its normal guide");
    Checkpoint(session, "empty_setup", "pick_up_or_place_first_piece");
    Feed(session, {0, 0, 0, 89, -202, 1450}, 40U);
    Feed(session, Level(), 500U);
    Check(!session.WaterSloshActive() && session.HomeStateSnapshot().freshSetup,
          "one bump and return to level leave the normal empty Setup guide visible");
    Checkpoint(session, "bump_ignored", "pick_up_or_place_first_piece");
    Feed(session, Tilt(), 600U);
    Check(session.WaterSloshActive(), "physical left-edge lift starts liquid");
    Check(session.DisplayText().empty(), "handling adds no text or menu");
    const auto& physical = session.PhysicalFrame();
    uint32_t leftEnergy = 0U;
    uint32_t rightEnergy = 0U;
    for (uint8_t row = 0U; row < 8U; ++row) {
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            const auto l = physical.pixels[(row * 8U) * 4U + slot];
            const auto r = physical.pixels[(row * 8U + 7U) * 4U + slot];
            leftEnergy += l.r + l.g + l.b + l.w;
            rightEnergy += r.r + r.g + r.b + r.w;
        }
    }
    Check(rightEnergy > leftEnergy,
          "final mapped RGBW light pools on physical downhill right in either game orientation");
    Checkpoint(session, "tilted_water", "move_board_or_set_down");
    const uint8_t physicalA2 = rotate ? 15U : 48U;
    Check(session.ObserveHallMask(1ULL << physicalA2) == VIRTUAL_LITCH_OK,
          "first piece reaches production occupancy owner");
    Check(!session.WaterSloshActive(), "first piece cancels water in that same frame");
    Checkpoint(session, "piece_preempts", "continue_arranging_opening");
    Check(session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK,
          "clearing the only assembly piece reaches the empty Setup owner");
    const auto cleared = session.HomeStateSnapshot();
    Check(cleared.freshSetup && cleared.placementMode &&
              session.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
              session.State().human_illegal_mask == 0ULL &&
              !session.HintDisplayRequested(),
          "clearing incomplete singleton assembly returns ordinary Setup without a gameplay pickup");
    Feed(session, Tilt(), 400U);
    Check(session.WaterSloshActive(),
          "motion may resume water after explicit pregame clearing retires the assembly owner");
    Checkpoint(session, "singleton_assembly_cleared", "arrange_opening_pieces");
    // A separate fresh-board episode exercises the control quiet interval.
    session.Reset();
    session.ObserveHallMask(0ULL);
    Feed(session, Tilt(), 500U);
    Check(session.WaterSloshActive(), "fresh control journey starts water");
    Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
        runtime::VirtualLitchPhysicalButton::Up, true) == VIRTUAL_LITCH_OK,
        "physical Up enters through production debounce");
    Feed(session, Tilt(), 80U);
    Check(!session.WaterSloshActive(), "debounced physical control immediately cancels water");
    Feed(session, Tilt(), 1500U);
    Check(!session.WaterSloshActive(), "holding a control cannot restart water");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
        runtime::VirtualLitchPhysicalButton::Up, false);
    Feed(session, Level(), 1500U);
    Feed(session, Tilt(), 500U);
    Check(session.WaterSloshActive(), "released control recovers after its quiet interval");
    Feed(session, Level(), 3000U);
    Check(!session.WaterSloshActive(), "set-down decays and restores setup without input");
    Checkpoint(session, "settled", "place_first_piece_or_pick_up_again");
}

void PerimeterDirections() {
    struct Direction {
        const char* name;
        hardware::Lis2dw12Sample sample;
        uint8_t downhillBank;
    };
    // Physical chain: top, right, bottom, left; opposite banks are 16 apart.
    const std::array<Direction, 4> directions{{
        {"top", {0, 0, 0, 646, 0, 741}, 0U},
        {"right", {0, 0, 0, 0, -646, 741}, 1U},
        {"bottom", {0, 0, 0, -646, 0, 741}, 2U},
        {"left", {0, 0, 0, 0, 646, 741}, 3U},
    }};
    std::array<virtual_litch_physical_frame, 4> references{};
    for (const bool rotate : {false, true}) {
        for (const bool firstPlayerIsP2 : {false, true}) {
            for (const auto& direction : directions) {
                virtual_litch_config config{};
                virtual_litch_default_config(&config);
                config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
                config.board_rotate = rotate ? 1U : 0U;
                config.first_player_is_p2 = firstPlayerIsP2 ? 1U : 0U;
                runtime::VirtualLitchSession session(config);
                Check(session.BoardRotate() == rotate &&
                          session.FirstPlayerIsP2() == firstPlayerIsP2,
                      "journey applies both independent orientation settings");
                session.ObserveHallMask(0ULL);
                Feed(session, Level(), 500U);
                Feed(session, direction.sample, 1500U);
                Check(session.WaterSloshActive(),
                      "all physical downhill directions enter liquid");
                const auto& frame = session.PhysicalFrame();
                Check(frame.led_count == 288U && frame.channels_per_led == 4U,
                      "journey covers 256 square slots and all 32 RGBW status LEDs");
                const uint8_t uphillBank = (direction.downhillBank + 2U) % 4U;
                const uint32_t downhill = StatusBankEnergy(frame, direction.downhillBank);
                const uint32_t uphill = StatusBankEnergy(frame, uphillBank);
                Check(downhill > uphill,
                      "final RGBW perimeter pools downhill independent of both orientation settings");
                Check(BoardBankEnergy(frame, direction.downhillBank) >
                          BoardBankEnergy(frame, uphillBank),
                      "square slots and perimeter agree on physical downhill");
                if (!rotate && !firstPlayerIsP2) {
                    references[direction.downhillBank] = frame;
                } else {
                    const auto& reference = references[direction.downhillBank];
                    for (uint16_t pixel = 0U; pixel < 288U; ++pixel) {
                        Check(SameColor(frame.pixels[pixel], reference.pixels[pixel]),
                              "all 288 physical RGBW pixels ignore chess orientation during liquid");
                    }
                }
                std::printf("[WATER_PERIMETER] direction=%s board_rotate=%u "
                            "first_player_is_p2=%u downhill=%u uphill=%u\n",
                            direction.name, rotate ? 1U : 0U,
                            firstPlayerIsP2 ? 1U : 0U, downhill, uphill);
            }
        }
    }
    for (uint16_t pixel = 256U; pixel < 288U; ++pixel) {
        bool respondsToMotion = false;
        for (uint8_t direction = 1U; direction < 4U; ++direction) {
            respondsToMotion = respondsToMotion ||
                !SameColor(references[0].pixels[pixel], references[direction].pixels[pixel]);
        }
        Check(respondsToMotion,
              "every physical perimeter LED responds to the same changing liquid surface");
    }
}

virtual_litch_physical_frame AmbientWaterFrame(const float lux,
                                               const bool available,
                                               const bool rotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.board_rotate = rotate ? 1U : 0U;
    runtime::VirtualLitchSession session(config);
    session.ObserveHallMask(0ULL);
    session.ObserveWaterAmbientSample(lux, available);
    Feed(session, Level(), 500U);
    Feed(session, Tilt(), 600U);
    Check(session.WaterSloshActive() && session.DisplayText().empty(),
          "cached ambient light changes highlights without changing liquid admission or text");
    const auto frame = session.PhysicalFrame();
    Check(frame.led_count == 288U && frame.channels_per_led == 4U &&
              BoardBankEnergy(frame, 1U) > BoardBankEnergy(frame, 3U) &&
              StatusBankEnergy(frame, 1U) > StatusBankEnergy(frame, 3U),
          "ambient-lit final RGBW keeps square and perimeter water physically downhill");
    Checkpoint(session, available ? "ambient_highlights" : "ambient_fallback",
               "move_board_or_place_first_piece");
    std::printf("[WATER_AMBIENT] lux=%.1f available=%u board_rotate=%u\n",
                double(lux), available ? 1U : 0U, rotate ? 1U : 0U);
    return frame;
}

void AmbientHighlightJourney() {
    const auto dark = AmbientWaterFrame(0.0F, true, false);
    const auto bright = AmbientWaterFrame(400.0F, true, false);
    const auto rotated = AmbientWaterFrame(400.0F, true, true);
    const auto unavailable = AmbientWaterFrame(400.0F, false, false);
    const auto fallback = AmbientWaterFrame(0.0F, false, false);
    uint16_t changedSquares = 0U;
    uint16_t changedStatus = 0U;
    for (uint16_t pixel = 0U; pixel < 288U; ++pixel) {
        if (!SameColor(dark.pixels[pixel], bright.pixels[pixel])) {
            if (pixel < 256U) { ++changedSquares; }
            else { ++changedStatus; }
        }
        Check(SameColor(bright.pixels[pixel], rotated.pixels[pixel]),
              "mean ambient light never invents a player-relative light direction");
        Check(SameColor(unavailable.pixels[pixel], fallback.pixels[pixel]),
              "unavailable cached ambient sample restores deterministic RGBW fallback");
    }
    Check(changedSquares != 0U && changedStatus != 0U,
          "cached ambient light visibly modulates water highlights on squares and perimeter");
    std::printf("[WATER_AMBIENT] changed_square_slots=%u changed_status=%u\n",
                changedSquares, changedStatus);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession reset(config);
    reset.ObserveWaterAmbientSample(400.0F, true);
    Check(reset.Reset() == VIRTUAL_LITCH_OK, "ambient journey resets through its public owner");
    reset.ObserveHallMask(0ULL);
    Feed(reset, Level(), 500U);
    Feed(reset, Tilt(), 600U);
    for (uint16_t pixel = 0U; pixel < 288U; ++pixel) {
        Check(SameColor(reset.PhysicalFrame().pixels[pixel], fallback.pixels[pixel]),
              "native reset clears cached ambient input back to the default unavailable fallback");
    }
}

void VexRiderJourney() {
    for (const bool rotate : {false, true}) {
      for (const bool firstPlayerIsP2 : {false, true}) {
        virtual_litch_config config{};
        virtual_litch_default_config(&config);
        config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
        config.board_rotate = rotate ? 1U : 0U;
        config.first_player_is_p2 = firstPlayerIsP2 ? 1U : 0U;
        runtime::VirtualLitchSession session(config);
        const bool squareRotated = rotate != firstPlayerIsP2;
        const uint8_t vex = squareRotated ? 45U : 18U; // authoritative logical c6
        session.ObserveHallMask(1ULL << vex);
        Check(session.SetupDuckCandidateSquare() == 18U,
              "singleton Hall observation is provisionally Vex through the real setup owner");
        Feed(session, Tilt(), 1000U);
        Check(session.WaterSloshActive(), "authoritative singleton Vex rides water in Fresh Setup");
        const auto before = session.PhysicalFrame();
        for (const uint8_t slot : {1U, 3U}) {
            const auto pixel = before.pixels[vex * 4U + slot];
            Check(pixel.r > pixel.b + 10U && pixel.g > pixel.b + 10U,
                  "Vex piece-pair stays visibly yellow above the blue water");
        }
        Feed(session, {0, 0, 0, 0, 646, 741}, 1200U);
        const auto after = session.PhysicalFrame();
        Check(!SameColor(before.pixels[vex * 4U + 1U], after.pixels[vex * 4U + 1U]),
              "Vex yellow brightness rides the local wave as the board reverses");
        Checkpoint(session, "vex_riding", "tilt_board_or_add_first_chess_piece");
        session.ObserveHallMask((1ULL << vex) | (1ULL << (squareRotated ? 15U : 48U)));
        Check(!session.WaterSloshActive(), "adding any chess piece beside Vex preempts immediately");
        session.Reset();
        session.ObserveHallMask(1ULL << (squareRotated ? 15U : 48U));
        Feed(session, Tilt(), 1000U);
        Check(!session.WaterSloshActive(), "an ordinary singleton cannot ride as Vex");
      }
    }
}

void RequiredDuckPlanPreemptsWater() {
    for (const bool rotate : {false, true}) {
        virtual_litch_config config{};
        virtual_litch_default_config(&config);
        config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
        config.board_rotate = rotate ? 1U : 0U;
        test_support::BoundedHintSession fixture(config);
        auto& session = fixture.session;
        const uint8_t physicalDuck = rotate ? 19U : 44U;
        const uint8_t physicalE2 = rotate ? 11U : 52U;
        const uint64_t duckMask = UINT64_C(1) << physicalDuck;
        const uint64_t openingWithDuck = OPENING | duckMask;
        Check(session.ObserveHallMask(openingWithDuck) == VIRTUAL_LITCH_OK &&
                  session.Advance(2200U) == VIRTUAL_LITCH_OK &&
                  session.GameVariant() == modules::GameVariant::Duck &&
                  session.DuckSquare() == 44U,
              "raw complete e3 Duck opening establishes the authoritative variant");
        Check(session.ObserveHallMask(openingWithDuck &
                  ~(UINT64_C(1) << physicalE2)) == VIRTUAL_LITCH_OK &&
                  session.ObserveHallMask(openingWithDuck) == VIRTUAL_LITCH_OK &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::DuckRelocation),
              "blocked e2 pickup and return establish the real free obstruction plan");
        Checkpoint(session, "required_duck_plan", "move_duck_before_first_chess_move");
        uint64_t physical = openingWithDuck;
        for (uint8_t square = 0U; square < 64U; ++square) {
            if ((OPENING & (UINT64_C(1) << square)) == 0ULL) continue;
            physical &= ~(UINT64_C(1) << square);
            Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
                  "raw chess-piece removal preserves the nonempty free-plan observation owner");
        }
        const auto correction = session.HomeStateSnapshot();
        Check(session.State().physical_occupied_mask == (UINT64_C(1) << 44U) &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::DuckRelocation) &&
                  session.State().required_action ==
                      static_cast<uint8_t>(runtime::RequiredPhysicalAction::CorrectBoard) &&
                  session.State().correction_mask == OPENING &&
                  correction.action == modules::HomeStateAction::Correction &&
                  correction.duckRelocationOwner == 'h' && correction.vexMoveFirst,
              "singleton Duck retains its actual required free-plan correction and Home authority");
        Feed(session, Tilt(), 1600U);
        Check(!session.WaterSloshActive() && !session.HintDisplayRequested(),
              "required plan correction preempts singleton water and suppresses automatic Hint");
        Checkpoint(session, "singleton_required_plan_correction", "restore_missing_chess_pieces_or_clear_all");
        Check(session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK,
              "final Duck removal reaches explicit all-clear cancellation");
        const auto cleared = session.HomeStateSnapshot();
        Check(cleared.freshSetup && cleared.placementMode &&
                  cleared.duckSquare == modules::INVALID_DUCK_SQUARE &&
                  cleared.duckRelocationOwner == '\0' &&
                  session.State().interaction_phase ==
                      static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle),
              "zero retires the pregame free plan into ordinary Setup");
        Feed(session, Tilt(), 1600U);
        Check(session.WaterSloshActive(),
              "cleared Setup recovers ordinary motion handling after the required plan retires");
        Checkpoint(session, "required_plan_cleared", "arrange_opening_pieces");
    }
}

void VerticalAndFoamOutputJourney() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession session(config);
    session.ObserveHallMask(0ULL);
    Feed(session, {0, 0, 0, 0, -240, 1500}, 1000U);
    const auto up = session.PhysicalFrame();
    Feed(session, {0, 0, 0, 0, -240, 400}, 1000U);
    const auto down = session.PhysicalFrame();
    uint32_t upGreen = 0U, upBlue = 0U, downGreen = 0U, downBlue = 0U;
    for (uint16_t i = 0U; i < 288U; ++i) {
        upGreen += up.pixels[i].g + up.pixels[i].w;
        upBlue += up.pixels[i].b + up.pixels[i].w;
        downGreen += down.pixels[i].g + down.pixels[i].w;
        downBlue += down.pixels[i].b + down.pixels[i].w;
    }
    Check(uint64_t(upGreen) * downBlue > uint64_t(downGreen) * upBlue,
          "normal-force aqua-to-indigo tint survives the final RGBW/power pipeline");
    for (unsigned i = 0U; i < 24U; ++i) {
        const int32_t sign = (i % 2U) == 0U ? 1 : -1;
        Feed(session, {0, 0, 0, sign * 300, sign * 500, 1000 + sign * 450}, 40U);
    }
    unsigned pale = 0U;
    const auto shaken = session.PhysicalFrame();
    for (uint16_t i = 0U; i < 288U; ++i) {
        const auto c = shaken.pixels[i];
        pale += c.r + c.w > 25U && c.g + c.w > 30U && c.b + c.w > 35U;
    }
    Check(pale > 8U && pale < 180U, "shake foam remains scattered pale output through physical RGBW");
    Checkpoint(session, "shake_foam", "stop_shaking_to_settle");
    Feed(session, Level(), 3000U);
    Check(!session.WaterSloshActive(), "foam and pressure cannot keep a resting episode alive");
}

enum class RequiredOwner { Health, Sleep, Storage };

virtual_litch_physical_frame PreemptedFrame(const bool withWater,
                                           const RequiredOwner owner) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::virtual_litch_internal::Session session(config,
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney);
    session.DispatchBoardObservation(0ULL, 0ULL);
    Feed(session, withWater ? Tilt() : Level(), 500U);
    Check(session.WaterSloshActive() == withWater,
          "required-owner comparison starts with and without a liquid episode");
    if (owner == RequiredOwner::Sleep) {
        Check(session.RequestSleep(static_cast<uint8_t>(firmware::SleepRequestOrigin::Console)) ==
                  VIRTUAL_LITCH_OK, "sleep owns the canonical perimeter");
    } else if (owner == RequiredOwner::Health) {
        virtual_litch_runtime_health_fact fact{};
        fact.struct_size = sizeof(fact);
        fact.abi_version = VIRTUAL_LITCH_RUNTIME_HEALTH_ABI_VERSION;
        fact.kind = static_cast<uint8_t>(runtime::RuntimeHealthFactKind::Brownout);
        fact.brownout_detected = 1U;
        Check(session.DispatchRuntimeHealthFact(fact) == VIRTUAL_LITCH_OK,
              "health owns the canonical perimeter");
    } else {
        session.SettingsForPersistenceTest().ReportAsyncSaveFailure(
            modules::SettingsStoreError::WriteFailed);
        session.ObserveMotionSample(Tilt());
        Check(session.RuntimeHealth().state == VIRTUAL_LITCH_RUNTIME_HEALTH_HEALTHY,
              "storage status remains required even without a separate runtime health fault");
    }
    Check(!session.WaterSloshActive(), "required owner cancels liquid in the same frame");
    return session.PhysicalFrame();
}

void RequiredStatusOwners() {
    for (const auto owner : {RequiredOwner::Health, RequiredOwner::Sleep,
                            RequiredOwner::Storage}) {
        const auto ordinary = PreemptedFrame(false, owner);
        const auto afterWater = PreemptedFrame(true, owner);
        for (uint16_t pixel = 256U; pixel < 288U; ++pixel) {
            Check(SameColor(afterWater.pixels[pixel], ordinary.pixels[pixel]),
                  "required status owner gets its exact final RGBW frame with no residual water");
        }
    }
}

void ApplyAppCoaching(runtime::VirtualLitchSession& session,
                      const uint8_t owner, const bool enabled) {
    virtual_litch_action_request request{};
    request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_ABI_VERSION;
    request.action = VIRTUAL_LITCH_CONTROL_COACHING;
    request.owner = owner;
    request.coaching_mutation = static_cast<uint8_t>(enabled
        ? modules::CoachingMutation::Enable : modules::CoachingMutation::Disable);
    modules::ControlMutationOutcome outcome;
    Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "app-style coaching mutation uses the shared authoritative callback");
}

void AppCoachingAcknowledgement() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    session.ObserveHallMask(0ULL);
    Feed(session, Tilt(), 500U);
    Check(session.WaterSloshActive(), "app coaching journey starts a liquid episode");
    for (const bool enabled : {true, false}) {
        // Setup keeps automatic Hint suppressed on either side. The latch and
        // acknowledgement still use their ordinary control owner.
        ApplyAppCoaching(session, VIRTUAL_LITCH_OWNER_P2, enabled);
        Check(!session.WaterSloshActive(),
              "coaching enable and disable cancel water immediately for their acknowledgement");
        Feed(session, Tilt(), 1100U);
        Check(!session.WaterSloshActive(),
              "app coaching acknowledgement retains the full control quiet interval");
        Feed(session, Tilt(), 400U);
        Check(!session.HintDisplayRequested(), "inactive-side badge has no required Hint owner");
        Check(session.WaterSloshActive(),
              "handling resumes after acknowledgement with the idle coaching badge enabled or disabled");
        Checkpoint(session, enabled ? "coaching_enabled_reentry" : "coaching_disabled_reentry",
                   "move_board_or_place_first_piece");
    }
    ApplyAppCoaching(session, VIRTUAL_LITCH_OWNER_P1, true);
    Feed(session, Tilt(), 1500U);
    Check(session.PlacementModeActive() && !session.HintDisplayRequested() &&
              session.WaterSloshActive(),
          "active-side Coaching stays suppressed in empty Setup after its acknowledgement");
    Checkpoint(session, "active_side_setup_coaching", "arrange_opening_pieces");
    Check(session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
              session.Advance(199U) == VIRTUAL_LITCH_OK && session.PlacementModeActive() &&
              session.Advance(1U) == VIRTUAL_LITCH_OK && !session.PlacementModeActive(),
          "complete raw opening reaches the existing 200ms playable handoff");
    Feed(session, Tilt(), 2000U);
    Check(session.HomeStateSnapshot().action == modules::HomeStateAction::Idle &&
              session.HintDisplayRequested() && !session.WaterSloshActive(),
          "genuinely eligible human turn requests automatic Hint beyond the Ready acknowledgement");
    Checkpoint(session, "playable_turn_automatic_hint", "play_or_follow_current_hint");
    ApplyAppCoaching(session, VIRTUAL_LITCH_OWNER_P1, false);
    Check(session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK,
          "clearing the unplayed opening returns the same session to Setup");
    Feed(session, Tilt(), 1500U);
    Check(session.PlacementModeActive() && !session.HintDisplayRequested() &&
              session.WaterSloshActive(),
          "Hint-off Setup handling recovers after controls become quiet");
    Checkpoint(session, "coaching_turn_cleared", "arrange_opening_pieces");
}

void HealthAndSleepPreemption() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::virtual_litch_internal::Session session(config,
        runtime::virtual_litch_internal::SessionMode::EngineBackedJourney);
    session.DispatchBoardObservation(0ULL, 0ULL);
    Feed(session, Tilt(), 500U);
    Check(session.WaterSloshActive(), "health journey starts real liquid owner");
    virtual_litch_runtime_health_fact fact{};
    fact.struct_size = sizeof(fact);
    fact.abi_version = VIRTUAL_LITCH_RUNTIME_HEALTH_ABI_VERSION;
    fact.kind = static_cast<uint8_t>(runtime::RuntimeHealthFactKind::Brownout);
    fact.brownout_detected = 1U;
    Check(session.DispatchRuntimeHealthFact(fact) == VIRTUAL_LITCH_OK,
          "power protection enters production health reducer");
    Check(!session.WaterSloshActive(), "protection preempts liquid immediately");
    Check(session.Reset() == VIRTUAL_LITCH_OK, "health fixture resets cleanly");
    session.DispatchBoardObservation(0ULL, 0ULL);
    Feed(session, Tilt(), 500U);
    Check(session.RequestSleep(static_cast<uint8_t>(firmware::SleepRequestOrigin::Console)) ==
              VIRTUAL_LITCH_OK, "sleep request enters production sleep owner");
    Check(!session.WaterSloshActive(), "sleep countdown takes precedence over liquid");
}

} // namespace

int main() {
    PhysicalJourney(false);
    PhysicalJourney(true);
    PerimeterDirections();
    AmbientHighlightJourney();
    VexRiderJourney();
    RequiredDuckPlanPreemptsWater();
    VerticalAndFoamOutputJourney();
    HealthAndSleepPreemption();
    RequiredStatusOwners();
    AppCoachingAcknowledgement();
    std::printf("[WATER_JOURNEY] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
