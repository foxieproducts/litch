#pragma once

#ifdef ARDUINO

namespace firmware_internal {

struct FirmwareSystem;

void PublishHomeStateIfNeeded(FirmwareSystem& sys);

} // namespace firmware_internal

#endif
