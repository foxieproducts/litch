#if defined(ARDUINO) && defined(LITCH_ESP32S3)

#include <atomic>
#include <cerrno>

#include <esp_partition.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "firmware/settings_physical_access.hpp"
#include "foxie_embedded/millis.hpp"

namespace {

// SDK callback boundary only; the settings worker owns its observation and samples
// and outlives its task. Boot and other tasks never access its mutable state.
std::atomic<firmware::SettingsPhysicalAccess*> g_settingsObserver{nullptr};

template <typename Operation>
esp_err_t Observe(const esp_partition_t* partition,
                  const firmware::SettingsPhysicalAccessKind kind,
                  const size_t bytes, Operation operation) {
    const int beforeError = errno;
    auto* observer = g_settingsObserver.load(std::memory_order_acquire);
    if (observer != nullptr &&
        !observer->Matches(xTaskGetCurrentTaskHandle(), partition)) {
        observer = nullptr;
    }
    auto* arbiter = observer != nullptr ? observer->FlashArbiter() : nullptr;
    bool waited = false;
    uint32_t waitStartedMs = 0U;
    uint32_t waitMs = 0U;
    if (observer != nullptr) {
        // A missing binding must fail closed, never issue unprotected flash.
        if (arbiter == nullptr) { errno = EBUSY; return ESP_FAIL; }
        constexpr uint32_t FLASH_ACQUIRE_TIMEOUT_MS = 100U;
        const TickType_t started = xTaskGetTickCount();
        const TickType_t timeout = pdMS_TO_TICKS(FLASH_ACQUIRE_TIMEOUT_MS);
        arbiter->RequestFlash();
        while (!arbiter->TryBeginRequestedFlash()) {
            if (!waited) {
                waited = true;
                waitStartedMs = fxe::Millis();
            }
            if (static_cast<TickType_t>(xTaskGetTickCount() - started) >= timeout) {
                // This caller has not consumed its request or acquired flash.
                arbiter->CancelFlashRequest();
                errno = ETIMEDOUT;
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(1U);
        }
        if (waited) waitMs = fxe::Millis() - waitStartedMs;
    }
    const bool transaction = observer != nullptr && observer->IsRecordingTransaction();
    const bool recording = observer != nullptr && observer->IsRecordingOperation();
    const uint32_t startedMs = recording ? fxe::Millis() : 0U;
    const int64_t startedUs = transaction ? esp_timer_get_time() : 0;
    errno = beforeError;
    const esp_err_t result = operation();
    const int operationError = errno;
    // The primitive is now safe. Never wait for a fresh LED frame here.
    if (arbiter != nullptr) arbiter->EndFlash();
    if (waited) observer->RecordHardwareWait(waitMs);
    const uint64_t durationUs = transaction
        ? static_cast<uint64_t>(esp_timer_get_time() - startedUs) : 0U;
    if (transaction) {
        observer->RecordTransaction(kind, bytes, durationUs);
    }
    if (recording) {
        observer->Record(kind, bytes, fxe::Millis() - startedMs);
    }
    errno = operationError;
    return result;
}

} // namespace

namespace firmware {

void RegisterSettingsPhysicalAccess(SettingsPhysicalAccess* observer) {
    g_settingsObserver.store(observer, std::memory_order_release);
}

} // namespace firmware

extern "C" esp_err_t __real_esp_partition_read(
    const esp_partition_t*, size_t, void*, size_t);
extern "C" esp_err_t __real_esp_partition_write(
    const esp_partition_t*, size_t, const void*, size_t);
extern "C" esp_err_t __real_esp_partition_erase_range(
    const esp_partition_t*, size_t, size_t);

extern "C" esp_err_t __wrap_esp_partition_read(
    const esp_partition_t* partition, const size_t offset, void* out, const size_t bytes) {
    return Observe(partition, firmware::SettingsPhysicalAccessKind::Read, bytes,
                   [&] { return __real_esp_partition_read(partition, offset, out, bytes); });
}

extern "C" esp_err_t __wrap_esp_partition_write(
    const esp_partition_t* partition, const size_t offset, const void* data, const size_t bytes) {
    return Observe(partition, firmware::SettingsPhysicalAccessKind::Write, bytes,
                   [&] { return __real_esp_partition_write(partition, offset, data, bytes); });
}

extern "C" esp_err_t __wrap_esp_partition_erase_range(
    const esp_partition_t* partition, const size_t offset, const size_t bytes) {
    return Observe(partition, firmware::SettingsPhysicalAccessKind::Erase, bytes,
                   [&] { return __real_esp_partition_erase_range(partition, offset, bytes); });
}

#endif
