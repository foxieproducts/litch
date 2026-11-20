#pragma once
#include <cstdint>
using TickType_t=uint32_t;
using UBaseType_t=unsigned;
using BaseType_t=int;
constexpr BaseType_t pdPASS=1;
#define pdMS_TO_TICKS(ms) (ms)
