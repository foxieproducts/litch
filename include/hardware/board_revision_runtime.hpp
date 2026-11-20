#pragma once

#include <cstdint>

#include "hardware/board_hardware_profile.hpp"

namespace hardware {

// Adapter contract used by firmware and native tests. All three pull-ups are
// configured before any strap is read, so floating V2 pins resolve high.
template <typename GpioReader>
RuntimeBoardHardwareProfile SampleS3RuntimeHardwareProfile(
    GpioReader& gpio) {
    for (const uint8_t pin : S3_REVISION_STRAP_PINS) {
        gpio.ConfigureInputPullup(pin);
    }

    const S3RevisionStrapSample sample{
        gpio.IsLow(S3_REVISION_STRAP_PINS[0]),
        gpio.IsLow(S3_REVISION_STRAP_PINS[1]),
        gpio.IsLow(S3_REVISION_STRAP_PINS[2]),
    };
    return RuntimeProfileForS3Sample(sample);
}

} // namespace hardware
