#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"

// Shared native SDK boundary for production S3 adapter contract fixtures.
struct esp_flash_t;
enum esp_partition_type_t {
    ESP_PARTITION_TYPE_APP = 0x00,
    ESP_PARTITION_TYPE_DATA = 0x01,
};
enum esp_partition_subtype_t {
    ESP_PARTITION_SUBTYPE_DATA_OTA = 0x00,
    ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82,
    ESP_PARTITION_SUBTYPE_APP_OTA_MIN = 0x10,
    ESP_PARTITION_SUBTYPE_APP_OTA_0 = 0x10,
    ESP_PARTITION_SUBTYPE_APP_OTA_1 = 0x11,
    ESP_PARTITION_SUBTYPE_APP_OTA_2 = 0x12,
    ESP_PARTITION_SUBTYPE_APP_OTA_MAX = 0x20,
};
struct esp_partition_t {
    esp_flash_t* flash_chip = nullptr;
    esp_partition_type_t type = ESP_PARTITION_TYPE_APP;
    esp_partition_subtype_t subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    uint32_t address = 0U;
    uint32_t size = 0U;
    char label[17]{};
    bool encrypted = false;
};

extern "C" {
const esp_partition_t* esp_partition_find_first(esp_partition_type_t type,
    esp_partition_subtype_t subtype, const char* label);
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset,
                              void* destination, size_t size);
}
