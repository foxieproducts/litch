#pragma once
namespace batch_wdt_sdk {
inline unsigned resets = 0;
}
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERROR_CHECK(x) ((void)(x))
inline int esp_task_wdt_init(unsigned, bool) { return 0; }
inline int esp_task_wdt_status(void *) { return 0; }
inline int esp_task_wdt_add(void *) { return 0; }
inline int esp_task_wdt_delete(void *) { return 0; }
inline int esp_task_wdt_reset() {
    ++batch_wdt_sdk::resets;
    return 0;
}
