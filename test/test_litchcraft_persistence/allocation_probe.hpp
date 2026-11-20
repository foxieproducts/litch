#pragma once

#include <cstddef>

namespace litchcraft_allocation_probe {
extern size_t maximumBytes;
// Controlled per-search payload budget, separate from the largest-block cap.
// Reset acceptedBytes only while the tested worker is quiescent.
extern size_t maximumTotalBytes;
extern size_t acceptedBytes;
extern size_t calls;
extern size_t largestRequest;
}

namespace litchcraft_allocation_probe {
extern bool topologyEnabled;
void ResetTopology(bool fragmented);
}
