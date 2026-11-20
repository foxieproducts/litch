#include <cstdio>
#include <cstring>
#include <memory>

#include "foxie_embedded/millis.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/duck_game_persistence.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/game_persistence_coordinator_vs_cpu_internal.hpp"
#include "modules/initial_fen.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "modules/vs_cpu_difficulty.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"

namespace {

constexpr const char* WHITE_MATE_FEN = "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1";
constexpr const char* BLACK_MATE_FEN = "8/8/8/8/8/6k1/5q2/7K b - - 0 1";
int g_failures = 0;

bool GenerateHistoryIdentity(void*, modules::game_history::GameId& identity) {
    identity = {};
    identity.bytes[0] = 1U;
    return true;
}

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

void CheckFrozenStrengthWitnesses() {
    struct Case {
        const char* name;
        const char* fen;
        const char* best;
        const char* alternative = nullptr;
    };
    // Independently forced-reference graded moves at the unchanged CPU5/6
    // budgets. This is selected-move coverage, not a claim of measured Elo.
    constexpr Case CASES[] = {
        {"historical_king", "r1b1kb1r/ppN2ppp/2n5/4p1q1/2B5/8/PPPP1PPP/R1BQK2R b KQkq - 0 8", "e8d8"},
        {"poisoned_rook", "r1bk1b1r/ppN2ppp/2n5/4p1q1/2B5/8/PPPP1PPP/R1BQK2R w KQ - 1 9", "d2d3"},
        {"finite_mate_defense", "r1b2rk1/ppp2ppp/8/1NbBp3/2QnPn2/8/PPPP1PqP/1RB1K2R w K - 0 12", "b5d4", "c4f1"},
        {"later_queen_hint", "r2qk2r/pQ1b2p1/1p2p3/2p5/2P3n1/7N/PP1N2P1/R3KB1R b KQkq - 1 17", "d8h4"},
        {"opening_development", "rnbqkbnr/pp2pppp/2p5/8/3PN3/8/PPP2PPP/R1BQKBNR b KQkq - 0 4", "g8f6"},
        {"distant_queen_attack", "rnbq1rk1/1p3pp1/p3pn1p/3p2N1/N2P4/3B4/PPPQ1PPP/R3K2R w KQ - 0 11", "g5f3"},
    };
    modules::LitchMaxEngine engine;
    for (const auto& test : CASES) {
        for (const uint32_t level : {5U, 6U}) {
            engine.Reset();
            Check(engine.SetFen(test.fen), "frozen strength witness loads through production adapter");
            const auto config = modules::MakeThinkConfigForVsCpuLevel(level, 1U);
            Check(engine.StartThink(config), "unchanged real CPU profile starts");
            uint8_t progress = 0U;
            modules::UciMove best;
            const auto state = engine.PollThinkProgress(progress, best);
            Check(state == modules::ThinkState::Done &&
                      (std::strcmp(best.CStr(), test.best) == 0 ||
                       (test.alternative != nullptr && std::strcmp(best.CStr(), test.alternative) == 0)),
                  "CPU returns a reference-graded sound move, avoiding the witnessed weak choice");
            modules::ClassicThinkResult result;
            modules::ThinkStats stats;
            Check(engine.GetLastClassicThinkResult(result) && result.bestScoreValid &&
                      engine.GetLastThinkStats(stats) && stats.nodeCount <= config.maxNodes,
                  "strength result is analyzed and retains the actual hard CPU node cap");
            Check(engine.ApplyUciMove(best.CStr()), "graded CPU choice applies through production rules");
            std::printf("[CPU_FROZEN_WITNESS] case=%s level=%u move=%s nodes=%u\n",
                        test.name, level, best.CStr(), stats.nodeCount);
        }
    }
}

void CheckSmallBudgetFirstRoot() {
    constexpr const char* FEN = "4r1k1/r3q2p/p5p1/1pp5/4p1bN/4Q1P1/1P1N1PB1/R3R1K1 b - - 5 31";
    modules::LitchMaxEngine engine;
    Check(engine.SetFen(FEN), "small-budget escaped match position loads");
    auto config = modules::MakeThinkConfigForVsCpuLevel(4U, 1U);
    config.maxNodes = 11200U;
    config.maxDepth = 6U;
    Check(engine.StartThink(config), "small-budget real CPU search starts");
    uint8_t progress = 0U;
    modules::UciMove best;
    Check(engine.PollThinkProgress(progress, best) == modules::ThinkState::Done &&
              std::strcmp(best.CStr(), "e8d8") == 0,
          "CPU completes the first root instead of forfeiting with a missing move");
    modules::ClassicThinkResult result;
    modules::ThinkStats stats;
    Check(engine.GetLastClassicThinkResult(result) && result.bestScoreValid && result.preliminaryOnly &&
              engine.GetLastThinkStats(stats) && stats.nodeCount == 11200U,
          "small CPU budget returns an analyzed complete root, not an arbitrary legal fallback");
}

void CheckFirstRootResourceBounds() {
    constexpr const char* FEN = "4r1k1/r3q2p/p5p1/1pp5/4p1bN/4Q1P1/1P1N1PB1/R3R1K1 b - - 5 31";
    struct Case {
        uint32_t cap;
        uint16_t reserve;
        bool hint;
        bool cancel;
        uint32_t deadline;
        uint32_t expectedNodes;
        bool factoryDeadline = false;
    };
    constexpr Case CASES[] = {
        {11200U, 0U, false, false, 0U, 11200U},
        {1U, 1U, false, false, 0U, 1U},
        {64U, 1024U, false, false, 0U, 1024U},
        {11200U, 16384U, true, false, 0U, 11200U},
        {64U, 16384U, false, true, 0U, 128U},
        {64U, 16384U, false, false, 1U, 128U},
        {210000U, 16384U, false, false, 2000U, 128U, true},
    };
    modules::LitchMaxEngine engine;
    for (const auto& test : CASES) {
        fxe::ManualMillis::SetNowMs(1000U);
        Check(engine.SetFen(FEN), "bounded first-root stop fixture loads");
        char before[128]{};
        Check(engine.GetFen(before, sizeof(before)), "stop fixture captures authoritative exported position");
        auto config = modules::MakeThinkConfigForVsCpuLevel(4U, 1U);
        config.maxNodes = test.cap;
        config.firstRootNodeReserve = test.reserve;
        config.continuousAnalysis = test.hint;
        if (!test.factoryDeadline) config.maxThinkMs = test.deadline;
        struct Context {
            modules::LitchMaxEngine* engine;
            bool cancel;
            uint32_t deadline;
            uint32_t candidates = 0U;
        } context{&engine, test.cancel, test.deadline};
        config.progressObserver = {[](void* data, uint32_t) {
            auto& context = *static_cast<Context*>(data);
            if (context.cancel) context.engine->CancelThink();
            if (context.deadline != 0U) fxe::ManualMillis::AdvanceMs(context.deadline + 1U);
        }, &context};
        config.candidateObserver = {[](void* data, const modules::ThinkCandidate& candidate) {
            if (candidate.source == modules::ThinkCandidateSource::Seed) return;
            ++static_cast<Context*>(data)->candidates;
        }, &context};
        Check(engine.StartThink(config), "bounded first-root stop search starts");
        uint8_t progress = 0U;
        modules::UciMove best;
        const auto state = engine.PollThinkProgress(progress, best);
        modules::ThinkStats stats;
        modules::ClassicThinkResult result;
        char after[128]{};
        const bool resultValid = engine.GetLastClassicThinkResult(result);
        Check(test.cancel
                  ? state == modules::ThinkState::Canceled && best.Empty() && !resultValid
                  : state == modules::ThinkState::Done && !best.Empty() && resultValid,
              "resource stops retain legal decisions; deliberate cancellation retires the decision");
        Check(engine.GetLastThinkStats(stats) && stats.nodeCount == test.expectedNodes &&
                  engine.GetFen(after, sizeof(after)) && std::strcmp(after, before) == 0,
              "first-root allowance stays bounded and restores the authoritative position on stop");
        std::printf("[CPU_FIRST_ROOT_STOP] cap=%u reserve=%u hint=%u cancel=%u deadline=%u nodes=%u state=%u\n",
                    test.cap, test.reserve, test.hint, test.cancel, test.deadline,
                    stats.nodeCount, static_cast<unsigned>(state));
    }
}

void CheckCpuDeadlineRetainsCompletedRoot() {
    constexpr const char* FEN = "4r1k1/r3q2p/p5p1/1pp5/4p1bN/4Q1P1/1P1N1PB1/R3R1K1 b - - 5 31";
    struct Case { uint32_t level; uint32_t deadline; };
    constexpr Case CASES[] = {{3U, 900U}, {4U, 2000U}, {7U, 10000U}, {11U, 60000U}};
    // Advance the clock after a completed root, without overriding the factory
    // deadline. This catches a missing low/mid-level bound and clock wrap.
    for (const auto& test : CASES) {
        for (uint32_t turn = 0U; turn < 3U; ++turn) {
            fxe::ManualMillis::SetNowMs(UINT32_MAX - 500U);
            modules::LitchMaxEngine engine;
            Check(engine.SetFen(FEN), "CPU deadline fixture loads");
            char before[128]{};
            Check(engine.GetFen(before, sizeof(before)), "deadline captures authoritative exported position");
            // Exercise the level factory directly for the full profile,
            // including the witnessed unbounded level-4 calculation.
            auto config = turn == 2U ? modules::MakeThinkConfigForVsCpuLevel(test.level, 1U)
                                     : modules::MakeThinkConfigForVsCpuTurn(test.level, turn, 1U);
            struct Context {
                uint32_t deadline;
                uint32_t completedNodes = 0U;
                uint8_t depth = 0U;
                uint8_t publications = 0U;
            } context{test.deadline};
            config.candidateObserver = {[](void* data, const modules::ThinkCandidate& candidate) {
            if (candidate.source == modules::ThinkCandidateSource::Seed) return;
                auto& context = *static_cast<Context*>(data);
                if (candidate.rootKind == modules::ThinkRootKind::ClassicFull && candidate.completedDepth == 1U)
                    context.completedNodes = candidate.completedNodes;
                context.depth = candidate.completedDepth;
                if (++context.publications == 2U) fxe::ManualMillis::AdvanceMs(context.deadline);
            }, &context};
            Check(engine.StartThink(config), "factory CPU profile starts");
            uint8_t progress = 0U;
            modules::UciMove best;
            modules::ThinkStats stats;
            char after[128]{};
            Check(engine.PollThinkProgress(progress, best) == modules::ThinkState::Done &&
                      best.Equals("g6g5") && context.depth == 1U && context.publications == 2U,
                  "factory deadline retains the completed root through all tested ramp phases across wrap");
            modules::ClassicThinkResult result;
            Check(engine.GetLastClassicThinkResult(result) && result.bestScoreValid && !result.preliminaryOnly &&
                      context.completedNodes != 0U && engine.GetLastThinkStats(stats) &&
                      stats.nodeCount == context.completedNodes &&
                      engine.GetFen(after, sizeof(after)) && std::strcmp(after, before) == 0,
                  "factory deadline stops deeper work and restores the authoritative position");
            Check(engine.ApplyUciMove(best.CStr()), "deadline-retained analyzed move remains executable");
            std::printf("[CPU_FACTORY_DEADLINE] level=%u turn=%u profile=%u max_think_ms=%u nodes=%u depth=%u publications=%u\n",
                        test.level, turn + 1U, config.difficulty, config.maxThinkMs,
                        stats.nodeCount, context.depth, context.publications);
        }
    }
    fxe::ManualMillis::SetNowMs(1000U);
}

void CheckFirstRootCompletionBoundary() {
    constexpr const char* FEN = "4r1k1/r3q2p/p5p1/1pp5/4p1bN/4Q1P1/1P1N1PB1/R3R1K1 b - - 5 31";
    // Optimization may reduce work without changing either root. Establish
    // their boundaries independently, then interrupt immediately before/at
    // Full completion; a stale absolute count would stop testing that seam.
    struct Boundaries { uint32_t preliminary = 0U; uint32_t full = 0U; } boundaries;
    modules::LitchMaxEngine reference;
    Check(reference.SetFen(FEN), "boundary reference imports through the real adapter");
    auto referenceConfig = modules::MakeThinkConfigForVsCpuLevel(4U, 1U);
    referenceConfig.maxDepth = 2U;
    referenceConfig.maxThinkMs = 0U;
    referenceConfig.candidateObserver = {[](void* data, const modules::ThinkCandidate& candidate) {
            if (candidate.source == modules::ThinkCandidateSource::Seed) return;
        auto& result = *static_cast<Boundaries*>(data);
        if (candidate.rootKind == modules::ThinkRootKind::ClassicPreliminary) result.preliminary = candidate.completedNodes;
        if (candidate.rootKind == modules::ThinkRootKind::ClassicFull && candidate.completedDepth == 1U)
            result.full = candidate.completedNodes;
    }, &boundaries};
    uint8_t referenceProgress = 0U;
    modules::UciMove referenceMove;
    Check(reference.StartThink(referenceConfig) &&
              reference.PollThinkProgress(referenceProgress, referenceMove) == modules::ThinkState::Done &&
              boundaries.preliminary == 1497U && boundaries.full > boundaries.preliminary,
          "reference separately observes complete preliminary and first Full work");
    if (boundaries.preliminary != 1497U || boundaries.full <= boundaries.preliminary) return;
    struct Case { uint32_t cap; uint32_t nodes; const char* best; bool preliminary; uint8_t publications; };
    const Case CASES[] = {
        {1U, boundaries.preliminary, "e8d8", true, 1U},
        {boundaries.full - 1U, boundaries.full - 1U, "e8d8", true, 1U},
        {boundaries.full, boundaries.full, "g6g5", false, 2U},
    };
    modules::LitchMaxEngine engine;
    for (const auto& test : CASES) {
        Check(engine.SetFen(FEN), "completion boundary loads through real adapter");
        char before[128]{};
        Check(engine.GetFen(before, sizeof(before)), "completion boundary captures authoritative position");
        auto config = modules::MakeThinkConfigForVsCpuLevel(4U, 1U);
        config.maxNodes = test.cap;
        uint8_t publications = 0U;
        config.candidateObserver = {[](void* data, const modules::ThinkCandidate& candidate) {
            if (candidate.source == modules::ThinkCandidateSource::Seed) return;
            ++*static_cast<uint8_t*>(data);
        }, &publications};
        Check(engine.StartThink(config), "completion boundary starts");
        uint8_t progress = 0U;
        modules::UciMove best;
        modules::ClassicThinkResult result;
        modules::ThinkStats stats;
        char after[128]{};
        Check(engine.PollThinkProgress(progress, best) == modules::ThinkState::Done &&
                  best.Equals(test.best) && publications == test.publications,
              "reserve ends after first preliminary comparison and Full replacement requires its complete root");
        Check(engine.GetLastClassicThinkResult(result) && result.bestScoreValid &&
                  result.preliminaryOnly == test.preliminary &&
                  engine.GetLastThinkStats(stats) && stats.nodeCount == test.nodes &&
                  engine.GetFen(after, sizeof(after)) && std::strcmp(before, after) == 0,
              "below-preliminary and either-side Full boundaries retain exact work, provenance and restoration");
        Check(engine.ApplyUciMove(best.CStr()), "completion-boundary choice is executable");
        std::printf("[CPU_FIRST_COMPLETE_BOUNDARY] cap=%u nodes=%u move=%s preliminary=%u publications=%u\n",
                    test.cap, stats.nodeCount, best.CStr(), result.preliminaryOnly, publications);
    }
}

void CheckReplyAwareFirstRoot() {
    struct Case { const char* fen; const char* best; };
    constexpr Case CASES[] = {
        {"rn5r/1pp1kNb1/6pp/1B3b2/N1QPn3/4P3/PPK2P1P/R1B4q w - - 4 19", "c4c7"},
        {"r1bq1rk1/1pp4p/2n2p1p/2bNp3/2Pp4/Pp1P1NPP/3QPPB1/1R3RK1 b - - 1 15", "a8a3"},
    };
    modules::LitchMaxEngine engine;
    for (const auto& test : CASES) {
        Check(engine.SetFen(test.fen), "escaped L7 forfeit position loads through real adapter");
        char before[128]{};
        Check(engine.GetFen(before, sizeof(before)), "preliminary fixture captures authoritative position");
        auto config = modules::MakeThinkConfigForVsCpuLevel(7U, 1U);
        config.maxNodes = 19200U;
        struct Context { uint8_t depth = 0U; uint8_t publications = 0U; } context;
        config.candidateObserver = {[](void* data, const modules::ThinkCandidate& candidate) {
            if (candidate.source == modules::ThinkCandidateSource::Seed) return;
            auto& context = *static_cast<Context*>(data);
            context.depth = candidate.completedDepth;
            ++context.publications;
        }, &context};
        Check(engine.StartThink(config), "reply-aware CPU search starts at actual failed match budget");
        uint8_t progress = 0U;
        modules::UciMove best;
        modules::ClassicThinkResult result;
        modules::ThinkStats stats;
        char after[128]{};
        Check(engine.PollThinkProgress(progress, best) == modules::ThinkState::Done &&
                  best.Equals(test.best) && context.depth == 2U && context.publications == 1U,
              "CPU retains fully compared reply-aware choice instead of forfeiting or hanging its queen");
        Check(engine.GetLastClassicThinkResult(result) && result.bestScoreValid && result.preliminaryOnly &&
                  engine.GetLastThinkStats(stats) && stats.nodeCount == 19200U &&
                  engine.GetFen(after, sizeof(after)) && std::strcmp(before, after) == 0,
              "interrupted Full work retains explicit preliminary provenance, cap and restored state");
        Check(engine.ApplyUciMove(best.CStr()), "reply-aware candidate applies through production rules");
        std::printf("[CPU_REPLY_AWARE_ROOT] move=%s depth=%u preliminary=%u nodes=%u\n",
                    best.CStr(), context.depth, result.preliminaryOnly, stats.nodeCount);
    }
}

void CheckCpuDeadlineRetainsPreliminaryRoot() {
    struct Case { uint32_t level; uint32_t deadline; };
    constexpr Case CASES[] = {{3U, 900U}, {4U, 2000U}, {11U, 60000U}};
    for (const auto& test : CASES) {
        fxe::ManualMillis::SetNowMs(UINT32_MAX - 500U);
        modules::LitchMaxEngine engine;
        engine.Reset();
        char before[128]{};
        Check(engine.GetFen(before, sizeof(before)), "preliminary deadline captures authoritative position");
        auto config = modules::MakeThinkConfigForVsCpuLevel(test.level, 1U);
        struct Context {
            uint32_t deadline;
            uint8_t depth = 0U;
            uint8_t publications = 0U;
        } context{test.deadline};
        config.candidateObserver = {[](void* data, const modules::ThinkCandidate& candidate) {
            if (candidate.source == modules::ThinkCandidateSource::Seed) return;
            auto& context = *static_cast<Context*>(data);
            context.depth = candidate.completedDepth;
            if (++context.publications == 1U) fxe::ManualMillis::AdvanceMs(context.deadline);
        }, &context};
        Check(engine.StartThink(config), "factory deadline can interrupt after the preliminary root");
        uint8_t progress = 0U;
        modules::UciMove best;
        modules::ClassicThinkResult result;
        modules::ThinkStats stats;
        char after[128]{};
        Check(engine.PollThinkProgress(progress, best) == modules::ThinkState::Done &&
                  best.Equals("b1c3") && context.depth == 2U && context.publications == 1U &&
                  engine.GetLastClassicThinkResult(result) && result.preliminaryOnly && result.bestScoreValid,
              "factory deadline retains complete preliminary analysis with provenance across wrap");
        Check(engine.GetLastThinkStats(stats) && stats.nodeCount < config.firstRootNodeReserve &&
                  engine.GetFen(after, sizeof(after)) && std::strcmp(before, after) == 0,
              "factory deadline adds no Full work and restores authoritative position");
        Check(engine.ApplyUciMove(best.CStr()), "deadline-retained preliminary move is legal");
        std::printf("[CPU_FACTORY_PRELIMINARY_DEADLINE] level=%u max_think_ms=%u nodes=%u publications=%u\n",
                    test.level, config.maxThinkMs, stats.nodeCount, context.publications);
    }
    fxe::ManualMillis::SetNowMs(1000U);
}

void CheckRootResults() {
    for (const uint32_t level : {5U, 6U}) {
        constexpr uint32_t seed = 1U;
        for (const bool black : {false, true}) {
            modules::LitchMaxEngine engine;
            engine.Reset();
            Check(engine.SetFen(black ? BLACK_MATE_FEN : WHITE_MATE_FEN),
                  "mate position loads for either chess side");
            auto config = modules::MakeThinkConfigForVsCpuLevel(level, seed);
            Check(engine.StartThink(config), "real CPU search starts");
            uint8_t progress = 0U;
            modules::UciMove best;
            Check(engine.PollThinkProgress(progress, best) == modules::ThinkState::Done,
                  "CPU budget returns a completed result");
            modules::ClassicThinkResult result;
            modules::ThinkStats stats;
            Check(engine.GetLastClassicThinkResult(result) && result.bestScoreValid &&
                      (result.bestMoveFacts & modules::ClassicThinkGivesMate) != 0U,
                  "the searched mating result wins rather than selecting a bounded worse move");
            Check(engine.GetLastThinkStats(stats) &&
                      stats.nodeCount <= config.maxNodes + 256U,
                  "the configured node bound is retained");
            Check(engine.ApplyUciMove(best.CStr()), "selected CPU move is executable");
            const auto responder = black ? modules::SideToMove::White
                                         : modules::SideToMove::Black;
            Check(engine.IsInCheckmate(responder) && !engine.IsInStalemate(responder),
                  "mating-score choice actually checkmates rather than stalemates");
            std::printf("[CPU_ROOT_QUALITY] level=%u seed=%u side=%s move=%s nodes=%u\n",
                        level, seed, black ? "black" : "white",
                        best.CStr(), stats.nodeCount);
        }
    }
}

// Record the real CPU configuration/result without changing either. Preserve
// requested Hint policy, but bound background replay execution to 4096 nodes.
// Like BoundedHintEngine, bypass its calculation minimum and Classic preliminary
// pass: this frozen replay clock cannot advance inside synchronous Poll.
// The real calculation minimum has separate coverage.
class RecordedEngine final : public modules::IEngine {
  public:
    void Reset() override { m_engine.Reset(); }
    bool SetFen(const char* fen) override { return m_engine.SetFen(fen); }
    bool GetFen(char* fen, size_t size) const override { return m_engine.GetFen(fen, size); }
    bool ApplyUciMove(const char* move) override { return m_engine.ApplyUciMove(move); }
    modules::SideToMove GetSideToMove() const override { return m_engine.GetSideToMove(); }
    bool IsInCheck(modules::SideToMove side) const override { return m_engine.IsInCheck(side); }
    bool IsInCheckmate(modules::SideToMove side) const override { return m_engine.IsInCheckmate(side); }
    bool IsInStalemate(modules::SideToMove side) const override { return m_engine.IsInStalemate(side); }
    bool GetLegalDestinationsMask(uint8_t from, uint64_t& mask) override {
        return m_engine.GetLegalDestinationsMask(from, mask);
    }
    bool GetImmediateThinkCandidate(modules::ThinkCandidate& out) override {
        return m_engine.GetImmediateThinkCandidate(out);
    }
    bool StartThink(const modules::ThinkConfig& requested) override {
        auto config = requested;
        m_currentSearchIsCpu = !requested.continuousAnalysis;
        if (config.continuousAnalysis) {
            hintConfig = requested;
            ++hintStarts;
            config.maxNodes = 4096U;
            config.minimumCalculationMs = 0U;
        } else {
            cpuConfig = config;
            ++cpuStarts;
            RecordCpuCandidate(config);
        }
        const bool started = m_engine.StartThink(config);
        if (started && m_currentSearchIsCpu && rejectStartedCpuOnce) {
            rejectStartedCpuOnce = false;
            return false;
        }
        return started;
    }
    bool StartDuckThink(const char* fen, uint8_t duckSquare,
                        const modules::ThinkConfig& requested) override {
        m_currentSearchIsCpu = !requested.continuousAnalysis;
        auto config = requested;
        if (m_currentSearchIsCpu) {
            cpuConfig = config;
            ++cpuStarts;
            RecordCpuCandidate(config);
        } else {
            config.maxNodes = 4096U;
            config.minimumCalculationMs = 0U;
        }
        const bool started = m_engine.StartDuckThink(fen, duckSquare, config);
        if (started && m_currentSearchIsCpu && rejectStartedCpuOnce) {
            rejectStartedCpuOnce = false;
            return false;
        }
        return started;
    }
    bool GetLastDuckThinkResult(modules::DuckThinkResult& result) const override {
        return m_engine.GetLastDuckThinkResult(result);
    }
    void CancelThink() override { m_engine.CancelThink(); }
    modules::ThinkState PollThinkProgress(uint8_t& progress, modules::UciMove& best) override {
        const auto state = m_engine.PollThinkProgress(progress, best);
        if (m_currentSearchIsCpu && state == modules::ThinkState::Canceled) {
            ++canceledPolls;
        }
        if (m_currentSearchIsCpu && state == modules::ThinkState::Done) {
            cpuBest = best;
        }
        return state;
    }
    bool GetLastThinkStats(modules::ThinkStats& stats) const override {
        return m_engine.GetLastThinkStats(stats);
    }
    bool GetLastClassicThinkResult(modules::ClassicThinkResult& result) const override {
        return m_engine.GetLastClassicThinkResult(result);
    }

    modules::ThinkConfig cpuConfig;
    modules::ThinkConfig hintConfig;
    modules::UciMove cpuBest;
    uint32_t cpuStarts = 0U;
    uint32_t hintStarts = 0U;
    uint32_t canceledPolls = 0U;
    uint8_t cpuRootDepth = 0U;
    bool rejectStartedCpuOnce = false;

  private:
    void RecordCpuCandidate(modules::ThinkConfig& config) {
        cpuRootDepth = 0U;
        m_cpuObserver = config.candidateObserver;
        config.candidateObserver = {[](void* context, const modules::ThinkCandidate& candidate) {
            auto& self = *static_cast<RecordedEngine*>(context);
            self.cpuRootDepth = candidate.completedDepth;
            self.m_cpuObserver.Publish(candidate);
        }, this};
    }

    modules::LitchMaxEngine m_engine;
    modules::ThinkCandidateObserver m_cpuObserver;
    bool m_currentSearchIsCpu = false;
};

void CheckOpeningRampJourney(const bool cpuBlack) {
    constexpr const char* PATH = "build/test_cpu_opening_ramp.json";
    (void)std::remove(PATH);
    (void)std::remove("build/test_cpu_opening_ramp.json.bak");
    (void)std::remove("build/test_cpu_opening_ramp.json.tmp");
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "ramp settings load");
    settings.Doc()["VS_CPU_LEVEL"] = 11U;
    settings.Doc()["VS_CPU_PLAYER"] = cpuBlack ? 2U : 1U;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 0U;
    stateConfig.placementStableRequiredMs = 0U;
    stateConfig.cpuIdleDelayMs = 20U;
    runtime::GameStateMachine state(stateConfig);
    state.Reset();
    RecordedEngine engine;
    auto coordinator = std::make_unique<modules::GamePersistenceCoordinator>(
        settings, engine, state);
    coordinator->SetGameHistoryIdentityGenerator(&GenerateHistoryIdentity);
    Check(coordinator->LoadOrInitialize(), "ramp opening loads");
    auto dispatcher = std::make_unique<runtime::InteractionDispatcher>(state, *coordinator);
    const auto observe = [&](const uint64_t occupied) {
        (void)dispatcher->DispatchBoardObservation({
            fxe::Millis(), state.GetLastBoardObservationSequence() + 1U,
            state.GetPhysicalOccupancyMask() ^ occupied, occupied});
    };
    const auto tick = [&]() { state.Tick(); coordinator->Update(); };
    observe(state.GetExpectedOccupancyMask());
    const auto play = [&](const uint8_t from, const uint8_t to,
                          const bool capture, const uint8_t captureSquare) {
        const auto historyBefore = coordinator->GetUndoHistoryMoveCount();
        fxe::ManualMillis::AdvanceMs(750U);
        state.Tick();
        observe(state.GetPhysicalOccupancyMask() & ~(1ULL << from));
        coordinator->UpdateLegalDestinationsMask();
        if (capture) {
            observe(state.GetPhysicalOccupancyMask() & ~(1ULL << captureSquare));
        }
        observe(state.GetPhysicalOccupancyMask() | (1ULL << to));
        for (uint8_t i = 0U; i < 4U; ++i) {
            fxe::ManualMillis::AdvanceMs(1U);
            tick();
        }
        Check(coordinator->GetUndoHistoryMoveCount() == historyBefore + 1U,
              "physical ramp move commits exactly one history entry");
    };
    const auto playHuman = [&]() {
        coordinator->RequestCancelAllThinking();
        tick();
        // Advance a fresh pawn, avoiding captures and repeated-position draws.
        const uint8_t firstPawn = cpuBlack ? 48U : 8U;
        for (uint8_t from = firstPawn; from < firstPawn + 8U; ++from) {
            uint64_t destinations = 0U;
            if (!engine.GetLegalDestinationsMask(from, destinations)) continue;
            destinations &= ~state.GetExpectedOccupancyMask();
            for (uint8_t to = 0U; to < 64U; ++to) {
                if ((destinations & (1ULL << to)) == 0U) continue;
                play(from, to, false, 0U);
                return;
            }
        }
        Check(false, "human has a legal fresh-pawn move");
    };
    constexpr uint32_t EXPECTED_LEVELS[] = {3U, 7U, 11U, 11U, 11U};
    for (uint8_t turn = 0U; turn < 5U; ++turn) {
        if (cpuBlack || turn != 0U) playHuman();
        // Reconstruct from the persisted authority before CPU turns 2 and 3.
        // This exercises both an intermediate ramp and the full-level boundary.
        if (turn == 1U || turn == 2U) {
            coordinator->RequestCancelAllThinking();
            tick();
            Check(settings.SaveNow(), "ramp committed game saves before reboot");
            dispatcher.reset();
            coordinator.reset();
            state.Reset();
            engine.Reset();
            Check(settings.Load(), "ramp persisted settings reload");
            coordinator = std::make_unique<modules::GamePersistenceCoordinator>(
                settings, engine, state);
            coordinator->SetGameHistoryIdentityGenerator(&GenerateHistoryIdentity);
            Check(coordinator->LoadOrInitialize(), "ramp committed history restores");
            dispatcher = std::make_unique<runtime::InteractionDispatcher>(state, *coordinator);
            observe(state.GetExpectedOccupancyMask());
        }
        const uint32_t startsBefore = engine.cpuStarts;
        for (uint8_t i = 0U; i < 30U && !state.HasQueuedCpuMove(); ++i) {
            fxe::ManualMillis::AdvanceMs(100U);
            tick();
        }
        const auto expected = modules::MakeThinkConfigForVsCpuLevel(
            EXPECTED_LEVELS[turn], engine.cpuConfig.rngSeed);
        Check(engine.cpuStarts == startsBefore + 1U &&
                  engine.cpuConfig.difficulty == expected.difficulty &&
                  engine.cpuConfig.maxNodes == expected.maxNodes &&
                  engine.cpuConfig.maxDepth == expected.maxDepth &&
                  engine.cpuConfig.maxThinkMs == 60000U &&
                  engine.cpuConfig.firstRootNodeReserve == expected.firstRootNodeReserve &&
                  !engine.cpuConfig.continuousAnalysis,
              "turns 1-5 use 3/7/11/11/11 work profiles with the selected one-minute deadline across reboot");
        uint8_t from = 64U, to = 64U, captureSquare = 64U;
        bool capture = false;
        modules::UciMove delivered;
        const bool queued = state.TryGetQueuedCpuMove(from, to, capture, captureSquare);
        Check(queued && litch_max::TryFormatUciMove(from, to, '\0', delivered) &&
                  engine.cpuRootDepth >= 1U && engine.cpuBest.Equals(delivered.View()),
              "opening ramp queues the completed legal analysis without substitution");
        std::printf("[CPU_OPENING_RAMP] side=%s turn=%u level=%u nodes=%u max_think_ms=%u depth=%u move=%s history=%u\n",
                    cpuBlack ? "black" : "white", turn + 1U, engine.cpuConfig.difficulty,
                    engine.cpuConfig.maxNodes, engine.cpuConfig.maxThinkMs, engine.cpuRootDepth, delivered.CStr(),
                    coordinator->GetUndoHistoryMoveCount());
        if (!queued) break;
        play(from, to, capture, captureSquare);
    }
    Check(engine.hintStarts > 0U && engine.hintConfig.continuousAnalysis &&
              engine.hintConfig.difficulty == 11U && engine.hintConfig.maxThinkMs == 90000U,
          "recorded human Hint request retains maximum-strength policy independently of bounded replay execution");
    coordinator->RequestCancelAllThinking();
    tick();
}

void CheckCpuDeliveryJourney(const uint32_t level, const uint32_t seed) {
    constexpr const char* PATH = "build/test_cpu_search_quality.json";
    (void)std::remove(PATH);
    (void)std::remove("build/test_cpu_search_quality.json.bak");
    (void)std::remove("build/test_cpu_search_quality.json.tmp");
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "CPU journey settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = "7k/8/8/8/8/8/8/5QK1 w - - 0 1";
    settings.Doc()["VS_CPU_LEVEL"] = 0U;
    settings.Doc()["VS_CPU_PLAYER"] = 1U;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 0U;
    stateConfig.placementStableRequiredMs = 0U;
    stateConfig.cpuIdleDelayMs = 20U;
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    RecordedEngine engine;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "CPU journey initial position loads");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    const auto observe = [&](const uint64_t occupied) {
        (void)dispatcher.DispatchBoardObservation({
            fxe::Millis(), stateMachine.GetLastBoardObservationSequence() + 1U,
            stateMachine.GetPhysicalOccupancyMask() ^ occupied, occupied});
    };
    observe(stateMachine.GetExpectedOccupancyMask());
    const auto play = [&](const char* move) {
        litch_max::ParsedUciMove parsed;
        Check(litch_max::TryParseUciMove(move, parsed), "journey move parses");
        fxe::ManualMillis::AdvanceMs(750U);
        stateMachine.Tick();
        observe(stateMachine.GetPhysicalOccupancyMask() & ~(1ULL << parsed.fromSquare));
        coordinator.UpdateLegalDestinationsMask();
        observe(stateMachine.GetPhysicalOccupancyMask() | (1ULL << parsed.toSquare));
        for (uint8_t tick = 0U; tick < 4U; ++tick) {
            fxe::ManualMillis::AdvanceMs(1U);
            stateMachine.Tick();
            coordinator.Update();
        }
    };
    // Twelve distinct, legal physical moves reach the fixture without forging
    // undo history or using FEN counters to activate personality prematurely.
    constexpr const char* MOVES[] = {
        "g1g2", "h8g8", "g2g3", "g8h8", "g3g4", "h8g8",
        "g4g5", "g8h8", "g5g6", "h8g8", "f1f7", "g8h8",
    };
    for (const char* move : MOVES) play(move);
    char fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    Check(coordinator.GetUndoHistoryMoveCount() == 12U &&
              engine.GetFen(fen, sizeof(fen)) && std::strcmp(fen, WHITE_MATE_FEN) == 0,
          "physical journey reaches the real twelve-ply personality boundary");

    coordinator.RequestCancelAllThinking();
    coordinator.Update();
    coordinator.SetRngSeed(seed);
    settings.Doc()["VS_CPU_LEVEL"] = level;
    settings.NotifyDocChanged();
    for (uint8_t tick = 0U; tick < 20U && !stateMachine.HasQueuedCpuMove(); ++tick) {
        fxe::ManualMillis::AdvanceMs(100U);
        stateMachine.Tick();
        coordinator.Update();
    }
    uint8_t from = 64U;
    uint8_t to = 64U;
    uint8_t captureSquare = 64U;
    bool capture = false;
    const bool queued = stateMachine.TryGetQueuedCpuMove(from, to, capture, captureSquare);
    modules::UciMove delivered;
    Check(queued && litch_max::TryFormatUciMove(from, to, '\0', delivered),
          "production CPU choice becomes a physical move prompt");
    const auto expectedConfig = modules::MakeThinkConfigForVsCpuLevel(level, seed);
    Check(engine.cpuStarts == 1U && !engine.cpuConfig.continuousAnalysis &&
              engine.cpuConfig.maxNodes == expectedConfig.maxNodes &&
              engine.cpuConfig.maxDepth == expectedConfig.maxDepth &&
              engine.cpuBest.Equals(delivered.View()),
          "CPU uses actual profile and delivers its selected search move once");
    if (queued) play(delivered.CStr());
    Check(engine.IsInCheckmate(modules::SideToMove::Black) &&
              !engine.IsInStalemate(modules::SideToMove::Black) &&
              coordinator.GetUndoHistoryMoveCount() == 13U,
          "executing the physical CPU prompt wins instead of converting mate to stalemate");
    std::printf("[CPU_SEARCH_DELIVERY] level=%u seed=%u "
                "searched=%s delivered=%s history=%u checkmate=%u stalemate=%u\n",
                level, seed,
                engine.cpuBest.CStr(), delivered.CStr(), coordinator.GetUndoHistoryMoveCount(),
                engine.IsInCheckmate(modules::SideToMove::Black),
                engine.IsInStalemate(modules::SideToMove::Black));
}

void CheckOpeningSearchJourney(const uint32_t level, const uint32_t seed,
                               const char* position = nullptr,
                               const bool importOpening = false) {
    constexpr const char* PATH = "build/test_cpu_search_opening.json";
    (void)std::remove(PATH);
    (void)std::remove("build/test_cpu_search_opening.json.bak");
    (void)std::remove("build/test_cpu_search_opening.json.tmp");
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "opening settings load");
    settings.Doc()["VS_CPU_LEVEL"] = level;
    settings.Doc()["VS_CPU_PLAYER"] = position == nullptr ||
        std::strstr(position, " w ") != nullptr ? 1U : 2U;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
    if (position != nullptr) { settings.Doc()[modules::KEY_GAME_FEN] = position; }
    runtime::GameStateMachineConfig stateConfig;
    stateConfig.placementStableRequiredMs = 0U;
    stateConfig.cpuIdleDelayMs = 0U;
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    RecordedEngine engine;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);
    coordinator.SetGameHistoryIdentityGenerator(&GenerateHistoryIdentity);
    Check(coordinator.LoadOrInitialize(), "opening position loads");
    if (importOpening) {
        Check(coordinator.LoadExternalFen(modules::INITIAL_FEN) &&
                  coordinator.GetGameHistoryPersistenceState().captureIncomplete,
              "production import marks even the initial position as incomplete history");
        Check(settings.SaveNow() && settings.Load() && coordinator.LoadOrInitialize() &&
                  coordinator.GetGameHistoryPersistenceState().captureIncomplete,
              "import provenance survives persisted reload before CPU selection");
    }
    coordinator.SetRngSeed(seed);
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    (void)dispatcher.DispatchBoardObservation({fxe::Millis(), 1U,
        stateMachine.GetExpectedOccupancyMask(), stateMachine.GetExpectedOccupancyMask()});
    for (uint8_t tick = 0U; tick < 30U && !stateMachine.HasQueuedCpuMove(); ++tick) {
        fxe::ManualMillis::AdvanceMs(100U);
        stateMachine.Tick();
        coordinator.Update();
    }
    uint8_t from = 64U, to = 64U, captureSquare = 64U;
    bool capture = false;
    modules::UciMove delivered;
    Check(stateMachine.TryGetQueuedCpuMove(from, to, capture, captureSquare) &&
              litch_max::TryFormatUciMove(from, to, '\0', delivered) &&
              engine.cpuStarts == 1U && engine.cpuRootDepth >= 1U &&
              engine.cpuBest.Equals(delivered.View()),
          "CPU completes analyzed work and delivers that result without an unintended substitution or retry loop");
    for (uint8_t tick = 0U; tick < 4U; ++tick) {
        fxe::ManualMillis::AdvanceMs(100U);
        stateMachine.Tick();
        coordinator.Update();
    }
    Check(engine.cpuStarts == 1U, "one searched opening result queues exactly once");
    if (position != nullptr || importOpening) {
        const auto expected = modules::MakeThinkConfigForVsCpuLevel(level, seed);
        Check(engine.cpuConfig.difficulty == expected.difficulty &&
                  engine.cpuConfig.maxNodes == expected.maxNodes &&
                  engine.cpuConfig.maxDepth == expected.maxDepth,
              "imported positions without opening history retain the configured search profile");
    }
    std::printf("[CPU_POSITION_DELIVERY] context=%s level=%u nodes_budget=%u seed=%u starts=%u depth=%u searched=%s delivered=%s\n",
                importOpening ? "imported-opening" : position == nullptr ? "opening" : "quiescence-witness",
                level, engine.cpuConfig.maxNodes,
                seed, engine.cpuStarts, engine.cpuRootDepth, engine.cpuBest.CStr(), delivered.CStr());
}

void CheckPartiallyStartedSearchRecovery(const bool duck, const bool opening = false) {
    constexpr const char* PATH = "build/test_cpu_partial_start.json";
    (void)std::remove(PATH);
    (void)std::remove("build/test_cpu_partial_start.json.bak");
    (void)std::remove("build/test_cpu_partial_start.json.tmp");
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStoreConfig config;
    config.path = PATH;
    modules::SettingsStore settings(config);
    Check(settings.Load(), "partial-start settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = opening ? modules::INITIAL_FEN : WHITE_MATE_FEN;
    settings.Doc()["VS_CPU_LEVEL"] = 6U;
    settings.Doc()["VS_CPU_PLAYER"] = 1U;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
    if (duck) {
        modules::DuckGameDurableState durable;
        durable.variant = modules::GameVariant::Duck;
        durable.duckSquare = 32U;
        char encoded[32] = {};
        Check(modules::EncodeDuckGameDurableState(durable, encoded, sizeof(encoded)),
              "Duck recovery context encodes");
        settings.Doc()[modules::KEY_GAME_VARIANT_V1] = encoded;
    }
    runtime::GameStateMachineConfig stateConfig;
    stateConfig.placementStableRequiredMs = 0U;
    stateConfig.cpuIdleDelayMs = 0U;
    runtime::GameStateMachine state(stateConfig);
    state.Reset();
    RecordedEngine engine;
    // Controlled failure boundary: acquire the real engine search, then report
    // start failure. This models worker allocation failure after StartThink;
    // CancelThink alone cannot retire the engine's active state.
    engine.rejectStartedCpuOnce = true;
    modules::GamePersistenceCoordinator coordinator(settings, engine, state);
    coordinator.SetGameHistoryIdentityGenerator(&GenerateHistoryIdentity);
    Check(coordinator.LoadOrInitialize(), "partial-start position loads");
    runtime::InteractionDispatcher dispatcher(state, coordinator);
    (void)dispatcher.DispatchBoardObservation({fxe::Millis(), 1U,
        state.GetExpectedOccupancyMask(), state.GetExpectedOccupancyMask()});
    const auto tick = [&]() { state.Tick(); coordinator.Update(); };
    tick(); tick();
    tick();
    Check(engine.cpuStarts == 1U && engine.canceledPolls == 1U && state.HasQueuedCpuMove(),
          "failed start drains cancellation then queues its ranked legal decision");
    fxe::ManualMillis::AdvanceMs(1000U);
    tick();
    Check(engine.cpuStarts == 1U && state.HasQueuedCpuMove(),
          "retained decision queues exactly once without retrying a failed search");
    Check(engine.cpuConfig.difficulty == (opening ? 3U : 6U),
          "Duck uses the shared opening ramp and a failed search does not advance it");
    std::printf("[CPU_PARTIAL_START] duck=%u opening=%u starts=%u canceled_polls=%u searched=%s queued=%u\n",
                duck, opening, engine.cpuStarts, engine.canceledPolls, engine.cpuBest.CStr(), state.HasQueuedCpuMove());
}

void CheckNovicePolicy() {
    modules::LitchMaxEngine engine;
    modules::UciMove searched;
    for (uint32_t level = 1U; level <= 3U; ++level) {
        engine.Reset();
        auto config = modules::MakeThinkConfigForVsCpuLevel(level, 2U);
        modules::ThinkCandidate latest;
        config.candidateObserver = {[](void* context, const modules::ThinkCandidate& value) {
            *static_cast<modules::ThinkCandidate*>(context) = value;
        }, &latest};
        Check(engine.StartThink(config), "CPU selection acquires its real configured search");
        uint8_t progress = 0U;
        Check(engine.PollThinkProgress(progress, searched) == modules::ThinkState::Done,
              "CPU receives a completed configured root");
        modules::ClassicThinkResult result;
        modules::ThinkStats stats;
        Check(engine.GetLastClassicThinkResult(result) && engine.GetLastThinkStats(stats) &&
                  stats.nodeCount <= config.maxNodes,
              "novice horizon never buys extra reserve work");
        if (level <= 2U) Check(result.preliminaryOnly && latest.completedDepth == level,
                              "one/two static plies stop before Full or quiescence");
        litch_max::ParsedUciMove parsed;
        modules::UciMove selected;
        Check(modules::game_persistence_vs_cpu_internal::TryChooseClassicCpuMove(
                  engine, searched, parsed, selected) && selected.Equals(searched.View()),
              "every level retains its best completed move without deliberate mistakes");
        Check(modules::game_persistence_vs_cpu_internal::IsParsedMoveLegalInCurrentPosition(
                  engine, parsed.fromSquare, parsed.toSquare), "selected CPU move stays legal");
        std::printf("[CPU_POLICY_MATCHING] level=%u searched=%s selected=%s nodes=%u depth=%u static=%u\n",
                    level, searched.CStr(), selected.CStr(), stats.nodeCount, latest.completedDepth, result.preliminaryOnly);
    }
    litch_max::ParsedUciMove parsed;
    modules::UciMove aliased = searched;
    Check(modules::game_persistence_vs_cpu_internal::TryChooseClassicCpuMove(
              engine, aliased, parsed, aliased) && aliased.Equals(searched.View()),
          "selection retains the searched move when its output buffer aliases input");
    modules::UciMove missing, selected;
    Check(modules::game_persistence_vs_cpu_internal::TryChooseClassicCpuMove(
              engine, missing, parsed, selected) && !selected.Empty(),
          "missing completed search retains a ranked legal Seed");
}

} // namespace

int main() {
    const fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    CheckNovicePolicy();
    CheckOpeningRampJourney(false);
    CheckOpeningRampJourney(true);
    CheckPartiallyStartedSearchRecovery(false);
    CheckPartiallyStartedSearchRecovery(true);
    CheckPartiallyStartedSearchRecovery(true, true);
    CheckSmallBudgetFirstRoot();
    CheckFirstRootResourceBounds();
    CheckFirstRootCompletionBoundary();
    CheckReplyAwareFirstRoot();
    CheckCpuDeadlineRetainsCompletedRoot();
    CheckCpuDeadlineRetainsPreliminaryRoot();
    CheckRootResults();
    CheckFrozenStrengthWitnesses();
    CheckOpeningSearchJourney(3U, 1U,
        "r1bq1rk1/3pnpbp/p3p1p1/2p3N1/2PnP3/3P2P1/PBP1NP1P/1R1QK2R b K - 4 12");
    CheckOpeningSearchJourney(2U, 1U,
        "r7/p1pk2p1/3p3p/1N2p2P/2Knq3/5r2/PPPP3P/R1BQ3R w - - 0 17");
    CheckOpeningSearchJourney(3U, 5U);
    CheckOpeningSearchJourney(4U, 8U);
    CheckOpeningSearchJourney(5U, 3U);
    CheckOpeningSearchJourney(11U, 1U, nullptr, true);
    CheckCpuDeliveryJourney(5U, 4U);
    CheckCpuDeliveryJourney(6U, 3U);
    std::printf("[CPU_SEARCH_QUALITY] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
