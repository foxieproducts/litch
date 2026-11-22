#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef void* QueueHandle_t;
typedef void* SemaphoreHandle_t;
#define configSUPPORT_STATIC_ALLOCATION 0
#define configUSE_MUTEXES 1
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
#define pdFALSE 0
#define pdTRUE 1
#define portYIELD_FROM_ISR(...) ((void)0)
