#include <array>
#include <cstdio>
#include <cstring>

#include <mcu-max.h>
#include "modules/async_think_worker.hpp"
#include <litch_max/classic_chess_kpk.hpp>
#include <litch_max/classic_chess_search.hpp>
#include "modules/initial_fen.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/vs_cpu_difficulty.hpp"

namespace {
using namespace modules;
using namespace litch_max::classic_chess;
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::printf("[FAIL] %s\n", message); }
}
struct Observed {
    std::array<ThinkCandidate, 40> candidates{};
    uint32_t nodes = 0U;
    unsigned count = 0U;
    uint32_t firstNodes = UINT32_MAX;
    ThinkConfig Config(bool book = true) {
        ThinkConfig config;
        config.continuousAnalysis = true;
        // Isolate Book metadata/reuse at zero minimum. Production human Hint
        // requires scored live work and its separate 500 ms calculation gate.
        config.minimumCalculationMs = 0U;
        config.maxNodes = 1U;
        config.maxDepth = 5U;
        config.reversibleHalfmoves = 0U;
        config.openingPrefix.fromCanonicalStart = book;
        config.progressObserver = {[](void* ctx, uint32_t n) {
            static_cast<Observed*>(ctx)->nodes = n;
        }, this};
        config.candidateObserver = {[](void* ctx, const ThinkCandidate& candidate) {
            auto& self = *static_cast<Observed*>(ctx);
            if (self.count == 0U) self.firstNodes = self.nodes;
            if (self.count < self.candidates.size()) self.candidates[self.count++] = candidate;
        }, this};
        return config;
    }
};
ThinkState Run(LitchMaxEngine& engine, const ThinkConfig& config, UciMove& best) {
    Check(engine.StartThink(config), "actual engine admits knowledge search");
    uint8_t progress = 0U;
    return engine.PollThinkProgress(progress, best);
}
void CheckBookLifecycle() {
    LitchMaxEngine engine;
    engine.Reset();
    Observed first;
    UciMove best;
    auto firstConfig = first.Config(); firstConfig.maxNodes = 500000U; firstConfig.maxDepth = 3U;
    Check(Run(engine, firstConfig, best) == ThinkState::Done && best.Equals("b1c3"),
          "offline Full5 opening move survives completed live Full3 work");
    Check(first.count == 1U && first.firstNodes == 0U &&
          first.candidates[0].source == ThinkCandidateSource::Book &&
          first.candidates[0].completedDepth == 0U &&
          first.candidates[0].offlineDepth == 5U &&
          first.candidates[0].completedNodes == 0U,
          "Book alone publishes truthful offline depth while Seed and shallower live work stay internal");
    ClassicThinkResult result;
    Check(engine.GetLastClassicThinkResult(result) && result.source == ThinkCandidateSource::Book &&
          result.offlineDepth == 5U && result.bestScoreValid && !result.preliminaryOnly,
          "final retained book diagnostics do not advertise live search depth");
    Observed refined;
    auto config = refined.Config(); config.maxNodes = 500000U;
    Check(Run(engine, config, best) == ThinkState::Done && refined.count == 2U &&
          refined.candidates[0].source == ThinkCandidateSource::Book &&
          refined.candidates[refined.count - 1U].source == ThinkCandidateSource::Search &&
          refined.candidates[refined.count - 1U].completedDepth == 5U,
          "same worker proceeds from Book to completed Full5 live search");
    Observed retry;
    Check(Run(engine, retry.Config(), best) == ThinkState::Done && retry.count == 1U &&
          retry.candidates[0].source == ThinkCandidateSource::Search &&
          retry.candidates[0].completedDepth == 5U,
          "tiny same-position retry cannot replace retained searched Hint with Book");
    Check(engine.ApplyUciMove("b1c3"), "acknowledged real opening move changes the engine root");
    Observed black;
    config = black.Config(); config.openingPrefix.moveCount = 1U;
    config.openingPrefix.moves[0] = {57U, 42U, '\0'}; config.reversibleHalfmoves = 1U;
    Check(Run(engine, config, best) == ThinkState::Done && black.count == 1U &&
          black.candidates[0].source == ThinkCandidateSource::Book,
          "acknowledged complete opening prefix supplies the next legal Book hint");
    engine.Reset();
    Observed imported;
    Check(Run(engine, imported.Config(false), best) == ThinkState::Done && imported.count == 0U,
          "unknown-prefix tiny Hint work publishes neither Book nor its internal Seed");
    engine.Reset();
    Observed malformed;
    config = malformed.Config(); config.openingPrefix.moveCount = 1U;
    config.openingPrefix.moves[0] = {57U, 42U, '\0'};
    Check(Run(engine, config, best) == ThinkState::Done && malformed.count == 0U,
          "history which disagrees with actual root cannot publish Book");
    engine.Reset();
    Observed canceled;
    config = canceled.Config();
    Check(engine.StartThink(config), "cancel fixture admits the opening worker");
    engine.CancelThink(); uint8_t progress = 0U;
    Check(engine.PollThinkProgress(progress, best) == ThinkState::Canceled && canceled.count == 0U,
          "canceled opening work cannot publish an otherwise valid Book result");
    for (uint8_t level = 1U; level <= 11U; ++level) {
        engine.Reset(); Observed cpu;
        config = MakeThinkConfigForVsCpuLevel(level, 1U);
        config.openingPrefix.fromCanonicalStart = true; config.reversibleHalfmoves = 0U;
        config.maxNodes = 10000U; config.maxDepth = 2U;
        config.candidateObserver = cpu.Config().candidateObserver;
        Check(Run(engine, config, best) == ThinkState::Done && cpu.count != 0U &&
              engine.GetLastClassicThinkResult(result) && result.source == ThinkCandidateSource::Search,
              "each CPU level still selects completed live search with book root ordering");
        for (unsigned n = 0U; n < cpu.count; ++n)
            Check(cpu.candidates[n].source == ThinkCandidateSource::Search ||
                      (n == 0U && cpu.candidates[n].source == ThinkCandidateSource::Seed &&
                       cpu.candidates[n].completedDepth == 0U),
                  "CPU book ordering never substitutes an offline move for analyzed selection");
    }
}
void CheckMailbox() {
    AsyncThinkWorker worker;
    auto publish = worker.PrepareCandidateObserver();
    ThinkCandidate book; book.fromSquare = 57U; book.toSquare = 42U;
    book.source = ThinkCandidateSource::Book; book.offlineDepth = 5U;
    publish.Publish(book); ThinkCandidate copy;
    Check(worker.TryGetPublishedCandidate(copy) && copy.source == ThinkCandidateSource::Book &&
          copy.offlineDepth == 5U && copy.completedDepth == 0U && copy.promotion == '\0' &&
          !copy.isDuck && copy.fromSquare == book.fromSquare && copy.toSquare == book.toSquare,
          "atomic32-bit mailbox preserves Book source and both truthful depths");
    for (const char promotion : {'\0', 'q', 'r', 'b', 'n'}) {
        ThinkCandidate searched; searched.fromSquare = 9U; searched.toSquare = 1U;
        searched.completedDepth = 127U; searched.promotion = promotion;
        publish.Publish(searched);
        Check(worker.TryGetPublishedCandidate(copy) && copy.source == ThinkCandidateSource::Search &&
              copy.completedDepth == 127U && copy.offlineDepth == 0U && copy.promotion == promotion,
              "all prior searched promotion/depth encodings round-trip unchanged");
    }
    book.completedDepth = 1U; publish.Publish(book);
    Check(worker.TryGetPublishedCandidate(copy) && copy.source == ThinkCandidateSource::Search,
          "mixed live/offline provenance cannot overwrite the atomic candidate");
    book.completedDepth = 0U; book.promotion = 'q'; publish.Publish(book);
    Check(worker.TryGetPublishedCandidate(copy) && copy.source == ThinkCandidateSource::Search,
          "reserved Book encoding declines promotion instead of losing metadata");
    book.promotion = '\0'; book.isDuck = true; book.duckDestination = 20U; publish.Publish(book);
    Check(worker.TryGetPublishedCandidate(copy) && copy.source == ThinkCandidateSource::Search,
          "Classic-only Book encoding cannot contaminate Duck candidates");
}
mcumax_position Position(const char* fen) {
    LitchMaxEngine engine; engine.Reset(); Check(engine.SetFen(fen), "KPK fixture FEN imports");
    mcumax_position root{};
    Check(mcumax_position_import(&root, &mcumax), "production pure KPK root imports");
    return root;
}
void CheckKpkSearch() {
    SearchWorkspace workspace;
    SearchConfig config; config.maxDepth = 5U; config.maxNodes = 200000U;
    config.reversibleHalfmoves = 0U;
    auto draw = Position("8/8/4k3/8/4K3/4P3/8/8 w - - 0 1");
    const auto saved = draw;
    auto result = workspace.Run(draw, config);
    Check(result.available && result.score == 0 && std::memcmp(&draw, &saved, sizeof(draw)) == 0,
          "actual search replaces the old+120cp opposition-draw error with exact Draw knowledge");
    auto win = Position("8/8/4k3/8/4K3/4P3/8/8 b - - 0 1");
    result = workspace.Run(win, config);
    Check(result.available && result.score <= -150,
          "actual search distinguishes the won opposition from the former equal material score");
    config.reversibleHalfmoves = 100U;
    result = workspace.Run(win, config);
    Check(result.terminal && !result.available && result.score == 0,
          "fifty-move boundary takes precedence over a bitbase win");
    auto mate = Position("7k/6Q1/6K1/8/8/8/8/8 b - - 100 1");
    result = workspace.Run(mate, config);
    Check(result.terminal && !result.available && result.score <= -29000,
          "checkmate takes precedence over the reversible-clock draw");
    config.reversibleHalfmoves = UINT16_MAX;
    result = workspace.Run(win, config);
    Check(result.available && result.score > -150,
          "unknown clock declines a win claim until a real searched pawn/capture resets it");
    config.reversibleHalfmoves = 0U;
    const auto fresh = workspace.Run(win, config);
    SearchWorkspace separate;
    const auto independent = separate.Run(win, config);
    Check(fresh.available && fresh.score == independent.score &&
          fresh.move.from == independent.move.from && fresh.move.to == independent.move.to &&
          fresh.nodes == independent.nodes,
          "prior unknown/high-clock requests cannot leave history-sensitive cached bounds");
    const char* conversions[]{
        "8/8/4k3/8/4K3/4P3/8/8 b - - 0 1",
        "8/8/8/8/4K3/4P3/8/4k3 w - - 0 1",
        "8/k1P5/2K5/8/8/8/8/8 w - - 0 1",
        "4K3/8/4p3/4k3/8/8/8/8 b - - 0 1"};
    for (const uint8_t depth : {5U, 8U}) {
    config.maxDepth = depth;
    for (const char* fen : conversions) {
        auto root = Position(fen); uint16_t halfmoves = 0U;
        Check(TryProbeKpk(root, halfmoves).outcome == KpkOutcome::PawnWin, "conversion begins with certified winning KPK");
        bool promoted = false;
        for (unsigned ply = 0U; ply < 64U && !promoted; ++ply) {
            config.reversibleHalfmoves = halfmoves;
            const auto turn = workspace.Run(root, config);
            if (!turn.available) { Check(false, "known winning KPK obtains a completed live move"); break; }
            const uint8_t type = root.board[turn.move.from] & 7U;
            const bool reset = type == MCUMAX_PAWN_UPSTREAM || type == MCUMAX_PAWN_DOWNSTREAM || root.board[turn.move.to] != 0U;
            mcumax_position_undo undo{};
            Check(mcumax_position_make(&root, turn.move, &undo), "selected KPK progress move is actually legal");
            halfmoves = reset ? 0U : halfmoves + 1U;
            if (turn.move.promotion != 0U) {
                promoted = true;
                Check(mcumax_position_any_legal(&root) || mcumax_position_is_check(&root, root.side),
                      "live search chooses conversion instead of promotion stalemate");
            } else {
                Check(TryProbeKpk(root, halfmoves).outcome == KpkOutcome::PawnWin,
                      "both sides' searched line preserves the certified winning strategy and clock margin");
            }
            std::printf("[KPK_SEARCH_PROGRESS] fen=%s ply=%u from=%u to=%u promotion=%u halfmoves=%u score=%d depth=%u\n",
                fen, ply, turn.move.from, turn.move.to, turn.move.promotion, halfmoves, turn.score, turn.completedDepth);
        }
        Check(promoted, "Full5/8 strategy converts the pawn without quiet king cycling");
    }
    }
}
} // namespace
int main() {
    failures = 0;
    CheckBookLifecycle(); CheckMailbox(); CheckKpkSearch();
    std::printf("[CLASSIC_KNOWLEDGE] failures=%d workspace_bytes=%zu\n", failures, SearchWorkspace::RequiredBytes());
    return failures == 0 ? 0 : 1;
}
