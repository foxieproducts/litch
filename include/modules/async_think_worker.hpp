#pragma once

#include <array>
#include <atomic>
#include <type_traits>
#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/engine.hpp"

#if defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#else
#include <thread>
#endif

namespace modules {

// Arduino's production owner is the firmware-lifetime FirmwareSystem. Its
// task stack/control block remain resident and must outlive the RTOS task.
class AsyncThinkWorker {
  public:
    AsyncThinkWorker() {
        for (auto& word : m_diagnosticWords) std::atomic_init(&word, uint32_t{0});
    }
    ~AsyncThinkWorker();

    bool Start(IEngine& engine);

    // Coordinator-only at quiescence, before preflight and mutable admission.
    void PrepareDiagnostics(uint32_t requestId, ThinkRequestKind kind,
                            const ThinkConfig& config, SideToMove side,
                            bool isDuck, uint8_t duckSquare);
    void RecordPreflight();
    // Native synchronous adapter records its existing Poll/statistics seam.
    void RecordSynchronousPollStarted();
    void RecordSynchronousResult(ThinkState state, uint32_t nodes, bool nodesValid);
    void RecordStartFailure();
    // Bounded coherent copy: contention returns false, never spins on a worker.
    bool TryGetDiagnostics(ThinkDiagnostics& out) const;

    // Configure each search after quiescence, before StartThink/Start. CPU
    // observations retain the existing250ms cadence and clear after Poll.
    // Continuous hints publish the latest observation and retain terminal nodes.
    // Reading either mailbox never enters the engine lane.
    ThinkProgressObserver PrepareProgressObserver(bool continuousHint = false);
    ThinkCandidateObserver PrepareCandidateObserver();
    // Copies one coherent compound candidate without entering the engine lane.
    bool TryGetPublishedCandidate(ThinkCandidate& outCandidate) const;
    uint32_t PublishedNodeCount() const {
        return m_liveNodes.load(std::memory_order_acquire);
    }

    // Requests cancellation without stalling the firmware loop. Returns true
    // only after PollThinkProgress and the worker's shared-lane cleanup have
    // both finished. Native callers also reap the completed std::thread.
    bool CancelAndJoin(IEngine& engine);

    bool IsActive() const;
    bool IsDone() const;

    ThinkState GetResult(uint8_t& outProgressPercent,
                         UciMove& outBestMoveUci,
                         uint32_t& outElapsedMs,
                         uint32_t& outNodeCount);

  private:
#if defined(LITCH_NATIVE)
    friend struct DiagnosticSnapshotTestAccess;
#endif
    static void PublishProgress(void* context, uint32_t nodes);
    static void RecordCompletedRoot(void* context, const ThinkCandidate& candidate);
    static void RecordEffectiveProfile(void* context, const ThinkProfile& profile);
    void PublishDiagnostics();
    void RecordPollStarted();
    void RecordQuiescent();
    // Each word is atomic, including padding, so a rejected seqlock copy has
    // no C++ data race. One writer owns the object: coordinator at quiescence,
    // worker during search and cleanup. Readers make at most two attempts.
    static_assert(std::is_trivially_copyable<ThinkDiagnostics>::value);
    static_assert(sizeof(ThinkDiagnostics) % sizeof(uint32_t) == 0U);
    static_assert(offsetof(ThinkCandidate, reservedBytes) + 2U == sizeof(ThinkCandidate));
    static_assert(offsetof(ThinkDiagnostics, stopReason) + 1U == sizeof(ThinkDiagnostics));
    static_assert(sizeof(ThinkDiagnostics) <= 256U);
    ThinkDiagnostics m_diagnostics{};
    std::array<std::atomic<uint32_t>, sizeof(ThinkDiagnostics) / sizeof(uint32_t)> m_diagnosticWords{};
    std::atomic<uint32_t> m_diagnosticRevision{0U};
    static void PublishCandidate(void* context, const ThinkCandidate& candidate);
    std::atomic<uint32_t> m_liveCandidate{0U};
    std::atomic<uint32_t> m_liveNodes{0U};
    // Written only by the coordinator at quiescence; immutable while active.
    bool m_continuousProgress = false;
    uint32_t m_lastProgressAtMs = 0U;

    void StoreResult(ThinkState state,
                     uint8_t progressPercent,
                     const UciMove& bestMoveUci,
                     uint32_t calculationMs,
                     uint32_t nodeCount);

#ifdef ARDUINO
    static void TaskEntry(void* ctx);
    void Run();

    static constexpr uint32_t THINK_TASK_STACK_BYTES = 8192U;
    static_assert(THINK_TASK_STACK_BYTES % sizeof(StackType_t) == 0U);
    alignas(16) StackType_t m_taskStack[THINK_TASK_STACK_BYTES / sizeof(StackType_t)]{};
    StaticTask_t m_taskControlBlock{};
    IEngine* m_engine = nullptr;
    void* m_task = nullptr;
#else
    void Run(IEngine& engine);
    std::thread m_worker;
#endif

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_done{false};
    ThinkState m_state = ThinkState::Idle;
    uint8_t m_progress = 0;
    UciMove m_best;
    uint32_t m_elapsedMs = 0U;
    uint32_t m_nodeCount = 0;
};

} // namespace modules
