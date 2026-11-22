#include "modules/game_console_commands.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include <litch_max/chess_notation.hpp>
namespace modules {
namespace {
bool ParseBounded(const Console::Args& args, const size_t index, const uint8_t limit, uint8_t& out) {
    if (!args.Has(index) || args.Get(index).empty()) return false;
    uint32_t value = 0U;
    for (const char c : args.Get(index)) {
        if (c < '0' || c > '9') return false;
        value = value * 10U + static_cast<uint32_t>(c - '0');
        if (value > limit) return false;
    }
    out = static_cast<uint8_t>(value);
    return out != 0U;
}
const char* StopName(const ThinkStopReason reason) {
    switch (reason) {
        case ThinkStopReason::Deadline: return "deadline";
        case ThinkStopReason::NodeCap: return "node_cap";
        case ThinkStopReason::Exhausted: return "exhausted";
        case ThinkStopReason::Canceled: return "canceled";
        case ThinkStopReason::Error: return "error";
        default: return "unavailable";
    }
}
} // namespace
bool GameBenchmarkCommand(GameConsoleCommandContext& ctx, const Console::Args& args) {
    if (args.overflow) return false;
    auto& console = *ctx.console;
    auto& coordinator = *ctx.gamePersistence;
    if (args.Equals(2, "start") && args.Has(4) && !args.Has(5)) {
        uint8_t caseId = 0U, level = 0U;
        if (ParseBounded(args, 3U, ENGINE_BENCHMARK_CASE_COUNT, caseId) &&
            ParseBounded(args, 4U, 11U, level)) {
            const bool accepted = coordinator.StartBenchmark(caseId, level);
            console.PrintLn(accepted ? "bench: started compute-only" :
                "bench: rejected (requires stable terminal Classic, stopped clock, quiescent engine)");
            return accepted;
        }
    } else if (args.Equals(2, "stop") && !args.Has(3)) {
        if (!coordinator.IsBenchmarkActive()) {
            console.PrintLn("bench: no active benchmark");
            return true;
        }
        coordinator.StopBenchmark();
        console.PrintLn("bench: cancellation requested; cleanup is asynchronous");
        return true;
    } else if (args.Equals(2, "status") && !args.Has(3)) {
        EngineBenchmarkStatus status;
        if (!coordinator.TryGetBenchmarkStatus(status)) {
            console.PrintLn("bench: unavailable");
            return true;
        }
        const auto& d = status.diagnostics;
        constexpr const char* STATES[] = {"idle", "thinking", "done", "canceled", "error"};
        constexpr const char* ROOTS[] = {"unavailable", "Book", "ClassicPreliminary", "ClassicFull", "DuckCompound", "Seed"};
        constexpr const char* UNITS[] = {"unavailable", "ClassicPawn100", "DuckPawn1000"};
        const auto state = static_cast<uint8_t>(d.state);
        console.PrintLn("bench: compute_only=1 case=%u name=%s level=%u request=%lu active=%u state=%s cancel=%u stop=%s seed=5 history=imported book=none",
            status.caseId, GetEngineBenchmarkCase(status.caseId)->name, status.level,
            static_cast<unsigned long>(d.requestId), d.active, state < 5U ? STATES[state] : "unavailable",
            status.cancelRequested, StopName(d.stopReason));
        const auto printProfile = [&](const char* label, const ThinkProfile& profile) {
            console.PrintLn("bench: %s level=%u time_ceiling_ms=%lu node_ceiling=%lu depth_ceiling=%lu minimum_depth=%u first_root_reserve=%u",
                label, profile.difficulty, static_cast<unsigned long>(profile.maxThinkMs),
                static_cast<unsigned long>(profile.maxNodes), static_cast<unsigned long>(profile.maxDepth),
                profile.minimumDepth, profile.firstRootNodeReserve);
        };
        printProfile("requested", d.requested);
        if (d.effectiveValid) printProfile("effective", d.effective);
        else console.PrintLn("bench: effective=unavailable");
        console.PrintLn("bench: final_nodes_valid=%u final_nodes=%lu", d.finalNodesValid,
            static_cast<unsigned long>(d.finalNodes));
        console.PrintLn("bench: request_ms=%lu preflight=%u/%lu start=%u/%lu poll=%u/%lu first_publication=%u/%lu full=%u/%lu full3=%u/%lu final=%u/%lu quiescent=%u/%lu",
            static_cast<unsigned long>(d.requestAtMs), d.preflightValid, static_cast<unsigned long>(d.preflightAtMs),
            d.startValid, static_cast<unsigned long>(d.startAtMs), d.pollValid, static_cast<unsigned long>(d.pollAtMs),
            d.firstPublicationValid, static_cast<unsigned long>(d.firstPublicationAtMs),
            d.firstFullRootValid, static_cast<unsigned long>(d.firstFullRootAtMs),
            d.firstFull3RootValid, static_cast<unsigned long>(d.firstFull3RootAtMs),
            d.finalValid, static_cast<unsigned long>(d.finalAtMs),
            d.quiescentValid, static_cast<unsigned long>(d.quiescentAtMs));
        const auto& candidate = d.latestPublication;
        UciMove published, completed;
        if (d.firstPublicationValid) (void)litch_max::TryFormatUciMove(candidate.fromSquare,
            candidate.toSquare, candidate.promotion, published);
        if (d.completedRootCount != 0U) (void)litch_max::TryFormatUciMove(d.lastCompletedRoot.fromSquare,
            d.lastCompletedRoot.toSquare, d.lastCompletedRoot.promotion, completed);
        console.PrintLn("bench: latest_move=%s completed_move=%s final_valid=%u final_compute_move=%s novice_selection=omitted move_facts=omitted plan_admission=omitted",
            published.Empty() ? "unavailable" : published.CStr(),
            completed.Empty() ? "unavailable" : completed.CStr(), status.finalMoveValid,
            status.finalMoveValid ? status.finalMove.CStr() : "unavailable");
        console.PrintLn("bench: publication_valid=%u root=%s source=%s depth=%u score_valid=%u score=%d units=%s perspective=%c replay=%u accepted_hint=omitted",
            d.firstPublicationValid,
            static_cast<uint8_t>(candidate.rootKind) < 6U ? ROOTS[static_cast<uint8_t>(candidate.rootKind)] : "unavailable",
            candidate.source == ThinkCandidateSource::Book ? "Book" :
                (candidate.source == ThinkCandidateSource::Seed ? "Seed" : "Search"),
            candidate.completedDepth, candidate.scoreValid, candidate.score, static_cast<uint8_t>(candidate.scoreUnits) < 3U ? UNITS[static_cast<uint8_t>(candidate.scoreUnits)] : "unavailable",
            candidate.scorePerspective == SideToMove::White ? 'w' : 'b', candidate.replayed);
        return true;
    }
    console.PrintLn("usage: game bench start <case 1..6> <CPU level 1..11> | status | stop");
    for (uint8_t id = 1U; id <= ENGINE_BENCHMARK_CASE_COUNT; ++id) {
        console.PrintLn("bench case %u: %s", id, GetEngineBenchmarkCase(id)->name);
    }
    return false;
}
} // namespace modules
