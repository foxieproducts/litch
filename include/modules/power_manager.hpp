#pragma once

#include <cstdint>

namespace modules {

class IPowerManager {
  public:
    virtual ~IPowerManager() = default;

    // \brief Informs power policy about the current LED budget.
    virtual void SetEstimatedLedCurrentMa(uint16_t milliAmps) = 0;

    // \brief Returns the currently allowed LED current budget.
    virtual uint16_t GetAllowedLedCurrentMa() const = 0;
};

} // namespace modules
