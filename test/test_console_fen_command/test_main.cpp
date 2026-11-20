#include <cstdio>
#include <cstring>
#include <string>

#include "bounded_hint_engine.hpp"
#include "console.hpp"
#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "modules/game_console_commands.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/game_state_machine.hpp"

namespace {
int failures = 0;
void Check(const bool accepted, const char* message) {
    if (!accepted) {
        std::printf("[FAIL] %s\n", message);
        ++failures;
    }
}

// Rules and FEN remain production-owned. Only counters and one explicit
// nonmutating backend rejection are controlled at this import boundary.
class ImportEngine final : public modules::IEngine {
  public:
    void Reset() override { rules.Reset(); }
    bool SetFen(const char* fen) override {
        ++setCalls;
        if (rejectNextImport) {
            rejectNextImport = false;
            return false;
        }
        return rules.SetFen(fen);
    }
    bool GetFen(char* fen, size_t capacity) const override {
        ++fenReads;
        return rules.GetFen(fen, capacity);
    }
    bool ApplyUciMove(const char* move) override { return rules.ApplyUciMove(move); }
    modules::SideToMove GetSideToMove() const override { return rules.GetSideToMove(); }
    bool IsInCheck(modules::SideToMove side) const override { return rules.IsInCheck(side); }
    bool IsInCheckmate(modules::SideToMove side) const override { return rules.IsInCheckmate(side); }
    bool IsInStalemate(modules::SideToMove side) const override { return rules.IsInStalemate(side); }
    bool GetLegalDestinationsMask(uint8_t from, uint64_t& mask) override {
        return rules.GetLegalDestinationsMask(from, mask);
    }
    bool StartThink(const modules::ThinkConfig& config) override { return rules.StartThink(config); }
    void CancelThink() override { ++cancelCalls; rules.CancelThink(); }
    modules::ThinkState PollThinkProgress(uint8_t& progress, modules::UciMove& move) override {
        return rules.PollThinkProgress(progress, move);
    }
    bool GetLastThinkStats(modules::ThinkStats& stats) const override {
        return rules.GetLastThinkStats(stats);
    }
    bool GetLastClassicThinkResult(modules::ClassicThinkResult& result) const override {
        return rules.GetLastClassicThinkResult(result);
    }
    test_support::BoundedHintEngine rules;
    unsigned setCalls = 0U;
    unsigned cancelCalls = 0U;
    mutable unsigned fenReads = 0U;
    bool rejectNextImport = false;
};

struct GameSnapshot {
    std::string fen;
    std::string presentationFen;
    std::string settings;
    uint64_t interactionHash = 0U;
    uint32_t saveCount = 0U;
    uint8_t undoCount = 0U;
    bool requested = false;
    bool hintValid = false;
    uint8_t hintFrom = 255U;
    uint8_t hintTo = 255U;
    bool cpuThinking = false;
    bool hintThinking = false;
    bool queuedCpuMove = false;
};

GameSnapshot Capture(const modules::GamePersistenceCoordinator& coordinator,
                     const modules::SettingsStore& settings,
                     const runtime::GameStateMachine& state) {
    GameSnapshot snapshot;
    snapshot.fen = coordinator.GetAuthoritativeFen();
    snapshot.presentationFen = coordinator.GetPresentationFen();
    serializeJson(settings.Doc(), snapshot.settings);
    snapshot.interactionHash = runtime::ComputeChessInteractionModelHash(
        state.GetChessInteractionModel());
    snapshot.saveCount = settings.GetSaveCount();
    snapshot.undoCount = coordinator.GetUndoHistoryMoveCount();
    snapshot.requested = state.IsHintDisplayRequested();
    snapshot.hintValid = state.TryGetHintMoveSquares(snapshot.hintFrom, snapshot.hintTo);
    snapshot.cpuThinking = state.IsCpuThinkingActive();
    snapshot.hintThinking = state.IsHintThinkingActive();
    snapshot.queuedCpuMove = state.HasQueuedCpuMove();
    return snapshot;
}

bool Equals(const GameSnapshot& a, const GameSnapshot& b) {
    return a.fen == b.fen && a.presentationFen == b.presentationFen &&
        a.settings == b.settings &&
        a.interactionHash == b.interactionHash && a.saveCount == b.saveCount &&
        a.undoCount == b.undoCount && a.requested == b.requested &&
        a.hintValid == b.hintValid && a.hintFrom == b.hintFrom && a.hintTo == b.hintTo &&
        a.cpuThinking == b.cpuThinking && a.hintThinking == b.hintThinking &&
        a.queuedCpuMove == b.queuedCpuMove;
}
} // namespace

int main() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_console_fen_command.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    (void)std::remove("build/test_console_fen_command.json.bak");
    (void)std::remove("build/test_console_fen_command.json.tmp");
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "load isolated settings");
    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 0U;
    stateConfig.placementStableRequiredMs = 0U;
    stateConfig.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine state(stateConfig);
    state.Reset();
    ImportEngine engine;
    modules::GamePersistenceCoordinator coordinator(settings, engine, state);
    Check(coordinator.LoadOrInitialize(), "load actual coordinator");

    MemoryConsoleTransport transport;
    Console console(transport);
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);
    modules::GameConsoleCommandContext context;
    modules::RegisterGameConsoleCommands(console, settings, coordinator, state, context);
    // The Arduino-only legacy entry delegates this exact helper/tail offset.
    // Native tests substitute its registration, not import or game behavior.
    Check(console.RegisterCommand("engine", "legacy alias control",
              [&context](const Console::Args& args) {
                  return args.Equals(1U, "fen") && modules::FenCommand(context, args, 2U);
              }), "register the legacy helper alias");

    state.DebugSetPhysicalOccupancyMask(UINT64_C(0xFFFF00000000FFFF));
    fxe::test_support::RunCommand(console, transport,
        "fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    Check(output.Str().find("OK\r\n") != std::string::npos,
          "pasted six-field FEN reaches the real import owner");
    state.HandleBoardObservation(UINT64_C(1) << 52U,
        UINT64_C(0xFFFF00000000FFFF) & ~(UINT64_C(1) << 52U), 1000U, 1U);
    coordinator.Update();
    const uint64_t afterMove =
        (UINT64_C(0xFFFF00000000FFFF) & ~(UINT64_C(1) << 52U)) | (UINT64_C(1) << 36U);
    fxe::ManualMillis::SetNowMs(1001U);
    state.HandleBoardObservation(UINT64_C(1) << 36U, afterMove, 1001U, 2U);
    state.Tick();
    coordinator.Update();
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "real human move seeds nonempty history before rejected imports");

    // Existing typed presentation/queue setters are controlled supporting
    // seams. This test does not claim a physical CPU/Hint journey or strength.
    state.SetHintMoveSquares(12U, 28U);
    state.SetHintDisplayRequested(true);
    Check(state.QueueCpuMove(12U, 28U, false, 255U, '\0'),
          "typed CPU plan seeds an in-flight queue");
    state.SetCpuThinkingActive(true);
    state.SetHintThinkingActive(true);
    const auto before = Capture(coordinator, settings, state);
    const unsigned setCalls = engine.setCalls;
    const unsigned cancelCalls = engine.cancelCalls;
    const unsigned fenReads = engine.fenReads;
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "fen");
    Check(output.Str().find(before.fen + "\r\n") != std::string::npos &&
              engine.fenReads == fenReads && Equals(before, Capture(coordinator, settings, state)),
          "query prints authoritative cached FEN without engine reads or mutation");

    const char* malformed[] = {
        "not-a-fen", "8/8/8/8/8/8/8/9 w - - 0 1",
        "8/8/8/8/8/8/8/8 x - - 0 1", "8/8/8/8/8/8/8/8 w KK - 0 1",
        "8/8/8/8/8/8/8/8 w - e4 0 1", "8/8/8/8/8/8/8/8 w - - -1 1",
        "8/8/8/8/8/8/8/8 w - - 0 0", "8/8/8/8/8/8/8/8 w - - 0 4294967296",
        "8/8/8/8/8/8/8/8 w - - 0 1 extra",
    };
    for (const char* bad : malformed) {
        Check(!coordinator.LoadExternalFen(bad) &&
                  Equals(before, Capture(coordinator, settings, state)),
              "complete structural rejection preserves game, queue, Hint, history and settings");
    }
    Check(!coordinator.LoadExternalFen(nullptr) && !coordinator.LoadExternalFen(""),
          "null and empty imports reject before any engine work");
    const std::string oversized(160U, 'x');
    Check(!coordinator.LoadExternalFen(oversized.c_str()), "160 bytes reject before mutation");
    Check(!coordinator.LoadExternalFen("8/8/8/8/8/8/8/8 w - - 0 1\t"),
          "C-string control byte rejects before mutation");
    output.Clear();
    fxe::test_support::RunCommand(console, transport,
        "engine fen 8/8/8/8/8/8/8/8 w - - 0 1 extra");
    Check(output.Str().find("too many arguments") != std::string::npos,
          "legacy ninth token is rejected by the actual parser before alias dispatch");
    const std::string nulBytes = std::string("fen ") + before.fen + '\0' + "junk";
    transport.PushBytes(nulBytes.data(), nulBytes.size());
    console.Update();
    fxe::test_support::PressEnter(console, transport);
    fxe::test_support::RunCommand(console, transport,
        "fen 8/8/8/8/8/8/8/8 w - - 0 1\xC3\xA9");
    std::string nulArgument = before.fen + '\0' + "junk";
    Console::Args args;
    args.argc = 2U;
    args.argv[0] = {"fen", 3U};
    args.argv[1] = {nulArgument.data(), static_cast<uint8_t>(nulArgument.size())};
    Check(!modules::FenCommand(context, args),
          "length-aware argument rejects NUL rather than accepting its valid prefix");
    Check(engine.setCalls == setCalls && engine.cancelCalls == cancelCalls &&
              Equals(before, Capture(coordinator, settings, state)),
          "all malformed ingress leaves engine/search and complete state untouched");
    const unsigned malformedSetCalls = engine.setCalls - setCalls;
    const unsigned malformedCancelCalls = engine.cancelCalls - cancelCalls;

    state.SetCpuThinkingActive(false);
    state.SetHintThinkingActive(false);
    const auto beforeBackendFailure = Capture(coordinator, settings, state);
    engine.rejectNextImport = true;
    Check(!coordinator.LoadExternalFen("4k3/8/8/8/8/8/8/4K3 w - - 0 1") &&
              Equals(beforeBackendFailure, Capture(coordinator, settings, state)),
          "nonmutating backend rejection retains queued plan and completed Hint/history");

    const std::string prefix = "4k3/8/8/8/8/8/8/4K3 w - - 0 ";
    const std::string fullmove = "4294967295";
    const std::string maximumFen = prefix + std::string(159U - prefix.size() - fullmove.size(), '0') + fullmove;
    output.Clear();
    fxe::test_support::RunCommand(console, transport, ("fen \"" + maximumFen + "\"").c_str());
    Check(maximumFen.size() == 159U && output.Str().find("OK\r\n") != std::string::npos &&
              coordinator.GetHomePresentationFen() == prefix + fullmove &&
              coordinator.GetUndoHistoryMoveCount() == 0U && !state.HasQueuedCpuMove() &&
              !state.IsHintDisplayRequested(),
          "159-byte complete FEN accepts and performs the existing import reset once");
    const auto accepted = Capture(coordinator, settings, state);
    fxe::test_support::RunCommand(console, transport, ("fen \"" + maximumFen + "0\"").c_str());
    Check(Equals(accepted, Capture(coordinator, settings, state)),
          "160-byte quoted token is rejected without truncating a valid prefix");
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "engine fen");
    Check(output.Str().find(accepted.fen + "\r\n") != std::string::npos,
          "legacy query shares the same authoritative output");
    Check(coordinator.LoadExternalFen("8/8/8/8/8/8/8/8 w - - 0 1"),
          "existing structural policy still permits kingless analysis/setup positions");
    std::printf("[CONSOLE_FEN_COMMAND] failures=%d malformed_set_calls=%u cancel_calls=%u\n",
                failures, malformedSetCalls, malformedCancelCalls);
    return failures == 0 ? 0 : 1;
}
