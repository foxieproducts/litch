#include "runtime/progress_watchdog.hpp"
#include "foxie_embedded/millis.hpp"
#include <atomic>
#include <cstddef>
#include <cstring>
#ifndef ARDUINO
#include <mutex>
#endif
#ifdef ARDUINO
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

namespace runtime {
namespace {
// RTC and SDK globals are a platform boundary. Project bookkeeping uses no
// heap or flash; SDK task subscriptions allocate one bounded TWDT node.
#ifdef ARDUINO
RTC_NOINIT_ATTR RetainedProgress retained;
#if defined(LITCH_ESP32S3)
// Hash runtime state in fast DRAM and persist only changed words to slow RTC.
// The task mutex serializes both copies while RMT interrupts remain enabled.
RetainedProgress currentProgress{};
static_assert(sizeof(RetainedProgress) % sizeof(uint32_t) == 0U);
static_assert(offsetof(RetainedProgress, checksum) == sizeof(uint32_t));
void PublishRetainedChanges(const RetainedProgress& before) {
    const auto* oldBytes = reinterpret_cast<const unsigned char*>(&before);
    const auto* newBytes = reinterpret_cast<const unsigned char*>(&currentProgress);
    auto* rtcBytes = reinterpret_cast<unsigned char*>(&retained);
    bool changed = false;
    for (size_t offset = 0U; offset < sizeof(RetainedProgress); offset += sizeof(uint32_t)) {
        if (offset == offsetof(RetainedProgress, checksum)) continue;
        uint32_t oldWord = 0U;
        uint32_t newWord = 0U;
        std::memcpy(&oldWord, oldBytes + offset, sizeof(oldWord));
        std::memcpy(&newWord, newBytes + offset, sizeof(newWord));
        if (oldWord != newWord) {
            std::memcpy(rtcBytes + offset, &newWord, sizeof(newWord));
            changed = true;
        }
    }
    if (changed || before.checksum != currentProgress.checksum) {
        std::atomic_thread_fence(std::memory_order_release);
        retained.checksum = currentProgress.checksum;
    }
}
#endif
#if defined(LITCH_ESP32S3)
constexpr uint32_t PROGRESS_LOCK_TIMEOUT_MS = 10U;
StaticSemaphore_t g_progressMutexStorage;
SemaphoreHandle_t g_progressMutex = nullptr;
struct Lock {
    bool acquired = false;
    Lock() {
        // All callers run in task context. Keep bookkeeping serialized while
        // allowing RMT refill interrupts and other ready tasks to run.
        acquired = g_progressMutex != nullptr &&
            xSemaphoreTake(g_progressMutex, pdMS_TO_TICKS(PROGRESS_LOCK_TIMEOUT_MS)) == pdTRUE;
    }
    bool IsAcquired() const { return acquired; }
    ~Lock() {
        if (!acquired) return;
        (void)xSemaphoreGive(g_progressMutex);
    }
};
#else
portMUX_TYPE progressMux = portMUX_INITIALIZER_UNLOCKED;
struct Lock {
    Lock() { portENTER_CRITICAL(&progressMux); }
    ~Lock() { portEXIT_CRITICAL(&progressMux); }
    bool IsAcquired() const { return true; }
};
#endif
#else
RetainedProgress retained{};
std::mutex progressMutex;
struct Lock {
    std::lock_guard<std::mutex> guard{progressMutex};
    bool IsAcquired() const { return true; }
};
#endif
}
void StartProgressWatchdog() {
#ifdef ARDUINO
    const uint32_t resetReason = static_cast<uint32_t>(esp_reset_reason());
#if defined(LITCH_ESP32S3)
    if (g_progressMutex == nullptr) {
        g_progressMutex = xSemaphoreCreateMutexStatic(&g_progressMutexStorage);
    }
    ESP_ERROR_CHECK(g_progressMutex != nullptr ? ESP_OK : ESP_ERR_NO_MEM);
#endif
    {
        Lock lock;
        ESP_ERROR_CHECK(lock.IsAcquired() ? ESP_OK : ESP_ERR_TIMEOUT);
#if defined(LITCH_ESP32S3)
        currentProgress = retained;
        BeginProgressBoot(currentProgress, resetReason);
        // Boot initialization precedes the renderer; later publications write
        // only changed RTC words.
        retained = currentProgress;
#else
        BeginProgressBoot(retained, resetReason);
#endif
    }
    // IDF 4.4 reconfigures an already initialized TWDT without removing idle
    // subscriptions. Panic is required: logging a stuck task is not recovery.
    ESP_ERROR_CHECK(esp_task_wdt_init(WATCHDOG_TIMEOUT_SECONDS, true));
#else
    BeginProgressBoot(retained, 1U);
#endif
    WatchCurrentTask(0U, ProgressStage::Boot);
}
void WatchCurrentTask(const uint8_t lane, const ProgressStage stage) {
#ifdef ARDUINO
    if (esp_task_wdt_status(nullptr) != ESP_OK) {
        ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    }
#endif
    NoteProgress(lane, stage);
}
void UnwatchCurrentTask(const uint8_t lane) {
    NoteProgress(lane, ProgressStage::Idle);
#ifdef ARDUINO
    ESP_ERROR_CHECK(esp_task_wdt_delete(nullptr));
#endif
}
void FeedProgressWatchdog() {
#ifdef ARDUINO
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        (void)esp_task_wdt_reset();
    }
#endif
}
void DelayDiagnosticSlice(const uint32_t milliseconds) {
#ifdef ARDUINO
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    vTaskDelay(ticks == 0U ? 1U : ticks);
#else
    (void)milliseconds;
#endif
}
void NoteProgress(const uint8_t lane, const ProgressStage stage) {
    const uint32_t nowMs = fxe::Millis();
    Lock lock;
    if (!lock.IsAcquired()) return;
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    const auto before = currentProgress;
    RecordProgress(currentProgress, lane, stage, nowMs);
    PublishRetainedChanges(before);
#else
    RecordProgress(retained, lane, stage, nowMs);
#endif
}
void UpdateIncidentContext(const IncidentContext& context) {
    Lock lock;
    if (!lock.IsAcquired()) return;
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    const auto before = currentProgress;
    RecordIncidentContext(currentProgress, context);
    PublishRetainedChanges(before);
#else
    RecordIncidentContext(retained, context);
#endif
}
ProgressSnapshot GetProgressSnapshot() {
    Lock lock;
    if (!lock.IsAcquired()) return {};
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    return SnapshotProgress(currentProgress);
#else
    return SnapshotProgress(retained);
#endif
}
}
