#include "modules/ota_image_identity_esp32.hpp"

#if defined(LITCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#if !defined(LITCH_NATIVE)
#include <esp_timer.h>
#endif

#include "foxie_embedded/elapsed_time.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "logger.hpp"

namespace modules {
namespace {

// Initial and final SDK snapshots each retain a five-second lease wait.
// Leave the remaining worker request budget for SDK overhead; target timing
// still requires measurement.
constexpr uint32_t FLASH_ACQUIRE_TIMEOUT_MS = 5000U;
constexpr uint32_t YIELD_AFTER_BYTES = 4096U;

uint64_t DiagnosticClockUs() {
#if defined(LITCH_NATIVE)
    // Existing actual-source fixtures retain their manual timing authority.
    return static_cast<uint64_t>(fxe::Millis()) * 1000ULL;
#else
    // Platform-only duration profiling, never an application/event clock.
    return static_cast<uint64_t>(esp_timer_get_time());
#endif
}

struct PartitionReadContext {
    const esp_partition_t& partition;
    hardware::RmtFlashArbiter& arbiter;
    Esp32ImageReadWindow window;
    fxe::ElapsedTime elapsed = fxe::ElapsedTime::StartNow();
    uint32_t bytesSinceYield = 0U;
    uint32_t readAttempts = 0U;
    uint32_t lastOffset = 0U;
    uint32_t lastBytes = 0U;
    uint32_t grantWaitMs = 0U;
    uint32_t maxGrantWaitMs = 0U;
    uint64_t validationResumedUs = DiagnosticClockUs();
    uint32_t validationUs = 0U;
    uint32_t sdkReadUs = 0U;
    uint32_t grantWaitUs = 0U;
    uint32_t yieldUs = 0U;
    uint32_t readBytes = 0U;
    esp_err_t result = ESP_OK;
    const char* reason = "ok";

    void PauseValidation() {
        validationUs += static_cast<uint32_t>(DiagnosticClockUs() - validationResumedUs);
    }

    bool ResumeValidation(const bool accepted) {
        validationResumedUs = DiagnosticClockUs();
        return accepted;
    }

    void RecordGrantWait(const fxe::ElapsedTime& wait) {
        const uint32_t waitMs = wait.ElapsedMs();
        grantWaitMs += waitMs;
        if (waitMs > maxGrantWaitMs) maxGrantWaitMs = waitMs;
    }

    void Report(const esp_err_t error, const char* const outcome) const {
        const auto log = error == ESP_OK ? logger::Info : logger::Warn;
        const char* const task = pcTaskGetName(nullptr);
        log("ota_image: task=%s slot=%08lx err=%d reason=%s elapsed_ms=%lu "
            "reads=%lu off=%lu n=%lu wait_ms=%lu max_wait_ms=%lu stack_free_bytes=%lu",
            task == nullptr ? "unknown" : task,
            static_cast<unsigned long>(partition.address), static_cast<int>(error), outcome,
            static_cast<unsigned long>(elapsed.ElapsedMs()), static_cast<unsigned long>(readAttempts),
            static_cast<unsigned long>(lastOffset), static_cast<unsigned long>(lastBytes),
            static_cast<unsigned long>(grantWaitMs), static_cast<unsigned long>(maxGrantWaitMs),
            static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
        log("ota_image_cost: task=%s bytes=%lu validation_hash_us=%lu sdk_read_us=%lu grant_wait_us=%lu yield_us=%lu scope=wall_including_scheduling",
            task == nullptr ? "unknown" : task,
            static_cast<unsigned long>(readBytes), static_cast<unsigned long>(validationUs),
            static_cast<unsigned long>(sdkReadUs), static_cast<unsigned long>(grantWaitUs),
            static_cast<unsigned long>(yieldUs));
    }

    static bool Read(void* context, const uint32_t offset,
                     uint8_t* out, const size_t size) {
        auto& self = *static_cast<PartitionReadContext*>(context);
        self.PauseValidation();
        ++self.readAttempts;
        self.lastOffset = offset;
        self.lastBytes = static_cast<uint32_t>(size);
        const TickType_t acquireStarted = xTaskGetTickCount();
        const auto grantElapsed = fxe::ElapsedTime::StartNow();
        const uint64_t grantStartedUs = DiagnosticClockUs();
        self.arbiter.RequestFlash();
        while (true) {
            const bool imageExpired = self.window.Expired();
            if (imageExpired ||
                static_cast<TickType_t>(xTaskGetTickCount() - acquireStarted) >=
                pdMS_TO_TICKS(FLASH_ACQUIRE_TIMEOUT_MS)) {
                self.result = ESP_ERR_TIMEOUT;
                self.reason = imageExpired ? "image_deadline" : "grant_timeout";
                self.RecordGrantWait(grantElapsed);
                self.grantWaitUs += static_cast<uint32_t>(DiagnosticClockUs() - grantStartedUs);
                self.arbiter.CancelFlashRequest();
                return self.ResumeValidation(false);
            }
            if (self.window.urgentRequest != nullptr && self.window.urgentRequest->load(std::memory_order_acquire)) {
                self.arbiter.CancelFlashRequest();
                self.result = ESP_ERR_NOT_FINISHED;
                return self.ResumeValidation(false);
            }
            if (self.arbiter.TryBeginRequestedFlash()) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1U));
        }
        self.RecordGrantWait(grantElapsed);
        self.grantWaitUs += static_cast<uint32_t>(DiagnosticClockUs() - grantStartedUs);
        if (self.window.Expired()) {
            self.arbiter.EndFlash();
            self.result = ESP_ERR_TIMEOUT;
            self.reason = "image_deadline";
            return self.ResumeValidation(false);
        }
        const uint64_t readStartedUs = DiagnosticClockUs();
        self.result = esp_partition_read(&self.partition, offset, out, size);
        self.sdkReadUs += static_cast<uint32_t>(DiagnosticClockUs() - readStartedUs);
        self.arbiter.EndFlash();
        if (self.result != ESP_OK) {
            self.reason = "sdk_read";
            return self.ResumeValidation(false);
        }
        self.readBytes += static_cast<uint32_t>(size);
        self.bytesSinceYield += static_cast<uint32_t>(size);
        if (self.bytesSinceYield >= YIELD_AFTER_BYTES) {
            self.bytesSinceYield = 0U;
            const uint64_t yieldStartedUs = DiagnosticClockUs();
            vTaskDelay(pdMS_TO_TICKS(1U));
            self.yieldUs += static_cast<uint32_t>(DiagnosticClockUs() - yieldStartedUs);
        }
        return self.ResumeValidation(true);
    }
};

} // namespace

esp_err_t ReadEsp32ImageIdentity(const esp_partition_t* const partition,
                                hardware::RmtFlashArbiter& arbiter,
                                OtaImageIdentity& outIdentity,
                                const Esp32ImageReadWindow window) {
    if (partition == nullptr || partition->type != ESP_PARTITION_TYPE_APP) {
        logger::Warn("ota_image: task=%s error=%d reason=partition",
                     pcTaskGetName(nullptr), static_cast<int>(ESP_ERR_INVALID_ARG));
        return ESP_ERR_INVALID_ARG;
    }
    PartitionReadContext context{*partition, arbiter, window};
    OtaImageIdentityCollector collector;
    OtaImageIdentity identity;
    const OtaImagePartitionReader reader{
        partition->size, &context, PartitionReadContext::Read};
    const bool valid = collector.ReadPartition(reader, CONFIG_IDF_FIRMWARE_CHIP_ID, identity);
    context.PauseValidation();
    if (!valid) {
        if (context.result == ESP_ERR_NOT_FINISHED) return context.result;
        const esp_err_t result = context.result == ESP_OK ? ESP_ERR_INVALID_CRC : context.result;
        context.Report(result, context.result == ESP_OK ? "integrity" : context.reason);
        return result;
    }
    if (context.window.Expired()) {
        context.Report(ESP_ERR_TIMEOUT, "image_deadline");
        return ESP_ERR_TIMEOUT;
    }
    outIdentity = identity;
    context.Report(ESP_OK, "ok");
    return ESP_OK;
}

} // namespace modules
#endif
