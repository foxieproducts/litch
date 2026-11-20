#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/beginner_mode_controller.hpp"
#include "modules/beginner_mode_policy.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/litch_max_engine.hpp"
#include "modules/initial_fen.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"

namespace {

constexpr const char* DEVELOPMENT_FEN =
    "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 1";
int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

struct Publications {
    modules::LitchMaxEngine* engine = nullptr;
    // The legal seed is immediate; completed search begins at one full ply.
    uint8_t minimumDepth = 1U;
    uint8_t maximumDepth = 0U;
    uint8_t cancelAtDepth = 0U;
    uint8_t deepest = 0U;
    uint32_t count = 0U;
    bool shallow = false;
    bool hangingKnight = false;
    modules::ThinkCandidate retained;
    modules::ThinkCandidate latest;
};

void Observe(void* context, const modules::ThinkCandidate& candidate) {
    auto& result = *static_cast<Publications*>(context);
    ++result.count;
    const bool retained = candidate.fromSquare == result.retained.fromSquare &&
                          candidate.toSquare == result.retained.toSquare &&
                          candidate.promotion == result.retained.promotion &&
                          candidate.completedDepth == result.retained.completedDepth;
    result.shallow |= candidate.source != modules::ThinkCandidateSource::Seed && !retained &&
                      candidate.completedDepth < result.minimumDepth;
    result.hangingKnight |= candidate.fromSquare == 45U &&
                            candidate.toSquare == 28U;
    result.deepest = candidate.completedDepth;
    result.latest = candidate;
    if (result.cancelAtDepth != 0U &&
        result.deepest >= result.cancelAtDepth) {
        result.engine->CancelThink();
    }
}

modules::ThinkState Search(modules::LitchMaxEngine& engine,
                           Publications& publications,
                           const uint32_t nodeBudget,
                           modules::UciMove& best) {
    modules::ThinkConfig config;
    config.continuousAnalysis = true;
    config.maxNodes = nodeBudget;
    config.maxDepth = publications.maximumDepth;
    config.candidateObserver = {Observe, &publications};
    Check(engine.StartThink(config), "production continuous search starts");
    uint8_t progress = 0U;
    return engine.PollThinkProgress(progress, best);
}


void CheckFirstPublicationQuality() {
    struct Fixture { const char* name; const char* fen; const char* soundMove; bool mateInOne = false; };
    constexpr Fixture cases[]{
        {"poisoned_rook", "r1bk1b1r/ppN2ppp/2n5/4p1q1/2B5/8/PPPP1PPP/R1BQK2R w KQ - 1 9", "d2d3"},
        {"quiet_queen_check", "r2qk2r/pQ1b2p1/1p2p3/2p5/2P3n1/7N/PP1N2P1/R3KB1R b KQkq - 1 17", "d8h4"},
        {"save_knight", "rnbq1rk1/1p3pp1/p3pn1p/3p2N1/N2P4/3B4/PPPQ1PPP/R3K2R w KQ - 0 11", "g5f3"},
        {"mate_one", "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1", "f7g7", true},
        {"check_escape", "k7/8/2K5/8/8/8/8/R7 b - - 0 1", "a8b8"},
    };
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const auto& fixture = cases[index];
        modules::LitchMaxEngine engine;
        engine.Reset();
        Check(engine.SetFen(fixture.fen), "first-publication fixture loads");
        modules::UciMove best;
        if (index < 3U) {
            Publications shortWork{&engine};
            shortWork.maximumDepth = 5U;
            Check(Search(engine, shortWork, 16384U, best) == modules::ThinkState::Done &&
                      !best.Empty() && shortWork.count != 0U,
                  "bounded work retains a useful legal seed or complete shallow root");
            Check(engine.SetFen(fixture.fen), "fresh context follows interrupted initial work");
        }
        Publications first{&engine};
        first.maximumDepth = 5U;
        // A completed mate-in-one ends iterative search at its first root.
        const auto state = Search(engine, first, 1800000U, best);
        const bool completedRoots = fixture.mateInOne
            ? first.count == 1U && first.deepest == 1U : first.count >= 2U;
        Check(state == modules::ThinkState::Done &&
                  completedRoots && first.deepest >= 1U && !first.shallow &&
                  best.Equals(fixture.soundMove),
              "progressive roots reach the verified sound final move, including early mates");
        modules::ThinkStats stats;
        Check(engine.GetLastThinkStats(stats), "first-result actual work is available after quiescence");
        std::printf("[HINT_FIRST_COMPLETE] case=%s depth=%u nodes=%u move=%s publications=%u\n",
                    fixture.name, first.deepest, stats.nodeCount, best.CStr(), first.count);
    }
}

void CheckStartupAndRestart() {
    modules::LitchMaxEngine engine;
    engine.Reset();
    Check(engine.SetFen(DEVELOPMENT_FEN), "development fixture loads");
    modules::ThinkCandidate seed;
    modules::UciMove seedMove;
    Check(engine.GetImmediateThinkCandidate(seed) &&
              seed.source == modules::ThinkCandidateSource::Seed && seed.completedDepth == 0U &&
              litch_max::TryFormatUciMove(seed.fromSquare, seed.toSquare, seed.promotion, seedMove),
          "tiny-budget fixture obtains the actual internal depth-zero legal Seed");
    modules::UciMove best;
    Publications warmup{&engine};
    const auto warmupState = Search(engine, warmup, 2U, best);
    Check(warmupState == modules::ThinkState::Done && best.Equals(seedMove.CStr()) &&
              warmup.count == 0U,
          "tiny search retains its internal Seed without publishing human Hint guidance");

    // Cancel from the real completed-root observer, not by replacing the
    // engine result. This preserves completed analysis and production restart work.
    Publications first{&engine};
    first.cancelAtDepth = 5U;
    Check(Search(engine, first, 1800000U, best) == modules::ThinkState::Canceled,
          "cancel after a genuinely searched root iteration");
    Check(first.count != 0U && first.deepest >= 5U && !first.shallow,
          "initial analyzed publications include replies and avoid Nxe5 loss");

    Publications interruptedRestart{&engine};
    interruptedRestart.minimumDepth = first.deepest;
    interruptedRestart.retained = first.latest;
    Check(Search(engine, interruptedRestart, 2U, best) == modules::ThinkState::Done &&
              !best.Empty() && interruptedRestart.count == 1U &&
              !interruptedRestart.shallow,
          "a short retry republishes retained work without a shallow terminal result");

    Publications restart{&engine};
    restart.maximumDepth = 6U;
    restart.minimumDepth = first.deepest;
    restart.retained = first.latest;
    Check(Search(engine, restart, 1800000U, best) == modules::ThinkState::Done,
          "same-position search can resume after cancellation and short retry");
    Check(restart.count != 0U && !restart.shallow,
          "same-position restart never republishes shallower analysis");
    Publications retainedGuard{&engine};
    retainedGuard.maximumDepth = 5U;
    retainedGuard.minimumDepth = restart.deepest;
    retainedGuard.retained = restart.latest;
    modules::UciMove weakerBest;
    Check(Search(engine, retainedGuard, 1800000U, weakerBest) == modules::ThinkState::Done &&
              !weakerBest.Empty() && retainedGuard.count == 1U &&
              retainedGuard.latest.completedDepth == 6U && !retainedGuard.shallow,
          "same-position depth5 completion cannot replace the retained depth6 result");
    char fen[160] = {};
    Check(engine.GetFen(fen, sizeof(fen)) &&
              std::strcmp(fen, DEVELOPMENT_FEN) == 0,
          "all interruption and publication paths preserve the position");

    Check(engine.SetFen(DEVELOPMENT_FEN), "explicit same-FEN reload succeeds");
    Publications reload{&engine};
    reload.maximumDepth = 5U;
    const auto reloadState = Search(engine, reload, 1800000U, best);
    std::printf("[HINT_SAME_FEN_RELOAD] first_depth=%u restart_depth=%u reload_state=%u "
                "reload_depth=%u minimum=%u publications=%u shallow=%u move=%u-%u\n",
                first.deepest, restart.deepest, static_cast<unsigned>(reloadState),
                reload.deepest, reload.minimumDepth, reload.count,
                static_cast<unsigned>(reload.shallow),
                reload.latest.fromSquare, reload.latest.toSquare);
    // The full restart, rather than the earlier canceled iteration, owns the
    // current retained floor. Reload must publish below that deeper floor.
    Check(restart.deepest > first.deepest,
          "full restart establishes a deeper retained floor for the reload discriminator");
    Check(reloadState == modules::ThinkState::Done &&
              reload.count != 0U && reload.deepest < restart.deepest && !reload.shallow,
          "explicit same-FEN reload starts a fresh analysis context");

    Publications beforeVariant{&engine};
    beforeVariant.maximumDepth = 6U;
    Check(Search(engine, beforeVariant, 1800000U, best) == modules::ThinkState::Done,
          "Classic analysis is ready before variant change");
    modules::ThinkConfig duckConfig;
    duckConfig.continuousAnalysis = true;
    Check(engine.StartDuckThink(DEVELOPMENT_FEN, 32U, duckConfig),
          "Duck search starts a distinct context");
    engine.CancelThink();
    uint8_t progress = 0U;
    Check(engine.PollThinkProgress(progress, best) == modules::ThinkState::Canceled,
          "Duck context is canceled before computation");
    Publications afterVariant{&engine};
    afterVariant.maximumDepth = 5U;
    Check(Search(engine, afterVariant, 1800000U, best) == modules::ThinkState::Done &&
              afterVariant.count != 0U && afterVariant.deepest < beforeVariant.deepest,
          "variant round-trip does not inherit a stale Classic depth floor");

    Publications beforeNewPosition{&engine};
    beforeNewPosition.maximumDepth = 6U;
    Check(Search(engine, beforeNewPosition, 1800000U, best) == modules::ThinkState::Done &&
              beforeNewPosition.deepest == 6U,
          "new-position discriminator starts with genuinely deeper retained work");
    Check(engine.SetFen("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1"),
          "next-position fixture loads");
    Publications next{&engine};
    next.maximumDepth = 5U;
    Check(Search(engine, next, 1800000U, best) == modules::ThinkState::Done &&
              next.count != 0U && next.deepest < beforeNewPosition.deepest && !next.shallow,
          "new FEN clears the previous position's publication floor");
}

// Only the work budget/cancellation trigger is controlled. Legal moves,
// completed-root publication, restart and delivered cache are production code.
class InterruptedHintEngine final : public modules::IEngine {
  public:
    void Reset() override { engine.Reset(); }
    bool SetFen(const char* fen) override { return engine.SetFen(fen); }
    bool GetFen(char* fen, size_t size) const override { return engine.GetFen(fen, size); }
    bool ApplyUciMove(const char* move) override { return engine.ApplyUciMove(move); }
    modules::SideToMove GetSideToMove() const override { return engine.GetSideToMove(); }
    bool IsInCheck(modules::SideToMove side) const override { return engine.IsInCheck(side); }
    bool IsInCheckmate(modules::SideToMove side) const override { return engine.IsInCheckmate(side); }
    bool IsInStalemate(modules::SideToMove side) const override { return engine.IsInStalemate(side); }
    bool GetLegalDestinationsMask(uint8_t from, uint64_t& mask) override {
        return engine.GetLegalDestinationsMask(from, mask);
    }
    bool GetImmediateThinkCandidate(modules::ThinkCandidate& out) override {
        return engine.GetImmediateThinkCandidate(out);
    }
    bool StartThink(const modules::ThinkConfig& requested) override {
        ++starts;
        auto config = requested;
        // Non-timing retry journeys freeze the clock inside Poll. Their node
        // limits exercise retained results, not the physical500ms requirement.
        if (calculationStepMs == 0U) config.minimumCalculationMs = 0U;
        config.maxNodes = starts == 1U ? initialNodeBudget : 2U;
        config.maxDepth = maximumDepth;
        observer = requested.candidateObserver;
        if (calculationStepMs != 0U) {
            config.progressObserver = {[](void* context, uint32_t nodes) {
                auto& self = *static_cast<InterruptedHintEngine*>(context);
                if (fxe::Millis() - self.calculationStartedMs < 500U) {
                    fxe::ManualMillis::AdvanceMs(self.calculationStepMs);
                }
                if (fxe::Millis() - self.calculationStartedMs == 499U) {
                    uint8_t from = 64U, to = 64U;
                    self.hiddenAt499 = self.publications == 0U && self.stateMachine != nullptr &&
                        !self.stateMachine->TryGetHintMoveSquares(from, to);
                }
                self.progressObserver.Publish(nodes);
            }, this};
            progressObserver = requested.progressObserver;
        }
        config.candidateObserver = {[](void* context, const modules::ThinkCandidate& candidate) {
            auto& self = *static_cast<InterruptedHintEngine*>(context);
            if (self.publications++ == 0U) self.firstPublicationMs = fxe::Millis() - self.calculationStartedMs;
            self.sawSeed |= candidate.source == modules::ThinkCandidateSource::Seed;
            self.observer.Publish(candidate);
            if (self.starts == 1U) {
                self.firstCandidate = candidate;
                self.firstCandidateDepth = candidate.completedDepth;
            }
            if (self.starts == 1U && self.cancelAtDepth != 0U &&
                candidate.completedDepth >= self.cancelAtDepth) {
                self.engine.CancelThink();
            }
        }, this, [](void* context, const modules::ThinkCandidate& candidate) {
            auto& self = *static_cast<InterruptedHintEngine*>(context);
            self.observer.RootCompleted(candidate);
            if (self.releaseClock && self.maximumDepth != 0U &&
                candidate.rootKind == modules::ThinkRootKind::ClassicFull &&
                candidate.completedDepth >= self.maximumDepth) self.calculationFinished = true;
        }};
        return engine.StartThink(config);
    }
    void CancelThink() override { engine.CancelThink(); }
    modules::ThinkState PollThinkProgress(uint8_t& progress, modules::UciMove& move) override {
        calculationStartedMs = fxe::Millis();
        if (releaseClock) {
            releaseClockOwner = this;
            fxe::ScopedMillisProviderOverride clock(&ReleaseMillis);
            const auto state = engine.PollThinkProgress(progress, move);
            releaseClockOwner = nullptr;
            return state;
        }
        return engine.PollThinkProgress(progress, move);
    }
    bool GetLastThinkStats(modules::ThinkStats& stats) const override {
        return engine.GetLastThinkStats(stats);
    }
    bool GetLastClassicThinkResult(modules::ClassicThinkResult& result) const override {
        return engine.GetLastClassicThinkResult(result);
    }
    modules::ThinkCandidate firstCandidate;
    uint8_t firstCandidateDepth = 0U;
    uint32_t starts = 0U;
    uint32_t initialNodeBudget = 1800000U;
    uint8_t maximumDepth = 0U;
    uint8_t cancelAtDepth = 5U;
    uint32_t calculationStepMs = 0U;
    uint32_t calculationStartedMs = 0U;
    uint32_t firstPublicationMs = 0U;
    uint32_t publications = 0U;
    bool hiddenAt499 = false;
    bool sawSeed = false;
    bool releaseClock = false;
    bool calculationFinished = false;
    runtime::GameStateMachine* stateMachine = nullptr;
  private:
    static uint32_t ReleaseMillis() {
        auto& self = *releaseClockOwner;
        if (self.calculationFinished && fxe::ManualMillis::NowMs() - self.calculationStartedMs < 500U) {
            fxe::ManualMillis::AdvanceMs(1U);
        }
        if (fxe::ManualMillis::NowMs() - self.calculationStartedMs == 499U) {
            uint8_t from = 64U, to = 64U;
            self.hiddenAt499 = self.publications == 0U && self.stateMachine != nullptr &&
                !self.stateMachine->TryGetHintMoveSquares(from, to);
        }
        return fxe::ManualMillis::NowMs();
    }
    inline static InterruptedHintEngine* releaseClockOwner = nullptr;
    modules::LitchMaxEngine engine;
    modules::ThinkCandidateObserver observer;
    modules::ThinkProgressObserver progressObserver;
};

void CheckFirstMoveDelivery() {
    for (const bool coaching : {false, true}) {
    constexpr const char* PATH = "build/test_hint_first_move_delivery.json";
    (void)std::remove(PATH);
    (void)std::remove("build/test_hint_first_move_delivery.json.bak");
    (void)std::remove("build/test_hint_first_move_delivery.json.tmp");
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "first-move settings load");
    settings.Doc()["P1_AUTO_HINTS"] = coaching;
    runtime::GameStateMachineConfig stateConfig;
    stateConfig.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    InterruptedHintEngine engine;
    engine.initialNodeBudget = 1800000U;
    engine.maximumDepth = coaching ? 0U : 5U;
    engine.cancelAtDepth = 0U;
    engine.calculationStepMs = 1U;
    engine.stateMachine = &stateMachine;
    engine.releaseClock = !coaching;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "standard Classic game loads");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    const uint64_t occupied = stateMachine.GetExpectedOccupancyMask();
    (void)dispatcher.DispatchBoardObservation({
        fxe::Millis(), stateMachine.GetLastBoardObservationSequence() + 1U,
        stateMachine.GetPhysicalOccupancyMask() ^ occupied, occupied});
    if (coaching) {
        // Use the saved preference and the production owner policy, without O
        // or a manual request. The same policy runs in firmware/VirtualLitch.
        modules::BeginnerModeController beginner(settings);
        modules::BeginnerModePolicyInputs inputs;
        inputs.variant = coordinator.GetGameVariant();
        inputs.sideToMove = coordinator.GetEngineDerivedState().sideToMove;
        const auto owner = modules::ActiveBeginnerModeOwnerForTurn(
            beginner, inputs, stateMachine.GetChessInteractionModel().phase);
        coordinator.SetCoachingHintActive(modules::BeginnerModeOwnerForHumanTurn(
            beginner, inputs) != modules::UiOwner::None);
        bool forced = false;
        runtime::SyncBeginnerModeHintDisplay(stateMachine, owner, forced);
        Check(forced && !stateMachine.IsManualHintRequested(),
              "saved Coaching arms automatic guidance without a manual request");
    } else {
        coordinator.RequestHintToggle();
    }
    coordinator.Update();
    uint8_t from = 64U;
    uint8_t to = 64U;
    Check(engine.starts == 1U && engine.hiddenAt499 && !engine.sawSeed &&
              stateMachine.IsManualHintRequested() == !coaching &&
              engine.firstPublicationMs == 500U && engine.firstCandidate.scoreValid &&
              engine.firstCandidate.source == modules::ThinkCandidateSource::Search &&
              stateMachine.TryGetHintMoveSquares(from, to),
          "manual and Coaching first reveal uses scored production work at500, never a Seed at499");
    modules::ThinkStats work;
    Check(engine.GetLastThinkStats(work) && work.calculationMsValid &&
              (coaching ? work.calculationMs == 500U : work.calculationMs < 500U),
          "finished early work records actual calculation separately from its500ms release");
    fxe::ManualMillis::AdvanceMs(500U);
    coordinator.Update();
    uint64_t legal = 0ULL;
    const bool delivered = stateMachine.TryGetHintMoveSquares(from, to);
    Check(delivered && engine.GetLegalDestinationsMask(from, legal) &&
              ((legal >> to) & 1ULL) != 0ULL && engine.starts == 1U,
          "first-move Hint delivers the completed search without request-driven restart");
    std::printf("[FIRST_MOVE_HINT_DELIVERY] coaching=%u calculation_ms=%u release_ms=%u hidden499=%u seed=%u root=%u depth=%u move=%u-%u starts=%u\n",
                coaching, work.calculationMs, engine.firstPublicationMs, engine.hiddenAt499, engine.sawSeed,
                static_cast<unsigned>(engine.firstCandidate.rootKind), engine.firstCandidate.completedDepth,
                from, to, engine.starts);
    }
}

void CheckCoordinatorRetryDelivery() {
    constexpr const char* PATH = "build/test_hint_search_quality.json";
    (void)std::remove(PATH);
    (void)std::remove("build/test_hint_search_quality.json.bak");
    (void)std::remove("build/test_hint_search_quality.json.tmp");
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "coordinator retry settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = DEVELOPMENT_FEN;
    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 0U;
    stateConfig.placementStableRequiredMs = 0U;
    stateConfig.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    InterruptedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "coordinator retry position loads");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    const uint64_t occupied = stateMachine.GetExpectedOccupancyMask();
    (void)dispatcher.DispatchBoardObservation({
        fxe::Millis(), stateMachine.GetLastBoardObservationSequence() + 1U,
        stateMachine.GetPhysicalOccupancyMask() ^ occupied, occupied});
    stateMachine.SetHintDisplayRequested(true);
    coordinator.Update();
    Check(engine.starts == 1U, "first background Hint reaches controlled cancellation");
    fxe::ManualMillis::AdvanceMs(500U);
    coordinator.Update();
    uint8_t from = 64U;
    uint8_t to = 64U;
    Check(engine.starts == 2U && stateMachine.TryGetHintMoveSquares(from, to) &&
              engine.firstCandidateDepth >= 5U &&
              from == engine.firstCandidate.fromSquare &&
              to == engine.firstCandidate.toSquare && !(from == 45U && to == 28U),
          "coordinator retry retains its completed analysis instead of shallow Nxe5");

    coordinator.RequestCancelHintWorkAndRestart();
    coordinator.Update();
    fxe::ManualMillis::AdvanceMs(500U);
    coordinator.Update();
    Check(engine.starts >= 3U && stateMachine.TryGetHintMoveSquares(from, to) &&
              from == engine.firstCandidate.fromSquare &&
              to == engine.firstCandidate.toSquare,
          "cache-clearing restart immediately recovers genuine retained analysis");
    std::printf("[HINT_RETRY_DELIVERY] starts=%u move=%u-%u retained=%u-%u depth=%u\n",
                engine.starts, from, to, engine.firstCandidate.fromSquare,
                engine.firstCandidate.toSquare, engine.firstCandidateDepth);
}

void CheckInitialDuckRemovalClearsRetainedAnalysis() {
    constexpr const char* PATH = "build/test_hint_variant_reset.json";
    constexpr uint64_t CHESS_OCCUPANCY = UINT64_C(0xffff00000000ffff);
    constexpr uint8_t DUCK = 32U;
    constexpr uint64_t DUCK_OCCUPANCY = CHESS_OCCUPANCY | (UINT64_C(1) << DUCK);
    (void)std::remove(PATH);
    (void)std::remove("build/test_hint_variant_reset.json.bak");
    (void)std::remove("build/test_hint_variant_reset.json.tmp");
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = PATH;
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "variant reset settings load");
    runtime::GameStateMachineConfig stateConfig;
    stateConfig.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    modules::LitchMaxEngine engine;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "untouched variant reset position loads");
    coordinator.SetBoardSettingsActive(true);
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    const auto observe = [&](const uint64_t occupied) {
        (void)dispatcher.DispatchBoardObservation({
            fxe::Millis(), stateMachine.GetLastBoardObservationSequence() + 1U,
            stateMachine.GetPhysicalOccupancyMask() ^ occupied, occupied});
        coordinator.Update();
    };
    const auto settle = [&] {
        fxe::ManualMillis::AdvanceMs(200U);
        dispatcher.DispatchTick(fxe::Millis());
        coordinator.Update();
    };
    observe(DUCK_OCCUPANCY);
    Check(coordinator.GetGameVariant() == modules::GameVariant::Undecided,
          "initial Duck observation waits for the production stability window");
    settle();
    Check(coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              coordinator.GetDuckSquare() == DUCK,
          "stable initial Duck observation activates the compound variant");

    // Prime actual completed adapter authority while board settings suppresses
    // automatic Hint admission, including any intervening Classic search.
    struct Probe {
        modules::LitchMaxEngine& engine;
        modules::ThinkCandidate latest;
        uint32_t publications = 0U;
        bool cancelOnPublication = true;
        uint32_t startedMs = 0U;
        uint32_t firstPublicationMs = 0U;
        bool hiddenAt499 = false;
        bool sawSeed = false;
    } probe{engine};
    modules::ThinkConfig config;
    config.continuousAnalysis = true;
    config.maxNodes = 100000U;
    config.minimumCalculationMs = 500U;
    config.progressObserver = {[](void* context, uint32_t nodes) {
        auto& result = *static_cast<Probe*>(context);
        if (nodes != 0U && fxe::Millis() - result.startedMs < 500U) fxe::ManualMillis::AdvanceMs(1U);
        if (fxe::Millis() - result.startedMs == 499U) result.hiddenAt499 = result.publications == 0U;
    }, &probe};
    config.candidateObserver = {[](void* context, const modules::ThinkCandidate& candidate) {
        auto& result = *static_cast<Probe*>(context);
        result.sawSeed |= candidate.source == modules::ThinkCandidateSource::Seed;
        if (result.publications == 0U) result.firstPublicationMs = fxe::Millis() - result.startedMs;
        result.latest = candidate;
        ++result.publications;
        if (result.cancelOnPublication) result.engine.CancelThink();
    }, &probe};
    uint8_t progress = 0U;
    modules::UciMove best;
    Check(engine.StartDuckThink(modules::INITIAL_FEN, DUCK, config),
          "real initial Duck adapter analysis starts");
    // Queue waiting is not calculation. The guard must still spend500 inside Poll.
    fxe::ManualMillis::AdvanceMs(20000U);
    probe.startedMs = fxe::Millis();
    Check(engine.PollThinkProgress(progress, best) == modules::ThinkState::Canceled &&
              probe.publications == 1U && probe.latest.isDuck &&
              probe.latest.completedDepth >= 1U && probe.latest.scoreValid &&
              probe.hiddenAt499 && probe.firstPublicationMs == 500U && !probe.sawSeed,
          "Duck publishes a scored compound root at500 calculation ms, excluding20seconds queued idle");
    std::printf("[DUCK_HINT_MINIMUM] simulated_calculation_ms=%u hidden499=%u seed=%u depth=%u queued_idle_ms=20000\n",
                probe.firstPublicationMs, probe.hiddenAt499, probe.sawSeed, probe.latest.completedDepth);
    const auto retained = probe.latest;

    observe(CHESS_OCCUPANCY);
    Check(coordinator.GetGameVariant() == modules::GameVariant::Undecided &&
              coordinator.GetDuckSquare() == modules::INVALID_DUCK_SQUARE,
          "physical initial Duck removal resets actual variant authority");
    settle();
    observe(DUCK_OCCUPANCY);
    Check(coordinator.GetGameVariant() == modules::GameVariant::Undecided,
          "same-square Duck replacement still waits for observation stability");
    settle();
    Check(coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              coordinator.GetDuckSquare() == DUCK &&
              stateMachine.GetPhysicalOccupancyMask() == DUCK_OCCUPANCY &&
              std::strcmp(coordinator.GetAuthoritativeFen(), modules::INITIAL_FEN) == 0,
          "same-square stable replacement restores Duck without a chess move");

    probe.publications = 0U;
    probe.cancelOnPublication = false;
    config.maxNodes = 1U;
    config.progressObserver = {};
    Check(engine.StartDuckThink(modules::INITIAL_FEN, DUCK, config),
          "short analysis starts after the physical variant reset");
    const auto result = engine.PollThinkProgress(progress, best);
    std::printf("[DUCK_VARIANT_ANALYSIS_RESET] duck=%u retained_depth=%u removed=Undecided readded=Duck budget=1 publications=%u state=%u\n",
                DUCK, retained.completedDepth, probe.publications,
                static_cast<unsigned>(result));
    Check(result == modules::ThinkState::Done && !best.Empty() &&
              probe.publications == 0U,
          "actual variant reset cannot republish an old same-FEN same-Duck candidate");
}

} // namespace

int main() {
    fxe::ManualMillis::SetNowMs(1000U);
    const fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    CheckFirstPublicationQuality();
    CheckStartupAndRestart();
    CheckFirstMoveDelivery();
    CheckCoordinatorRetryDelivery();
    CheckInitialDuckRemovalClearsRetainedAnalysis();
    std::printf("[HINT_SEARCH_QUALITY] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
