#pragma once

#include "esp_partition.h"

#include "esp_err.h"
enum esp_ota_img_states_t {
    ESP_OTA_IMG_NEW = 0U,
    ESP_OTA_IMG_PENDING_VERIFY = 1U,
    ESP_OTA_IMG_VALID = 2U,
    ESP_OTA_IMG_INVALID = 3U,
    ESP_OTA_IMG_ABORTED = 4U,
    ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFFU,
};

extern "C" {
const esp_partition_t* esp_ota_get_running_partition();
const esp_partition_t* esp_ota_get_boot_partition();
uint8_t esp_ota_get_app_partition_count();
esp_err_t esp_ota_mark_app_valid_cancel_rollback();
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot();
const esp_partition_t* esp_ota_get_next_update_partition(
    const esp_partition_t* start_from);
esp_err_t esp_ota_get_state_partition(const esp_partition_t* partition,
                                        esp_ota_img_states_t* ota_state);
}
