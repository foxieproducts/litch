#include <cstdio>
#include <cstring>

#include "foxie_embedded/test_support/test_assert.hpp"

#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/settings_store.hpp"
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

    void Reset() override { m_fen[0] = '\0'; }

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

    bool ApplyUciMove(const char*) override { return false; }

    modules::SideToMove GetSideToMove() const override { return modules::SideToMove::White; }

    bool IsInCheck(modules::SideToMove) const override { return false; }
    bool IsInCheckmate(modules::SideToMove) const override { return false; }
    bool IsInStalemate(modules::SideToMove) const override { return false; }

    bool GetLegalDestinationsMask(uint8_t fromSquare, uint64_t& outMask64) override {
        outMask64 = 0;

        // Test position uses:
        // - White pawn at e4 (sq=36), capturing d5 (sq=27).
        // - Kings at e1 (sq=60) and e8 (sq=4).
        if (fromSquare == 36) {
            outMask64 = (1ULL << 27);
            return true;
        }

        // Other pieces: allow the query but no destinations.
        return true;
    }

    bool StartThink(const modules::ThinkConfig&) override { return false; }
    void CancelThink() override {}
    modules::ThinkState PollThinkProgress(uint8_t&, modules::UciMove&) override {
        return modules::ThinkState::Idle;
    }
    bool GetLastThinkStats(modules::ThinkStats&) const override { return false; }

  private:
    char m_fen[litch_max::FEN_BUFFER_CAPACITY] = {};
};

} // namespace

int main() {
    fxe::test_support::TestResults results("CAPTURE_FIRST_VS_CPU_PLACEMENT_GUARD");
    results.Start();

    // Repro:
    // - VS-CPU enabled
    // - Human uses capture-first sequencing (remove opponent piece while Idle)
    // - Coordinator's VS-CPU mismatch guard must NOT force placement mode
    //   (capture-first temporarily makes expected/physical mismatch).
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_capture_first_vs_cpu_placement_guard.json";
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_capture_first_vs_cpu_placement_guard.json.bak");
        (void)std::remove("build/test_capture_first_vs_cpu_placement_guard.json.tmp");

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "settings load");

        // Minimal position where White can capture a Black pawn:
        // white: Ke1, Pe4
        // black: Ke8, pd5
        const char* fen = "4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1";
        settings.Doc()[modules::KEY_GAME_FEN] = fen;
        settings.Doc()["VS_CPU_LEVEL"] = 5;
        settings.Doc()["GAME_FIRST_PLAYER"] = 1U;

        FakeEngine engine(fen);

        runtime::GameStateMachineConfig smCfg;
        smCfg.placementStableRequiredMs = 0;
        smCfg.cpuIdleDelayMs = 999999;
        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        results.Check(coord.LoadOrInitialize(), "LoadOrInitialize");

#ifndef ARDUINO
        sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());
#endif

        // Target square d5 (sq=27): remove the opponent pawn first.
        sm.HandleSensorChange(27, false);
        results.Check(sm.GetChessInteractionModel().phase.tag ==
                          runtime::ChessInteractionPhase::CaptureStaged,
                      "target-first: enters CaptureStaged");
        results.Check(sm.HasCaptureFirstStaging(), "target-first: staging set");
        results.Check(!sm.IsPlacementModeActive(), "target-first: not in placement mode yet");

        coord.Update();

        results.Check(!sm.IsPlacementModeActive(), "coordinator: does not enter placement during capture-first");
    }

    return results.Finish();
}
