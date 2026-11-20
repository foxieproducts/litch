#include <chrono>
#include <cstdio>
#include <cstring>

#include <litch_max/chess_notation.hpp>
#include "foxie_embedded/millis.hpp"
#include "modules/chess_strings.hpp"
#include <litch_max/duck_chess_search.hpp>
#include <litch_max/duck_chess_analysis.hpp>
#include "modules/initial_fen.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/vs_cpu_difficulty.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

uint8_t Square(const char* algebraic) {
    uint8_t square = litch_max::DUCK_CHESS_INVALID_SQUARE;
    Check(litch_max::TryParseUciSquare(algebraic, square),
          "test square parses");
    return square;
}

bool CanCapturePiece(const char* fen, uint8_t duck, char target) {
    for (uint8_t to = 0U; to < 64U; ++to) {
        char piece = '\0';
        if (!modules::TryGetFenPieceAtSquare(fen, to, piece) || piece != target) {
            continue;
        }
        for (uint8_t from = 0U; from < 64U; ++from) {
            uint64_t destinations = 0ULL;
            Check(litch_max::GetDuckLegalDestinationsMask(fen, duck, from, destinations),
                  "tactical oracle obtains legal destinations");
            if ((destinations & (1ULL << to)) != 0ULL) {
                return true;
            }
        }
    }
    return false;
}

bool ApplyCompound(const char* fen, uint8_t duck,
                   const litch_max::DuckChessTurn& turn,
                   litch_max::DuckChessMoveResult& applied) {
    char occupant = '\0';
    return litch_max::TryApplyDuckMove(fen, duck, turn.chessMove.CStr(), applied) &&
           !applied.capturedKing && turn.duckDestination < 64U &&
           turn.duckDestination != duck &&
           !modules::TryGetFenPieceAtSquare(applied.resultingFen.CStr(),
                                            turn.duckDestination, occupant);
}

void TestSearchChoosesImmediateKingCapture() {
    static constexpr char FEN[] =
        "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1";

    litch_max::DuckChessSearchConfig config;
    config.maxNodes = 4096U;
    config.rngSeed = 7U;

    litch_max::DuckChessTurn best;
    litch_max::DuckChessSearchStats stats;
    Check(litch_max::FindBestDuckChessTurn(
              FEN, Square("a4"), config, best, stats),
          "Duck search returns a complete turn");
    Check(std::strcmp(best.chessMove.CStr(), "e2e8") == 0,
          "Duck search chooses an available king capture");
    Check(best.capturedKing,
          "king capture is marked terminal");
    Check(best.duckDestination == litch_max::DUCK_CHESS_INVALID_SQUARE,
          "terminal king capture skips duck relocation");
    Check(stats.nodesVisited != 0U,
          "Duck search reports bounded work");
}

void TestCapturedLivePositionChoosesKingCaptureAtEveryBudget() {
    static constexpr char FEN[] =
        "r1b5/pppk2p1/2n1q3/3p3P/1b1P3P/P4r2/1PP5/RN2K2R b - - 0 1";
    static constexpr uint32_t NODE_BUDGETS[] = {
        1U, 200U, 400U, 800U, 1600U, 2500U,
        4000U, 6000U, 9000U, 13000U, 18000U, 32000U,
    };

    uint64_t bishopDestinations = 0ULL;
    Check(litch_max::GetDuckLegalDestinationsMask(
              FEN, Square("f7"), Square("b4"), bishopDestinations),
          "captured live bishop destinations are readable");
    Check((bishopDestinations & (1ULL << Square("e1"))) != 0ULL,
          "captured live bishop can reach the exposed king");
    litch_max::DuckChessMoveResult capture;
    Check(litch_max::TryApplyDuckMove(
              FEN, Square("f7"), "b4e1", capture) &&
              capture.capturedKing,
          "captured live bishop move is a terminal king capture");

    for (uint8_t difficulty = 0U; difficulty < 12U; ++difficulty) {
        litch_max::DuckChessSearchConfig config;
        config.difficulty = difficulty;
        config.maxNodes = NODE_BUDGETS[difficulty];
        config.rngSeed = 0x3EAC972DU;

        litch_max::DuckChessTurn best;
        litch_max::DuckChessSearchStats stats;
        Check(litch_max::FindBestDuckChessTurn(
                  FEN, Square("f7"), config, best, stats),
              "captured live Duck position returns a turn");
        Check(best.chessMove.View() == "e6e1",
              "captured live Duck position takes the first exposed king");
        Check(best.capturedKing &&
                  best.duckDestination ==
                      litch_max::DUCK_CHESS_INVALID_SQUARE,
              "captured live terminal move skips Duck relocation");
        Check(stats.nodesVisited == 1U && stats.candidateTurns == 1U && stats.completed,
              "immediate king capture retains its bounded prepass at every difficulty");
    }

    modules::LitchMaxEngine engine;
    engine.Reset();
    modules::ThinkConfig config;
    config.difficulty = 0U;
    config.maxNodes = 1U;
    config.maxThinkMs = 1000U;
    config.rngSeed = 0x3EAC972DU;
    Check(engine.StartDuckThink(FEN, Square("f7"), config),
          "captured live position starts through the CPU engine adapter");
    uint8_t progress = 0U;
    modules::UciMove bestMove;
    Check(engine.PollThinkProgress(progress, bestMove) ==
                  modules::ThinkState::Done &&
              bestMove.View() == "e6e1",
          "lowest-level CPU adapter takes the immediate king capture");
    modules::DuckThinkResult result;
    Check(engine.GetLastDuckThinkResult(result) && result.capturedKing,
          "CPU adapter preserves the terminal king-capture result");
}

void TestDuckPlacementBlocksImmediateKingCapture() {
    static constexpr char AFTER_CHESS_FEN[] =
        "4r2k/8/8/8/8/8/8/4K3 b - - 0 1";

    litch_max::DuckChessSearchConfig config;
    config.maxNodes = 4096U;
    config.rngSeed = 11U;

    uint8_t destination = litch_max::DUCK_CHESS_INVALID_SQUARE;
    litch_max::DuckChessSearchStats stats;
    Check(litch_max::FindBestDuckPlacement(
              AFTER_CHESS_FEN, Square("a4"), config,
              destination, stats),
          "Duck placement search returns a destination");
    const uint64_t eFileBlockers =
        (1ULL << Square("e7")) | (1ULL << Square("e6")) |
        (1ULL << Square("e5")) | (1ULL << Square("e4")) |
        (1ULL << Square("e3")) | (1ULL << Square("e2"));
    Check((eFileBlockers & (1ULL << destination)) != 0ULL,
          "Duck search blocks an immediate rook capture of its king");
}

struct PoisonedRookCase {
    const char* fen;
    const char* duck;
    const char* capture;
    const char* reply;
    const char* safeMove;
    const char* safeDuck;
    char queen;
};

void CheckPoisonedRookCase(const PoisonedRookCase& fixture) {
    // Capturing the rook threatens the king, but a knight wins the queen.
    // Relocating the Duck cannot block capture onto the occupied queen square.
    Check(!CanCapturePiece(fixture.fen, Square(fixture.duck),
                           fixture.queen == 'Q' ? 'k' : 'K'),
          "the poisoned-rook fixture must reach search beyond the immediate-capture prepass");
    litch_max::DuckChessMoveResult capture;
    Check(litch_max::TryApplyDuckMove(
              fixture.fen, Square(fixture.duck), fixture.capture, capture),
          "poisoned rook can legally be captured");
    uint8_t poisonedReplies = 0U;
    for (uint8_t duck = 0U; duck < 64U; ++duck) {
        char occupant = '\0';
        if (duck == Square(fixture.duck) || modules::TryGetFenPieceAtSquare(
                capture.resultingFen.CStr(), duck, occupant)) {
            continue;
        }
        ++poisonedReplies;
        litch_max::DuckChessMoveResult reply;
        Check(litch_max::TryApplyDuckMove(
                  capture.resultingFen.CStr(), duck, fixture.reply, reply) &&
                  reply.capturedPieceFen == fixture.queen,
              "no legal Duck placement can prevent the knight winning the queen");
    }
    Check(poisonedReplies != 0U, "the poisoned capture has legal Duck relocations");
    litch_max::DuckChessMoveResult safe;
    Check(litch_max::TryApplyDuckMove(
              fixture.fen, Square(fixture.duck), fixture.safeMove, safe),
          "the mover can keep its queen with the safe alternative instead");
    for (uint8_t from = 0U; from < 64U; ++from) {
        uint64_t destinations = 0ULL;
        Check(litch_max::GetDuckLegalDestinationsMask(
                  safe.resultingFen.CStr(), Square(fixture.safeDuck), from, destinations) &&
                  (destinations & (1ULL << Square(fixture.safeMove + 2))) == 0ULL,
              "no opponent piece can immediately capture the alternative queen");
    }

    litch_max::DuckChessSearchConfig config;
    config.difficulty = 8U;
    config.maxNodes = 105000U;
    config.rngSeed = 7U;
    litch_max::DuckChessTurn best;
    litch_max::DuckChessSearchStats stats;
    Check(litch_max::FindBestDuckChessTurn(
              fixture.fen, Square(fixture.duck), config, best, stats) && stats.completed,
          "high-level poisoned-material search completes its configured horizon");
    std::printf("[DUCK_POISON] side=%s move=%s duck=%u score=%ld nodes=%lu completed=%u poisoned_replies=%u\n",
                fixture.queen == 'Q' ? "white" : "black", best.chessMove.CStr(),
                static_cast<unsigned>(best.duckDestination),
                static_cast<long>(best.score),
                static_cast<unsigned long>(stats.nodesVisited), stats.completed,
                static_cast<unsigned>(poisonedReplies));
    litch_max::DuckChessMoveResult selected;
    Check(ApplyCompound(fixture.fen, Square(fixture.duck), best, selected) &&
              !CanCapturePiece(selected.resultingFen.CStr(), best.duckDestination, fixture.queen),
          "high-level CPU returns a legal compound turn that keeps its queen safe");
    config.difficulty = 7U;
    Check(litch_max::FindBestDuckChessTurn(fixture.fen, Square(fixture.duck), config, best, stats) &&
              best.chessMove.View() == fixture.capture && stats.completed,
          "the lower-level shallow search retains its responsive material choice");
}

void TestHighLevelAvoidsPoisonedRook() {
    static constexpr PoisonedRookCase CASES[] = {
        {"3k4/8/5n2/3r4/8/8/8/3Q3K w - - 0 1", "a4",
         "d1d5", "f6d5", "d1c1", "a5", 'Q'},
        {"3q3k/8/8/8/3R4/5N2/8/3K4 b - - 0 1", "a5",
         "d8d4", "f3d4", "d8c8", "a4", 'q'},
    };
    for (const auto& fixture : CASES) {
        CheckPoisonedRookCase(fixture);
    }
}

void TestHighLevelProvesForcedKnightKingCapture() {
    // Nd6-f7 attacks h8; Nf6 covers g8/h7 and Nh5 covers g7. Black
    // cannot block a knight's capture by relocating the Duck after its king.
    static constexpr char FEN[] = "7k/8/3N1N2/7N/8/8/8/K7 w - - 0 1";
    litch_max::DuckChessSearchConfig config;
    config.difficulty = 8U;
    config.maxNodes = 105000U;
    config.rngSeed = 7U;
    litch_max::DuckChessTurn best;
    litch_max::DuckChessSearchStats stats;
    Check(!CanCapturePiece(FEN, Square("a4"), 'k'),
          "the forced knight fixture has no already-available king capture");
    Check(litch_max::FindBestDuckChessTurn(FEN, Square("a4"), config, best, stats),
          "high-level search returns a forced knight attack");
    litch_max::DuckChessMoveResult first;
    Check(ApplyCompound(FEN, Square("a4"), best, first) && best.score == 1000000,
          "reply search proves a future win while retaining mandatory Duck relocation");
    uint32_t replies = 0U;
    for (uint8_t to = 0U; to < 64U; ++to) {
        uint64_t destinations = 0ULL;
        Check(litch_max::GetDuckLegalDestinationsMask(first.resultingFen.CStr(),
                  best.duckDestination, Square("h8"), destinations),
              "the forced-line oracle enumerates Black king replies");
        if ((destinations & (1ULL << to)) == 0ULL) {
            continue;
        }
        modules::UciMove move;
        litch_max::DuckChessMoveResult reply;
        Check(litch_max::TryFormatUciMove(Square("h8"), to, '\0', move) &&
                  litch_max::TryApplyDuckMove(first.resultingFen.CStr(),
                      best.duckDestination, move.CStr(), reply) && !reply.capturedKing,
              "every Black reply is a legal nonterminal king move");
        for (uint8_t duck = 0U; duck < 64U; ++duck) {
            char occupant = '\0';
            if (duck == best.duckDestination || modules::TryGetFenPieceAtSquare(
                    reply.resultingFen.CStr(), duck, occupant)) {
                continue;
            }
            ++replies;
            Check(CanCapturePiece(reply.resultingFen.CStr(), duck, 'k'),
                  "every legal Black king-and-Duck reply permits terminal king capture");
        }
    }
    Check(replies != 0U && !best.capturedKing,
          "a proved future capture is not reported as an already-terminal move");
    std::printf("[DUCK_FORCED] move=%s duck=%u replies=%lu nodes=%lu completed=%u\n",
                best.chessMove.CStr(), static_cast<unsigned>(best.duckDestination),
                static_cast<unsigned long>(replies),
                static_cast<unsigned long>(stats.nodesVisited), stats.completed);
}

void TestTruncatedRepliesPreserveSafeFallback() {
    static constexpr char FEN[] = "8/8/3K1k2/8/8/8/8/8 w - - 0 1";
    litch_max::DuckChessSearchConfig config;
    config.difficulty = 7U;
    config.maxNodes = 504U;
    config.rngSeed = 7U;
    litch_max::DuckChessTurn shallow;
    litch_max::DuckChessSearchStats shallowStats;
    Check(litch_max::FindBestDuckChessTurn(FEN, Square("a4"), config,
                                        shallow, shallowStats) && shallowStats.completed,
          "tight-budget fixture completes its shallow fallback");
    config.difficulty = 8U;
    litch_max::DuckChessTurn best;
    litch_max::DuckChessSearchStats stats;
    Check(litch_max::FindBestDuckChessTurn(FEN, Square("a4"), config, best, stats),
          "a truncated opponent-reply pass still supplies a turn");
    litch_max::DuckChessMoveResult applied;
    Check(ApplyCompound(FEN, Square("a4"), best, applied) &&
              !CanCapturePiece(applied.resultingFen.CStr(), best.duckDestination, 'K') &&
              best.chessMove.View() == shallow.chessMove.View() &&
              best.duckDestination == shallow.duckDestination,
          "cheaply completed losing replies cannot displace an incomplete safe fallback");
    Check(!stats.completed && stats.nodesVisited <= config.maxNodes && best.score < 900000,
          "truncated replies neither report completion nor claim a forced win");
    std::printf("[DUCK_TRUNCATED] move=%s duck=%u nodes=%lu shallow_nodes=%lu completed=%u\n",
                best.chessMove.CStr(), static_cast<unsigned>(best.duckDestination),
                static_cast<unsigned long>(stats.nodesVisited),
                static_cast<unsigned long>(shallowStats.nodesVisited), stats.completed);
}

struct CancelAtNode {
    litch_max::DuckChessSearchStats& stats;
    uint32_t node;
    static bool Cancel(void* context) {
        const auto& stop = *static_cast<CancelAtNode*>(context);
        return stop.stats.nodesVisited >= stop.node;
    }
};

void TestBudgetCancellationAndDeterminism() {
    static constexpr char FEN[] = "3k4/8/5n2/3r4/8/8/8/3Q3K w - - 0 1";
    litch_max::DuckChessSearchConfig config;
    config.difficulty = 8U;
    config.maxNodes = 105000U;
    config.rngSeed = 7U;
    litch_max::DuckChessTurn first;
    litch_max::DuckChessSearchStats firstStats;
    Check(litch_max::FindBestDuckChessTurn(FEN, Square("a4"), config, first, firstStats) &&
              firstStats.completed && firstStats.nodesVisited < config.maxNodes,
          "adequate budget completes the configured selective reply horizon");
    litch_max::DuckChessTurn repeat;
    litch_max::DuckChessSearchStats repeatStats;
    Check(litch_max::FindBestDuckChessTurn(FEN, Square("a4"), config, repeat, repeatStats) &&
              first.chessMove.View() == repeat.chessMove.View() &&
              first.duckDestination == repeat.duckDestination && first.score == repeat.score &&
              firstStats.nodesVisited == repeatStats.nodesVisited &&
              firstStats.candidateTurns == repeatStats.candidateTurns,
          "fixed input, difficulty, seed and budget produce identical work and result");

    for (const uint32_t node : {0U, 100U, 1500U}) {
        litch_max::DuckChessTurn canceled;
        litch_max::DuckChessSearchStats stats;
        CancelAtNode stop{stats, node};
        const bool found = litch_max::FindBestDuckChessTurn(FEN, Square("a4"), config,
            canceled, stats, &CancelAtNode::Cancel, &stop);
        Check(stats.canceled && !stats.completed && stats.nodesVisited == node &&
                  found == (stats.completedDepth != 0U),
              "cancel is honored exactly and retains a turn only after a whole root completed");
        if (found) {
            litch_max::DuckChessMoveResult applied;
            Check(ApplyCompound(FEN, Square("a4"), canceled, applied) && canceled.score < 900000,
                  "canceled searches retain only an available legal non-proven fallback");
        }
    }
    config.maxNodes = 2U;
    Check(!litch_max::FindBestDuckChessTurn(FEN, Square("a4"), config, repeat, repeatStats) &&
              repeatStats.nodesVisited == config.maxNodes && !repeatStats.completed && repeat.chessMove.Empty(),
          "two nodes cannot present a partially compared root as an analyzed CPU choice");
    config.maxNodes = 1U;
    Check(!litch_max::FindBestDuckChessTurn(FEN, Square("a4"), config, repeat, repeatStats) &&
              repeatStats.nodesVisited == config.maxNodes && !repeatStats.completed,
          "an insufficient nonterminal budget cannot invent a complete compound turn");
}

void TestMcuMaxRunsDuckSearchThroughThinkContract() {
    static constexpr char FEN[] =
        "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1";
    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::ThinkConfig config;
    config.difficulty = 5U;
    config.maxNodes = 4096U;
    config.rngSeed = 17U;
    Check(engine.StartDuckThink(FEN, Square("a4"), config),
          "mcu-max adapter starts Duck search");

    uint8_t progress = 0U;
    modules::UciMove bestMove;
    Check(engine.PollThinkProgress(progress, bestMove) ==
              modules::ThinkState::Done &&
              progress == 100U,
          "Duck search completes through the async think poll seam");
    modules::DuckThinkResult result;
    Check(engine.GetLastDuckThinkResult(result),
          "compound Duck result is available after polling");
    Check(result.chessMove.View() == "e2e8" &&
              result.capturedKing &&
              result.duckDestination ==
                  litch_max::DUCK_CHESS_INVALID_SQUARE,
          "think result retains chess move and terminal duck decision");
}

// Controlled clock ticks exercise deadline boundaries deterministically. They
// are not elapsed native/S3 milliseconds or a search-strength benchmark.
uint32_t deadlineClockTicks = 0U;
uint32_t cancelAtClockTick = 0U;
modules::LitchMaxEngine* deadlineEngine = nullptr;
uint32_t DeadlineClock() {
    ++deadlineClockTicks;
    if (deadlineEngine != nullptr && cancelAtClockTick != 0U &&
        deadlineClockTicks >= cancelAtClockTick) {
        deadlineEngine->CancelThink();
    }
    return deadlineClockTicks;
}

void TestDuckDeadlinePreservesCompletedCandidate() {
    for (const bool explicitCancel : {false, true}) {
        modules::LitchMaxEngine engine;
        engine.Reset();
        deadlineClockTicks = 0U;
        cancelAtClockTick = explicitCancel ? 10001U : 0U;
        deadlineEngine = &engine;
        const fxe::ScopedMillisProviderOverride clock(&DeadlineClock);
        auto config = modules::MakeThinkConfigForVsCpuLevel(4U, 1U);
        config.maxThinkMs = 10000U;
        struct Publications {
            uint32_t seeds = 0U;
            uint32_t completedRoots = 0U;
        } publications;
        config.candidateObserver = {[](void* context, const modules::ThinkCandidate& candidate) {
            auto& observed = *static_cast<Publications*>(context);
            if (candidate.source == modules::ThinkCandidateSource::Seed) {
                Check(candidate.isDuck && candidate.completedDepth == 0U,
                      "CPU startup publishes an explicit depth-zero compound Seed");
                ++observed.seeds;
                return;
            }
            Check(candidate.isDuck && candidate.completedDepth != 0U &&
                      candidate.rootKind == modules::ThinkRootKind::DuckCompound && candidate.scoreValid,
                  "deadline observer receives completed scored compound roots");
            ++observed.completedRoots;
        }, &publications};
        Check(engine.StartDuckThink(modules::INITIAL_FEN, Square("a4"), config),
              "controlled deadline starts real Duck search");
        uint8_t progress = 0U;
        modules::UciMove move;
        const auto result = engine.PollThinkProgress(progress, move);
        modules::DuckThinkResult compound;
        const bool haveCompound = engine.GetLastDuckThinkResult(compound);
        Check(publications.seeds == 1U && publications.completedRoots != 0U,
              "retention and coincident-cancel reach a completed root after the separate startup Seed");
        if (explicitCancel) {
            Check(result == modules::ThinkState::Canceled && !haveCompound,
                  "explicit cancellation discards a coincident deadline candidate");
        } else {
            litch_max::DuckChessMoveResult applied;
            Check(result == modules::ThinkState::Done && haveCompound &&
                      litch_max::TryApplyDuckMove(modules::INITIAL_FEN, Square("a4"),
                                                compound.chessMove.CStr(), applied) &&
                      compound.duckDestination < 64U &&
                      compound.duckDestination != Square("a4"),
                  "deadline keeps a completed legal compound candidate instead of canceling it");
            char piece = '\0';
            Check(!modules::TryGetFenPieceAtSquare(applied.resultingFen.CStr(),
                                                  compound.duckDestination, piece),
                  "retained deadline destination is empty after its selected chess move");
        }
        std::printf("[DUCK_DEADLINE] cancel=%u state=%u candidate=%u clock_ticks=%u move=%s duck=%u\n",
                    explicitCancel, static_cast<unsigned>(result), haveCompound,
                    deadlineClockTicks, move.CStr(), compound.duckDestination);
        deadlineEngine = nullptr;
    }
    modules::LitchMaxEngine engine;
    engine.Reset();
    deadlineClockTicks = 0U;
    cancelAtClockTick = 0U;
    const fxe::ScopedMillisProviderOverride clock(&DeadlineClock);
    auto config = modules::MakeThinkConfigForVsCpuLevel(4U, 1U);
    config.maxThinkMs = 1U;
    modules::ThinkCandidate seed;
    config.candidateObserver = {[](void* context, const modules::ThinkCandidate& candidate) {
        *static_cast<modules::ThinkCandidate*>(context) = candidate;
    }, &seed};
    Check(engine.StartDuckThink(modules::INITIAL_FEN, Square("a4"), config),
          "empty deadline starts real Duck search");
    uint8_t progress = 0U;
    modules::UciMove move;
    modules::DuckThinkResult compound;
    modules::UciMove seedMove;
    Check(seed.source == modules::ThinkCandidateSource::Seed && seed.completedDepth == 0U &&
              seed.isDuck && litch_max::TryFormatUciMove(seed.fromSquare, seed.toSquare, seed.promotion, seedMove),
          "early deadline starts with the separately identified compound Seed");
    litch_max::DuckChessMoveResult applied;
    char occupant = '\0';
    Check(engine.PollThinkProgress(progress, move) == modules::ThinkState::Done &&
              engine.GetLastDuckThinkResult(compound) && move.Equals(seedMove.CStr()) &&
              compound.chessMove.Equals(seedMove.CStr()) && compound.duckDestination == seed.duckDestination &&
              litch_max::TryApplyDuckMove(modules::INITIAL_FEN, Square("a4"), compound.chessMove.CStr(), applied) &&
              compound.duckDestination < 64U && compound.duckDestination != Square("a4") &&
              !modules::TryGetFenPieceAtSquare(applied.resultingFen.CStr(), compound.duckDestination, occupant),
          "CPU deadline before a completed root retains its exact legal compound Seed");
}

struct ContinuousDuckPublicationProbe {
    litch_max::DuckChessSearchStats* stats = nullptr;
    uint32_t publications = 0U;
    uint32_t beforeCompletedRoot = 0U;
};

void ObserveContinuousDuckCandidate(
    void* context, const modules::ThinkCandidate&) {
    auto& probe = *static_cast<ContinuousDuckPublicationProbe*>(context);
    ++probe.publications;
    if (probe.stats != nullptr && probe.stats->completedDepth == 0U) {
        ++probe.beforeCompletedRoot;
    }
}

void TestContinuousDuckPublishesOnlyCompletedRootIterations() {
    litch_max::DuckChessAnalysisWorkspace workspace;
    litch_max::DuckChessSearchConfig config;
    config.difficulty = 8U;
    config.maxNodes = 4096U;
    config.rngSeed = 23U;
    config.continuousAnalysis = true;
    litch_max::DuckChessSearchStats stats;
    ContinuousDuckPublicationProbe probe{&stats};
    config.candidateObserver = {
        &ObserveContinuousDuckCandidate, &probe};
    litch_max::DuckChessTurn best;

    Check(litch_max::FindBestDuckChessTurn(
              modules::INITIAL_FEN, Square("a4"), config, best, stats,
              nullptr, nullptr),
          "continuous Duck search returns its available compound candidate");
    Check(stats.completedDepth != 0U && probe.publications != 0U,
          "continuous Duck search completes and publishes a candidate");
    Check(probe.beforeCompletedRoot == 0U,
          "continuous Duck Hint waits for a complete root iteration");
}

void BenchmarkBoundedInitialPositionSearch() {
    litch_max::DuckChessSearchConfig config;
    config.maxNodes = 4096U;
    config.rngSeed = 23U;
    config.difficulty = 8U;
    litch_max::DuckChessTurn best;
    litch_max::DuckChessSearchStats stats;
    const auto start = std::chrono::steady_clock::now();
    const bool found = litch_max::FindBestDuckChessTurn(
        modules::INITIAL_FEN, Square("a4"), config, best, stats);
    const auto elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    Check(found && !stats.completed,
          "a tight initial-position budget returns its available turn with partial status");
    Check(stats.nodesVisited <= config.maxNodes &&
              stats.candidateTurns != 0U,
          "Duck search honors its fixed node budget");
    std::printf(
        "[DUCK_SEARCH_BENCH] elapsed_us=%lld nodes=%lu turns=%lu move=%s duck=%u\n",
        static_cast<long long>(elapsedUs),
        static_cast<unsigned long>(stats.nodesVisited),
        static_cast<unsigned long>(stats.candidateTurns),
        best.chessMove.CStr(),
        static_cast<unsigned>(best.duckDestination));
}

void BenchmarkConfiguredInitialPositionSearch() {
    for (const uint8_t level : {7U, 8U}) {
        const auto configured = modules::MakeThinkConfigForVsCpuLevel(level, 23U);
        litch_max::DuckChessSearchConfig config;
        config.difficulty = configured.difficulty;
        config.maxNodes = configured.maxNodes;
        config.rngSeed = configured.rngSeed;
        litch_max::DuckChessTurn best;
        litch_max::DuckChessSearchStats stats;
        const auto start = std::chrono::steady_clock::now();
        Check(litch_max::FindBestDuckChessTurn(modules::INITIAL_FEN, Square("a4"),
                  config, best, stats) && stats.completed && !stats.canceled &&
                  stats.nodesVisited <= config.maxNodes,
              "configured lower and higher levels complete their bounded initial-position horizon");
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("[DUCK_CONFIGURED_BENCH] level=%u budget=%lu elapsed_us=%lld nodes=%lu turns=%lu completed=%u\n",
                    static_cast<unsigned>(level), static_cast<unsigned long>(config.maxNodes),
                    static_cast<long long>(elapsedUs),
                    static_cast<unsigned long>(stats.nodesVisited),
                    static_cast<unsigned long>(stats.candidateTurns), stats.completed);
    }
}

} // namespace

int main() {
    std::puts("[DUCK_CHESS_SEARCH] start");
    TestSearchChoosesImmediateKingCapture();
    TestCapturedLivePositionChoosesKingCaptureAtEveryBudget();
    TestDuckPlacementBlocksImmediateKingCapture();
    TestHighLevelAvoidsPoisonedRook();
    TestHighLevelProvesForcedKnightKingCapture();
    TestTruncatedRepliesPreserveSafeFallback();
    TestBudgetCancellationAndDeterminism();
    TestMcuMaxRunsDuckSearchThroughThinkContract();
    TestDuckDeadlinePreservesCompletedCandidate();
    TestContinuousDuckPublishesOnlyCompletedRootIterations();
    BenchmarkBoundedInitialPositionSearch();
    BenchmarkConfiguredInitialPositionSearch();
    if (g_failures == 0) {
        std::puts("[DUCK_CHESS_SEARCH] ok");
        return 0;
    }
    std::printf("[DUCK_CHESS_SEARCH] failures=%d\n", g_failures);
    return 1;
}
