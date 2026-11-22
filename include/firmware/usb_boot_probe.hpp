#pragma once

#include <cstdint>

namespace fxe {
class Console;
}

namespace firmware {
// Temporary failure19 diagnostic; remove after the USB cause is established.
void PrintUsbBootProbe(fxe::Console& console);
void PrintUsbIrqProbe(fxe::Console& console);
void RecordUsbBootPrecharge(uint32_t nominalUs, uint32_t pulses);
void RecordUsbBootFirstNonzeroRender();
void RecordUsbBootServicesReady();
}
