#pragma once
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount();
inline const char* pcTaskGetName(void*) { return "digest_fixture"; }
void vTaskDelay(TickType_t);
inline void taskYIELD() {}
inline UBaseType_t uxTaskGetStackHighWaterMark(void*) { return 0U; }
