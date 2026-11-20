#include "firmware/ota_platform_adapter.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hardware/rmt_flash_arbiter.hpp"
#include "firmware/ota_runtime_evidence_internal.hpp"
#include "modules/firmware_digest_provider.hpp"
#include "modules/ota_image_identity_esp32.hpp"
#include "modules/ota_slot_policy.hpp"
#include "runtime/progress_watchdog.hpp"

#if defined(ARDUINO) && defined(LITCH_ESP32S3)
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

namespace firmware {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
namespace {

constexpr uint32_t FLASH_ACQUIRE_TIMEOUT_MS = 5000U;
constexpr uint32_t BOOT_REQUEST_TIMEOUT_MS = 30000U;
constexpr uint32_t BOOT_TASK_STACK_BYTES = 4096U;
// Metadata preflight runs before rail enable; preserve its original priority.
// Full integrity jobs start after rail admission and outrank optional work.
constexpr UBaseType_t BOOT_TASK_PRIORITY = 2U;
constexpr UBaseType_t INTEGRITY_TASK_PRIORITY = 3U;
constexpr BaseType_t BOOT_TASK_CORE = 0;

bool AcquireFlash(hardware::RmtFlashArbiter& arbiter) {
    const TickType_t started = xTaskGetTickCount();
    arbiter.RequestFlash();
    while (!arbiter.TryBeginRequestedFlash()) {
        if (static_cast<TickType_t>(xTaskGetTickCount() - started) >=
            pdMS_TO_TICKS(FLASH_ACQUIRE_TIMEOUT_MS)) {
            arbiter.CancelFlashRequest();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    return true;
}

const esp_partition_t* FindSlot(const modules::OtaPostBootSlotIdentity& slot) {
    const auto* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(slot.subtype), nullptr);
    return partition != nullptr && partition->type == ESP_PARTITION_TYPE_APP &&
                   partition->address == slot.address && partition->subtype == slot.subtype
               ? partition : nullptr;
}

class OtaPostBootWorker final {
  public:
    bool Begin(hardware::RmtFlashArbiter& arbiter) {
        return EnsureWorker(arbiter) && m_requestMutex != nullptr && m_completion != nullptr;
    }

    bool QueueDigest(modules::FirmwareDigestProvider& provider) {
        auto* arbiter = provider.FlashArbiter();
        if (arbiter == nullptr || !EnsureWorker(*arbiter)) return false;
        modules::FirmwareDigestProvider* empty = nullptr;
        if (!m_pendingDigest.compare_exchange_strong(empty, &provider, std::memory_order_acq_rel)) return false;
        xTaskNotifyGive(m_task);
        return true;
    }

    bool ReadRunningState(modules::OtaPostBootRunningState& outState) {
        Response response;
        if (!Submit({RequestKind::ReadRunningState, modules::OtaPostBootHealth::NotReported}, response) ||
            !response.succeeded) return false;
        outState = response.runningState;
        return true;
    }

    modules::OtaPostBootExecutionResult ProcessHealth(
        const modules::OtaPostBootHealth health) {
        Response response;
        return Submit({RequestKind::ProcessHealth, health}, response)
                   ? response.result
                   : modules::OtaPostBootExecutionResult::SnapshotUnavailable;
    }

    bool VerifyUpdateSource(modules::OtaPostBootSnapshot& out) {
        Response response;
        if (!Submit({RequestKind::VerifyUpdateSource, modules::OtaPostBootHealth::NotReported}, response) ||
            !response.succeeded) return false;
        out = response.snapshot;
        return true;
    }

    OtaBootHealthRequestStatus BeginHealth(
        const modules::OtaPostBootHealth health, OtaBootHealthTicket& ticket) {
        ticket = 0U;
        if (!Available())
            return OtaBootHealthRequestStatus::Unavailable;
        if (xSemaphoreTake(m_requestMutex, 0U) != pdTRUE)
            return OtaBootHealthRequestStatus::Busy;
        if (!PrepareRequest()) {
            xSemaphoreGive(m_requestMutex);
            return OtaBootHealthRequestStatus::Busy;
        }
        if (m_nextHealthTicket == UINT32_MAX) {
            xSemaphoreGive(m_requestMutex);
            return OtaBootHealthRequestStatus::Unavailable;
        }
        m_request = {health == modules::OtaPostBootHealth::NotReported
                         ? RequestKind::PrepareHealth : RequestKind::ProcessHealth, health};
        m_healthDecision.store(health, std::memory_order_release);
        m_healthImageSha256.fill(0U);
        m_executionResult = modules::OtaPostBootExecutionResult::SnapshotUnavailable;
        m_healthTicket = ++m_nextHealthTicket;
        m_busy = true;
        ticket = m_healthTicket;
        NotifyRequest();
        xSemaphoreGive(m_requestMutex);
        return OtaBootHealthRequestStatus::Pending;
    }

    OtaBootHealthRequestStatus ResolveHealth(
        const OtaBootHealthTicket ticket, const modules::OtaPostBootHealth health) {
        if (!Available() || health == modules::OtaPostBootHealth::NotReported)
            return OtaBootHealthRequestStatus::Unavailable;
        if (xSemaphoreTake(m_requestMutex, 0U) != pdTRUE)
            return OtaBootHealthRequestStatus::Busy;
        const bool matches = ticket != 0U && m_healthTicket == ticket &&
            m_request.kind == RequestKind::PrepareHealth;
        if (matches) {
            m_healthDecision.store(health, std::memory_order_release);
            m_interruptDigest.store(true, std::memory_order_release);
            xTaskNotifyGive(m_task);
        }
        xSemaphoreGive(m_requestMutex);
        return matches ? OtaBootHealthRequestStatus::Pending : OtaBootHealthRequestStatus::Stale;
    }

    OtaBootHealthRequestStatus PollHealth(
        const OtaBootHealthTicket ticket, modules::OtaPostBootExecutionResult& out,
        std::array<uint8_t, modules::OTA_POST_BOOT_SHA256_BYTES>& outImageSha256) {
        if (!Available()) return OtaBootHealthRequestStatus::Unavailable;
        if (ticket == 0U) return OtaBootHealthRequestStatus::Stale;
        if (xSemaphoreTake(m_requestMutex, 0U) != pdTRUE)
            return OtaBootHealthRequestStatus::Busy;
        if (m_healthTicket != ticket) {
            xSemaphoreGive(m_requestMutex);
            return OtaBootHealthRequestStatus::Stale;
        }
        if (xSemaphoreTake(m_completion, 0U) != pdTRUE) {
            xSemaphoreGive(m_requestMutex);
            return OtaBootHealthRequestStatus::Pending;
        }
        out = m_executionResult;
        outImageSha256.fill(0U);
        if (m_healthDecision.load(std::memory_order_acquire) == modules::OtaPostBootHealth::Passed &&
            m_executionResult == modules::OtaPostBootExecutionResult::Confirmed) {
            outImageSha256 = m_healthImageSha256;
        }
        m_healthTicket = 0U;
        m_busy = false;
        xSemaphoreGive(m_requestMutex);
        return OtaBootHealthRequestStatus::Complete;
    }

  private:
    bool EnsureWorker(hardware::RmtFlashArbiter& arbiter) {
        if (m_started.load(std::memory_order_acquire)) return m_arbiter == &arbiter;
        if (m_starting.test_and_set(std::memory_order_acq_rel)) {
            return m_started.load(std::memory_order_acquire) && m_arbiter == &arbiter;
        }
        m_requestMutex = xSemaphoreCreateMutexStatic(&m_requestMutexStorage);
        m_completion = xSemaphoreCreateBinaryStatic(&m_completionStorage);
        // A recovery digest needs only the task and its flash arbiter. Missing
        // OTA metadata or request semaphores cannot disable that capability.
        m_arbiter = &arbiter;
        m_task = xTaskCreateStaticPinnedToCore(
            &TaskEntry, "ota_boot", BOOT_TASK_STACK_BYTES, this, BOOT_TASK_PRIORITY,
            m_taskStack, &m_taskControl, BOOT_TASK_CORE);
        if (m_task == nullptr) {
            m_arbiter = nullptr;
            m_starting.clear(std::memory_order_release);
            return false;
        }
        m_started.store(true, std::memory_order_release);
        return true;
    }

    enum class RequestKind : uint8_t { ReadRunningState, ProcessHealth, VerifyUpdateSource, PrepareHealth };
    struct Request {
        RequestKind kind = RequestKind::ReadRunningState;
        modules::OtaPostBootHealth health = modules::OtaPostBootHealth::NotReported;
    };

    struct Response {
        modules::OtaPostBootRunningState runningState = modules::OtaPostBootRunningState::Unknown;
        modules::OtaPostBootExecutionResult result = modules::OtaPostBootExecutionResult::SnapshotUnavailable;
        bool succeeded = false;
        modules::OtaPostBootSnapshot snapshot;
    };

    static void TaskEntry(void* context) {
        static_cast<OtaPostBootWorker*>(context)->Run();
    }

    bool Available() const {
        return m_started.load(std::memory_order_acquire) && m_task != nullptr &&
            m_arbiter != nullptr && m_requestMutex != nullptr && m_completion != nullptr;
    }

    // The mutex spans every live synchronous caller. Only a prior timed-out
    // synchronous result may be reaped here; a retained health owner must poll
    // its exact ticket even after the worker has posted completion.
    bool PrepareRequest() {
        if (m_healthTicket != 0U) return false;
        if (m_busy && xSemaphoreTake(m_completion, 0U) != pdTRUE) return false;
        m_busy = false;
        (void)xSemaphoreTake(m_completion, 0U);
        return true;
    }

    bool Submit(const Request& request, Response& out) {
        runtime::NoteProgress(0U, runtime::ProgressStage::Ota);
        if (!Available() || xSemaphoreTake(m_requestMutex, 0U) != pdTRUE) return false;
        if (!PrepareRequest()) {
            xSemaphoreGive(m_requestMutex);
            return false;
        }
        runtime::FeedProgressWatchdog();
        m_request = request;
        m_boolResult = false;
        m_executionResult = modules::OtaPostBootExecutionResult::SnapshotUnavailable;
        m_busy = true;
        NotifyRequest();
        const bool completed = xSemaphoreTake(
            m_completion, pdMS_TO_TICKS(BOOT_REQUEST_TIMEOUT_MS)) == pdTRUE;
        if (completed) {
            runtime::FeedProgressWatchdog();
            m_busy = false;
            // Copy the completed response before releasing request ownership;
            // another caller must not replace this request's result.
            out = {m_outputRunningState, m_executionResult, m_boolResult, m_outputSnapshot};
        }
        xSemaphoreGive(m_requestMutex);
        return completed;
    }

    static OtaPostBootWorker& Self(void* context) {
        return *static_cast<OtaPostBootWorker*>(context);
    }
    static bool Acquire(void* context) {
        auto& self = Self(context);
        self.m_publishSnapshot = false;
        if (AcquireFlash(*self.m_arbiter)) return true;
        ota_evidence_internal::InvalidateSnapshot();
        return false;
    }
    static void Release(void* context) {
        auto& self = Self(context);
        self.m_arbiter->EndFlash();
        if (self.m_publishSnapshot) {
            ota_evidence_internal::RecordSnapshot(
                self.m_snapshotAvailable ? &self.m_observedSnapshot : nullptr,
                self.m_observedStamp);
            self.m_publishSnapshot = false;
        }
    }
    static bool ReadSnapshot(void* context, modules::OtaPostBootSnapshot& out) {
        auto& self = Self(context);
        self.m_publishSnapshot = true;
        self.m_snapshotAvailable = ReadEsp32OtaBootSnapshotWithFlashLease(out);
        self.m_observedStamp = ota_evidence_internal::CaptureStamp();
        if (self.m_snapshotAvailable) self.m_observedSnapshot = out;
        return self.m_snapshotAvailable;
    }
    static bool ReadImage(void* context,
                          const modules::OtaPostBootSlotIdentity& slot,
                          modules::OtaPostBootImageEvidence& out) {
        auto& self = Self(context);
        // CaptureSnapshot has already initialized the immutable SDK partition
        // list. Looking up this owned slot adds no flash lease or I/O wait.
        const auto* partition = FindSlot(slot);
        modules::OtaImageIdentity identity;
        if (partition == nullptr ||
            modules::ReadEsp32ImageIdentity(partition, *self.m_arbiter, identity) != ESP_OK) {
            return false;
        }
        out.slot = slot;
        out.buildId = identity.buildId;
        out.buildIdLength = identity.buildIdLength;
        out.imageSha256 = identity.imageSha256;
        if (self.m_request.kind == RequestKind::PrepareHealth ||
            self.m_request.health == modules::OtaPostBootHealth::Passed) {
            self.m_healthImageSha256 = identity.imageSha256;
        }
        return true;
    }
    static void InvalidateObservation(void* context) {
        auto& self = Self(context);
        self.m_publishSnapshot = true;
        self.m_snapshotAvailable = false;
        self.m_observedStamp = ota_evidence_internal::CaptureStamp();
    }
    static bool MarkValid(void* context) {
        InvalidateObservation(context);
        return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
    }
    static bool MarkInvalidAndReboot(void* context) {
        InvalidateObservation(context);
        return esp_ota_mark_app_invalid_rollback_and_reboot() == ESP_OK;
    }

    void NotifyRequest() {
        m_requestQueued.store(true, std::memory_order_release);
        m_interruptDigest.store(true, std::memory_order_release);
        xTaskNotifyGive(m_task);
    }

    void ServiceDigest(const bool awaitingHealth = false) {
        auto* provider = m_pendingDigest.load(std::memory_order_acquire);
        if (provider == nullptr) return;
        // Clear the interruption first, then check the authoritative job state.
        // A later request sets it again and stops the hash at its next flash read.
        m_interruptDigest.store(false, std::memory_order_release);
        const bool urgent = m_requestQueued.load(std::memory_order_acquire) ||
            (awaitingHealth && m_healthDecision.load(std::memory_order_acquire) != modules::OtaPostBootHealth::NotReported);
        if (urgent && !modules::Esp32ImageReadWindow{provider->StandaloneStartedMs()}.Expired()) return;
        uint8_t digest[modules::FirmwareDigestProvider::DIGEST_BYTES]{};
        const esp_err_t result = modules::ReadEsp32FirmwareDigest(*provider, digest, &m_interruptDigest);
        if (result == ESP_ERR_NOT_FINISHED && m_interruptDigest.load(std::memory_order_acquire)) return;
        m_pendingDigest.store(nullptr, std::memory_order_release);
        if (result == ESP_OK) provider->CompleteStandalone(digest, modules::Esp32ImageReadWindow::TIMEOUT_MS);
        else provider->Fail();
    }

    [[noreturn]] void Run() {
        while (true) {
            ServiceDigest();
            if (!m_requestQueued.exchange(false, std::memory_order_acq_rel)) {
                (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
            const bool integrityJob = m_request.kind == RequestKind::PrepareHealth ||
                m_request.kind == RequestKind::VerifyUpdateSource ||
                (m_request.kind == RequestKind::ProcessHealth &&
                 m_request.health != modules::OtaPostBootHealth::NotReported);
            if (integrityJob) vTaskPrioritySet(nullptr, INTEGRITY_TASK_PRIORITY);
            runtime::WatchCurrentTask(3U, runtime::ProgressStage::Ota);
            ota_evidence_internal::InvalidateSnapshot();
            if (m_request.kind == RequestKind::PrepareHealth) {
                const modules::OtaPostBootExecutionOps ops{
                    this, &Acquire, &Release, &ReadSnapshot, &ReadImage,
                    &MarkValid, &MarkInvalidAndReboot};
                modules::OtaPostBootPreparedImage prepared;
                m_executionResult = modules::PrepareOtaPostBootRunningImage(ops, prepared);
                if (m_executionResult == modules::OtaPostBootExecutionResult::AwaitingHealth) {
                    // Retain the proof and ticket, with no flash lease or mutex,
                    // while the live runtime observes startup and transport.
                    runtime::UnwatchCurrentTask(3U);
                    vTaskPrioritySet(nullptr, BOOT_TASK_PRIORITY);
                    while (m_healthDecision.load(std::memory_order_acquire) ==
                           modules::OtaPostBootHealth::NotReported) {
                        ServiceDigest(true);
                        if (m_healthDecision.load(std::memory_order_acquire) ==
                            modules::OtaPostBootHealth::NotReported)
                            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                    }
                    vTaskPrioritySet(nullptr, INTEGRITY_TASK_PRIORITY);
                    runtime::WatchCurrentTask(3U, runtime::ProgressStage::Ota);
                    const auto health = m_healthDecision.load(std::memory_order_acquire);
                    m_executionResult = health == modules::OtaPostBootHealth::Passed
                        ? modules::FinishOtaPostBootRunningImage(ops, prepared, health)
                        : modules::ExecuteOtaPostBootVerification(ops, health);
                }
            } else if (m_request.kind == RequestKind::ProcessHealth) {
                const modules::OtaPostBootExecutionOps ops{
                    this, &Acquire, &Release, &ReadSnapshot, &ReadImage,
                    &MarkValid, &MarkInvalidAndReboot};
                m_executionResult = modules::ExecuteOtaPostBootVerification(ops, m_request.health);
            } else if (m_request.kind == RequestKind::VerifyUpdateSource) {
                m_boolResult = ReadEsp32OtaVerifiedSourceOnWorker(*m_arbiter, m_outputSnapshot);
                if (!m_boolResult) ota_evidence_internal::InvalidateSnapshot();
            } else if (Acquire(this)) {
                modules::OtaPostBootSnapshot snapshot;
                m_outputRunningState = modules::OtaPostBootRunningState::Unknown;
                m_boolResult = ReadSnapshot(this, snapshot);
                if (m_boolResult) {
                    m_outputRunningState = modules::EvaluateOtaPostBootVerification(
                        snapshot, modules::OtaPostBootHealth::NotReported).runningState;
                    m_boolResult = m_outputRunningState != modules::OtaPostBootRunningState::Unknown;
                }
                Release(this);
            }
            runtime::UnwatchCurrentTask(3U);
            if (integrityJob) vTaskPrioritySet(nullptr, BOOT_TASK_PRIORITY);
            xSemaphoreGive(m_completion);
        }
    }

    hardware::RmtFlashArbiter* m_arbiter = nullptr;
    static_assert(BOOT_TASK_STACK_BYTES % sizeof(StackType_t) == 0U);
    alignas(16) StackType_t m_taskStack[BOOT_TASK_STACK_BYTES / sizeof(StackType_t)]{};
    StaticTask_t m_taskControl{};
    TaskHandle_t m_task = nullptr;
    std::atomic<modules::FirmwareDigestProvider*> m_pendingDigest{nullptr};
    std::atomic<bool> m_requestQueued{false};
    std::atomic<bool> m_interruptDigest{false};
    StaticSemaphore_t m_requestMutexStorage{};
    SemaphoreHandle_t m_requestMutex = nullptr;
    StaticSemaphore_t m_completionStorage{};
    SemaphoreHandle_t m_completion = nullptr;
    std::atomic_flag m_starting = ATOMIC_FLAG_INIT;
    std::atomic<bool> m_started{false};
    bool m_busy = false;
    OtaBootHealthTicket m_healthTicket = 0U;
    OtaBootHealthTicket m_nextHealthTicket = 0U;
    Request m_request{};
    std::atomic<modules::OtaPostBootHealth> m_healthDecision{modules::OtaPostBootHealth::NotReported};
    modules::OtaPostBootRunningState m_outputRunningState = modules::OtaPostBootRunningState::Unknown;
    modules::OtaPostBootExecutionResult m_executionResult = modules::OtaPostBootExecutionResult::SnapshotUnavailable;
    bool m_boolResult = false;
    modules::OtaPostBootSnapshot m_outputSnapshot{};
    modules::OtaPostBootSnapshot m_observedSnapshot{};
    std::array<uint8_t, modules::OTA_POST_BOOT_SHA256_BYTES> m_healthImageSha256{};
    ota_evidence_internal::SnapshotStamp m_observedStamp{};
    bool m_publishSnapshot = false;
    bool m_snapshotAvailable = false;
};

OtaPostBootWorker& Worker() {
    static OtaPostBootWorker worker;
    return worker;
}

} // namespace

bool InitializeEsp32OtaPostBootWorker(hardware::RmtFlashArbiter& arbiter) {
    return Worker().Begin(arbiter);
}

bool QueueEsp32FirmwareDigest(modules::FirmwareDigestProvider& provider) {
    return Worker().QueueDigest(provider);
}

OtaBootHealthRequestStatus BeginEsp32OtaBootHealth(
    const modules::OtaPostBootHealth health, OtaBootHealthTicket& ticket) {
    return Worker().BeginHealth(health, ticket);
}

OtaBootHealthRequestStatus ResolveEsp32OtaBootHealth(
    const OtaBootHealthTicket ticket, const modules::OtaPostBootHealth health) {
    return Worker().ResolveHealth(ticket, health);
}

OtaBootHealthRequestStatus PollEsp32OtaBootHealth(
    const OtaBootHealthTicket ticket, modules::OtaPostBootExecutionResult& out,
    std::array<uint8_t, modules::OTA_POST_BOOT_SHA256_BYTES>& outImageSha256) {
    return Worker().PollHealth(ticket, out, outImageSha256);
}

bool VerifyEsp32OtaUpdateSource(modules::OtaPostBootSnapshot& out) {
    return Worker().VerifyUpdateSource(out);
}

modules::OtaPostBootExecutionResult ProcessEsp32OtaBootHealth(const modules::OtaPostBootHealth health) {
    return Worker().ProcessHealth(health);
}

bool ReadEsp32OtaRunningState(modules::OtaPostBootRunningState& out) {
    return Worker().ReadRunningState(out);
}

#else
bool InitializeEsp32OtaPostBootWorker(hardware::RmtFlashArbiter&) { return false; }
bool QueueEsp32FirmwareDigest(modules::FirmwareDigestProvider&) { return false; }
bool ReadEsp32OtaRunningState(modules::OtaPostBootRunningState&) { return false; }
bool VerifyEsp32OtaUpdateSource(modules::OtaPostBootSnapshot&) { return false; }

OtaBootHealthRequestStatus BeginEsp32OtaBootHealth(modules::OtaPostBootHealth, OtaBootHealthTicket& ticket) {
    ticket = 0U;
    return OtaBootHealthRequestStatus::Unsupported;
}
OtaBootHealthRequestStatus ResolveEsp32OtaBootHealth(OtaBootHealthTicket, modules::OtaPostBootHealth) {
    return OtaBootHealthRequestStatus::Unsupported;
}
OtaBootHealthRequestStatus PollEsp32OtaBootHealth(OtaBootHealthTicket, modules::OtaPostBootExecutionResult&,
    std::array<uint8_t, modules::OTA_POST_BOOT_SHA256_BYTES>&) {
    return OtaBootHealthRequestStatus::Unsupported;
}
modules::OtaPostBootExecutionResult ProcessEsp32OtaBootHealth(modules::OtaPostBootHealth) {
    return modules::OtaPostBootExecutionResult::Unsupported;
}
#endif

} // namespace firmware
