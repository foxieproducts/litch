#include "foxie_embedded/heap_stats.hpp"

#ifdef ARDUINO
#include <Arduino.h>
#else
#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif
#endif

namespace fxe {

HeapStats GetHeapStats() {
    HeapStats out;

#ifdef ARDUINO
    out.supported = true;
    out.totalBytes = static_cast<uint64_t>(ESP.getHeapSize());
    out.freeBytes = static_cast<uint64_t>(ESP.getFreeHeap());
    out.minFreeBytes = static_cast<uint64_t>(ESP.getMinFreeHeap());
    out.usedBytes = (out.totalBytes >= out.freeBytes) ? (out.totalBytes - out.freeBytes) : 0;
    return out;
#else
#if defined(__APPLE__)
    out.supported = true;

    malloc_statistics_t stats;
    malloc_zone_t* zone = malloc_default_zone();
    if (zone) {
        malloc_zone_statistics(zone, &stats);
        out.totalBytes = static_cast<uint64_t>(stats.size_allocated);
        out.usedBytes = static_cast<uint64_t>(stats.size_in_use);
        out.freeBytes = (out.totalBytes >= out.usedBytes) ? (out.totalBytes - out.usedBytes) : 0;
    }

    return out;
#else
    // Unsupported platform for now.
    return out;
#endif
#endif
}

} // namespace fxe
