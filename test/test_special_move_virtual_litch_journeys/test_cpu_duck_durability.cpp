#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

#include <ArduinoJson.h>

#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/draw_rules.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/orientation.hpp"
#include "runtime/chess_interaction_persistence.hpp"
#include "virtual_litch_session.hpp"

namespace {

constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t DUCK_A5 = 24U;
constexpr uint8_t DUCK_B5 = 25U;
int g_failures = 0;

bool Check(const bool condition, const char* run, const char* message) {
    if (!condition) {
        std::printf("[FAIL] CPU_DUCK_DURABILITY %s: %s\n", run, message);
        ++g_failures;
    }
    return condition;
}

void RemoveStore(const char* path) {
    (void)std::remove(path);
    char sibling[192] = {};
    for (const char* suffix : {".bak", ".tmp"}) {
        (void)std::snprintf(sibling, sizeof(sibling), "%s%s", path, suffix);
        (void)std::remove(sibling);
    }
}

bool Set(runtime::VirtualLitchSession& session, const uint16_t id,
         const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return session.MutateSettingJson(id, key, value, outcome) == VIRTUAL_LITCH_OK &&
           (outcome.status == modules::ControlMutationStatus::Applied ||
            outcome.status == modules::ControlMutationStatus::Noop);
}

bool Observe(runtime::VirtualLitchSession& session, const uint64_t logical) {
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

bool Pump(runtime::VirtualLitchSession& session, const uint64_t occupancy,
          const uint32_t durationMs) {
    for (uint32_t elapsed = 0U; elapsed < durationMs;) {
        const uint32_t remaining = durationMs - elapsed;
        const uint32_t step = remaining < 50U ? remaining : 50U;
        if (session.Advance(step) != VIRTUAL_LITCH_OK || !Observe(session, occupancy)) {
            return false;
        }
        elapsed += step;
    }
    return true;
}

bool Move(runtime::VirtualLitchSession& session, uint64_t& occupancy,
          const uint8_t from, const uint8_t to) {
    if (from >= 64U || to >= 64U || from == to) {
        return false;
    }
    occupancy &= ~(UINT64_C(1) << from);
    if (!Observe(session, occupancy)) {
        return false;
    }
    occupancy |= UINT64_C(1) << to;
    return Observe(session, occupancy);
}

bool ReadHome(const runtime::VirtualLitchSession& session, JsonDocument& home) {
    return deserializeJson(home, session.HomeStateLine().CStr()) ==
           DeserializationError::Ok;
}

uint8_t Square(const char* text) {
    if (text == nullptr || text[0] < 'a' || text[0] > 'h' ||
        text[1] < '1' || text[1] > '8') {
        return 64U;
    }
    return static_cast<uint8_t>((8 - (text[1] - '0')) * 8 + text[0] - 'a');
}

bool HasYellowRing(const runtime::VirtualLitchSession& session, const uint8_t logical) {
    if (logical >= 64U) {
        return false;
    }
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    const uint8_t physical = modules::MapSquareToPhysical(logical, rotate);
    const auto color = session.PhysicalFrame().pixels[
        hardware::LedIndexForSquare(physical, hardware::SquareLedSlot::Square0)];
    return color.r > color.b && color.g > color.b;
}

bool ReadPersisted(runtime::VirtualLitchSession& session,
                   modules::GamePersistenceState& state) {
    return modules::ReadGamePersistenceState(
        session.SettingsForPersistenceTest().Doc(), state);
}

bool SameDuckPosition(const char* before, const char* after) {
    // McuMax normalizes FEN counters on restore; the coordinator owns the
    // history/counters. Compare the same four position fields as production.
    fxe::FixedString<96> beforeKey;
    fxe::FixedString<96> afterKey;
    std::printf("[CPU_DUCK_REBOOT_FEN] before=%s after=%s\n", before, after);
    return modules::TryBuildFenPositionKey4(before, beforeKey) &&
           modules::TryBuildFenPositionKey4(after, afterKey) &&
           beforeKey.Equals(afterKey.View());
}

bool HasHistory(runtime::VirtualLitchSession& session, const uint16_t count) {
    modules::GamePersistenceState state;
    return ReadPersisted(session, state) && !state.fen.Empty() &&
           state.undoHistoryCount == count * modules::UNDO_HISTORY_FIELDS_PER_MOVE;
}

void Checkpoint(runtime::VirtualLitchSession& session, const char* run,
                const char* stage, const char* next) {
    const auto& state = session.State();
    modules::GamePersistenceState persisted;
    const bool decoded = ReadPersisted(session, persisted);
    std::printf(
        "[CPU_DUCK_DURABILITY_JOURNEY] run=%s stage=%s t=%u "
        "phase=%u required=%u occupancy=%016llx expected=%016llx "
        "correction=%016llx pending=%u display=%.*s doc_fen=%u "
        "history=%u cpu_record=%u interaction=%u variant=%u next=%s home=%s\n",
        run, stage, state.now_ms, state.interaction_phase, state.required_action,
        static_cast<unsigned long long>(state.physical_occupied_mask),
        static_cast<unsigned long long>(state.expected_occupied_mask),
        static_cast<unsigned long long>(state.correction_mask),
        session.IsUndoActionPending(), static_cast<int>(session.DisplayText().size()),
        session.DisplayText().data(), decoded && !persisted.fen.Empty(),
        static_cast<unsigned>(persisted.undoHistoryCount /
                              modules::UNDO_HISTORY_FIELDS_PER_MOVE),
        persisted.cpuMoveValid, !persisted.interactionState.Empty(),
        !persisted.variantState.Empty(), next, session.HomeStateLine().CStr());
    std::printf("[CPU_DUCK_DURABILITY_FRAME] run=%s stage=%s semantic_rgb=", run, stage);
    for (uint8_t square = 0U; square < 64U; ++square) {
        for (uint8_t slot = 0U; slot < session.SemanticFrame().leds_per_square; ++slot) {
            const auto color = session.SemanticFrame().board[square][slot];
            std::printf("%02x%02x%02x,", color.r, color.g, color.b);
        }
    }
    std::printf("\n");
}

bool Save(runtime::VirtualLitchSession& session) {
    bool saved = false;
    // Existing seam supplies the authoritative session clock. Save the live
    // coordinator-prepared document; do not inject a game record or flush it
    // through an extra preparation step that could hide stale CPU metadata.
    session.RunControlIngress(
        [](void* context, modules::SettingsStore& settings,
           modules::ControlMutationService&,
           modules::GamePersistenceCoordinator&) {
            *static_cast<bool*>(context) = settings.SaveNow();
        }, &saved);
    return saved;
}

bool Undo(runtime::VirtualLitchSession& session, const uint16_t id,
          const modules::ControlMutationStatus expected, const char* run) {
    virtual_litch_action_request request{};
    virtual_litch_default_action_request(&request);
    request.request_id = id;
    request.action = VIRTUAL_LITCH_CONTROL_UNDO;
    request.undo_move_count = 1U;
    modules::ControlMutationOutcome outcome;
    const bool received = session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
        outcome.status == modules::ControlMutationStatus::Accepted &&
        session.Advance(1U) == VIRTUAL_LITCH_OK &&
        session.TryTakeControlCompletion(outcome) && outcome.requestId == id;
    std::printf("[CPU_DUCK_DURABILITY_CONTROL] run=%s id=%u received=%u status=%u reason=%u\n",
                run, id, received, static_cast<unsigned>(outcome.status),
                static_cast<unsigned>(outcome.reason));
    return Check(received && outcome.status == expected, run,
                 "canonical Undo returns the expected correlated terminal result");
}

struct CpuTurn {
    uint64_t occupancy = OPENING | (UINT64_C(1) << DUCK_A5);
    uint64_t decisionOccupancy = 0ULL;
    uint64_t beforeCpuOccupancy = 0ULL;
    modules::FenString decisionFen;
    modules::FenString beforeCpuFen;
    uint8_t chessFrom = 64U;
    uint8_t chessTo = 64U;
    uint8_t duckTo = 64U;
};

bool PrepareCpuTurn(runtime::VirtualLitchSession& session, CpuTurn& turn,
                    const char* run) {
    if (!Check(session.Reset() == VIRTUAL_LITCH_OK &&
                   Set(session, 300U, "VS_CPU_LEVEL", "0") &&
                   Set(session, 301U, "TIMER", "0") &&
                   Set(session, 302U, "BOARD_SETUP_ASSISTANCE", "false") &&
                   Set(session, 303U, "WHITE_SQUARES_ON", "false") &&
                   Observe(session, turn.occupancy) && Pump(session, turn.occupancy, 1000U) &&
                   session.GameVariant() == modules::GameVariant::Duck,
               run, "raw complete opening plus a5 Duck initializes the real production session")) {
        return false;
    }
    StaticJsonDocument<1536> home;
    const auto commitHuman = [&](const uint8_t from, const uint8_t to,
                                 const uint8_t duckFrom, const uint8_t duckTo) {
        return Move(session, turn.occupancy, from, to) &&
               Pump(session, turn.occupancy, 2600U) && ReadHome(session, home) &&
               std::strcmp(home["a"] | "", "duck") == 0 &&
               Move(session, turn.occupancy, duckFrom, duckTo) &&
               Pump(session, turn.occupancy, 2000U);
    };
    if (!Check(commitHuman(52U, 36U, DUCK_A5, DUCK_B5) &&
                   HasHistory(session, 1U) && ReadHome(session, home) &&
                   turn.decisionFen.Assign(home["fen"] | ""),
               run, "raw e2e4/a5-b5 creates the exact preceding decision boundary")) {
        return false;
    }
    turn.decisionOccupancy = turn.occupancy;
    if (!Check(commitHuman(12U, 28U, DUCK_B5, DUCK_A5) &&
                   HasHistory(session, 2U) && ReadHome(session, home) &&
                   turn.beforeCpuFen.Assign(home["fen"] | ""),
               run, "raw e7e5/b5-a5 commits the human Black turn before CPU White")) {
        return false;
    }
    turn.beforeCpuOccupancy = turn.occupancy;
    if (!Check(Set(session, 310U, "VS_CPU_PLAYER", "1") &&
                   Set(session, 311U, "VS_CPU_LEVEL", "1"),
               run, "canonical settings enable the logical White CPU independent of board mapping")) {
        return false;
    }
    bool ready = false;
    for (uint16_t sample = 0U; sample < 120U; ++sample) {
        if (!ReadHome(session, home)) {
            break;
        }
        if (std::strcmp(home["a"] | "", "cpu") == 0 && home["c"].is<JsonArray>()) {
            ready = true;
            break;
        }
        if (!Pump(session, turn.occupancy, 100U)) {
            break;
        }
    }
    turn.chessFrom = Square(home["c"][0U] | "--");
    turn.chessTo = Square(home["c"][1U] | "--");
    if (!Check(ready && turn.chessFrom < 64U && turn.chessTo < 64U &&
                   std::strcmp(home["c"][2U] | "", "--") == 0,
               run, "the real Duck engine supplies a quiet CPU chess move")) {
        return false;
    }
    Checkpoint(session, run, "cpu-chess-selected", "move-the-selected-CPU-chess-piece");
    if (!Check(Move(session, turn.occupancy, turn.chessFrom, turn.chessTo) &&
                   Pump(session, turn.occupancy, 2600U) && ReadHome(session, home) &&
                   std::strcmp(home["a"] | "", "duck") == 0 &&
                   turn.beforeCpuFen.Equals(home["fen"] | ""),
               run, "CPU chess physical acknowledgement stages a Duck turn without committing FEN")) {
        return false;
    }
    const char* plan = home["dr"] | "";
    turn.duckTo = std::strlen(plan) == 5U ? Square(plan + 3U) : 64U;
    const bool fixed = std::strlen(plan) == 5U && plan[0] == 'c' &&
                       Square(plan + 1U) == DUCK_A5 && turn.duckTo < 64U &&
                       turn.duckTo != DUCK_A5;
    Checkpoint(session, run, "cpu-duck-staged", "save-or-change-CPU-setting-with-Duck-untouched");
    return Check(fixed && HasYellowRing(session, DUCK_A5) &&
                     HasYellowRing(session, turn.duckTo),
                 run, "the original CPU-owned source and fixed target have mapped yellow rings");
}

bool SameFixedPlan(runtime::VirtualLitchSession& session, const CpuTurn& turn,
                   const char owner) {
    StaticJsonDocument<1536> home;
    if (!ReadHome(session, home)) {
        return false;
    }
    const char* plan = home["dr"] | "";
    return std::strcmp(home["a"] | "", "duck") == 0 &&
           std::strlen(plan) == 5U && plan[0] == owner &&
           Square(plan + 1U) == DUCK_A5 && Square(plan + 3U) == turn.duckTo &&
           session.State().correction_mask == 0ULL;
}

bool CheckStagedSnapshot(runtime::VirtualLitchSession& session,
                         const CpuTurn& turn, const char* run) {
    modules::GamePersistenceState state;
    modules::DuckGameDurableState variant;
    runtime::ChessInteractionDurableState interaction;
    const bool valid = ReadPersisted(session, state) &&
        modules::DecodeDuckGameDurableState(state.variantState.CStr(), variant) &&
        runtime::DecodeChessInteractionDurableState(state.interactionState.CStr(), interaction);
    const auto savedFen = state.fen.View();
    const auto homeFen = turn.beforeCpuFen.View();
    const size_t fullmove = homeFen.rfind(' ');
    const bool priorPreserved = Check(
        fullmove != std::string_view::npos && savedFen.substr(0U, fullmove) == homeFen.substr(0U, fullmove) &&
        savedFen.substr(fullmove) == " 1" && homeFen.substr(fullmove) == " 2" &&
        HasHistory(session, 2U), run,
        "natural CPU staged preparation never erases the previous committed FEN/history from Doc");
    const bool stagedValid = Check(valid && !state.cpuMoveValid && variant.pendingChessMoveValid &&
                     variant.pendingMoveFrom == turn.chessFrom &&
                     variant.pendingMoveTo == turn.chessTo &&
                     interaction.phase.plan.source == runtime::PhysicalTransitionSource::Cpu &&
                     interaction.phase.plan.moveFrom == DUCK_A5 &&
                     interaction.phase.selectedSquare == turn.duckTo &&
                     interaction.phase.plan.moveTo == turn.duckTo,
                 run, "immediate snapshot stores one staged CPU Duck authority without a queued chess duplicate");
    return priorPreserved && stagedValid;
}

enum class RebootExit { Commit, Undo };

void RunStagedCpuReboot(const RebootExit exit) {
    const char* run = exit == RebootExit::Commit ? "reboot-commit" : "reboot-undo";
    const char* path = exit == RebootExit::Commit
        ? "build/cpu_duck_durability_commit.json" : "build/cpu_duck_durability_undo.json";
    RemoveStore(path);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession sessionFixture(config, path);
    auto& session = sessionFixture.session;
    CpuTurn turn;
    if (!PrepareCpuTurn(session, turn, run) || !CheckStagedSnapshot(session, turn, run)) {
        return;
    }
    if (!Check(Save(session) && session.Reset() == VIRTUAL_LITCH_OK &&
                   session.PersistenceDegradation() == modules::PersistenceDegradation::None &&
                   Observe(session, turn.occupancy) && SameFixedPlan(session, turn, 'c') &&
                   HasYellowRing(session, DUCK_A5) && HasYellowRing(session, turn.duckTo) &&
                   CheckStagedSnapshot(session, turn, run),
               run, "save/reboot/full raw Hall scan restores the exact CPU owner and fixed yellow target")) {
        Checkpoint(session, run, "restore-rejected", "inspect-failed-durable-authority");
        return;
    }
    Checkpoint(session, run, "reboot-fixed-target", "lift-Duck-then-return-to-source");
    turn.occupancy &= ~(UINT64_C(1) << DUCK_A5);
    if (!Check(Observe(session, turn.occupancy) && SameFixedPlan(session, turn, 'c'),
               run, "first post-boot Duck lift retains the fixed CPU destination") ||
        !Undo(session, 420U, modules::ControlMutationStatus::Rejected, run)) {
        return;
    }
    turn.occupancy |= UINT64_C(1) << DUCK_A5;
    if (!Check(Observe(session, turn.occupancy) && SameFixedPlan(session, turn, 'c'),
               run, "source return recovers the original CPU plan after rejected in-hand Undo")) {
        return;
    }
    if (exit == RebootExit::Commit) {
        if (!Check(Move(session, turn.occupancy, DUCK_A5, turn.duckTo) &&
                       Pump(session, turn.occupancy, 500U) && HasHistory(session, 3U) &&
                       session.DuckSquare() == turn.duckTo,
                   run, "restored fixed Duck placement commits the original compound turn once")) {
            return;
        }
        modules::GamePersistenceState committed;
        Check(ReadPersisted(session, committed) && !committed.cpuMoveValid &&
                  committed.undoHistory[2U * modules::UNDO_HISTORY_FIELDS_PER_MOVE + 5U] == turn.chessFrom &&
                  committed.undoHistory[2U * modules::UNDO_HISTORY_FIELDS_PER_MOVE + 6U] == turn.chessTo,
              run, "committed history records the selected CPU move without a leftover CPU prompt");
        StaticJsonDocument<1536> home;
        Check(Save(session) && session.Reset() == VIRTUAL_LITCH_OK &&
                  Observe(session, turn.occupancy) && Pump(session, turn.occupancy, 500U) &&
                  ReadHome(session, home) &&
                  SameDuckPosition(committed.fen.CStr(), home["fen"] | "") &&
                  !home.containsKey("dr") && HasHistory(session, 3U) &&
                  session.DuckSquare() == turn.duckTo &&
                  session.State().expected_occupied_mask == turn.occupancy,
              run, "a second reboot preserves exactly one committed turn and no stale CPU replay");
        Checkpoint(session, run, "committed-reboot", "choose-a-human-Black-move");
        return;
    }
    if (!Undo(session, 421U, modules::ControlMutationStatus::Applied, run)) {
        return;
    }
    Checkpoint(session, run, "restored-CPU-inverse", "return-CPU-chess-piece-before-prior-human-turn");
    if (!Check(Move(session, turn.occupancy, turn.chessTo, turn.chessFrom) &&
                   Pump(session, turn.occupancy, 1U) &&
                   turn.occupancy == turn.beforeCpuOccupancy,
               run, "restored staged CPU inverse returns its chess piece without a history entry")) {
        return;
    }
    CpuTurn humanDuck = turn;
    humanDuck.duckTo = DUCK_B5;
    if (!Check(SameFixedPlan(session, humanDuck, 'u') &&
                   session.IsUndoActionPending() && HasHistory(session, 2U) &&
                   HasYellowRing(session, DUCK_A5) && HasYellowRing(session, DUCK_B5),
               run, "restored CPU ownership continues into the preceding human Duck inverse")) {
        return;
    }
    Checkpoint(session, run, "restored-human-Duck-inverse", "return-a5-Duck-to-b5-then-e5-pawn-to-e7");
    StaticJsonDocument<1536> home;
    Check(Move(session, turn.occupancy, DUCK_A5, DUCK_B5) &&
              Move(session, turn.occupancy, 28U, 12U) && Pump(session, turn.occupancy, 1000U) &&
              ReadHome(session, home) && turn.decisionFen.Equals(home["fen"] | "") &&
              turn.occupancy == turn.decisionOccupancy && HasHistory(session, 1U) &&
              !session.IsUndoActionPending() && !home.containsKey("dr") &&
              session.State().expected_occupied_mask == turn.decisionOccupancy,
          run, "post-reboot Undo returns the exact human decision without replaying the CPU half");
    Checkpoint(session, run, "human-decision-restored", "choose-a-human-Black-move");
}

void RunCpuSettingBeforeDuckLift(const char* key, const char* value) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
    CpuTurn turn;
    if (!PrepareCpuTurn(session, turn, key)) {
        return;
    }
    // RequestCancelCpuWork acts inside the settings callback. A spare runtime
    // tick can resume an accidentally canceled Duck plan and mask this race.
    // Send the next raw lift without Advance or an unchanged Hall scan.
    if (!Check(Set(session, 450U, key, value),
               key, "CPU setting change receives its canonical result before the immediate raw lift")) {
        return;
    }
    Check(SameFixedPlan(session, turn, 'c'), key,
          "changing CPU settings keeps the already staged fixed Duck owner");
    turn.occupancy &= ~(UINT64_C(1) << DUCK_A5);
    const bool lifted = Observe(session, turn.occupancy) && SameFixedPlan(session, turn, 'c') &&
        session.State().required_action == static_cast<uint8_t>(
            runtime::RequiredPhysicalAction::PlacePrimary) &&
        HasYellowRing(session, turn.duckTo);
    Check(lifted, key, "immediate raw Duck lift cannot discard the staged CPU chess half");
    Checkpoint(session, key, "immediate-Duck-lift", "place-Duck-at-original-fixed-target");
    if (!lifted) {
        return;
    }
    turn.occupancy |= UINT64_C(1) << turn.duckTo;
    Check(Observe(session, turn.occupancy) && HasHistory(session, 3U) &&
              session.DuckSquare() == turn.duckTo &&
              session.State().expected_occupied_mask == turn.occupancy,
          key, "the original staged turn completes exactly once after CPU settings mutation");
    Checkpoint(session, key, "setting-change-turn-complete", "continue-with-the-new-CPU-setting");
}

std::string JsonText(const JsonDocument& doc) {
    std::string text;
    serializeJson(doc, text);
    return text;
}

std::string GameJson(const JsonDocument& doc) {
    StaticJsonDocument<8192> game;
    for (const auto entry : doc.as<JsonObjectConst>()) {
        if (std::strncmp(entry.key().c_str(), "GAME_", 5U) == 0) {
            game[entry.key().c_str()].set(entry.value());
        }
    }
    return JsonText(game);
}

void TestPreparationFailurePreservesPriorGame() {
    constexpr char RUN[] = "controlled-invalid-durable-source";
    constexpr char PATH[] = "build/cpu_duck_preparation_failure.json";
    RemoveStore(PATH);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStoreConfig storeConfig;
    storeConfig.path = PATH;
    modules::SettingsStore settings(storeConfig);
    runtime::GameStateMachine stateMachine;
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);
    if (!Check(settings.Load(), RUN, "real storage initializes the supporting coordinator case")) {
        return;
    }
    settings.Doc()["VS_CPU_LEVEL"] = 0U;
    settings.Doc()["TIMER"] = 0U;
    if (!Check(coordinator.LoadOrInitialize(), RUN, "real engine/coordinator load a fresh game")) {
        return;
    }
    stateMachine.ApplyPhysicalOccupancySnapshot(OPENING | (UINT64_C(1) << DUCK_A5));
    fxe::ManualMillis::AdvanceMs(200U);
    coordinator.Update();
    for (uint8_t poll = 0U; poll < 40U; ++poll) {
        coordinator.RequestCancelAllThinking();
        coordinator.Update();
        if (!stateMachine.IsHintThinkingActive() && !stateMachine.IsCpuThinkingActive()) {
            break;
        }
        fxe::ManualMillis::AdvanceMs(10U);
    }
    if (!Check(coordinator.GetGameVariant() == modules::GameVariant::Duck &&
                   !stateMachine.IsHintThinkingActive() && !stateMachine.IsCpuThinkingActive() &&
                   coordinator.FlushToSettingsNow(),
               RUN, "quiescent real Duck baseline saves successfully before the controlled fault")) {
        return;
    }
    const std::string beforeDoc = JsonText(settings.Doc());
    const std::string beforeGame = GameJson(settings.Doc());
    // Disclosed unit fault, approved separately from the raw journeys: ask the
    // existing reducer builder for an invalid durable source enum. No phase,
    // FEN, history, fake engine, or production test hook is injected.
    const bool faultPrepared = stateMachine.BeginDuckRelocation(
        DUCK_A5, DUCK_B5, static_cast<runtime::PhysicalTransitionSource>(255U));
    char encoded[128] = {};
    if (!Check(faultPrepared && runtime::CanSafelyPersistChessInteractionState(
                   stateMachine.GetChessInteractionModel()) &&
                   !runtime::EncodeChessInteractionDurableState(
                       stateMachine.GetChessInteractionModel(), encoded, sizeof(encoded)),
               RUN, "controlled builder input reaches serializer rejection after the safe-state gate")) {
        return;
    }
    const bool refused = !coordinator.FlushToSettingsNow();
    const bool docUnchanged = JsonText(settings.Doc()) == beforeDoc;
    Check(refused && docUnchanged, RUN,
          "preparation refusal leaves every byte of the previous live document intact");

    modules::ControlMutationService mutation;
    mutation.Configure(settings, {});
    StaticJsonDocument<32> value;
    value.set(6U);
    const auto outcome = mutation.MutateSetting(
        "BRIGHTNESS", value.as<JsonVariantConst>(), 490U);
    Check(outcome.status == modules::ControlMutationStatus::Applied, RUN,
          "a later ordinary brightness change uses the production settings mutation owner");
    fxe::ManualMillis::AdvanceMs(storeConfig.deferredSaveDelayMs);
    settings.Update();
    modules::SettingsStore reloaded(storeConfig);
    const bool savedPriorGame = reloaded.Load() &&
        reloaded.ReadValue("BRIGHTNESS", 0U) == 6U && GameJson(reloaded.Doc()) == beforeGame;
    Check(savedPriorGame, RUN,
          "the ordinary settings save retains the exact previous game after preparation failure");
    std::printf("[CPU_DUCK_PREPARATION_FAILURE] fault=invalid-source-via-builder "
                "refused=%u doc_unchanged=%u later_setting_saved_prior_game=%u\n",
                refused, docUnchanged, savedPriorGame);
}

} // namespace

int RunCpuDuckDurabilityVirtualLitchJourneys() {
    const int before = g_failures;
    RunStagedCpuReboot(RebootExit::Commit);
    RunStagedCpuReboot(RebootExit::Undo);
    RunCpuSettingBeforeDuckLift("VS_CPU_LEVEL", "2");
    RunCpuSettingBeforeDuckLift("VS_CPU_PLAYER", "2");
    TestPreparationFailurePreservesPriorGame();
    return g_failures - before;
}
