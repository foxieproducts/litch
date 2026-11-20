#include "modules/vs_cpu_rng.hpp"

#include <cstdint>

namespace modules {

namespace {

constexpr uint32_t DEFAULT_VS_CPU_RNG_SEED = 1U;

uint32_t NormalizeVsCpuRngSeed(const uint32_t seed) {
    return (seed == 0U) ? DEFAULT_VS_CPU_RNG_SEED : seed;
}

uint32_t HashUciMoveString(const char* uci) {
    // FNV-1a 32-bit. Keeps behavior stable across toolchains.
    uint32_t h = 2166136261U;
    if (uci == nullptr) {
        return h;
    }
    for (const char* p = uci; *p != '\0'; ++p) {
        h ^= static_cast<uint8_t>(*p);
        h *= 16777619U;
    }
    return h;
}

uint32_t MixSeed32(uint32_t seed, const uint32_t v) {
    // Tiny Jenkins-style mix; good enough for seed evolution (not crypto).
    seed ^= v + 0x9e3779b9U + (seed << 6) + (seed >> 2);
    return NormalizeVsCpuRngSeed(seed);
}

} // namespace

uint32_t NextVsCpuSeedAfterMove(const uint32_t seed, const char* uciMove) {
    return MixSeed32(seed, HashUciMoveString(uciMove));
}

} // namespace modules
