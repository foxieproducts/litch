#include <cstdio>

#include "foxie_embedded/test_support/manual_millis_guard.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {
class ClockBoundaryEngine final : public modules::IEngine {
  public:
    explicit ClockBoundaryEngine(runtime::ManualClock& clock) : m_clock(clock) {}
    void Reset() override { m_engine.Reset(); }
    bool SetFen(const char* fen) override { return m_engine.SetFen(fen); }
    bool GetFen(char* fen, size_t size) const override { return m_engine.GetFen(fen, size); }
    bool ApplyUciMove(const char* move) override {
        const bool accepted = m_engine.ApplyUciMove(move);
        if (accepted && advanceDuringApply) {
            advanceDuringApply = false;
            m_clock.AdvanceMs(2U);
        }
        return accepted;
    }
    modules::SideToMove GetSideToMove() const override { return m_engine.GetSideToMove(); }
    bool IsInCheck(modules::SideToMove side) const override { return m_engine.IsInCheck(side); }
    bool IsInCheckmate(modules::SideToMove side) const override { return m_engine.IsInCheckmate(side); }
    bool IsInStalemate(modules::SideToMove side) const override { return m_engine.IsInStalemate(side); }
    bool GetLegalDestinationsMask(uint8_t from, uint64_t& mask) override { return m_engine.GetLegalDestinationsMask(from, mask); }
    bool StartThink(const modules::ThinkConfig&) override {
        ++searchStarts;
        return false; // Observe admission without running search under a stationary clock.
    }
    void CancelThink() override { m_engine.CancelThink(); }
    modules::ThinkState PollThinkProgress(uint8_t& progress, modules::UciMove& move) override { return m_engine.PollThinkProgress(progress, move); }
    bool GetLastThinkStats(modules::ThinkStats& stats) const override { return m_engine.GetLastThinkStats(stats); }
    bool advanceDuringApply = false;
    uint32_t searchStarts = 0U;
  private:
    runtime::ManualClock& m_clock;
    modules::LitchMaxEngine m_engine;
};
} // namespace

int RunSecondClockEvaluationTimeoutRegression() {
    constexpr const char* journey = "second-clock-evaluation-timeout-does-not-restart-hint";
    runtime::ManualClock clock(1000U);
    clock.SetNowMs(1000U);
    fxe::test_support::ManualMillisGuard millisGuard;
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/second_clock_timeout.json";
    (void)std::remove(settingsConfig.path);
    (void)std::remove("build/second_clock_timeout.json.bak");
    (void)std::remove("build/second_clock_timeout.json.tmp");
    modules::SettingsStore settings(settingsConfig);
    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 10U;
    stateConfig.placementStableRequiredMs = 0U;
    stateConfig.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine state(stateConfig);
    state.Reset();
    ClockBoundaryEngine engine(clock);
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(settings, engine, state);
    bool ok = settings.Load();
    settings.Doc()["TIMER"] = 1U;
    ok = ok && coordinator.LoadOrInitialize() &&
         coordinator.LoadExternalFen("8/8/8/8/8/8/4P3/4K2k w - - 0 1");
    state.DebugSetPhysicalOccupancyMask((UINT64_C(1) << 52U) | (UINT64_C(1) << 60U) | (UINT64_C(1) << 63U));
    state.Tick();
    coordinator.Update();
    clock.AdvanceMs(750U);
    coordinator.Update();
    state.HandleSensorChange(52U, false);
    coordinator.Update();
    ok = ok && coordinator.GetGameClock().IsRunning();
    clock.SetNowMs(61739U);
    state.HandleSensorChange(36U, true);
    coordinator.Update();
    clock.AdvanceMs(10U);
    state.Tick();
    const uint32_t startsBefore = engine.searchStarts;
    engine.advanceDuringApply = true;
    coordinator.Update();
    ok = ok && !engine.advanceDuringApply &&
         coordinator.GetGameClock().HasTimedOut() &&
         !state.IsHintThinkingActive() && engine.searchStarts == startsBefore;
    const uint32_t history = coordinator.GetUndoHistoryMoveCount();
    coordinator.Update();
    ok = ok && engine.searchStarts == startsBefore &&
         coordinator.GetUndoHistoryMoveCount() == history;
    std::printf("[CLOCK_BOUNDARY_JOURNEY] %s t=%lu timed_out=%u hint=%u starts_before=%lu starts_after=%lu history=%lu result=%s\n",
                journey, static_cast<unsigned long>(clock.NowMs()),
                coordinator.GetGameClock().HasTimedOut(), state.IsHintThinkingActive(),
                static_cast<unsigned long>(startsBefore), static_cast<unsigned long>(engine.searchStarts),
                static_cast<unsigned long>(history), ok ? "PASS" : "FAIL");
    if (!ok) std::printf("[FAIL] %s\n", journey);
    return ok ? 0 : 1;
}
