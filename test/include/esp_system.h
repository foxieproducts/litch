#pragma once
#include <cstdint>
inline unsigned esp_reset_reason() { return 1U; }
inline uint32_t esp_random() { static uint32_t next = 100U; return ++next; }
