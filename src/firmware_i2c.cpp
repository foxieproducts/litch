#include "firmware/firmware_internal.hpp"

#ifdef ARDUINO

namespace firmware_internal {

void SetSystemI2cClock(FirmwareSystem& sys, const uint32_t hz) {
    Wire.setClock(hz);
    sys.i2cClockHz = hz;
    sys.inputScan.SetBusClockHz(hz);
}

} // namespace firmware_internal

#endif
