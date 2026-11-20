#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

#include <mcu-max.h>
#include <mcu-max-position.h>

#include <litch_max/classic_chess_evaluation.hpp>
#include <litch_max/classic_chess_search.hpp>
#include "modules/classic_search_config.hpp"
#include "../../lib/litch-max/src/classic_chess_move_order.hpp"

namespace {
using litch_max::classic_chess::internal::INVALID_MOVE;
using litch_max::classic_chess::internal::IsCapture;
using litch_max::classic_chess::internal::MoveOrder;
using litch_max::classic_chess::internal::MoveSelection;
using litch_max::classic_chess::internal::RecapturePreference;
int failures = 0;

bool Check(bool condition, const char* name, const char* message) {
    if (!condition) { ++failures; std::printf("[FAIL] %s: %s\n", name, message); }
    return condition;
}
bool Same(mcumax_position_move a, mcumax_position_move b) {
    return a.from == b.from && a.to == b.to && a.promotion == b.promotion;
}
mcumax_position_move Move(const char* text) {
    if (text == nullptr) return INVALID_MOVE;
    uint8_t promotion = 0U;
    switch (text[4]) {
        case 'q': promotion = MCUMAX_QUEEN; break;
        case 'r': promotion = MCUMAX_ROOK; break;
        case 'b': promotion = MCUMAX_BISHOP; break;
        case 'n': promotion = MCUMAX_KNIGHT; break;
        default: break;
    }
    return {static_cast<uint8_t>((('8' - text[1]) << 4U) + text[0] - 'a'),
            static_cast<uint8_t>((('8' - text[3]) << 4U) + text[2] - 'a'), promotion};
}
mcumax_position Position(const char* fen) {
    mcumax_init();
    mcumax_set_fen_position(fen);
    mcumax_position position{};
    Check(mcumax_position_import(&position, &mcumax), "import", "real strict position imports");
    return position;
}
uint32_t Encoded(mcumax_position_move move) {
    return (static_cast<uint32_t>(move.from) << 10U) | (static_cast<uint32_t>(move.to) << 3U) | move.promotion;
}
void CheckCoverage(mcumax_position& current, mcumax_position_move preferred, const char* name) {
    const auto before = current;
    for (const auto selection : {MoveSelection::All, MoveSelection::Tactical}) {
        std::vector<uint32_t> expected, actual;
        mcumax_position_iterator iterator{};
        mcumax_position_iterator_init(&iterator);
        mcumax_position_move move{};
        while (mcumax_position_iterator_next(&current, &iterator, &move)) {
            if (selection == MoveSelection::Tactical && !IsCapture(current, move) && move.promotion == 0U) continue;
            mcumax_position_undo undo{};
            if (!mcumax_position_make(&current, move, &undo)) continue;
            expected.push_back(Encoded(move));
            mcumax_position_unmake(&current, &undo);
        }
        MoveOrder order;
        order.Begin(preferred);
        unsigned count = 0U, promotions[8]{};
        while (order.Next(current, move, selection)) {
            if (count++ == 0U && preferred.from != MCUMAX_SQUARE_INVALID)
                Check(Same(move, preferred) && !order.LastMoveWasGenerated(), name,
                      "recapture is first and retains strict preferred-move provenance");
            mcumax_position_undo undo{};
            if (mcumax_position_make(&current, move, &undo)) {
                actual.push_back(Encoded(move));
                if (preferred.promotion != 0U && move.from == preferred.from && move.to == preferred.to)
                    ++promotions[move.promotion];
                mcumax_position_unmake(&current, &undo);
            }
            Check(std::memcmp(&current, &before, sizeof(current)) == 0, name,
                  "each ordered probe restores the same parent before the next move");
        }
        std::sort(expected.begin(), expected.end());
        std::sort(actual.begin(), actual.end());
        Check(expected == actual && std::adjacent_find(actual.begin(), actual.end()) == actual.end(), name,
              "preference preserves the complete strict legal set without omissions or duplicates");
        if (preferred.promotion != 0U)
            Check(promotions[0] == 0U && promotions[MCUMAX_QUEEN] == 1U &&
                      promotions[MCUMAX_ROOK] == 1U && promotions[MCUMAX_BISHOP] == 1U &&
                      promotions[MCUMAX_KNIGHT] == 1U, name,
                  "preferred explicit Queen leaves all four recapture promotions covered exactly once");
    }
}
struct Fixture { const char* name; const char* fen; const char* previous; const char* preferred; };
void CheckFixture(const Fixture& fixture) {
    auto current = Position(fixture.fen);
    const auto previous = Move(fixture.previous);
    mcumax_position_undo previousUndo{};
    if (!Check(mcumax_position_make(&current, previous, &previousUndo), fixture.name,
               "fixture reaches the child through a real strict previous move")) return;
    const auto before = current;
    const auto singleton = mcumax;
    const auto preferred = RecapturePreference(current, previousUndo, previous);
    Check(Same(preferred, Move(fixture.preferred)), fixture.name,
          "targeted geometry returns the expected least valuable legal recapture or no preference");
    Check(std::memcmp(&current, &before, sizeof(current)) == 0 &&
              std::memcmp(&mcumax, &singleton, sizeof(mcumax)) == 0, fixture.name,
          "preference leaves every parent and legacy-engine byte unchanged");
    if (preferred.from != MCUMAX_SQUARE_INVALID) {
        const auto value = [&before](mcumax_position_move move) {
            const uint8_t type = before.board[move.from] & 7U;
            return type == MCUMAX_KING ? 32767 : litch_max::classic_chess::PieceValue(type);
        };
        mcumax_position_iterator iterator{};
        mcumax_position_iterator_init(&iterator);
        mcumax_position_move alternative{};
        while (mcumax_position_iterator_next(&current, &iterator, &alternative)) {
            if (alternative.to != previous.to || !IsCapture(current, alternative)) continue;
            mcumax_position_undo undo{};
            if (!mcumax_position_make(&current, alternative, &undo)) continue;
            Check(value(preferred) <= value(alternative), fixture.name,
                  "independent full generator finds no cheaper legal recapturer");
            mcumax_position_unmake(&current, &undo);
        }
    }
    CheckCoverage(current, preferred, fixture.name);
}
void CheckStaleMetadata() {
    const char* name = "stale-metadata";
    auto current = Position("3r3k/8/8/3P4/4PN2/8/8/7K b - - 0 1");
    const auto previous = Move("d8d5");
    mcumax_position_undo original{};
    if (!Check(mcumax_position_make(&current, previous, &original), name, "real capture supplies compact evidence")) return;
    const auto reject = [&](mcumax_position position, mcumax_position_undo undo, mcumax_position_move move) {
        const auto before = position;
        Check(Same(RecapturePreference(position, undo, move), INVALID_MOVE) &&
                  std::memcmp(&position, &before, sizeof(position)) == 0, name,
              "stale or incomplete last-capture evidence gives no preference and never mutates parent");
    };
    auto undo = original;
    undo.side = current.side; reject(current, undo, previous);
    undo = original; undo.count = 5U; reject(current, undo, previous);
    undo = original; undo.piece[0] = 0U; reject(current, undo, previous);
    undo = original; undo.piece[1] = 0U; reject(current, undo, previous);
    reject(current, original, {MCUMAX_SQUARE_INVALID, previous.to, 0U});
    reject(current, original, {previous.from, MCUMAX_SQUARE_INVALID, 0U});
    auto changed = current; changed.board[previous.to] = 0U; reject(changed, original, previous);
    changed = current; changed.board[previous.to] = current.side | MCUMAX_ROOK; reject(changed, original, previous);
    changed = current; changed.board[previous.to] = original.side | MCUMAX_KING; reject(changed, original, previous);
    changed = current; changed.board[previous.from] = current.side | MCUMAX_KNIGHT; reject(changed, original, previous);

    auto ep = Position("7k/2p5/8/3pP3/8/8/8/7K w - d6 0 1");
    const auto epMove = Move("e5d6");
    mcumax_position_undo epUndo{};
    if (!Check(mcumax_position_make(&ep, epMove, &epUndo) && epUndo.count == 3U,
               name, "real EP capture supplies its third-square victim evidence")) return;
    epUndo.count = 2U;
    reject(ep, epUndo, epMove);
}
void CheckResolverPolicy() {
    using modules::ResolveClassicSearchConfig;
    const char* name = "resolver-policy";
    const litch_max::classic_chess::SearchConfig raw;
    const modules::ThinkConfig defaults;
    const auto resolvedDefault = ResolveClassicSearchConfig(defaults);
    Check(!raw.recaptureOrdering && !resolvedDefault.recaptureOrdering &&
              resolvedDefault.minimumDepth == 1U && resolvedDefault.maxNodes == 180000U &&
              resolvedDefault.maxDepth == 3U && resolvedDefault.firstRootNodeReserve == 0U,
          name, "raw and resolved ordinary defaults keep recapture ordering off and retain their limits");

    // MakeHintThinkConfig is private. Manual Hint and Automatic Hints share
    // this typed continuous-request contract; existing coordinator journeys
    // separately record its actual StartThink request at that boundary.
    auto hint = modules::MakeThinkConfigForVsCpuLevel(11U, 1U);
    hint.continuousAnalysis = true;
    hint.maxThinkMs = 90000U;
    hint.maxNodes = 0U;
    hint.maxDepth = 0U;
    const auto resolvedHint = ResolveClassicSearchConfig(hint);
    Check(resolvedHint.recaptureOrdering && resolvedHint.minimumDepth == modules::CLASSIC_MIN_HINT_PLIES &&
              resolvedHint.minimumDepth == 1U && resolvedHint.maxNodes == std::numeric_limits<uint32_t>::max() &&
              resolvedHint.maxDepth == 31U && resolvedHint.firstRootNodeReserve == 0U &&
              resolvedHint.reversibleHalfmoves == hint.reversibleHalfmoves &&
              hint.difficulty == 11U && hint.maxThinkMs == 90000U,
          name, "continuous human Hint and Coaching enable ordering with scored-root eligibility and unchanged analysis limits");
    hint.maxNodes = 54321U;
    hint.maxDepth = 9U;
    hint.firstRootNodeReserve = 777U;
    hint.reversibleHalfmoves = 42U;
    const auto boundedHint = ResolveClassicSearchConfig(hint);
    Check(boundedHint.recaptureOrdering && boundedHint.maxNodes == 54321U && boundedHint.maxDepth == 9U &&
              boundedHint.minimumDepth == 1U && boundedHint.firstRootNodeReserve == 0U &&
              boundedHint.reversibleHalfmoves == 42U,
          name, "continuous ordering preserves explicit ceilings and clock while refusing a preliminary Hint root");

    for (const uint32_t level : {1U, 3U, 11U}) {
        const auto profile = modules::MakeThinkConfigForVsCpuLevel(level, 5U);
        const auto resolvedProfile = ResolveClassicSearchConfig(profile);
        Check(!profile.continuousAnalysis && !resolvedProfile.recaptureOrdering &&
                  resolvedProfile.minimumDepth == 1U && resolvedProfile.maxNodes == profile.maxNodes &&
                  resolvedProfile.maxDepth == profile.maxDepth &&
                  resolvedProfile.firstRootNodeReserve == profile.firstRootNodeReserve &&
                  resolvedProfile.reversibleHalfmoves == profile.reversibleHalfmoves,
              name, "actual CPU level factory retains ordinary ordering, eligibility and configured work limits");
        for (const uint32_t turn : {0U, 1U, 2U}) {
            const auto cpu = modules::MakeThinkConfigForVsCpuTurn(level, turn, 5U);
            const auto resolvedCpu = ResolveClassicSearchConfig(cpu);
            Check(!cpu.continuousAnalysis && !resolvedCpu.recaptureOrdering &&
                      resolvedCpu.minimumDepth == 1U && resolvedCpu.maxNodes == cpu.maxNodes &&
                      resolvedCpu.maxDepth == cpu.maxDepth &&
                      resolvedCpu.firstRootNodeReserve == cpu.firstRootNodeReserve &&
                      resolvedCpu.reversibleHalfmoves == cpu.reversibleHalfmoves,
                  name, "each actual CPU opening-ramp request keeps ordering off and its existing limits");
        }
    }
    auto boundedCpu = modules::MakeThinkConfigForVsCpuTurn(3U, 2U, 5U);
    boundedCpu.maxNodes = 54321U;
    boundedCpu.maxDepth = 9U;
    boundedCpu.firstRootNodeReserve = 777U;
    boundedCpu.reversibleHalfmoves = 42U;
    const auto resolvedCpu = ResolveClassicSearchConfig(boundedCpu);
    Check(!resolvedCpu.recaptureOrdering && resolvedCpu.minimumDepth == 1U &&
              resolvedCpu.maxNodes == 54321U && resolvedCpu.maxDepth == 9U &&
              resolvedCpu.firstRootNodeReserve == 777U && resolvedCpu.reversibleHalfmoves == 42U,
          name, "bounded ordinary requests preserve ceilings, preliminary reserve and reversible clock");
    std::printf("[CLASSIC_RECAPTURE_POLICY] default=off continuous-Hint-Coaching=on CPU1-3-11=off failures=%d\n", failures);
}

struct HintBudgetPublication {
    unsigned count = 0U;
    modules::ThinkCandidate latest{};
    static void Observe(void* context, const modules::ThinkCandidate& candidate) {
        auto& self = *static_cast<HintBudgetPublication*>(context);
        ++self.count;
        self.latest = candidate;
    }
};

void CheckProductionHintBudget() {
    const char* name = "production-Hint-work-budget";
    const auto root = Position("2r1kb1r/p2q1p2/Q2ppn1p/3P2p1/P7/2N2N1P/1PP2PP1/R1B1R1K1 w k - 0 1");
    const auto before = root;
    const auto singleton = mcumax;
    auto request = modules::MakeThinkConfigForVsCpuLevel(11U, 1U);
    request.continuousAnalysis = true;
    request.maxThinkMs = 90000U;
    request.maxNodes = 330000U;
    request.maxDepth = 5U;
    const auto production = modules::ResolveClassicSearchConfig(request);
    Check(production.recaptureOrdering && production.minimumDepth == 1U &&
              production.maxNodes == 330000U && production.maxDepth == 5U && production.firstRootNodeReserve == 0U,
          name, "production continuous-Hint resolver admits scored roots while retaining the explicit comparison cap");
    litch_max::classic_chess::SearchWorkspace workspace;
    auto controlConfig = production;
    controlConfig.recaptureOrdering = false;
    HintBudgetPublication oldPublication, publication;
    controlConfig.candidateObserver = {&HintBudgetPublication::Observe, &oldPublication};
    const auto control = workspace.Run(root, controlConfig);
    Check(control.available && control.interrupted && control.completedDepth > 0U &&
              control.completedDepth < 5U && oldPublication.count == control.completedDepth &&
              oldPublication.latest.scoreValid &&
              oldPublication.latest.completedDepth == control.completedDepth,
          name, "unordered control retains every completed scored root before its work allowance expires");
    Check(std::memcmp(&root, &before, sizeof(root)) == 0 &&
              std::memcmp(&mcumax, &singleton, sizeof(mcumax)) == 0,
          name, "budget-exhausted control preserves every caller and global-engine byte");
    auto config = production;
    config.candidateObserver = {&HintBudgetPublication::Observe, &publication};
    const auto improved = workspace.Run(root, config);
    Check(improved.available && !improved.interrupted && !improved.terminal && !improved.preliminaryOnly &&
              improved.completedDepth == 5U && improved.score == 521 && Same(improved.move, Move("a6e2")) &&
              improved.nodes <= production.maxNodes && improved.nodes < control.nodes && publication.count == improved.completedDepth &&
              publication.latest.completedDepth == 5U && publication.latest.rootKind == modules::ThinkRootKind::ClassicFull &&
              publication.latest.scoreValid && publication.latest.score == 521 &&
              publication.latest.fromSquare == 16U && publication.latest.toSquare == 52U,
          name, "same allowance publishes successive scored roots and reaches the complete Full5 a6e2 comparison result");
    auto child = root;
    mcumax_position_undo undo{};
    const bool legal = improved.available && mcumax_position_make(&child, improved.move, &undo);
    Check(legal, name, "the newly available Hint is executable through the existing strict rules");
    if (legal) mcumax_position_unmake(&child, &undo);
    Check(std::memcmp(&child, &root, sizeof(root)) == 0 && std::memcmp(&root, &before, sizeof(root)) == 0 &&
              std::memcmp(&mcumax, &singleton, sizeof(mcumax)) == 0,
          name, "successful Hint and strict validation restore the exact root and legacy singleton");
    // Node allowance is a deterministic surrogate for the owner's wait symptom.
    // This does not establish first-visible-Hint latency on the physical S3.
    std::printf("[CLASSIC_RECAPTURE_HINT_BUDGET] cap=%u control_available=%u control_nodes=%u "
                "Hint_available=%u Hint_nodes=%u depth=%u score=%d host-work-surrogate=1 S3-latency=unmeasured\n",
                production.maxNodes, static_cast<unsigned>(control.available), control.nodes,
                static_cast<unsigned>(improved.available), improved.nodes, improved.completedDepth, improved.score);
}

void CheckFullWidthOrderingEquivalence() {
    using namespace litch_max::classic_chess;
    const struct { const char* name; const char* fen; } cases[] = {
        {"full-reported", "2r1kb1r/p2q1p2/Q2ppn1p/3P2p1/P7/2N2N1P/1PP2PP1/R1B1R1K1 w k - 0 1"},
        {"full-checked", "1rb1k1nr/pp1p1ppp/2npp3/qN6/3P1B2/5N2/PPP2PPP/R2QKB1R w KQk - 0 1"},
        {"full-quiet-Queen", "r2qk2r/pQ1b2p1/1p2p3/2p5/2P3n1/7N/PP1N2P1/R3KB1R b KQkq - 1 17"},
        {"full-poisoned-Rook", "r1bk1b1r/ppN2ppp/2n5/4p1q1/2B5/8/PPPP1PPP/R1BQK2R w KQ - 1 9"},
        {"full-save-Knight", "rnbq1rk1/1p3pp1/p3pn1p/3p2N1/N2P4/3B4/PPPQ1PPP/R3K2R w KQ - 0 11"},
        {"full-EP", "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1"},
        {"full-pinned-EP", "8/8/8/r4pPK/8/8/8/4k3 w - f6 0 1"},
        {"full-promotion", "1r5k/P7/8/8/8/8/8/7K w - - 0 1"},
        {"full-King-evasion", "k7/8/2K5/8/8/8/8/R7 b - - 0 1"},
        {"full-Brandon-evasion", "rnbqkb1r/p1ppp2p/5n2/5p1Q/4PP1p/2P5/P1PP2P1/1RB1KBNR b Kkq - 0 1"},
        {"full-checkmate", "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1"},
        {"full-stalemate", "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"},
    };
    SearchWorkspace workspace;
    SearchConfig config;
    config.maxDepth = 3U;
    config.minimumDepth = 3U;
    config.maxNodes = 0U;
    config.cacheMode = CacheMode::Bounds;
    config.principalVariationSearch = true;
    config.quiescenceCache = true;
    config.nullMovePruning = false;
    config.lateQuietReduction = false;
    // Fixed complete roots isolate ordering; no time/node cutoff, preliminary
    // root, or selective pruning can turn changed work into a changed result.
    for (const auto& fixture : cases) {
        const auto root = Position(fixture.fen);
        const auto before = root;
        const auto singleton = mcumax;
        SearchResult reference;
        for (const bool enabled : {false, true}) {
            config.recaptureOrdering = enabled;
            const auto result = workspace.Run(root, config);
            Check(!result.interrupted && result.allocationFailureBytes == 0U &&
                      (result.available || result.terminal) && !result.preliminaryOnly &&
                      (result.terminal || result.completedDepth == config.maxDepth),
                  fixture.name, "same-policy comparison finishes a complete Full3 root or an actual terminal root");
            Check(std::memcmp(&root, &before, sizeof(root)) == 0 &&
                      std::memcmp(&mcumax, &singleton, sizeof(mcumax)) == 0,
                  fixture.name, "both complete searches preserve every caller and global-engine byte");
            if (result.available) {
                auto child = root;
                mcumax_position_undo undo{};
                const bool legal = mcumax_position_make(&child, result.move, &undo);
                Check(legal && !mcumax_position_is_check(&child, root.side), fixture.name,
                      "each selected move is strictly executable and preserves the mover king");
                if (legal) mcumax_position_unmake(&child, &undo);
                Check(std::memcmp(&child, &root, sizeof(root)) == 0, fixture.name,
                      "selected-move validation restores the exact root");
            }
            if (!enabled) reference = result;
            else Check(result.score == reference.score && result.available == reference.available &&
                           result.terminal == reference.terminal && result.completedDepth == reference.completedDepth,
                       fixture.name, "recapture ordering preserves full-root score, availability and terminal outcome");
            // Equal-scoring legal root moves may differ when ordering breaks a
            // tie. Node counts are recorded as work evidence, not equality.
            std::printf("[CLASSIC_RECAPTURE_FULL_WIDTH] case=%s enabled=%u depth=%u score=%d "
                        "available=%u terminal=%u nodes=%u\n", fixture.name, static_cast<unsigned>(enabled),
                        result.completedDepth, result.score, static_cast<unsigned>(result.available),
                        static_cast<unsigned>(result.terminal), result.nodes);
        }
    }
}

} // namespace

int main() {
    CheckResolverPolicy();
    CheckProductionHintBudget();
    const Fixture cases[] = {
        {"pawn-before-Knight", "3r3k/8/8/3P4/4PN2/8/8/7K b - - 0 1", "d8d5", "e4d5"},
        {"pinned-pawn-skipped", "3rr2k/8/8/3P4/4PN2/8/8/4K3 b - - 0 1", "d8d5", "f4d5"},
        {"Bishop-before-Rook-Queen", "3r3k/8/8/3P1Q2/8/8/6B1/3R3K b - - 0 1", "d8d5", "g2d5"},
        {"first-blocker", "3r3k/8/8/3P1Q2/8/5P2/6B1/3R3K b - - 0 1", "d8d5", "d1d5"},
        {"Queen-recapture", "3r3k/8/8/3P1Q2/8/8/8/7K b - - 0 1", "d8d5", "f5d5"},
        {"Bishop-check-escape", "4r2k/8/8/8/8/3B4/4N3/4K3 b - - 0 1", "e8e2", "d3e2"},
        {"King-check-escape", "3r3k/8/8/3P4/3K4/8/8/8 b - - 0 1", "d8d5", "d4d5"},
        {"King-last", "3r3k/8/8/3P1Q2/3K4/8/8/8 b - - 0 1", "d8d5", "f5d5"},
        {"unsafe-King", "b2r3k/8/8/3P4/3K4/8/8/8 b - - 0 1", "d8d5", nullptr},
        {"previous-quiet", "3r3k/8/8/4P3/8/8/8/7K b - - 0 1", "d8d6", nullptr},
        {"previous-castle", "4k2r/8/8/8/8/8/8/K7 b k - 0 1", "e8g8", nullptr},
        {"previous-EP", "7k/2p5/8/3pP3/8/8/8/7K w - d6 0 1", "e5d6", "c7d6"},
        {"recapture-promotions", "1N5k/P7/1r6/8/8/8/8/7K b - - 0 1", "b6b8", "a7b8q"},
        {"previous-promotion-capture", "7k/8/8/8/8/8/2pN4/1R5K b - - 0 1", "c2b1q", "d2b1"},
    };
    for (const auto& fixture : cases) CheckFixture(fixture);
    CheckStaleMetadata();
    CheckFullWidthOrderingEquivalence();
    std::printf("[CLASSIC_RECAPTURE_ORDER] fixtures=%zu failures=%d ordering-policy=Hint-only evidence=host-work S3-latency=unmeasured\n",
                sizeof(cases) / sizeof(cases[0]), failures);
    return failures == 0 ? 0 : 1;
}
