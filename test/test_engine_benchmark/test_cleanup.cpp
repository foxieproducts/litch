#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/player_end_palette.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "virtual_litch_session.hpp"
namespace {
// Test-only gate wraps the actual adapter. It parks the existing worker at
// its real statistics/cleanup boundary; it does not emulate search or board state.
class CleanupGateEngine final : public modules::IEngine {
  public:
    void Reset() override { Track(); m_engine.Reset(); }
    bool SetFen(const char* fen) override { Track(); return m_engine.SetFen(fen); }
    bool GetFen(char* fen, size_t size) const override { Track(); return m_engine.GetFen(fen, size); }
    bool ApplyUciMove(const char* move) override { Track(); ++mutations; return m_engine.ApplyUciMove(move); }
    modules::SideToMove GetSideToMove() const override { Track(); return m_engine.GetSideToMove(); }
    bool IsInCheck(modules::SideToMove side) const override { Track(); return m_engine.IsInCheck(side); }
    bool IsInCheckmate(modules::SideToMove side) const override { Track(); return m_engine.IsInCheckmate(side); }
    bool IsInStalemate(modules::SideToMove side) const override { Track(); return m_engine.IsInStalemate(side); }
    bool GetLegalDestinationsMask(uint8_t from, uint64_t& mask) override {
        Track(); return m_engine.GetLegalDestinationsMask(from, mask);
    }
    bool StartThink(const modules::ThinkConfig& config) override { Track(); return m_engine.StartThink(config); }
    bool StartBenchmark(uint8_t id, const modules::ThinkConfig& config) override {
        Track(); if (failAdmission) return false;
        auto bounded = config; bounded.maxNodes = 1U; bounded.firstRootNodeReserve = 0U;
        return m_engine.StartBenchmark(id, bounded);
    }
    void CancelThink() override { m_engine.CancelThink(); }
    modules::ThinkState PollThinkProgress(uint8_t& progress, modules::UciMove& best) override {
        return m_engine.PollThinkProgress(progress, best);
    }
    bool GetLastThinkStats(modules::ThinkStats& stats) const override {
        atCleanup.store(true, std::memory_order_release);
        while (!releaseCleanup.load(std::memory_order_acquire)) std::this_thread::yield();
        return m_engine.GetLastThinkStats(stats);
    }
    mutable std::atomic<bool> atCleanup{false};
    std::atomic<bool> releaseCleanup{false};
    mutable std::atomic<uint32_t> unsafe{0U};
    uint32_t mutations = 0U;
    bool failAdmission = false;
  private:
    void Track() const {
        if (atCleanup.load(std::memory_order_acquire) && !releaseCleanup.load(std::memory_order_acquire)) ++unsafe;
    }
    modules::LitchMaxEngine m_engine;
};
} // namespace
void CheckBenchmarkCleanup(fxe::test_support::TestResults& results) {
    constexpr const char* PATH = "build/test_engine_benchmark_cleanup.json";
    (void)std::remove(PATH);
    (void)std::remove("build/test_engine_benchmark_cleanup.json.bak");
    (void)std::remove("build/test_engine_benchmark_cleanup.json.tmp");
    { std::ifstream input("test/data/terminal_checkmate_107.json"); std::ofstream output(PATH); output << input.rdbuf(); }
    modules::SettingsStoreConfig config;
    config.path = PATH;
    modules::SettingsStore settings(config);
    results.Check(settings.Load(), "cleanup fixture loads actual retained history");
    CleanupGateEngine engine;
    runtime::GameStateMachineConfig smConfig;
    smConfig.settleDurationMs = 0U;
    smConfig.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(smConfig);
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine, nullptr, &worker);
    coordinator.SetBenchmarkControlsIdleProbe(nullptr, [](void*) { return true; });
    results.Check(coordinator.LoadOrInitialize(), "cleanup gate traverses production coordinator");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    const uint64_t expected = stateMachine.GetExpectedOccupancyMask();
    (void)dispatcher.DispatchBoardObservation({fxe::Millis(), 1U, expected, expected});
    coordinator.Update();
    engine.failAdmission = true;
    modules::EngineBenchmarkStatus failed;
    results.Check(!coordinator.StartBenchmark(1U, 1U) && coordinator.TryGetBenchmarkStatus(failed) &&
        failed.diagnostics.state == modules::ThinkState::Error && failed.diagnostics.quiescentValid &&
        !coordinator.IsEngineLaneActive(), "adapter start failure leaves coherent cached quiescent error");
    engine.failAdmission = false;
    results.Check(coordinator.StartBenchmark(1U, 1U), "cleanup-gated benchmark admitted");
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!engine.atCleanup.load() && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    results.Check(engine.atCleanup.load() && worker.IsActive(), "completed Poll remains active during real worker statistics cleanup");
    const uint32_t mutations = engine.mutations;
    coordinator.RequestUndo(1U);
    for (uint32_t tick = 0U; tick < 32U; ++tick) coordinator.Update();
    modules::EngineBenchmarkStatus status;
    results.Check(coordinator.TryGetBenchmarkStatus(status) && status.cancelRequested && worker.IsActive() &&
        engine.unsafe.load() == 0U && engine.mutations == mutations,
        "Undo waits without live getters or engine mutation while whole lane cleanup is parked");
    engine.releaseCleanup.store(true, std::memory_order_release);
    const auto releaseEnd = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (worker.IsActive() && std::chrono::steady_clock::now() < releaseEnd) std::this_thread::yield();
    for (uint32_t tick = 0U; tick < 5U; ++tick) coordinator.Update();
    results.Check(!worker.IsActive() && engine.unsafe.load() == 0U && coordinator.HasPendingUndoCommand() == false &&
        stateMachine.GetExpectedOccupancyMask() != expected,
        "after cleanup gameplay Undo resumes and requests physical premate restoration");
}

namespace {
struct RawControlStart {
    modules::GamePersistenceCoordinator* owner = nullptr;
    bool accepted = false;
    static void Run(void* context, modules::SettingsStore&, modules::ControlMutationService&,
                    modules::GamePersistenceCoordinator& owner) {
        auto& self = *static_cast<RawControlStart*>(context);
        self.owner = &owner;
        self.accepted = owner.StartBenchmark(1U, 1U);
    }
};
void RawCheckpoint(runtime::VirtualLitchSession& session, const char* name, bool canceled) {
    const auto state = session.State();
    std::printf("BENCH_RAW checkpoint=%s ms=%u observed=%016llx expected=%016llx history=%u phase=%u required=%u canceled=%u frame=%u semantic=%016llx physical=%016llx display=%.*s next=release-control-then-worker-cleanup\n",
        name, state.now_ms, static_cast<unsigned long long>(state.physical_occupied_mask),
        static_cast<unsigned long long>(state.expected_occupied_mask), session.UndoHistoryMoveCount(),
        state.interaction_phase, state.required_action, canceled, session.SemanticFrame().frame_sequence,
        static_cast<unsigned long long>(session.SemanticFrame().combined_hash),
        static_cast<unsigned long long>(session.PhysicalFrame().frame_hash),
        int(session.DisplayText().size()), session.DisplayText().data());
}
}
void CheckBenchmarkRawControls(fxe::test_support::TestResults& results) {
    for (const uint8_t owner : {VIRTUAL_LITCH_OWNER_P1, VIRTUAL_LITCH_OWNER_P2}) {
        for (const auto button : {runtime::VirtualLitchPhysicalButton::Up, runtime::VirtualLitchPhysicalButton::Down,
                                 runtime::VirtualLitchPhysicalButton::X, runtime::VirtualLitchPhysicalButton::O}) {
            constexpr const char* PATH = "build/test_benchmark_raw_controls.json";
            (void)std::remove(PATH); (void)std::remove("build/test_benchmark_raw_controls.json.bak");
            (void)std::remove("build/test_benchmark_raw_controls.json.tmp");
            { std::ifstream input("test/data/terminal_checkmate_107.json"); std::ofstream output(PATH); output << input.rdbuf(); }
            CleanupGateEngine engine;
            modules::AsyncThinkWorker worker;
            virtual_litch_config config{}; virtual_litch_default_config(&config);
            config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
            runtime::VirtualLitchSession session(config, engine, worker, PATH);
            session.ObserveBootHallMask(session.State().expected_occupied_mask);
            const bool direction = button == runtime::VirtualLitchPhysicalButton::Up || button == runtime::VirtualLitchPhysicalButton::Down;
            RawControlStart start;
            if (direction) {
                session.RunControlIngress(&RawControlStart::Run, &start);
                results.Check(start.accepted, "RAW benchmark admits before direction shortcut");
                const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (!engine.atCleanup.load() && std::chrono::steady_clock::now() < end) std::this_thread::yield();
                results.Check(engine.atCleanup.load(), "RAW real worker statistics parked");
            }
            const auto paletteBefore = modules::player_end_palette::ReadSettingUInt32(session.SettingsForPersistenceTest(),
                owner == VIRTUAL_LITCH_OWNER_P1 ? "P1_PIECE_WHEEL" : "P2_PIECE_WHEEL",
                owner == VIRTUAL_LITCH_OWNER_P1 ? 170U : 0U);
            session.SetButtonDown(owner, button, true); session.Advance(20U);
            if (!direction) {
                session.Advance(780U);
                session.RunControlIngress(&RawControlStart::Run, &start);
                results.Check(!start.accepted, "RAW both-owner X/O holds reject benchmark at800ms");
                RawCheckpoint(session, "held800-rejected", false);
                if (button == runtime::VirtualLitchPhysicalButton::X) {
                    session.Advance(400U);
                    results.Check(session.QuickSettingsActive(), "RAW original X hold still opens Quick Settings after rejection");
                    session.RunControlIngress(&RawControlStart::Run, &start);
                    results.Check(!start.accepted, "RAW open menu continues to reject benchmark");
                }
            } else {
                modules::EngineBenchmarkStatus status;
                results.Check(start.owner->TryGetBenchmarkStatus(status) && status.cancelRequested && worker.IsActive() &&
                    engine.unsafe.load() == 0U && session.UndoHistoryMoveCount() == 107U,
                    "RAW both-owner Up/Down shortcut cancels before consuming event, without active getter/mutation");
                results.Check(session.SettingsForPersistenceTest().ReadValue(
                    owner == VIRTUAL_LITCH_OWNER_P1 ? "P1_PIECE_WHEEL" : "P2_PIECE_WHEEL", uint32_t{255U}) != paletteBefore,
                    "RAW palette shortcut retains ordinary setting mutation");
                RawCheckpoint(session, "shortcut20-canceled", status.cancelRequested);
                const auto sequence = session.SemanticFrame().frame_sequence;
                const auto physical = session.PhysicalFrame().frame_hash;
                session.Advance(200U);
                results.Check(session.SemanticFrame().frame_sequence > sequence &&
                    session.PhysicalFrame().frame_hash != physical && worker.IsActive() && engine.unsafe.load() == 0U,
                    "RAW timed semantic and mapped V3 frames continue while worker cleanup remains parked");
                RawCheckpoint(session, "shortcut220-rendered", true);
                session.Advance(500U); // Existing repeat path also remains cancellation-safe.
            }
            session.SetButtonDown(owner, button, false); session.Advance(20U);
            engine.releaseCleanup.store(true);
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (worker.IsActive() && std::chrono::steady_clock::now() < end) std::this_thread::yield();
            session.Advance(1U);
            results.Check(!worker.IsActive() && engine.unsafe.load() == 0U, "RAW release reaches full cleanup safely");
            if (direction) results.Check(session.UndoHistoryMoveCount() == 107U,
                "RAW ordinary palette shortcut leaves terminal history intact");
        }
    }
}
