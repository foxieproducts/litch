#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"

namespace firmware_internal {

struct PeriodicTaskCadence {
    uint32_t periodMs = 0;
    bool initialized = false;
    fxe::ElapsedTime sinceLast;

    void EnsureInitialized();
    bool IsDue() const;
    void RescheduleFromNow();
    uint32_t RemainingMs() const;
};

} // namespace firmware_internal
