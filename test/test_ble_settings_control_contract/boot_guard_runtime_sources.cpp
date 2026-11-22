#include "ble_service_test_config.hpp"

#include <cstdint>
#include <esp_attr.h>
#include <esp_system.h>

#include "runtime/boot_guard.hpp"

namespace ota_runtime_test {
namespace {
uint32_t resetReason = 1U;
}
uint32_t ObserveBootResetReason() { return resetReason; }
void SetBootResetReason(const uint32_t reason) { resetReason = reason; }
} // namespace ota_runtime_test

// Compile the actual retained-state owner. Only SDK reset cause and RTC
// placement are substituted; all counters, intent consumption and latches are
// production. Values match the pinned ESP-IDF reset-reason enum.
using esp_reset_reason_t = unsigned;
#define ESP_RST_POWERON 1U
#define ESP_RST_EXT 2U
#define ESP_RST_SW 3U
#define ESP_RST_PANIC 4U
#define ESP_RST_INT_WDT 5U
#define ESP_RST_TASK_WDT 6U
#define ESP_RST_WDT 7U
#define ESP_RST_BROWNOUT 9U
#define RTC_NOINIT_ATTR
#define esp_reset_reason ota_runtime_test::ObserveBootResetReason
#define ARDUINO 1
#include "../../src/boot_guard.cpp"
#undef ARDUINO
#undef esp_reset_reason
#undef RTC_NOINIT_ATTR
#undef ESP_RST_BROWNOUT
#undef ESP_RST_WDT
#undef ESP_RST_TASK_WDT
#undef ESP_RST_INT_WDT
#undef ESP_RST_PANIC
#undef ESP_RST_SW
#undef ESP_RST_EXT
#undef ESP_RST_POWERON
