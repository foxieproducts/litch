#pragma once

using esp_err_t = int;
using intr_handler_t = void (*)(void*);
using intr_handle_t = void*;
constexpr esp_err_t ESP_OK = 0;
constexpr int ESP_INTR_FLAG_LEVEL1 = 2;
constexpr int ESP_INTR_FLAG_LEVEL2 = 4;
constexpr int ESP_INTR_FLAG_LEVEL3 = 8;

extern "C" esp_err_t esp_intr_disable(intr_handle_t handle);
extern "C" esp_err_t esp_intr_enable(intr_handle_t handle);
