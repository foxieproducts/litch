#include <cstdio>
#include <cstring>

#include <mcu-max.h>
#include <mcu-max-position.h>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/game_persistence_coordinator_vs_cpu_internal.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/vs_cpu_difficulty.hpp"
#include "../../lib/litch-max/src/classic_chess_move_order.hpp"

namespace {

constexpr const char* QUEEN_ESCAPE_FEN =
    "r2q1b1r/pppk1Bpp/2np1n2/4p1Nb/4P1P1/2N2P2/PPPP3P/R1BQK2R b KQ - 0 8";

struct RootObservation {
    modules::ThinkCandidate latest{};

    static void Completed(void* context, const modules::ThinkCandidate& candidate) {
        static_cast<RootObservation*>(context)->latest = candidate;
    }
};

void CheckCpuQueenEscape(fxe::test_support::TestResults& results) {
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    modules::LitchMaxEngine engine;
    engine.Reset();
    if (!engine.SetFen(QUEEN_ESCAPE_FEN)) {
        results.Check(false, "quality regression imports through the production engine");
        return;
    }
    char before[128]{}, after[128]{};
    results.Check(engine.GetFen(before, sizeof(before)), "initial FEN is available");
    RootObservation observed;
    auto config = modules::MakeThinkConfigForVsCpuTurn(11U, 2U, 5U);
    config.candidateObserver.completedRoot = &RootObservation::Completed;
    config.candidateObserver.context = &observed;
    results.Check(config.maxNodes == 1800000U && config.maxDepth == 16U &&
                      config.reversibleHalfmoves == UINT16_MAX &&
                      !config.openingPrefix.fromCanonicalStart,
                  "imported position retains the normal full CPU11 budget and unknown history");
    if (!engine.StartThink(config)) {
        results.Check(false, "production CPU analysis starts");
        return;
    }
    // StartThink legitimately installs the requested hash seed. Search itself
    // must preserve that initialized singleton as well as the live FEN.
    unsigned char singleton[sizeof(mcumax)];
    std::memcpy(singleton, &mcumax, sizeof(mcumax));
    mcumax_position root{};
    results.Check(mcumax_position_import(&root, &mcumax), "initialized live position imports");
    uint8_t progress = 0U;
    modules::UciMove searched;
    const auto state = engine.PollThinkProgress(progress, searched);
    modules::ClassicThinkResult analysis;
    modules::ThinkStats stats;
    results.Check(state == modules::ThinkState::Done &&
                      engine.GetLastClassicThinkResult(analysis) &&
                      engine.GetLastThinkStats(stats),
                  "normal bounded search produces completed analysis and statistics");
    results.Check(std::memcmp(&mcumax, singleton, sizeof(singleton)) == 0,
                  "search preserves every byte of the initialized legacy singleton");
    // The old selective search misses a safe queen escape after h6 Ne6 and
    // chooses Nd4. Finite independent analysis prefers h6 at both tested limits.
    results.CheckEqStr(searched.View(), "h7h6",
                      "CPU11 retains the stronger move when its reply needs a quiet queen escape");
    results.Check(observed.latest.rootKind == modules::ThinkRootKind::ClassicFull &&
                      observed.latest.completedDepth >= 8U &&
                      analysis.source == modules::ThinkCandidateSource::Search &&
                      !analysis.preliminaryOnly && analysis.bestScoreValid &&
                      stats.nodeCount <= config.maxNodes,
                  "quality comes from completed Full search within the unchanged budget");
    litch_max::ParsedUciMove parsed;
    modules::UciMove selected;
    results.Check(modules::game_persistence_vs_cpu_internal::TryChooseClassicCpuMove(
                      engine, searched, parsed, selected),
                  "the board's production CPU selector accepts the analyzed move");
    results.CheckEqStr(selected.View(), "h7h6", "the actual queued selection retains the quality improvement");
    results.Check(observed.latest.fromSquare == parsed.fromSquare &&
                      observed.latest.toSquare == parsed.toSquare &&
                      observed.latest.score == analysis.bestScore,
                  "the selected move and score belong to the last completed Full root");
    mcumax_position selectedRoot{};
    results.Check(engine.GetFen(after, sizeof(after)) && std::strcmp(before, after) == 0 &&
                      mcumax_position_import(&selectedRoot, &mcumax) &&
                      std::memcmp(&root, &selectedRoot, sizeof(root)) == 0,
                  "selection preserves the live board, side, king, castling and EP state");
    std::printf("[QUEEN_ESCAPE_CPU] searched=%s selected=%s depth=%u nodes=%u\n",
                searched.CStr(), selected.CStr(), observed.latest.completedDepth, stats.nodeCount);
}

struct EscapeFixture {
    const char* name;
    const char* fen;
    const char* prefix[2];
    const char* move;
    bool expected;
};

mcumax_position_move Move(const char* text) {
    return {static_cast<uint8_t>((('8' - text[1]) << 4U) + text[0] - 'a'),
            static_cast<uint8_t>((('8' - text[3]) << 4U) + text[2] - 'a'), 0U};
}

void CheckEscapeFixture(fxe::test_support::TestResults& results, const EscapeFixture& fixture) {
    const int failures = results.Failures();
    modules::LitchMaxEngine engine;
    engine.Reset();
    mcumax_position position{};
    bool ready = engine.SetFen(fixture.fen) && mcumax_position_import(&position, &mcumax);
    mcumax_position_undo undo{};
    for (const char* text : fixture.prefix) {
        if (ready && text != nullptr) ready = mcumax_position_make(&position, Move(text), &undo);
    }
    const auto parent = position;
    const auto move = Move(fixture.move);
    ready = ready && mcumax_position_make(&position, move, &undo);
    results.Check(ready, "fixture imports and every requested move uses production legality");
    if (ready) {
        const auto child = position;
        const auto savedUndo = undo;
        unsigned char singleton[sizeof(mcumax)];
        std::memcpy(singleton, &mcumax, sizeof(mcumax));
        const bool escaped = litch_max::classic_chess::internal::EscapesCheaperCapture(position, move, undo);
        results.Check(escaped == fixture.expected, "only an existing legal cheaper-piece capture that is escaped is protected");
        results.Check(litch_max::classic_chess::internal::EscapesCheaperCapture(position, move, undo) == escaped,
                      "repeating the helper has no hidden state");
        results.Check(std::memcmp(&position, &child, sizeof(child)) == 0 &&
                          std::memcmp(&undo, &savedUndo, sizeof(undo)) == 0 &&
                          std::memcmp(&mcumax, singleton, sizeof(singleton)) == 0,
                      "helper preserves all child, undo and legacy singleton bytes");
        mcumax_position_unmake(&position, &undo);
        results.Check(std::memcmp(&position, &parent, sizeof(parent)) == 0,
                      "the caller can still restore the exact parent after probing");
    }
    std::printf("[QUEEN_ESCAPE_HELPER] fixture=%s result=%s\n", fixture.name,
                results.Failures() == failures ? "PASS" : "FAIL");
}

void CheckEscapeLegality(fxe::test_support::TestResults& results) {
    const EscapeFixture FIXTURES[] = {
        {"safe_Qe7", QUEEN_ESCAPE_FEN, {"h7h6", "g5e6"}, "d8e7", true},
        {"safe_Qc8", QUEEN_ESCAPE_FEN, {"h7h6", "g5e6"}, "d8c8", true},
        {"safe_Qb8", QUEEN_ESCAPE_FEN, {"h7h6", "g5e6"}, "d8b8", true},
        {"unsafe_Qe8_Bf7xe8", QUEEN_ESCAPE_FEN, {"h7h6", "g5e6"}, "d8e8", false},
        {"pinned_knight", "4k3/8/4n3/2Q5/8/8/8/4R2K w - - 0 1", {}, "c5b5", false},
        {"multiple_queens", "k7/8/4n3/8/3Q1Q2/8/8/7K w - - 0 1", {}, "d4d3", true},
        {"nonqueen_rook", "k7/8/4n3/8/3R4/8/8/7K w - - 0 1", {}, "d4d3", false},
        {"equal_queen_excluded", "7k/4q3/8/2Q5/8/8/8/7K w - - 0 1", {}, "c5b5", false},
        // This adversarial imported state tests type exclusion only; an adjacent
        // enemy king is not a reachable cheaper-capture threat in legal play.
        {"imported_king_excluded", "8/8/2k5/2Q5/8/8/8/7K w - - 0 1", {}, "c5e5", false},
        {"no_origin_threat_EP", "r3k2r/ppp2ppp/2n5/3pP3/8/2N5/PPP2PPP/R2QK2R w KQkq d6 0 1",
            {}, "d1d2", false},
        {"promoting_pawn_capture", "1q4k1/P7/8/8/8/8/8/4K3 b - - 0 1", {}, "b8c8", true},
        {"pinned_white_knight", "4r1k1/8/8/8/3q4/8/4N3/4K3 b - - 0 1", {}, "d4c4", false},
    };
    for (const auto& fixture : FIXTURES) CheckEscapeFixture(results, fixture);
}

} // namespace

int main() {
    fxe::test_support::TestResults results("CLASSIC_QUEEN_ESCAPE_LMR");
    results.Start();
    CheckCpuQueenEscape(results);
    CheckEscapeLegality(results);
    return results.Finish();
}
