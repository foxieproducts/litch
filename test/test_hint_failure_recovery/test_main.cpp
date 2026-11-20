#include <array>
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

#include <litch_max/chess_notation.hpp>
#include "modules/coach_insight.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/frame_hash.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/led_mapping.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/orientation.hpp"
#include "modules/player_end_palette.hpp"
#include "modules/settings_store.hpp"
#include "modules/ui_color_wheel.hpp"
#include "modules/ui_color_keys.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#include "runtime/square_mapping.hpp"
#include "../../src/trace_replay_frames.hpp"

namespace {

constexpr const char* UNSAFE_ONLY_FEN = "4k3/8/3p4/8/4Q3/8/8/4K3 w - - 0 1";
constexpr const char* SAFE_ALTERNATIVE_FEN = "4k3/8/3p4/8/4Q3/8/P7/4K3 w - - 0 1";
constexpr const char* CHECKMATE_FEN = "7k/6Q1/6K1/8/8/8/8/8 b - - 0 1";
constexpr const char* STALEMATE_FEN = "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1";

enum class TerminalState : uint8_t {
    None,
    Checkmate,
    Stalemate,
};

class FakeHintEngine final : public modules::IEngine {
  public:
    FakeHintEngine(const modules::ThinkState thinkResult,
                   const char* bestMove,
                   const bool includeSafeAlternative,
                   const TerminalState terminalState = TerminalState::None,
                   const bool exposeCoachResult = false)
        : m_thinkResult(thinkResult),
          m_includeSafeAlternative(includeSafeAlternative),
          m_terminalState(terminalState),
          m_exposeCoachResult(exposeCoachResult) {
        (void)m_bestMove.Assign(bestMove);
    }

    void Reset() override {}

    bool SetFen(const char* fen) override { return m_fen.Assign(fen); }

    bool GetFen(char* outFen, const size_t outSize) const override {
        if (outFen == nullptr || outSize == 0U || m_fen.Empty()) {
            return false;
        }
        std::snprintf(outFen, outSize, "%s", m_fen.CStr());
        return true;
    }

    bool ApplyUciMove(const char*) override { return false; }

    modules::SideToMove GetSideToMove() const override {
        return (m_terminalState == TerminalState::None) ? modules::SideToMove::White
                                                        : modules::SideToMove::Black;
    }

    bool IsInCheck(modules::SideToMove) const override {
        return m_terminalState == TerminalState::Checkmate;
    }

    bool IsInCheckmate(modules::SideToMove) const override {
        return m_terminalState == TerminalState::Checkmate;
    }

    bool IsInStalemate(modules::SideToMove) const override {
        return m_terminalState == TerminalState::Stalemate;
    }

    bool GetLegalDestinationsMask(const uint8_t fromSquare, uint64_t& outDestMask64) override {
        outDestMask64 = 0ULL;
        if (m_legalFailuresRemaining != 0U) {
            --m_legalFailuresRemaining;
            return false;
        }
        if (m_terminalState != TerminalState::None) {
            return true;
        }
        if (fromSquare == 36U) {
            outDestMask64 = (1ULL << 28U); // Qe4-e5: legal but hangs the queen to d6 pawn.
        } else if (m_includeSafeAlternative && fromSquare == 48U) {
            outDestMask64 = (1ULL << 40U); // a2-a3: safe deterministic fallback.
        }
        return true;
    }

    bool StartThink(const modules::ThinkConfig& config) override {
        ++m_startCount;
        m_lastDifficulty = config.difficulty;
        m_lastConfig = config;
        m_candidateObserver = config.candidateObserver;
        if (m_staleThinking) {
            return false;
        }
        m_thinking = true;
        m_cancelRequested = false;
        return true;
    }

    void PublishLiveMove(const char* text) {
        litch_max::ParsedUciMove move;
        if (!litch_max::TryParseUciMove(text, move)) { return; }
        modules::ThinkCandidate candidate;
        candidate.fromSquare = move.fromSquare;
        candidate.toSquare = move.toSquare;
        candidate.promotion = move.promotionSuffixLower;
        // This double controls a completed scored root, not a ranked Seed.
        candidate.completedDepth = 1U;
        candidate.rootKind = modules::ThinkRootKind::ClassicFull;
        candidate.scoreUnits = modules::ThinkScoreUnits::ClassicPawn100;
        candidate.scorePerspective = GetSideToMove();
        candidate.scoreValid = true;
        candidate.completedNodes = 1U;
        m_candidateObserver.RootCompleted(candidate);
        m_candidateObserver.Publish(candidate);
    }

    void CancelThink() override {
        ++m_cancelCount;
        m_cancelRequested = true;
    }

    modules::ThinkState PollThinkProgress(uint8_t& outProgressPercent,
                                          modules::UciMove& outBestMoveUci) override {
        ++m_pollCount;
        outProgressPercent = 0U;
        outBestMoveUci.Clear();
        if (m_cancelRequested) {
            m_staleThinking = false;
            m_thinking = false;
            return modules::ThinkState::Canceled;
        }
        if (!m_thinking) {
            return modules::ThinkState::Idle;
        }
        if (m_pollsBeforeDone != 0U) {
            --m_pollsBeforeDone;
            return modules::ThinkState::Thinking;
        }
        m_thinking = false;
        outProgressPercent = 100U;
        outBestMoveUci = m_bestMove;
        if (m_thinkResult == modules::ThinkState::Done) {
            PublishLiveMove(m_bestMove.CStr());
        }
        return m_thinkResult;
    }

    bool GetLastThinkStats(modules::ThinkStats& outStats) const override {
        outStats.nodeCount = 1U;
        return true;
    }

    bool GetLastClassicThinkResult(
        modules::ClassicThinkResult& outResult) const override {
        outResult = modules::ClassicThinkResult{};
        litch_max::ParsedUciMove move;
        if (!m_exposeCoachResult ||
            m_thinkResult != modules::ThinkState::Done ||
            !litch_max::TryParseUciMove(m_bestMove.CStr(), move)) {
            return false;
        }
        outResult.candidates[0] = {
            move.fromSquare,
            move.toSquare,
            move.hasPromotion ? move.promotionSuffixLower : '\0',
        };
        outResult.candidateCount = 1U;
        outResult.bestScoreValid = true;
        outResult.bestMoveFacts = modules::ClassicThinkOutcomeKnown;
        return true;
    }

    void SetLegalFailures(const uint8_t count) { m_legalFailuresRemaining = count; }
    void SetStaleThinking() { m_staleThinking = true; }
    void SetPollsBeforeDone(const uint8_t count) { m_pollsBeforeDone = count; }
    void SetThinkResult(const modules::ThinkState result) { m_thinkResult = result; }

    uint32_t StartCount() const { return m_startCount; }
    uint32_t CancelCount() const { return m_cancelCount; }
    uint32_t PollCount() const { return m_pollCount; }
    uint8_t LastDifficulty() const { return m_lastDifficulty; }
    bool HasContinuousSearchBudget() const {
        return m_lastConfig.continuousAnalysis && m_lastConfig.maxNodes == 0U &&
            m_lastConfig.maxDepth == 0U && m_lastConfig.maxThinkMs > 0U &&
            m_lastConfig.maxThinkMs <= 90000U;
    }

  private:
    modules::FenString m_fen;
    modules::UciMove m_bestMove;
    modules::ThinkState m_thinkResult = modules::ThinkState::Error;
    bool m_includeSafeAlternative = false;
    TerminalState m_terminalState = TerminalState::None;
    bool m_exposeCoachResult = false;
    uint8_t m_legalFailuresRemaining = 0U;
    uint8_t m_pollsBeforeDone = 0U;
    bool m_staleThinking = false;
    bool m_thinking = false;
    bool m_cancelRequested = false;
    uint32_t m_startCount = 0U;
    uint32_t m_cancelCount = 0U;
    uint32_t m_pollCount = 0U;
    uint8_t m_lastDifficulty = 0U;
    modules::ThinkConfig m_lastConfig{};
    modules::ThinkCandidateObserver m_candidateObserver{};
};

// Real chess rules/position, with only the asynchronous search-result boundary
// controlled by this test. Native LitchMaxEngine otherwise finishes in one poll.
class DeferredCastleHintEngine final : public modules::IEngine {
  public:
    explicit DeferredCastleHintEngine(const char* bestMove) : m_bestMove(bestMove) {}
    void Reset() override { m_engine.Reset(); }
    bool SetFen(const char* fen) override { return m_engine.SetFen(fen); }
    bool GetFen(char* fen, size_t size) const override {
        ++fenReads;
        return m_engine.GetFen(fen, size);
    }
    bool ApplyUciMove(const char* move) override { return m_engine.ApplyUciMove(move); }
    modules::SideToMove GetSideToMove() const override { return m_engine.GetSideToMove(); }
    bool IsInCheck(modules::SideToMove side) const override { return m_engine.IsInCheck(side); }
    bool IsInCheckmate(modules::SideToMove side) const override { return m_engine.IsInCheckmate(side); }
    bool IsInStalemate(modules::SideToMove side) const override { return m_engine.IsInStalemate(side); }
    bool GetLegalDestinationsMask(uint8_t from, uint64_t& mask) override {
        return m_engine.GetLegalDestinationsMask(from, mask);
    }
    bool StartThink(const modules::ThinkConfig&) override {
        ++starts;
        active = true;
        released = false;
        return true;
    }
    void CancelThink() override {
        ++cancels;
        active = false;
    }
    modules::ThinkState PollThinkProgress(uint8_t& progress, modules::UciMove& move) override {
        ++polls;
        progress = 0U;
        move.Clear();
        if (!active) return modules::ThinkState::Idle;
        if (!released) return modules::ThinkState::Thinking;
        progress = 100U;
        (void)move.Assign(m_bestMove);
        active = false;
        ++resultsConsumed;
        return modules::ThinkState::Done;
    }
    bool GetLastThinkStats(modules::ThinkStats& stats) const override {
        stats.nodeCount = 1U;
        return true;
    }
    void ReleaseResult() { released = true; }
    uint32_t starts = 0U;
    uint32_t cancels = 0U;
    uint32_t polls = 0U;
    uint32_t resultsConsumed = 0U;
    mutable uint32_t fenReads = 0U;

  private:
    modules::LitchMaxEngine m_engine;
    const char* m_bestMove;
    bool active = false;
    bool released = false;
};

struct HintCaseResult {
    uint32_t startCount = 0U;
    bool hintValid = false;
    uint8_t fromSquare = 0U;
    uint8_t toSquare = 0U;
};

runtime::GameStateMachineConfig MakeStateMachineConfig() {
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 0U;
    cfg.cpuIdleDelayMs = 999999U;
    cfg.placementStableRequiredMs = 0U;
    return cfg;
}

void ObserveExpectedBoard(runtime::GameStateMachine& sm,
                          modules::GamePersistenceCoordinator& coordinator) {
    runtime::InteractionDispatcher dispatcher(sm, coordinator);
    const uint64_t occupied = sm.GetExpectedOccupancyMask();
    (void)dispatcher.DispatchBoardObservation({
        fxe::Millis(), sm.GetLastBoardObservationSequence() + 1U,
        sm.GetPhysicalOccupancyMask() ^ occupied, occupied});
}

void RemoveSettingsFiles(const char* path) {
    (void)std::remove(path);
    modules::FenString sidecar;
    if (sidecar.Format("%s.bak", path)) {
        (void)std::remove(sidecar.CStr());
    }
    if (sidecar.Format("%s.tmp", path)) {
        (void)std::remove(sidecar.CStr());
    }
}

HintCaseResult RunCompletedHintCase(const char* path,
                                    const char* fen,
                                    const modules::ThinkState resultState,
                                    const char* bestMove,
                                    const bool includeSafeAlternative) {
    RemoveSettingsFiles(path);
    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = path;
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();
    settings.Doc()[modules::KEY_GAME_FEN] = fen;
    // Exercise production result validation with scripted completed roots.
    // Real-engine/async journeys own calculation and first-reveal timing.

    FakeHintEngine engine(resultState, bestMove, includeSafeAlternative);
    runtime::GameStateMachine sm(MakeStateMachineConfig());
    sm.Reset();
    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    (void)coord.LoadOrInitialize();
    ObserveExpectedBoard(sm, coord);
    coord.Update();
    fxe::ManualMillis::AdvanceMs(500U);
    coord.Update();

    HintCaseResult out;
    out.startCount = engine.StartCount();
    out.hintValid = sm.TryGetHintMoveSquares(out.fromSquare, out.toSquare);
    return out;
}

void CheckHintCase(fxe::test_support::TestResults& results,
                   const HintCaseResult& actual,
                   const uint8_t expectedFrom,
                   const uint8_t expectedTo,
                   const char* message) {
    results.Check(actual.startCount == 1U, "hint calculation starts exactly once");
    results.Check(actual.hintValid, message);
    results.Check(actual.fromSquare == expectedFrom, "calculated hint origin matches engine result");
    results.Check(actual.toSquare == expectedTo, "calculated hint destination matches engine result");
}

void CheckRejectedHintCase(fxe::test_support::TestResults& results,
                           const HintCaseResult& actual, const char* message) {
    results.Check(!actual.hintValid, message);
    results.Check(actual.startCount == 2U,
                  "missing calculated result retries after backoff instead of inventing a legal move");
}

void CheckTerminalPosition(fxe::test_support::TestResults& results,
                           const char* path,
                           const char* fen,
                           const TerminalState terminalState) {
    RemoveSettingsFiles(path);
    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = path;
    modules::SettingsStore settings(settingsCfg);
    results.Check(settings.Load(), "terminal settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = fen;

    FakeHintEngine engine(modules::ThinkState::Error, "", false, terminalState);
    runtime::GameStateMachine sm(MakeStateMachineConfig());
    sm.Reset();
    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    results.Check(coord.LoadOrInitialize(), "terminal coordinator load");
    ObserveExpectedBoard(sm, coord);
    coord.Update();
    coord.Update();
    coord.Update();

    uint8_t fromSquare = 0U;
    uint8_t toSquare = 0U;
    results.Check(engine.StartCount() == 0U, "game-over position never starts hint analysis");
    results.Check(!sm.TryGetHintMoveSquares(fromSquare, toSquare), "game-over position has no hint");
}

struct DeferredCastleCase {
    const char* name;
    const char* fen;
    const char* expectedFen;
    const char* move;
    uint8_t kingFrom;
    uint8_t kingTo;
    uint8_t rookFrom;
    uint8_t rookTo;
    bool black;
};

void CheckResultReadyAtCastleCommitIsRejected(
    fxe::test_support::TestResults& results,
    const DeferredCastleCase& fixture) {
    constexpr const char* PATH =
        "build/test_hint_result_ready_at_castle_commit.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "stale-result castle settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = fixture.fen;

    fxe::ManualMillis::SetNowMs(1000U);
    DeferredCastleHintEngine engine(fixture.move);
    runtime::GameStateMachine sm(MakeStateMachineConfig());
    sm.Reset();
    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    results.Check(coord.LoadOrInitialize(),
                  "stale-result castle coordinator loads captured FEN");

    const uint64_t before = sm.GetExpectedOccupancyMask();
    runtime::InteractionDispatcher dispatcher(sm, coord);
    uint32_t sequence = 0U;
    uint64_t observed = 0ULL;
    const auto observe = [&](const uint64_t occupancy) {
        sequence = dispatcher.DispatchBoardObservation({
            fxe::Millis(), sequence + 1U, observed ^ occupancy, occupancy});
        observed = occupancy;
        coord.Update();
    };

    observe(before);
    coord.Update();
    results.Check(engine.starts == 1U && sm.IsHintThinkingActive(),
                  "one long-running hint starts from position X");

    uint64_t physical = before & ~(UINT64_C(1) << fixture.kingFrom);
    observe(physical);
    physical |= UINT64_C(1) << fixture.kingTo;
    observe(physical);
    physical &= ~(UINT64_C(1) << fixture.rookFrom);
    observe(physical);
    results.Check(
        sm.GetChessInteractionModel().phase.requiredAction ==
            runtime::RequiredPhysicalAction::PlaceSecondary &&
            sm.GetExpectedOccupancyMask() == before,
        "castle remains one uncommitted transaction while Hint X is active");

    // The old result becomes ready immediately before the Hall observation
    // which commits the castle. Update() must apply the physical transaction
    // first, recognize the side/position boundary, and cancel the old search
    // without ever consuming or projecting its X-derived result as Y.
    engine.ReleaseResult();
    physical |= UINT64_C(1) << fixture.rookTo;
    observe(physical);

    uint8_t hintFrom = 0U;
    uint8_t hintTo = 0U;
    const bool hintPublished =
        sm.TryGetHintMoveSquares(hintFrom, hintTo);
    const bool accepted =
        engine.resultsConsumed == 0U && engine.cancels != 0U &&
        !sm.IsHintThinkingActive() && !hintPublished &&
        coord.GetUndoHistoryMoveCount() == 1U &&
        std::strcmp(coord.GetAuthoritativeFen(), fixture.expectedFen) == 0 &&
        sm.GetExpectedOccupancyMask() == physical &&
        sm.GetPhysicalOccupancyMask() == physical &&
        sm.GetChessInteractionModel().phase.tag ==
            runtime::ChessInteractionPhase::Idle &&
        coord.GetEngineDerivedState().sideToMove ==
            (fixture.black ? modules::SideToMove::White
                           : modules::SideToMove::Black);
    results.Check(
        accepted,
        "result made ready at castle commit is rejected before it can mutate position Y");
    std::printf(
        "[STALE_HINT_CASTLE_COMMIT] case=%s events=%lu "
        "start_fen=%s result=%s result_consumed=%lu cancels=%lu "
        "history=%u physical=%016llx expected=%016llx hint=%u "
        "phase=%s side=%s authoritative_fen=%s accepted=%u\n",
        fixture.name,
        static_cast<unsigned long>(sequence),
        fixture.fen,
        fixture.move,
        static_cast<unsigned long>(engine.resultsConsumed),
        static_cast<unsigned long>(engine.cancels),
        static_cast<unsigned>(coord.GetUndoHistoryMoveCount()),
        static_cast<unsigned long long>(sm.GetPhysicalOccupancyMask()),
        static_cast<unsigned long long>(sm.GetExpectedOccupancyMask()),
        hintPublished ? 1U : 0U,
        runtime::ChessInteractionPhaseToString(
            sm.GetChessInteractionModel().phase.tag),
        coord.GetEngineDerivedState().sideToMove ==
                modules::SideToMove::White
            ? "White"
            : "Black",
        coord.GetAuthoritativeFen(),
        accepted ? 1U : 0U);
}

void CheckDeferredHintDuringCastle(fxe::test_support::TestResults& results,
                                  const DeferredCastleCase& fixture,
                                  const bool firstPlayerIsP2,
                                  const bool boardRotate) {
    const char* path = "build/test_hint_pending_captured_castle.json";
    RemoveSettingsFiles(path);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "deferred castle settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = fixture.fen;
    settings.Doc()["GAME_FIRST_PLAYER"] = firstPlayerIsP2 ? 2U : 1U;
    settings.Doc()["BOARD_ROTATE"] = boardRotate;
    // Fixed reference storage slots avoid coupling this regression to legacy
    // palette migration during an empty-store boot at a different placement.
    settings.Doc()[modules::ui_color_keys::P1_END_PIECE_WHEEL] = 170U;
    settings.Doc()[modules::ui_color_keys::P2_END_PIECE_WHEEL] = 0U;
    modules::player_end_palette::SynchronizeLegacyAliases(settings.Doc());
    const bool squareRotate = modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2);
    fxe::ManualMillis::SetNowMs(1000U);
    DeferredCastleHintEngine engine(fixture.move);
    runtime::GameStateMachine sm(MakeStateMachineConfig());
    sm.Reset();
    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    results.Check(coord.LoadOrInitialize(), "deferred castle coordinator loads captured FEN");
    const uint64_t before = sm.GetExpectedOccupancyMask();
    const uint64_t kingPlaced =
        (before & ~(UINT64_C(1) << fixture.kingFrom)) |
        (UINT64_C(1) << fixture.kingTo);
    runtime::InteractionDispatcher dispatcher(sm, coord);
    uint32_t sequence = 0U;
    uint64_t observed = 0ULL;
    const auto observe = [&](const uint64_t occupancy) {
        sequence = dispatcher.DispatchBoardObservation({
            fxe::Millis(), sequence + 1U, observed ^ occupancy, occupancy});
        observed = occupancy;
        coord.Update();
    };
    observe(before);
    coord.Update();
    uint64_t legalKingDestinations = 0ULL;
    results.Check(engine.GetLegalDestinationsMask(fixture.kingFrom, legalKingDestinations) &&
                      (legalKingDestinations & (UINT64_C(1) << fixture.kingTo)) != 0ULL,
                  "real engine confirms the fixture castle is legal");
    results.Check(engine.starts == 1U && sm.IsHintThinkingActive(),
                  "production coordinator owns one unfinished background hint");

    observe(before & ~(UINT64_C(1) << fixture.kingFrom));
    fxe::ManualMillis::AdvanceMs(200U);
    dispatcher.DispatchTick(fxe::Millis());
    observe(kingPlaced);

    bool sawRookPieceOn = false;
    bool sawRookPieceOff = false;
    const auto checkpoint = [&](const char* stage) {
        const uint32_t readsBeforeRender = engine.fenReads;
        runtime::InteractionModelSnapshotBuildInputs snapshotInputs;
        snapshotInputs.stateMachine = &sm;
        snapshotInputs.derived = &coord.GetEngineDerivedState();
        snapshotInputs.authoritativeFen = coord.GetAuthoritativeFen();
        snapshotInputs.gameClock = &coord.GetGameClock();
        const auto snapshot = runtime::BuildInteractionModelSnapshot(snapshotInputs);
        modules::led_frame_composer::ComposeInputs inputs;
        inputs.settings = &settings;
        inputs.interactionSnapshot = &snapshot;
        inputs.firstPlayerIsP2 = firstPlayerIsP2;
        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> frame{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> edge{};
        std::array<modules::Rgb, 64> grid{};
        modules::led_frame_composer::Compose(inputs, frame, edge, grid);
        const auto replayFrame = trace::replay_internal::ComputeComposedFrameHashes(
            sm, &coord.GetEngineDerivedState(), &coord,
            boardRotate, firstPlayerIsP2, squareRotate);
        const bool replayParity = replayFrame.board ==
            fxe::HashRgbFrame(frame.data(), hardware::BOARD_LED_COUNT);
        results.Check(replayParity,
                      "coordinator-backed replay preserves moved-piece ownership and castle prompts");
        uint32_t edgeEnergy = 0U;
        for (const auto color : edge) edgeEnergy += color.r + color.g + color.b;
        results.Check(edgeEnergy == 0U,
                      "legal castling keeps the Placement gameplay perimeter quiet");
        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> physical{};
        for (uint16_t index = 0U; index < hardware::BOARD_LED_COUNT; ++index) {
            physical[hardware::MapLedIndexForSquareAndEdgeRotate(
                index, squareRotate, boardRotate)] = frame[index];
        }
        results.Check(engine.fenReads == readsBeforeRender,
                      "snapshot and render never query the mutable search engine");
        const auto colorAt = [&](uint8_t square, hardware::SquareLedSlot slot) {
            const uint8_t physicalSquare = runtime::MapSquareIndex(square, squareRotate);
            return physical[hardware::LedIndexForSquare(physicalSquare, slot)];
        };
        const auto lit = [&](uint8_t square, hardware::SquareLedSlot slot) {
            const auto color = colorAt(square, slot);
            return color.r != 0U || color.g != 0U || color.b != 0U;
        };
        const auto& phase = sm.GetChessInteractionModel().phase;
        const bool plan = phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
            phase.plan.kind == runtime::PhysicalTransitionKind::Castling &&
            phase.requiredAction == runtime::RequiredPhysicalAction::LiftSecondary &&
            phase.plan.progress.relocationsPlacedMask == 1U &&
            phase.pendingActionToken == 0U &&
            sm.GetExpectedOccupancyMask() == before;
        const bool king = lit(fixture.kingTo, hardware::SquareLedSlot::Piece);
        sawRookPieceOn |= lit(fixture.rookFrom, hardware::SquareLedSlot::Piece);
        sawRookPieceOff |= !lit(fixture.rookFrom, hardware::SquareLedSlot::Piece);
        const auto squareColor = [&](const uint8_t square) {
            return colorAt(square, hardware::SquareLedSlot::Square0);
        };
        const auto sameColor = [](const modules::Rgb a, const modules::Rgb b) {
            return a.r == b.r && a.g == b.g && a.b == b.b;
        };
        // Empty a6/b6 supply the same checkerboard backgrounds as either rook
        // endpoint. Nonzero alone falsely accepts that background as guidance.
        const auto referenceSquare = [](const uint8_t square) {
            return static_cast<uint8_t>(((square / 8U + square % 8U) & 1U) != 0U ? 17U : 16U);
        };
        const uint8_t ownerWheel = (fixture.black != firstPlayerIsP2) ? 0U : 170U;
        const auto moverPiece = modules::ui_color_wheel::WheelToRgb(ownerWheel);
        const auto expectedKing = modules::ScaleRgbSaturatingBy256(
            modules::ScaleRgbSaturatingBy256(modules::ui_color_wheel::WheelToRgb(ownerWheel), 304U), 320U);
        const bool kingOwner = sameColor(colorAt(fixture.kingTo, hardware::SquareLedSlot::Piece), expectedKing);
        const auto logicalSteady = fixture.rookTo % 8U < fixture.rookFrom % 8U
            ? hardware::SquareLedSlot::Square0 : hardware::SquareLedSlot::Square2;
        const auto steady = squareRotate ? hardware::RotateSquareLedSlot180(logicalSteady) : logicalSteady;
        const auto travel = hardware::RotateSquareLedSlot180(steady);
        // The unfinished human castle uses its physical interaction clock.
        // Each horizontal cell has an incoming and outgoing rail. Untouched
        // occupied-source rails retain full Active; only the selected head
        // blends its floor toward the actual mover. The empty target also
        // receives the shared half-strength head envelope.
        const int fileDelta = int(fixture.rookTo % 8U) - int(fixture.rookFrom % 8U);
        const uint32_t distance = static_cast<uint32_t>(fileDelta < 0 ? -fileDelta : fileDelta);
        const uint32_t travelMs = 480U * distance < 900U ? 900U : 480U * distance;
        const uint32_t phaseMs = snapshot.interaction.lastInteractionElapsedMs % (travelMs + 240U);
        const uint32_t travelPhaseMs = phaseMs < travelMs ? phaseMs : travelMs;
        const uint32_t progress = travelPhaseMs * (2U * distance + 1U) * 256U / travelMs;
        const uint32_t head = progress / 256U;
        const uint16_t fraction = static_cast<uint16_t>(progress % 256U);
        const auto endpoint = [&](const uint8_t square) {
            const bool source = square == fixture.rookFrom;
            const auto role = modules::ui_color_wheel::WheelToRgb(
                static_cast<uint8_t>(ownerWheel + (source ? 32U : 16U)));
            const auto floor = modules::ScaleRgbSaturatingBy256(role, 96U);
            const uint32_t incomingNode = source ? 0U : 2U * distance;
            const auto weightAt = [&](const uint32_t node) -> uint16_t {
                return node == head ? 256U - fraction : node == head + 1U ? fraction : 0U;
            };
            const uint16_t incomingWeight = weightAt(incomingNode);
            const uint16_t outgoingWeight = weightAt(incomingNode + 1U);
            const auto expectedRail = [&](const bool incoming) {
                const uint16_t weight = incoming ? incomingWeight : outgoingWeight;
                auto color = source && weight == 0U ? role : modules::AlphaBlendRgb(
                    floor, moverPiece, static_cast<uint8_t>(weight * 255U / 256U));
                if (!source) {
                    const uint16_t strongest = incomingWeight > outgoingWeight ? incomingWeight : outgoingWeight;
                    const uint16_t envelope = strongest * 127U / 256U;
                    color = modules::AlphaBlendRgb(color, moverPiece, static_cast<uint8_t>(envelope));
                }
                return modules::ScaleRgbSaturatingBy256(color, 320U);
            };
            return !sameColor(colorAt(square,steady),squareColor(referenceSquare(square))) &&
                !sameColor(colorAt(square,travel),squareColor(referenceSquare(square))) &&
                lit(square,steady) && lit(square,travel) &&
                sameColor(colorAt(square,steady), expectedRail(false)) &&
                sameColor(colorAt(square,travel), expectedRail(true));
        };
        const bool rookFrom = endpoint(fixture.rookFrom);
        const bool rookTo = endpoint(fixture.rookTo);
        results.Check(plan, "deferred hint preserves authoritative unfinished castle");
        results.Check(king && kingOwner, "background hint preserves the placed king and its player color");
        results.Check(rookFrom && rookTo, "background hint preserves exact full-active source rails and selected mover-head endpoint roles");
        results.Check(engine.starts == 1U, "physical castle never starts replacement hint work");
        std::printf("[DEFERRED_HINT_CASTLE] case=%s first=%u rotate=%u stage=%s hint=%u polls=%lu consumed=%lu "
                    "plan=%u king=%u owner=%u rook_from=%u rook_to=%u replay_parity=%u edge_energy=%lu accepted=%u\n",
                    fixture.name, firstPlayerIsP2 ? 2U : 1U, boardRotate, stage,
                    sm.IsHintThinkingActive(), static_cast<unsigned long>(engine.polls),
                    static_cast<unsigned long>(engine.resultsConsumed), plan, king, kingOwner, rookFrom, rookTo,
                    replayParity, static_cast<unsigned long>(edgeEnergy),
                    plan && king && kingOwner && rookFrom && rookTo && replayParity && edgeEnergy == 0U);
    };
    checkpoint("worker-running-king-placed");
    fxe::ManualMillis::AdvanceMs(200U);
    dispatcher.DispatchTick(fxe::Millis());
    coord.Update();
    checkpoint("worker-running-rook-pulse");
    fxe::ManualMillis::AdvanceMs(400U);
    dispatcher.DispatchTick(fxe::Millis());
    coord.Update();
    checkpoint("worker-running-bloom-expired");
    results.Check(sawRookPieceOn && sawRookPieceOff,
                  "prompted rook visibly pulses while its relocation is awaited");

    engine.ReleaseResult();
    for (uint8_t i = 0U; i < 3U; ++i) {
        fxe::ManualMillis::AdvanceMs(100U);
        dispatcher.DispatchTick(fxe::Millis());
        coord.Update();
    }
    checkpoint("worker-done-rook-still-awaited");
    results.Check(engine.resultsConsumed == 1U && !sm.IsHintThinkingActive(),
                  "completed hint is consumed while the castle still awaits its rook");
    fxe::ManualMillis::AdvanceMs(4100U);
    dispatcher.DispatchTick(fxe::Millis());
    coord.Update();
    checkpoint("worker-consumed-five-second-rook-wait");
    observe(kingPlaced & ~(UINT64_C(1) << fixture.rookFrom));
    results.Check(sm.GetChessInteractionModel().phase.requiredAction ==
                      runtime::RequiredPhysicalAction::PlaceSecondary,
                  "rook pickup advances to its destination after hint completion");
    const uint64_t completed = (kingPlaced & ~(UINT64_C(1) << fixture.rookFrom)) |
        (UINT64_C(1) << fixture.rookTo);
    observe(completed);
    results.Check(sm.GetExpectedOccupancyMask() == completed &&
                      sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle &&
                      (coord.GetEngineDerivedState().sideToMove == modules::SideToMove::Black) != fixture.black,
                  "rook placement completes the same castle and hands over the turn");
}

} // namespace

int main() {
    fxe::test_support::TestResults results("HINT_FAILURE_RECOVERY");
    results.Start();
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);

    const DeferredCastleCase captured{
        "captured-white-kingside",
        "r3k2r/pppbq1pp/2n2p1n/2bpp3/2P1PB2/2NP1Q1N/PP2BPPP/R3K2R w KQkq - 0 1",
        "r3k2r/pppbq1pp/2n2p1n/2bpp3/2P1PB2/2NP1Q1N/PP2BPPP/R4RK1 b kq - 0 1",
        "e1g1", 60U, 62U, 63U, 61U, false};
    CheckResultReadyAtCastleCommitIsRejected(results, captured);
    CheckDeferredHintDuringCastle(results, captured, false, false);
    // Existing verified special-move journey fixtures; only search timing differs.
    const std::array<DeferredCastleCase, 4> castles{{
        {"white-kingside", "4k3/8/8/8/8/8/8/4K2R w K - 0 1", "4k3/8/8/8/8/8/8/5RK1 b - - 0 1", "e1g1", 60U, 62U, 63U, 61U, false},
        {"white-queenside", "4k3/8/8/8/8/8/8/R3K3 w Q - 0 1", "4k3/8/8/8/8/8/8/2KR4 b - - 0 1", "e1c1", 60U, 58U, 56U, 59U, false},
        {"black-kingside", "4k2r/8/8/8/8/8/8/4K3 b k - 0 1", "5rk1/8/8/8/8/8/8/4K3 w - - 0 1", "e8g8", 4U, 6U, 7U, 5U, true},
        {"black-queenside", "r3k3/8/8/8/8/8/8/4K3 b q - 0 1", "2kr4/8/8/8/8/8/8/4K3 w - - 0 1", "e8c8", 4U, 2U, 0U, 3U, true},
    }};
    for (const auto& fixture : castles) {
        CheckResultReadyAtCastleCommitIsRejected(results, fixture);
        for (const bool firstPlayerIsP2 : {false, true}) {
            for (const bool boardRotate : {false, true}) {
                CheckDeferredHintDuringCastle(results, fixture, firstPlayerIsP2, boardRotate);
            }
        }
    }

    CheckTerminalPosition(results,
                          "build/test_hint_failure_checkmate.json",
                          CHECKMATE_FEN,
                          TerminalState::Checkmate);
    CheckTerminalPosition(results,
                          "build/test_hint_failure_stalemate.json",
                          STALEMATE_FEN,
                          TerminalState::Stalemate);

    CheckRejectedHintCase(results,
                  RunCompletedHintCase("build/test_hint_failure_malformed.json",
                                       UNSAFE_ONLY_FEN,
                                       modules::ThinkState::Done,
                                       "bad",
                                       false),
                  "malformed engine result cannot become a Hint");

    CheckRejectedHintCase(results,
                  RunCompletedHintCase("build/test_hint_failure_illegal.json",
                                       UNSAFE_ONLY_FEN,
                                       modules::ThinkState::Done,
                                       "e4e6",
                                       false),
                  "illegal engine result cannot become a Hint");

    CheckHintCase(results,
                  RunCompletedHintCase("build/test_hint_failure_unsafe.json",
                                       SAFE_ALTERNATIVE_FEN,
                                       modules::ThinkState::Done,
                                       "e4e5",
                                       true),
                  36U,
                  28U,
                  "a legal completed engine result remains authoritative over the coaching heuristic");

    CheckRejectedHintCase(results,
                  RunCompletedHintCase("build/test_hint_failure_error.json",
                                       SAFE_ALTERNATIVE_FEN,
                                       modules::ThinkState::Error,
                                       "",
                                       true),
                  "engine error cannot fabricate a Hint");

    CheckRejectedHintCase(results,
                  RunCompletedHintCase("build/test_hint_failure_canceled.json",
                                       SAFE_ALTERNATIVE_FEN,
                                       modules::ThinkState::Canceled,
                                       "",
                                       true),
                  "unexpected cancellation cannot fabricate a Hint");

    CheckRejectedHintCase(results,
                  RunCompletedHintCase("build/test_hint_failure_idle.json",
                                       SAFE_ALTERNATIVE_FEN,
                                       modules::ThinkState::Idle,
                                       "",
                                       true),
                  "unexpected idle result cannot fabricate a Hint");

    {
        const char* path = "build/test_hint_failure_start_retry.json";
        RemoveSettingsFiles(path);
        modules::SettingsStoreConfig settingsCfg;
        settingsCfg.path = path;
        modules::SettingsStore settings(settingsCfg);
        results.Check(settings.Load(), "start-retry settings load");
        settings.Doc()[modules::KEY_GAME_FEN] = SAFE_ALTERNATIVE_FEN;

        settings.Doc()["HINT_LEVEL"] = 4U;
        FakeHintEngine engine(modules::ThinkState::Done, "a2a3", true);
        engine.SetStaleThinking();
        runtime::GameStateMachine sm(MakeStateMachineConfig());
        sm.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        results.Check(coord.LoadOrInitialize(), "start-retry coordinator load");
        ObserveExpectedBoard(sm, coord);

        coord.Update();
        uint8_t fromSquare = 0U;
        uint8_t toSquare = 0U;
        results.Check(!sm.TryGetHintMoveSquares(fromSquare, toSquare),
                      "failed start has no calculated Hint");
        fxe::ManualMillis::AdvanceMs(500U);
        coord.Update();
        results.Check(sm.TryGetHintMoveSquares(fromSquare, toSquare),
                      "successful retry reveals its actual calculated move after the initial window");
        results.Check(engine.StartCount() == 2U, "failed start retries after backoff");
        coord.Update();
        results.Check(engine.StartCount() == 2U, "successful retry is not restarted");
        results.Check(engine.CancelCount() != 0U && engine.PollCount() != 0U,
                      "failed start drains stale engine state");
    }

    {
        const char* path = "build/test_hint_failure_legal_cache_retry.json";
        RemoveSettingsFiles(path);
        modules::SettingsStoreConfig settingsCfg;
        settingsCfg.path = path;
        modules::SettingsStore settings(settingsCfg);
        results.Check(settings.Load(), "legal-cache-retry settings load");
        settings.Doc()[modules::KEY_GAME_FEN] = SAFE_ALTERNATIVE_FEN;

        FakeHintEngine engine(modules::ThinkState::Done, "a2a3", true);
        runtime::GameStateMachine sm(MakeStateMachineConfig());
        sm.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        results.Check(coord.LoadOrInitialize(), "legal-cache-retry coordinator load");
        ObserveExpectedBoard(sm, coord);
        engine.SetLegalFailures(1U);

        fxe::ManualMillis::SetNowMs(2000U);
        coord.Update();
        coord.Update();
        results.Check(engine.StartCount() == 0U, "legal-cache failure is not retried in a tight loop");

        fxe::ManualMillis::SetNowMs(2250U);
        coord.Update();
        uint8_t fromSquare = 0U;
        uint8_t toSquare = 0U;
        results.Check(engine.StartCount() == 1U, "legal-cache failure retries after backoff");
        // This fake publishes its scripted root immediately. The real worker
        // CheckBookHintFirstRevealJourney owns the 499/500 ms reveal boundary.
        results.Check(sm.TryGetHintMoveSquares(fromSquare, toSquare) &&
                          fromSquare == 48U && toSquare == 40U,
                      "legal-cache retry caches its exact scripted a2-a3 result");
        fxe::ManualMillis::AdvanceMs(500U);
        coord.Update();
        results.Check(engine.StartCount() == 1U &&
                          sm.TryGetHintMoveSquares(fromSquare, toSquare) &&
                          fromSquare == 48U && toSquare == 40U,
                      "successful legal-cache recovery retains a2-a3 without restarting after backoff");
    }

    {
        const char* path = "build/test_hint_coach_insight_cache.json";
        RemoveSettingsFiles(path);
        modules::SettingsStoreConfig settingsCfg;
        settingsCfg.path = path;
        modules::SettingsStore settings(settingsCfg);
        results.Check(settings.Load(), "coach-insight settings load");
        settings.Doc()[modules::KEY_GAME_FEN] = SAFE_ALTERNATIVE_FEN;

        FakeHintEngine engine(modules::ThinkState::Done, "e4e5", true,
                              TerminalState::None, true);
        runtime::GameStateMachine sm(MakeStateMachineConfig());
        sm.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        results.Check(coord.LoadOrInitialize(), "coach-insight coordinator load");
        ObserveExpectedBoard(sm, coord);
        coord.Update();
        fxe::ManualMillis::AdvanceMs(500U);
        coord.Update();
        coord.Update();
        results.Check(engine.HasContinuousSearchBudget() &&
                          engine.StartCount() == 1U,
                      "one naturally completed analysis caches the actual Hint");
        modules::CoachInsight insight;
        results.Check(!coord.TryGetCurrentCoachInsight(insight),
                      "ordinary Hint does not expose Coaching while disabled");

        engine.SetPollsBeforeDone(1U);
        coord.SetCoachingHintActive(true);
        coord.Update();

        uint8_t fromSquare = 0U;
        uint8_t toSquare = 0U;
        results.Check(sm.TryGetHintMoveSquares(fromSquare, toSquare) &&
                          fromSquare == 36U && toSquare == 28U,
                      "ordinary Hint stays visible when Coaching is enabled");
        results.Check(coord.TryGetCurrentCoachInsight(insight),
                      "Coaching reuses verified metadata from the current manual Hint");
        coord.Update();

        results.Check(engine.HasContinuousSearchBudget() &&
                          engine.StartCount() == 1U,
                      "active Coaching reuses the completed Hint without restarting analysis");
        results.Check(coord.TryGetCurrentCoachInsight(insight),
                      "engine-best hint caches its matching Coach insight");
        results.Check(insight.topic == modules::CoachTopic::GeneralMove &&
                          insight.fromSquare == 36U &&
                          insight.toSquare == 28U,
                      "cached Coach insight matches the selected legal hint");

        coord.SetCoachingHintActive(false);
        results.Check(!coord.TryGetCurrentCoachInsight(insight),
                      "disabling Coaching hides its cached insight");
        coord.SetCoachingHintActive(true);
        coord.Update();
        results.Check(engine.StartCount() == 1U &&
                          coord.TryGetCurrentCoachInsight(insight),
                      "re-enabling Coaching reuses the current legal result");

        results.Check(coord.LoadExternalFen(
                          "4k3/8/8/8/8/8/8/4K3 w - - 0 1"),
                      "external FEN replaces coached position");
        results.Check(!coord.TryGetCurrentCoachInsight(insight),
                      "position replacement invalidates the Coach insight");
    }

    {
        const char* path = "build/test_hint_coach_fallback.json";
        RemoveSettingsFiles(path);
        modules::SettingsStoreConfig settingsCfg;
        settingsCfg.path = path;
        modules::SettingsStore settings(settingsCfg);
        results.Check(settings.Load(), "coach-fallback settings load");
        settings.Doc()[modules::KEY_GAME_FEN] = SAFE_ALTERNATIVE_FEN;

        FakeHintEngine engine(modules::ThinkState::Done, "e4e6", true,
                              TerminalState::None, true);
        runtime::GameStateMachine sm(MakeStateMachineConfig());
        sm.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        results.Check(coord.LoadOrInitialize(), "coach-fallback coordinator load");
        ObserveExpectedBoard(sm, coord);
        coord.SetCoachingHintActive(true);
        coord.Update();

        modules::CoachInsight insight;
        results.Check(engine.HasContinuousSearchBudget(),
                      "Coaching uses the same background analysis config");
        results.Check(!coord.TryGetCurrentCoachInsight(insight),
                      "an illegal engine result never fabricates a Coach insight");
    }

    for (const modules::ThinkState failure :
         {modules::ThinkState::Error, modules::ThinkState::Canceled}) {
        const char* path = "build/test_hint_coach_retained_failure.json";
        RemoveSettingsFiles(path);
        modules::SettingsStoreConfig settingsCfg;
        settingsCfg.path = path;
        modules::SettingsStore settings(settingsCfg);
        results.Check(settings.Load(), "retained-cache settings load");
        settings.Doc()[modules::KEY_GAME_FEN] = SAFE_ALTERNATIVE_FEN;

        FakeHintEngine engine(modules::ThinkState::Done, "a2a3", true,
                              TerminalState::None, true);
        runtime::GameStateMachine sm(MakeStateMachineConfig());
        sm.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        results.Check(coord.LoadOrInitialize(), "retained-cache coordinator load");
        ObserveExpectedBoard(sm, coord);
        engine.SetPollsBeforeDone(2U);
        coord.Update();
        engine.PublishLiveMove("a2a3");
        sm.SetHintDisplayRequested(true);
        engine.SetThinkResult(failure);
        coord.SetCoachingHintActive(true);
        fxe::ManualMillis::AdvanceMs(500U);
        coord.Update();
        uint8_t fromSquare = 0U;
        uint8_t toSquare = 0U;
        results.Check(sm.TryGetHintMoveSquares(fromSquare, toSquare) &&
                          fromSquare == 48U && toSquare == 40U && engine.StartCount() == 1U,
                      "actual live Hint remains available during its original unfinished search");
        coord.Update();
        results.Check(sm.TryGetHintMoveSquares(fromSquare, toSquare) &&
                          fromSquare == 48U && toSquare == 40U,
                      "failed analysis preserves its already published actual candidate");
        modules::CoachInsight insight;
        results.Check(!coord.TryGetCurrentCoachInsight(insight),
                      "live move alone does not fabricate unavailable coaching metadata");
    }

    {
        const char* path =
            "build/test_hint_coach_inactive_unconsumed_reenable.json";
        RemoveSettingsFiles(path);
        modules::SettingsStoreConfig settingsCfg;
        settingsCfg.path = path;
        modules::SettingsStore settings(settingsCfg);
        results.Check(settings.Load(),
                      "inactive-unconsumed settings load");
        settings.Doc()[modules::KEY_GAME_FEN] = SAFE_ALTERNATIVE_FEN;

        FakeHintEngine engine(modules::ThinkState::Done, "a2a3", true,
                              TerminalState::None, true);
        runtime::GameStateMachine sm(MakeStateMachineConfig());
        sm.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        results.Check(coord.LoadOrInitialize(),
                      "inactive-unconsumed coordinator load");
        ObserveExpectedBoard(sm, coord);
        engine.SetPollsBeforeDone(1U);
        coord.SetCoachingHintActive(true);
        coord.Update();
        results.Check(engine.StartCount() == 1U &&
                          engine.HasContinuousSearchBudget(),
                      "inactive-unconsumed automatic search is logically owned");
        coord.SetCoachingHintActive(false);
        coord.SetCoachingHintActive(true);
        coord.Update();
        fxe::ManualMillis::AdvanceMs(500U);
        coord.Update();
        uint8_t fromSquare = 64U;
        uint8_t toSquare = 64U;
        const bool reused = engine.StartCount() == 1U &&
            sm.TryGetHintMoveSquares(fromSquare, toSquare) &&
            fromSquare == 48U && toSquare == 40U;
        results.Check(reused,
                      "false-true visibility changes reuse unfinished analysis without cancel/restart");
        std::printf("[COACH_VISIBILITY_REUSE] starts=%lu retained=48-40 reused=%u\n",
                    static_cast<unsigned long>(engine.StartCount()), reused);
    }

    return results.Finish();
}
