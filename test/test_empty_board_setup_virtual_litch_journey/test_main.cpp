#include <cstdio>

#include "bounded_hint_engine.hpp"
#include "hardware/led_mapping.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/game_clock.hpp"

namespace {

constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t E2 = 52U;
constexpr uint8_t E4 = 36U;
int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::printf("[FAIL] EMPTY_BOARD_SETUP_JOURNEY: %s\n", message);
    ++failures;
}

void Checkpoint(runtime::VirtualLitchSession& session,
                const char* stage, const char* next, const char* result = "observed") {
    const auto& state = session.State();
    const auto home = session.HomeStateSnapshot();
    std::printf("[EMPTY_BOARD_SETUP_JOURNEY] stage=%s t=%u occupancy=%016llx "
                "mode=%s phase=%s fresh=%u placement=%u action=%u token=%u "
                "history=%u result=%s illegal=%016llx correction=%016llx display=\"%.*s\" "
                "clock=%u/%u white=%u black=%u next=%s home=%s\n",
                stage, state.now_ms,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                session.Litchcraft().IsActive() ? "Litchcraft" : "Chess",
                runtime::ChessInteractionPhaseToString(
                    static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                unsigned(home.freshSetup), unsigned(home.placementMode),
                unsigned(home.action), state.pending_action_token,
                unsigned(session.UndoHistoryMoveCount()),
                result,
                static_cast<unsigned long long>(state.human_illegal_mask),
                static_cast<unsigned long long>(state.correction_mask),
                static_cast<int>(session.DisplayText().size()), session.DisplayText().data(),
                unsigned(home.clockRunning), unsigned(home.clockPaused),
                session.GameClock()->GetRemainingMs(runtime::ClockSide::White),
                session.GameClock()->GetRemainingMs(runtime::ClockSide::Black),
                next, session.HomeStateLine().CStr());
    uint64_t litCenters = 0ULL;
    const auto& frame = session.SemanticFrame();
    for (uint8_t square = 0U; square < 64U; ++square) {
        const auto c = frame.board[square][static_cast<uint8_t>(hardware::SquareLedSlot::Piece)];
        if (c.r || c.g || c.b || c.w) litCenters |= UINT64_C(1) << square;
    }
    std::printf("[EMPTY_BOARD_SETUP_LEDS] stage=%s lit_centers=%016llx status=",
                stage, static_cast<unsigned long long>(litCenters));
    for (uint8_t led = 0U; led < VIRTUAL_LITCH_STATUS_LED_COUNT; ++led) {
        const auto c = frame.status[led];
        std::printf("%s%u:%u/%u/%u/%u", led ? "," : "", led, c.r, c.g, c.b, c.w);
    }
    std::puts("");
}

void CheckEmptySetup(runtime::VirtualLitchSession& session, const char* stage) {
    Checkpoint(session, stage, "arrange-opening-pieces");
    const auto home = session.HomeStateSnapshot();
    Check(!session.Litchcraft().IsActive() && home.freshSetup && home.placementMode &&
              home.action == modules::HomeStateAction::Correction &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().pending_action_token == 0U,
          "empty untouched board returns ordinary Setup, with no illegal pickup or pending move");
    unsigned redStatus = 0U;
    for (const auto c : session.SemanticFrame().status) {
        if (c.r != 0U && c.g == 0U && c.b == 0U) ++redStatus;
    }
    Check(redStatus < 16U && session.DisplayText().empty(),
          "empty Setup has ordinary Turn guidance and no fault ring or error text");
    for (const auto& square : session.SemanticFrame().board) {
        const auto c = square[static_cast<uint8_t>(hardware::SquareLedSlot::Piece)];
        Check(c.r == 0U && c.g == 0U && c.b == 0U && c.w == 0U,
              "empty setup centers are black");
    }
}

virtual_litch_config Config() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    return config;
}

bool Set(runtime::VirtualLitchSession& session, const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return session.MutateSettingJson(100U, key, value, outcome) == VIRTUAL_LITCH_OK &&
        (outcome.status == modules::ControlMutationStatus::Applied ||
         outcome.status == modules::ControlMutationStatus::Noop);
}

bool Action(runtime::VirtualLitchSession& session, uint8_t action) {
    virtual_litch_action_request request{};
    virtual_litch_default_action_request(&request);
    request.action = action;
    request.owner = VIRTUAL_LITCH_OWNER_P1;
    request.undo_move_count = 1U;
    modules::ControlMutationOutcome outcome;
    const bool accepted = session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
        outcome.status == modules::ControlMutationStatus::Accepted;
    Checkpoint(session, action == VIRTUAL_LITCH_CONTROL_UNDO ? "undo-admitted" : "new-game-admitted",
               "complete-authoritative-action", accepted ? "accepted" : "rejected");
    return accepted && session.Advance(1U) == VIRTUAL_LITCH_OK;
}

void RunEmptyBoardSetup() {
    test_support::BoundedHintSession fixture(Config());
    auto& session = fixture.session;
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(OPENING & ~(UINT64_C(1) << E2)) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK,
          "partial opening can be cleared through raw Hall input");
    CheckEmptySetup(session, "partial-assembly-cleared");
    Check(session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
              session.Advance(2200U) == VIRTUAL_LITCH_OK &&
              !session.PlacementModeActive(), "replacement reaches Play Ready");
    Checkpoint(session, "play-ready-before-first-move", "remove-all-pieces");
    const auto revision = session.SettingsForPersistenceTest().GetRevision();
    const auto saves = session.SettingsForPersistenceTest().GetSaveCount();
    uint64_t hall = OPENING;
    // A legal first pickup followed by the remaining removals reproduces
    // clearing a ready board, rather than injecting a corrective phase.
    hall &= ~(UINT64_C(1) << E2);
    Check(session.ObserveHallMask(hall) == VIRTUAL_LITCH_OK, "lift e2 first");
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((hall & (UINT64_C(1) << square)) == 0ULL) continue;
        hall &= ~(UINT64_C(1) << square);
        Check(session.ObserveHallMask(hall) == VIRTUAL_LITCH_OK, "remove each remaining piece");
    }
    CheckEmptySetup(session, "ready-opening-cleared");
    const auto statusHash = session.SemanticFrame().status_hash;
    Check(session.Advance(80U) == VIRTUAL_LITCH_OK &&
              session.SemanticFrame().status_hash != statusHash,
          "returned empty Setup keeps the ordinary Turn indicator animated");
    Check(session.Advance(1600U) == VIRTUAL_LITCH_OK, "empty board remains stable");
    CheckEmptySetup(session, "empty-setup-still-idle");
    Check(session.SettingsForPersistenceTest().GetRevision() == revision &&
              session.SettingsForPersistenceTest().GetSaveCount() == saves,
          "ordinary clearing and stable empty ticks cause no settings revision or flash save");
    Check(session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
              session.Advance(199U) == VIRTUAL_LITCH_OK && session.PlacementModeActive() &&
              session.Advance(1U) == VIRTUAL_LITCH_OK && !session.PlacementModeActive(),
          "replacement reuses the production 200ms Setup stability handoff");
    Check(session.ObserveHallMask(OPENING | (UINT64_C(1) << 35U) | (UINT64_C(1) << 42U)) ==
              VIRTUAL_LITCH_OK && session.Advance(1600U) == VIRTUAL_LITCH_OK &&
              session.State().human_illegal_mask != 0ULL,
          "ordinary extra pieces still require correction");
    Checkpoint(session, "invalid-extra-pieces", "remove-unexpected-pieces", "rejected");
    Check(session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK, "clear malformed setup");
    CheckEmptySetup(session, "invalid-setup-cleared");
}

void RunEmptyDuckSetup(const bool freeRelocation) {
    constexpr uint8_t DUCK_E3 = 44U;
    test_support::BoundedHintSession fixture(Config());
    auto& session = fixture.session;
    const uint64_t openingWithDuck = OPENING | (UINT64_C(1) << DUCK_E3);
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(openingWithDuck) == VIRTUAL_LITCH_OK &&
              session.Advance(2200U) == VIRTUAL_LITCH_OK &&
              session.GameVariant() == modules::GameVariant::Duck &&
              session.DuckSquare() == DUCK_E3,
          "raw opening with e3 Duck latches the fresh Duck variant");
    if (freeRelocation) {
        Check(session.ObserveHallMask(openingWithDuck & ~(UINT64_C(1) << E2)) ==
                  VIRTUAL_LITCH_OK &&
                  session.ObserveHallMask(openingWithDuck) == VIRTUAL_LITCH_OK,
              "return the Duck-blocked e2 pawn through raw Hall input");
        bool relocation = false;
        session.RunControlIngress([](void* context, modules::SettingsStore&,
            modules::ControlMutationService&, modules::GamePersistenceCoordinator& game) {
            *static_cast<bool*>(context) = game.IsInitialDuckObstructionRelocationPromptActive();
        }, &relocation);
        Check(relocation && session.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::DuckRelocation),
              "free opening Duck relocation owns its real pending plan");
    }
    Checkpoint(session, freeRelocation ? "free-duck-relocation-before-clear" : "duck-opening-before-clear",
               "remove-chess-pieces-then-duck");
    uint64_t physical = openingWithDuck;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((OPENING & (UINT64_C(1) << square)) == 0ULL) continue;
        physical &= ~(UINT64_C(1) << square);
        Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
              "each chess-piece removal passes production ingress with Duck still present");
    }
    Check(session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK,
          "remove the tracked Duck last through production ingress");
    CheckEmptySetup(session, freeRelocation ? "free-duck-relocation-cleared" : "duck-removed-last");
    Check(session.GameVariant() == modules::GameVariant::Undecided &&
              session.DuckSquare() == modules::INVALID_DUCK_SQUARE &&
              session.State().expected_occupied_mask == OPENING &&
              session.State().interaction_phase !=
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::DuckRelocation),
          "empty Setup retires the Duck square, acknowledged occupancy and relocation plan together");
    const auto revision = session.SettingsForPersistenceTest().GetRevision();
    Check(session.Advance(2500U) == VIRTUAL_LITCH_OK &&
              session.SettingsForPersistenceTest().GetRevision() == revision &&
              session.UndoHistoryMoveCount() == 0U,
          "retired Duck stays empty without repeated saves or chess history mutation");
    Check(session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
              session.Advance(199U) == VIRTUAL_LITCH_OK && session.PlacementModeActive() &&
              session.Advance(1U) == VIRTUAL_LITCH_OK && !session.PlacementModeActive(),
          "32-piece replacement exits Setup at the existing 200 ms boundary without a phantom Duck");
    Checkpoint(session, freeRelocation ? "free-duck-clear-replaced" : "duck-clear-replaced",
               "play-first-chess-move");
}

void RunAbortedFirstMoveWithClockAndHints() {
    test_support::BoundedHintSession fixture(Config());
    auto& session = fixture.session;
    Check(session.Reset() == VIRTUAL_LITCH_OK && Set(session, "TIMER", "1") &&
              Set(session, "HINT_LEVEL", "4") &&
              session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
              session.Advance(2200U) == VIRTUAL_LITCH_OK, "timed opening becomes ready");
    const uint32_t base = session.GameClock()->GetRemainingMs(runtime::ClockSide::White);
    Check(!session.GameClock()->IsRunning(), "untouched opening does not start clocks");
    virtual_litch_action_request hint{};
    virtual_litch_default_action_request(&hint);
    hint.action = VIRTUAL_LITCH_CONTROL_HINT;
    hint.owner = VIRTUAL_LITCH_OWNER_P1;
    modules::ControlMutationOutcome hintOutcome;
    Check(session.DispatchControlAction(hint, hintOutcome) == VIRTUAL_LITCH_OK &&
              hintOutcome.status == modules::ControlMutationStatus::Applied &&
              session.HintDisplayRequested(), "production Hint action owns opening analysis before the pickup");
    Check(session.ObserveHallMask(OPENING & ~(UINT64_C(1) << E2)) == VIRTUAL_LITCH_OK &&
              session.Advance(600U) == VIRTUAL_LITCH_OK && session.GameClock()->IsRunning(),
          "legal first pickup starts the mover clock without committing a move");
    Checkpoint(session, "first-pickup-held", "return-e2-or-place-e4");
    Check(session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.UndoHistoryMoveCount() == 0U, "returning pickup aborts the first move");
    Check(session.ObserveHallMask(OPENING & ~(UINT64_C(1) << E2)) == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask((OPENING & ~(UINT64_C(1) << E2)) |
                  (UINT64_C(1) << E4)) == VIRTUAL_LITCH_OK &&
              session.Advance(100U) == VIRTUAL_LITCH_OK &&
              session.GameVariant() == modules::GameVariant::Classic &&
              session.UndoHistoryMoveCount() == 0U,
          "first placement is still pending before engine acknowledgement");
    Checkpoint(session, "first-move-settling", "wait-for-confirmation-or-clear-board");
    const uint32_t remaining = session.GameClock()->GetRemainingMs(runtime::ClockSide::White);
    Check(session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK, "clear during first-move settling");
    CheckEmptySetup(session, "pending-first-move-cleared");
    Check(session.GameClock()->IsPaused() &&
              session.GameClock()->GetRemainingMs(runtime::ClockSide::White) == remaining &&
              remaining < base && session.UndoHistoryMoveCount() == 0U &&
              !session.HintDisplayRequested(),
          "clearing cancels pending move and Hint without a clock refill or increment");
    const auto revision = session.SettingsForPersistenceTest().GetRevision();
    Check(session.Advance(2500U) == VIRTUAL_LITCH_OK &&
              session.SettingsForPersistenceTest().GetRevision() == revision &&
              session.GameClock()->GetRemainingMs(runtime::ClockSide::White) == remaining,
          "empty Setup does not resume settle, search, clock or repeated persistence");
    Check(session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(OPENING & ~(UINT64_C(1) << E2)) == VIRTUAL_LITCH_OK &&
              session.State().valid_destination_mask ==
                  ((UINT64_C(1) << 44U) | (UINT64_C(1) << E4)),
          "replacement restores legal first-move guidance after canceled settle/search");
    Checkpoint(session, "aborted-move-recovered", "place-e4-or-return-e2");
}

void RunPlayedGameAndFinalUndo() {
    test_support::BoundedHintSession fixture(Config());
    auto& session = fixture.session;
    const uint64_t afterMove = (OPENING & ~(UINT64_C(1) << E2)) | (UINT64_C(1) << E4);
    Check(session.Reset() == VIRTUAL_LITCH_OK, "initialize history journey");
    session.RunControlIngress([](void*, modules::SettingsStore&,
        modules::ControlMutationService&, modules::GamePersistenceCoordinator& game) {
        game.SetGameHistoryIdentityGenerator([](void*, modules::game_history::GameId& id) {
            id.bytes[0] = 1U;
            return true;
        });
    }, nullptr);
    Check(Action(session, VIRTUAL_LITCH_CONTROL_NEW_GAME), "start canonical game-history identity");
    modules::ControlMutationOutcome completion;
    Check(session.TryTakeControlCompletion(completion) &&
              completion.status == modules::ControlMutationStatus::Applied,
          "New Game receives its correlated applied acknowledgement");
    Check(
              session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
              session.Advance(2200U) == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(OPENING & ~(UINT64_C(1) << E2)) == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(afterMove) == VIRTUAL_LITCH_OK &&
              session.Advance(2100U) == VIRTUAL_LITCH_OK && session.UndoHistoryMoveCount() == 1U,
          "first move commits through the real engine and acknowledgement");
    Checkpoint(session, "first-move-accepted", "continue-game", "applied");
    Check(session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK &&
              !session.HomeStateSnapshot().freshSetup && session.GameVariant() == modules::GameVariant::Classic,
          "clearing a played game preserves hard correction and game authority");
    Checkpoint(session, "played-game-cleared", "restore-current-position", "correction-required");
    Check(session.ObserveHallMask(afterMove) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK && Action(session, VIRTUAL_LITCH_CONTROL_UNDO) &&
              session.ObserveHallMask(afterMove & ~(UINT64_C(1) << E4)) == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK && session.UndoHistoryMoveCount() == 0U,
          "final untimed Undo returns a new selectable opening");
    Check(session.TryTakeControlCompletion(completion) &&
              completion.status == modules::ControlMutationStatus::Applied,
          "final Undo receives its correlated applied acknowledgement");
    Check(session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK &&
              session.HomeStateSnapshot().placementMode && session.HomeStateSnapshot().freshSetup &&
              session.State().human_illegal_mask == 0ULL,
          "final untimed Undo reopens ordinary empty-board Setup");
    Checkpoint(session, "played-game-final-undo-cleared", "arrange-opening-pieces",
               "fresh-setup");
    bool restored = false;
    session.RunControlIngress([](void* context, modules::SettingsStore&,
        modules::ControlMutationService&, modules::GamePersistenceCoordinator& game) {
        *static_cast<bool*>(context) = game.LoadOrInitialize();
    }, &restored);
    Check(restored && session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK &&
              session.HomeStateSnapshot().placementMode && session.HomeStateSnapshot().freshSetup,
          "production restore retains the untimed fresh-game boundary");
    Checkpoint(session, "final-undo-fresh-setup-restored", "arrange-opening-pieces",
               "fresh-setup");
    Check(Action(session, VIRTUAL_LITCH_CONTROL_NEW_GAME), "explicit New Game resets the lifecycle");
    Check(session.TryTakeControlCompletion(completion) &&
              completion.status == modules::ControlMutationStatus::Applied,
          "explicit New Game receives its correlated applied acknowledgement");
    CheckEmptySetup(session, "explicit-new-game-on-empty-board");
    Check(session.LoadExternalFen(modules::INITIAL_FEN) &&
              session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK &&
              !session.HomeStateSnapshot().freshSetup && session.GameVariant() == modules::GameVariant::Classic,
          "external position authority cannot be silently converted to fresh Setup");
}

void RawButton(runtime::VirtualLitchSession& session,
               runtime::VirtualLitchPhysicalButton button, bool down) {
    Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, button, down) == VIRTUAL_LITCH_OK &&
              session.Advance(30U) == VIRTUAL_LITCH_OK, "raw button passes production debounce");
}

void Chord(runtime::VirtualLitchSession& session, uint32_t heldMs) {
    RawButton(session, runtime::VirtualLitchPhysicalButton::X, true);
    RawButton(session, runtime::VirtualLitchPhysicalButton::O, true);
    Check(session.Advance(heldMs) == VIRTUAL_LITCH_OK, "advance physical mode confirmation hold");
    RawButton(session, runtime::VirtualLitchPhysicalButton::X, false);
    RawButton(session, runtime::VirtualLitchPhysicalButton::O, false);
}

void RunEmptyBoardLitchcraftOwnership() {
    test_support::BoundedHintSession fixture(Config());
    auto& session = fixture.session;
    session.ObserveBootHallMask(0ULL);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK, "empty chess boot settles");
    CheckEmptySetup(session, "empty-boot-setup");
    Chord(session, 1500U);
    Check(session.Litchcraft().IsActive(), "physical confirmation accepts Litchcraft entry");
    Checkpoint(session, "empty-litchcraft-entry", "paint-or-exit", "applied");
    Check(session.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK &&
              session.Advance(10000U) == VIRTUAL_LITCH_OK && session.Litchcraft().IsActive(),
          "accepted empty Litchcraft session retains authoritative mode across Hall and idle updates");
    Checkpoint(session, "empty-litchcraft-idle", "paint-or-exit", "retained");
    Chord(session, 300U);
    Check(session.Litchcraft().IsActive(), "short exit hold cancels and retains Litchcraft");
    Checkpoint(session, "litchcraft-exit-canceled", "retry-confirmed-exit", "canceled");
    Chord(session, 1500U);
    Check(!session.Litchcraft().IsActive(), "confirmed exit returns chess ownership");
    CheckEmptySetup(session, "litchcraft-exit-to-empty-setup");
}

} // namespace

int main() {
    RunEmptyBoardSetup();
    RunEmptyDuckSetup(false);
    RunEmptyDuckSetup(true);
    RunAbortedFirstMoveWithClockAndHints();
    RunPlayedGameAndFinalUndo();
    RunEmptyBoardLitchcraftOwnership();
    if (failures != 0) return 1;
    std::puts("EMPTY_BOARD_SETUP_JOURNEY_OK");
    return 0;
}
