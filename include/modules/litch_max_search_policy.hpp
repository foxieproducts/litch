#pragma once
#include <cstdint>

#if defined(LITCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace modules {
inline void CooperateLitchMaxSearch(void*, const uint32_t nodes) {
#if defined(LITCH_ESP32S3)
    constexpr uint32_t YIELD_NODE_MASK = 0x1ffU;
#else
    constexpr uint32_t YIELD_NODE_MASK = 0x1fffU;
#endif
    // Retain the existing scheduling cadence so IDLE can feed TWDT.
    if ((nodes & YIELD_NODE_MASK) == 0U) vTaskDelay(1);
}
} // namespace modules
#endif
