#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/settings_store.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

class FakeEngine final : public modules::IEngine {
  public:
    explicit FakeEngine(const char* fen) {
        if (fen != nullptr) {
            std::strncpy(m_fen, fen, sizeof(m_fen));
            m_fen[sizeof(m_fen) - 1] = '\0';
        }
    }

    void Reset() override {
        m_fen[0] = '\0';
    }

    bool SetFen(const char* fen) override {
        if (fen == nullptr) {
            return false;
        }
        std::strncpy(m_fen, fen, sizeof(m_fen));
        m_fen[sizeof(m_fen) - 1] = '\0';
        return true;
    }

    bool GetFen(char* outFen, size_t outSize) const override {
        if (outFen == nullptr || outSize == 0) {
            return false;
        }
        std::strncpy(outFen, m_fen, outSize);
        outFen[outSize - 1] = '\0';
        return outFen[0] != '\0';
    }

    bool ApplyUciMove(const char*) override {
        return false;
    }

    modules::SideToMove GetSideToMove() const override {
        return modules::SideToMove::Black;
    }

    bool IsInCheck(modules::SideToMove) const override { return false; }
    bool IsInCheckmate(modules::SideToMove) const override { return false; }
    bool IsInStalemate(modules::SideToMove) const override { return false; }

    bool GetLegalDestinationsMask(uint8_t fromSquare, uint64_t& outDestMask64) override {
        outDestMask64 = 0;

        // Allow the deterministic best move used by this test: e8 -> e7.
        // Squares: e8=4, e7=12.
        if (fromSquare == 4U) {
            outDestMask64 = (1ULL << 12);
        }
        return true;
    }

    bool StartThink(const modules::ThinkConfig&) override {
        if (m_thinking) {
            return false;
        }
        ++m_startThinkCount;
        m_thinking = true;
        m_pollCount = 0;
        return true;
    }

    void CancelThink() override {
        ++m_cancelThinkCount;
        m_thinking = false;
    }

    modules::ThinkState PollThinkProgress(uint8_t& outProgressPercent,
                                          modules::UciMove& outBestMoveUci) override {
        outProgressPercent = 0;
        outBestMoveUci.Clear();

        if (!m_thinking) {
            return modules::ThinkState::Idle;
        }

        ++m_pollCount;
        outProgressPercent = static_cast<uint8_t>((m_pollCount * 33U) > 100U ? 100U : (m_pollCount * 33U));

        // Deterministic completion after a few polls.
        if (m_pollCount >= 3U) {
            (void)outBestMoveUci.Assign("e8e7");
            m_thinking = false;
            return modules::ThinkState::Done;
        }

        return modules::ThinkState::Thinking;
    }

    bool GetLastThinkStats(modules::ThinkStats& outStats) const override {
        outStats.nodeCount = 123;
        return true;
    }

    uint32_t StartThinkCount() const { return m_startThinkCount; }
    uint32_t CancelThinkCount() const { return m_cancelThinkCount; }

  private:
    char m_fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    bool m_thinking = false;
    uint32_t m_startThinkCount = 0;
    uint32_t m_cancelThinkCount = 0;
    uint8_t m_pollCount = 0;
};

} // namespace

int main() {
    fxe::test_support::TestResults results("VS_CPU_THINKING_NOT_CANCELED_BY_ILLEGAL");
    results.Start();

    {
        fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(1000);

        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_vs_cpu_thinking_not_canceled_by_illegal.json";
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_vs_cpu_thinking_not_canceled_by_illegal.json.bak");
        (void)std::remove("build/test_vs_cpu_thinking_not_canceled_by_illegal.json.tmp");

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "settings load");

        // Minimal playable position with Black to move (CPU plays for P2;
        // GAME_FIRST_PLAYER=1 => CPU is Black).
        const char* fen = "4k3/8/8/8/8/8/4P3/4K3 b - - 0 1";
        settings.Doc()[modules::KEY_GAME_FEN] = fen;
        settings.Doc()["VS_CPU_LEVEL"] = 5;
        settings.Doc()["GAME_FIRST_PLAYER"] = 1U;

        FakeEngine engine(fen);

        runtime::ManualClock clock(1000);

        runtime::GameStateMachineConfig smCfg;
        smCfg.placementStableRequiredMs = 0;
        smCfg.cpuIdleDelayMs = 0;
        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        results.Check(coord.LoadOrInitialize(), "LoadOrInitialize");

#ifndef ARDUINO
        sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());
#endif

        // Trigger the idle timeout so the coordinator starts thinking.
        clock.SetNowMs(1001);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        sm.Tick();
        coord.Update();

        results.Check(engine.StartThinkCount() == 1U, "think started");
        results.Check(sm.IsCpuThinkingActive(), "state machine thinking active");

        // While thinking, introduce a temporary mismatch/illegal by lifting e8.
        sm.HandleSensorChange(4, false);
        results.Check(sm.HasHumanIllegal() || sm.IsPlacementModeActive(), "mismatch feedback during thinking");

        // Coordinator should keep thinking (no early cancellation).
        coord.Update();
        results.Check(engine.CancelThinkCount() == 0U, "not canceled by illegal");
        results.Check(sm.IsCpuThinkingActive(), "still thinking");

        // Let the think complete while the mismatch remains; move should be pending but not queued.
        coord.Update();
        coord.Update();
        coord.Update();
        results.Check(!sm.IsCpuThinkingActive(), "thinking ended");
        results.Check(!sm.HasQueuedCpuMove(), "cpu move not queued while illegal");
        results.Check(sm.HasHumanIllegal() || sm.IsPlacementModeActive(), "mismatch still active");

        // Correct the mismatch; pending CPU move should queue immediately without restarting think.
        sm.HandleSensorChange(4, true);
        clock.SetNowMs(1002);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        sm.Tick();
        results.Check(!sm.HasHumanIllegal() && !sm.IsPlacementModeActive(), "mismatch cleared");

        coord.Update();
        results.Check(engine.StartThinkCount() == 1U, "no restart think");
        results.Check(sm.HasQueuedCpuMove(), "pending cpu move queued after correction");
        results.Check((sm.HasQueuedCpuMove() && !sm.IsMoveSettlingActive()), "entered CpuMoving");
    }

    return results.Finish();
}
