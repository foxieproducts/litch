#pragma once

#if defined(LITCH_ESP32)
#include <atomic>
#include <esp_err.h>
#include <esp_partition.h>

#include "foxie_embedded/millis.hpp"
#include "modules/ota_image_identity.hpp"

namespace hardware {
class RmtFlashArbiter;
}

namespace modules {

struct Esp32ImageReadWindow {
    static constexpr uint32_t TIMEOUT_MS = 18000U;
    uint32_t startedMs = fxe::Millis();
    const std::atomic<bool>* urgentRequest = nullptr;
    bool Expired() const { return static_cast<uint32_t>(fxe::Millis() - startedMs) >= TIMEOUT_MS; }
};

/** Full software verification, with bounded flash leases and no SHA/GDMA heap. */
esp_err_t ReadEsp32ImageIdentity(const esp_partition_t* partition,
                                hardware::RmtFlashArbiter& arbiter,
                                OtaImageIdentity& outIdentity,
                                Esp32ImageReadWindow window = {});

} // namespace modules
#endif
