#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "hardware/rmt_flash_arbiter.hpp"

namespace firmware {

enum class SettingsPhysicalAccessKind : uint8_t { Read, Write, Erase };

struct SettingsPhysicalAccessCounter {
    uint32_t calls = 0U;
    uint32_t bytes = 0U;
    uint32_t totalMs = 0U;
    uint32_t maxMs = 0U;

    void Record(const size_t requestedBytes, const uint32_t durationMs) {
        Add(calls, 1U);
        Add(bytes, requestedBytes);
        Add(totalMs, durationMs);
        if (durationMs > maxMs) maxMs = durationMs;
    }

  private:
    static void Add(uint32_t& value, const size_t amount) {
        const auto remaining = std::numeric_limits<uint32_t>::max() - value;
        value += static_cast<uint32_t>(amount > remaining ? remaining : amount);
    }
};

struct SettingsPhysicalAccessSample {
    bool available = false;
    std::array<SettingsPhysicalAccessCounter, 3U> access{};
};

struct SettingsPhysicalAccessMicroCounter {
    uint32_t calls = 0U;
    uint32_t bytes = 0U;
    uint32_t totalUs = 0U;
    uint32_t maxUs = 0U;

    static void Add(uint32_t& value, const uint64_t amount) {
        const auto remaining = std::numeric_limits<uint32_t>::max() - value;
        value += static_cast<uint32_t>(amount > remaining ? remaining : amount);
    }
    void Record(const size_t requestedBytes, const uint64_t durationUs) {
        Add(calls, 1U);
        Add(bytes, requestedBytes);
        Add(totalUs, durationUs);
        const auto bounded = static_cast<uint32_t>(std::min<uint64_t>(
            durationUs, std::numeric_limits<uint32_t>::max()));
        if (bounded > maxUs) maxUs = bounded;
    }
};

struct SettingsFlashTiming {
    uint32_t outerCalls = 0U;
    uint32_t outerWaitMs = 0U;
    uint32_t handoffs = 0U;
    uint32_t handoffWaitMs = 0U;
    std::array<SettingsPhysicalAccessMicroCounter, 3U> physical{};
};

struct SettingsTransactionFlashSample {
    bool available = false;
    SettingsFlashTiming work{};
    SettingsFlashTiming paging{};
};

// The settings worker owns this boundary. Binding is immutable after publication;
// only its task reads observation/sampling state and records SDK driver calls.
class SettingsPhysicalAccess final {
  public:
    void Bind(const void* task, const void* partition) {
        m_task = task;
        m_partition = partition;
    }

    void BindFlashArbiter(hardware::RmtFlashArbiter& arbiter) { m_flashArbiter = &arbiter; }
    bool HasFlashArbiter(const hardware::RmtFlashArbiter& arbiter) const {
        return m_flashArbiter == &arbiter;
    }
    hardware::RmtFlashArbiter* FlashArbiter() const { return m_flashArbiter; }

    // File observation eligibility. Hardware exclusion is acquired separately
    // at each real SDK primitive, never across a whole filesystem operation.
    void BeginFlashLease() { m_flashLeaseActive = true; }
    void EndFlashLease() { m_flashLeaseActive = false; }

    void RecordHardwareWait(const uint32_t waitMs) {
        if (!m_transactionActive) return;
        auto& timing = TransactionTiming();
        SettingsPhysicalAccessMicroCounter::Add(timing.handoffs, 1U);
        SettingsPhysicalAccessMicroCounter::Add(timing.handoffWaitMs, waitMs);
    }

    bool IsRecordingOperation() const { return m_samplingActive; }

    bool IsRecordingTransaction() const { return m_transactionActive; }
    void BeginTransaction() {
        m_transaction = {};
        m_transaction.available = m_task != nullptr && m_partition != nullptr;
        m_transactionPaging = false;
        m_transactionActive = true;
    }
    void EndTransaction() { m_transactionActive = false; }
    void SetTransactionPaging(const bool paging) { m_transactionPaging = paging; }
    void RecordOuterAccess(const uint32_t waitMs) {
        if (!m_transactionActive) return;
        auto& timing = TransactionTiming();
        SettingsPhysicalAccessMicroCounter::Add(timing.outerCalls, 1U);
        SettingsPhysicalAccessMicroCounter::Add(timing.outerWaitMs, waitMs);
    }
    void RecordTransaction(const SettingsPhysicalAccessKind kind,
                           const size_t bytes, const uint64_t durationUs) {
        if (m_transactionActive) {
            TransactionTiming().physical[static_cast<size_t>(kind)].Record(bytes, durationUs);
        }
    }
    SettingsTransactionFlashSample TransactionSample() const { return m_transaction; }

    void BeginOperation() {
        m_sample = {};
        m_sample.available = m_task != nullptr && m_partition != nullptr;
        m_samplingActive = true;
    }

    void EndOperation() { m_samplingActive = false; }

    bool Matches(const void* task, const void* partition) const {
        // Do not read worker-only observation state from another task.
        return task != nullptr && task == m_task && partition != nullptr &&
               partition == m_partition && m_flashLeaseActive;
    }

    void Record(const SettingsPhysicalAccessKind kind, const size_t bytes,
                const uint32_t durationMs) {
        m_sample.access[static_cast<size_t>(kind)].Record(bytes, durationMs);
    }

    SettingsPhysicalAccessSample Sample() const { return m_sample; }

  private:
    SettingsFlashTiming& TransactionTiming() {
        return m_transactionPaging ? m_transaction.paging : m_transaction.work;
    }
    hardware::RmtFlashArbiter* m_flashArbiter = nullptr;
    const void* m_task = nullptr;
    const void* m_partition = nullptr;
    bool m_flashLeaseActive = false;
    bool m_samplingActive = false;
    bool m_transactionActive = false;
    bool m_transactionPaging = false;
    SettingsPhysicalAccessSample m_sample{};
    SettingsTransactionFlashSample m_transaction{};
};

// One SDK-boundary registration, published after the worker and exact partition
// have been identified. A null registration leaves every SDK call untouched.
void RegisterSettingsPhysicalAccess(SettingsPhysicalAccess* observer);

} // namespace firmware
