#include <cstdlib>
#include <cstring>

#include "modules/game_console_commands.hpp"

#include "foxie_embedded/millis.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/control_mutation_service.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/game_persistence_scrub.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"

namespace modules {

namespace {

uint32_t ReadVsCpuLevelSetting(const fxe::SettingsStore& settings) {
    uint32_t v = settings.ReadValue("VS_CPU_LEVEL", static_cast<uint32_t>(0U));
    return v;
}

bool TryParseUint32Arg(const Console::Args& args, const size_t index, uint32_t& out) {
    const char* s = args.CStr(index);
    if (!s || s[0] == '\0') {
        return false;
    }

    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (end == s || (end && *end != '\0')) {
        return false;
    }

    out = static_cast<uint32_t>(v);
    return true;
}

size_t AddCompletionMatch(const char* candidate,
                          const char* prefix,
                          const char** outMatches,
                          size_t matchCount,
                          const size_t outMatchesCap) {
    if (!candidate || candidate[0] == '\0') {
        return matchCount;
    }

    if (prefix && prefix[0] != '\0') {
        const size_t prefixLen = std::strlen(prefix);
        if (std::strncmp(candidate, prefix, prefixLen) != 0) {
            return matchCount;
        }
    }

    if (outMatches && matchCount < outMatchesCap) {
        outMatches[matchCount] = candidate;
    }
    return matchCount + 1;
}

template <size_t N>
size_t CompleteChoices(const size_t expectedArgIndex,
                       const size_t argIndex,
                       const char* prefix,
                       const char** outMatches,
                       const size_t outMatchesCap,
                       const char* const (&choices)[N]) {
    if (argIndex != expectedArgIndex) {
        return 0;
    }

    size_t matchCount = 0;
    for (const char* choice : choices) {
        matchCount = AddCompletionMatch(choice, prefix, outMatches, matchCount, outMatchesCap);
    }
    return matchCount;
}

size_t GameComplete(const Console::Args& args,
                    const size_t argIndex,
                    const char* prefix,
                    const char** outMatches,
                    const size_t outMatchesCap) {
    static constexpr const char* kGameSubcommands[] = {
        "status",
        "new",
        "undo",
        "mode",
        "hint",
        "placement",
        "place",
        "scrub",
        "bench",
    };
    static constexpr const char* kModeTargets[] = {
        "human",
        "cpu",
        "0",
        "1",
        "2",
        "3",
        "4",
        "5",
        "6",
        "7",
        "8",
        "9",
        "10",
        "11",
    };
    static constexpr const char* kCpuLevels[] = {
        "1",
        "2",
        "3",
        "4",
        "5",
        "6",
        "7",
        "8",
        "9",
        "10",
        "11",
    };
    static constexpr const char* kHintTargets[] = {
        "stats",
    };

    if (argIndex == 1) {
        return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kGameSubcommands);
    }

    if (args.Equals(1, "mode")) {
        if (argIndex == 2) {
            return CompleteChoices(2, argIndex, prefix, outMatches, outMatchesCap, kModeTargets);
        }
        if (argIndex == 3 && args.Equals(2, "cpu")) {
            return CompleteChoices(3, argIndex, prefix, outMatches, outMatchesCap, kCpuLevels);
        }
        return 0;
    }

    if (!args.Equals(1, "hint")) {
        return 0;
    }

    if (argIndex == 2) {
        return CompleteChoices(2, argIndex, prefix, outMatches, outMatchesCap, kHintTargets);
    }
    return 0;
}

bool SetControlSetting(GameConsoleCommandContext& ctx,
                       const char* key,
                       const uint32_t value) {
    if (ctx.controlMutation == nullptr) {
        ctx.console->PrintLn("FAIL reason=%s",
                             ControlMutationReasonToString(
                                 ControlMutationReason::Unsupported));
        return false;
    }

    StaticJsonDocument<32> doc;
    doc.set(value);
    const ControlMutationOutcome outcome =
        ctx.controlMutation->MutateSetting(
            key, doc.as<JsonVariantConst>());
    const bool ok = outcome.status == ControlMutationStatus::Applied ||
                    outcome.status == ControlMutationStatus::Noop;
    if (ok) {
        ctx.console->PrintLn("OK");
    } else {
        ctx.console->PrintLn("FAIL reason=%s",
                             ControlMutationReasonToString(outcome.reason));
    }
    return ok;
}

bool RunControlAction(GameConsoleCommandContext& ctx,
                      const ControlAction action,
                      const uint8_t undoMoveCount = 1U) {
    if (ctx.controlMutation == nullptr) {
        ctx.console->PrintLn("FAIL id=0 reason=%s",
                             ControlMutationReasonToString(
                                 ControlMutationReason::Unsupported));
        return false;
    }
    const ControlMutationOutcome accepted =
        ctx.controlMutation->MutateAction(
            action, 0U, UiOwner::None, CoachingMutation::Toggle,
            undoMoveCount);
    if (accepted.status != ControlMutationStatus::Accepted) {
        ctx.console->PrintLn(
            "FAIL id=%u reason=%s",
            static_cast<unsigned>(accepted.requestId),
            ControlMutationReasonToString(accepted.reason));
        return false;
    }
    ctx.controlMutation->Update();
    ControlMutationOutcome completion;
    if (!ctx.controlMutation->TryTakeCompletion(
            accepted.requestId, completion)) {
        if (ctx.trackAsyncControlRequest != nullptr) {
            ctx.trackAsyncControlRequest(
                ctx.asyncControlTrackingContext, accepted.requestId);
        }
        ctx.console->PrintLn("ACCEPTED id=%u",
                             static_cast<unsigned>(accepted.requestId));
        return true;
    }
    const bool ok = completion.status == ControlMutationStatus::Applied ||
                    completion.status == ControlMutationStatus::Noop;
    ctx.console->PrintLn(
        "%s id=%u reason=%s", ok ? "OK" : "FAIL",
        static_cast<unsigned>(completion.requestId),
        ControlMutationReasonToString(completion.reason));
    return ok;
}

bool PrintGameScrubReport(GameConsoleCommandContext& ctx) {
    if (!ctx.console || !ctx.settings) {
        return false;
    }

    const GamePersistenceScrubReport& r = GetLastGamePersistenceScrubReport();

    const auto& doc = ctx.settings->Doc();
    const bool hasFen = doc.containsKey(KEY_GAME_FEN);
    const bool hasUndoBaseFen = doc.containsKey(KEY_GAME_UNDO_BASE_FEN);
    const JsonString fen = hasFen ? doc[KEY_GAME_FEN].as<JsonString>() : JsonString(nullptr);
    const JsonString undoBaseFen =
        hasUndoBaseFen ? doc[KEY_GAME_UNDO_BASE_FEN].as<JsonString>() : JsonString(nullptr);

    ctx.console->PrintLn("scrub: runs=%lu removed_fen=%u removed_undo_base_fen=%u",
                         static_cast<unsigned long>(r.runCount),
                         static_cast<unsigned>(r.removedGameFen ? 1U : 0U),
                         static_cast<unsigned>(r.removedUndoBaseFen ? 1U : 0U));
    ctx.console->PrintLn("scrub: present_fen=%u len_fen=%lu",
                         static_cast<unsigned>(hasFen ? 1U : 0U),
                         static_cast<unsigned long>(fen.isNull() ? 0U : fen.size()));
    ctx.console->PrintLn("scrub: present_undo_base_fen=%u len_undo_base_fen=%lu",
                         static_cast<unsigned>(hasUndoBaseFen ? 1U : 0U),
                         static_cast<unsigned long>(undoBaseFen.isNull() ? 0U : undoBaseFen.size()));
    return true;
}

fxe::FixedString<24> DiagnosticInterval(const bool valid, const uint32_t end, const uint32_t start) {
    fxe::FixedString<24> text;
    if (valid) (void)text.Format("%lu", static_cast<unsigned long>(end - start));
    else (void)text.Assign("unavailable");
    return text;
}

void PrintThinkDiagnostics(GameConsoleCommandContext& ctx, const ThinkRequestKind kind, const bool detailed) {
    const char* name = kind == ThinkRequestKind::Cpu ? "cpu" : "hint";
    ThinkDiagnostics data;
    if (!ctx.gamePersistence->TryGetThinkDiagnostics(kind, data)) {
        ctx.console->PrintLn("search: kind=%s active=unavailable status=unavailable", name);
        return;
    }
    constexpr const char* STATES[] = {"Idle", "Thinking", "Done", "Canceled", "Error"};
    const auto state = static_cast<uint8_t>(data.state);
    ctx.console->PrintLn("search: kind=%s request=%lu side=%c duck=%u configured_cpu_level=%s active=%u status=%s retired=%u publications=%lu roots=%s",
        name, static_cast<unsigned long>(data.requestId), data.side == SideToMove::White ? 'w' : 'b',
        static_cast<unsigned>(data.duckSquare), DiagnosticInterval(data.configuredCpuLevel != 0U, data.configuredCpuLevel, 0U).CStr(), data.active ? 1U : 0U,
        state < 5U ? STATES[state] : "unavailable", data.retired ? 1U : 0U,
        static_cast<unsigned long>(data.publicationCount), DiagnosticInterval(data.rootProgressValid, data.completedRootCount, 0U).CStr());
    if (!detailed) return;
    ctx.console->PrintLn("search: kind=%s position_fen=%s", name, ctx.gamePersistence->GetAuthoritativeFen());
    const auto printProfile = [&](const char* label, const ThinkProfile& profile) {
        ctx.console->PrintLn("search: kind=%s %s level=%u time_ceiling_ms=%lu node_ceiling=%lu depth_ceiling=%lu minimum_depth=%u first_root_reserve=%u",
            name, label, static_cast<unsigned>(profile.difficulty), static_cast<unsigned long>(profile.maxThinkMs),
            static_cast<unsigned long>(profile.maxNodes), static_cast<unsigned long>(profile.maxDepth),
            static_cast<unsigned>(profile.minimumDepth), static_cast<unsigned>(profile.firstRootNodeReserve));
    };
    printProfile("requested", data.requested);
    if (data.effectiveValid) printProfile("effective", data.effective);
    else ctx.console->PrintLn("search: kind=%s effective=unavailable", name);
    ctx.console->PrintLn("search: kind=%s request_at_ms=%lu preflight_ms=%s dispatch_ms=%s poll_ms=%s first_worker_ms=%s first_full_ms=%s first_hint_ms=%s final_ms=%s quiescent_ms=%s",
        name, static_cast<unsigned long>(data.requestAtMs),
        DiagnosticInterval(data.preflightValid, data.preflightAtMs, data.requestAtMs).CStr(),
        DiagnosticInterval(data.startValid, data.startAtMs, data.requestAtMs).CStr(),
        DiagnosticInterval(data.pollValid, data.pollAtMs, data.requestAtMs).CStr(),
        DiagnosticInterval(data.firstPublicationValid, data.firstPublicationAtMs, data.requestAtMs).CStr(),
        DiagnosticInterval(data.firstFullRootValid, data.firstFullRootAtMs, data.requestAtMs).CStr(),
        DiagnosticInterval(data.firstAcceptedHintValid, data.firstAcceptedHintAtMs, data.requestAtMs).CStr(),
        DiagnosticInterval(data.finalValid, data.finalAtMs, data.requestAtMs).CStr(),
        DiagnosticInterval(data.quiescentValid, data.quiescentAtMs, data.requestAtMs).CStr());
    ctx.console->PrintLn("search: kind=%s first_full3_ms=%s", name,
        DiagnosticInterval(data.firstFull3RootValid, data.firstFull3RootAtMs, data.requestAtMs).CStr());
    constexpr const char* ROOTS[] = {"unavailable", "Book", "ClassicPreliminary", "ClassicFull", "DuckCompound", "Seed"};
    constexpr const char* UNITS[] = {"unavailable", "ClassicPawn100", "DuckPawn1000"};
    const auto& candidate = data.latestPublication;
    const auto root = static_cast<uint8_t>(candidate.rootKind), units = static_cast<uint8_t>(candidate.scoreUnits);
    UciMove best;
    if (data.firstPublicationValid) (void)litch_max::TryFormatUciMove(candidate.fromSquare, candidate.toSquare, candidate.promotion, best);
    ctx.console->PrintLn("search: kind=%s latest=%s source=%s live_depth=%s offline_depth=%s full_depth=%s preliminary_depth=%s duck_depth=%s replay=%u root_nodes=%s final_nodes=%s",
        name, best.Empty() ? "unavailable" : best.CStr(), root < 6U ? ROOTS[root] : "unavailable",
        DiagnosticInterval(root > 0U && root < 6U, candidate.completedDepth, 0U).CStr(),
        DiagnosticInterval(root > 0U && root < 6U, candidate.offlineDepth, 0U).CStr(),
        DiagnosticInterval(data.rootProgressValid, data.fullDepth, 0U).CStr(),
        DiagnosticInterval(data.rootProgressValid, data.preliminaryDepth, 0U).CStr(),
        DiagnosticInterval(data.rootProgressValid, data.duckDepth, 0U).CStr(), candidate.replayed ? 1U : 0U,
        DiagnosticInterval(root > 0U && root < 6U, candidate.completedNodes, 0U).CStr(),
        DiagnosticInterval(data.finalNodesValid, data.finalNodes, 0U).CStr());
    if (candidate.scoreValid && root > 0U && root < 6U && units > 0U && units < 3U) ctx.console->PrintLn("search: kind=%s score=%ld units=%s perspective=%c source=%s", name,
        static_cast<long>(candidate.score), units < 3U ? UNITS[units] : "unavailable",
        candidate.scorePerspective == SideToMove::White ? 'w' : 'b', root < 6U ? ROOTS[root] : "unavailable");
    else ctx.console->PrintLn("search: kind=%s score=unavailable", name);
}

void PrintPonderDiagnostics(GameConsoleCommandContext& ctx) {
    const CpuPonderStatus status = ctx.gamePersistence->GetPonderStatus();
    ctx.console->PrintLn(
        "ponder: configured=%u active=%u ready=%u starts=%lu hits=%lu misses=%lu cancels=%lu prediction=%s reply=%s full=%u depth=%u nodes_valid=%u nodes=%lu completed_root_nodes=%lu ms=%lu reused_floor=%u",
        status.enabled, status.active, status.ready,
        static_cast<unsigned long>(status.starts), static_cast<unsigned long>(status.hits),
        static_cast<unsigned long>(status.misses), static_cast<unsigned long>(status.cancels),
        status.predictedHumanMove.Empty() ? "unavailable" : status.predictedHumanMove.CStr(),
        status.reply.Empty() ? "unavailable" : status.reply.CStr(), status.fullRoot, status.completedDepth,
        status.nodesValid, static_cast<unsigned long>(status.nodes),
        static_cast<unsigned long>(status.completedRootNodes), static_cast<unsigned long>(status.elapsedMs), status.reusedFloor);
}

bool PrintGameStatus(GameConsoleCommandContext& ctx) {
    const GameVariantStateSnapshot state =
        ctx.gamePersistence->GetGameVariantStateSnapshot();
    char duckSquare[3] = {'-', '-', '\0'};
    if (IsValidDuckSquare(state.duckSquare)) {
        (void)litch_max::TryFormatUciSquare(
            state.duckSquare, duckSquare, sizeof(duckSquare));
    }
    const uint64_t modelHash = runtime::ComputeChessInteractionModelHash(
        ctx.stateMachine->GetChessInteractionModel());
    const uint64_t variantHash = ComputeGameVariantStateHash(state);
    const uint64_t appHash = ComputeAppStateHash(modelHash, state);
    ctx.console->PrintLn(
        "game: variant=%s duck=%s phase=%s pending=%u outcome=%s winner=%s variant_hash=0x%016llx app_hash=0x%016llx",
        GameVariantToString(state.variant), duckSquare,
        DuckTurnPhaseToString(state.duckTurnPhase),
        static_cast<unsigned>(state.pendingChessMove ? 1U : 0U),
        VariantOutcomeReasonToString(state.outcome.reason),
        GameWinnerToString(state.outcome.winner),
        static_cast<unsigned long long>(variantHash),
        static_cast<unsigned long long>(appHash));
    PrintThinkDiagnostics(ctx, ThinkRequestKind::Cpu, false);
    PrintThinkDiagnostics(ctx, ThinkRequestKind::Hint, false);
    PrintPonderDiagnostics(ctx);
    return true;
}

} // namespace

namespace {

bool ResetGameToInitialPosition(GameConsoleCommandContext& ctx) {
    return RunControlAction(ctx, ControlAction::NewGame);
}

} // namespace

bool GameCommand(GameConsoleCommandContext& ctx, const Console::Args& args) {
    if (!ctx.console || !ctx.settings || !ctx.gamePersistence || !ctx.stateMachine) {
        return false;
    }

    const std::string_view sub = args.Has(1) ? args.Get(1) : std::string_view();
    if (sub.empty()) {
        ctx.console->PrintLn("usage: game <status|new|undo|mode|hint|placement|scrub|bench> ...");
        return false;
    }

    if (sub == "bench") return GameBenchmarkCommand(ctx, args);

    if (sub == "status") {
        return PrintGameStatus(ctx);
    }

    if (sub == "new") {
        return ResetGameToInitialPosition(ctx);
    }

    if (sub == "undo") {
        const uint32_t v = ReadVsCpuLevelSetting(*ctx.settings);
        const uint8_t count = (v != 0) ? 2U : 1U;
        return RunControlAction(ctx, ControlAction::Undo, count);
    }

    if (sub == "placement" || sub == "place") {
        // Diagnostic compatibility exception: forced placement is a direct
        // reducer recovery seam, not a persisted setting or one of the
        // board-equivalent Hint/Undo/New Game/Coaching actions.
        const bool ok = ctx.stateMachine->ForceEnterPlacementMode();
        ctx.console->PrintLn(ok ? "OK" : "FAIL");
        return ok;
    }

    if (sub == "scrub") {
        return PrintGameScrubReport(ctx);
    }

    if (sub == "mode") {
        if (!args.Has(2)) {
            const uint32_t v = ReadVsCpuLevelSetting(*ctx.settings);
            if (v == 0) {
                ctx.console->PrintLn("mode: human (VS_CPU_LEVEL=0)");
            } else {
                ctx.console->PrintLn("mode: cpu (VS_CPU_LEVEL=%lu)", static_cast<unsigned long>(v));
            }
            return true;
        }

        if (args.Equals(2, "human")) {
            return SetControlSetting(ctx, "VS_CPU_LEVEL", 0U);
        }

        if (args.Equals(2, "cpu")) {
            uint32_t level = 1;
            if (args.Has(3)) {
                if (!TryParseUint32Arg(args, 3, level)) {
                    ctx.console->PrintLn("usage: game mode <human|cpu [1..11]|0..11>");
                    return false;
                }
            }

            if (level == 0) {
                level = 1;
            }
            if (level > 11) {
                ctx.console->PrintLn("usage: game mode <human|cpu [1..11]|0..11>");
                return false;
            }

            return SetControlSetting(ctx, "VS_CPU_LEVEL", level);
        }

        uint32_t level = 0;
        if (!TryParseUint32Arg(args, 2, level) || level > 11) {
            ctx.console->PrintLn("usage: game mode <human|cpu [1..11]|0..11>");
            return false;
        }

        return SetControlSetting(ctx, "VS_CPU_LEVEL", level);
    }

    if (sub == "hint") {
        if (!args.Has(2)) {
            ctx.console->PrintLn("hint: continuous analysis; first scored hint waits at least 500ms; early completed results are held; search window 90000ms");
            ctx.console->PrintLn("usage: game hint stats");
            return true;
        }
        if (args.Equals(2, "level")) {
            ctx.console->PrintLn("hint: level setting retired; analysis improves while you wait");
            return false;
        }

        if (args.Equals(2, "stats")) {
            UciMove best;
            uint32_t elapsedMs = 0;
            uint32_t nodeCount = 0;
            uint8_t difficulty = 0;
            if (!ctx.gamePersistence->TryGetLastHintThinkStats(best, elapsedMs, nodeCount, difficulty)) {
                ctx.console->PrintLn("hint: no completed hint stats");
            } else {
                ctx.console->PrintLn("hint: last ms=%lu nodes=%lu best=%s",
                                     static_cast<unsigned long>(elapsedMs),
                                     static_cast<unsigned long>(nodeCount), best.CStr());
            }
            PrintThinkDiagnostics(ctx, ThinkRequestKind::Hint, true);
            PrintThinkDiagnostics(ctx, ThinkRequestKind::Cpu, true);
            PrintPonderDiagnostics(ctx);
            return true;
        }

        ctx.console->PrintLn("usage: game hint stats");
        return false;
    }

    ctx.console->PrintLn("usage: game <status|new|undo|mode|hint|placement|scrub|bench> ...");
    return false;
}

void RegisterGameConsoleCommands(Console& console,
                                 SettingsStore& settings,
                                 GamePersistenceCoordinator& gamePersistence,
                                 runtime::GameStateMachine& stateMachine,
                                 GameConsoleCommandContext& ctx,
                                 ControlMutationService* controlMutation) {
    ctx.console = &console;
    ctx.settings = &settings;
    ctx.gamePersistence = &gamePersistence;
    ctx.stateMachine = &stateMachine;
    ctx.controlMutation = controlMutation;

    console.RegisterCommand(
        "game",
        "Game: status|new|undo|mode|hint|placement|scrub|bench",
        [&ctx](const Console::Args& args) { return GameCommand(ctx, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return GameComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        },
        "Usage: game status | new | undo | mode <human|cpu [1..11]|0..11> | "
        "hint stats | placement | scrub | bench <start <case 1..6> <CPU level 1..11>|status|stop>");
    console.RegisterCommand("reset", "Alias: game new", [&ctx](const Console::Args&) {
        return ResetGameToInitialPosition(ctx);
    });
    console.RegisterCommand(
        "fen", "Print current FEN or load a FEN (maximum 159 ASCII bytes)",
        [&ctx](const Console::Args& args) { return FenCommand(ctx, args); });
}

} // namespace modules
