#pragma once

#include <cstdint>

namespace modules {

class ILedRenderer {
  public:
    virtual ~ILedRenderer() = default;

    virtual uint16_t GetLedCount() const = 0;

    virtual void Clear() = 0;
    virtual void SetLedRgb(uint16_t ledIndex, uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void Show() = 0;
};

} // namespace modules
