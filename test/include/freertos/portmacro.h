#pragma once

#include <mutex>

// Controlled native SDK boundary for the production Arduino logger owner.
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
inline void portENTER_CRITICAL(portMUX_TYPE* mux) { mux->lock(); }
inline void portEXIT_CRITICAL(portMUX_TYPE* mux) { mux->unlock(); }
