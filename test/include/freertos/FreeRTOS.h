#pragma once

#include <cstdint>

using BaseType_t = int;
using TaskHandle_t = void*;
using UBaseType_t = unsigned int;
using TickType_t = uint32_t;
using StackType_t = uint8_t;
struct StaticTask_t { uint8_t storage[384]; };

constexpr BaseType_t pdPASS = 1;
constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdFALSE = 0;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;

#define pdMS_TO_TICKS(ms) (ms)
