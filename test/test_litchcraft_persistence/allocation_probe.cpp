#include "allocation_probe.hpp"

#include <cstdint>
#include <array>
#include <cstdio>
#include <new>

namespace litchcraft_allocation_probe {
size_t maximumBytes = SIZE_MAX;
size_t maximumTotalBytes = SIZE_MAX;
size_t acceptedBytes = 0U;
size_t calls = 0U;
size_t largestRequest = 0U;
bool topologyEnabled = false;
std::array<size_t,14> holes{};
void ResetTopology(bool fragmented) {
    holes = fragmented ? std::array<size_t,14>{7668,4096,4096,2048,2048,2048,1024,1024,512,512,256,256,144,244}
                       : std::array<size_t,14>{25976};
    topologyEnabled = true;
    acceptedBytes = 0U; calls = 0U; largestRequest = 0U;
}


}

// Observe the real nonthrowing firmware allocation boundary. Unlike a fake
// persistence allocator, this also catches a future reintroduced allocation.
void* operator new(const size_t bytes, const std::nothrow_t&) noexcept {
    using namespace litchcraft_allocation_probe;
    ++calls;
    if (bytes > largestRequest) largestRequest = bytes;
    if (bytes > maximumBytes || acceptedBytes > maximumTotalBytes ||
        bytes > maximumTotalBytes - acceptedBytes) return nullptr;
    if (topologyEnabled) {
        const size_t charged = (bytes + 7U) / 8U * 8U + 8U;
        size_t selected = holes.size();
        for (size_t i=0U;i<holes.size();++i)
            if (holes[i]>=charged && (selected==holes.size() || holes[i]<holes[selected])) selected=i;
        std::printf("[TOPOLOGY_ALLOC] request=%zu charged=%zu ordinal=%zu hole=%zu accepted=%u\n",bytes,charged,calls,
            selected==holes.size()?0U:holes[selected],selected!=holes.size());
        if (selected==holes.size()) return nullptr;
        holes[selected]-=charged;
    }
    try {
        void* memory = ::operator new(bytes);
        acceptedBytes += bytes;
        return memory;
    } catch (...) {
        return nullptr;
    }
}
