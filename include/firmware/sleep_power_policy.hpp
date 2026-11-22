#pragma once

#include <cstdint>

namespace firmware {

constexpr uint64_t LightSleepTimerWakeIntervalUs() {
    return 250000ULL;
}

constexpr bool ShouldEnableLightSleepTimerWake(const bool detachedUsb) {
    if (detachedUsb) {
        return false;
    }

#ifdef LITCH_RELEASE_PROFILE
    return false;
#else
    return true;
#endif
}

} // namespace firmware