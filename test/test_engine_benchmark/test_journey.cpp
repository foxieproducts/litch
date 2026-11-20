#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/orientation.hpp"
#include "virtual_litch_session.hpp"
namespace {
void Checkpoint(runtime::VirtualLitchSession& session, const char* name, const char* next) {
    const auto state = session.State();
    modules::ControlMutationOutcome ack;
    const bool hasAck = session.TryGetLastBoardControlOutcome(ack);
    std::printf("BENCH_JOURNEY checkpoint=%s ms=%u observed=%016llx expected=%016llx phase=%u required=%u history=%u ack=%u/%u/%u display=%.*s next=%s\n",
        name, state.now_ms, static_cast<unsigned long long>(state.physical_occupied_mask),
        static_cast<unsigned long long>(state.expected_occupied_mask), state.interaction_phase,
        state.required_action, session.UndoHistoryMoveCount(), hasAck, unsigned(ack.status),
        unsigned(ack.actionState), int(session.DisplayText().size()), session.DisplayText().data(), next);
    const bool rotate = modules::ComputeSquareRotate(session.BoardRotate(), session.FirstPlayerIsP2(), false);
    for (const uint8_t square : {19U, 40U}) {
        const auto mapped = runtime::MapSquareIndex(square, rotate);
        std::printf("BENCH_CUE checkpoint=%s square=%u semantic=", name, square);
        const auto& semantic = session.SemanticFrame();
        for (uint8_t i = 0U; i < semantic.leds_per_square; ++i) {
            const auto c = semantic.board[square][i]; std::printf("%u,%u,%u;", c.r, c.g, c.b);
        }
        std::printf(" mapped_V3=%u physical=", mapped);
        for (uint8_t i = 0U; i < 4U; ++i) {
            const auto c = session.PhysicalFrame().pixels[mapped * 4U + i];
            std::printf("%u,%u,%u,%u;", c.r, c.g, c.b, c.w);
        }
        std::puts("");
    }
}
struct Start {
    bool accepted = false;
    modules::GamePersistenceCoordinator* coordinator = nullptr;
    static void Run(void* context, modules::SettingsStore&, modules::ControlMutationService&,
                    modules::GamePersistenceCoordinator& coordinator) {
        auto& self = *static_cast<Start*>(context);
        self.coordinator = &coordinator;
        self.accepted = coordinator.StartBenchmark(6U, 11U);
    }
};
} // namespace
void CheckBenchmarkJourney(fxe::test_support::TestResults& results) {
    constexpr const char* PATH = "build/test_engine_benchmark_journey.json";
    (void)std::remove(PATH);
    (void)std::remove("build/test_engine_benchmark_journey.json.bak");
    (void)std::remove("build/test_engine_benchmark_journey.json.tmp");
    { std::ifstream input("test/data/terminal_checkmate_107.json"); std::ofstream output(PATH); output << input.rdbuf(); }
    modules::LitchMaxEngine engine;
    modules::AsyncThinkWorker worker;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession session(config, engine, worker, PATH);
    const uint64_t terminal = session.State().expected_occupied_mask;
    session.ObserveBootHallMask(terminal);
    results.Check(session.UndoHistoryMoveCount() == 107U, "BENCH-TERMINAL actual107 imported with raw Hall occupancy");
    Checkpoint(session, "terminal107", "start fixed diagnostic");
    const auto before = session.SemanticFrame();
    Start start;
    session.RunControlIngress(&Start::Run, &start);
    results.Check(start.accepted && before.board_hash == session.SemanticFrame().board_hash &&
        session.UndoHistoryMoveCount() == 107U, "BENCH-TERMINAL active compute retains checkmate semantic render and game");
    Checkpoint(session, "compute-active", "press raw X to cancel and Undo");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, true);
    session.Advance(20U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, false);
    session.Advance(20U);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (worker.IsActive() && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    session.Advance(1U);
    modules::ControlMutationOutcome ack;
    modules::EngineBenchmarkStatus status;
    results.Check(start.coordinator->TryGetBenchmarkStatus(status) && status.cancelRequested &&
        session.TryGetLastBoardControlOutcome(ack) && ack.status == modules::ControlMutationStatus::Applied &&
        ack.actionState == modules::ControlActionState::UndoPending && session.UndoHistoryMoveCount() == 106U,
        "BENCH-TERMINAL raw X cancels compute, cleanup releases, actual Applied/UndoPending106 ACK");
    results.Check(session.SemanticFrame().board[19U][0U].r != 0U &&
        session.SemanticFrame().board[19U][1U].b != 0U && session.SemanticFrame().board[40U][1U].b != 0U &&
        session.PhysicalFrame().pixels[19U * 4U].r != 0U &&
        session.PhysicalFrame().pixels[19U * 4U + 1U].b != 0U &&
        session.PhysicalFrame().pixels[19U * 4U + 3U].b != 0U &&
        session.PhysicalFrame().pixels[40U * 4U + 1U].b != 0U,
        "BENCH-TERMINAL Undo lights semantic and both mapped d6 landing Piece LEDs with the occupied a3 source cue");
    Checkpoint(session, "raw-X-UndoPending106", "lift queen a3 then return d6");
    const uint64_t lifted = terminal & ~(UINT64_C(1) << 40U);
    const uint64_t premate = lifted | (UINT64_C(1) << 19U);
    session.ObserveHallMask(lifted);
    Checkpoint(session, "queen-lift-a3", "place queen d6");
    session.ObserveHallMask(premate);
    session.Advance(1U);
    results.Check(!session.IsUndoActionPending() && session.State().expected_occupied_mask == premate,
        "BENCH-TERMINAL physical restoration clears Undo and presents play-ready106");
    bool sourceCleared = true;
    for (const auto color : session.SemanticFrame().board[40U])
        sourceCleared = sourceCleared && color.r == 0U && color.g == 0U && color.b == 0U;
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        const auto color = session.PhysicalFrame().pixels[40U * 4U + slot];
        sourceCleared = sourceCleared && color.r == 0U && color.g == 0U && color.b == 0U && color.w == 0U;
    }
    results.Check(sourceCleared, "BENCH-TERMINAL restoration clears the vacated a3 source guidance in both outputs");
    Checkpoint(session, "restored106", "play Qd6-a3 to mate again");
    results.Check(!start.coordinator->StartBenchmark(1U, 1U), "BENCH-TERMINAL nonterminal premate rejects diagnostic");
    session.ObserveHallMask(lifted);
    session.ObserveHallMask(terminal);
    session.Advance(2000U);
    const auto remateEnd = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (session.UndoHistoryMoveCount() != 107U && std::chrono::steady_clock::now() < remateEnd) {
        session.Advance(1U);
        std::this_thread::yield();
    }
    results.Check(session.UndoHistoryMoveCount() == 107U && session.State().expected_occupied_mask == terminal,
        "BENCH-TERMINAL same mate resumes normal game admission after canceled benchmark");
    Checkpoint(session, "remate107", "leave game intact");
}
