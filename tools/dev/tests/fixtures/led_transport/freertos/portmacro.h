#pragma once
#include "rmt_fixture.hpp"
using portMUX_TYPE = unsigned;
#define portMUX_INITIALIZER_UNLOCKED 0U
#define portENTER_CRITICAL(mux) (++fixture::criticalDepth)
#define portEXIT_CRITICAL(mux) (--fixture::criticalDepth)
