#pragma once

#include "freertos/FreeRTOS.h"

using TaskFunction_t = void (*)(void*);

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task,
                                   const char* name,
                                   uint32_t stackBytes,
                                   void* context,
                                   uint32_t priority,
                                   TaskHandle_t* handle,
                                   int core);
void vTaskDelay(uint32_t ticks);
void vTaskDelete(TaskHandle_t handle);
unsigned int uxTaskGetStackHighWaterMark(TaskHandle_t handle);

TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t task,
                                          const char* name,
                                          uint32_t stackBytes,
                                          void* context,
                                          UBaseType_t priority,
                                          StackType_t* stack,
                                          StaticTask_t* controlBlock,
                                          BaseType_t core);
BaseType_t xTaskNotifyGive(TaskHandle_t handle);
uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t ticksToWait);

TickType_t xTaskGetTickCount();
inline const char* pcTaskGetName(TaskHandle_t) { return "native_fixture"; }
#ifndef taskYIELD
#define taskYIELD() do {} while (0)
#endif
