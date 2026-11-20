#include "modules/async_think_worker.hpp"

#include <cstring>
#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "runtime/progress_watchdog.hpp"

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace modules {

namespace {
#ifdef ARDUINO
// Priority 2 on S3 lets think run whenever BLE (pri 21+) is idle on Core 0.
// On C3 (single-core), priority 1 = same as loop; cooperative via yields.
#if defined(LITCH_ESP32S3)
constexpr uint32_t THINK_TASK_PRIORITY = 2U;
#else
constexpr uint32_t THINK_TASK_PRIORITY = 1U;
#endif

// S3: Think on Core 0 alongside BLE. BLE preemption is brief/bursty and far
// less damaging than think continuously preempting the LED render loop on
// Core 1. The Arduino loop keeps Core 1 exclusively for smooth animation.
constexpr int GetThinkTaskCore() {
    return 0;
}
#endif

} // namespace

AsyncThinkWorker::~AsyncThinkWorker() {
#ifndef ARDUINO
    if (m_worker.joinable()) {
        m_worker.join();
    }
#endif
}

void AsyncThinkWorker::PrepareDiagnostics(
    const uint32_t requestId, const ThinkRequestKind kind,
    const ThinkConfig& config, const SideToMove side,
    const bool isDuck, const uint8_t duckSquare) {
    if (IsActive()) return;
    m_diagnostics = ThinkDiagnostics{};
    m_diagnostics.requestId = requestId;
    m_diagnostics.kind = kind;
    m_diagnostics.requestAtMs = fxe::Millis();
    m_diagnostics.requested = ThinkProfile::Requested(config);
    m_diagnostics.side = side;
    m_diagnostics.isDuck = isDuck;
    m_diagnostics.duckSquare = duckSquare;
    PublishDiagnostics();
}

void AsyncThinkWorker::PublishDiagnostics() {
    uint32_t words[sizeof(ThinkDiagnostics) / sizeof(uint32_t)];
    std::memcpy(words, &m_diagnostics, sizeof(words));
    m_diagnosticRevision.fetch_add(1U);
    for (size_t i = 0U; i < m_diagnosticWords.size(); ++i) m_diagnosticWords[i].store(words[i]);
    m_diagnosticRevision.fetch_add(1U);
}

bool AsyncThinkWorker::TryGetDiagnostics(ThinkDiagnostics& out) const {
    for (uint8_t attempt = 0U; attempt < 2U; ++attempt) {
        const bool active = IsActive();
        const uint32_t before = m_diagnosticRevision.load();
        if ((before & 1U) != 0U) continue;
        uint32_t words[sizeof(ThinkDiagnostics) / sizeof(uint32_t)];
        for (size_t i = 0U; i < m_diagnosticWords.size(); ++i) words[i] = m_diagnosticWords[i].load();
        if (before != m_diagnosticRevision.load() || active != IsActive()) continue;
        std::memcpy(&out, words, sizeof(words));
        out.active = active;
        return out.requestId != 0U;
    }
    return false;
}

void AsyncThinkWorker::RecordPreflight() {
    if (IsActive()) return;
    m_diagnostics.preflightAtMs = fxe::Millis();
    m_diagnostics.preflightValid = true;
    PublishDiagnostics();
}

void AsyncThinkWorker::RecordSynchronousPollStarted() {
    if (IsActive()) return;
    if (!m_diagnostics.startValid) {
        m_diagnostics.startValid = true;
        m_diagnostics.startAtMs = fxe::Millis();
    }
    if (!m_diagnostics.pollValid) RecordPollStarted();
}

void AsyncThinkWorker::RecordSynchronousResult(const ThinkState state,
                                              const uint32_t nodes, const bool nodesValid) {
    if (IsActive() || state == ThinkState::Thinking) return;
    m_diagnostics.state = state;
    m_diagnostics.finalAtMs = fxe::Millis();
    m_diagnostics.finalValid = true;
    m_diagnostics.finalNodes = nodes;
    m_diagnostics.finalNodesValid = nodesValid;
    RecordQuiescent();
}

void AsyncThinkWorker::RecordStartFailure() {
    if (IsActive()) return;
    m_diagnostics.state = ThinkState::Error;
    m_diagnostics.stopReason = ThinkStopReason::Error;
    m_diagnostics.finalValid = true;
    m_diagnostics.finalAtMs = fxe::Millis();
    RecordQuiescent();
}

void AsyncThinkWorker::RecordEffectiveProfile(void* context, const ThinkProfile& profile) {
    auto& self = *static_cast<AsyncThinkWorker*>(context);
    self.m_diagnostics.effective = profile;
    self.m_diagnostics.effectiveValid = true;
    self.m_diagnostics.rootProgressValid = true;
    self.PublishDiagnostics();
}

void AsyncThinkWorker::RecordCompletedRoot(void* context, const ThinkCandidate& candidate) {
    auto& self = *static_cast<AsyncThinkWorker*>(context);
    if (candidate.replayed || candidate.source != ThinkCandidateSource::Search) return;
    uint8_t* depth = candidate.rootKind == ThinkRootKind::ClassicPreliminary ? &self.m_diagnostics.preliminaryDepth :
        candidate.rootKind == ThinkRootKind::ClassicFull ? &self.m_diagnostics.fullDepth :
        candidate.rootKind == ThinkRootKind::DuckCompound ? &self.m_diagnostics.duckDepth : nullptr;
    if (depth == nullptr || candidate.completedDepth <= *depth) return;
    self.m_diagnostics.rootProgressValid = true;
    *depth = candidate.completedDepth;
    ++self.m_diagnostics.completedRootCount;
    self.m_diagnostics.lastCompletedRoot = candidate;
    if (candidate.rootKind == ThinkRootKind::ClassicFull && !self.m_diagnostics.firstFullRootValid) {
        self.m_diagnostics.firstFullRootValid = true;
        self.m_diagnostics.firstFullRootAtMs = fxe::Millis();
    }
    if (candidate.rootKind == ThinkRootKind::ClassicFull &&
        candidate.completedDepth == 3U && !self.m_diagnostics.firstFull3RootValid) {
        self.m_diagnostics.firstFull3RootValid = true;
        self.m_diagnostics.firstFull3RootAtMs = fxe::Millis();
    }
    self.PublishDiagnostics();
}

void AsyncThinkWorker::RecordPollStarted() {
    m_diagnostics.pollAtMs = fxe::Millis();
    m_diagnostics.pollValid = true;
    PublishDiagnostics();
}

void AsyncThinkWorker::RecordQuiescent() {
    m_diagnostics.quiescentAtMs = fxe::Millis();
    m_diagnostics.quiescentValid = true;
    PublishDiagnostics();
}

ThinkProgressObserver AsyncThinkWorker::PrepareProgressObserver(const bool continuousHint) {
    if (IsActive()) return {};
    m_continuousProgress = continuousHint;
    m_lastProgressAtMs = fxe::Millis();
    m_liveNodes.store(0U, std::memory_order_release);
    return {&AsyncThinkWorker::PublishProgress, this};
}

ThinkCandidateObserver AsyncThinkWorker::PrepareCandidateObserver() {
    if (IsActive()) return {};
    m_liveCandidate.store(0U, std::memory_order_release);
    return {&AsyncThinkWorker::PublishCandidate, this,
            &AsyncThinkWorker::RecordCompletedRoot, &AsyncThinkWorker::RecordEffectiveProfile};
}

void AsyncThinkWorker::PublishCandidate(void* context,
                                       const ThinkCandidate& candidate) {
    if (candidate.fromSquare >= 64U || candidate.toSquare >= 64U ||
        candidate.completedDepth > 127U ||
        (candidate.isDuck && !candidate.capturedKing &&
         candidate.duckDestination >= 64U)) return;
    uint32_t promotion = 0U;
    switch (candidate.promotion) {
        case '\0': break;
        case 'q': promotion = 1U; break;
        case 'r': promotion = 2U; break;
        case 'b': promotion = 3U; break;
        case 'n': promotion = 4U; break;
        default: return;
    }
    // Spare promotion code5 marks a nonpromoting Classic book result. This
    // keeps source and offline provenance in the existing lock-free32-bit copy.
    const bool book = candidate.source == ThinkCandidateSource::Book;
    if (book) {
        if (candidate.isDuck || candidate.capturedKing || candidate.promotion != '\0' ||
            candidate.completedDepth != 0U || candidate.offlineDepth < 5U ||
            candidate.offlineDepth > 127U) return;
        promotion = 5U;
    } else if (candidate.offlineDepth != 0U ||
               (candidate.source == ThinkCandidateSource::Seed
                    ? candidate.completedDepth != 0U
                    : candidate.source != ThinkCandidateSource::Search || candidate.completedDepth == 0U)) return;
    const uint32_t depth = book ? candidate.offlineDepth : candidate.completedDepth;
    const uint32_t duck = candidate.isDuck && !candidate.capturedKing
                              ? candidate.duckDestination : 64U;
    // The upper seven bits carry the depth belonging to the encoded source.
    const uint32_t packed = candidate.fromSquare |
        (static_cast<uint32_t>(candidate.toSquare) << 6U) |
        (promotion << 12U) | (duck << 15U) |
        (static_cast<uint32_t>(candidate.isDuck) << 22U) |
        (static_cast<uint32_t>(candidate.capturedKing) << 23U) | (1U << 24U) |
        (depth << 25U);
    auto& self = *static_cast<AsyncThinkWorker*>(context);
    const uint32_t nowMs = fxe::Millis();
    const bool newPublication = !candidate.replayed &&
        (!self.m_diagnostics.firstPublicationValid || self.m_diagnostics.latestPublication.replayed ||
         candidate.rootKind != self.m_diagnostics.latestPublication.rootKind ||
         candidate.completedDepth != self.m_diagnostics.latestPublication.completedDepth ||
         candidate.offlineDepth != self.m_diagnostics.latestPublication.offlineDepth);
    self.m_liveCandidate.store(packed, std::memory_order_release);
    if (!self.m_diagnostics.firstPublicationValid) {
        self.m_diagnostics.firstPublicationValid = true;
        self.m_diagnostics.firstPublicationAtMs = nowMs;
        self.m_diagnostics.firstPublication = candidate;
    }
    self.m_diagnostics.latestPublication = candidate;
    if (newPublication) ++self.m_diagnostics.publicationCount;
    self.PublishDiagnostics();
}

bool AsyncThinkWorker::TryGetPublishedCandidate(ThinkCandidate& outCandidate) const {
    const uint32_t packed = m_liveCandidate.load(std::memory_order_acquire);
    outCandidate = ThinkCandidate{};
    if ((packed & (1U << 24U)) == 0U) return false;
    constexpr char PROMOTIONS[] = {'\0', 'q', 'r', 'b', 'n'};
    outCandidate.fromSquare = static_cast<uint8_t>(packed & 63U);
    outCandidate.toSquare = static_cast<uint8_t>((packed >> 6U) & 63U);
    const uint8_t promotion = static_cast<uint8_t>((packed >> 12U) & 7U);
    if (promotion > 5U) return false;
    outCandidate.promotion = promotion == 5U ? '\0' : PROMOTIONS[promotion];
    const uint8_t duck = static_cast<uint8_t>((packed >> 15U) & 127U);
    outCandidate.duckDestination = duck < 64U ? duck : 255U;
    outCandidate.isDuck = (packed & (1U << 22U)) != 0U;
    outCandidate.capturedKing = (packed & (1U << 23U)) != 0U;
    if (promotion == 5U) {
        outCandidate.source = ThinkCandidateSource::Book;
        outCandidate.offlineDepth = static_cast<uint8_t>(packed >> 25U);
    } else {
        outCandidate.completedDepth = static_cast<uint8_t>(packed >> 25U);
        if (outCandidate.completedDepth == 0U) {
            outCandidate.source = ThinkCandidateSource::Seed;
            outCandidate.rootKind = ThinkRootKind::Seed;
        }
    }
    return true;
}

void AsyncThinkWorker::PublishProgress(void* context, const uint32_t nodes) {
    auto& self = *static_cast<AsyncThinkWorker*>(context);
    if (self.m_continuousProgress) {
        // Latest observed count; engine callbacks need not run on every node.
        self.m_liveNodes.store(nodes, std::memory_order_release);
        return;
    }
    constexpr uint32_t SAMPLE_INTERVAL_MS = 250U;
    const uint32_t nowMs = fxe::Millis();
    if (nodes != 0U &&
        (self.m_liveNodes.load(std::memory_order_relaxed) == 0U ||
         nowMs - self.m_lastProgressAtMs >= SAMPLE_INTERVAL_MS)) {
        self.m_liveNodes.store(nodes, std::memory_order_release);
        self.m_lastProgressAtMs = nowMs;
    }
}

bool AsyncThinkWorker::Start(IEngine& engine) {
    if (m_active.load(std::memory_order_acquire)) {
        return false;
    }

#ifndef ARDUINO
    // A naturally completed native worker remains joinable until its result is
    // consumed or the lane is reused. Reap it before resetting shared output.
    if (m_worker.joinable()) {
        m_worker.join();
    }
#endif

    m_diagnostics.startAtMs = fxe::Millis();
    m_diagnostics.startValid = true;
    m_diagnostics.state = ThinkState::Thinking;
    PublishDiagnostics();
    m_done.store(false, std::memory_order_release);
    m_active.store(true, std::memory_order_release);
    m_state = ThinkState::Thinking;
    m_progress = 0;
    m_best.Clear();
    m_elapsedMs = 0U;
    m_nodeCount = 0;
    m_liveNodes.store(0U, std::memory_order_release);

#ifdef ARDUINO
    m_engine = &engine;
    if (m_task == nullptr) {
        // The firmware-lifetime owner reserves the same full stack and TCB.
        // A later Hint/CPU handoff cannot depend on a fresh contiguous heap
        // block, nor on the idle task reclaiming a just-deleted worker.
        // ESP-IDF specifies the stack depth in bytes, not FreeRTOS words.
        m_task = xTaskCreateStaticPinnedToCore(&AsyncThinkWorker::TaskEntry,
                                             "cpu_think",
                                             THINK_TASK_STACK_BYTES,
                                             this,
                                             THINK_TASK_PRIORITY,
                                             m_taskStack,
                                             &m_taskControlBlock,
                                             GetThinkTaskCore());
    }
    if (m_task == nullptr) {
        m_engine = nullptr;
        m_active.store(false, std::memory_order_release);
        m_done.store(false, std::memory_order_release);
        RecordStartFailure();
        return false;
    }
    // Notifications accumulate before the task reaches its wait, including
    // the first start and an immediate restart after whole-lane cleanup.
    (void)xTaskNotifyGive(static_cast<TaskHandle_t>(m_task));
#else
    m_worker = std::thread([this, &engine]() {
        Run(engine);
        RecordQuiescent();
        m_active.store(false, std::memory_order_release);
    });
#endif

    return true;
}

bool AsyncThinkWorker::CancelAndJoin(IEngine& engine) {
    if (m_active.load(std::memory_order_acquire)) {
        engine.CancelThink();
        if (m_active.load(std::memory_order_acquire)) {
            return false;
        }
    }

#ifndef ARDUINO
    if (m_worker.joinable()) {
        m_worker.join();
    }
#endif
    // Quiescence deliberately discards any coincident ready result. The
    // initiating position/owner transition has made that output stale.
    m_done.store(false, std::memory_order_release);
    m_liveNodes.store(0U, std::memory_order_release);
    m_liveCandidate.store(0U, std::memory_order_release);
    return true;
}

bool AsyncThinkWorker::IsActive() const {
    return m_active.load(std::memory_order_acquire);
}

bool AsyncThinkWorker::IsDone() const {
    return m_done.load(std::memory_order_acquire);
}

ThinkState AsyncThinkWorker::GetResult(uint8_t& outProgressPercent,
                                       UciMove& outBestMoveUci,
                                       uint32_t& outElapsedMs,
                                       uint32_t& outNodeCount) {
    if (!m_done.load(std::memory_order_acquire)) {
        outProgressPercent = 0;
        outBestMoveUci.Clear();
        outElapsedMs = 0;
        outNodeCount = 0;
        return ThinkState::Thinking;
    }

#ifndef ARDUINO
    if (m_worker.joinable()) {
        m_worker.join();
    }
#endif

    outProgressPercent = m_progress;
    outBestMoveUci = m_best;
    outElapsedMs = m_elapsedMs;
    outNodeCount = m_nodeCount;
    return m_state;
}

void AsyncThinkWorker::StoreResult(const ThinkState state,
                                   const uint8_t progressPercent,
                                   const UciMove& bestMoveUci,
                                   const uint32_t calculationMs,
                                   const uint32_t nodeCount) {
    m_state = state;
    m_progress = progressPercent;
    m_best = bestMoveUci;
    m_elapsedMs = calculationMs;
    m_nodeCount = nodeCount;
    if (m_continuousProgress) {
        m_liveNodes.store(nodeCount, std::memory_order_release);
    }

    m_diagnostics.state = state;
    m_diagnostics.finalAtMs = fxe::Millis();
    m_diagnostics.finalValid = true;
    m_diagnostics.finalNodes = nodeCount;
    PublishDiagnostics();
    m_done.store(true, std::memory_order_release);
}

#ifdef ARDUINO
void AsyncThinkWorker::TaskEntry(void* ctx) {
    auto* self = static_cast<AsyncThinkWorker*>(ctx);
    if (self == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        // A waiting worker owns no engine lane or watchdog subscription.
        // Only actual work/progress feeds the watchdog during a search.
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        runtime::WatchCurrentTask(1U, runtime::ProgressStage::Engine);
        self->Run();
        runtime::UnwatchCurrentTask(1U);
        if (logger::GetMinLevel() <= logger::Level::TRACE) {
            const auto minFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
            logger::Trace("cpu_think min_free_stack_bytes=%u through=search_cleanup",
                          static_cast<unsigned>(minFreeBytes));
        }
        // Result publication alone cannot admit a restart. Retain ownership
        // through watchdog cleanup and any synchronous diagnostic sink.
        self->RecordQuiescent();
        self->m_active.store(false, std::memory_order_release);
    }
}

void AsyncThinkWorker::Run() {
    if (m_engine == nullptr) {
        StoreResult(ThinkState::Error, 0, UciMove(), 0U, 0);
        return;
    }

    RecordPollStarted();
    fxe::ElapsedTime elapsed;
    elapsed.Reset();
    uint8_t progress = 0;
    UciMove best;
    const ThinkState state = m_engine->PollThinkProgress(progress, best);
    if (!m_continuousProgress) {
        m_liveNodes.store(0U, std::memory_order_release);
    }
    ThinkStats stats;
    uint32_t nodeCount = 0;
    m_diagnostics.finalNodesValid = m_engine->GetLastThinkStats(stats);
    if (m_diagnostics.finalNodesValid) nodeCount = stats.nodeCount;

    m_diagnostics.stopReason = stats.stopReason;
    StoreResult(state, progress, best,
        stats.calculationMsValid ? stats.calculationMs : elapsed.ElapsedMs(), nodeCount);
}
#else
void AsyncThinkWorker::Run(IEngine& engine) {
    RecordPollStarted();
    fxe::ElapsedTime elapsed;
    elapsed.Reset();
    uint8_t progress = 0;
    UciMove best;
    const ThinkState state = engine.PollThinkProgress(progress, best);
    if (!m_continuousProgress) {
        m_liveNodes.store(0U, std::memory_order_release);
    }
    ThinkStats stats;
    uint32_t nodeCount = 0;
    m_diagnostics.finalNodesValid = engine.GetLastThinkStats(stats);
    if (m_diagnostics.finalNodesValid) nodeCount = stats.nodeCount;

    m_diagnostics.stopReason = stats.stopReason;
    StoreResult(state, progress, best,
        stats.calculationMsValid ? stats.calculationMs : elapsed.ElapsedMs(), nodeCount);
}
#endif

} // namespace modules
