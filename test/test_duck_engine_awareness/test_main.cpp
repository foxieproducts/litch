#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/manual_millis_guard.hpp"
#include "modules/chess_strings.hpp"
#include <litch_max/duck_chess_rules.hpp>
#include "modules/duck_game_persistence.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
constexpr uint8_t DUCK = 24U;
int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

modules::SettingsStoreConfig MakeSettingsConfig(const char* path) {
    modules::SettingsStoreConfig config;
    config.path = path;
    config.deferredSaveDelayMs = 100000U;
    return config;
}

runtime::GameStateMachineConfig MakeStateMachineConfig() {
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 1U;
    config.cpuIdleDelayMs = 0U;
    config.placementStableRequiredMs = 0U;
    return config;
}

struct Harness {
    explicit Harness(const char* path, const bool clearStore = true)
        : settingsConfig(MakeSettingsConfig(path)),
          stateMachineConfig(MakeStateMachineConfig()),
          settings(settingsConfig),
          stateMachine(stateMachineConfig),
          coordinator(settings, engine, stateMachine) {
        if (clearStore) {
            (void)std::remove(path);
            char sibling[160] = {};
            (void)std::snprintf(
                sibling, sizeof(sibling), "%s.bak", path);
            (void)std::remove(sibling);
            (void)std::snprintf(
                sibling, sizeof(sibling), "%s.tmp", path);
            (void)std::remove(sibling);
        }
        stateMachine.Reset();
    }

    bool LoadDuck(const uint32_t vsCpuLevel,
                  const bool firstPlayerIsP2 = false,
                  const bool cpuPlayerIsP2 = true,
                  const char* fen = modules::INITIAL_FEN,
                  const uint8_t duckSquare = DUCK) {
        if (!settings.Load()) {
            return false;
        }
        settings.Doc()["VS_CPU_LEVEL"] = vsCpuLevel;
        settings.Doc()["HINT_LEVEL"] = 5U;
        settings.Doc()["GAME_FIRST_PLAYER"] =
            firstPlayerIsP2 ? 2U : 1U;
        settings.Doc()["VS_CPU_PLAYER"] =
            cpuPlayerIsP2 ? 2U : 1U;
        settings.Doc()["TIMER"] = 0U;

        modules::GamePersistenceState state;
        if (!state.fen.Assign(fen) || !state.undoBaseFen.Assign(fen)) {
            return false;
        }
        modules::DuckGameDurableState duck;
        duck.variant = modules::GameVariant::Duck;
        duck.duckSquare = duckSquare;
        char encoded[32] = {};
        return modules::EncodeDuckGameDurableState(
                   duck, encoded, sizeof(encoded)) &&
               state.variantState.Assign(encoded) &&
               modules::WriteGamePersistenceState(settings.Doc(), state) &&
               settings.SaveNow() && coordinator.LoadOrInitialize();
    }

    bool LoadExisting() {
        return settings.Load() && coordinator.LoadOrInitialize();
    }

    modules::SettingsStoreConfig settingsConfig;
    runtime::GameStateMachineConfig stateMachineConfig;
    modules::SettingsStore settings;
    test_support::BoundedHintEngine engine;
    runtime::GameStateMachine stateMachine;
    modules::GamePersistenceCoordinator coordinator;
};

class ScriptedCastleEngine final : public modules::IEngine {
  public:
    static constexpr char CASTLE_FEN[] =
        "4k3/8/8/8/8/8/8/4K2R w K - 0 1";

    void Reset() override { (void)m_fen.Assign(CASTLE_FEN); }
    bool SetFen(const char* fen) override {
        return fen != nullptr && m_fen.Assign(fen);
    }
    bool GetFen(char* outFen, const size_t outSize) const override {
        if (outFen == nullptr || outSize <= m_fen.Size()) {
            return false;
        }
        std::memcpy(outFen, m_fen.CStr(), m_fen.Size() + 1U);
        return true;
    }
    bool ApplyUciMove(const char*) override { return false; }
    modules::SideToMove GetSideToMove() const override {
        return std::strstr(m_fen.CStr(), " b ") != nullptr
                   ? modules::SideToMove::Black
                   : modules::SideToMove::White;
    }
    bool IsInCheck(modules::SideToMove) const override { return false; }
    bool IsInCheckmate(modules::SideToMove) const override { return false; }
    bool IsInStalemate(modules::SideToMove) const override { return false; }
    bool GetLegalDestinationsMask(uint8_t, uint64_t& outMask) override {
        outMask = 0ULL;
        return true;
    }
    bool StartThink(const modules::ThinkConfig&) override { return false; }
    bool StartDuckThink(const char*, uint8_t, const modules::ThinkConfig&) override {
        m_thinking = true;
        return true;
    }
    void CancelThink() override { m_thinking = false; }
    modules::ThinkState PollThinkProgress(
        uint8_t& outProgress,
        modules::UciMove& outMove) override {
        if (!m_thinking) {
            return modules::ThinkState::Idle;
        }
        m_thinking = false;
        outProgress = 100U;
        (void)outMove.Assign("e1g1");
        return modules::ThinkState::Done;
    }
    bool GetLastThinkStats(modules::ThinkStats&) const override {
        return false;
    }
    bool GetLastDuckThinkResult(
        modules::DuckThinkResult& out) const override {
        (void)out.chessMove.Assign("e1g1");
        out.duckDestination = 35U; // d4
        out.score = 1;
        out.capturedKing = false;
        return true;
    }

  private:
    modules::FenString m_fen;
    bool m_thinking = false;
};

constexpr char ScriptedCastleEngine::CASTLE_FEN[];

struct CastleHarness {
    explicit CastleHarness(const char* path)
        : settingsConfig(MakeSettingsConfig(path)),
          stateMachineConfig(MakeStateMachineConfig()),
          settings(settingsConfig),
          stateMachine(stateMachineConfig),
          coordinator(settings, engine, stateMachine) {
        (void)std::remove(path);
        char sibling[160] = {};
        (void)std::snprintf(sibling, sizeof(sibling), "%s.bak", path);
        (void)std::remove(sibling);
        (void)std::snprintf(sibling, sizeof(sibling), "%s.tmp", path);
        (void)std::remove(sibling);
        stateMachine.Reset();
    }

    bool Load() {
        if (!settings.Load()) {
            return false;
        }
        settings.Doc()["VS_CPU_LEVEL"] = 5U;
        settings.Doc()["GAME_FIRST_PLAYER"] = 2U;
        settings.Doc()["VS_CPU_PLAYER"] = 1U;
        settings.Doc()["TIMER"] = 0U;
        modules::GamePersistenceState state;
        if (!state.fen.Assign(ScriptedCastleEngine::CASTLE_FEN) ||
            !state.undoBaseFen.Assign(
                ScriptedCastleEngine::CASTLE_FEN)) {
            return false;
        }
        modules::DuckGameDurableState duck;
        duck.variant = modules::GameVariant::Duck;
        duck.duckSquare = 32U; // a4
        char encoded[32] = {};
        return modules::EncodeDuckGameDurableState(
                   duck, encoded, sizeof(encoded)) &&
               state.variantState.Assign(encoded) &&
               modules::WriteGamePersistenceState(
                   settings.Doc(), state) &&
               settings.SaveNow() && coordinator.LoadOrInitialize();
    }

    modules::SettingsStoreConfig settingsConfig;
    runtime::GameStateMachineConfig stateMachineConfig;
    modules::SettingsStore settings;
    ScriptedCastleEngine engine;
    runtime::GameStateMachine stateMachine;
    modules::GamePersistenceCoordinator coordinator;
};

template <typename THarness>
void Observe(THarness& harness,
             const uint64_t changed,
             const uint64_t occupied) {
    harness.stateMachine.HandleBoardObservation(
        changed, occupied, fxe::Millis(),
        harness.stateMachine.GetLastBoardObservationSequence() + 1U);
}

bool WaitForHint(Harness& harness, uint8_t& from, uint8_t& to) {
    for (uint8_t attempt = 0U; attempt < 8U; ++attempt) {
        harness.coordinator.Update();
        if (harness.stateMachine.TryGetHintMoveSquares(from, to)) {
            return true;
        }
        fxe::ManualMillis::AdvanceMs(100U);
        harness.stateMachine.Tick();
    }
    return false;
}

void TestHumanDuckTurnGetsOneCompoundHint() {
    Harness harness("build/test_duck_engine_hints.json");
    Check(harness.LoadDuck(0U), "human Duck hint fixture loads");
    uint64_t physical = INITIAL_OCCUPANCY | (1ULL << DUCK);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);

    uint8_t moveFrom = 255U;
    uint8_t moveTo = 255U;
    Check(WaitForHint(harness, moveFrom, moveTo),
          "Duck chess phase computes a chess-move hint");
    harness.coordinator.RequestHintToggle();
    Check(harness.stateMachine.IsHintDisplayRequested(),
          "human can reveal the compound Duck hint");

    uint8_t hintedDuckFrom = 255U;
    uint8_t hintedDuckTo = 255U;
    Check(harness.stateMachine.TryGetHintDuckMoveSquares(
              hintedDuckFrom, hintedDuckTo) &&
              hintedDuckFrom == DUCK && hintedDuckTo < 64U &&
              hintedDuckTo != DUCK,
          "the same revealed hint includes the Duck relocation before play");

    physical &= ~(1ULL << moveFrom);
    Observe(harness, 1ULL << moveFrom, physical);
    harness.coordinator.UpdateLegalDestinationsMask();
    physical |= 1ULL << moveTo;
    Observe(harness, 1ULL << moveTo, physical);
    fxe::ManualMillis::AdvanceMs(1U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();

    uint8_t duckFrom = 255U;
    uint8_t duckTo = 255U;
    Check(harness.coordinator.GetDuckTurnPhase() ==
              modules::DuckTurnPhase::DuckRelocation,
          "hinted chess action advances to mandatory duck relocation");
    Check(harness.stateMachine.TryGetHintMoveSquares(duckFrom, duckTo) &&
              duckFrom == DUCK && duckTo == hintedDuckTo,
          "Duck phase retains the destination shown in the compound hint");
    Check(!harness.stateMachine.TryGetHintDuckMoveSquares(
              hintedDuckFrom, hintedDuckTo),
          "completed chess step leaves one active Duck prompt, not a second hint");
    Check(harness.stateMachine.IsHintDisplayRequested(),
          "revealed hint remains visible for the second Duck decision");

    Harness vsCpuHarness("build/test_duck_engine_human_cpu_hint.json");
    Check(vsCpuHarness.LoadDuck(5U, false),
          "Duck VS-CPU fixture loads with the human owning White");
    vsCpuHarness.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | (1ULL << DUCK));
    uint8_t vsCpuFrom = 255U;
    uint8_t vsCpuTo = 255U;
    const bool vsCpuHintReady =
        WaitForHint(vsCpuHarness, vsCpuFrom, vsCpuTo);
    vsCpuHarness.coordinator.RequestHintToggle();
    Check(vsCpuHintReady &&
              vsCpuHarness.stateMachine.IsHintDisplayRequested(),
          "human side of Duck VS-CPU can reveal its chess-move hint");
}

template <typename THarness>
bool WaitForCpuMove(THarness& harness) {
    for (uint8_t attempt = 0U; attempt < 12U; ++attempt) {
        fxe::ManualMillis::AdvanceMs(1U);
        harness.stateMachine.Tick();
        harness.coordinator.Update();
        if (harness.stateMachine.HasQueuedCpuMove()) {
            return true;
        }
    }
    return false;
}

void TestCpuPlayerOneStartsDuckGame() {
    Harness harness("build/test_duck_engine_cpu_p1.json");
    Check(harness.LoadDuck(5U, false, false),
          "Duck CPU=P1 fixture loads with P1 owning White");
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | (1ULL << DUCK));
    Check(WaitForCpuMove(harness),
          "Duck CPU=P1 queues the opening compound action");
    Check(harness.stateMachine.GetChessInteractionModel().phase.plan.source ==
              runtime::PhysicalTransitionSource::Cpu,
          "Duck CPU=P1 owns the opening physical choreography");
}

template <typename THarness>
void ApplyPlan(THarness& harness,
               const runtime::PhysicalTransitionPlan& plan,
               uint64_t& physical) {
    if (plan.removalRequired) {
        physical &= ~(1ULL << plan.removalSquare);
        Observe(harness, 1ULL << plan.removalSquare, physical);
    }
    for (uint8_t index = 0U; index < plan.relocationCount; ++index) {
        const runtime::PieceRelocation& relocation =
            plan.relocations[index];
        physical &= ~(1ULL << relocation.from);
        Observe(harness, 1ULL << relocation.from, physical);
        physical |= 1ULL << relocation.to;
        Observe(harness, 1ULL << relocation.to, physical);
    }
}

void TestCpuCastlesBeforeMovingDuck() {
    CastleHarness harness("build/test_duck_engine_cpu_castle.json");
    Check(harness.Load(), "scripted Duck CPU castling fixture loads");
    constexpr uint8_t DUCK_FROM = 32U;
    constexpr uint8_t DUCK_TO = 35U;
    uint64_t physical = (1ULL << 4U) | (1ULL << 60U) |
                        (1ULL << 63U) | (1ULL << DUCK_FROM);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    Check(WaitForCpuMove(harness),
          "Duck CPU queues its scripted castling move");
    const runtime::PhysicalTransitionPlan castlePlan =
        harness.stateMachine.GetChessInteractionModel().phase.plan;
    Check(castlePlan.kind ==
                  runtime::PhysicalTransitionKind::Castling &&
              castlePlan.source ==
                  runtime::PhysicalTransitionSource::Cpu &&
              castlePlan.relocationCount == 2U,
          "Duck CPU retains normal king-and-rook castling choreography");
    ApplyPlan(harness, castlePlan, physical);
    fxe::ManualMillis::AdvanceMs(1U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    const runtime::PhysicalTransitionPlan duckPlan =
        harness.stateMachine.GetChessInteractionModel().phase.plan;
    Check(duckPlan.kind ==
                  runtime::PhysicalTransitionKind::DuckRelocation &&
              duckPlan.moveFrom == DUCK_FROM &&
              duckPlan.moveTo == DUCK_TO,
          "castling completion is followed by the selected duck move");
}

void TestCpuCompletesChessMoveThenFixedDuckMove() {
    Harness harness("build/test_duck_engine_cpu.json");
    Check(harness.LoadDuck(5U, true, false),
          "Duck CPU fixture loads with CPU owning White");
    uint64_t physical = INITIAL_OCCUPANCY | (1ULL << DUCK);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    Check(WaitForCpuMove(harness),
          "Duck-aware CPU queues its chosen chess action");

    const runtime::PhysicalTransitionPlan chessPlan =
        harness.stateMachine.GetChessInteractionModel().phase.plan;
    Check(chessPlan.source ==
                  runtime::PhysicalTransitionSource::Cpu &&
              chessPlan.kind !=
                  runtime::PhysicalTransitionKind::DuckRelocation,
          "CPU first exposes an ordinary chess choreography");
    ApplyPlan(harness, chessPlan, physical);
    fxe::ManualMillis::AdvanceMs(1U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();

    const runtime::PhysicalTransitionPlan duckPlan =
        harness.stateMachine.GetChessInteractionModel().phase.plan;
    Check(harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::DuckRelocation &&
              duckPlan.kind ==
                  runtime::PhysicalTransitionKind::DuckRelocation,
          "CPU chess action advances to its duck choreography");
    Check(duckPlan.source ==
                  runtime::PhysicalTransitionSource::Cpu &&
              duckPlan.moveFrom == DUCK &&
              duckPlan.moveTo < 64U && duckPlan.moveTo != DUCK,
          "CPU duck relocation has the engine-selected fixed destination");
    runtime::InvariantReport<8> duckInvariantReport;
    runtime::CheckChessInteractionModelInvariants(
        harness.stateMachine.GetChessInteractionModel(),
        duckInvariantReport);
    Check(duckInvariantReport.IsOk(),
          "CPU-owned duck relocation satisfies interaction invariants");

    ApplyPlan(harness, duckPlan, physical);
    harness.coordinator.Update();
    Check(harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::ChessMove &&
              harness.coordinator.GetDuckSquare() == duckPlan.moveTo &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.engine.GetSideToMove() ==
                  modules::SideToMove::Black,
          "CPU combined turn commits only after the fixed duck move");

    Check(harness.coordinator.RequestUndo(1U),
          "CPU Duck turn accepts one whole-turn undo request");
    const runtime::PhysicalTransitionPlan duckUndoPlan =
        harness.stateMachine.GetChessInteractionModel().phase.plan;
    Check(duckUndoPlan.kind ==
                  runtime::PhysicalTransitionKind::DuckRelocation &&
              duckUndoPlan.source ==
                  runtime::PhysicalTransitionSource::Human &&
              duckUndoPlan.moveFrom == duckPlan.moveTo &&
              duckUndoPlan.moveTo == DUCK,
          "undo prompts the player to return the CPU's duck first");
    ApplyPlan(harness, duckUndoPlan, physical);
    harness.coordinator.Update();
    uint8_t undoFrom = 255U;
    uint8_t undoTo = 255U;
    Check(harness.stateMachine.IsUndoPromptActive() &&
              harness.stateMachine.TryGetUndoPromptMove(
                  undoFrom, undoTo) &&
              undoFrom == chessPlan.moveFrom &&
              undoTo == chessPlan.moveTo,
          "CPU whole-turn undo prompts the chess action only after duck return");
}

void TestCpuCompoundChoiceSurvivesReboot() {
    static constexpr char PATH[] =
        "build/test_duck_engine_cpu_reboot.json";
    Harness before(PATH);
    Check(before.LoadDuck(5U, true, false),
          "Duck CPU reboot fixture loads");
    uint64_t physical = INITIAL_OCCUPANCY | (1ULL << DUCK);
    before.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    Check(WaitForCpuMove(before) &&
              before.coordinator.FlushToSettingsNow(),
          "queued Duck CPU compound choice persists before choreography");

    Harness after(PATH, false);
    Check(after.LoadExisting(),
          "queued Duck CPU compound choice reloads");
    after.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    Check(after.stateMachine.HasQueuedCpuMove(),
          "reboot restores the normal CPU chess choreography");
    const runtime::PhysicalTransitionPlan chessPlan =
        after.stateMachine.GetChessInteractionModel().phase.plan;
    ApplyPlan(after, chessPlan, physical);
    fxe::ManualMillis::AdvanceMs(1U);
    after.stateMachine.Tick();
    after.coordinator.Update();
    const runtime::PhysicalTransitionPlan duckPlan =
        after.stateMachine.GetChessInteractionModel().phase.plan;
    Check(duckPlan.kind ==
                  runtime::PhysicalTransitionKind::DuckRelocation &&
              duckPlan.source ==
                  runtime::PhysicalTransitionSource::Cpu &&
              duckPlan.moveFrom == DUCK &&
              duckPlan.moveTo < 64U && duckPlan.moveTo != DUCK,
          "rebooted CPU chess action resumes its persisted fixed duck move");
}

void TestHighLevelCpuCommitsTheSafeTacticalCompoundChoice() {
    static constexpr char FEN[] = "3k4/8/5n2/3r4/8/8/8/3Q3K w - - 0 1";
    constexpr uint8_t DUCK_FROM = 32U; // a4
    Harness harness("build/test_duck_engine_cpu_tactics.json");
    Check(harness.LoadDuck(8U, false, false, FEN, DUCK_FROM),
          "level-eight CPU restores the poisoned-rook Duck position");
    uint64_t physical = (1ULL << 3U) | (1ULL << 21U) | (1ULL << 27U) |
                        (1ULL << 59U) | (1ULL << 63U) | (1ULL << DUCK_FROM);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    Check(WaitForCpuMove(harness),
          "the real CPU coordinator queues a high-level tactical choice");
    modules::DuckThinkResult result;
    Check(harness.engine.GetLastDuckThinkResult(result) &&
              result.chessMove.View() != "d1d5" && !result.capturedKing,
          "configured CPU difficulty reaches the deeper engine and declines the poisoned rook");
    const auto chessPlan = harness.stateMachine.GetChessInteractionModel().phase.plan;
    ApplyPlan(harness, chessPlan, physical);
    fxe::ManualMillis::AdvanceMs(1U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    const auto duckPlan = harness.stateMachine.GetChessInteractionModel().phase.plan;
    Check(duckPlan.kind == runtime::PhysicalTransitionKind::DuckRelocation &&
              duckPlan.moveFrom == DUCK_FROM && duckPlan.moveTo == result.duckDestination &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "the tactical chess move retains its selected Duck step before atomic commit");
    ApplyPlan(harness, duckPlan, physical);
    harness.coordinator.Update();
    Check(harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == result.duckDestination &&
              harness.engine.GetSideToMove() == modules::SideToMove::Black,
          "the selected tactical compound turn commits once after both physical steps");
    char fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    Check(harness.engine.GetFen(fen, sizeof(fen)), "committed tactical FEN is readable");
    bool haveQueen = false;
    for (uint8_t to = 0U; to < 64U; ++to) {
        char piece = '\0';
        if (!modules::TryGetFenPieceAtSquare(fen, to, piece) || piece != 'Q') {
            continue;
        }
        haveQueen = true;
        for (uint8_t from = 0U; from < 64U; ++from) {
            uint64_t destinations = 0ULL;
            Check(litch_max::GetDuckLegalDestinationsMask(
                      fen, result.duckDestination, from, destinations) &&
                      (destinations & (1ULL << to)) == 0ULL,
                  "the physically committed choice leaves no immediate Black queen capture");
        }
    }
    Check(haveQueen, "the CPU keeps its queen through the actual coordinator journey");
}

} // namespace

int main() {
    std::puts("[DUCK_ENGINE_AWARENESS] start");
    fxe::test_support::ManualMillisGuard millisGuard;
    fxe::ManualMillis::SetNowMs(1000U);
    TestHumanDuckTurnGetsOneCompoundHint();
    TestCpuPlayerOneStartsDuckGame();
    TestCpuCastlesBeforeMovingDuck();
    TestCpuCompletesChessMoveThenFixedDuckMove();
    TestCpuCompoundChoiceSurvivesReboot();
    TestHighLevelCpuCommitsTheSafeTacticalCompoundChoice();
    if (g_failures == 0) {
        std::puts("[DUCK_ENGINE_AWARENESS] ok");
        return 0;
    }
    std::printf("[DUCK_ENGINE_AWARENESS] failures=%d\n", g_failures);
    return 1;
}
