#pragma once
#include "freertos/FreeRTOS.h"
inline TickType_t xTaskGetTickCount() {
    return static_cast<TickType_t>(fixture::nowUs / 1000U);
}
inline void vTaskDelay(TickType_t ticks) { fixture::Advance(ticks * 1000U); }
inline void vTaskSuspendAll() { ++fixture::schedulerDepth; ++fixture::schedulerSuspensions; }
inline int xTaskResumeAll() { --fixture::schedulerDepth; return 0; }
