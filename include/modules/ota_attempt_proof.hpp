#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/ota_platform_adapter.hpp"

namespace modules {

struct OtaAttemptProofRequest {
    uint32_t requestId = 0U;
    uint64_t bootNonce = 0U;
    bool runtimeAccepted = false;
};

/** Copy bounded RAM evidence and encode version 1, or correlated unavailable. */
bool FormatOtaAttemptProof(OtaPlatformAdapter* adapter,
                          const OtaAttemptProofRequest& request,
                          char* out, size_t capacity);

} // namespace modules
