#pragma once

#include <mutex>
#include "freertos/FreeRTOS.h"

using StaticSemaphore_t = std::mutex;
using SemaphoreHandle_t = StaticSemaphore_t*;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* mutex) { return mutex; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks) {
    if (ticks == 0U) return mutex->try_lock() ? 1 : 0;
    mutex->lock();
    return 1;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); return 1; }
