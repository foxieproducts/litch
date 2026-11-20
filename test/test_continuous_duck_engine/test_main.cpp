#include <cassert>
#include <atomic>
#include <thread>
#include <cstdio>
#include "modules/litch_max_engine.hpp"
#include "modules/async_think_worker.hpp"
#include "modules/initial_fen.hpp"
#include "foxie_embedded/millis.hpp"
namespace { uint32_t ticks=0, publishes=0, observedNodes=0;
uint32_t Clock() { return ++ticks; }
void Candidate(void*, const modules::ThinkCandidate& c) {
    assert(c.isDuck && c.fromSquare < 64U && c.toSquare < 64U);
    if (c.source == modules::ThinkCandidateSource::Seed) return;
    ++publishes;
}
struct Progress {
    modules::LitchMaxEngine* engine;
    bool cancel;
    static void On(void* context, uint32_t nodes) {
        const auto& self=*static_cast<Progress*>(context);
        observedNodes=nodes;
        if(self.cancel && nodes >= 100U) self.engine->CancelThink();
    }
};
} // namespace
namespace {
struct RetryProbe {
    modules::LitchMaxEngine& engine;
    modules::ThinkCandidate first;
    modules::ThinkCandidate latest;
    uint32_t nodes = 0U;
    uint32_t firstNodes = 0U;
    uint32_t publications = 0U;
    uint32_t cancelAtPublication = 0U;

    explicit RetryProbe(modules::LitchMaxEngine& value) : engine(value) {}

    static void ObserveNodes(void* context, const uint32_t nodes) {
        static_cast<RetryProbe*>(context)->nodes = nodes;
    }
    static void ObserveCandidate(void* context, const modules::ThinkCandidate& candidate) {
        if (candidate.source == modules::ThinkCandidateSource::Seed) return;
        auto& self = *static_cast<RetryProbe*>(context);
        if (self.publications++ == 0U) {
            self.first = candidate;
            self.firstNodes = self.nodes;
        }
        self.latest = candidate;
        if (self.publications == self.cancelAtPublication) self.engine.CancelThink();
    }
};

bool SameCandidate(const modules::ThinkCandidate& lhs, const modules::ThinkCandidate& rhs) {
    return lhs.fromSquare == rhs.fromSquare && lhs.toSquare == rhs.toSquare &&
           lhs.promotion == rhs.promotion && lhs.duckDestination == rhs.duckDestination &&
           lhs.capturedKing == rhs.capturedKing;
}

void CheckSamePositionRetryRetainsDeeperCompoundTurn() {
    constexpr char FEN[] = "3k4/8/5n2/3r4/8/8/8/3Q3K w - - 0 1";
    constexpr uint8_t DUCK = 32U;
    fxe::ManualMillis::SetNowMs(1000U);
    const fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    modules::LitchMaxEngine engine;
    engine.Reset();
    assert(engine.SetFen(FEN));
    RetryProbe first{engine};
    first.cancelAtPublication = 2U;
    modules::ThinkConfig config;
    config.continuousAnalysis = true;
    config.maxNodes = 1000000U;
    config.candidateObserver = {&RetryProbe::ObserveCandidate, &first};
    config.progressObserver = {&RetryProbe::ObserveNodes, &first};
    assert(engine.StartDuckThink(FEN, DUCK, config));
    uint8_t percent = 0U;
    modules::UciMove best;
    assert(engine.PollThinkProgress(percent, best) == modules::ThinkState::Canceled);
    assert(first.publications == 2U && !SameCandidate(first.first, first.latest));
    assert(first.first.completedDepth == 1U && first.latest.completedDepth == 2U);

    // The real adapter completed a deeper compound root before cancellation.
    // A new search has enough work for depth one only; it must retain depth two.
    RetryProbe retry{engine};
    config.maxNodes = first.firstNodes + 1U;
    config.candidateObserver = {&RetryProbe::ObserveCandidate, &retry};
    config.progressObserver = {&RetryProbe::ObserveNodes, &retry};
    assert(engine.StartDuckThink(FEN, DUCK, config));
    const auto state = engine.PollThinkProgress(percent, best);
    std::printf("[DUCK_RETRY_DEPTH] initial_nodes=%u retry_nodes=%u publications=%u retained=%u\n",
                first.firstNodes, retry.nodes, retry.publications,
                SameCandidate(first.latest, retry.latest));
    std::fflush(stdout);
    assert(retry.publications == 1U && SameCandidate(first.latest, retry.latest));
    // A shallower terminal result must not be classified as completed analysis.
    assert(state == modules::ThinkState::Done && !best.Empty());
    assert(retry.latest.completedDepth == 2U);
    modules::DuckThinkResult result;
    assert(engine.GetLastDuckThinkResult(result));

    // Ordinary CPU analysis does not publish or erase human Hint authority for
    // the unchanged position. It still returns its configured novice result.
    modules::ThinkConfig cpu;
    cpu.maxNodes = 4096U;
    assert(engine.StartDuckThink(FEN, DUCK, cpu));
    assert(engine.PollThinkProgress(percent, best) == modules::ThinkState::Done);
    assert(engine.GetLastDuckThinkResult(result));
    RetryProbe afterCpu{engine};
    config.candidateObserver = {&RetryProbe::ObserveCandidate, &afterCpu};
    config.progressObserver = {&RetryProbe::ObserveNodes, &afterCpu};
    assert(engine.StartDuckThink(FEN, DUCK, config));
    assert(engine.PollThinkProgress(percent, best) == modules::ThinkState::Done);
    assert(afterCpu.publications == 1U && afterCpu.latest.completedDepth == 2U &&
           SameCandidate(first.latest, afterCpu.latest));

    const auto primeRoot = [&] {
        RetryProbe fresh{engine};
        config.maxNodes = first.firstNodes + 1U;
        config.candidateObserver = {&RetryProbe::ObserveCandidate, &fresh};
        config.progressObserver = {&RetryProbe::ObserveNodes, &fresh};
        assert(engine.StartDuckThink(FEN, DUCK, config));
        assert(engine.PollThinkProgress(percent, best) == modules::ThinkState::Done);
        assert(fresh.publications != 0U && fresh.latest.completedDepth == 1U);
    };
    const auto expectUncached = [&](const char* fen, const uint8_t duck) {
        RetryProbe fresh{engine};
        config.maxNodes = 1U;
        config.candidateObserver = {&RetryProbe::ObserveCandidate, &fresh};
        config.progressObserver = {&RetryProbe::ObserveNodes, &fresh};
        assert(engine.StartDuckThink(fen, duck, config));
        assert(engine.PollThinkProgress(percent, best) == modules::ThinkState::Done);
        assert(fresh.publications == 0U && !best.Empty());
    };
    // A same-FEN reload is explicitly fresh. Variant and position keys retire
    // authority even when subsequent work cannot complete its first root.
    assert(engine.SetFen(FEN));
    primeRoot();
    expectUncached(FEN, 33U);
    primeRoot();
    modules::ThinkConfig classic;
    classic.continuousAnalysis = true;
    classic.maxNodes = 2U;
    assert(engine.StartThink(classic));
    assert(engine.PollThinkProgress(percent, best) == modules::ThinkState::Done);
    expectUncached(FEN, DUCK);
    primeRoot();
    engine.Reset();
    expectUncached(FEN, DUCK);
    primeRoot();
    expectUncached("7k/8/8/8/8/8/8/K7 w - - 0 1", DUCK);
    std::puts("[DUCK_ANALYSIS_CONTEXT] retry_depth=2 cpu_preserved=1 reload=1 duck_change=1 variant_change=1 reset=1 fen_change=1");
}

void CheckCandidateDepthMailbox() {
    modules::AsyncThinkWorker mailbox;
    const auto publish = mailbox.PrepareCandidateObserver();
    const modules::ThinkCandidate duck{52U, 36U, 'q', 18U, true, false, 15U};
    const modules::ThinkCandidate classic{12U, 4U, 'n', 255U, false, false, 96U};
    std::atomic<bool> stop{false};
    std::atomic<bool> writerStarted{false};
    std::thread writer([&] {
        publish.Publish(duck);
        writerStarted.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
            publish.Publish(duck);
            publish.Publish(classic);
        }
    });
    while (!writerStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    modules::ThinkCandidate observed;
    uint32_t successfulReads = 0U;
    for (uint32_t i = 0U; i < 100000U; ++i) {
        if (!mailbox.TryGetPublishedCandidate(observed)) continue;
        ++successfulReads;
        assert((SameCandidate(observed, duck) && observed.isDuck && observed.completedDepth == 15U) ||
               (SameCandidate(observed, classic) && !observed.isDuck && observed.completedDepth == 96U));
    }
    assert(successfulReads == 100000U);
    stop.store(true, std::memory_order_release);
    writer.join();
    modules::ThinkCandidate unknown{1U, 2U};
    unknown.source = modules::ThinkCandidateSource::Seed;
    unknown.rootKind = modules::ThinkRootKind::Seed;
    publish.Publish(unknown);
    assert(mailbox.TryGetPublishedCandidate(observed) && observed.completedDepth == 0U);
    const auto acceptedUnknown = observed;
    unknown.fromSquare = 3U;
    unknown.toSquare = 4U;
    unknown.promotion = 'q';
    unknown.completedDepth = 128U;
    publish.Publish(unknown);
    assert(mailbox.TryGetPublishedCandidate(observed) &&
           SameCandidate(observed, acceptedUnknown) &&
           observed.isDuck == acceptedUnknown.isDuck &&
           observed.completedDepth == acceptedUnknown.completedDepth);
    mailbox.PrepareCandidateObserver();
    assert(!mailbox.TryGetPublishedCandidate(observed));
    std::printf("[THINK_CANDIDATE_DEPTH] atomic_copies=%u duck=15 classic=96 unknown=0 overflow_rejected=1\n",
                successfulReads);
}
} // namespace

int main() {
    CheckCandidateDepthMailbox();
    CheckSamePositionRetryRetainsDeeperCompoundTurn();
    for(bool cancel : {false,true}) {
        ticks=publishes=observedNodes=0U;
        fxe::ScopedMillisProviderOverride clock(&Clock);
        modules::LitchMaxEngine engine;
        engine.Reset();
        Progress progress{&engine,cancel};
        modules::ThinkConfig cfg;
        cfg.continuousAnalysis=true;
        cfg.maxThinkMs=90000U;
        cfg.maxNodes=0U;
        cfg.candidateObserver={&Candidate,nullptr};
        cfg.progressObserver={&Progress::On,&progress};
        assert(engine.StartDuckThink(modules::INITIAL_FEN,32U,cfg));
        modules::UciMove move;
        uint8_t percent=0U;
        const auto result=engine.PollThinkProgress(percent,move);
        modules::DuckThinkResult compound;
        const bool have=engine.GetLastDuckThinkResult(compound);
        // A cancellation before the first complete root iteration must not
        // publish a partial Duck candidate. A deadline that reaches a
        // completed iteration still publishes the best complete result.
        if (cancel) {
            assert(publishes == 0U);
        } else {
            assert(publishes > 0U);
        }
        if(cancel) assert(result==modules::ThinkState::Canceled && !have && ticks<90000U);
        else assert(result==modules::ThinkState::Done && have && ticks>=90000U && ticks<90010U);
        std::printf("adapter cancel=%u simulated_ms=%u nodes=%u publishes=%u\n",
                    cancel,ticks,observedNodes,publishes);
    }
    return 0;
}
