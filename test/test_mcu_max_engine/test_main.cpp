#include <array>
#include <cstdio>
#include <cstring>

#include <mcu-max.h>

#include "foxie_embedded/millis.hpp"
#include "modules/engine.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/vs_cpu_difficulty.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckFixture(const bool condition,
                  const char* fixture,
                  const char* expectation) {
    char message[160] = {};
    (void)std::snprintf(
        message, sizeof(message), "%s: %s", fixture, expectation);
    Check(condition, message);
}

bool ClassicResultsEqual(const modules::ClassicThinkResult& lhs,
                         const modules::ClassicThinkResult& rhs) {
    if (lhs.candidateCount > modules::CLASSIC_THINK_CANDIDATE_CAPACITY ||
        lhs.bestScore != rhs.bestScore ||
        lhs.candidateCount != rhs.candidateCount ||
        lhs.bestMoveFacts != rhs.bestMoveFacts ||
        lhs.bestScoreValid != rhs.bestScoreValid) {
        return false;
    }
    for (uint8_t index = 0U; index < lhs.candidateCount; ++index) {
        if (lhs.candidates[index].fromSquare !=
                rhs.candidates[index].fromSquare ||
            lhs.candidates[index].toSquare !=
                rhs.candidates[index].toSquare ||
            lhs.candidates[index].promotion !=
                rhs.candidates[index].promotion) {
            return false;
        }
    }
    return true;
}

void CheckOutcomeProbeRestoresSpecialState(
    const char* fixture,
    const char* fen,
    const uint8_t specialFrom,
    const uint8_t specialTo,
    const char* specialMove) {
    modules::LitchMaxEngine engine;
    engine.Reset();
    CheckFixture(engine.SetFen(fen), fixture, "position loads");

    modules::ThinkConfig config;
    config.maxNodes = 10000U;
    config.maxDepth = 5U;
    config.rngSeed = 1U;
    CheckFixture(engine.StartThink(config), fixture, "search starts");
    uint8_t progress = 0U;
    modules::UciMove best;
    CheckFixture(
        engine.PollThinkProgress(progress, best) ==
            modules::ThinkState::Done,
        fixture,
        "search and selected-move outcome probe complete");

    modules::ClassicThinkResult firstResult;
    modules::ThinkStats firstStats;
    CheckFixture(engine.GetLastClassicThinkResult(firstResult),
                 fixture,
                 "completed root bookkeeping remains available");
    CheckFixture(engine.GetLastThinkStats(firstStats) &&
                     firstStats.nodeCount != 0U,
                 fixture,
                 "completed search statistics remain available");

    char restoredFen[160] = {};
    uint64_t destinations = 0ULL;
    modules::ClassicThinkResult secondResult;
    modules::ThinkStats secondStats;
    CheckFixture(engine.GetFen(restoredFen, sizeof(restoredFen)) &&
                     std::strcmp(restoredFen, fen) == 0,
                 fixture,
                 "complete FEN survives selected-move probing");
    CheckFixture(engine.GetLegalDestinationsMask(
                     specialFrom, destinations) &&
                     (destinations & (UINT64_C(1) << specialTo)) != 0ULL,
                 fixture,
                 "special-move legality survives selected-move probing");
    CheckFixture(engine.GetLastClassicThinkResult(secondResult) &&
                     ClassicResultsEqual(firstResult, secondResult),
                 fixture,
                 "root candidates, score, and facts survive later reads");
    CheckFixture(engine.GetLastThinkStats(secondStats) &&
                     secondStats.nodeCount == firstStats.nodeCount,
                 fixture,
                 "node bookkeeping survives later reads");
    CheckFixture(engine.ApplyUciMove(specialMove),
                 fixture,
                 "the restored special move remains executable");
}


bool CandidateMatchesMove(const uint8_t from,
                          const uint8_t to,
                          const char promotion,
                          const modules::UciMove& move) {
    if (from >= 64U || to >= 64U ||
        move.Size() != (promotion == '\0' ? 4U : 5U)) {
        return false;
    }
    const char* uci = move.CStr();
    return uci[0] == 'a' + from % 8U && uci[1] == '8' - from / 8U &&
        uci[2] == 'a' + to % 8U && uci[3] == '8' - to / 8U &&
        (promotion == '\0' || uci[4] == promotion);
}

// Observe actual search progress and immutable completed candidates. The millis
// seam only requests cancellation or expires the deadline; no result is injected.
// This clock waits for work after a publication, so a completed-root boundary
// cannot masquerade as an interruption inside a later partial iteration.
struct ClassicStopClock {
    modules::LitchMaxEngine* engine = nullptr;
    std::array<uint8_t, 129> rootBoard{};
    std::array<uint64_t, 64> legal{};
    modules::ThinkCandidate completed{};
    uint32_t observedNodes = 0U;
    uint32_t completedAtNodes = 0U;
    uint32_t stoppedAtNodes = 0U;
    uint32_t publications = 0U;
    uint32_t publicationsAtStop = 0U;
    uint8_t minimumCompletedDepth = 0U;
    bool cancel = false;
    bool triggered = false;
    bool rootPreserved = true;
    bool progressMonotonic = true;
    bool publicationsValid = true;
};
ClassicStopClock* g_classicStopClock = nullptr;
constexpr uint32_t STOP_CLOCK_START_MS = 10000U;
constexpr uint32_t STOP_CLOCK_LIMIT_MS = 100U;

void ClassicInterruptionProgress(void* context, const uint32_t nodes) {
    auto& clock = *static_cast<ClassicStopClock*>(context);
    clock.progressMonotonic = clock.progressMonotonic && nodes >= clock.observedNodes;
    clock.observedNodes = nodes;
    clock.rootPreserved = clock.rootPreserved &&
        std::memcmp(mcumax.board, clock.rootBoard.data(), clock.rootBoard.size()) == 0;
}

void ClassicInterruptionCandidate(void* context,
                                  const modules::ThinkCandidate& candidate) {
    if (candidate.source == modules::ThinkCandidateSource::Seed) return;
    auto& clock = *static_cast<ClassicStopClock*>(context);
    clock.rootPreserved = clock.rootPreserved &&
        std::memcmp(mcumax.board, clock.rootBoard.data(), clock.rootBoard.size()) == 0;
    clock.publicationsValid = clock.publicationsValid && !candidate.isDuck &&
        candidate.duckDestination == 255U && candidate.fromSquare < 64U &&
        candidate.toSquare < 64U && candidate.completedDepth > clock.completed.completedDepth;
    if (candidate.fromSquare < 64U && candidate.toSquare < 64U) {
        clock.publicationsValid = clock.publicationsValid &&
            (clock.legal[candidate.fromSquare] &
             (UINT64_C(1) << candidate.toSquare)) != 0ULL;
    }
    clock.completed = candidate;
    clock.completedAtNodes = clock.observedNodes;
    ++clock.publications;
}

uint32_t ClassicInterruptionMillis() {
    auto* clock = g_classicStopClock;
    if (clock != nullptr && !clock->triggered && clock->minimumCompletedDepth != 0U &&
        clock->completed.completedDepth >= clock->minimumCompletedDepth &&
        clock->observedNodes > clock->completedAtNodes) {
        clock->triggered = true;
        clock->stoppedAtNodes = clock->observedNodes;
        clock->publicationsAtStop = clock->publications;
        if (clock->cancel) {
            clock->engine->CancelThink();
        }
    }
    return STOP_CLOCK_START_MS +
        ((clock != nullptr && clock->triggered && !clock->cancel) ? STOP_CLOCK_LIMIT_MS : 0U);
}

void CheckClassicInterruptionRestoresPosition(const char* fixture,
                                             const uint8_t minimumCompletedDepth,
                                             const bool cancel) {
    constexpr const char* FEN =
        "r3k2r/ppp2ppp/2n5/3pP3/8/2N5/PPP2PPP/R3K2R w KQkq d6 0 1";
    modules::LitchMaxEngine engine;
    engine.Reset();
    CheckFixture(engine.SetFen(FEN), fixture, "special-rights position loads");
    ClassicStopClock stop;
    stop.engine = &engine;
    stop.minimumCompletedDepth = minimumCompletedDepth;
    stop.cancel = cancel;
    for (uint8_t square = 0U; square < 64U; ++square) {
        CheckFixture(engine.GetLegalDestinationsMask(square, stop.legal[square]),
                     fixture, "capture legal destinations before search");
    }
    CheckFixture((stop.legal[60U] & ((UINT64_C(1) << 58U) | (UINT64_C(1) << 62U))) ==
                     ((UINT64_C(1) << 58U) | (UINT64_C(1) << 62U)) &&
                     (stop.legal[28U] & (UINT64_C(1) << 19U)) != 0ULL,
                 fixture, "both castles and en passant are genuinely available");
    std::memcpy(stop.rootBoard.data(), mcumax.board, stop.rootBoard.size());
    const fxe::ScopedMillisProviderOverride clock(&ClassicInterruptionMillis);
    g_classicStopClock = &stop;
    modules::ThinkConfig config;
    config.maxNodes = 100000U;
    config.maxDepth = 14U;
    config.maxThinkMs = STOP_CLOCK_LIMIT_MS;
    config.rngSeed = 1U;
    config.progressObserver = {&ClassicInterruptionProgress, &stop};
    config.candidateObserver = {&ClassicInterruptionCandidate, &stop};
    CheckFixture(engine.StartThink(config), fixture, "real Classic search starts");
    if (minimumCompletedDepth == 0U) {
        engine.CancelThink();
    }
    uint8_t progress = 0U;
    modules::UciMove best;
    const auto state = engine.PollThinkProgress(progress, best);
    g_classicStopClock = nullptr;
    modules::ClassicThinkResult result;
    const bool hasResult = engine.GetLastClassicThinkResult(result);
    modules::ThinkStats stats;
    CheckFixture(engine.GetLastThinkStats(stats), fixture, "actual work stats remain available");
    if (minimumCompletedDepth == 0U) {
        CheckFixture(!stop.triggered && stop.publications == 0U && stats.nodeCount == 0U,
                     fixture, "cancel before poll performs no search or publication");
    } else {
        CheckFixture(stop.triggered && stop.completed.completedDepth >= minimumCompletedDepth &&
                         stop.stoppedAtNodes > stop.completedAtNodes &&
                         stats.nodeCount >= stop.stoppedAtNodes && stats.nodeCount < config.maxNodes,
                     fixture, "real progress requests stop inside partial work after a completed iteration");
        CheckFixture(stop.publications != 0U && stop.publications == stop.publicationsAtStop &&
                         stop.publicationsValid && stop.progressMonotonic,
                     fixture, "interrupted partial work cannot replace the last immutable completed candidate");
    }
    CheckFixture(stop.rootPreserved &&
                     std::memcmp(mcumax.board, stop.rootBoard.data(), stop.rootBoard.size()) == 0,
                 fixture, "published progress and candidates leave the caller's root board unchanged");
    if (cancel) {
        CheckFixture(state == modules::ThinkState::Canceled && progress == 0U &&
                         best.Empty() && !hasResult,
                     fixture, "explicit cancel discards the final move and root result");
    } else {
        CheckFixture(state == modules::ThinkState::Done && progress == 100U &&
                         best.Size() >= 4U,
                     fixture, "deadline returns a playable result instead of cancellation");
        CheckFixture(hasResult && result.bestScoreValid && result.candidateCount > 0U &&
                         result.candidateCount <= modules::CLASSIC_THINK_CANDIDATE_CAPACITY &&
                         result.candidates[0].fromSquare == stop.completed.fromSquare &&
                         result.candidates[0].toSquare == stop.completed.toSquare &&
                         result.candidates[0].promotion == stop.completed.promotion &&
                         CandidateMatchesMove(stop.completed.fromSquare, stop.completed.toSquare,
                                              stop.completed.promotion, best),
                     fixture, "deadline retains the completed legal iteration's candidate");
    }
    char restoredFen[160] = {};
    CheckFixture(engine.GetFen(restoredFen, sizeof(restoredFen)) &&
                     std::strcmp(restoredFen, FEN) == 0,
                 fixture, "complete FEN and special rights survive interruption");
    for (uint8_t square = 0U; square < 64U; ++square) {
        uint64_t destinations = 0ULL;
        CheckFixture(engine.GetLegalDestinationsMask(square, destinations) &&
                         destinations == stop.legal[square],
                     fixture, "all legal destinations survive interruption");
    }
    const auto isOriginalLegalMove = [&stop](const modules::UciMove& move) {
        if (move.Size() < 4U) {
            return false;
        }
        const char* uci = move.CStr();
        if (uci[0] < 'a' || uci[0] > 'h' || uci[2] < 'a' || uci[2] > 'h' ||
            uci[1] < '1' || uci[1] > '8' || uci[3] < '1' || uci[3] > '8') {
            return false;
        }
        const uint8_t from = static_cast<uint8_t>((('8' - uci[1]) * 8) + uci[0] - 'a');
        const uint8_t to = static_cast<uint8_t>((('8' - uci[3]) * 8) + uci[2] - 'a');
        return (stop.legal[from] & (UINT64_C(1) << to)) != 0ULL;
    };
    if (!cancel) {
        CheckFixture(isOriginalLegalMove(best), fixture,
                     "deadline move is legal in unchanged position");
    }
    config.maxNodes = 9000U;
    config.maxDepth = 5U;
    config.maxThinkMs = 0U;
    config.progressObserver = {};
    config.candidateObserver = {};
    CheckFixture(engine.StartThink(config), fixture, "restart works without Reset or SetFen");
    modules::UciMove restarted;
    CheckFixture(engine.PollThinkProgress(progress, restarted) == modules::ThinkState::Done &&
                     isOriginalLegalMove(restarted),
                 fixture, "restarted real search returns a legal move");
    char appliedFen[160] = {};
    CheckFixture(engine.ApplyUciMove(restarted.CStr()) &&
                     engine.GetFen(appliedFen, sizeof(appliedFen)),
                 fixture, "restarted move applies without resetting interrupted engine");
    // Only after the actual restart/application, compare its result with a clean
    // application of the same move. A reset cannot hide failed search restoration.
    engine.Reset();
    char referenceFen[160] = {};
    CheckFixture(engine.SetFen(FEN) && engine.ApplyUciMove(restarted.CStr()) &&
                     engine.GetFen(referenceFen, sizeof(referenceFen)) &&
                     std::strcmp(appliedFen, referenceFen) == 0,
                 fixture, "post-move position matches clean application including special rights");
    std::printf("[CLASSIC_INTERRUPTION] case=%s stop=%s after_completed_depth=%u "
                "triggered=%u publication_node=%u stop_node=%u actual_nodes=%u "
                "completed_depth=%u publications=%u root_preserved=%u state=%u "
                "final_result=%u move=%s restart=%s\n",
                fixture, cancel ? "cancel" : "deadline", minimumCompletedDepth,
                stop.triggered, stop.completedAtNodes, stop.stoppedAtNodes, stats.nodeCount,
                stop.completed.completedDepth, stop.publications, stop.rootPreserved,
                static_cast<unsigned>(state), hasResult, best.CStr(), restarted.CStr());
}

} // namespace

int main() {
    {
        modules::LitchMaxEngine replayEngine;
        replayEngine.Reset();
        Check(replayEngine.SetFen("r1b1k2r/ppp1qp2/2np3p/4p1P1/P3P1n1/2PBBQ2/1PPN1PP1/R4RK1 w kq - 1 16"), "replay regression FEN loads");
        char before[160] = {}, after[160] = {};
        Check(replayEngine.GetFen(before, sizeof(before)), "replay pre-query FEN");
        for (int repeat = 0; repeat < 2; ++repeat) {
            uint64_t destinations = 0;
            Check(replayEngine.GetLegalDestinationsMask(30, destinations), "g5 destinations available");
            Check((destinations & (1ULL << 22)) != 0, "g5 includes g6 despite previous-best replay");
            Check((destinations & (1ULL << 23)) != 0, "g5 includes h6 capture despite previous-best replay");
            Check(replayEngine.GetFen(after, sizeof(after)) && std::strcmp(before, after) == 0, "destination enumeration preserves position");
        }
        Check(replayEngine.ApplyUciMove("g5h6"), "enumerated ordinary pawn capture applies");
    }

    std::puts("[MCU_MAX_ENGINE] start");

    modules::LitchMaxEngine eng;
    eng.Reset();

    Check(!eng.SetFen(nullptr), "SetFen nullptr returns false");
    Check(!eng.ApplyUciMove(nullptr), "ApplyUciMove nullptr returns false");
    Check(!eng.ApplyUciMove("e2"), "ApplyUciMove short string returns false");
    Check(!eng.ApplyUciMove("z9z9"), "ApplyUciMove invalid square returns false");

    char fen[160];
    std::memset(fen, 0, sizeof(fen));

    Check(eng.GetFen(fen, sizeof(fen)), "GetFen after Reset");
    Check(std::strlen(fen) > 0, "FEN non-empty");

    const char* startFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    Check(eng.SetFen(startFen), "SetFen start position");

    Check(eng.GetSideToMove() == modules::SideToMove::White, "side-to-move white at start");

    // Legal destination masks (0=a8 .. 63=h1).
    // e2 => e3/e4 at start position.
    {
        uint64_t dest = 0;
        Check(!eng.GetLegalDestinationsMask(64, dest), "GetLegalDestinationsMask invalid square returns false");

        dest = 0;
        Check(eng.GetLegalDestinationsMask(52, dest), "GetLegalDestinationsMask e2 returns true");
        Check((dest & (1ULL << 44)) != 0ULL, "e2 includes e3");
        Check((dest & (1ULL << 36)) != 0ULL, "e2 includes e4");
    }

    std::memset(fen, 0, sizeof(fen));
    Check(eng.GetFen(fen, sizeof(fen)), "GetFen after SetFen");
    Check(std::strstr(fen, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR") != nullptr,
          "FEN placement matches start position");
    Check(std::strstr(fen, " w ") != nullptr, "side-to-move white in FEN");

    const char* kingCapturedFen =
        "4Q3/8/8/8/8/8/8/4K3 b - - 0 8";
    Check(eng.SetFen(kingCapturedFen),
          "SetFen accepts Duck terminal position without captured king");
    std::memset(fen, 0, sizeof(fen));
    Check(eng.GetFen(fen, sizeof(fen)) &&
              std::strstr(fen, "4Q3/8/8/8/8/8/8/4K3") != nullptr,
          "Duck terminal placement survives engine round-trip");
    Check(eng.SetFen(startFen),
          "restore start position after Duck terminal fixture");

    const bool appliedE2E3 = eng.ApplyUciMove("e2e3");
    Check(appliedE2E3, "ApplyUciMove e2e3");

    Check(eng.GetSideToMove() == modules::SideToMove::Black, "side-to-move black after e2e3");

    // After e2e3, black to move: e7 => e6/e5.
    {
        uint64_t dest = 0;
        Check(eng.GetLegalDestinationsMask(12, dest), "GetLegalDestinationsMask e7 returns true");
        Check((dest & (1ULL << 20)) != 0ULL, "e7 includes e6");
        Check((dest & (1ULL << 28)) != 0ULL, "e7 includes e5");
    }

    std::memset(fen, 0, sizeof(fen));
    Check(eng.GetFen(fen, sizeof(fen)), "GetFen after e2e3");
    Check(std::strstr(fen, " b ") != nullptr, "side-to-move black after e2e3");

    // Determinism hook: repeatability for a fixed seed.
    modules::UciMove best1;
    modules::UciMove best2;
    {
        eng.Reset();
        Check(eng.SetFen(startFen), "SetFen start position (determinism)");

        modules::ThinkConfig cfg1;
        cfg1.difficulty = 3;
        cfg1.maxNodes = 2500;
        cfg1.rngSeed = 123;
        fxe::ManualMillis::SetNowMs(2000);
        Check(eng.StartThink(cfg1), "StartThink (determinism run 1)");

        uint8_t progress1 = 0;
        fxe::ManualMillis::SetNowMs(2001);
        const modules::ThinkState st1 = eng.PollThinkProgress(progress1, best1);
        Check(st1 == modules::ThinkState::Done, "PollThinkProgress done (determinism run 1)");
        Check(progress1 == 100, "progress==100 (determinism run 1)");
        Check(best1.Size() >= 4, "best move has >=4 chars (determinism run 1)");

        modules::LitchMaxEngine verify;
        verify.Reset();
        Check(verify.SetFen(startFen), "SetFen start position (verify move)");
        Check(verify.ApplyUciMove(best1.CStr()), "determinism best move is legal");
    }
    {
        eng.Reset();
        Check(eng.SetFen(startFen), "SetFen start position (determinism)");

        modules::ThinkConfig cfg2;
        cfg2.difficulty = 3;
        cfg2.maxNodes = 2500;
        cfg2.rngSeed = 123;
        fxe::ManualMillis::SetNowMs(3000);
        Check(eng.StartThink(cfg2), "StartThink (determinism run 2)");

        uint8_t progress2 = 0;
        fxe::ManualMillis::SetNowMs(3001);
        const modules::ThinkState st2 = eng.PollThinkProgress(progress2, best2);
        Check(st2 == modules::ThinkState::Done, "PollThinkProgress done (determinism run 2)");
        Check(progress2 == 100, "progress==100 (determinism run 2)");
        Check(best2.Size() >= 4, "best move has >=4 chars (determinism run 2)");

        modules::LitchMaxEngine verify;
        verify.Reset();
        Check(verify.SetFen(startFen), "SetFen start position (verify move)");
        Check(verify.ApplyUciMove(best2.CStr()), "determinism best move is legal");
    }
    Check(best1.View() == best2.View(), "best move stable for same seed");

    // Opening sanity: at a mid-high VS-CPU level, the engine should not pick an
    // obviously weak edge pawn move like a2a3 from the initial position.
    {
        const uint32_t level = 8;
        const uint32_t seed = 123;
        modules::ThinkConfig cfg = modules::MakeThinkConfigForVsCpuLevel(level, seed);

        eng.Reset();
        Check(eng.SetFen(startFen), "SetFen start position (opening sanity)");

        fxe::ManualMillis::SetNowMs(6000);
        Check(eng.StartThink(cfg), "StartThink (opening sanity)");

        uint8_t progressS = 0;
        modules::UciMove bestS;
        fxe::ManualMillis::SetNowMs(6001);
        const modules::ThinkState stS = eng.PollThinkProgress(progressS, bestS);
        Check(stS == modules::ThinkState::Done, "PollThinkProgress done (opening sanity)");
        Check(progressS == 100, "progress==100 (opening sanity)");
        Check(bestS.View() != "a2a3", "opening sanity: != a2a3");
    }

    // Coaching needs the bounded root result that produced the recommended
    // move. Keep this engine-owned so higher layers do not grow a second chess
    // evaluator just to recover a trustworthy completed-iteration score.
    {
        static_assert(sizeof(modules::ClassicThinkResult) <= 32U,
                      "classic analysis result must stay MCU-sized");

        eng.Reset();
        modules::ClassicThinkResult coachResult;
        Check(!eng.GetLastClassicThinkResult(coachResult),
              "classic root result is unavailable before a Classic search");

        const char* forcedMateFen =
            "r1b1kbr1/3pp2p/7n/3KPp2/pp1N1Pp1/5n2/P2P2PP/RQB3R1 b q - 3 20";
        modules::ThinkConfig coachCfg =
            modules::MakeThinkConfigForVsCpuLevel(10U, 1U);

        eng.Reset();
        Check(eng.SetFen(forcedMateFen), "SetFen forced-mate coaching fixture");
        Check(eng.StartThink(coachCfg), "StartThink forced-mate coaching fixture");

        uint8_t coachProgress = 0;
        modules::UciMove coachBest;
        const modules::ThinkState coachState =
            eng.PollThinkProgress(coachProgress, coachBest);
        Check(coachState == modules::ThinkState::Done,
              "PollThinkProgress done (forced-mate coaching fixture)");
        Check(coachBest.View() == "e7e6",
              "level-10 coaching search finds the forced-mate move");

        Check(eng.GetLastClassicThinkResult(coachResult),
              "classic search exposes its bounded root result");
        Check(coachResult.candidateCount > 0U &&
                  coachResult.candidateCount <= modules::CLASSIC_THINK_CANDIDATE_CAPACITY,
              "classic root result retains a nonempty bounded candidate list");
        if (coachResult.candidateCount > 0U) {
            const auto& first = coachResult.candidates[0];
            Check(CandidateMatchesMove(first.fromSquare, first.toSquare,
                                       first.promotion, coachBest),
                  "recommended final move is the first classic root candidate");
        }
        Check(coachResult.bestScoreValid,
              "completed Classic result exposes its selected-move score");
        const auto savedScore = coachResult.bestScore;
        Check(!eng.SetFen(""), "an empty import is rejected after analysis");
        Check(eng.GetLastClassicThinkResult(coachResult) &&
                  coachResult.bestScoreValid && coachResult.bestScore == savedScore &&
                  coachResult.candidateCount > 0U &&
                  CandidateMatchesMove(coachResult.candidates[0].fromSquare,
                                       coachResult.candidates[0].toSquare,
                                       coachResult.candidates[0].promotion, coachBest),
              "rejected import preserves the completed coaching reply and score");
        Check((coachResult.bestMoveFacts &
               modules::ClassicThinkOutcomeKnown) != 0U,
              "completed Classic result verifies its selected move outcome");

        Check(eng.ApplyUciMove(coachBest.CStr()),
              "ApplyUciMove accepts the analyzed coaching move");
        Check(!eng.GetLastClassicThinkResult(coachResult),
              "applying a move invalidates the prior classic root result");

        Check(eng.SetFen(startFen), "SetFen start for exhausted novice search");
        modules::ThinkConfig tinyCfg;
        tinyCfg.difficulty = 2U;
        tinyCfg.maxNodes = 1U;
        tinyCfg.maxDepth = 2U;
        Check(eng.StartThink(tinyCfg), "StartThink exhausted novice search");
        coachBest.Clear();
        Check(eng.PollThinkProgress(coachProgress, coachBest) ==
                  modules::ThinkState::Done && !coachBest.Empty(),
              "exhausted novice search retains its ranked legal seed");
        Check(eng.GetLastClassicThinkResult(coachResult) &&
                  coachResult.source == modules::ThinkCandidateSource::Seed,
              "exhausted search labels its seed without fabricating completed analysis");

        tinyCfg.difficulty = 3U;
        Check(eng.StartThink(tinyCfg), "StartThink scoreless strong CPU result");
        coachBest.Clear();
        Check(eng.PollThinkProgress(coachProgress, coachBest) ==
                  modules::ThinkState::Done && !coachBest.Empty(),
              "strong CPU retains its ranked legal seed when the work budget expires");

        modules::ThinkConfig quickCfg;
        quickCfg.maxNodes = 2500U;
        quickCfg.maxDepth = 3U;
        Check(eng.StartThink(quickCfg), "StartThink quick Classic result");
        Check(eng.PollThinkProgress(coachProgress, coachBest) ==
                  modules::ThinkState::Done,
              "quick Classic search completes");
        Check(eng.GetLastClassicThinkResult(coachResult),
              "quick Classic result is available");

        Check(eng.StartDuckThink(startFen, 27U, quickCfg),
              "StartDuckThink invalidates Classic result");
        Check(!eng.GetLastClassicThinkResult(coachResult),
              "Duck start clears the Classic result");
        eng.CancelThink();
        Check(eng.PollThinkProgress(coachProgress, coachBest) ==
                  modules::ThinkState::Canceled,
              "Duck search cancels after lifecycle check");

        Check(eng.SetFen(startFen), "SetFen start for cancellation lifecycle");
        Check(eng.StartThink(quickCfg), "StartThink cancellation lifecycle");
        Check(!eng.GetLastClassicThinkResult(coachResult),
              "new Classic search clears the prior result before work");
        eng.CancelThink();
        Check(eng.PollThinkProgress(coachProgress, coachBest) ==
                  modules::ThinkState::Canceled,
              "Classic search cancels after lifecycle check");
        Check(!eng.GetLastClassicThinkResult(coachResult),
              "canceled Classic search leaves no result");

        Check(eng.SetFen(startFen),
              "SetFen replaces the analyzed coaching position");
        Check(!eng.GetLastClassicThinkResult(coachResult),
              "new position has no stale classic root result");

        const char* mateInOneFen =
            "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1";
        Check(eng.SetFen(mateInOneFen),
              "SetFen immediate-mate outcome fixture");
        modules::ThinkConfig mateCfg;
        mateCfg.maxNodes = 10000U;
        mateCfg.maxDepth = 5U;
        mateCfg.rngSeed = 1U;
        Check(eng.StartThink(mateCfg),
              "StartThink immediate-mate outcome fixture");
        coachBest.Clear();
        Check(eng.PollThinkProgress(coachProgress, coachBest) ==
                  modules::ThinkState::Done,
              "immediate-mate outcome search completes");
        Check(eng.GetLastClassicThinkResult(coachResult),
              "immediate-mate outcome result is available");
        Check((coachResult.bestMoveFacts &
               modules::ClassicThinkOutcomeKnown) != 0U &&
                  (coachResult.bestMoveFacts &
                   modules::ClassicThinkGivesCheck) != 0U &&
                  (coachResult.bestMoveFacts &
                   modules::ClassicThinkGivesMate) != 0U,
              "selected mate-in-one is verified as checkmate");

        char mateFenAfter[160] = {};
        Check(eng.GetFen(mateFenAfter, sizeof(mateFenAfter)) &&
                  std::strcmp(mateFenAfter, mateInOneFen) == 0,
              "outcome probe leaves the complete engine FEN unchanged");
        Check(eng.GetLastClassicThinkResult(coachResult),
              "outcome probe preserves the completed Classic result");
    }

    // The probe snapshots the whole engine, not merely the FEN-facing board.
    // Re-exercise state whose legality depends on castling rights, en-passant
    // bookkeeping, and promotion handling after the probe has run.
    CheckOutcomeProbeRestoresSpecialState(
        "outcome-probe/castling",
        "4k3/8/8/8/8/8/8/4K2R w K - 0 1",
        60U,
        62U,
        "e1g1");
    CheckOutcomeProbeRestoresSpecialState(
        "outcome-probe/en-passant",
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
        28U,
        19U,
        "e5d6");
    CheckOutcomeProbeRestoresSpecialState(
        "outcome-probe/promotion",
        "4k3/P7/8/8/8/8/8/4K3 w - - 0 1",
        8U,
        0U,
        "a7a8q");

    CheckClassicInterruptionRestoresPosition("cancel-before-poll", 0U, true);
    CheckClassicInterruptionRestoresPosition("cancel-recursion", 1U, true);
    CheckClassicInterruptionRestoresPosition("deadline-recursion", 1U, false);
    CheckClassicInterruptionRestoresPosition("cancel-completed-iteration", 2U, true);
    CheckClassicInterruptionRestoresPosition("deadline-completed-iteration", 2U, false);

    modules::ThinkConfig cfg;
    cfg.difficulty = 0;
    fxe::ManualMillis::SetNowMs(1234);
    Check(eng.StartThink(cfg), "StartThink");
    Check(!eng.StartThink(cfg), "StartThink while thinking returns false");

    // Cancel before polling should deterministically cancel without engine work.
    eng.CancelThink();

    uint8_t progress = 0;
    modules::UciMove best;
    fxe::ManualMillis::SetNowMs(1235);
    const modules::ThinkState st = eng.PollThinkProgress(progress, best);

    Check(st == modules::ThinkState::Canceled && progress == 0U && best.Empty(),
          "cancel before polling deterministically returns Canceled with no move");

    // Once not thinking, polling should be idle.
    progress = 123;
    best.Clear();
    fxe::ManualMillis::SetNowMs(1236);
    const modules::ThinkState idle = eng.PollThinkProgress(progress, best);
    Check(idle == modules::ThinkState::Idle, "PollThinkProgress when not thinking returns Idle");

    // The generic API and CPU coordinator share one viable work policy.
    // Zero means standalone default here; the coordinator owns CPU-off state.
    for (const uint8_t level : {0U, 3U}) {
        Check(eng.SetFen("r7/p1pk2p1/3p3p/1N2p2P/2Knq3/5r2/PPPP3P/R1BQ3R w - - 0 17"),
              "default-limit forcing position loads");
        modules::ThinkConfig defaultConfig;
        defaultConfig.difficulty = level;
        Check(eng.StartThink(defaultConfig), "implicit-limit search starts");
        best.Clear();
        Check(eng.PollThinkProgress(progress, best) == modules::ThinkState::Done &&
                  !best.Empty(),
              "implicit limits complete analyzed work instead of using obsolete shallow defaults");
        modules::ClassicThinkResult defaultResult;
        Check(eng.GetLastClassicThinkResult(defaultResult) && defaultResult.bestScoreValid,
              "default work carries genuine completed analysis");
    }

    if (g_failures == 0) {
        std::puts("[MCU_MAX_ENGINE] ok");
        return 0;
    }

    std::printf("[MCU_MAX_ENGINE] failures=%d\n", g_failures);
    return 1;
}
