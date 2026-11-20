#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "foxie_embedded/test_support/test_assert.hpp"
#include "freertos/task.h"
#include "logger.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/vs_cpu_difficulty.hpp"

namespace {

std::thread g_task;
std::mutex g_taskMutex;
std::condition_variable g_taskCondition;
uint32_t g_notifications = 0U;
bool g_stopTask = false;
bool g_dynamicTask = false;
std::atomic<uint32_t> g_largestHeapBlock{65536U};
std::atomic<uint32_t> g_dynamicCreates{0U};
std::atomic<uint32_t> g_staticCreates{0U};
std::atomic<uint32_t> g_failedCreates{0U};
std::atomic<bool> g_taskArgumentsValid{true};
StackType_t* g_staticStack = nullptr;
StaticTask_t* g_staticControlBlock = nullptr;
struct TaskStopped {};

void JoinFinishedDynamicTask() {
    if (g_dynamicTask && g_task.joinable()) g_task.join();
}

void StopTask() {
    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        g_stopTask = true;
    }
    g_taskCondition.notify_all();
    if (g_task.joinable()) g_task.join();
}

void LaunchTask(TaskFunction_t task, void* context, bool dynamicTask) {
    if (g_task.joinable()) g_task.join();
    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        g_notifications = 0U;
        g_stopTask = false;
    }
    g_dynamicTask = dynamicTask;
    g_task = std::thread([task, context]() {
        try { task(context); } catch (const TaskStopped&) {}
    });
}
std::atomic<uint32_t> g_delayCalls{0U};
std::atomic<bool> g_blockUnwatch{false};
std::atomic<bool> g_unwatchEntered{false};
std::atomic<bool> g_allowUnwatch{true};
std::atomic<uint32_t> g_stackSamples{0U};
std::atomic<bool> g_sampleAfterCleanup{false};

} // namespace

BaseType_t xTaskCreatePinnedToCore(
    const TaskFunction_t task, const char*, const uint32_t stackBytes,
    void* const context, const uint32_t priority,
    TaskHandle_t* const handle, const int core) {
    ++g_dynamicCreates;
    g_taskArgumentsValid.store(g_taskArgumentsValid.load() &&
        stackBytes == 8192U && priority == 2U && core == 0);
    // Equivalent controlled fault: the captured S3 largest internal block was
    // 8180 bytes, below this exact shipping 8192-byte stack request. This host
    // boundary models allocation failure; it does not emulate ESP-IDF's heap.
    if (stackBytes > g_largestHeapBlock.load()) {
        ++g_failedCreates;
        if (handle != nullptr) *handle = nullptr;
        return 0;
    }
    LaunchTask(task, context, true);
    if (handle != nullptr) *handle = reinterpret_cast<TaskHandle_t>(1U);
    return pdPASS;
}

TaskHandle_t xTaskCreateStaticPinnedToCore(
    const TaskFunction_t task, const char*, const uint32_t stackBytes,
    void* const context, const UBaseType_t priority,
    StackType_t* const stack, StaticTask_t* const controlBlock,
    const BaseType_t core) {
    ++g_staticCreates;
    g_staticStack = stack;
    g_staticControlBlock = controlBlock;
    g_taskArgumentsValid.store(g_taskArgumentsValid.load() &&
        stackBytes == 8192U && priority == 2U && core == 0 &&
        stack != nullptr && controlBlock != nullptr);
    LaunchTask(task, context, false);
    return reinterpret_cast<TaskHandle_t>(1U);
}

BaseType_t xTaskNotifyGive(const TaskHandle_t handle) {
    if (handle == nullptr) return 0;
    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        ++g_notifications;
    }
    g_taskCondition.notify_all();
    return pdPASS;
}

uint32_t ulTaskNotifyTake(const BaseType_t clearOnExit, const TickType_t ticks) {
    std::unique_lock<std::mutex> lock(g_taskMutex);
    if (ticks == portMAX_DELAY) {
        g_taskCondition.wait(lock, []() { return g_stopTask || g_notifications != 0U; });
    }
    if (g_stopTask) throw TaskStopped{};
    const uint32_t prior = g_notifications;
    if (clearOnExit == pdTRUE) g_notifications = 0U;
    else if (g_notifications != 0U) --g_notifications;
    return prior;
}

void vTaskDelay(const uint32_t) {
    ++g_delayCalls;
    std::this_thread::yield();
}

void vTaskDelete(const TaskHandle_t handle) {
    if (handle != nullptr) StopTask();
}

unsigned int uxTaskGetStackHighWaterMark(TaskHandle_t handle) {
    ++g_stackSamples;
    g_sampleAfterCleanup.store(
        handle == nullptr && g_unwatchEntered.load() && g_allowUnwatch.load());
    return 1234U;
}

// Compile and execute the production Arduino branch in this native contract
// suite. Renaming only avoids colliding with the normally linked native class;
// the cancellation/task-tail implementation below is the shipping source.
#define ARDUINO 1
#define LITCH_ESP32S3 1
#define AsyncThinkWorker ArduinoAsyncThinkWorker
#define UnwatchCurrentTask TestUnwatchCurrentTask
#include "../../src/async_think_worker.cpp"
#undef UnwatchCurrentTask
#undef AsyncThinkWorker
#undef LITCH_ESP32S3
#undef ARDUINO

namespace runtime {

void TestUnwatchCurrentTask(const uint8_t) {
    g_unwatchEntered.store(true, std::memory_order_release);
    while (g_blockUnwatch.load(std::memory_order_acquire) &&
           !g_allowUnwatch.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

} // namespace runtime

namespace {

class ControlledEngine final : public modules::IEngine {
  public:
    void Reset() override {}
    bool SetFen(const char*) override { return true; }
    bool GetFen(char*, size_t) const override { return false; }
    bool ApplyUciMove(const char*) override { return false; }
    modules::SideToMove GetSideToMove() const override {
        return modules::SideToMove::White;
    }
    bool IsInCheck(modules::SideToMove) const override { return false; }
    bool IsInCheckmate(modules::SideToMove) const override { return false; }
    bool IsInStalemate(modules::SideToMove) const override { return false; }
    bool GetLegalDestinationsMask(uint8_t, uint64_t& mask) override {
        mask = 0ULL;
        return true;
    }
    bool StartThink(const modules::ThinkConfig& config) override {
        candidateObserver = config.candidateObserver;
        cancelRequested.store(false, std::memory_order_release);
        return true;
    }
    void CancelThink() override {
        ++cancelCalls;
        cancelRequested.store(true, std::memory_order_release);
    }
    modules::ThinkState PollThinkProgress(
        uint8_t& progress,
        modules::UciMove& best) override {
        const uint32_t concurrent =
            pollsInProgress.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        uint32_t priorMax = maxConcurrentPolls.load(std::memory_order_relaxed);
        while (priorMax < concurrent &&
               !maxConcurrentPolls.compare_exchange_weak(
                   priorMax, concurrent, std::memory_order_relaxed)) {}
        modules::ThinkCandidate candidate{12U, 4U, 'n', 35U, true, false};
        // This tuple claims no completed root or score; exercise its Seed copy.
        candidate.source = modules::ThinkCandidateSource::Seed;
        candidateObserver.Publish(candidate);
        pollEntered.store(true, std::memory_order_release);
        while (!releasePoll.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        pollsInProgress.fetch_sub(1U, std::memory_order_acq_rel);
        progress = 100U;
        (void)best.Assign("a2a3");
        return cancelRequested.load(std::memory_order_acquire)
                   ? modules::ThinkState::Canceled
                   : modules::ThinkState::Done;
    }
    bool GetLastThinkStats(modules::ThinkStats& stats) const override {
        stats.nodeCount = 1U;
        return true;
    }

    bool WaitUntilEntered() const {
        for (uint32_t attempt = 0U; attempt < 1000000U; ++attempt) {
            if (pollEntered.load(std::memory_order_acquire)) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    modules::ThinkCandidateObserver candidateObserver{};
    std::atomic<bool> pollEntered{false};
    std::atomic<bool> releasePoll{false};
    std::atomic<bool> cancelRequested{false};
    std::atomic<uint32_t> pollsInProgress{0U};
    std::atomic<uint32_t> maxConcurrentPolls{0U};
    std::atomic<uint32_t> cancelCalls{0U};
};

bool WaitUntilInactive(
    const modules::ArduinoAsyncThinkWorker& worker) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!worker.IsActive()) return true;
        std::this_thread::yield();
    }
    return false;
}

bool WaitUntilUnwatchEntered() {
    for (uint32_t attempt = 0U; attempt < 1000000U; ++attempt) {
        if (g_unwatchEntered.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

void CheckCancelBeforeReady(
    fxe::test_support::TestResults& results) {
    g_blockUnwatch.store(true, std::memory_order_release);
    g_unwatchEntered.store(false, std::memory_order_release);
    g_allowUnwatch.store(false, std::memory_order_release);
    ControlledEngine engine;
    modules::ArduinoAsyncThinkWorker worker;
    modules::ThinkConfig config;
    results.Check(engine.StartThink(config) && worker.Start(engine) &&
                      engine.WaitUntilEntered(),
                  "Arduino worker enters the controlled search");

    g_delayCalls.store(0U);
    const bool firstCancel = worker.CancelAndJoin(engine);
    results.Check(!firstCancel && worker.IsActive() &&
                      g_delayCalls.load() == 0U,
                  "cancel-before-ready reports pending without delaying the loop");
    results.Check(!worker.Start(engine),
                  "pending worker cannot be overlapped by a restart");

    engine.releasePoll.store(true, std::memory_order_release);
    results.Check(WaitUntilUnwatchEntered() && worker.IsDone() &&
                      worker.IsActive() && !worker.Start(engine) &&
                      !worker.CancelAndJoin(engine),
                  "published result does not release ownership before shared watchdog cleanup");
    g_allowUnwatch.store(true, std::memory_order_release);
    results.Check(WaitUntilInactive(worker),
                  "late completion publishes whole-task quiescence");
    JoinFinishedDynamicTask();
    results.Check(worker.CancelAndJoin(engine),
                  "retry observes the released worker as quiesced");

    g_blockUnwatch.store(false, std::memory_order_release);
    engine.pollEntered.store(false, std::memory_order_release);
    engine.releasePoll.store(true, std::memory_order_release);
    results.Check(engine.StartThink(config) && worker.Start(engine) &&
                      engine.WaitUntilEntered() &&
                      WaitUntilInactive(worker),
                  "normal restart completes after prior task cleanup");
    JoinFinishedDynamicTask();
    uint8_t progress = 0U;
    uint32_t elapsedMs = 0U;
    uint32_t nodeCount = 0U;
    modules::UciMove best;
    const modules::ThinkState state = worker.GetResult(
        progress, best, elapsedMs, nodeCount);
    const bool accepted = state == modules::ThinkState::Done &&
                          best.Equals("a2a3") &&
                          engine.maxConcurrentPolls.load() == 1U &&
                          g_delayCalls.load() == 0U;
    results.Check(accepted,
                  "result access and restart retain one worker owner");
    std::printf(
        "[ARDUINO_ASYNC_CANCEL_CONTRACT] first_cancel=%u delays=%u "
        "cancel_calls=%u max_concurrent=%u restart_state=%u "
        "best=%s accepted=%u\n",
        firstCancel ? 1U : 0U,
        static_cast<unsigned>(g_delayCalls.load()),
        static_cast<unsigned>(engine.cancelCalls.load()),
        static_cast<unsigned>(engine.maxConcurrentPolls.load()),
        static_cast<unsigned>(state),
        best.CStr(),
        accepted ? 1U : 0U);
    StopTask();
}

void CheckReadyBeforeCancel(
    fxe::test_support::TestResults& results) {
    g_blockUnwatch.store(false, std::memory_order_release);
    g_allowUnwatch.store(true, std::memory_order_release);
    ControlledEngine engine;
    modules::ArduinoAsyncThinkWorker worker;
    modules::ThinkConfig config;
    engine.releasePoll.store(true, std::memory_order_release);
    results.Check(engine.StartThink(config) && worker.Start(engine) &&
                      engine.WaitUntilEntered() &&
                      WaitUntilInactive(worker),
                  "ready-before-cancel worker completes normally");
    JoinFinishedDynamicTask();
    results.Check(worker.CancelAndJoin(engine) && !worker.IsDone(),
                  "ready result is safely discarded by later cancellation");
    StopTask();
}

void CheckLiveCandidate(fxe::test_support::TestResults& results) {
    modules::ArduinoAsyncThinkWorker worker;
    ControlledEngine engine;
    modules::ThinkConfig config;
    config.continuousAnalysis = true;
    config.progressObserver = worker.PrepareProgressObserver(true);
    config.candidateObserver = worker.PrepareCandidateObserver();
    worker.PrepareDiagnostics(1U, modules::ThinkRequestKind::Hint, config, modules::SideToMove::White, true, 35U);
    worker.RecordPreflight();
    modules::ThinkCandidate candidate;
    results.Check(!worker.TryGetPublishedCandidate(candidate), "new mailbox empty");
    results.Check(engine.StartThink(config) && worker.Start(engine) && engine.WaitUntilEntered(),
                  "search runs with candidate observer");
    results.Check(worker.IsActive() && !worker.IsDone() &&
                      worker.TryGetPublishedCandidate(candidate) &&
                      candidate.fromSquare == 12U && candidate.toSquare == 4U &&
                      candidate.promotion == 'n' && candidate.duckDestination == 35U &&
                      candidate.isDuck && !candidate.capturedKing &&
                      candidate.source == modules::ThinkCandidateSource::Seed &&
                      candidate.completedDepth == 0U,
                  "active search publishes coherent compound promotion without waiting");
    modules::ThinkDiagnostics diagnostics;
    results.Check(worker.TryGetDiagnostics(diagnostics) && diagnostics.active &&
                      diagnostics.pollValid && diagnostics.firstPublicationValid &&
                      !diagnostics.finalValid && !diagnostics.effectiveValid && !diagnostics.rootProgressValid &&
                      diagnostics.latestPublication.rootKind == modules::ThinkRootKind::Unavailable &&
                      diagnostics.latestPublication.scoreUnits == modules::ThinkScoreUnits::Unavailable &&
                      !diagnostics.latestPublication.scoreValid &&
                      diagnostics.latestPublication.completedNodes == 0U &&
                      diagnostics.completedRootCount == 0U,
                  "actual Arduino active cached copy preserves unknown provenance without engine reads");
    results.Check(worker.PrepareCandidateObserver().publish == nullptr,
                  "active mailbox cannot be reset");
    engine.releasePoll.store(true, std::memory_order_release);
    results.Check(WaitUntilInactive(worker), "candidate search finishes");
    JoinFinishedDynamicTask();
    results.Check(worker.PublishedNodeCount() == 1U, "terminal nodes retained");
    results.Check(worker.TryGetDiagnostics(diagnostics) && diagnostics.finalValid && diagnostics.finalNodesValid &&
                      diagnostics.finalNodes == 1U && diagnostics.quiescentValid && !diagnostics.active &&
                      diagnostics.state == modules::ThinkState::Done,
                  "actual Arduino final nodes and cleanup completion are coherent cached evidence");
    results.Check(worker.CancelAndJoin(engine) && !worker.TryGetPublishedCandidate(candidate),
                  "quiescent invalidation discards stale candidate");
    StopTask();
}

void CheckDiagnosticTail(fxe::test_support::TestResults& results) {
    const auto priorLevel = logger::GetMinLevel();
    logger::SetMinLevel(logger::Level::TRACE);
    g_stackSamples.store(0U);
    g_unwatchEntered.store(false);
    std::atomic<bool> logEntered{false};
    std::atomic<bool> allowLog{false};
    std::atomic<bool> correctUnits{false};
    logger::SetOutputLineCallback([&](const char* line) {
        if (std::strstr(line, "cpu_think") == nullptr) {
            return;
        }
        correctUnits.store(std::strstr(line, "min_free_stack_bytes=1234") != nullptr);
        logEntered.store(true, std::memory_order_release);
        while (!allowLog.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    ControlledEngine engine;
    modules::ArduinoAsyncThinkWorker worker;
    modules::ThinkConfig config;
    worker.PrepareDiagnostics(1U, modules::ThinkRequestKind::Cpu, config, modules::SideToMove::White, false, 255U);
    engine.releasePoll.store(true);
    results.Check(engine.StartThink(config) && worker.Start(engine),
                  "diagnostic worker starts");
    for (uint32_t i = 0U; i < 1000000U && !logEntered.load(); ++i) {
        std::this_thread::yield();
    }
    results.Check(logEntered.load() && worker.IsActive() && worker.IsDone() &&
                      !worker.Start(engine) && !worker.CancelAndJoin(engine),
                  "slow completion logger retains whole-task ownership");
    modules::ThinkDiagnostics diagnostic;
    results.Check(worker.TryGetDiagnostics(diagnostic) && diagnostic.finalValid && diagnostic.active &&
                      !diagnostic.quiescentValid,
                  "actual Arduino diagnostic tail cannot claim quiescence from result publication");
    allowLog.store(true, std::memory_order_release);
    results.Check(WaitUntilInactive(worker), "diagnostic tail releases ownership");
    JoinFinishedDynamicTask();
    results.Check(g_stackSamples.load() == 1U && g_sampleAfterCleanup.load() &&
                      correctUnits.load(),
                  "actual task sampled once after cleanup and reported in bytes");
    logger::SetOutputLineCallback({});
    logger::SetMinLevel(logger::Level::INFO);
    results.Check(engine.StartThink(config) && worker.Start(engine) &&
                      WaitUntilInactive(worker), "info-level worker completes");
    JoinFinishedDynamicTask();
    results.Check(g_stackSamples.load() == 1U,
                  "normal info level does not scan task stack");
    logger::SetMinLevel(priorLevel);
    StopTask();
}


void CheckCapturedHintToCpuAllocation(fxe::test_support::TestResults& results) {
    constexpr const char* BEFORE_CAPTURE =
        "rn1qkb1r/pp1bpp2/3p1n1p/3p2p1/4P3/2NB1N1P/PPP2PP1/R1BQ1RK1 w kq - 0 1";
    constexpr const char* AFTER_CAPTURE =
        "rn1qkb1r/pp1bpp2/3p1n1p/3N2p1/4P3/3B1N1P/PPP2PP1/R1BQ1RK1 b kq - 0 1";
    g_largestHeapBlock.store(65536U);
    g_dynamicCreates.store(0U);
    g_staticCreates.store(0U);
    g_failedCreates.store(0U);
    g_taskArgumentsValid.store(true);
    modules::LitchMaxEngine engine;
    modules::ArduinoAsyncThinkWorker worker;
    modules::ThinkConfig hint;
    hint.continuousAnalysis = true;
    hint.maxDepth = 5U;
    hint.maxNodes = 1800000U;
    hint.candidateObserver = worker.PrepareCandidateObserver();
    const bool hintStarted = engine.SetFen(BEFORE_CAPTURE) &&
        engine.StartThink(hint) && worker.Start(engine);
    results.Check(hintStarted && WaitUntilInactive(worker),
                  "real captured-position Full5 Hint completes before human capture");
    JoinFinishedDynamicTask();
    modules::ThinkCandidate hintCandidate;
    results.Check(worker.IsDone() && worker.TryGetPublishedCandidate(hintCandidate) &&
                      hintCandidate.completedDepth >= 5U,
                  "retained Hint was analyzed with five complete Full plies");
    StackType_t* const stack = g_staticStack;
    StaticTask_t* const controlBlock = g_staticControlBlock;
    results.Check(worker.CancelAndJoin(engine) && engine.ApplyUciMove("c3d5"),
                  "quiescent real WKnight captures the recorded BPawn on d5");
    char fen[128]{};
    results.Check(engine.GetFen(fen, sizeof(fen)) && std::strcmp(fen, AFTER_CAPTURE) == 0,
                  "production engine reaches the exact witnessed stalled Black turn");
    // Runtime allocation pressure changes between the Hint and CPU searches.
    // A new task needs a contiguous stack; a retained task needs no new block.
    g_largestHeapBlock.store(8180U);
    auto cpu = modules::MakeThinkConfigForVsCpuLevel(4U, 1U);
    cpu.candidateObserver = worker.PrepareCandidateObserver();
    const bool engineStarted = engine.StartThink(cpu);
    const bool workerStarted = engineStarted && worker.Start(engine);
    results.Check(workerStarted,
                  "CPU4 starts after Hint and human capture with largest heap block8180");
    if (workerStarted) {
        results.Check(WaitUntilInactive(worker), "CPU4 actual search completes under allocation pressure");
        JoinFinishedDynamicTask();
        uint8_t progress = 0U;
        uint32_t elapsedMs = 0U, nodes = 0U;
        modules::UciMove best;
        const auto state = worker.GetResult(progress, best, elapsedMs, nodes);
        modules::ThinkCandidate candidate;
        results.Check(state == modules::ThinkState::Done && nodes > 0U &&
                          worker.TryGetPublishedCandidate(candidate) &&
                          candidate.completedDepth >= 1U && engine.ApplyUciMove(best.CStr()),
                      "allocation-safe CPU supplies a legal complete analyzed reply");
        results.Check(worker.CancelAndJoin(engine), "completed CPU output retires before next human Hint");
        modules::ThinkConfig nextHint = hint;
        nextHint.candidateObserver = worker.PrepareCandidateObserver();
        const bool nextStarted = engine.StartThink(nextHint) && worker.Start(engine);
        results.Check(nextStarted && WaitUntilInactive(worker) && worker.IsDone(),
                      "next human Full5 Hint starts and completes after prompted CPUmove under8180 pressure");
        modules::ThinkCandidate nextCandidate;
        uint64_t legalMask = 0ULL;
        const bool nextAnalyzed = worker.TryGetPublishedCandidate(nextCandidate) &&
            nextCandidate.completedDepth >= 5U &&
            engine.GetSideToMove() == modules::SideToMove::White &&
            engine.GetLegalDestinationsMask(nextCandidate.fromSquare, legalMask) &&
            ((legalMask >> nextCandidate.toSquare) & 1ULL) != 0ULL;
        results.Check(nextAnalyzed, "next human Hint is legal Full5 guidance for the newWhite turn");
        results.Check(g_staticCreates.load() == 1U && g_dynamicCreates.load() == 0U &&
                          stack == g_staticStack && controlBlock == g_staticControlBlock &&
                          g_taskArgumentsValid.load(),
                      "Hint CPU and nextHint retain one8192-byte S3core0priority2task without allocation");
        std::printf("[CAPTURED_NEXT_HUMAN_HINT] largest_block=8180 started=%u "
                    "completed_depth=%u legal=%u static_creates=%u dynamic_creates=%u\n",
                    nextStarted, nextCandidate.completedDepth, nextAnalyzed,
                    g_staticCreates.load(), g_dynamicCreates.load());
    } else {
        engine.CancelThink();
        uint8_t progress = 0U;
        modules::UciMove best;
        (void)engine.PollThinkProgress(progress, best);
    }
    std::printf("[CAPTURED_S3_HINT_CPU_ALLOCATION] largest_block=8180 hint_started=%u "
                "cpu_engine_started=%u cpu_worker_started=%u dynamic_creates=%u "
                "static_creates=%u failed_creates=%u accepted=%u\n",
                hintStarted, engineStarted, workerStarted,
                g_dynamicCreates.load(), g_staticCreates.load(),
                g_failedCreates.load(), workerStarted);
    StopTask();
    g_largestHeapBlock.store(65536U);
}


void CheckCapturedCpuToHintAllocation(fxe::test_support::TestResults& results) {
    constexpr const char* CPU_FEN =
        "rn1qkb1r/pp1bpp2/3p1n1p/3N2p1/4P3/3B1N1P/PPP2PP1/R1BQ1RK1 b kq - 0 1";
    constexpr const char* HUMAN_FEN =
        "rn1qkb1r/pp1b1p2/3ppn1p/3N2p1/4P3/3B1N1P/PPP2PP1/R1BQ1RK1 w kq - 0 1";
    g_largestHeapBlock.store(65536U);
    g_dynamicCreates.store(0U);
    g_staticCreates.store(0U);
    g_failedCreates.store(0U);
    modules::LitchMaxEngine engine;
    modules::ArduinoAsyncThinkWorker worker;
    const auto cpu = modules::MakeThinkConfigForVsCpuLevel(4U, 1U);
    const bool cpuStarted = engine.SetFen(CPU_FEN) && engine.StartThink(cpu) && worker.Start(engine);
    results.Check(cpuStarted && WaitUntilInactive(worker), "actual captured CPU4 reply finishes before next human turn");
    JoinFinishedDynamicTask();
    uint8_t progress = 0U;
    uint32_t elapsedMs = 0U, nodes = 0U;
    modules::UciMove best;
    results.Check(worker.GetResult(progress, best, elapsedMs, nodes) == modules::ThinkState::Done &&
                      best.Equals("e7e6") && worker.CancelAndJoin(engine) && engine.ApplyUciMove(best.CStr()),
                  "actual engine commits the exact observed e7e6 CPUreply");
    char fen[128]{};
    results.Check(engine.GetFen(fen, sizeof(fen)) && std::strcmp(fen, HUMAN_FEN) == 0,
                  "next Hint allocation probe reaches the exact newly observed WhiteFEN");
    g_largestHeapBlock.store(8180U);
    modules::ThinkConfig hint;
    hint.continuousAnalysis = true;
    hint.maxDepth = 5U;
    hint.maxNodes = 1800000U;
    hint.candidateObserver = worker.PrepareCandidateObserver();
    const bool engineStarted = engine.StartThink(hint);
    const bool hintStarted = engineStarted && worker.Start(engine);
    results.Check(hintStarted, "requested nexthumanHint starts with largest8180 after CPUreply");
    if (hintStarted) {
        results.Check(WaitUntilInactive(worker), "actual nexthumanHint completes its strict Full5 comparison");
        JoinFinishedDynamicTask();
        modules::ThinkCandidate candidate;
        uint64_t legalMask = 0ULL;
        results.Check(worker.IsDone() && worker.TryGetPublishedCandidate(candidate) &&
                          candidate.completedDepth >= 5U &&
                          engine.GetLegalDestinationsMask(candidate.fromSquare, legalMask) &&
                          ((legalMask >> candidate.toSquare) & 1ULL) != 0ULL,
                      "actual nexthumanHint retains a legal analyzed current-position candidate");
    } else {
        engine.CancelThink();
        (void)engine.PollThinkProgress(progress, best);
    }
    std::printf("[CAPTURED_CPU_TO_HINT_ALLOCATION] largest_block=8180 cpu_started=%u "
                "hint_engine_started=%u hint_worker_started=%u dynamic_creates=%u "
                "static_creates=%u failed_creates=%u\n",
                cpuStarted, engineStarted, hintStarted, g_dynamicCreates.load(),
                g_staticCreates.load(), g_failedCreates.load());
    StopTask();
    g_largestHeapBlock.store(65536U);
}

} // namespace

int main() {
    fxe::test_support::TestResults results(
        "ASYNC_THINK_WORKER_ARDUINO_CONTRACT");
    results.Start();
    CheckCancelBeforeReady(results);
    CheckReadyBeforeCancel(results);
    CheckDiagnosticTail(results);
    CheckLiveCandidate(results);
    CheckCapturedHintToCpuAllocation(results);
    CheckCapturedCpuToHintAllocation(results);
    return results.Finish();
}
