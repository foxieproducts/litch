#pragma once

#include <cstdint>

namespace fxe {

struct HeapStats {
    bool supported = false;

    // Total heap bytes (if known).
    uint64_t totalBytes = 0;

    // Bytes currently allocated/in-use (if known).
    uint64_t usedBytes = 0;

    // Bytes currently free (if known).
    uint64_t freeBytes = 0;

    // Minimum free heap observed (device-only, if available).
    uint64_t minFreeBytes = 0;
};

// Cross-target heap stats.
// - On ESP32 Arduino: uses ESP.getHeapSize/getFreeHeap/getMinFreeHeap.
// - On macOS: uses malloc_zone_statistics.
// - Other native platforms: returns supported=false.
HeapStats GetHeapStats();

} // namespace fxe
