#pragma once
#include <cstdint>
#include <cstddef>
#include "esp_err.h"
constexpr unsigned ESP_PARTITION_TYPE_APP = 0;
struct esp_partition_t {
    unsigned type = ESP_PARTITION_TYPE_APP;
    uint32_t address = 0x10000U;
    uint32_t size = 4096;
};
esp_err_t esp_partition_read(const esp_partition_t*, size_t, void*, size_t);
