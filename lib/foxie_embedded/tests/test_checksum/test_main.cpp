#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "foxie_embedded/checksum.hpp"

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::printf("[FAIL] %s\n", message);
}
} // namespace

int main() {
    const std::array<uint8_t, 9U> standard{{'1','2','3','4','5','6','7','8','9'}};
    Check(fxe::Crc32(standard.data(), standard.size()) == 0xcbf43926U,
          "CRC-32 matches its standard known vector");
    for (size_t split = 0U; split <= standard.size(); ++split) {
        const uint32_t left = split == 0U ? 0U : fxe::Crc32(standard.data(), split);
        const uint32_t right = split == standard.size() ? 0U :
            fxe::Crc32(standard.data() + split, standard.size() - split);
        Check(fxe::CombineCrc32(left, right, standard.size() - split) == 0xcbf43926U,
              "every split including empty sides combines to the known CRC");
    }
    std::vector<uint8_t> data(1U << 20U);
    uint32_t state = 0x13572468U;
    for (auto& byte : data) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<uint8_t>(state >> 24U);
    }
    for (size_t power = 0U; power <= 19U; ++power) {
        const size_t size = (size_t(1U) << power) + 1U;
        const size_t split = size / 3U;
        Check(fxe::CombineCrc32(split == 0U ? 0U : fxe::Crc32(data.data(), split),
                  fxe::Crc32(data.data() + split, size - split), size - split) ==
                  fxe::Crc32(data.data(), size),
              "shift powers around binary length boundaries match direct CRC");
    }
    Check(fxe::CombineCrc32(0x12345678U, 0U, 0U) == 0x12345678U,
          "zero second length preserves the first finalized CRC");
    std::printf("[CHECKSUM] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
