#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

#include "modules/draw_rules.hpp"
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
        m_thinking = false;
        m_pollCount = 0;
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

    bool ApplyUciMove(const char*) override { return false; }

    modules::SideToMove GetSideToMove() const override { return modules::SideToMove::Black; }

    bool IsInCheck(modules::SideToMove) const override { return false; }
    bool IsInCheckmate(modules::SideToMove) const override { return false; }
    bool IsInStalemate(modules::SideToMove) const override { return false; }

    bool GetLegalDestinationsMask(uint8_t fromSquare, uint64_t& outDestMask64) override {
        outDestMask64 = 0;

        // Only allow one legal move: e8 -> e7.
        // Squares: e8=4, e7=12.
        if (fromSquare == 4U) {
            outDestMask64 = (1ULL << 12);
        }

        return true;
    }

    bool GetImmediateThinkCandidate(modules::ThinkCandidate& out) override {
        out = modules::ThinkCandidate{};
        out.fromSquare = 4U;
        out.toSquare = 12U;
        out.source = modules::ThinkCandidateSource::Seed;
        out.rootKind = modules::ThinkRootKind::Seed;
        return true;
    }

    bool StartThink(const modules::ThinkConfig&) override {
        ++starts;
        if (rejectStart) return false;
        if (m_thinking) {
            return false;
        }
        m_thinking = true;
        m_pollCount = 0;
        return true;
    }

    void CancelThink() override { m_thinking = false; }

    modules::ThinkState PollThinkProgress(uint8_t& outProgressPercent,
                                          modules::UciMove& outBestMoveUci) override {
        outProgressPercent = 0;
        outBestMoveUci.Clear();

        if (!m_thinking) {
            return modules::ThinkState::Idle;
        }

        ++m_pollCount;
        if (m_pollCount >= 2U) {
            if (returnError) {
                m_thinking = false;
                return modules::ThinkState::Error;
            }
            (void)outBestMoveUci.Assign(returnLegal ? "e8e7" : "a1a1");
            m_thinking = false;
            outProgressPercent = 100;
            return modules::ThinkState::Done;
        }

        outProgressPercent = 50;
        return modules::ThinkState::Thinking;
    }

    bool GetLastThinkStats(modules::ThinkStats& outStats) const override {
        outStats.nodeCount = 0;
        return true;
    }

    uint32_t starts = 0U;
    bool rejectStart = false;
    bool returnError = false;
    bool returnLegal = false;

  private:
    char m_fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    bool m_thinking = false;
    uint8_t m_pollCount = 0;
};

static constexpr const char* REPEAT_BASE_FEN =
    "4k2r/8/8/8/8/8/8/4K3 w - - 0 1";
static constexpr const char* REPEAT_AFTER_WHITE_KING_OUT_FEN =
    "4k2r/8/8/8/8/8/4K3/8 b - - 1 1";
static constexpr const char* REPEAT_SHORT_AFTER_BLACK_KING_OUT_FEN =
    "7r/4k3/8/8/8/8/4K3/8 w - - 2 2";
static constexpr const char* REPEAT_SHORT_AFTER_WHITE_KING_BACK_FEN =
    "7r/4k3/8/8/8/8/8/4K3 b - - 3 2";
static constexpr const char* REPEAT_AFTER_ROOK_OUT_FEN =
    "4k3/7r/8/8/8/8/4K3/8 w - - 2 2";
static constexpr const char* REPEAT_AFTER_WHITE_KING_BACK_FEN =
    "4k3/7r/8/8/8/8/8/4K3 b - - 3 2";
static constexpr const char* REPEAT_AFTER_BLACK_KING_OUT_FEN =
    "8/4k2r/8/8/8/8/8/4K3 w - - 4 3";
static constexpr const char* REPEAT_AFTER_SECOND_WHITE_KING_OUT_FEN =
    "8/4k2r/8/8/8/8/4K3/8 b - - 5 3";
static constexpr const char* REPEAT_AFTER_BLACK_KING_BACK_FEN =
    "4k3/7r/8/8/8/8/4K3/8 w - - 6 4";
static constexpr const char* REPEAT_CURRENT_FEN =
    "4k3/7r/8/8/8/8/8/4K3 b - - 7 4";
static constexpr const char* REPEAT_RESULT_FEN =
    "4k2r/8/8/8/8/8/8/4K3 w - - 8 5";
static constexpr const char* REPEAT_ALTERNATIVE_FEN =
    "8/4k2r/8/8/8/8/8/4K3 w - - 8 5";

class FakeRepeatingEngine final : public modules::IEngine {
  public:
    explicit FakeRepeatingEngine(const char* fen) {
        if (fen != nullptr) {
            std::strncpy(m_fen, fen, sizeof(m_fen));
            m_fen[sizeof(m_fen) - 1] = '\0';
        }
    }

    void Reset() override {
        m_fen[0] = '\0';
        m_thinking = false;
        m_pollCount = 0;
        m_thinkStartCount = 0;
        m_legalMaskCallCount = 0;
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

    bool ApplyUciMove(const char* uci) override {
        if (uci == nullptr) {
            return false;
        }

        struct Transition {
            const char* before;
            const char* uci;
            const char* after;
        };
        static constexpr Transition TRANSITIONS[] = {
            {REPEAT_BASE_FEN, "e1e2", REPEAT_AFTER_WHITE_KING_OUT_FEN},
            {REPEAT_AFTER_WHITE_KING_OUT_FEN, "e8e7", REPEAT_SHORT_AFTER_BLACK_KING_OUT_FEN},
            {REPEAT_SHORT_AFTER_BLACK_KING_OUT_FEN, "e2e1", REPEAT_SHORT_AFTER_WHITE_KING_BACK_FEN},
            {REPEAT_SHORT_AFTER_WHITE_KING_BACK_FEN, "e7e8", REPEAT_BASE_FEN},
            {REPEAT_AFTER_WHITE_KING_OUT_FEN, "h8h7", REPEAT_AFTER_ROOK_OUT_FEN},
            {REPEAT_AFTER_ROOK_OUT_FEN, "e2e1", REPEAT_AFTER_WHITE_KING_BACK_FEN},
            {REPEAT_AFTER_WHITE_KING_BACK_FEN, "e8e7", REPEAT_AFTER_BLACK_KING_OUT_FEN},
            {REPEAT_AFTER_BLACK_KING_OUT_FEN, "e1e2", REPEAT_AFTER_SECOND_WHITE_KING_OUT_FEN},
            {REPEAT_AFTER_SECOND_WHITE_KING_OUT_FEN, "e7e8", REPEAT_AFTER_BLACK_KING_BACK_FEN},
            {REPEAT_AFTER_BLACK_KING_BACK_FEN, "e2e1", REPEAT_CURRENT_FEN},
            {REPEAT_CURRENT_FEN, "h7h8", REPEAT_RESULT_FEN},
            {REPEAT_CURRENT_FEN, "e8e7", REPEAT_ALTERNATIVE_FEN},
        };

        for (const Transition& transition : TRANSITIONS) {
            if (std::strcmp(m_fen, transition.before) == 0 &&
                std::strcmp(uci, transition.uci) == 0) {
                return SetFen(transition.after);
            }
        }
        return false;
    }

    modules::SideToMove GetSideToMove() const override {
        return std::strstr(m_fen, " w ") != nullptr
                   ? modules::SideToMove::White
                   : modules::SideToMove::Black;
    }

    bool IsInCheck(modules::SideToMove) const override { return false; }
    bool IsInCheckmate(modules::SideToMove) const override { return false; }
    bool IsInStalemate(modules::SideToMove) const override { return false; }

    bool GetLegalDestinationsMask(uint8_t fromSquare, uint64_t& outDestMask64) override {
        ++m_legalMaskCallCount;
        outDestMask64 = 0;

        // The completed engine result and one policy-era alternative are legal.
        if (fromSquare == 15U) {
            outDestMask64 = (1ULL << 7);
        }
        if (fromSquare == 4U) {
            outDestMask64 = (1ULL << 12);
        }

        return true;
    }

    bool StartThink(const modules::ThinkConfig&) override {
        if (m_thinking) {
            return false;
        }
        m_thinking = true;
        m_pollCount = 0;
        m_legalMaskCallCount = 0;
        ++m_thinkStartCount;
        return true;
    }

    void CancelThink() override { m_thinking = false; }

    modules::ThinkState PollThinkProgress(uint8_t& outProgressPercent,
                                          modules::UciMove& outBestMoveUci) override {
        outProgressPercent = 0;
        outBestMoveUci.Clear();

        if (!m_thinking) {
            return modules::ThinkState::Idle;
        }

        ++m_pollCount;
        if (m_pollCount >= 2U) {
            // This legal result returns to REPEAT_BASE_FEN's position key.
            (void)outBestMoveUci.Assign("h7h8");
            m_thinking = false;
            outProgressPercent = 100;
            return modules::ThinkState::Done;
        }

        outProgressPercent = 50;
        return modules::ThinkState::Thinking;
    }

    bool GetLastThinkStats(modules::ThinkStats& outStats) const override {
        outStats.nodeCount = 0;
        return true;
    }

    uint8_t PollCount() const { return m_pollCount; }
    uint8_t ThinkStartCount() const { return m_thinkStartCount; }
    uint16_t LegalMaskCallCount() const { return m_legalMaskCallCount; }

  private:
    char m_fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    bool m_thinking = false;
    uint8_t m_pollCount = 0;
    uint8_t m_thinkStartCount = 0;
    uint16_t m_legalMaskCallCount = 0;
};

void AddUndoMove(JsonArray& undo,
                 const uint8_t side,
                 const char movedPiece,
                 const uint8_t from,
                 const uint8_t to) {
    undo.add(side);
    undo.add(static_cast<uint8_t>(movedPiece));
    undo.add(0U);
    undo.add(255U);
    undo.add(0U);
    undo.add(from);
    undo.add(to);
}

} // namespace

int main() {
    fxe::test_support::TestResults results("VS_CPU_ILLEGAL_ENGINE_MOVE_FALLBACK");
    results.Start();

    struct FailureCase { uint32_t level; uint8_t failure; };
    // Novices reject invalid results too; strong search also covers acquisition
    // and explicit engine failure. Avoid duplicating every level/failure pair.
    constexpr FailureCase CASES[] = {{1U, 0U}, {2U, 0U}, {3U, 0U}, {6U, 1U}, {6U, 2U}};
    for (const auto& item : CASES) {
        const uint32_t level = item.level;
        const uint8_t failure = item.failure;
        fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
        // Every recovery path crosses the uint32 clock boundary.
        fxe::ManualMillis::SetNowMs(UINT32_MAX - 500U);
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_vs_cpu_illegal_engine_move_fallback.json";
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_vs_cpu_illegal_engine_move_fallback.json.bak");
        (void)std::remove("build/test_vs_cpu_illegal_engine_move_fallback.json.tmp");
        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "settings load");
        const char* fen = "4k3/8/8/8/8/8/4P3/4K3 b - - 0 1";
        settings.Doc()[modules::KEY_GAME_FEN] = fen;
        settings.Doc()["VS_CPU_LEVEL"] = level;
        settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
        FakeEngine engine(fen);
        engine.rejectStart = failure == 1U;
        engine.returnError = failure == 2U;
        runtime::GameStateMachineConfig smCfg;
        smCfg.placementStableRequiredMs = 0U;
        smCfg.cpuIdleDelayMs = 0U;
        runtime::GameStateMachine sm(smCfg);
        sm.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        results.Check(coord.LoadOrInitialize(), "LoadOrInitialize");
        sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());
        const auto tick = [&]() { sm.Tick(); coord.Update(); };
        tick(); tick(); tick();
        results.Check(engine.starts == 1U && sm.HasQueuedCpuMove(),
                      "search failure retains and queues the current legal seed");
        fxe::ManualMillis::AdvanceMs(1000U);
        tick(); tick();
        results.Check(engine.starts == 1U && sm.HasQueuedCpuMove(),
                      "retained seed prevents repeat search across clock wrap");

        // A new external position retires failure delay immediately.
        results.Check(coord.LoadExternalFen("4k3/8/8/8/8/8/3PP3/4K3 b - - 0 1"),
                      "new position accepted during failure cooldown");
        sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());
        tick(); tick(); tick(); tick();
        results.Check(engine.starts == 2U && sm.HasQueuedCpuMove(),
                      "new position starts fresh without inherited failure delay");
        engine.rejectStart = false;
        engine.returnError = false;
        engine.returnLegal = true;
        coord.RequestCancelCpuWork();
        coord.Update();
        tick(); tick(); tick();
        results.Check(engine.starts == 3U && sm.HasQueuedCpuMove(),
                      "explicit cancellation resets delay and recovered result queues");
        uint8_t from = 0U, to = 0U, capSq = 0U;
        bool capValid = false;
        results.Check(sm.TryGetQueuedCpuMove(from, to, capValid, capSq) &&
                          from == 4U && to == 12U,
                      "recovery delivers the searched move");
        for (uint8_t i = 0U; i < 10U; ++i) {
            fxe::ManualMillis::AdvanceMs(200U);
            tick();
        }
        results.Check(engine.starts == 3U, "recovered result queues exactly once");
        std::printf("[CPU_FAILURE_RECOVERY] level=%u failure=%u starts=%u queued=%u\n",
                    level, failure, engine.starts, sm.HasQueuedCpuMove());
    }

    {
        fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(2000);

        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_vs_cpu_legal_repeating_engine_move.json";
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_vs_cpu_legal_repeating_engine_move.json.bak");
        (void)std::remove("build/test_vs_cpu_legal_repeating_engine_move.json.tmp");

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "settings load repeating result");

        // Eleven historical plies leave Black to move with the rook on h7.
        // The position produced by h7->h8 has already occurred twice, so
        // physically completing the engine result must invoke the ordinary
        // official threefold-draw path.
        // The engine's h7->h8 result returns to REPEAT_BASE_FEN's key4
        // position without reversing Black's most recent king move.
        settings.Doc()[modules::KEY_GAME_FEN] = REPEAT_CURRENT_FEN;
        settings.Doc()[modules::KEY_GAME_UNDO_BASE_FEN] = REPEAT_BASE_FEN;
        settings.Doc()["VS_CPU_LEVEL"] = 5U;
        settings.Doc()["GAME_FIRST_PLAYER"] = 1U;

        const char* repeatHistory[] = {REPEAT_BASE_FEN, REPEAT_RESULT_FEN};
        results.Check(
            modules::CountThreefoldRepetition(
                REPEAT_RESULT_FEN, repeatHistory, 2U) == 2U,
            "fixture engine result already has two historical occurrences");

        JsonArray undo = settings.Doc()[modules::KEY_GAME_UNDO_HISTORY].to<JsonArray>();
        AddUndoMove(undo, 0U, 'K', 60U, 52U); // e1->e2
        AddUndoMove(undo, 1U, 'k', 4U, 12U);  // e8->e7
        AddUndoMove(undo, 0U, 'K', 52U, 60U); // e2->e1
        AddUndoMove(undo, 1U, 'k', 12U, 4U);  // e7->e8 (second base occurrence)
        AddUndoMove(undo, 0U, 'K', 60U, 52U); // e1->e2
        AddUndoMove(undo, 1U, 'r', 7U, 15U);  // h8->h7
        AddUndoMove(undo, 0U, 'K', 52U, 60U); // e2->e1
        AddUndoMove(undo, 1U, 'k', 4U, 12U);  // e8->e7
        AddUndoMove(undo, 0U, 'K', 60U, 52U); // e1->e2
        AddUndoMove(undo, 1U, 'k', 12U, 4U);  // e7->e8
        AddUndoMove(undo, 0U, 'K', 52U, 60U); // e2->e1

        FakeRepeatingEngine engine(REPEAT_CURRENT_FEN);

        runtime::ManualClock clock(2000);

        runtime::GameStateMachineConfig smCfg;
        smCfg.placementStableRequiredMs = 0;
        smCfg.cpuIdleDelayMs = 0;
        smCfg.settleDurationMs = 0;
        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        results.Check(coord.LoadOrInitialize(), "LoadOrInitialize repeating result");
        results.Check(
            coord.GetEngineDerivedState().threefoldRepetitionCount == 2U,
            "official draw tracker records the current position twice");
        results.Check(!coord.GetEngineDerivedState().inDraw &&
                          coord.GetEngineDerivedState().drawReason ==
                              modules::DrawReason::None,
                      "official draw tracker keeps the twofold game active");

#ifndef ARDUINO
        sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());
#endif

        // Trigger think start.
        clock.SetNowMs(2001);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        sm.Tick();
        coord.Update();
        const uint32_t revisionBeforeResult = settings.GetRevision();

        // Let think complete. The legal engine result is queued as-is even
        // though applying it would repeat a recent key4 position.
        coord.Update();
        coord.Update();

        results.Check(sm.HasQueuedCpuMove(), "legal repeating cpu result queued");

        uint8_t from = 0;
        uint8_t to = 0;
        bool capValid = false;
        uint8_t capSq = 0;
        results.Check(sm.TryGetQueuedCpuMove(from, to, capValid, capSq),
                      "queued repeating result readable");
        results.Check(from == 15U, "queued repeating result from==h7");
        results.Check(to == 7U, "queued repeating result to==h8");
        results.Check(settings.GetRevision() == revisionBeforeResult + 1U,
                      "repeating result is queued and persisted exactly once");
        const uint32_t revisionAfterQueue = settings.GetRevision();
        results.Check(engine.ThinkStartCount() == 1U,
                      "repeating result starts one CPU search");
        results.Check(engine.PollCount() == 2U,
                      "repeating result completes one CPU search");
        results.Check(engine.LegalMaskCallCount() == 1U,
                      "repeating result does not launch an alternate search");

        coord.Update();
        coord.Update();

        results.Check(settings.GetRevision() == revisionAfterQueue,
                      "later updates do not queue or persist the result again");
        results.Check(engine.ThinkStartCount() == 1U,
                      "later updates do not restart CPU search");
        results.Check(engine.LegalMaskCallCount() == 1U,
                      "later updates do not search for another move");

        const uint64_t physicalBefore = sm.GetExpectedOccupancyMask();
        const uint64_t physicalAfter =
            (physicalBefore & ~(UINT64_C(1) << from)) |
            (UINT64_C(1) << to);
        sm.HandleBoardObservation(
            (UINT64_C(1) << from) | (UINT64_C(1) << to),
            physicalAfter,
            clock.NowMs(),
            2U);
        sm.Tick();
        coord.Update();
        coord.Update();

        const modules::EngineDerivedState& draw =
            coord.GetEngineDerivedState();
        results.Check(!sm.HasQueuedCpuMove(),
                      "physical repeating result completes the queued move");
        results.Check(draw.threefoldRepetitionCount == 3U && draw.inDraw &&
                          draw.drawReason ==
                              modules::DrawReason::ThreefoldRepetition,
                      "third occurrence ends through the official draw policy");
        results.Check(engine.ThinkStartCount() == 1U,
                      "threefold terminal does not start another CPU search");
    }

    return results.Finish();
}
