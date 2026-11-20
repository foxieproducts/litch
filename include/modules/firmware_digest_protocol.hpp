#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/firmware_digest_provider.hpp"

namespace modules {

bool FormatFirmwareDigestState(FirmwareDigestState state,
                               char* outPayload,
                               size_t capacity);
bool FormatFirmwareDigestFragment(const uint8_t* digest,
                                  uint8_t fragmentIndex,
                                  char* outPayload,
                                  size_t capacity);

} // namespace modules
