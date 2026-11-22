#include "modules/firmware_digest_protocol.hpp"

#include <cstdio>

namespace modules {

bool FormatFirmwareDigestState(const FirmwareDigestState state,
                               char* const outPayload,
                               const size_t capacity) {
    if (outPayload == nullptr || capacity == 0U) {
        return false;
    }

    char token = 'u';
    switch (state) {
        case FirmwareDigestState::Pending:
            token = 'p';
            break;
        case FirmwareDigestState::Ready:
            token = 'r';
            break;
        case FirmwareDigestState::Error:
            token = 'e';
            break;
        case FirmwareDigestState::Unavailable:
        default:
            token = 'u';
            break;
    }
    const int written = std::snprintf(outPayload, capacity, "{\"h\":\"%c\"}", token);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool FormatFirmwareDigestFragment(const uint8_t* const digest,
                                  const uint8_t fragmentIndex,
                                  char* const outPayload,
                                  const size_t capacity) {
    if (digest == nullptr || outPayload == nullptr || fragmentIndex >= 8U) {
        return false;
    }
    const size_t offset = static_cast<size_t>(fragmentIndex) * 4U;
    const int written = std::snprintf(outPayload,
                                      capacity,
                                      "{\"h%u\":\"%02x%02x%02x%02x\"}",
                                      static_cast<unsigned>(fragmentIndex),
                                      static_cast<unsigned>(digest[offset]),
                                      static_cast<unsigned>(digest[offset + 1U]),
                                      static_cast<unsigned>(digest[offset + 2U]),
                                      static_cast<unsigned>(digest[offset + 3U]));
    return written > 0 && static_cast<size_t>(written) < capacity;
}

} // namespace modules
