#pragma once
#include "freertos/FreeRTOS.h"
#include <mutex>
using QueueHandle_t = void*;

struct StaticQueue_t {
    std::mutex mutex;
    uint8_t* storage = nullptr;
    UBaseType_t capacity = 0U;
    UBaseType_t itemSize = 0U;
    UBaseType_t head = 0U;
    UBaseType_t count = 0U;
};

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize);
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t itemSize,
                               uint8_t* storage, StaticQueue_t* control);
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t wait);
BaseType_t xQueueSendToFront(QueueHandle_t queue, const void* item, TickType_t wait);
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void* item);
BaseType_t xQueuePeek(QueueHandle_t queue, void* item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t wait);

inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t handle) {
    if (handle == nullptr) return 0U;
    auto& queue = *static_cast<StaticQueue_t*>(handle);
    std::lock_guard<std::mutex> lock(queue.mutex);
    return queue.count;
}
