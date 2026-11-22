#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/async_think_worker.hpp"
#include "modules/beginner_mode_controller.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/duck_game_persistence.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/home_state_snapshot.hpp"
#include "modules/initial_fen.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/settings_store.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#include "../../src/virtual_litch_session_internal.hpp"

void CheckCpuThinkingJourneys(fxe::test_support::TestResults& results);

namespace {

constexpr const char* POSITION_X =
    "4k3/8/3p4/8/4Q3/8/P7/4K3 w - - 0 1";
constexpr const char* POSITION_Y =
    "4k3/8/3p4/8/8/P7/8/4K3 w - - 0 2";
constexpr uint64_t POSITION_Y_OCCUPANCY =
    (UINT64_C(1) << 4U) | (UINT64_C(1) << 19U) |
    (UINT64_C(1) << 40U) | (UINT64_C(1) << 60U);
constexpr const char* POSITION_X_AFTER_E4_E5 =
    "4k3/8/3p4/4Q3/8/8/P7/4K3 b - - 1 1";
constexpr uint64_t POSITION_X_OCCUPANCY =
    (UINT64_C(1) << 4U) | (UINT64_C(1) << 19U) |
    (UINT64_C(1) << 36U) | (UINT64_C(1) << 48U) |
    (UINT64_C(1) << 60U);
constexpr uint64_t INITIAL_OCCUPANCY = UINT64_C(0xffff00000000ffff);

class ControlledHintEngine final : public modules::IEngine {
  public:
    explicit ControlledHintEngine(const uint32_t blockedStartNumber = 1U)
        : m_blockedStartNumber(blockedStartNumber) {}

    void Reset() override {
        ++resetCalls;
        if (pollsInProgress.load(std::memory_order_acquire) != 0U) {
            ++unsafeEngineAccesses;
        }
    }

    bool SetFen(const char* fen) override {
        ++setFenCalls;
        if (pollsInProgress.load(std::memory_order_acquire) != 0U) {
            ++unsafeEngineAccesses;
        }
        return m_fen.Assign(fen);
    }

    bool GetFen(char* outFen, const size_t outSize) const override {
        ++getFenCalls;
        if (pollsInProgress.load(std::memory_order_acquire) != 0U) {
            ++unsafeEngineAccesses;
        }
        if (outFen == nullptr || outSize == 0U || m_fen.Empty()) {
            return false;
        }
        std::snprintf(outFen, outSize, "%s", m_fen.CStr());
        return true;
    }

    bool ApplyUciMove(const char* move) override {
        ++applyMoveCalls;
        if (pollsInProgress.load(std::memory_order_acquire) != 0U) {
            ++unsafeEngineAccesses;
        }
        return move != nullptr && std::strcmp(move, "e4e5") == 0 &&
               m_fen.Assign(POSITION_X_AFTER_E4_E5);
    }

    modules::SideToMove GetSideToMove() const override {
        return std::strstr(m_fen.CStr(), " b ") != nullptr
                   ? modules::SideToMove::Black
                   : modules::SideToMove::White;
    }

    bool IsInCheck(modules::SideToMove) const override { return false; }
    bool IsInCheckmate(modules::SideToMove) const override { return false; }
    bool IsInStalemate(modules::SideToMove) const override { return false; }

    bool GetLegalDestinationsMask(
        const uint8_t fromSquare,
        uint64_t& outDestMask64) override {
        if (pollsInProgress.load(std::memory_order_acquire) != 0U ||
            (statsEntered.load(std::memory_order_acquire) &&
             !releaseStats.load(std::memory_order_acquire))) {
            ++unsafeEngineAccesses;
        }
        const bool positionY = m_fen.Equals(POSITION_Y);
        outDestMask64 =
            (!positionY && fromSquare == 36U)
                ? UINT64_C(1) << 28U
                : (!positionY && fromSquare == 48U)
                ? UINT64_C(1) << 40U
                : (positionY && fromSquare == 40U)
                      ? UINT64_C(1) << 32U
                      : 0ULL;
        return true;
    }

    bool StartThink(const modules::ThinkConfig& config) override {
        if (pollsInProgress.load(std::memory_order_acquire) != 0U ||
            (statsEntered.load(std::memory_order_acquire) &&
             !releaseStats.load(std::memory_order_acquire))) {
            ++unsafeEngineAccesses;
            return false;
        }
        const uint32_t startNumber =
            startThinkCalls.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        lastConfig = config;
        cancelRequested.store(false, std::memory_order_release);
        pollEntered.store(false, std::memory_order_release);
        releaseCurrentPoll.store(
            startNumber != m_blockedStartNumber,
            std::memory_order_release);
        (void)m_searchBest.Assign(
            m_fen.Equals(POSITION_Y) ? "a3a4" :
            refineBest && config.difficulty > 4U ? "e4e5" : "a2a3");
        return true;
    }

    bool StartDuckThink(const char*, uint8_t,
                        const modules::ThinkConfig& config) override {
        const bool started = StartThink(config);
        liveDuck = true;
        return started;
    }

    bool GetLastDuckThinkResult(modules::DuckThinkResult& out) const override {
        ++classicResultReads;
        if (pollsInProgress.load(std::memory_order_acquire) != 0U ||
            (statsEntered.load(std::memory_order_acquire) &&
             !releaseStats.load(std::memory_order_acquire))) {
            ++unsafeEngineAccesses;
        }
        out = modules::DuckThinkResult{};
        out.chessMove = m_searchBest;
        out.duckDestination = 35U;
        return true;
    }

    void CancelThink() override {
        ++cancelThinkCalls;
        cancelRequested.store(true, std::memory_order_release);
    }

    modules::ThinkState PollThinkProgress(
        uint8_t& outProgressPercent,
        modules::UciMove& outBestMoveUci) override {
        const uint32_t concurrent =
            pollsInProgress.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        uint32_t priorMax = maxConcurrentPolls.load(std::memory_order_relaxed);
        while (priorMax < concurrent &&
               !maxConcurrentPolls.compare_exchange_weak(
                   priorMax, concurrent, std::memory_order_relaxed)) {}
        pollEntered.store(true, std::memory_order_release);
        while (!releaseCurrentPoll.load(std::memory_order_acquire)) {
            const uint32_t step = requestedCandidate.load(std::memory_order_acquire);
            if (step != 0U && step != publishedCandidate.load(std::memory_order_relaxed)) {
                modules::ThinkCandidate candidate;
                candidate.completedDepth = static_cast<uint8_t>(step);
                candidate.rootKind = liveDuck ? modules::ThinkRootKind::DuckCompound
                                              : modules::ThinkRootKind::ClassicFull;
                candidate.scoreUnits = liveDuck ? modules::ThinkScoreUnits::DuckPawn1000
                                                : modules::ThinkScoreUnits::ClassicPawn100;
                candidate.scoreValid = true;
                candidate.completedNodes = step * 1000U;
                const bool positionY = m_fen.Equals(POSITION_Y);
                candidate.fromSquare = positionY ? 40U : step == 1U ? 48U : 36U;
                candidate.toSquare = positionY ? 32U : step == 1U ? 40U : 28U;
                candidate.isDuck = liveDuck;
                candidate.duckDestination = liveDuck ? 35U : 255U;
                (void)m_searchBest.Assign(positionY ? "a3a4" : step == 1U ? "a2a3" : "e4e5");
                lastConfig.candidateObserver.Publish(candidate);
                lastConfig.progressObserver.Publish(step * 1000U);
                publishedCandidate.store(step, std::memory_order_release);
            }
            std::this_thread::yield();
        }
        pollsInProgress.fetch_sub(1U, std::memory_order_acq_rel);
        pollCompletions.fetch_add(1U, std::memory_order_release);
        outProgressPercent = 100U;
        outBestMoveUci = m_searchBest;
        return cancelRequested.load(std::memory_order_acquire)
                   ? modules::ThinkState::Canceled
                   : failCurrentPoll.load(std::memory_order_acquire)
                   ? modules::ThinkState::Error
                   : modules::ThinkState::Done;
    }

    bool GetLastThinkStats(modules::ThinkStats& outStats) const override {
        statsEntered.store(true, std::memory_order_release);
        while (!releaseStats.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const uint32_t step = publishedCandidate.load(std::memory_order_acquire);
        outStats.nodeCount = step == 0U ? 1U : step * 1000U;
        return true;
    }

    bool GetLastClassicThinkResult(
        modules::ClassicThinkResult& outResult) const override {
        ++classicResultReads;
        if (statsEntered.load(std::memory_order_acquire) &&
            !releaseStats.load(std::memory_order_acquire)) {
            ++unsafeEngineAccesses;
        }
        outResult = modules::ClassicThinkResult{};
        litch_max::ParsedUciMove parsed;
        if (!litch_max::TryParseUciMove(m_searchBest.CStr(), parsed)) {
            return false;
        }
        outResult.candidates[0] = {parsed.fromSquare, parsed.toSquare, '\0'};
        outResult.candidateCount = 1U;
        outResult.bestScoreValid = true;
        outResult.bestMoveFacts = modules::ClassicThinkOutcomeKnown;
        return true;
    }

    bool WaitForStatsEntry() const {
        for (uint32_t attempt = 0U; attempt < 1000000U; ++attempt) {
            if (statsEntered.load(std::memory_order_acquire)) return true;
            std::this_thread::yield();
        }
        return false;
    }

    bool WaitForPollEntry() const {
        for (uint32_t attempt = 0U; attempt < 1000000U; ++attempt) {
            if (pollEntered.load(std::memory_order_acquire)) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    bool WaitForWorkerCompletion(const modules::AsyncThinkWorker& worker,
                                 const uint32_t expected = 0U) const {
        for (uint32_t attempt = 0U; attempt < 1000000U; ++attempt) {
            if (pollCompletions.load(std::memory_order_acquire) >= expected &&
                !worker.IsActive()) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    bool PublishCandidate(const uint32_t step) {
        requestedCandidate.store(step, std::memory_order_release);
        for (uint32_t attempt = 0U; attempt < 1000000U; ++attempt) {
            if (publishedCandidate.load(std::memory_order_acquire) == step) return true;
            std::this_thread::yield();
        }
        return false;
    }

    void ReleaseFirstPoll() {
        releaseCurrentPoll.store(true, std::memory_order_release);
    }

    void BlockStartNumber(const uint32_t startNumber) {
        m_blockedStartNumber = startNumber;
    }

    mutable std::atomic<uint32_t> unsafeEngineAccesses{0U};
    mutable std::atomic<uint32_t> getFenCalls{0U};
    mutable std::atomic<uint32_t> classicResultReads{0U};
    std::atomic<uint32_t> setFenCalls{0U};
    std::atomic<uint32_t> resetCalls{0U};
    std::atomic<uint32_t> applyMoveCalls{0U};
    std::atomic<uint32_t> startThinkCalls{0U};
    std::atomic<uint32_t> cancelThinkCalls{0U};
    std::atomic<uint32_t> pollsInProgress{0U};
    std::atomic<uint32_t> maxConcurrentPolls{0U};
    std::atomic<uint32_t> pollCompletions{0U};
    std::atomic<bool> pollEntered{false};
    std::atomic<bool> releaseCurrentPoll{false};
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> failCurrentPoll{false};
    mutable std::atomic<bool> statsEntered{false};
    std::atomic<bool> releaseStats{true};
    modules::ThinkConfig lastConfig{};
    bool refineBest = false;
    bool liveDuck = false;
    std::atomic<uint32_t> requestedCandidate{0U};
    std::atomic<uint32_t> publishedCandidate{0U};

  private:
    modules::FenString m_fen;
    modules::UciMove m_searchBest;
    uint32_t m_blockedStartNumber = 1U;
};

runtime::GameStateMachineConfig MakeStateMachineConfig() {
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 0U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    return config;
}

void ObserveExpectedBoard(runtime::GameStateMachine& stateMachine,
                          modules::GamePersistenceCoordinator& coordinator) {
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    const uint64_t occupied = stateMachine.GetExpectedOccupancyMask();
    (void)dispatcher.DispatchBoardObservation({
        fxe::Millis(), 1U,
        stateMachine.GetPhysicalOccupancyMask() ^ occupied, occupied});
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

enum class OrdinaryLegalRailSample : uint8_t { Live, SteadyReference };

std::array<virtual_litch_color, 64U> CaptureSquareFrame(
    const modules::SettingsStore& settings,
    const modules::GamePersistenceCoordinator& coordinator,
    const runtime::GameStateMachine& stateMachine,
    const bool showHint,
    std::array<modules::Rgb, 64U>* composedSquares = nullptr,
    const OrdinaryLegalRailSample legalRails = OrdinaryLegalRailSample::Live) {
    auto snapshot = runtime::BuildInteractionModelSnapshot({
        .stateMachine = &stateMachine,
        .derived = &coordinator.GetEngineDerivedState(),
        .authoritativeFen = coordinator.GetAuthoritativeFen(),
        .variant = coordinator.GetGameVariant(),
        .duckSquare = coordinator.GetDuckSquare(),
        .duckTurnPhase = coordinator.GetDuckTurnPhase(),
    });
    if (!showHint) snapshot.render.hintDisplayRequested = false;
    if (legalRails == OrdinaryLegalRailSample::SteadyReference) {
        snapshot.render.legalDestinationReveal = {};
    }
    modules::led_frame_composer::ComposeInputs input;
    input.settings = &settings;
    input.interactionSnapshot = &snapshot;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edge{};
    std::array<modules::Rgb, 64U> grid{};
    modules::led_frame_composer::Compose(input, board, edge, grid);
    modules::status_led_frame_adapter::ApplyToDedicatedChain(edge, false, board);
    runtime::virtual_litch_internal::CaptureLedRenderer renderer(
        hardware::TOTAL_LED_COUNT, true);
    modules::PowerBudgetPolicy power(
        modules::led_render_policy::MakeConservativePowerBudgetPolicyConfig(
            {true, false}));
    modules::BudgetAwareLedPipeline pipeline(
        renderer, power,
        modules::led_render_policy::MakePipelineConfig(true, 32U));
    pipeline.RenderFrame(board.data(), board.size());
    std::array<virtual_litch_color, 64U> frame{};
    for (uint8_t square = 0U; square < 64U; ++square) {
        const auto index = hardware::LedIndexForSquare(
            square, hardware::SquareLedSlot::Square2);
        frame[square] = renderer.Pixels()[index];
        if (composedSquares != nullptr) (*composedSquares)[square] = board[index];
    }
    return frame;
}

bool SamePixel(const virtual_litch_color a, const virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}

void CheckPollReturnIsNotWorkerQuiescence(
    fxe::test_support::TestResults& results) {
    ControlledHintEngine engine;
    modules::AsyncThinkWorker worker;
    (void)engine.SetFen(POSITION_X);
    engine.releaseStats.store(false, std::memory_order_release);
    results.Check(engine.StartThink({}) && worker.Start(engine) &&
                      engine.WaitForPollEntry(),
                  "cleanup-tail fixture starts the real worker");
    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForStatsEntry() && worker.IsActive(),
                  "real worker still owns engine after poll returns");
    results.Check(!engine.WaitForWorkerCompletion(worker),
                  "post-completion wait must not succeed during statistics cleanup");
    engine.releaseStats.store(true, std::memory_order_release);
    for (uint32_t attempt = 0U; attempt < 1000000U && worker.IsActive(); ++attempt) {
        std::this_thread::yield();
    }
    results.Check(!worker.IsActive() && worker.IsDone(),
                  "released cleanup publishes a complete inactive worker");
}

void CheckClassicResultWaitsForWorkerCleanup(
    fxe::test_support::TestResults& results) {
    constexpr const char* PATH =
        "build/test_async_engine_classic_result_cleanup.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "classic-result cleanup settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = POSITION_X;

    ControlledHintEngine engine;
    engine.releaseStats.store(false, std::memory_order_release);
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(),
                  "classic-result cleanup coordinator load");
    ObserveExpectedBoard(stateMachine, coordinator);
    coordinator.Update();
    results.Check(engine.WaitForPollEntry() && worker.IsActive(),
                  "classic-result cleanup Hint starts in real worker");
    const modules::ControlCoordinatorResult hintOutcome =
        coordinator.RequestControlHint(701U);
    const modules::BeginnerModeController beginnerMode;
    const uint32_t unsafeBeforeSnapshot =
        engine.unsafeEngineAccesses.load();
    modules::HomeStateNotification activeHome =
        modules::BuildHomeStateSnapshot(
            stateMachine, coordinator, settings, beginnerMode,
            modules::PersistenceDegradation::None);
    activeHome.sequence = 1U;
    modules::HomeStateNotificationLine activeHomeLine;
    const bool projectedWhileOwned =
        modules::TryFormatHomeStateNotification(
            activeHome, activeHomeLine) &&
        activeHome.assistance.hintRequested &&
        activeHome.assistance.manualAffordanceUseful &&
        activeHome.assistance.presentation ==
            modules::HomeAssistancePresentation::Thinking;
    results.Check(
        hintOutcome == modules::ControlCoordinatorResult::Applied &&
            projectedWhileOwned && worker.IsActive() &&
            engine.unsafeEngineAccesses.load() == unsafeBeforeSnapshot,
        "Home snapshots requested thinking state and the observed board's Hint affordance without reading the worker-owned engine");
    fxe::ManualMillis::AdvanceMs(500U);
    results.Check(engine.PublishCandidate(1U),
                  "controlled calculation publishes its qualified root after500ms before the statistics tail");
    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForStatsEntry() && worker.IsActive(),
                  "classic-result cleanup holds after PollThinkProgress returns");

    coordinator.Update();
    results.Check(worker.IsActive() &&
                      engine.classicResultReads.load() == 0U &&
                      engine.unsafeEngineAccesses.load() == 0U,
                  "coordinator does not read Classic result during worker statistics tail");
    engine.releaseStats.store(true, std::memory_order_release);
    results.Check(engine.WaitForWorkerCompletion(worker, 1U),
                  "classic-result cleanup worker publishes inactive completion");
    coordinator.Update();
    uint8_t fromSquare = 64U;
    uint8_t toSquare = 64U;
    const bool consumedSafely =
        !worker.IsActive() && engine.classicResultReads.load() == 1U &&
        engine.unsafeEngineAccesses.load() == 0U &&
        stateMachine.TryGetHintMoveSquares(fromSquare, toSquare) &&
        fromSquare == 48U && toSquare == 40U;
    results.Check(
        consumedSafely,
        "coordinator reads Classic result exactly once after public worker quiescence");
    std::printf(
        "[ASYNC_ENGINE_OWNERSHIP] case=classic-result-cleanup "
        "worker_active=%u reads=%u unsafe=%u hint=%u-%u accepted=%u\n",
        worker.IsActive() ? 1U : 0U,
        engine.classicResultReads.load(),
        engine.unsafeEngineAccesses.load(),
        static_cast<unsigned>(fromSquare),
        static_cast<unsigned>(toSquare),
        consumedSafely ? 1U : 0U);
}

void CheckEmptyOpeningWaitsForWorkerCleanup(
    fxe::test_support::TestResults& results) {
    constexpr const char* PATH = "build/test_async_engine_empty_opening_cleanup.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "empty opening cleanup settings load");
    settings.Doc()["TIMER"] = 1U;
    ControlledHintEngine engine;
    engine.releaseStats.store(false, std::memory_order_release);
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(), "empty opening cleanup initializes a fresh game");
    ObserveExpectedBoard(stateMachine, coordinator);
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();
    results.Check(engine.WaitForPollEntry() && worker.IsActive(),
                  "opening Hint owns the real worker");
    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForStatsEntry() && worker.IsActive(),
                  "empty clearing overlaps real post-poll statistics cleanup");
    const uint32_t fenReads = engine.getFenCalls.load();
    const uint32_t fenWrites = engine.setFenCalls.load();
    const uint32_t white = coordinator.GetGameClock().GetRemainingMs(runtime::ClockSide::White);
    const auto history = coordinator.GetGameHistoryPersistenceState();
    (void)dispatcher.DispatchBoardObservation({
        fxe::Millis(), stateMachine.GetLastBoardObservationSequence() + 1U,
        INITIAL_OCCUPANCY, 0ULL});
    for (uint32_t tick = 0U; tick < 3U; ++tick) {
        fxe::ManualMillis::AdvanceMs(500U);
        dispatcher.DispatchTick(fxe::Millis());
        coordinator.Update();
    }
    results.Check(worker.IsActive() && engine.unsafeEngineAccesses.load() == 0U &&
                      engine.getFenCalls.load() == fenReads && engine.setFenCalls.load() == fenWrites &&
                      engine.applyMoveCalls.load() == 0U && coordinator.GetUndoHistoryMoveCount() == 0U &&
                      stateMachine.GetPhysicalOccupancyMask() == 0ULL,
                  "empty retries own raw occupancy without reading or mutating the busy engine or committing a move");
    engine.releaseStats.store(true, std::memory_order_release);
    results.Check(engine.WaitForWorkerCompletion(worker, 1U), "cleanup releases actual worker ownership");
    coordinator.Update();
    const uint32_t starts = engine.startThinkCalls.load();
    const auto revision = settings.GetRevision();
    for (uint32_t tick = 0U; tick < 3U; ++tick) {
        fxe::ManualMillis::AdvanceMs(500U);
        dispatcher.DispatchTick(fxe::Millis());
        coordinator.Update();
    }
    uint8_t from = 64U;
    uint8_t to = 64U;
    const auto& restoredHistory = coordinator.GetGameHistoryPersistenceState();
    results.Check(stateMachine.IsPlacementModeActive() &&
                      coordinator.GetGameVariant() == modules::GameVariant::Undecided &&
                      !worker.IsActive() && !stateMachine.TryGetHintMoveSquares(from, to) &&
                      !stateMachine.IsHintThinkingActive() && !stateMachine.IsCpuThinkingActive() &&
                      engine.classicResultReads.load() == 0U && engine.startThinkCalls.load() == starts &&
                      engine.applyMoveCalls.load() == 0U && settings.GetRevision() == revision &&
                      restoredHistory.gameId == history.gameId &&
                      restoredHistory.lineRevision == history.lineRevision &&
                      coordinator.GetGameClock().GetRemainingMs(runtime::ClockSide::White) == white,
                  "quiescence retires once into empty Setup without stale publication, search restart, history or clock mutation");
    std::printf("[ASYNC_ENGINE_OWNERSHIP] case=empty-opening-cleanup physical=%016llx "
                "placement=%u reads=%u unsafe=%u starts=%u history=%u white=%u next=arrange-opening\n",
                static_cast<unsigned long long>(stateMachine.GetPhysicalOccupancyMask()),
                stateMachine.IsPlacementModeActive() ? 1U : 0U,
                engine.classicResultReads.load(), engine.unsafeEngineAccesses.load(),
                engine.startThinkCalls.load(), coordinator.GetUndoHistoryMoveCount(),
                coordinator.GetGameClock().GetRemainingMs(runtime::ClockSide::White));
}

void CheckCpuTakeoverWaitsForHint(fxe::test_support::TestResults& results,
                                const bool duringCleanup,
                                const bool changeSeat,
                                const bool duck = false) {
    constexpr const char* PATH = "build/test_async_engine_cpu_takeover.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "CPU takeover settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = duck ? POSITION_X : modules::INITIAL_FEN;
    if (duck) {
        modules::DuckGameDurableState durable;
        durable.variant = modules::GameVariant::Duck;
        durable.duckSquare = 24U;
        char encoded[32] = {};
        results.Check(modules::EncodeDuckGameDurableState(durable, encoded, sizeof(encoded)),
                      "CPU takeover uses the production Duck durable codec");
        modules::GamePersistenceState state;
        (void)state.fen.Assign(POSITION_X);
        (void)state.undoBaseFen.Assign(POSITION_X);
        results.Check(state.variantState.Assign(encoded) &&
                          modules::WriteGamePersistenceState(settings.Doc(), state),
                      "CPU takeover stores the production Duck position");
    }
    settings.Doc()["VS_CPU_LEVEL"] = changeSeat ? 3U : 0U;
    settings.Doc()["VS_CPU_PLAYER"] = changeSeat ? 2U : 1U;
    settings.Doc()["P1_PIECE_WHEEL"] = 0U;
    ControlledHintEngine engine;
    engine.releaseStats.store(!duringCleanup, std::memory_order_release);
    auto config = MakeStateMachineConfig();
    config.cpuIdleDelayMs = 1U;
    runtime::GameStateMachine stateMachine(config);
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(), "CPU takeover coordinator load");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    (void)dispatcher.DispatchBoardObservation({
        fxe::Millis(), 1U,
        duck ? POSITION_X_OCCUPANCY | (UINT64_C(1) << 24U) : INITIAL_OCCUPANCY,
        duck ? POSITION_X_OCCUPANCY | (UINT64_C(1) << 24U) : INITIAL_OCCUPANCY});
    coordinator.Update();
    results.Check(engine.WaitForPollEntry() && worker.IsActive(),
                  "human Hint owns engine before CPU takeover");
    fxe::ManualMillis::SetNowMs(fxe::Millis() + 500U);
    results.Check(engine.PublishCandidate(1U),
                  "human Hint publishes a scored root through the real worker mailbox after500ms before takeover");
    coordinator.RequestHintToggle();
    uint8_t from = 0U;
    uint8_t to = 0U;
    const bool visibleBeforeTakeover =
        stateMachine.IsHintDisplayRequested() &&
        stateMachine.TryGetHintMoveSquares(from, to) &&
        from == 48U && to == 40U &&
        (!duck || (stateMachine.TryGetHintDuckMoveSquares(from, to) &&
                   from == 24U && to == 35U));
    results.Check(visibleBeforeTakeover && worker.IsActive(),
                  "manual Hint reveals mailbox guidance before CPU takeover, including the Duck relocation");
    const auto printFrame = [&](const char* checkpoint, const char* nextAction,
                                const std::array<virtual_litch_color, 64U>& frame) {
        const auto source = frame[48U];
        const auto target = frame[40U];
        const auto duckSource = frame[24U];
        const auto duckTarget = frame[35U];
        std::printf("[CPU_TAKEOVER_RGBW] variant=%s tail=%u seat=%u checkpoint=%s "
                    "worker_active=%u phase=%u occupancy=%016llx "
                    "a2=%u,%u,%u,%u a3=%u,%u,%u,%u "
                    "a5=%u,%u,%u,%u d4=%u,%u,%u,%u next=%s\n",
                    duck ? "Duck" : "Classic", duringCleanup ? 1U : 0U,
                    changeSeat ? 1U : 0U, checkpoint, worker.IsActive() ? 1U : 0U,
                    static_cast<unsigned>(stateMachine.GetChessInteractionModel().phase.tag),
                    static_cast<unsigned long long>(stateMachine.GetPhysicalOccupancyMask()),
                    source.r, source.g, source.b, source.w,
                    target.r, target.g, target.b, target.w,
                    duckSource.r, duckSource.g, duckSource.b, duckSource.w,
                    duckTarget.r, duckTarget.g, duckTarget.b, duckTarget.w, nextAction);
    };
    const auto visibleFrame = CaptureSquareFrame(settings, coordinator, stateMachine, true);
    const auto hiddenFrame = CaptureSquareFrame(settings, coordinator, stateMachine, false);
    results.Check(!SamePixel(visibleFrame[40U], hiddenFrame[40U]) &&
                      (!duck || !SamePixel(visibleFrame[35U], hiddenFrame[35U])),
                  "manual chess and Duck Hint targets reach the actual RGBW renderer before takeover");
    printFrame("visible-before", duck ? "consider a2a3 then Duck a5d4" : "consider a2a3",
               visibleFrame);
    if (duringCleanup) {
        engine.ReleaseFirstPoll();
        results.Check(engine.WaitForStatsEntry() && worker.IsActive(),
                      "CPU takeover occurs inside the real worker cleanup tail");
    }
    fxe::ManualMillis::SetNowMs(fxe::Millis() + 10U);
    dispatcher.DispatchTick(fxe::Millis());
    modules::ControlMutationCallbacks callbacks;
    callbacks.context = &coordinator;
    callbacks.cancelCpuWork = [](void* context) {
        static_cast<modules::GamePersistenceCoordinator*>(context)->RequestCancelCpuWork();
    };
    modules::ControlMutationService controls;
    controls.Configure(settings, callbacks);
    StaticJsonDocument<16> value;
    value.set(changeSeat ? 1U : 3U);
    results.Check(controls.MutateSetting(changeSeat ? "VS_CPU_PLAYER" : "VS_CPU_LEVEL",
                                       value.as<JsonVariantConst>(), 81U)
                      .status == modules::ControlMutationStatus::Applied,
                  "canonical CPU setting admits takeover without canceling unrelated Hint directly");
    coordinator.Update();
    results.Check(engine.unsafeEngineAccesses.load() == 0U &&
                      engine.startThinkCalls.load() == 1U && worker.IsActive(),
                  "CPU takeover never accesses an engine still owned by Hint");
    results.Check(engine.cancelRequested.load(std::memory_order_acquire),
                  "CPU takeover retires the ineligible Hint before its 90 second budget expires");
    const bool suppressedWhileActive =
        worker.IsActive() &&
        !stateMachine.TryGetHintMoveSquares(from, to) &&
        !stateMachine.TryGetHintDuckMoveSquares(from, to) &&
        stateMachine.IsHintDisplayRequested() == !duck;
    results.Check(suppressedWhileActive,
                  "CPU takeover hides old Hint cues while poll or cleanup still owns the engine");
    const auto drainingFrame = CaptureSquareFrame(settings, coordinator, stateMachine, true);
    const auto drainingHidden = CaptureSquareFrame(settings, coordinator, stateMachine, false);
    results.Check(SamePixel(drainingFrame[40U], drainingHidden[40U]) &&
                      (!duck || SamePixel(drainingFrame[35U], drainingHidden[35U])),
                  "pending cleanup renders no human chess or compound Hint influence");
    printFrame("suppressed-draining", "wait for CPU search ownership", drainingFrame);
    results.Check(stateMachine.GetChessInteractionModel().phase.tag ==
                      runtime::ChessInteractionPhase::Idle &&
                      stateMachine.GetPhysicalOccupancyMask() ==
                          (duck ? POSITION_X_OCCUPANCY | (UINT64_C(1) << 24U) : INITIAL_OCCUPANCY) &&
                      std::strcmp(coordinator.GetAuthoritativeFen(),
                                  duck ? POSITION_X : modules::INITIAL_FEN) == 0,
                  "pending CPU takeover preserves occupancy and authoritative position");
    engine.ReleaseFirstPoll();
    engine.releaseStats.store(true, std::memory_order_release);
    results.Check(engine.WaitForWorkerCompletion(worker, 1U),
                  "Hint cancellation fully releases worker ownership");
    coordinator.Update();
    coordinator.Update();
    results.Check(engine.WaitForWorkerCompletion(worker, 2U),
                  "retained CPU idle intent starts exactly one replacement search");
    coordinator.Update();
    results.Check(engine.startThinkCalls.load() == 2U &&
                      engine.unsafeEngineAccesses.load() == 0U &&
                      stateMachine.HasQueuedCpuMove() &&
                      !engine.lastConfig.continuousAnalysis &&
                      !stateMachine.TryGetHintMoveSquares(from, to) &&
                      !stateMachine.TryGetHintDuckMoveSquares(from, to),
                  "CPU prompt replaces canceled Hint once without losing takeover intent");
    bool captureValid = false;
    uint8_t captureSquare = 64U;
    results.Check(stateMachine.TryGetQueuedCpuMove(from, to, captureValid, captureSquare) &&
                      from == 48U && to == 40U && !captureValid,
                  "selected CPU next action comes from the replacement result");
    const auto selectedFrame = CaptureSquareFrame(settings, coordinator, stateMachine, true);
    const auto selectedHidden = CaptureSquareFrame(settings, coordinator, stateMachine, false);
    results.Check(SamePixel(selectedFrame[40U], selectedHidden[40U]) &&
                      (!duck || SamePixel(selectedFrame[35U], selectedHidden[35U])) &&
                      !SamePixel(selectedFrame[40U], drainingFrame[40U]),
                  "selected CPU target renders its physical action cue with no old Hint influence");
    printFrame("CPU-selected", duck ? "move a2a3 then relocate Duck" : "move a2a3",
               selectedFrame);
    std::printf("[ASYNC_ENGINE_OWNERSHIP] case=cpu-takeover variant=%s tail=%u seat=%u visible_before=%u suppressed_while_active=%u starts=%u unsafe=%u queued=%u\n",
                duck ? "Duck" : "Classic", duringCleanup ? 1U : 0U, changeSeat ? 1U : 0U,
                visibleBeforeTakeover ? 1U : 0U, suppressedWhileActive ? 1U : 0U,
                engine.startThinkCalls.load(), engine.unsafeEngineAccesses.load(),
                stateMachine.HasQueuedCpuMove() ? 1U : 0U);
}

void CheckCancelBeforeReadyAndRetry(
    fxe::test_support::TestResults& results) {
    constexpr const char* PATH =
        "build/test_async_engine_ownership_cancel_before_ready.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "async ownership settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = POSITION_X;

    ControlledHintEngine engine;
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(),
                  "async ownership coordinator load");
    ObserveExpectedBoard(stateMachine, coordinator);
    const uint32_t baselineSetFen = engine.setFenCalls.load();

    coordinator.Update();
    results.Check(engine.WaitForPollEntry(),
                  "coordinator starts Hint through the injected real worker");

    const auto cancelStarted = std::chrono::steady_clock::now();
    const bool loadedWhileActive = coordinator.LoadExternalFen(POSITION_Y);
    const auto cancelElapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cancelStarted)
            .count();
    results.Check(!loadedWhileActive,
                  "external FEN fails closed while cancellation is pending");
    results.Check(cancelElapsedMs < 100,
                  "pending cancellation returns without stalling the loop");
    results.Check(engine.setFenCalls.load() == baselineSetFen &&
                      engine.unsafeEngineAccesses.load() == 0U &&
                      std::strcmp(coordinator.GetAuthoritativeFen(),
                                  POSITION_X) == 0 &&
                      stateMachine.IsHintThinkingActive(),
                  "failed load preserves X and outstanding search ownership");

    const auto pendingRetryStarted = std::chrono::steady_clock::now();
    const bool repeatedLoadWhileActive =
        coordinator.LoadExternalFen(POSITION_Y);
    const auto pendingRetryElapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pendingRetryStarted)
            .count();
    results.Check(!repeatedLoadWhileActive && pendingRetryElapsedMs < 100 &&
                      engine.setFenCalls.load() == baselineSetFen &&
                      engine.unsafeEngineAccesses.load() == 0U,
                  "a never-completing worker keeps repeated retries nonblocking and fail-closed");

    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker),
                  "late canceled search eventually exits");
    results.Check(coordinator.LoadExternalFen(POSITION_Y),
                  "external FEN retry succeeds after worker release");
    results.Check(engine.setFenCalls.load() == baselineSetFen + 1U &&
                      engine.unsafeEngineAccesses.load() == 0U &&
                      std::strcmp(coordinator.GetAuthoritativeFen(),
                                  POSITION_Y) == 0 &&
                      !stateMachine.IsHintThinkingActive(),
                  "retry performs exactly one safe Y mutation and clears stale Hint");

    // Keep Y's first pass owned until the test releases it. Otherwise the
    // worker can finish inside the update that starts it, and the +500ms update
    // legitimately starts refinement instead of first consuming this result.
    engine.BlockStartNumber(2U);
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    const uint64_t observedBefore =
        stateMachine.GetPhysicalOccupancyMask();
    (void)dispatcher.DispatchBoardObservation({
        fxe::Millis(), stateMachine.GetLastBoardObservationSequence() + 1U,
        observedBefore ^ POSITION_Y_OCCUPANCY,
        POSITION_Y_OCCUPANCY});
    coordinator.Update();
    results.Check(engine.startThinkCalls.load() == 2U &&
                      engine.WaitForPollEntry() && worker.IsActive(),
                  "next-position Hint owns the worker before its controlled completion");
    fxe::ManualMillis::AdvanceMs(500U);
    results.Check(engine.PublishCandidate(1U),
                  "the replacement calculation publishes its qualified current-Y root at500ms");
    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker, 2U),
                  "next-position Hint completes through the same real worker");
    coordinator.Update();
    uint8_t hintFrom = 0U;
    uint8_t hintTo = 0U;
    const bool hintAvailable = stateMachine.TryGetHintMoveSquares(
        hintFrom, hintTo);
    results.Check(engine.startThinkCalls.load() == 2U &&
                      engine.maxConcurrentPolls.load() == 1U &&
                      engine.unsafeEngineAccesses.load() == 0U &&
                      hintAvailable && hintFrom == 40U && hintTo == 32U,
                  "normal restart/result access is single-owner and publishes only Y's Hint");

    std::printf(
        "[ASYNC_ENGINE_OWNERSHIP] case=cancel-before-ready "
        "pending_ms=%lld/%lld first_load=%u/%u retry=1 set_fen=%u "
        "unsafe=%u starts=%u completions=%u max_concurrent=%u "
        "hint_valid=%u hint=%u-%u x=%s y=%s accepted=%u\n",
        static_cast<long long>(cancelElapsedMs),
        static_cast<long long>(pendingRetryElapsedMs),
        loadedWhileActive ? 1U : 0U,
        repeatedLoadWhileActive ? 1U : 0U,
        static_cast<unsigned>(engine.setFenCalls.load()),
        static_cast<unsigned>(engine.unsafeEngineAccesses.load()),
        static_cast<unsigned>(engine.startThinkCalls.load()),
        static_cast<unsigned>(engine.pollCompletions.load()),
        static_cast<unsigned>(engine.maxConcurrentPolls.load()),
        hintAvailable ? 1U : 0U,
        static_cast<unsigned>(hintFrom),
        static_cast<unsigned>(hintTo),
        POSITION_X,
        coordinator.GetAuthoritativeFen(),
        !loadedWhileActive &&
                engine.setFenCalls.load() == baselineSetFen + 1U &&
                engine.unsafeEngineAccesses.load() == 0U &&
                engine.startThinkCalls.load() == 2U &&
                engine.pollCompletions.load() == 2U &&
                engine.maxConcurrentPolls.load() == 1U &&
                hintAvailable && hintFrom == 40U && hintTo == 32U
            ? 1U
            : 0U);
}

void CheckReadyBeforeCancel(
    fxe::test_support::TestResults& results) {
    constexpr const char* PATH =
        "build/test_async_engine_ownership_ready_before_cancel.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "ready-before-cancel settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = POSITION_X;

    ControlledHintEngine engine;
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(),
                  "ready-before-cancel coordinator load");
    ObserveExpectedBoard(stateMachine, coordinator);
    const uint32_t baselineSetFen = engine.setFenCalls.load();

    coordinator.Update();
    results.Check(engine.WaitForPollEntry(),
                  "ready-before-cancel Hint starts through coordinator");
    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker, 1U),
                  "X result becomes ready before the position change");
    for (uint32_t attempt = 0U;
         attempt < 1000000U && worker.IsActive();
         ++attempt) {
        std::this_thread::yield();
    }
    results.Check(!worker.IsActive(),
                  "worker publishes complete task cleanup before restart");

    const bool loaded = coordinator.LoadExternalFen(POSITION_Y);
    uint8_t hintFrom = 0U;
    uint8_t hintTo = 0U;
    const bool hintPublished =
        stateMachine.TryGetHintMoveSquares(hintFrom, hintTo);
    const bool accepted = loaded &&
        engine.setFenCalls.load() == baselineSetFen + 1U &&
        engine.unsafeEngineAccesses.load() == 0U &&
        std::strcmp(coordinator.GetAuthoritativeFen(), POSITION_Y) == 0 &&
        !hintPublished && !stateMachine.IsHintThinkingActive();
    results.Check(
        accepted,
        "ready X result is discarded rather than consumed during Y load");
    std::printf(
        "[ASYNC_ENGINE_OWNERSHIP] case=ready-before-cancel "
        "loaded=%u set_fen=%u unsafe=%u result_hint=%u "
        "worker_active=%u y=%s accepted=%u\n",
        loaded ? 1U : 0U,
        static_cast<unsigned>(engine.setFenCalls.load()),
        static_cast<unsigned>(engine.unsafeEngineAccesses.load()),
        hintPublished ? 1U : 0U,
        worker.IsActive() ? 1U : 0U,
        coordinator.GetAuthoritativeFen(),
        accepted ? 1U : 0U);
}

void CheckPendingMoveSurvivesCancellation(
    fxe::test_support::TestResults& results) {
    constexpr const char* PATH =
        "build/test_async_engine_ownership_pending_move.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "pending-move ownership settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = POSITION_X;

    ControlledHintEngine engine;
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(),
                  "pending-move ownership coordinator load");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence = 0U;
    uint64_t physical = 0ULL;
    const auto observe = [&](const uint64_t occupied) {
        sequence = dispatcher.DispatchBoardObservation({
            fxe::Millis(), sequence + 1U, physical ^ occupied, occupied});
        physical = occupied;
        coordinator.Update();
    };

    observe(POSITION_X_OCCUPANCY);
    coordinator.Update();
    results.Check(engine.WaitForPollEntry(),
                  "physical journey starts one long Hint from X");

    observe(POSITION_X_OCCUPANCY & ~(UINT64_C(1) << 36U));
    coordinator.UpdateLegalDestinationsMask();
    const uint64_t afterMoveOccupancy =
        (POSITION_X_OCCUPANCY & ~(UINT64_C(1) << 36U)) |
        (UINT64_C(1) << 28U);
    observe(afterMoveOccupancy);
    // Quiet moves pass through the production settle tick before exposing the
    // reducer action token, even when this fixture configures a zero duration.
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();

    const uint32_t pendingToken =
        stateMachine.GetChessInteractionModel().phase.pendingActionToken;
    results.Check(
        stateMachine.HasCommittedMovePending() && pendingToken != 0U &&
            coordinator.GetUndoHistoryMoveCount() == 0U &&
            std::strcmp(coordinator.GetAuthoritativeFen(), POSITION_X) == 0 &&
            engine.applyMoveCalls.load() == 0U &&
            engine.unsafeEngineAccesses.load() == 0U,
        "cancel-pending cadence leaves the one-shot move token unclaimed and X unchanged");

    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker),
                  "canceled Hint exits after the physical move is ready");
    coordinator.Update();

    uint8_t hintFrom = 0U;
    uint8_t hintTo = 0U;
    const bool hintPublished =
        stateMachine.TryGetHintMoveSquares(hintFrom, hintTo);
    const bool accepted =
        engine.applyMoveCalls.load() == 1U &&
        engine.unsafeEngineAccesses.load() == 0U &&
        coordinator.GetUndoHistoryMoveCount() == 1U &&
        std::strcmp(coordinator.GetAuthoritativeFen(),
                    POSITION_X_AFTER_E4_E5) == 0 &&
        stateMachine.GetExpectedOccupancyMask() == afterMoveOccupancy &&
        stateMachine.GetPhysicalOccupancyMask() == afterMoveOccupancy &&
        stateMachine.GetChessInteractionModel().phase.tag ==
            runtime::ChessInteractionPhase::Idle &&
        stateMachine.GetChessInteractionModel().phase.pendingActionToken == 0U &&
        !hintPublished;
    results.Check(
        accepted,
        "late quiescence commits the preserved move exactly once without stale Hint state");
    std::printf(
        "[ASYNC_ENGINE_OWNERSHIP] case=pending-physical-move "
        "events=%u token=%u apply=%u history=%u unsafe=%u "
        "physical=%016llx expected=%016llx hint=%u fen=%s accepted=%u\n",
        static_cast<unsigned>(sequence),
        static_cast<unsigned>(pendingToken),
        static_cast<unsigned>(engine.applyMoveCalls.load()),
        static_cast<unsigned>(coordinator.GetUndoHistoryMoveCount()),
        static_cast<unsigned>(engine.unsafeEngineAccesses.load()),
        static_cast<unsigned long long>(
            stateMachine.GetPhysicalOccupancyMask()),
        static_cast<unsigned long long>(
            stateMachine.GetExpectedOccupancyMask()),
        hintPublished ? 1U : 0U,
        coordinator.GetAuthoritativeFen(),
        accepted ? 1U : 0U);
}

void CheckNewGameControlTruthfullyRetries(
    fxe::test_support::TestResults& results) {
    constexpr const char* PATH =
        "build/test_async_engine_ownership_new_game.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "new-game ownership settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = POSITION_X;

    ControlledHintEngine engine;
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(),
                  "new-game ownership coordinator load");
    ObserveExpectedBoard(stateMachine, coordinator);
    const uint32_t baselineSetFen = engine.setFenCalls.load();
    const uint32_t baselineReset = engine.resetCalls.load();

    coordinator.Update();
    results.Check(engine.WaitForPollEntry(),
                  "new-game journey starts Hint through coordinator");

    const modules::ControlCoordinatorResult firstResult =
        coordinator.RequestControlNewGame(41U);
    const modules::ControlCoordinatorResult repeatedResult =
        coordinator.PollControlAction(41U);
    const bool directResetWhileActive = coordinator.ResetEngine();
    results.Check(
        firstResult == modules::ControlCoordinatorResult::Pending &&
            repeatedResult == modules::ControlCoordinatorResult::Pending &&
            !directResetWhileActive &&
            engine.resetCalls.load() == baselineReset &&
            engine.setFenCalls.load() == baselineSetFen &&
            engine.unsafeEngineAccesses.load() == 0U &&
            std::strcmp(coordinator.GetAuthoritativeFen(), POSITION_X) == 0 &&
            stateMachine.IsHintThinkingActive(),
        "New Game remains pending and preserves the current game while cancellation drains");

    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker),
                  "new-game cancellation eventually exits");
    const modules::ControlCoordinatorResult retryResult =
        coordinator.PollControlAction(41U);
    uint8_t hintFrom = 0U;
    uint8_t hintTo = 0U;
    const bool hintPublished =
        stateMachine.TryGetHintMoveSquares(hintFrom, hintTo);
    const bool accepted =
        retryResult == modules::ControlCoordinatorResult::Applied &&
        engine.resetCalls.load() == baselineReset + 1U &&
        engine.setFenCalls.load() == baselineSetFen + 1U &&
        engine.unsafeEngineAccesses.load() == 0U &&
        std::strcmp(coordinator.GetAuthoritativeFen(),
                    modules::INITIAL_FEN) == 0 &&
        !hintPublished && !stateMachine.IsHintThinkingActive();
    results.Check(
        accepted,
        "one New Game request resets exactly once after quiescence without publishing the stale Hint");
    std::printf(
        "[ASYNC_ENGINE_OWNERSHIP] case=new-game-control "
        "first=%u repeated=%u direct=%u retry=%u reset=%u set_fen=%u unsafe=%u "
        "hint=%u fen=%s accepted=%u\n",
        static_cast<unsigned>(firstResult),
        static_cast<unsigned>(repeatedResult),
        directResetWhileActive ? 1U : 0U,
        static_cast<unsigned>(retryResult),
        static_cast<unsigned>(engine.resetCalls.load()),
        static_cast<unsigned>(engine.setFenCalls.load()),
        static_cast<unsigned>(engine.unsafeEngineAccesses.load()),
        hintPublished ? 1U : 0U,
        coordinator.GetAuthoritativeFen(),
        accepted ? 1U : 0U);
}

void CheckLoadOrInitializeReuseRetries(
    fxe::test_support::TestResults& results) {
    constexpr const char* PATH =
        "build/test_async_engine_ownership_reinitialize.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "reinitialize ownership settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = POSITION_X;

    ControlledHintEngine engine;
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(),
                  "reinitialize ownership coordinator load");
    ObserveExpectedBoard(stateMachine, coordinator);
    const uint32_t baselineSetFen = engine.setFenCalls.load();
    const uint32_t baselineReset = engine.resetCalls.load();

    coordinator.Update();
    results.Check(engine.WaitForPollEntry(),
                  "reinitialize journey starts Hint through coordinator");
    const bool firstLoad = coordinator.LoadOrInitialize();
    const bool repeatedLoad = coordinator.LoadOrInitialize();
    results.Check(
        !firstLoad && !repeatedLoad &&
            engine.resetCalls.load() == baselineReset &&
            engine.setFenCalls.load() == baselineSetFen &&
            engine.unsafeEngineAccesses.load() == 0U &&
            std::strcmp(coordinator.GetAuthoritativeFen(), POSITION_X) == 0 &&
            stateMachine.IsHintThinkingActive(),
        "reinitialize remains non-mutating and retryable while cancellation is pending");

    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker),
                  "reinitialize cancellation eventually exits");
    const bool retryLoad = coordinator.LoadOrInitialize();
    uint8_t hintFrom = 0U;
    uint8_t hintTo = 0U;
    const bool hintPublished =
        stateMachine.TryGetHintMoveSquares(hintFrom, hintTo);
    const bool accepted = retryLoad &&
        engine.resetCalls.load() == baselineReset &&
        engine.setFenCalls.load() == baselineSetFen + 1U &&
        engine.unsafeEngineAccesses.load() == 0U &&
        std::strcmp(coordinator.GetAuthoritativeFen(), POSITION_X) == 0 &&
        !hintPublished && !stateMachine.IsHintThinkingActive();
    results.Check(
        accepted,
        "reinitialize retry restores once after quiescence without stale search output");
    std::printf(
        "[ASYNC_ENGINE_OWNERSHIP] case=reinitialize "
        "first=%u repeated=%u retry=%u reset=%u set_fen=%u unsafe=%u "
        "hint=%u fen=%s accepted=%u\n",
        firstLoad ? 1U : 0U,
        repeatedLoad ? 1U : 0U,
        retryLoad ? 1U : 0U,
        static_cast<unsigned>(engine.resetCalls.load()),
        static_cast<unsigned>(engine.setFenCalls.load()),
        static_cast<unsigned>(engine.unsafeEngineAccesses.load()),
        hintPublished ? 1U : 0U,
        coordinator.GetAuthoritativeFen(),
        accepted ? 1U : 0U);
}

void CheckClockTimeoutWaitsForQuiescence(
    fxe::test_support::TestResults& results) {
    constexpr const char* PATH =
        "build/test_async_engine_ownership_clock_timeout.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "clock-timeout ownership settings load");
    settings.Doc()["TIMER"] = 1U;

    // Let the opening-side Hint finish normally; block the new side's Hint
    // after e4-e5 so the real clock timeout collides with active worker use.
    ControlledHintEngine engine(2U);
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(),
                  "clock-timeout ownership coordinator load");
    results.Check(coordinator.LoadExternalFen(POSITION_X),
                  "clock-timeout ownership position loads as a live boundary");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence = 0U;
    uint64_t physical = 0ULL;
    const auto observe = [&](const uint64_t occupied) {
        sequence = dispatcher.DispatchBoardObservation({
            fxe::Millis(), sequence + 1U, physical ^ occupied, occupied});
        physical = occupied;
        coordinator.Update();
    };

    observe(POSITION_X_OCCUPANCY);
    results.Check(engine.WaitForWorkerCompletion(worker, 1U),
                  "opening-side Hint completes before clock fixture move");
    coordinator.Update();
    fxe::ManualMillis::AdvanceMs(750U);

    observe(POSITION_X_OCCUPANCY & ~(UINT64_C(1) << 36U));
    coordinator.UpdateLegalDestinationsMask();
    const uint64_t afterMoveOccupancy =
        (POSITION_X_OCCUPANCY & ~(UINT64_C(1) << 36U)) |
        (UINT64_C(1) << 28U);
    observe(afterMoveOccupancy);
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();
    results.Check(
        coordinator.GetUndoHistoryMoveCount() == 1U &&
            coordinator.GetGameClock().IsRunning() &&
            engine.startThinkCalls.load() == 2U &&
            engine.WaitForPollEntry(),
        "committed move starts the next side's blocked Hint and live clock");

    // The commit begins while physical occupancy still differs from the
    // pre-action acknowledgement, so the clock was paused at that tick's
    // opening. One ordinary cadence reconciles the acknowledged destination
    // and resumes the new side without consuming search output.
    coordinator.Update();
    results.Check(!coordinator.GetGameClock().IsPaused(),
                  "post-commit cadence resumes the acknowledged position's clock");

    fxe::ManualMillis::AdvanceMs(60000U);
    coordinator.Update();
    results.Check(
        coordinator.HasClockTerminalOutcome() && worker.IsActive() &&
            stateMachine.IsHintThinkingActive() &&
            engine.unsafeEngineAccesses.load() == 0U,
        "clock result latches immediately while board repair waits for worker quiescence");
    coordinator.Update();
    results.Check(
        worker.IsActive() && engine.unsafeEngineAccesses.load() == 0U,
        "repeated terminal cadence remains nonblocking and engine-read-free");

    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker),
                  "clock-timeout cancellation eventually exits");
    coordinator.Update();
    uint8_t hintFrom = 0U;
    uint8_t hintTo = 0U;
    const bool hintPublished =
        stateMachine.TryGetHintMoveSquares(hintFrom, hintTo);
    const bool accepted =
        coordinator.HasClockTerminalOutcome() &&
        coordinator.GetClockTerminalWinner() == modules::GameWinner::White &&
        !worker.IsActive() && !stateMachine.IsHintThinkingActive() &&
        !hintPublished && engine.unsafeEngineAccesses.load() == 0U &&
        engine.applyMoveCalls.load() == 1U &&
        coordinator.GetUndoHistoryMoveCount() == 1U &&
        std::strcmp(coordinator.GetAuthoritativeFen(),
                    POSITION_X_AFTER_E4_E5) == 0;
    results.Check(
        accepted,
        "post-quiescence timeout repair preserves the committed game and discards stale Hint output");
    std::printf(
        "[ASYNC_ENGINE_OWNERSHIP] case=clock-timeout "
        "events=%u terminal=%u winner=%u apply=%u history=%u unsafe=%u "
        "worker=%u hint=%u fen=%s accepted=%u\n",
        static_cast<unsigned>(sequence),
        coordinator.HasClockTerminalOutcome() ? 1U : 0U,
        static_cast<unsigned>(coordinator.GetClockTerminalWinner()),
        static_cast<unsigned>(engine.applyMoveCalls.load()),
        static_cast<unsigned>(coordinator.GetUndoHistoryMoveCount()),
        static_cast<unsigned>(engine.unsafeEngineAccesses.load()),
        worker.IsActive() ? 1U : 0U,
        hintPublished ? 1U : 0U,
        coordinator.GetAuthoritativeFen(),
        accepted ? 1U : 0U);
}

void CheckFastFinalUndoRearmWaitsForQuiescence(
    fxe::test_support::TestResults& results) {
    constexpr const char* PATH =
        "build/test_async_engine_ownership_final_undo.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "final-Undo ownership settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = POSITION_X;

    // Block the opening Hint so the physical move cancels and discards it;
    // this prevents a cached X Hint from masking the post-Undo restart. The
    // test selects that later restart after observing the intervening search.
    ControlledHintEngine engine(1U);
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(),
                  "final-Undo ownership coordinator load");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence = 0U;
    uint64_t physical = 0ULL;
    const auto dispatchOnly = [&](const uint64_t occupied) {
        sequence = dispatcher.DispatchBoardObservation({
            fxe::Millis(), sequence + 1U, physical ^ occupied, occupied});
        physical = occupied;
    };
    const auto observe = [&](const uint64_t occupied) {
        dispatchOnly(occupied);
        coordinator.Update();
    };
    const auto consumeCompletedSearch = [&]() {
        // A move commit intentionally suppresses same-cadence replacement
        // search; the following cadence starts it.
        coordinator.Update();
        const uint32_t started = engine.startThinkCalls.load();
        if (started > 0U) {
            results.Check(engine.WaitForWorkerCompletion(worker, started),
                          "ordinary lifecycle Hint completes");
            coordinator.Update();
        }
    };

    observe(POSITION_X_OCCUPANCY);
    results.Check(engine.WaitForPollEntry(),
                  "opening Hint owns the engine before the fixture move");

    dispatchOnly(POSITION_X_OCCUPANCY &
                 ~(UINT64_C(1) << 36U));
    coordinator.UpdateLegalDestinationsMask();
    const uint64_t afterMoveOccupancy =
        (POSITION_X_OCCUPANCY & ~(UINT64_C(1) << 36U)) |
        (UINT64_C(1) << 28U);
    dispatchOnly(afterMoveOccupancy);
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();
    results.Check(
        coordinator.GetUndoHistoryMoveCount() == 0U && worker.IsActive(),
        "fixture move remains unclaimed while opening Hint cancellation is pending");
    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker),
                  "opening Hint cancellation exits before fixture commit");
    coordinator.Update();
    consumeCompletedSearch();
    results.Check(
        coordinator.GetUndoHistoryMoveCount() == 1U &&
            std::strcmp(coordinator.GetAuthoritativeFen(),
                        POSITION_X_AFTER_E4_E5) == 0,
        "final-Undo fixture commits one move before inverse journey");

    const uint32_t rewindSetFenBaseline = engine.setFenCalls.load();
    const uint32_t blockedStart =
        engine.startThinkCalls.load() + 1U;
    engine.BlockStartNumber(blockedStart);
    results.Check(coordinator.RequestUndo(1U),
                  "final-Undo request opens inverse physical plan");

    // The human completes the inverse before the next coordinator cadence.
    // That cadence consumes the one-shot Undo action, returns to X, and starts
    // a new Hint while the completed-Undo close window is still retained.
    dispatchOnly(afterMoveOccupancy & ~(UINT64_C(1) << 28U));
    dispatchOnly(POSITION_X_OCCUPANCY);
    fxe::ManualMillis::AdvanceMs(1U);
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();
    results.Check(
        engine.startThinkCalls.load() + 1U == blockedStart &&
            !worker.IsActive() &&
            coordinator.GetUndoHistoryMoveCount() == 0U &&
            engine.setFenCalls.load() == rewindSetFenBaseline + 1U &&
            std::strcmp(coordinator.GetAuthoritativeFen(), POSITION_X) == 0,
        "fast physical final Undo rewinds once and suppresses same-cadence Hint restart");

    // The next cadence closes/rearms the completed final-Undo window before
    // it can start the blocked replacement Hint.
    coordinator.Update();
    results.Check(
        engine.startThinkCalls.load() == blockedStart &&
            engine.WaitForPollEntry() && worker.IsActive(),
        "post-Undo Hint starts only after the close/rearm cadence is safe");
    coordinator.Update();
    const uint32_t unsafeBeforeRelease =
        engine.unsafeEngineAccesses.load();
    results.Check(
        unsafeBeforeRelease == 0U && worker.IsActive() &&
            engine.setFenCalls.load() == rewindSetFenBaseline + 1U &&
            std::strcmp(coordinator.GetAuthoritativeFen(), POSITION_X) == 0 &&
            stateMachine.GetPhysicalOccupancyMask() == POSITION_X_OCCUPANCY &&
            stateMachine.GetExpectedOccupancyMask() == POSITION_X_OCCUPANCY,
        "completed final-Undo close remains exact and engine-read-free while Hint cancellation is pending");

    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker),
                  "post-Undo Hint cancellation eventually exits");
    coordinator.Update();
    if (engine.startThinkCalls.load() > blockedStart) {
        results.Check(
            engine.WaitForWorkerCompletion(worker, 
                engine.startThinkCalls.load()),
            "post-rearm Hint restart completes without overlap");
        coordinator.Update();
    }

    const modules::BeginnerModeController beginnerMode;
    modules::HomeStateNotification home =
        modules::BuildHomeStateSnapshot(
            stateMachine, coordinator, settings,
            beginnerMode,
            modules::PersistenceDegradation::None);
    uint8_t hintFrom = 0U;
    uint8_t hintTo = 0U;
    const bool hintPublished =
        stateMachine.TryGetHintMoveSquares(hintFrom, hintTo);
    const bool hintMatchesRestoredX =
        !hintPublished || (hintFrom == 48U && hintTo == 40U);
    const bool accepted =
        unsafeBeforeRelease == 0U &&
        engine.unsafeEngineAccesses.load() == 0U &&
        engine.maxConcurrentPolls.load() == 1U &&
        engine.setFenCalls.load() == rewindSetFenBaseline + 1U &&
        coordinator.GetUndoHistoryMoveCount() == 0U &&
        std::strcmp(coordinator.GetAuthoritativeFen(), POSITION_X) == 0 &&
        stateMachine.GetPhysicalOccupancyMask() == POSITION_X_OCCUPANCY &&
        stateMachine.GetExpectedOccupancyMask() == POSITION_X_OCCUPANCY &&
        home.fen != nullptr && std::strcmp(home.fen, POSITION_X) == 0 &&
        home.occupiedMask == POSITION_X_OCCUPANCY &&
        hintMatchesRestoredX;
    results.Check(
        accepted,
        "late quiescence closes final Undo once with atomic board/Home authority and no stale Hint");
    std::printf(
        "[ASYNC_ENGINE_OWNERSHIP] case=fast-final-undo "
        "events=%u blocked_start=%u starts=%u completions=%u "
        "set_fen_delta=%u history=%u unsafe_before=%u unsafe=%u "
        "max_concurrent=%u physical=%016llx expected=%016llx "
        "home=%016llx hint=%u fen=%s accepted=%u\n",
        static_cast<unsigned>(sequence),
        static_cast<unsigned>(blockedStart),
        static_cast<unsigned>(engine.startThinkCalls.load()),
        static_cast<unsigned>(engine.pollCompletions.load()),
        static_cast<unsigned>(
            engine.setFenCalls.load() - rewindSetFenBaseline),
        static_cast<unsigned>(coordinator.GetUndoHistoryMoveCount()),
        static_cast<unsigned>(unsafeBeforeRelease),
        static_cast<unsigned>(engine.unsafeEngineAccesses.load()),
        static_cast<unsigned>(engine.maxConcurrentPolls.load()),
        static_cast<unsigned long long>(
            stateMachine.GetPhysicalOccupancyMask()),
        static_cast<unsigned long long>(
            stateMachine.GetExpectedOccupancyMask()),
        static_cast<unsigned long long>(home.occupiedMask),
        hintPublished ? 1U : 0U,
        coordinator.GetAuthoritativeFen(),
        accepted ? 1U : 0U);
}

void CheckStartOnlySettingAdmitsBackgroundHint(
    fxe::test_support::TestResults& results) {
    constexpr const char* PATH =
        "build/test_async_engine_ownership_start_only_setting.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "start-only ownership settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = modules::INITIAL_FEN;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;

    ControlledHintEngine engine;
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(),
                  "start-only ownership coordinator load");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    (void)dispatcher.DispatchBoardObservation({
        fxe::Millis(), 1U, INITIAL_OCCUPANCY, INITIAL_OCCUPANCY});
    coordinator.Update();
    results.Check(engine.WaitForPollEntry() && worker.IsActive(),
                  "initial-position Hint owns engine before setting request");

    modules::ControlMutationCallbacks callbacks;
    callbacks.context = &coordinator;
    callbacks.checkStartOnlyGate = [](void* context, const char* key) {
        return static_cast<modules::GamePersistenceCoordinator*>(context)
            ->CheckControlStartOnlyGate(key);
    };
    modules::ControlMutationService controlMutation;
    controlMutation.Configure(settings, callbacks);
    StaticJsonDocument<16> value;
    const uint32_t revisionBefore = settings.GetRevision();
    const uint32_t resetBefore = engine.resetCalls.load();
    results.Check(coordinator.CheckControlStartOnlyGate("BOARD_ROTATE") ==
                      modules::ControlCoordinatorResult::Applied,
                  "fresh board rotation is available during owned background Hint");
    value.set(3U);
    const auto timer = controlMutation.MutateSetting(
        "TIMER", value.as<JsonVariantConst>(), 74U);
    coordinator.Update();
    results.Check(
        timer.status == modules::ControlMutationStatus::Applied &&
            settings.ReadValue("TIMER", 0U) == 3U &&
            settings.GetRevision() == revisionBefore + 1U && worker.IsActive() &&
            engine.unsafeEngineAccesses.load() == 0U &&
            !coordinator.GetGameClock().IsRunning() &&
            coordinator.GetGameClock().GetRemainingMs(runtime::ClockSide::White) == 300000U &&
            coordinator.GetGameClock().GetRemainingMs(runtime::ClockSide::Black) == 300000U,
        "TIMER changes during owned Hint search without engine access or starting time");

    value.set(2U);
    const auto first = controlMutation.MutateSetting(
        "GAME_FIRST_PLAYER", value.as<JsonVariantConst>(), 71U);
    const auto repeated = controlMutation.MutateSetting(
        "GAME_FIRST_PLAYER", value.as<JsonVariantConst>(), 72U);
    results.Check(
        first.status == modules::ControlMutationStatus::Applied &&
            repeated.status == modules::ControlMutationStatus::Noop &&
            settings.ReadValue("GAME_FIRST_PLAYER", 1U) == 2U &&
            settings.GetRevision() == revisionBefore + 2U && worker.IsActive() &&
            engine.unsafeEngineAccesses.load() == 0U,
        "background Hint admits and persists fresh player placement without retransmission");
    coordinator.Update();
    coordinator.Update();
    results.Check(worker.IsActive() && engine.resetCalls.load() == resetBefore &&
                      engine.unsafeEngineAccesses.load() == 0U,
                  "admitted player placement waits for worker quiescence before engine projection");
    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker),
                  "setting projection cancellation releases Hint");
    coordinator.Update();
    results.Check(engine.resetCalls.load() == resetBefore + 1U &&
                      engine.unsafeEngineAccesses.load() == 0U &&
                      settings.ReadValue("GAME_FIRST_PLAYER", 1U) == 2U &&
                      stateMachine.IsKingQueenSwapPromptActive(),
                  "quiescent projection applies the admitted player placement exactly once");
}

void CheckAdmittedFirstPlayerIntentRetriesAfterWorker(
    fxe::test_support::TestResults& results) {
    constexpr const char* PATH =
        "build/test_async_engine_ownership_admitted_setting.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "admitted-setting ownership load");
    settings.Doc()[modules::KEY_GAME_FEN] = modules::INITIAL_FEN;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;

    ControlledHintEngine engine;
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(),
                  "admitted-setting coordinator load");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    (void)dispatcher.DispatchBoardObservation({
        fxe::Millis(), 1U, INITIAL_OCCUPANCY, INITIAL_OCCUPANCY});
    coordinator.Update();
    results.Check(engine.WaitForPollEntry() && worker.IsActive(),
                  "admitted-setting fixture owns engine with Hint");

    // Simulate the narrow inter-task race after a successful control gate but
    // before the coordinator observes the already-written latest setting.
    settings.Doc()["GAME_FIRST_PLAYER"] = 2U;
    const uint32_t resetBefore = engine.resetCalls.load();
    coordinator.Update();
    coordinator.Update();
    results.Check(
        worker.IsActive() && engine.resetCalls.load() == resetBefore &&
            engine.unsafeEngineAccesses.load() == 0U,
        "already-admitted latest setting remains pending without consuming its marker");

    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker),
                  "admitted-setting Hint cancellation eventually exits");
    coordinator.Update();
    const bool accepted =
        engine.resetCalls.load() == resetBefore + 1U &&
        engine.unsafeEngineAccesses.load() == 0U &&
        settings.ReadValue(
            "GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U &&
        std::strcmp(coordinator.GetAuthoritativeFen(),
                    modules::INITIAL_FEN) == 0 &&
        stateMachine.IsKingQueenSwapPromptActive();
    results.Check(
        accepted,
        "post-quiescence retry applies the admitted latest setting and its physical correction once");
    std::printf(
        "[ASYNC_ENGINE_OWNERSHIP] case=admitted-start-only-setting "
        "reset_delta=%u unsafe=%u first_player=%u kq_prompt=%u "
        "fen=%s accepted=%u\n",
        static_cast<unsigned>(
            engine.resetCalls.load() - resetBefore),
        static_cast<unsigned>(engine.unsafeEngineAccesses.load()),
        static_cast<unsigned>(settings.ReadValue(
            "GAME_FIRST_PLAYER", static_cast<uint32_t>(1U))),
        stateMachine.IsKingQueenSwapPromptActive() ? 1U : 0U,
        coordinator.GetAuthoritativeFen(),
        accepted ? 1U : 0U);
}

void CheckProgressiveHintWindowAndStages(
    fxe::test_support::TestResults& results, const bool automatic,
    const uint8_t manualCeiling, const bool earlyCompletion,
    const bool duck = false) {
    constexpr const char* PATH = "build/test_progressive_hint_window.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "progressive Hint settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = POSITION_X;
    settings.Doc()["HINT_LEVEL"] = manualCeiling;
    settings.Doc()["P1_PIECE_WHEEL"] = 0U;
    if (duck) {
        modules::GamePersistenceState state;
        (void)state.fen.Assign(POSITION_X);
        (void)state.undoBaseFen.Assign(POSITION_X);
        modules::DuckGameDurableState durable;
        durable.variant = modules::GameVariant::Duck;
        durable.duckSquare = 24U;
        char encoded[32] = {};
        results.Check(modules::EncodeDuckGameDurableState(
                          durable, encoded, sizeof(encoded)) &&
                          state.variantState.Assign(encoded) &&
                          modules::WriteGamePersistenceState(settings.Doc(), state),
                      "Duck Hint fixture uses the existing durable variant codec");
    }
    // One corpus crosses millis wrap; no search timing depends on wall time.
    const uint32_t startMs = automatic ? UINT32_MAX - 200U : 10000U;
    fxe::ManualMillis::SetNowMs(startMs);
    ControlledHintEngine engine;
    engine.refineBest = true;
    runtime::GameStateMachine sm(MakeStateMachineConfig());
    sm.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, sm, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(), "progressive Hint coordinator load");
    coordinator.SetCoachingHintActive(automatic);
    runtime::InteractionDispatcher dispatcher(sm, coordinator);
    uint32_t sequence = 0U;
    uint64_t physical = 0ULL;
    const auto observe = [&](const uint64_t occupied) {
        sequence = dispatcher.DispatchBoardObservation({
            fxe::Millis(), sequence + 1U, physical ^ occupied, occupied});
        physical = occupied;
        coordinator.Update();
    };
    const auto hintIs = [&](const uint8_t expectedFrom, const uint8_t expectedTo) {
        uint8_t from = 64U;
        uint8_t to = 64U;
        return sm.TryGetHintMoveSquares(from, to) &&
               from == expectedFrom && to == expectedTo;
    };
    const uint64_t baselineOccupancy = POSITION_X_OCCUPANCY |
        (duck ? UINT64_C(1) << 24U : 0ULL);
    // Search completion is controlled; everything after the completed typed
    // result uses the actual coordinator, reducer, snapshot, composer, S3
    // budget pipeline and existing VirtualLitch RGBW capture renderer.
    // A requested manual Hint supplies display intent here. Raw O ownership
    // and Hall/orientation mapping are covered by the companion whole journey.
    const auto hintFrame = [&](const bool showHint, const char* checkpoint,
                               const uint8_t targetSquare,
                               std::array<modules::Rgb, 64U>* composedSquares = nullptr) {
        const auto frame = CaptureSquareFrame(settings, coordinator, sm, showHint, composedSquares);
        const auto pixel = frame[targetSquare];
        if (checkpoint != nullptr) {
            uint8_t hintFrom = 64U;
            uint8_t hintTo = 64U;
            const bool targetHint = sm.TryGetHintMoveSquares(hintFrom, hintTo) &&
                                    hintTo == targetSquare;
            std::printf("[PROGRESSIVE_HELD_RGBW] variant=%s automatic=%u early=%u "
                        "checkpoint=%s elapsed=%lu phase=%u selected=%u "
                        "hint=%u actual=%u-%u target=%u rgbw=%u,%u,%u,%u\n",
                        duck ? "Duck" : "Classic", automatic, earlyCompletion,
                        checkpoint, static_cast<unsigned long>(fxe::Millis() - startMs),
                        static_cast<unsigned>(sm.GetChessInteractionModel().phase.tag),
                        sm.GetChessInteractionModel().phase.selectedSquare,
                        targetHint, hintFrom, hintTo, targetSquare,
                        pixel.r, pixel.g, pixel.b, pixel.w);
        }
        return frame;
    };
    const auto steadyLegalColor = [&](const uint8_t square) {
        std::array<modules::Rgb, 64U> steady{};
        // A visible Hint gives its landing rail full strength while the
        // ordinary invitation can still be revealing. Use a steady copy for
        // the exact target reference without advancing this worker's clock.
        (void)CaptureSquareFrame(settings, coordinator, sm, false, &steady,
                                 OrdinaryLegalRailSample::SteadyReference);
        return steady[square];
    };
    observe(baselineOccupancy);
    results.Check(engine.WaitForPollEntry() && worker.IsActive(),
                  "initial human Hint starts on the real worker at turn entry");
    results.Check(engine.lastConfig.continuousAnalysis &&
                      engine.lastConfig.maxThinkMs == 90000U &&
                      engine.lastConfig.maxNodes == 0U &&
                      engine.lastConfig.maxDepth == 0U,
                  "hint has one90second analysis window without old level/node caps");
    const uint32_t cancelBefore = engine.cancelThinkCalls.load();
    const uint32_t getterBefore = engine.classicResultReads.load();
    fxe::ManualMillis::SetNowMs(startMs + (earlyCompletion ? 50U : 499U));
    if (!automatic) coordinator.RequestHintToggle();
    else sm.SetHintDisplayRequested(true); // Existing Coaching visibility adapter; no manual request.
    coordinator.Update();
    results.Check(!hintIs(48U, 40U) && worker.IsActive(),
                  "manual O and automatic Coaching retain pending intent before the common500ms scored-root boundary");
    observe(baselineOccupancy & ~(UINT64_C(1) << 48U));
    coordinator.UpdateLegalDestinationsMask();
    fxe::ManualMillis::SetNowMs(startMs + 500U);
    results.Check(engine.PublishCandidate(1U), "worker publishes a legal scored first root at500ms");
    coordinator.Update();
    results.Check(hintIs(48U, 40U) && worker.IsActive(),
                  "the first legal target is available by500ms during its real pickup");
    std::array<modules::Rgb, 64U> shownColors{}, hiddenColors{};
    const auto shown = hintFrame(true, "live-first-held", 40U, &shownColors);
    (void)hintFrame(false, nullptr, 40U, &hiddenColors);
    const auto firstSteady = steadyLegalColor(40U);
    // Hint adds other LEDs, so the final whole-frame power scale can change.
    // Compare target strength before that scale and visibility after it.
    results.Check(shownColors[40U].r == firstSteady.r &&
                      shownColors[40U].g == firstSteady.g &&
                      shownColors[40U].b == firstSteady.b &&
                      !SamePixel(shown[40U], {}) &&
                      shownColors[41U].r == hiddenColors[41U].r &&
                      shownColors[41U].g == hiddenColors[41U].g &&
                      shownColors[41U].b == hiddenColors[41U].b,
                  "first live Hint preserves its full opposite target rail while b3 retains ordinary pre-budget output");
    fxe::ManualMillis::SetNowMs(startMs + 1000U);
    coordinator.Update();
    results.Check(worker.IsActive() && hintIs(48U, 40U),
                  "one-second request window retains actual calculated hint");
    results.Check(engine.PublishCandidate(2U), "same worker publishes refined actual best");
    fxe::ManualMillis::SetNowMs(startMs + 3000U);
    coordinator.Update();
    results.Check(hintIs(48U, 40U), "refinement cannot retarget a held piece");
    coordinator.SetBoardSettingsActive(true);
    observe(baselineOccupancy);
    results.Check(hintIs(48U, 40U), "Quick Setup retains prior physical guidance");
    coordinator.SetBoardSettingsActive(false);
    coordinator.Update();
    results.Check(hintIs(36U, 28U), "Idle receives latest completed candidate without ending search");
    observe(baselineOccupancy & ~(UINT64_C(1) << 36U));
    coordinator.UpdateLegalDestinationsMask();
    const auto& latePhase = sm.GetChessInteractionModel().phase;
    results.Check(latePhase.tag == runtime::ChessInteractionPhase::PieceLifted &&
                      latePhase.selectedSquare == 36U &&
                      ((sm.GetValidDestMask() >> 28U) & 1ULL) != 0ULL,
                  "later pickup reaches its physical held-piece legal-target phase");
    results.Check(hintIs(36U, 28U),
                  "later pickup retains the already completed refinement");
    results.Check(sm.IsHintDisplayRequested(),
                  "later pickup retains the actual Hint display request");
    const auto lateShown = hintFrame(true, "late-refined-held", 28U, &shownColors);
    const auto lateHidden = hintFrame(false, nullptr, 28U, &hiddenColors);
    const auto lateSteady = steadyLegalColor(28U);
    std::printf("[PROGRESSIVE_HELD_COMPARE] variant=%s target_raw=%u,%u,%u/%u,%u,%u "
                "neighbor_raw=%u,%u,%u/%u,%u,%u neighbor_mapped=%u,%u,%u/%u,%u,%u legal=%016llx\n",
                duck ? "Duck" : "Classic",
                shownColors[28U].r, shownColors[28U].g, shownColors[28U].b,
                lateSteady.r, lateSteady.g, lateSteady.b,
                shownColors[29U].r, shownColors[29U].g, shownColors[29U].b,
                hiddenColors[29U].r, hiddenColors[29U].g, hiddenColors[29U].b,
                lateShown[29U].r, lateShown[29U].g, lateShown[29U].b,
                lateHidden[29U].r, lateHidden[29U].g, lateHidden[29U].b,
                static_cast<unsigned long long>(sm.GetValidDestMask()));
    results.Check(shownColors[28U].r == lateSteady.r &&
                      shownColors[28U].g == lateSteady.g &&
                      shownColors[28U].b == lateSteady.b &&
                      !SamePixel(lateShown[28U], {}) &&
                      shownColors[29U].r == hiddenColors[29U].r &&
                      shownColors[29U].g == hiddenColors[29U].g &&
                      shownColors[29U].b == hiddenColors[29U].b,
                  "later pickup keeps its refined opposite target rail full and nonroute f5 ordinary before power scaling");
    observe(baselineOccupancy);
    if (sm.IsHintDisplayRequested()) coordinator.RequestHintToggle();
    const auto requestStarted = std::chrono::steady_clock::now();
    coordinator.RequestHintToggle();
    const auto requestDuration = std::chrono::steady_clock::now() - requestStarted;
    results.Check(hintIs(36U, 28U) && worker.IsActive() &&
                      requestDuration < std::chrono::milliseconds(100) &&
                      engine.cancelThinkCalls.load() == cancelBefore &&
                      engine.classicResultReads.load() == getterBefore,
                  "explicit request reads latest active snapshot immediately without cancel/live getter");
    for (const uint32_t waited : {10000U, 30000U, 89999U}) {
        fxe::ManualMillis::SetNowMs(startMs + waited);
        coordinator.Update();
        results.Check(worker.IsActive() && engine.startThinkCalls.load() == 1U &&
                          hintIs(36U, 28U),
                      "one background search remains active through10s30sandjustbefore90s");
    }
    observe(baselineOccupancy & ~(UINT64_C(1) << 36U));
    fxe::ManualMillis::SetNowMs(startMs + 90000U);
    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker, 1U),
                  "controlled deadline publishes terminal result through real worker cleanup");
    coordinator.Update();
    fxe::ManualMillis::AdvanceMs(10000U);
    uint8_t terminalProgress = 0U;
    modules::UciMove terminalBest;
    uint32_t terminalElapsed = 0U;
    uint32_t terminalNodes = 0U;
    (void)worker.GetResult(terminalProgress, terminalBest,
                           terminalElapsed, terminalNodes);
    results.Check(terminalElapsed == 90000U && terminalNodes == 2000U,
                  "held guidance after completion does not inflate search time or nodes");
    observe(baselineOccupancy);
    coordinator.Update();
    results.Check(!worker.IsActive() && engine.startThinkCalls.load() == 1U &&
                      hintIs(36U, 28U) && engine.unsafeEngineAccesses.load() == 0U,
                  "90second completion retains best and never restarts on same position");
    coordinator.RequestCancelHintWorkAndRestart();
    coordinator.Update();
    results.Check(!hintIs(36U, 28U), "explicit invalidation removes stale published guidance");
    engine.ReleaseFirstPoll();
    (void)engine.WaitForWorkerCompletion(worker);
    (void)worker.CancelAndJoin(engine);
    std::printf("[LIVE_HINT_WINDOW] variant=%s automatic=%u config_level=%u "
                "first_ms=%lu deadline_ms=90000 unsafe=%lu\n",
                duck ? "Duck" : "Classic", automatic, manualCeiling,
                static_cast<unsigned long>(500U),
                static_cast<unsigned long>(engine.unsafeEngineAccesses.load()));
}

void CheckHintCandidateAcrossPickupTimings(
    fxe::test_support::TestResults& results,
    const uint32_t pickupAtMs) {
    constexpr const char* PATH = "build/test_hint_pickup_timing.json";
    RemoveSettingsFiles(PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    results.Check(settings.Load(), "pickup-timing Hint settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = POSITION_X;
    settings.Doc()["HINT_LEVEL"] = 8U;
    settings.Doc()["P1_PIECE_WHEEL"] = 0U;

    fxe::ManualMillis::SetNowMs(0U);
    ControlledHintEngine engine;
    engine.refineBest = true;
    runtime::GameStateMachine stateMachine(MakeStateMachineConfig());
    stateMachine.Reset();
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine, nullptr, &worker);
    results.Check(coordinator.LoadOrInitialize(),
                  "pickup-timing Hint coordinator load");
    coordinator.SetCoachingHintActive(true);
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence = 0U;
    uint64_t physical = 0ULL;
    const auto observe = [&](const uint64_t occupied) {
        sequence = dispatcher.DispatchBoardObservation({
            fxe::Millis(), sequence + 1U, physical ^ occupied, occupied});
        physical = occupied;
        coordinator.Update();
    };
    const auto hintIs = [&](const uint8_t expectedFrom,
                            const uint8_t expectedTo) {
        uint8_t from = 64U;
        uint8_t to = 64U;
        return stateMachine.TryGetHintMoveSquares(from, to) &&
               from == expectedFrom && to == expectedTo;
    };

    const uint64_t baselineOccupancy = POSITION_X_OCCUPANCY;
    observe(baselineOccupancy);
    results.Check(engine.WaitForPollEntry() && worker.IsActive(),
                  "pickup-timing search starts before physical pickup");
    coordinator.RequestHintToggle();
    coordinator.Update();

    fxe::ManualMillis::SetNowMs(pickupAtMs);
    observe(baselineOccupancy & ~(UINT64_C(1) << 48U));
    coordinator.UpdateLegalDestinationsMask();

    const uint32_t firstCandidateAtMs = pickupAtMs + 500U;
    fxe::ManualMillis::SetNowMs(firstCandidateAtMs);
    results.Check(engine.PublishCandidate(1U),
                  "first candidate publishes while piece remains held");
    coordinator.Update();
    results.Check(hintIs(48U, 40U),
                  "best available hint becomes visible during held pickup");

    fxe::ManualMillis::SetNowMs(firstCandidateAtMs + 100U);
    results.Check(engine.PublishCandidate(2U),
                  "deeper candidate publishes during the same pickup");
    coordinator.Update();
    results.Check(hintIs(48U, 40U),
                  "deeper refinement cannot retarget a held piece");

    fxe::ManualMillis::SetNowMs(firstCandidateAtMs + 200U);
    observe(baselineOccupancy);
    results.Check(hintIs(36U, 28U),
                  "latest completed candidate appears after pickup returns Idle");

    engine.ReleaseFirstPoll();
    results.Check(engine.WaitForWorkerCompletion(worker, 1U),
                  "pickup-timing worker completes after release");
    coordinator.Update();
    results.Check(hintIs(36U, 28U) &&
                      engine.unsafeEngineAccesses.load() == 0U,
                  "terminal result preserves the latest completed hint safely");
    (void)worker.CancelAndJoin(engine);
    std::printf(
        "[HINT_PICKUP_TIMING] pickup_ms=%lu candidate_ms=%lu hint=36-28 "
        "unsafe=%lu\n",
        static_cast<unsigned long>(pickupAtMs),
        static_cast<unsigned long>(firstCandidateAtMs),
        static_cast<unsigned long>(engine.unsafeEngineAccesses.load()));
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fxe::test_support::TestResults results("ASYNC_ENGINE_OWNERSHIP");
    results.Start();
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    CheckPollReturnIsNotWorkerQuiescence(results);
    CheckClassicResultWaitsForWorkerCleanup(results);
    CheckEmptyOpeningWaitsForWorkerCleanup(results);
    for (const bool duringCleanup : {false, true}) {
        for (const bool changeSeat : {false, true}) {
            CheckCpuTakeoverWaitsForHint(results, duringCleanup, changeSeat);
            CheckCpuTakeoverWaitsForHint(results, duringCleanup, changeSeat, true);
        }
    }
    CheckCancelBeforeReadyAndRetry(results);
    CheckReadyBeforeCancel(results);
    CheckPendingMoveSurvivesCancellation(results);
    CheckNewGameControlTruthfullyRetries(results);
    CheckLoadOrInitializeReuseRetries(results);
    CheckClockTimeoutWaitsForQuiescence(results);
    CheckFastFinalUndoRearmWaitsForQuiescence(results);
    CheckStartOnlySettingAdmitsBackgroundHint(results);
    CheckAdmittedFirstPlayerIntentRetriesAfterWorker(results);
    CheckProgressiveHintWindowAndStages(results, true, 8U, true);
    CheckProgressiveHintWindowAndStages(results, true, 8U, false);
    CheckProgressiveHintWindowAndStages(results, true, 8U, true, true);
    CheckProgressiveHintWindowAndStages(results, true, 8U, false, true);
    CheckProgressiveHintWindowAndStages(results, false, 2U, true);
    CheckProgressiveHintWindowAndStages(results, false, 5U, true);
    CheckProgressiveHintWindowAndStages(results, false, 11U, true);
    for (const uint32_t pickupAtMs : {100U, 500U, 3000U}) {
        CheckHintCandidateAcrossPickupTimings(results, pickupAtMs);
    }
    CheckCpuThinkingJourneys(results);
    return results.Finish();
}
