#include "firmware/ota_platform_adapter.hpp"
#include "firmware/ota_runtime_evidence_internal.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "modules/ota_image_identity_esp32.hpp"

#if defined(ARDUINO) && defined(LITCH_ESP32S3)
#include <array>
#include <cstdint>
#include <cstring>
#include <bootloader_common.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace firmware {
namespace {
constexpr size_t OTA_SELECTOR_SECTOR_BYTES = 0x1000U;
bool Matches(const esp_partition_t* partition,
             const modules::OtaPostBootSlotIdentity& slot) {
    return partition != nullptr && partition->type == ESP_PARTITION_TYPE_APP &&
           partition->address == slot.address && partition->subtype == slot.subtype;
}

bool CaptureSnapshot(modules::OtaPostBootSnapshot& outSnapshot) {
    const std::array<const esp_partition_t*, 2U> slots{{
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr),
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr)}};
    if (esp_ota_get_app_partition_count() != slots.size()) return false;
    for (const auto* slot : slots) {
        if (slot == nullptr || slot->address == 0U || slot->size == 0U ||
            (slot->address & 0xFFFU) != 0U ||
            static_cast<uint64_t>(slot->address) + slot->size > UINT32_MAX) {
            return false;
        }
    }
    if (!(static_cast<uint64_t>(slots[0]->address) + slots[0]->size <= slots[1]->address ||
          static_cast<uint64_t>(slots[1]->address) + slots[1]->size <= slots[0]->address)) {
        return false;
    }
    const auto* data = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    const auto* running = esp_ota_get_running_partition();
    const auto* configured = esp_ota_get_boot_partition();
    if (data == nullptr || data->size < 2U * OTA_SELECTOR_SECTOR_BYTES ||
        running == nullptr || configured == nullptr) {
        return false;
    }
    modules::OtaPostBootSnapshot snapshot;
    std::array<esp_ota_select_entry_t, 2U> entries{};
    static_assert(sizeof(entries[0]) == modules::OTA_POST_BOOT_SELECTOR_BYTES);
    for (size_t index = 0U; index < entries.size(); ++index) {
        if (esp_partition_read(data, index * OTA_SELECTOR_SECTOR_BYTES,
                               snapshot.selectors[index].data(),
                               snapshot.selectors[index].size()) != ESP_OK) {
            return false;
        }
        std::memcpy(&entries[index], snapshot.selectors[index].data(), sizeof(entries[index]));
        snapshot.slots[index] = {slots[index]->address,
                                static_cast<uint8_t>(slots[index]->subtype)};
    }
    // The SDK state API returns the first same-slot record, while mutation
    // targets the active selector. Correlate the actual raw active record.
    const int active = bootloader_common_get_active_otadata(entries.data());
    if (active < 0 || active >= static_cast<int>(entries.size())) {
        return false;
    }
    const uint32_t sequence = entries[active].ota_seq;
    if (sequence == 0U || sequence == UINT32_MAX ||
        !bootloader_common_ota_select_valid(&entries[active])) {
        return false;
    }
    const auto* selected = slots[(sequence - 1U) % slots.size()];
    const modules::OtaPostBootSlotIdentity selectedSlot{
        selected->address, static_cast<uint8_t>(selected->subtype)};
    const bool supportedRunning = Matches(running, snapshot.slots[0]) ||
                                  Matches(running, snapshot.slots[1]);
    if (!supportedRunning || !Matches(configured, selectedSlot)) {
        return false;
    }
    snapshot.running = {running->address, static_cast<uint8_t>(running->subtype)};
    snapshot.selected = selectedSlot;
    snapshot.selectedEntry = static_cast<uint8_t>(active);
    snapshot.selectedSequence = sequence;
    outSnapshot = snapshot;
    return true;
}

} // namespace
} // namespace firmware
#endif

namespace firmware {
bool ReadEsp32OtaBootSnapshotWithFlashLease(modules::OtaPostBootSnapshot& out) {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    return CaptureSnapshot(out);
#else
    (void)out;
    return false;
#endif
}
} // namespace firmware

namespace firmware {
bool ReadEsp32OtaVerifiedSourceOnWorker(
    hardware::RmtFlashArbiter& arbiter,
    modules::OtaPostBootSnapshot& outSnapshot) {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    const auto acquire = [&arbiter]() {
        const TickType_t started = xTaskGetTickCount();
        arbiter.RequestFlash();
        while (!arbiter.TryBeginRequestedFlash()) {
            if (static_cast<TickType_t>(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(5000U)) {
                arbiter.CancelFlashRequest();
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1U));
        }
        return true;
    };
    if (!acquire()) return false;
    modules::OtaPostBootSnapshot snapshot;
    const bool captured = ReadEsp32OtaBootSnapshotWithFlashLease(snapshot);
    const auto capturedStamp = ota_evidence_internal::CaptureStamp();
    const auto* running = esp_ota_get_running_partition();
    arbiter.EndFlash();
    ota_evidence_internal::RecordSnapshot(captured ? &snapshot : nullptr, capturedStamp);
    if (!captured || snapshot.selectedSequence >= UINT32_MAX - 2U ||
        modules::EvaluateOtaPostBootVerification(
            snapshot, modules::OtaPostBootHealth::NotReported).runningState !=
            modules::OtaPostBootRunningState::Valid) return false;
    modules::OtaImageIdentity identity;
    if (modules::ReadEsp32ImageIdentity(running, arbiter, identity) != ESP_OK || !acquire()) return false;
    modules::OtaPostBootSnapshot fresh;
    const bool freshCaptured = ReadEsp32OtaBootSnapshotWithFlashLease(fresh);
    const auto freshStamp = ota_evidence_internal::CaptureStamp();
    const bool unchanged = freshCaptured && modules::OtaPostBootSnapshotsMatch(snapshot, fresh);
    arbiter.EndFlash();
    ota_evidence_internal::RecordSnapshot(freshCaptured ? &fresh : nullptr, freshStamp);
    if (unchanged) outSnapshot = snapshot;
    return unchanged;
#else
    (void)arbiter;
    (void)outSnapshot;
    return false;
#endif
}
} // namespace firmware
