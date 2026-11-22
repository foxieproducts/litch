#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstring>
#include <mcu-max.h>
#include "modules/litch_max_engine.hpp"
#include "modules/async_think_worker.hpp"
using namespace modules;
namespace {
void CheckDiagnosticCoherence() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    AsyncThinkWorker worker;
    ThinkConfig config;
    worker.PrepareDiagnostics(41U, ThinkRequestKind::Hint, config, SideToMove::Black, true, 18U);
    auto observer = worker.PrepareCandidateObserver();
    ThinkCandidate first{52U, 36U, 'q', 18U, true, false, 5U};
    first.rootKind = ThinkRootKind::DuckCompound;
    first.scoreUnits = ThinkScoreUnits::DuckPawn1000;
    first.scorePerspective = SideToMove::Black;
    first.score = 1234;
    first.scoreValid = true;
    first.completedNodes = 42U;
    ThinkCandidate second{12U, 4U, 'n', 255U, true, true, 6U};
    second.rootKind = ThinkRootKind::DuckCompound;
    second.scoreUnits = ThinkScoreUnits::DuckPawn1000;
    second.scorePerspective = SideToMove::White;
    second.score = -5678;
    second.scoreValid = true;
    second.completedNodes = 84U;
    observer.RootCompleted(first);
    observer.Publish(first);
    ThinkDiagnostics data;
    assert(worker.TryGetDiagnostics(data) && data.requestAtMs == 0U &&
           data.firstPublicationValid && data.firstPublicationAtMs == 0U &&
           data.completedRootCount == 1U && data.publicationCount == 1U);
    observer.RootCompleted(first);
    observer.Publish(first);
    second.replayed = true;
    observer.RootCompleted(second);
    observer.Publish(second);
    assert(worker.TryGetDiagnostics(data) && data.completedRootCount == 1U && data.publicationCount == 1U);
    second.replayed = false;
    std::atomic<bool> stop{false};
    std::atomic<bool> publishedPair{false};
    std::thread writer([&] {
        while (!stop.load()) {
            observer.Publish(first);
            observer.Publish(second);
            publishedPair.store(true);
        }
    });
    while (!publishedPair.load()) std::this_thread::yield();
    uint32_t coherent = 0U;
    for (uint32_t i = 0U; i < 100000U; ++i) if (worker.TryGetDiagnostics(data)) {
        ++coherent;
        const auto& candidate = data.latestPublication;
        assert(data.requestId == 41U && data.firstPublicationAtMs == 0U &&
               data.firstPublication.fromSquare == 52U && data.firstPublication.score == 1234);
        assert((candidate.fromSquare == 52U && candidate.toSquare == 36U && candidate.promotion == 'q' &&
                candidate.completedDepth == 5U && candidate.score == 1234 && candidate.completedNodes == 42U &&
                candidate.scorePerspective == SideToMove::Black && !candidate.capturedKing) ||
               (candidate.fromSquare == 12U && candidate.toSquare == 4U && candidate.promotion == 'n' &&
                candidate.completedDepth == 6U && candidate.score == -5678 && candidate.completedNodes == 84U &&
                candidate.scorePerspective == SideToMove::White && candidate.capturedKing));
    }
    stop.store(true);
    writer.join();
    // Bounded reads may all reject a busy writer. Once joined, the last complete
    // pair must be readable with its original request and first publication.
    assert(worker.TryGetDiagnostics(data) && data.requestId == 41U && data.requestAtMs == 0U &&
           data.firstPublicationValid && data.firstPublicationAtMs == 0U &&
           data.firstPublication.fromSquare == 52U && data.firstPublication.score == 1234);
    const auto& finalCandidate = data.latestPublication;
    assert(finalCandidate.fromSquare == second.fromSquare && finalCandidate.toSquare == second.toSquare &&
           finalCandidate.promotion == second.promotion && finalCandidate.duckDestination == second.duckDestination &&
           finalCandidate.isDuck == second.isDuck && finalCandidate.capturedKing == second.capturedKing &&
           finalCandidate.completedDepth == second.completedDepth && finalCandidate.source == second.source &&
           finalCandidate.offlineDepth == second.offlineDepth && finalCandidate.rootKind == second.rootKind &&
           finalCandidate.scoreUnits == second.scoreUnits && finalCandidate.scorePerspective == second.scorePerspective &&
           finalCandidate.score == second.score && finalCandidate.completedNodes == second.completedNodes &&
           finalCandidate.scoreValid == second.scoreValid && finalCandidate.replayed == second.replayed);
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 10U);
    worker.PrepareDiagnostics(42U, ThinkRequestKind::Cpu, config, SideToMove::White, false, 255U);
    assert(worker.TryGetDiagnostics(data) && !data.firstPublicationValid && data.completedRootCount == 0U &&
           !data.firstAcceptedHintValid && !data.effectiveValid);
    fxe::ManualMillis::SetNowMs(5U);
    observer.Publish(first);
    assert(worker.TryGetDiagnostics(data) && data.firstPublicationValid &&
           data.firstPublicationAtMs - data.requestAtMs == 16U);
    printf("[DIAGNOSTIC_COHERENCE] successful_copies=%u post_join=PASS fixed_snapshot_bytes=%zu worker_bytes=%zu zero_and_wrap=PASS\n",
           coherent, sizeof(ThinkDiagnostics), sizeof(AsyncThinkWorker));
    printf("[DIAGNOSTIC_RESOURCE] candidate=%zu config=%zu profile=%zu workspace_native=%zu\n",
           sizeof(ThinkCandidate), sizeof(ThinkConfig), sizeof(ThinkProfile), litch_max::classic_chess::SearchWorkspace::RequiredBytes());
}
}

int main() {
 CheckDiagnosticCoherence();
 fxe::ScopedMillisProviderOverride realClock(nullptr);
 AsyncThinkWorker mailbox;
 auto pub=mailbox.PrepareCandidateObserver();
 ThinkCandidate out;
 assert(!mailbox.TryGetPublishedCandidate(out));
 std::atomic<bool> stop{false};
 // Atomic tuple stress deliberately carries no completed-root/score evidence.
 ThinkCandidate firstSeed{52,36,'q',18,true,false};
 ThinkCandidate secondSeed{12,4,'n',255,true,true};
 firstSeed.source=ThinkCandidateSource::Seed;
 firstSeed.rootKind=ThinkRootKind::Seed;
 secondSeed.source=ThinkCandidateSource::Seed;
 secondSeed.rootKind=ThinkRootKind::Seed;
 std::thread writer([&]{while(!stop.load()){
   pub.Publish(firstSeed);
   pub.Publish(secondSeed);
 }});
 uint32_t coherentCopies=0;
 for(int i=0;i<100000;++i) {
   if(!mailbox.TryGetPublishedCandidate(out)) {std::this_thread::yield();continue;}
   ++coherentCopies;
   assert(out.source==ThinkCandidateSource::Seed&&out.completedDepth==0&&
          out.rootKind==ThinkRootKind::Seed&&out.scoreUnits==ThinkScoreUnits::Unavailable&&
          !out.scoreValid&&out.completedNodes==0);
   assert((out.fromSquare==52&&out.toSquare==36&&out.promotion=='q'&&out.duckDestination==18&&out.isDuck&&!out.capturedKing)||
          (out.fromSquare==12&&out.toSquare==4&&out.promotion=='n'&&out.duckDestination==255&&out.isDuck&&out.capturedKing));
 }
 stop=true;writer.join();assert(coherentCopies!=0);mailbox.PrepareCandidateObserver();assert(!mailbox.TryGetPublishedCandidate(out));
 LitchMaxEngine engine;engine.Reset();
 ThinkConfig cfg;cfg.continuousAnalysis=true;cfg.maxThinkMs=0;cfg.maxNodes=1800000;cfg.maxDepth=6;cfg.difficulty=1;
 cfg.candidateObserver=mailbox.PrepareCandidateObserver();cfg.progressObserver=mailbox.PrepareProgressObserver(cfg.continuousAnalysis);
 assert(engine.StartThink(cfg)&&mailbox.Start(engine));
 bool sawActive=false;uint32_t observedNodes=0;
 while(mailbox.IsActive()) {
   if(mailbox.TryGetPublishedCandidate(out)) {assert(out.source==ThinkCandidateSource::Search&&out.completedDepth>=1&&out.completedDepth<=cfg.maxDepth);sawActive=true;observedNodes=mailbox.PublishedNodeCount();}
   std::this_thread::yield();
 }
 uint8_t progress;UciMove best;uint32_t elapsed,nodes;
 assert(mailbox.GetResult(progress,best,elapsed,nodes)==ThinkState::Done);
 assert(sawActive&&observedNodes>0&&nodes>400&&mailbox.PublishedNodeCount()==nodes);
 assert(mailbox.TryGetPublishedCandidate(out)&&out.source==ThinkCandidateSource::Search&&out.completedDepth>=1&&out.completedDepth<=cfg.maxDepth);
 printf("active publication PASS elapsed=%u nodes=%u completed_depth=%u\n",elapsed,nodes,out.completedDepth);
 assert(mailbox.CancelAndJoin(engine));assert(!mailbox.TryGetPublishedCandidate(out));
 assert(engine.SetFen("7k/P7/8/8/8/8/8/7K w - - 0 1"));
 cfg.candidateObserver=mailbox.PrepareCandidateObserver();cfg.progressObserver=mailbox.PrepareProgressObserver(cfg.continuousAnalysis);
 assert(engine.StartThink(cfg)&&mailbox.Start(engine));
 bool promotion=false;
 while(mailbox.IsActive()) {if(mailbox.TryGetPublishedCandidate(out)) {assert(out.source==ThinkCandidateSource::Search&&out.completedDepth>=1&&out.completedDepth<=cfg.maxDepth);if(out.fromSquare==8&&out.toSquare==0) {assert(out.promotion=='q');promotion=true;}}std::this_thread::yield();}
 assert(mailbox.GetResult(progress,best,elapsed,nodes)==ThinkState::Done);assert(promotion);
 assert(mailbox.CancelAndJoin(engine));engine.Reset();
 cfg.candidateObserver=mailbox.PrepareCandidateObserver();cfg.progressObserver=mailbox.PrepareProgressObserver(cfg.continuousAnalysis);cfg.maxThinkMs=90000;cfg.maxNodes=0;cfg.maxDepth=0;
 assert(engine.StartThink(cfg)&&mailbox.Start(engine));
 while(!mailbox.TryGetPublishedCandidate(out)&&mailbox.IsActive())std::this_thread::yield();
 while(!mailbox.CancelAndJoin(engine))std::this_thread::yield();
 assert(!mailbox.TryGetPublishedCandidate(out));char fen[180];assert(engine.GetFen(fen,sizeof(fen)));assert(strncmp(fen,"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR",43)==0);
 cfg.continuousAnalysis=false;cfg.maxThinkMs=1000;cfg.candidateObserver=mailbox.PrepareCandidateObserver();cfg.progressObserver=mailbox.PrepareProgressObserver(cfg.continuousAnalysis);
 assert(engine.StartThink(cfg)&&mailbox.Start(engine));while(mailbox.IsActive())std::this_thread::yield();assert(mailbox.GetResult(progress,best,elapsed,nodes)==ThinkState::Done);assert(mcumax.recursion_limit==0&&mcumax.recursion_depth==0);
 assert(mailbox.CancelAndJoin(engine));engine.Reset();
 cfg.continuousAnalysis=true;cfg.maxNodes=4096;cfg.maxThinkMs=0;
 mailbox.PrepareDiagnostics(99U, ThinkRequestKind::Hint, cfg, SideToMove::White, false, 255U);
 mailbox.RecordPreflight();
 cfg.candidateObserver=mailbox.PrepareCandidateObserver();cfg.progressObserver=mailbox.PrepareProgressObserver(cfg.continuousAnalysis);
 assert(engine.StartThink(cfg)&&mailbox.Start(engine));while(mailbox.IsActive())std::this_thread::yield();
 // This node cap completes scored roots; it does not impose a Full5 floor.
 assert(mailbox.GetResult(progress,best,elapsed,nodes)==ThinkState::Done);
 assert(!best.Empty()&&mailbox.TryGetPublishedCandidate(out)&&out.source==ThinkCandidateSource::Search);
 assert(nodes==4096);
 ThinkDiagnostics diagnostic;
 assert(mailbox.TryGetDiagnostics(diagnostic) && diagnostic.state==ThinkState::Done && diagnostic.finalValid &&
        diagnostic.quiescentValid && diagnostic.finalNodesValid && diagnostic.finalNodes==4096U &&
        diagnostic.effectiveValid && diagnostic.rootProgressValid && diagnostic.effective.minimumDepth==1U && diagnostic.effective.maxNodes==4096U &&
        diagnostic.fullDepth>0U && diagnostic.fullDepth==out.completedDepth && diagnostic.completedRootCount==diagnostic.fullDepth &&
        diagnostic.firstPublicationValid && diagnostic.firstPublication.source==ThinkCandidateSource::Search &&
        diagnostic.firstPublication.rootKind==ThinkRootKind::ClassicFull && diagnostic.firstPublication.completedDepth==1U &&
        diagnostic.firstPublication.scoreValid && !diagnostic.firstAcceptedHintValid);
 assert(mailbox.CancelAndJoin(engine));engine.Reset();cfg.maxNodes=1;
 mailbox.PrepareDiagnostics(100U, ThinkRequestKind::Hint, cfg, SideToMove::White, false, 255U);
 cfg.candidateObserver=mailbox.PrepareCandidateObserver();cfg.progressObserver=mailbox.PrepareProgressObserver(cfg.continuousAnalysis);
 assert(engine.StartThink(cfg)&&mailbox.Start(engine));while(mailbox.IsActive())std::this_thread::yield();
 assert(mailbox.GetResult(progress,best,elapsed,nodes)==ThinkState::Done);
 assert(!best.Empty()&&!mailbox.TryGetPublishedCandidate(out));
 ClassicThinkResult seedResult;
 assert(engine.GetLastClassicThinkResult(seedResult)&&seedResult.source==ThinkCandidateSource::Seed&&seedResult.candidateCount==1U);
 assert(mailbox.TryGetDiagnostics(diagnostic) && diagnostic.requestId==100U && diagnostic.state==ThinkState::Done &&
        diagnostic.finalNodes==1U && diagnostic.completedRootCount==0U && diagnostic.fullDepth==0U &&
        !diagnostic.firstPublicationValid && !diagnostic.firstFullRootValid && !diagnostic.firstAcceptedHintValid);
 puts("promotion, cancel, stale reset, CPU restart, bounded completed roots, internal Seed without Hint publication PASS");
 return 0;
}
