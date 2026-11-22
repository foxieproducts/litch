#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <mcu-max.h>

#include "modules/classic_search_config.hpp"
#include "../../lib/litch-max/src/classic_chess_quiescence_cache.hpp"

namespace {
using namespace litch_max::classic_chess;
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::printf("[FAIL] %s\n", message); }
}
mcumax_position Position(const char* fen) {
    mcumax_init(); mcumax_set_fen_position(fen);
    mcumax_position root{};
    Check(mcumax_position_import(&root, &mcumax), "fixture imports through production rules");
    return root;
}
bool SameMove(mcumax_position_move a, mcumax_position_move b) {
    return a.from == b.from && a.to == b.to && a.promotion == b.promotion;
}

void CheckBoundsAndContext() {
    internal::QuiescenceCache cache;
    int score = 777;
    Check(!cache.Probe(123U, 4U, true, -100, 100, score) && score == 777,
          "fresh cache cannot invent a score");
    cache.Store(123U, 4U, true, 25, 0, 50);
    Check(cache.Probe(123U, 4U, true, -100, 100, score) && score == 25,
          "complete exact result survives a wider window");
    Check(!cache.Probe(123U, 5U, true, -100, 100, score) &&
              !cache.Probe(123U, 4U, false, -100, 100, score),
          "different horizon or quiet-check permission refuses reuse");
    cache.Store(123U, 4U, true, 80, 0, 50);
    Check(cache.Probe(123U, 4U, true, 0, 60, score) && score == 80 &&
              !cache.Probe(123U, 4U, true, -100, 100, score),
          "narrow lower bound cannot masquerade as a wide-window exact score");
    cache.Store(123U, 4U, true, -80, -50, 0);
    Check(cache.Probe(123U, 4U, true, -60, 0, score) && score == -80 &&
              !cache.Probe(123U, 4U, true, -100, 100, score),
          "narrow upper bound cannot masquerade as a wide-window exact score");
    cache.Store(123U + 128U, 4U, true, 29995, -31000, 31000);
    Check(!cache.Probe(123U, 4U, true, -100, 100, score) &&
              cache.Probe(251U, 4U, true, -31000, 31000, score) && score == 29995,
          "index collision needs the full identity and preserves mate score at the same ply");
    cache.Store(321U, 6U, false, 0, -100, 100);
    Check(cache.Probe(321U, 6U, false, -100, 100, score) && score == 0,
          "a completed zero terminal value remains distinct from an empty entry");
}

struct Publication {
    unsigned count = 0U;
    modules::ThinkCandidate last{};
    static void Observe(void* context, const modules::ThinkCandidate& candidate) {
        auto& self = *static_cast<Publication*>(context);
        ++self.count; self.last = candidate;
    }
};

void CheckFixedDepthCacheBenefit() {
    const auto root = Position("2r1kb1r/p2q1p2/Q2ppn1p/3P2p1/P7/2N2N1P/1PP2PP1/R1B1R1K1 w k - 0 1");
    const auto before = root;
    modules::ThinkConfig request;
    request.continuousAnalysis = true;
    request.maxNodes = 350000U;
    request.maxDepth = 5U;
    auto config = modules::ResolveClassicSearchConfig(request);
    // Compare fixed-depth kernel work, independently of human Hint eligibility.
    // This local minimum withholds shallower publications in both cache modes.
    config.minimumDepth = 5U;
    // Isolate tactical-result reuse from the separate recapture ordering.
    config.recaptureOrdering = false;
    SearchWorkspace workspace;
    Publication oldPublication, publication;
    config.candidateObserver = {&Publication::Observe, &oldPublication};
    config.quiescenceCache = false;
    const auto old = workspace.Run(root, config);
    config.quiescenceCache = true;
    config.candidateObserver = {&Publication::Observe, &publication};
    const auto improved = workspace.Run(root, config);
    Check(!old.available && oldPublication.count == 0U && old.interrupted,
          "uncached fixed-depth search cannot complete Full5 inside the work budget");
    Check(improved.available && !improved.interrupted && improved.completedDepth == 5U &&
              publication.count == 1U && publication.last.rootKind == modules::ThinkRootKind::ClassicFull &&
              publication.last.completedDepth == 5U && publication.last.scoreValid,
          "cached fixed-depth search completes exactly one scored Full5 within the same budget");
    config.maxNodes = 0U;
    config.quiescenceCache = false;
    config.candidateObserver = {};
    const auto reference = workspace.Run(root, config);
    Check(SameMove(reference.move, improved.move) && reference.score == improved.score &&
              improved.nodes < reference.nodes && std::memcmp(&root, &before, sizeof(root)) == 0,
          "cached Full5 matches the uncached completed move/score with fewer nodes and preserves the caller");
    std::printf("[Q_CACHE_FIXED_DEPTH] reference=%u improved=%u budget=%u depth=%u score=%d workspace=%zu\n",
                reference.nodes, improved.nodes, request.maxNodes, improved.completedDepth,
                improved.score, SearchWorkspace::RequiredBytes());
}

void CheckHistoryAndTerminalOracles() {
    const char* fixtures[] = {
        "r2qk2r/pQ1b2p1/1p2p3/2p5/2P3n1/7N/PP1N2P1/R3KB1R b KQkq - 1 17",
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
        "8/8/8/8/3k4/8/3P4/3K4 w - - 0 1",
        "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1",
        "7k/5K2/6Q1/8/8/8/8/8 b - - 0 1",
    };
    SearchWorkspace workspace;
    for (const char* fen : fixtures) for (const uint16_t clock : {uint16_t{0}, uint16_t{99}, uint16_t{UINT16_MAX}}) {
        const auto root = Position(fen), before = root;
        SearchConfig config;
        config.maxDepth = 3U; config.reversibleHalfmoves = clock;
        config.nullMovePruning = false; config.lateQuietReduction = false;
        config.quiescenceCache = false;
        const auto reference = workspace.Run(root, config);
        config.quiescenceCache = true;
        const auto candidate = workspace.Run(root, config);
        Check(reference.available == candidate.available && reference.terminal == candidate.terminal &&
                  reference.score == candidate.score &&
                  (candidate.terminal || SameMove(reference.move, candidate.move)) &&
                  std::memcmp(&root, &before, sizeof(root)) == 0,
              "full-width check, EP, KPK clock, mate and stalemate values match the uncached oracle");
        config.cacheMode = CacheMode::Disabled;
        config.quiescenceCache = false;
        const auto disabled = workspace.Run(root, config);
        config.quiescenceCache = true;
        const auto noTraffic = workspace.Run(root, config);
        Check(disabled.nodes == noTraffic.nodes && disabled.score == noTraffic.score,
              "disabled cache policy makes no tactical-result reuse traffic");
    }
}

} // namespace

int main() {
    CheckBoundsAndContext();
    CheckFixedDepthCacheBenefit();
    CheckHistoryAndTerminalOracles();
    std::printf("[CLASSIC_QUIESCENCE_CACHE] failures=%d; work budget is not measured S3 time\n", failures);
    return failures == 0 ? 0 : 1;
}
