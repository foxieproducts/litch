#pragma once

#include <cstdint>

namespace modules {

class IAmbientLight {
  public:
    virtual ~IAmbientLight() = default;

    // \brief Returns a monotonic ambient brightness estimate.
    // \note Units are implementation-defined; consumers should treat it as
    //       relative brightness.
    virtual uint16_t ReadLevel() const = 0;
};

} // namespace modules
