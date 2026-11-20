#include "modules/firmware_digest_provider.hpp"

#if defined(LITCH_ESP32)
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if defined(LITCH_ESP32S3)
#include "firmware/ota_platform_adapter.hpp"
#endif
#include "foxie_embedded/millis.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "logger.hpp"
#include "modules/ota_image_identity_esp32.hpp"
#endif

namespace modules {

#if defined(LITCH_ESP32)
namespace {

constexpr uint32_t kEarliestDigestStartMs = 1000U;

#if !defined(LITCH_ESP32S3)
void FirmwareDigestTask(void* const context) {
    auto& provider = *static_cast<FirmwareDigestProvider*>(context);
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint8_t digest[FirmwareDigestProvider::DIGEST_BYTES]{};
        if (ReadEsp32FirmwareDigest(provider, digest, nullptr) == ESP_OK)
            provider.CompleteStandalone(digest, Esp32ImageReadWindow::TIMEOUT_MS);
        else provider.Fail();
    }
}
#endif
} // namespace

esp_err_t ReadEsp32FirmwareDigest(FirmwareDigestProvider& provider, uint8_t* const digest,
                                  const std::atomic<bool>* const urgentRequest) {
    const Esp32ImageReadWindow window{provider.StandaloneStartedMs(), urgentRequest};
    while (fxe::Millis() < kEarliestDigestStartMs && !window.Expired()) {
        if (urgentRequest != nullptr && urgentRequest->load(std::memory_order_acquire))
            return ESP_ERR_NOT_FINISHED;
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    if (window.Expired()) return ESP_ERR_TIMEOUT;
    const auto* partition = esp_ota_get_running_partition();
    OtaImageIdentity identity;
    esp_err_t result = ReadEsp32ImageIdentity(partition, *provider.FlashArbiter(), identity, window);
    if (result == ESP_OK && esp_ota_get_running_partition() != partition) result = ESP_ERR_INVALID_STATE;
    if (window.Expired()) result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK) {
        for (size_t index = 0U; index < identity.imageSha256.size(); ++index)
            digest[index] = identity.imageSha256[index];
    } else if (result != ESP_ERR_NOT_FINISHED || urgentRequest == nullptr ||
               !urgentRequest->load(std::memory_order_acquire)) {
        logger::Warn("firmware digest failed: error=%d", static_cast<int>(result));
    }
    return result;
}

bool StartEsp32FirmwareDigest(FirmwareDigestProvider& provider) {
    if (provider.FlashArbiter() == nullptr) {
        return false;
    }
    const FirmwareDigestState state = provider.State();
    if (state == FirmwareDigestState::Ready || state == FirmwareDigestState::Pending) {
        return true;
    }
    if (!provider.TryBeginStandalone(fxe::Millis())) {
        const auto joined = provider.State();
        return joined == FirmwareDigestState::Pending || joined == FirmwareDigestState::Ready;
    }

#if defined(LITCH_ESP32S3)
    if (!firmware::QueueEsp32FirmwareDigest(provider)) {
        provider.Fail();
        return false;
    }
#else
    if (provider.m_task == nullptr) {
        provider.m_task = xTaskCreateStaticPinnedToCore(
            FirmwareDigestTask, "fw_digest", FirmwareDigestProvider::TASK_STACK_BYTES, &provider, 1U,
            provider.m_taskStack, &provider.m_taskControl, 0);
        if (provider.m_task == nullptr) {
            provider.Fail();
            return false;
        }
    }
    xTaskNotifyGive(provider.m_task);
#endif
    return true;
}
#else
bool StartEsp32FirmwareDigest(FirmwareDigestProvider& provider) {
    (void)provider;
    return false;
}
#endif

} // namespace modules
