#pragma once

#include "hardware/lis2dw12.hpp"

#ifdef ARDUINO

#include <Wire.h>

namespace hardware {

class Lis2dw12Wire final {
  public:
    explicit Lis2dw12Wire(TwoWire& wire) : m_wire(&wire) {}

    Lis2dw12Transport Transport();

  private:
    static bool Read(void* context,
                     uint8_t address,
                     uint8_t reg,
                     uint8_t* data,
                     size_t length);
    static bool Write(void* context,
                      uint8_t address,
                      uint8_t reg,
                      const uint8_t* data,
                      size_t length);
    static void Delay(void* context, uint32_t milliseconds);

    TwoWire* m_wire = nullptr;
};

} // namespace hardware

#endif
