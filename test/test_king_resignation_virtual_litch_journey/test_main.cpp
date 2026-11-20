#include "bounded_hint_engine.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "hardware/board_led_output_mapping.hpp"
#include "hardware/status_led_chain_profile.hpp"
#include "modules/game_persistence.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/orientation.hpp"
#include "modules/settings_store.hpp"

namespace {
int g_failures = 0;
constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] KING_RESIGNATION_JOURNEY: %s\n", message);
        ++g_failures;
    }
}

bool Observe(runtime::VirtualLitchSession& session, uint64_t logical) {
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    uint64_t physical = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square)
        if ((logical & (UINT64_C(1) << square)) != 0ULL)
            physical |= UINT64_C(1) << modules::MapSquareToPhysical(square, rotate);
    return session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK;
}

uint8_t PhysicalOwner(const runtime::VirtualLitchSession& session, bool black) {
    const bool swap = modules::ComputeControlBankSwap(
        session.BoardRotate(), session.FirstPlayerIsP2(), false);
    return black != swap ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
}

unsigned ColorEnergy(const virtual_litch_color color) {
    return color.r + color.g + color.b + color.w;
}

unsigned CircleEnergy(const runtime::VirtualLitchSession& session, bool black) {
    // PCB: P2 O is LED261 (chain 4); P1 O is LED283 (chain 26).
    const uint8_t index = PhysicalOwner(session, black) == VIRTUAL_LITCH_OWNER_P2
        ? 4U : 26U;
    return ColorEnergy(session.PhysicalFrame().pixels[256U + index]);
}

bool OnlyOwningCircleLit(const runtime::VirtualLitchSession& session, bool black) {
    const uint8_t expected = PhysicalOwner(session, black) == VIRTUAL_LITCH_OWNER_P2
        ? 4U : 26U;
    for (bool physicalP2 : {false, true}) {
        const auto range = physicalP2 ? hardware::V3_3_P2_CONTROL_LED_RANGE
                                     : hardware::V3_3_P1_CONTROL_LED_RANGE;
        for (uint8_t offset = 0U; offset < range.count; ++offset) {
            const uint8_t index = range.first + offset;
            const auto pixel = session.PhysicalFrame().pixels[256U + index];
            if ((pixel.r + pixel.g + pixel.b + pixel.w > 0U) != (index == expected))
                return false;
        }
    }
    return true;
}

bool Down(runtime::VirtualLitchSession& session, bool black,
          runtime::VirtualLitchPhysicalButton button, bool down) {
    return session.SetButtonDown(PhysicalOwner(session, black), button, down) == VIRTUAL_LITCH_OK;
}

void Tap(runtime::VirtualLitchSession& session, bool black,
         runtime::VirtualLitchPhysicalButton button) {
    Check(Down(session, black, button, true) && session.Advance(20U) == VIRTUAL_LITCH_OK &&
              Down(session, black, button, false) && session.Advance(20U) == VIRTUAL_LITCH_OK &&
              Observe(session, session.State().physical_occupied_mask),
          "raw physical press/debounce/release reaches existing button and chord owners");
}

void Move(runtime::VirtualLitchSession& session, uint64_t& occupancy,
          uint8_t from, uint8_t to) {
    occupancy &= ~(UINT64_C(1) << from);
    Check(Observe(session, occupancy), "raw piece pickup admitted");
    occupancy |= UINT64_C(1) << to;
    Check(Observe(session, occupancy) && session.Advance(2600U) == VIRTUAL_LITCH_OK &&
              session.Advance(1U) == VIRTUAL_LITCH_OK,
          "raw destination completes real settle/action/acknowledgement");
}

void Checkpoint(const runtime::VirtualLitchSession& session, const char* stage,
                const char* next) {
    std::printf("[KING_RESIGNATION_JOURNEY] stage=%s t=%u rotate=%u first_p2=%u "
                "physical=%016llx expected=%016llx phase=%u action=%u text=%.*s "
                "white_circle=%u black_circle=%u history=%u home=%s next=%s\n",
        stage, session.State().now_ms, session.BoardRotate(), session.FirstPlayerIsP2(),
        static_cast<unsigned long long>(session.State().physical_occupied_mask),
        static_cast<unsigned long long>(session.State().expected_occupied_mask),
        session.State().interaction_phase, session.State().required_action,
        static_cast<int>(session.DisplayText().size()), session.DisplayText().data(),
        CircleEnergy(session, false), CircleEnergy(session, true),
        session.UndoHistoryMoveCount(), session.HomeStateLine().CStr(), next);
}

void Save(runtime::VirtualLitchSession& session) {
    bool saved = false;
    session.RunControlIngress([](void* context, modules::SettingsStore& settings,
        modules::ControlMutationService&, modules::GamePersistenceCoordinator&) {
        *static_cast<bool*>(context) = settings.SaveNow();
    }, &saved);
    Check(saved, "native save persists the coordinator-prepared document");
}

void Terminal(runtime::VirtualLitchSession& session, bool black, const char* stage) {
    const auto home = session.HomeStateSnapshot();
    Check(static_cast<uint8_t>(home.outcome.reason) == 3U &&
              home.outcome.winner == (black ? modules::GameWinner::White : modules::GameWinner::Black) &&
              home.clockWinner == modules::GameWinner::None &&
              home.action == modules::HomeStateAction::Idle && !home.freshSetup &&
              !home.placementMode && home.cpuThinking.side == '\0' &&
              !home.physicalMoveValid && home.assistance.presentation == modules::HomeAssistancePresentation::Hidden &&
              !session.GameClock()->IsRunning() && !session.GameClock()->HasTimedOut(),
          "explicit resignation stops clock and move guidance without inventing timeout/checkmate");
    Checkpoint(session, stage, "Undo-or-hold-X+Circle-for-New-Game");
}

void ConfirmResignationAndCheckFireLob(runtime::VirtualLitchSession& session, bool black) {
    using hardware::SquareLedSlot;
    const uint8_t loser = black ? 4U : 60U;
    const uint8_t winner = black ? 60U : 4U;
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    const auto colorAt = [&](uint8_t square, SquareLedSlot slot) {
        return session.SemanticFrame().board[square][static_cast<uint8_t>(slot)];
    };
    const auto mappedLightIs = [&](uint8_t square, SquareLedSlot slot, bool lit) {
        const auto targets = hardware::MapSemanticSquareLedTargets(
            hardware::BoardRevision::V3_3S3, square, slot, rotate);
        for (uint8_t index = 0U; index < targets.count; ++index) {
            if ((ColorEnergy(session.PhysicalFrame().pixels[targets.indices[index]]) > 0U) != lit)
                return false;
        }
        return true;
    };
    const auto warmth = [](virtual_litch_color color) {
        return static_cast<int>(color.r) - color.b;
    };
    const auto prior = colorAt(winner, SquareLedSlot::Piece);
    const auto checkpoint = [&](const char* stage, uint32_t age) {
        const auto winningPiece = colorAt(winner, SquareLedSlot::Piece);
        const auto resignedPiece = colorAt(loser, SquareLedSlot::Piece);
        const auto left = colorAt(loser, SquareLedSlot::Square0);
        const auto right = colorAt(loser, SquareLedSlot::Square2);
        std::printf("[RESIGNATION_FIRE_LOB] stage=%s age=%u winner=%s target=%s "
                    "prior_rgb=%u/%u/%u winner_rgb=%u/%u/%u target_piece_rgb=%u/%u/%u "
                    "target_left_rgb=%u/%u/%u target_right_rgb=%u/%u/%u mapped_winner_lit=%u\n",
                    stage, age, black ? "e1" : "e8", black ? "e8" : "e1",
                    prior.r, prior.g, prior.b, winningPiece.r, winningPiece.g, winningPiece.b,
                    resignedPiece.r, resignedPiece.g, resignedPiece.b,
                    left.r, left.g, left.b, right.r, right.g, right.b,
                    mappedLightIs(winner, SquareLedSlot::Piece, true) ? 1U : 0U);
    };

    Tap(session, black, runtime::VirtualLitchPhysicalButton::O);
    Terminal(session, black, "fresh-confirmation");
    checkpoint("confirmed", 0U);
    const auto confirmed = colorAt(winner, SquareLedSlot::Piece);
    Check(ColorEnergy(confirmed) != 0U && confirmed.r == prior.r && confirmed.g == prior.g &&
              confirmed.b == prior.b && mappedLightIs(winner, SquareLedSlot::Piece, true),
          "the winning king retains its player color on confirmation");
    Check(mappedLightIs(loser, SquareLedSlot::Piece, false),
          "the resigned king is absent and its mapped piece LEDs stay dark on confirmation");
    const int leftBefore = warmth(colorAt(loser, SquareLedSlot::Square0));
    const int rightBefore = warmth(colorAt(loser, SquareLedSlot::Square2));

    constexpr uint32_t IMPACT_SAMPLE_MS = 910U;
    Check(session.Advance(IMPACT_SAMPLE_MS) == VIRTUAL_LITCH_OK, "sample the first Fire Lob impact");
    checkpoint("impact", IMPACT_SAMPLE_MS);
    Check(warmth(colorAt(loser, SquareLedSlot::Square0)) > leftBefore + 30 &&
              warmth(colorAt(loser, SquareLedSlot::Square2)) > rightBefore + 30 &&
              mappedLightIs(loser, SquareLedSlot::Square0, true) &&
              mappedLightIs(loser, SquareLedSlot::Square2, true),
          "Fire Lob lands on both mapped square LEDs of the resigned king's square");
    Check(mappedLightIs(winner, SquareLedSlot::Piece, true),
          "Fire Lob impact keeps both of the winning king's mapped piece LEDs lit");
    Check(mappedLightIs(loser, SquareLedSlot::Piece, false),
          "Fire Lob impact keeps both piece LEDs beneath the missing king dark");
}

void BoundaryJourney(bool rotate, bool firstP2, bool black) {
    char path[128]{};
    std::snprintf(path, sizeof(path), "build/king_resignation_%u_%u_%u.json", rotate, firstP2, black);
    (void)std::remove(path);
    (void)std::remove((std::string(path) + ".bak").c_str());
    (void)std::remove((std::string(path) + ".tmp").c_str());
    { // Existing preferences before boot avoid an unrelated K/Q orientation prompt.
        modules::SettingsStoreConfig config; config.path = path;
        modules::SettingsStore settings(config);
        Check(settings.Load(), "native preference store loads");
        settings.Doc()["BOARD_ROTATE"] = rotate;
        settings.Doc()["GAME_FIRST_PLAYER"] = firstP2 ? 2U : 1U;
        settings.Doc()["BOARD_SETUP_ASSISTANCE"] = false;
        settings.Doc()["TIMER"] = 1U;
        Check(settings.SaveNow(), "pre-boot orientation and timer persist");
    }
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config, path);
    auto& session = fixture.session;
    Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) &&
              session.Advance(1000U) == VIRTUAL_LITCH_OK,
          "raw Hall setup reaches playable opening");
    // Existing start-only orientation mutation reseeds held GPIO ownership.
    const uint8_t originalBank = PhysicalOwner(session, false);
    Check(session.SetButtonDown(originalBank, runtime::VirtualLitchPhysicalButton::O, true) == VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK && session.Advance(800U) == VIRTUAL_LITCH_OK, "opening Circle held beyond chord buffer before orientation reset");
    modules::ControlMutationOutcome orientation;
    const auto rotateResult = session.MutateSettingJson(81U, "BOARD_ROTATE", rotate ? "false" : "true", orientation);
    std::printf("[KING_RESIGNATION_ORIENTATION] rotate=%u first_p2=%u first_rc=%u status=%s reason=%s\n", rotate, firstP2,
        static_cast<unsigned>(rotateResult), modules::ControlMutationStatusToString(orientation.status), modules::ControlMutationReasonToString(orientation.reason));
    Check(rotateResult == VIRTUAL_LITCH_OK && orientation.status == modules::ControlMutationStatus::Applied &&
              session.Advance(1U) == VIRTUAL_LITCH_OK, "legal opening orientation mutation accepted while Circle held");
    const auto rotateBackResult = session.MutateSettingJson(82U, "BOARD_ROTATE", rotate ? "true" : "false", orientation);
    std::printf("[KING_RESIGNATION_ORIENTATION] second_rc=%u status=%s reason=%s\n", static_cast<unsigned>(rotateBackResult),
        modules::ControlMutationStatusToString(orientation.status), modules::ControlMutationReasonToString(orientation.reason));
    Check(rotateBackResult == VIRTUAL_LITCH_OK && orientation.status == modules::ControlMutationStatus::Applied &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.SetButtonDown(originalBank, runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK, "orientation reseed discards old logical held/release state");
    const uint8_t king = black ? 4U : 60U;
    const uint64_t kingBit = UINT64_C(1) << king;
    Check(Observe(session, OPENING & ~kingBit) && session.Advance(5000U) == VIRTUAL_LITCH_OK &&
              CircleEnergy(session, black) == 0U && !session.HomeStateSnapshot().outcome.IsTerminal(),
          "Fresh Setup king pickup cannot arm resignation");
    Check(Observe(session, OPENING) && session.Advance(1000U) == VIRTUAL_LITCH_OK,
          "returning setup king restores ordinary setup");
    uint64_t occupancy = OPENING;
    Move(session, occupancy, 52U, 36U);
    Check(session.UndoHistoryMoveCount() == 1U, "e2-e4 commits real game history");
    Check(Observe(session, occupancy & ~kingBit), "known acknowledged king lifted through raw Hall");
    Check(session.Advance(2500U) == VIRTUAL_LITCH_OK && CircleEnergy(session, black) > 0U,
          "king hold exposes restrained proportional Circle progress");
    const unsigned halfProgress = CircleEnergy(session, black);
    Check(session.Advance(2499U) == VIRTUAL_LITCH_OK &&
              !session.HomeStateSnapshot().outcome.IsTerminal() &&
              CircleEnergy(session, black) > halfProgress,
          "4999 ms increases steady progress without confirming resignation");
    Checkpoint(session, "4999", "continue-holding-king");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK && OnlyOwningCircleLit(session, black) &&
              !session.HomeStateSnapshot().outcome.IsTerminal(),
          "5000 ms arms only the king owner's actual physical Circle without auto-resigning");
    Checkpoint(session, "5000", "press-owner-Circle-or-return-king");
    Check(session.Advance(500U) == VIRTUAL_LITCH_OK && CircleEnergy(session, black) == 0U &&
              session.Advance(500U) == VIRTUAL_LITCH_OK && OnlyOwningCircleLit(session, black),
          "armed Circle flashes on/off through real timed physical frames");
    Tap(session, !black, runtime::VirtualLitchPhysicalButton::O);
    Check(!session.HomeStateSnapshot().outcome.IsTerminal(), "opposite owner Circle cannot resign this king");
    Check(Observe(session, occupancy) && CircleEnergy(session, black) == 0U,
          "king return cancels the gesture immediately");
    Tap(session, black, runtime::VirtualLitchPhysicalButton::O);
    Check(!session.HomeStateSnapshot().outcome.IsTerminal(), "post-cancel Circle cannot commit a stale resignation");
    Check(Observe(session, occupancy & ~kingBit) && session.Advance(5000U) == VIRTUAL_LITCH_OK,
          "new king pickup starts a new independent hold");
    Check(Observe(session, occupancy & ~kingBit & ~(UINT64_C(1) << 12U)), "second missing piece observed before Circle");
    Tap(session, black, runtime::VirtualLitchPhysicalButton::O);
    Check(!session.HomeStateSnapshot().outcome.IsTerminal() && CircleEnergy(session, black) == 0U,
          "multi-removal cancels before fresh Circle can commit");
    // Production order is buttons first, then the next admitted Hall scan.
    for (bool additionalPiece : {false, true}) {
        Check(Observe(session, occupancy) && Observe(session, occupancy & ~kingBit) &&
                  session.Advance(5000U) == VIRTUAL_LITCH_OK, "button-first safety case arms a fresh lift");
        Check(Down(session, black, runtime::VirtualLitchPhysicalButton::O, true) && session.Advance(20U) == VIRTUAL_LITCH_OK &&
                  Down(session, black, runtime::VirtualLitchPhysicalButton::O, false) && session.Advance(20U) == VIRTUAL_LITCH_OK &&
                  !session.HomeStateSnapshot().outcome.IsTerminal(), "Circle intent waits for a fresh admitted Hall scan");
        const uint64_t nextMask = additionalPiece ? occupancy & ~kingBit & ~(UINT64_C(1) << 12U) : occupancy;
        Check(Observe(session, nextMask) && !session.HomeStateSnapshot().outcome.IsTerminal() && CircleEnergy(session, black) == 0U,
                  "button-first king return or second mismatch cancels pending confirmation");
        Checkpoint(session, additionalPiece ? "button-first-second-removal" : "button-first-king-return", "restore-board-and-restart-hold");
    }
    Check(Observe(session, occupancy) && Observe(session, occupancy & ~kingBit), "correction restores board and restarts king hold");
    Check(session.Advance(4979U) == VIRTUAL_LITCH_OK &&
              Down(session, black, runtime::VirtualLitchPhysicalButton::O, true) &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              session.Advance(751U) == VIRTUAL_LITCH_OK && !session.HomeStateSnapshot().outcome.IsTerminal(),
          "pre-boundary Circle press buffered/replayed after5000 cannot confirm while held");
    Check(Down(session, black, runtime::VirtualLitchPhysicalButton::O, false) &&
              session.Advance(20U) == VIRTUAL_LITCH_OK && !session.HomeStateSnapshot().outcome.IsTerminal(),
          "held Circle release does not confirm");
    Checkpoint(session, "held-before-boundary-released", "fresh-owner-Circle-required");
    const modules::FenString committed(session.HomeStateSnapshot().fen);
    ConfirmResignationAndCheckFireLob(session, black);
    modules::GamePersistenceState durable;
    modules::DuckGameDurableState variant;
    Check(modules::ReadGamePersistenceState(session.SettingsForPersistenceTest().Doc(), durable) &&
              modules::DecodeDuckGameDurableState(durable.variantState.CStr(), variant) &&
              static_cast<uint8_t>(variant.outcome.reason) == 3U && durable.fen.Equals(committed.View()) &&
              durable.undoHistoryCount == modules::UNDO_HISTORY_FIELDS_PER_MOVE &&
              durable.interactionState.Empty() && !durable.cpuMoveValid && !durable.clockState.clock.running,
          "durable resignation preserves canonical FEN/history and retires executable work");
    Save(session);
    Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, occupancy & ~kingBit),
          "cold native reboot and new Hall scan restore explicit resignation");
    Terminal(session, black, "reboot-restored");
    Check(Observe(session, occupancy), "replace king before existing Undo gesture");
    Tap(session, black, runtime::VirtualLitchPhysicalButton::X);
    Check(!session.HomeStateSnapshot().outcome.IsTerminal() && session.IsUndoActionPending() &&
              session.GameClock()->IsRunning() && session.GameClock()->IsPaused(),
          "successful existing Undo clears resignation only after engine acceptance");
    Tap(session, black, runtime::VirtualLitchPhysicalButton::X);
    Terminal(session, black, "cancel-Undo-restores-result");
    Tap(session, black, runtime::VirtualLitchPhysicalButton::X);
    Move(session, occupancy, 36U, 52U);
    Check(!session.HomeStateSnapshot().outcome.IsTerminal() && session.UndoHistoryMoveCount() == 0U &&
              session.GameClock()->IsRunning() && !session.GameClock()->IsPaused(),
          "physical inverse completes terminal Undo and retires result");
    Checkpoint(session, "Undo-complete", "play-or-New-Game");
    // Ordinary active game again, then test the existing destructive chord.
    Move(session, occupancy, 52U, 36U);
    Check(Observe(session, occupancy & ~kingBit) && session.Advance(5000U) == VIRTUAL_LITCH_OK,
          "later real game can arm another king hold");
    Tap(session, black, runtime::VirtualLitchPhysicalButton::O);
    Terminal(session, black, "before-New-Game");
    Check(Down(session, black, runtime::VirtualLitchPhysicalButton::X, true) &&
              Down(session, black, runtime::VirtualLitchPhysicalButton::O, true) &&
              session.Advance(20U) == VIRTUAL_LITCH_OK && session.Advance(2000U) == VIRTUAL_LITCH_OK &&
              Down(session, black, runtime::VirtualLitchPhysicalButton::X, false) &&
              Down(session, black, runtime::VirtualLitchPhysicalButton::O, false) &&
              session.Advance(20U) == VIRTUAL_LITCH_OK && !session.HomeStateSnapshot().outcome.IsTerminal() &&
              session.UndoHistoryMoveCount() == 0U,
          "same-bank X+Circle chord retains New Game priority and clears durable terminal/history");
    Checkpoint(session, "New-Game", "arrange-opening");
}
void OrdinaryKingAndCastleJourney() {
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    constexpr const char* fen = "4k3/8/8/8/8/8/8/4K2R w K - 0 1";
    const uint64_t initial = (UINT64_C(1) << 4U) | (UINT64_C(1) << 60U) | (UINT64_C(1) << 63U);
    Check(session.LoadExternalFen(fen) && Observe(session, initial) && session.Advance(1000U) == VIRTUAL_LITCH_OK, "ordinary king fixture enters production external import and raw Hall");
    uint64_t occupancy = initial;
    Check(Observe(session, occupancy & ~(UINT64_C(1) << 60U)) && session.Advance(4999U) == VIRTUAL_LITCH_OK &&
              !session.HomeStateSnapshot().outcome.IsTerminal(), "ordinary king pickup remains unarmed before five seconds");
    Move(session, occupancy, 60U, 59U);
    Check(session.UndoHistoryMoveCount() == 1U && !session.HomeStateSnapshot().outcome.IsTerminal() && CircleEnergy(session, false) == 0U &&
              std::strstr(session.HomeStateSnapshot().fen, "3K3R") != nullptr,
          "ordinary legal king relocation commits and clears hold cue");
    Checkpoint(session, "ordinary-king-move", "play-next-turn");
    Check(session.LoadExternalFen(fen) && Observe(session, initial) && session.Advance(1000U) == VIRTUAL_LITCH_OK, "castle fixture uses existing authorized position import");
    occupancy = initial & ~(UINT64_C(1) << 60U);
    Check(Observe(session, occupancy) && session.Advance(4999U) == VIRTUAL_LITCH_OK, "castle king pickup keeps ordinary guidance before arm boundary");
    occupancy |= UINT64_C(1) << 62U;
    Check(Observe(session, occupancy) && session.Advance(5000U) == VIRTUAL_LITCH_OK && CircleEnergy(session, false) == 0U &&
              session.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::LiftSecondary) &&
              !session.HomeStateSnapshot().outcome.IsTerminal(), "king placement retains castle rook next action and cancels resignation timer");
    Checkpoint(session, "castle-rook-next-action", "move-h1-rook-to-f1");
    Move(session, occupancy, 63U, 61U);
    Check(session.UndoHistoryMoveCount() == 1U && !session.HomeStateSnapshot().outcome.IsTerminal() &&
              std::strstr(session.HomeStateSnapshot().fen, "5RK1") != nullptr,
          "complete physical castle commits normal king and rook position");
    Checkpoint(session, "castle-complete", "play-next-turn");
}

void DuckKingJourney() {
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    uint64_t occupancy = OPENING | (UINT64_C(1) << 24U);
    Check(Observe(session, occupancy) && session.Advance(1000U) == VIRTUAL_LITCH_OK, "raw opening plus neutral Duck enters variant-selectable setup");
    Move(session, occupancy, 52U, 36U);
    Check(session.HomeStateSnapshot().variant == modules::GameVariant::Duck && session.HomeStateSnapshot().duckRelocationOwner == 'h',
          "real chess half stages mandatory Duck relocation");
    const modules::FenString before(session.HomeStateSnapshot().fen);
    Check(Observe(session, occupancy & ~(UINT64_C(1) << 60U)) && session.Advance(5000U) == VIRTUAL_LITCH_OK && CircleEnergy(session, false) == 0U,
          "king lift during staged Duck half cannot arm or steal relocation control");
    Tap(session, false, runtime::VirtualLitchPhysicalButton::O);
    Check(!session.HomeStateSnapshot().outcome.IsTerminal() && session.UndoHistoryMoveCount() == 0U && before.Equals(session.HomeStateSnapshot().fen),
          "Circle during invalid staged Duck input cannot resign or commit partial turn");
    Checkpoint(session, "staged-Duck-king-excluded", "return-king-then-relocate-Duck");
    Check(Observe(session, occupancy), "return king repairs staged Duck occupancy");
    Move(session, occupancy, 24U, 26U);
    Tap(session, false, runtime::VirtualLitchPhysicalButton::O);
    Check(session.UndoHistoryMoveCount() == 1U && session.HomeStateSnapshot().duckRelocationOwner == '\0',
          "mandatory Duck acknowledgement commits one whole turn");
    Check(Observe(session, occupancy & ~(UINT64_C(1) << 60U)) && session.Advance(5000U) == VIRTUAL_LITCH_OK && OnlyOwningCircleLit(session, false),
          "active Duck acknowledged king arms normal owning Circle");
    Tap(session, false, runtime::VirtualLitchPhysicalButton::O);
    Terminal(session, false, "active-Duck-resigned");
    Check(session.HomeStateSnapshot().variant == modules::GameVariant::Duck && session.UndoHistoryMoveCount() == 1U,
          "Duck resignation retains variant and complete combined turn history");
}

void CpuQuietKingJourney() {
    const char* path = "build/king_resignation_cpu_quiet.json";
    (void)std::remove(path);
    (void)std::remove((std::string(path) + ".bak").c_str());
    (void)std::remove((std::string(path) + ".tmp").c_str());
    modules::SettingsStoreConfig storeConfig; storeConfig.path = path;
    modules::SettingsStore settings(storeConfig);
    Check(settings.Load(), "CPU quiet king fixture settings load");
    settings.Doc()["VS_CPU_LEVEL"] = 1U;
    settings.Doc()["VS_CPU_PLAYER"] = 1U;
    settings.Doc()["BOARD_SETUP_ASSISTANCE"] = false;
    Check(settings.SaveNow(), "CPU quiet king seat persists before boot");
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config, path);
    auto& session = fixture.session;
    Check(session.LoadExternalFen("4k3/8/p7/8/8/8/8/4K3 w - - 0 1"),
          "production external-position import establishes CPU king-only legal moves");
    const uint64_t occupancy = (UINT64_C(1) << 4U) | (UINT64_C(1) << 16U) | (UINT64_C(1) << 60U);
    Check(Observe(session, occupancy), "raw Hall acknowledges CPU fixture position");
    for (uint8_t i = 0U; i < 100U && !session.HomeStateSnapshot().physicalMoveValid; ++i)
        Check(session.Advance(100U) == VIRTUAL_LITCH_OK, "production CPU calculation advances");
    Check(session.HomeStateSnapshot().physicalMoveValid && session.HomeStateSnapshot().physicalFromSquare == 60U,
          "actual CPU result requests a legal quiet king move");
    Check(Observe(session, occupancy & ~(UINT64_C(1) << 60U)) && session.Advance(5000U) == VIRTUAL_LITCH_OK &&
              OnlyOwningCircleLit(session, false), "CPU quiet king lift owns physical Circle above directed move overlay");
    Checkpoint(session, "CPU-quiet-king-armed", "fresh-white-Circle-or-complete-king-move");
    Tap(session, false, runtime::VirtualLitchPhysicalButton::O);
    Terminal(session, false, "CPU-quiet-king-resigned");
    Tap(session, false, runtime::VirtualLitchPhysicalButton::X);
    Terminal(session, false, "no-history-Undo-rejected");
}

} // namespace

int main() {
    for (bool rotate : {false, true})
        for (bool firstP2 : {false, true})
            for (bool black : {false, true})
                BoundaryJourney(rotate, firstP2, black);
    OrdinaryKingAndCastleJourney();
    DuckKingJourney();
    CpuQuietKingJourney();
    return g_failures == 0 ? 0 : 1;
}
