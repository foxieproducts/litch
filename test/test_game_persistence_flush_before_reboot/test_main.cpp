#include <cstdio>
#include <cstring>

#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

class FakeEngine final : public modules::IEngine {
  public:
    explicit FakeEngine(const char* fen) {
        if (fen) {
            std::strncpy(m_fen, fen, sizeof(m_fen));
            m_fen[sizeof(m_fen) - 1] = '\0';
        }
    }

    void Reset() override {
        std::strncpy(m_fen, modules::INITIAL_FEN, sizeof(m_fen));
        m_fen[sizeof(m_fen) - 1] = '\0';
    }

    bool SetFen(const char* fen) override {
        if (!fen) {
            return false;
        }
        std::strncpy(m_fen, fen, sizeof(m_fen));
        m_fen[sizeof(m_fen) - 1] = '\0';
        return true;
    }

    bool GetFen(char* outFen, size_t outSize) const override {
        if (!outFen || outSize == 0) {
            return false;
        }
        if (m_failGetFen) {
            return false;
        }

        const char* src = m_fen;
        if (m_overrideGetFen[0] != '\0') {
            src = m_overrideGetFen;
        }
        std::strncpy(outFen, src, outSize);
        outFen[outSize - 1] = '\0';
        return outFen[0] != '\0';
    }

    void SetFailGetFen(const bool fail) { m_failGetFen = fail; }

    void SetOverrideGetFen(const char* fen) {
        m_overrideGetFen[0] = '\0';
        if (!fen) {
            return;
        }
        std::strncpy(m_overrideGetFen, fen, sizeof(m_overrideGetFen));
        m_overrideGetFen[sizeof(m_overrideGetFen) - 1] = '\0';
    }

    bool ApplyUciMove(const char*) override { return false; }

    modules::SideToMove GetSideToMove() const override { return modules::SideToMove::White; }

    bool IsInCheck(modules::SideToMove) const override { return false; }
    bool IsInCheckmate(modules::SideToMove) const override { return false; }
    bool IsInStalemate(modules::SideToMove) const override { return false; }

    bool GetLegalDestinationsMask(uint8_t, uint64_t&) override { return false; }

    bool StartThink(const modules::ThinkConfig&) override { return false; }
    void CancelThink() override {}
    modules::ThinkState PollThinkProgress(uint8_t&, modules::UciMove&) override {
        return modules::ThinkState::Idle;
    }
    bool GetLastThinkStats(modules::ThinkStats&) const override { return false; }

  private:
    char m_fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    char m_overrideGetFen[litch_max::FEN_BUFFER_CAPACITY] = {};
    mutable bool m_failGetFen = false;
};

} // namespace

int main() {
    fxe::test_support::TestResults results("GAME_PERSISTENCE_FLUSH_BEFORE_REBOOT");
    results.Start();

    // Regression coverage:
    // - If the settings doc has been mutated since the last persistence snapshot,
    //   FlushToSettingsNow() must re-write GAME_* keys and force-save.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_game_persistence_flush_before_reboot.json";
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_game_persistence_flush_before_reboot.json.bak");
        (void)std::remove("build/test_game_persistence_flush_before_reboot.json.tmp");

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "settings load");

        FakeEngine engine(modules::INITIAL_FEN);
        runtime::GameStateMachine stateMachine;
        modules::GamePersistenceCoordinator persist(settings, engine, stateMachine);

        // Simulate some other code path that rewrites settings without the GAME_* keys.
        settings.Doc().clear();
        settings.Doc()["BRIGHTNESS"] = 3;

        results.Check(persist.FlushToSettingsNow(), "flush persists snapshot");

        // Reload from storage and ensure GAME_FEN exists.
        modules::SettingsStore settings2(cfg);
        results.Check(settings2.Load(), "settings reload");

        const char* fen = settings2.ReadValue(modules::KEY_GAME_FEN, "");
        results.Check(fen != nullptr && std::strcmp(fen, modules::INITIAL_FEN) == 0,
                      "GAME_FEN preserved after flush + reload");
    }

    // Regression coverage:
    // - If the engine's GetFen() can return a transient search node during thinking,
    //   FlushToSettingsNow() must persist the cached stable FEN instead.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/t_gp_flush_think.json";
        (void)std::remove(cfg.path);
        (void)std::remove("build/t_gp_flush_think.json.bak");
        (void)std::remove("build/t_gp_flush_think.json.tmp");

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "settings load (thinking transient)");

        FakeEngine engine(modules::INITIAL_FEN);
        runtime::GameStateMachine stateMachine;
        modules::GamePersistenceCoordinator persist(settings, engine, stateMachine);

        // Ensure cached FEN is populated from a stable state.
        results.Check(persist.LoadOrInitialize(), "load/init seeds cached FEN (thinking transient)");

        // Simulate unrelated settings mutation that could drop GAME_* keys.
        settings.Doc().clear();
        settings.Doc()["BRIGHTNESS"] = 5;

        // While thinking, GetFen returns an incorrect/transient position.
        stateMachine.SetCpuThinkingActive(true);
        engine.SetOverrideGetFen("8/8/8/8/8/8/8/8 w - - 0 1");

        results.Check(persist.FlushToSettingsNow(), "flush persists snapshot (thinking transient)");

        modules::SettingsStore settings2(cfg);
        results.Check(settings2.Load(), "settings reload (thinking transient)");

        const char* fen = settings2.ReadValue(modules::KEY_GAME_FEN, "");
        results.Check(fen != nullptr && std::strcmp(fen, modules::INITIAL_FEN) == 0,
                      "GAME_FEN preserved (uses cached stable FEN while thinking)");
    }

    // Regression coverage:
    // - If the engine can't provide a FEN snapshot right now (e.g., CPU thinking),
    //   FlushToSettingsNow() must not clear the last cached FEN and drop GAME_* keys.
    {
        modules::SettingsStoreConfig cfg;
        // Keep paths short: SettingsStore derives tmp/bak filenames from this.
        cfg.path = "build/t_gp_flush_busy.json";
        (void)std::remove(cfg.path);
        (void)std::remove("build/t_gp_flush_busy.json.bak");
        (void)std::remove("build/t_gp_flush_busy.json.tmp");

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "settings load (engine busy)");

        FakeEngine engine(modules::INITIAL_FEN);
        runtime::GameStateMachine stateMachine;
        modules::GamePersistenceCoordinator persist(settings, engine, stateMachine);

        // Ensure coordinator has a cached FEN first.
        results.Check(persist.LoadExternalFen(modules::INITIAL_FEN), "seed cached FEN (engine busy)");

        // Simulate some other code path that rewrites settings without the GAME_* keys.
        settings.Doc().clear();
        settings.Doc()["BRIGHTNESS"] = 4;

        engine.SetFailGetFen(true);
        results.Check(persist.FlushToSettingsNow(), "flush persists snapshot (engine busy)");

        // Reload from storage and ensure GAME_FEN exists.
        modules::SettingsStore settings2(cfg);
        results.Check(settings2.Load(), "settings reload (engine busy)");

        const char* fen = settings2.ReadValue(modules::KEY_GAME_FEN, "");
        results.Check(fen != nullptr && std::strcmp(fen, modules::INITIAL_FEN) == 0,
                      "GAME_FEN preserved after flush + reload (engine busy)");
    }

    // Regression coverage:
    // - Last-move pulse state should survive reboot.
    // - It should be derived from persisted undo history (per-side), not RAM state.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/t_gp_last_move_reboot.json";
        (void)std::remove(cfg.path);
        (void)std::remove("build/t_gp_last_move_reboot.json.bak");
        (void)std::remove("build/t_gp_last_move_reboot.json.tmp");

        // Persist a snapshot that includes undo history.
        {
            modules::SettingsStore settings(cfg);
            results.Check(settings.Load(), "settings load (last-move reboot)");

            // Position after: 1. e4 e5
            settings.Doc()[modules::KEY_GAME_FEN] =
                "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2";

            uint8_t fields[modules::UNDO_HISTORY_FIELDS_PER_MOVE * 2] = {};

            // White: e2->e4
            fields[0] = modules::PersistedSideToU8(modules::PersistedSide::White);
            fields[1] = static_cast<uint8_t>('P');
            fields[2] = 0;
            fields[3] = 255;
            fields[4] = 0;
            fields[5] = 52;
            fields[6] = 36;

            // Black: e7->e5
            fields[7] = modules::PersistedSideToU8(modules::PersistedSide::Black);
            fields[8] = static_cast<uint8_t>('p');
            fields[9] = 0;
            fields[10] = 255;
            fields[11] = 0;
            fields[12] = 12;
            fields[13] = 28;

            results.Check(modules::WriteUndoHistory(settings.Doc(), fields, sizeof(fields)),
                          "write undo history (last-move reboot)");
            results.Check(settings.SaveNow(), "save settings (last-move reboot)");
        }

        // Simulate reboot: reload settings and initialize coordinator.
        modules::SettingsStore settings2(cfg);
        results.Check(settings2.Load(), "settings reload (last-move reboot)");

        FakeEngine engine(modules::INITIAL_FEN);
        runtime::GameStateMachine stateMachine;
        modules::GamePersistenceCoordinator persist(settings2, engine, stateMachine);
        results.Check(persist.LoadOrInitialize(), "coordinator load/init (last-move reboot)");

        uint8_t from = 255;
        uint8_t to = 255;

        results.Check(stateMachine.TryGetLastMoveForSide(modules::PersistedSide::White, from, to) &&
                          from == 52 && to == 36,
                      "last move restored for White");

        results.Check(stateMachine.TryGetLastMoveForSide(modules::PersistedSide::Black, from, to) &&
                          from == 12 && to == 28,
                      "last move restored for Black");

        // Starting a new game should clear last-move highlights.
        persist.ResetEngine();

        results.Check(!stateMachine.TryGetLastMoveForSide(modules::PersistedSide::White, from, to),
                      "last move cleared for White on new game");
        results.Check(!stateMachine.TryGetLastMoveForSide(modules::PersistedSide::Black, from, to),
                      "last move cleared for Black on new game");
    }

    return results.Finish();
}
