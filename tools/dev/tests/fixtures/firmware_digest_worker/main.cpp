#include <array>
#include <cstdio>
#include <cstring>
#include "hardware/rmt_flash_arbiter.hpp"
#include "modules/firmware_digest_provider.hpp"
#include "modules/ota_image_identity_esp32.hpp"
#include "esp_partition.h"
#include "freertos/task.h"

namespace fixture {
hardware::RmtFlashArbiter arbiter;
modules::FirmwareDigestProvider* pending = nullptr;
unsigned ticks = 1000U;
unsigned delays = 0U;
unsigned reads = 0U;
bool otherReadActive = false;
bool releaseAfterDelay = true;
bool sdkFails = false;
bool sdkOverlap = false;
std::array<uint8_t, 336> image{};
}

namespace firmware {
bool QueueEsp32FirmwareDigest(modules::FirmwareDigestProvider& provider) {
    if (fixture::pending != nullptr) return false;
    fixture::pending = &provider;
    return true;
}
}

void DispatchDigest(const uint32_t advanceBeforePublication = 0U) {
    auto* provider = fixture::pending;
    if (provider == nullptr) return;
    fixture::pending = nullptr;
    uint8_t digest[modules::FirmwareDigestProvider::DIGEST_BYTES]{};
    if (modules::ReadEsp32FirmwareDigest(*provider, digest, nullptr) == ESP_OK) {
        fixture::ticks += advanceBeforePublication;
        provider->CompleteStandalone(digest, modules::Esp32ImageReadWindow::TIMEOUT_MS);
    } else provider->Fail();
}
TickType_t xTaskGetTickCount() { return fixture::ticks; }
void vTaskDelay(TickType_t ticks) {
    fixture::ticks += ticks;
    ++fixture::delays;
    if (fixture::otherReadActive && fixture::releaseAfterDelay) {
        fixture::otherReadActive = false;
        fixture::arbiter.EndFlash();
    }
}
namespace fxe { uint32_t Millis() { return fixture::ticks; } }
const esp_partition_t* esp_ota_get_running_partition() {
    static const esp_partition_t partition{};
    return &partition;
}
esp_err_t esp_partition_read(const esp_partition_t*, size_t offset,
                             void* out, size_t size) {
    ++fixture::reads;
    // A concurrent OTA read owns the same production flash lease.
    if (fixture::otherReadActive) {
        fixture::sdkOverlap = true;
        return ESP_ERR_IMAGE_INVALID;
    }
    if (fixture::arbiter.TryBeginFlash(
            fixture::arbiter.CompletedRenderGeneration())) {
        fixture::arbiter.EndFlash();
        std::puts("FAIL: digest SDK call did not hold the shared flash lease");
        return ESP_ERR_IMAGE_INVALID;
    }
    if (fixture::arbiter.TryBeginRender()) {
        fixture::arbiter.EndRender();
        std::puts("FAIL: digest SDK call allowed an RMT transfer");
        return ESP_ERR_IMAGE_INVALID;
    }
    if (fixture::sdkFails) { return ESP_ERR_IMAGE_INVALID; }
    if (offset > fixture::image.size() || size > fixture::image.size() - offset) {
        return ESP_ERR_IMAGE_INVALID;
    }
    std::memcpy(out, fixture::image.data() + offset, size);
    return ESP_OK;
}

bool Check(bool value, const char* message) {
    if (!value) { std::printf("FAIL: %s\n", message); }
    return value;
}
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    FILE* file = std::fopen(argv[1], "rb");
    if (file == nullptr) return 1;
    const size_t read = std::fread(fixture::image.data(), 1,
                                  fixture::image.size(), file);
    std::fclose(file);
    if (read != fixture::image.size()) return 1;
    modules::FirmwareDigestProvider provider{&fixture::arbiter};
    fixture::arbiter.TryBeginFlash(fixture::arbiter.CompletedRenderGeneration());
    fixture::otherReadActive = true;
    if (!Check(modules::StartEsp32FirmwareDigest(provider), "worker starts")) {
        return 1;
    }
    DispatchDigest();
    uint8_t digest[32]{};
    if (!Check(!fixture::sdkOverlap, "digest waits for existing OTA SDK mapping") ||
        !Check(fixture::delays > 0U, "wait yields rather than blocking core") ||
        !Check(provider.CopyDigest(digest, sizeof(digest)) &&
                   std::memcmp(digest, fixture::image.data() + 304, sizeof(digest)) == 0,
               "successful digest becomes ready") ||
        !Check(fixture::arbiter.TryBeginFlash(
                   fixture::arbiter.CompletedRenderGeneration()),
               "success releases shared flash lease")) { return 1; }
    fixture::arbiter.EndFlash();
    modules::FirmwareDigestProvider failing{&fixture::arbiter};
    fixture::sdkFails = true;
    modules::StartEsp32FirmwareDigest(failing);
    DispatchDigest();
    if (!Check(failing.State() == modules::FirmwareDigestState::Error,
               "SDK failure remains an error") ||
        !Check(fixture::arbiter.TryBeginRender(),
               "SDK failure releases lease for LEDs")) { return 1; }
    fixture::arbiter.EndRender();
    fixture::sdkFails = false;
    modules::FirmwareDigestProvider blocked{&fixture::arbiter};
    fixture::releaseAfterDelay = false;
    fixture::otherReadActive = true;
    fixture::arbiter.TryBeginFlash(fixture::arbiter.CompletedRenderGeneration());
    const unsigned readsBefore = fixture::reads;
    const uint32_t startedMs = UINT32_MAX - 8000U;
    fixture::ticks = startedMs;
    modules::StartEsp32FirmwareDigest(blocked);
    std::atomic<bool> urgent{true};
    if (!Check(modules::ReadEsp32FirmwareDigest(blocked, digest, &urgent) == ESP_ERR_NOT_FINISHED,
               "urgent OTA work defers the active digest without releasing its deadline")) return 1;
    fixture::ticks += 14000U;
    DispatchDigest();
    if (!Check(fixture::ticks - startedMs == 18000U &&
                   modules::ReadEsp32FirmwareDigest(blocked, digest, &urgent) == ESP_ERR_TIMEOUT,
               "resumed image scan retains its original deadline across clock wrap and another interruption") ||
        !Check(blocked.State() == modules::FirmwareDigestState::Error,
               "busy owner times out truthfully") ||
        !Check(fixture::reads == readsBefore, "timeout never overlaps SDK read") ||
        !Check(!fixture::arbiter.TryBeginFlash(
                   fixture::arbiter.CompletedRenderGeneration()),
               "timeout never releases another owner's lease")) { return 1; }
    fixture::otherReadActive = false;
    fixture::arbiter.EndFlash();
    modules::StartEsp32FirmwareDigest(blocked);
    DispatchDigest(18000U);
    if (!Check(blocked.State() == modules::FirmwareDigestState::Error,
               "expired standalone result cannot publish Ready after its final read")) return 1;
    modules::FirmwareDigestProvider unbound;
    if (!Check(!modules::StartEsp32FirmwareDigest(unbound),
               "missing shared owner does not start unsafe read")) { return 1; }
    std::puts("PASS: production digest worker verifies image, serializes reads and releases leases");
}
