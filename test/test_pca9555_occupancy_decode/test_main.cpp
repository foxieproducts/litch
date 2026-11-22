#include <cstdio>

#include "hardware/pca9555_occupancy_decode.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

uint16_t BitForPin(const uint8_t pin) {
    // Matches legacy: bit index is inverted relative to pin.
    return static_cast<uint16_t>(1U << (15U - pin));
}

uint64_t DecodeSingleBit(const uint8_t expander,
                         const uint8_t pin,
                         const uint8_t mapping[hardware::PCA9555_EXPANDER_COUNT][hardware::PCA9555_PINS_PER_EXPANDER]) {
    const uint16_t exp0 = (expander == 0) ? BitForPin(pin) : 0;
    const uint16_t exp1 = (expander == 1) ? BitForPin(pin) : 0;
    const uint16_t exp2 = (expander == 2) ? BitForPin(pin) : 0;
    const uint16_t exp3 = (expander == 3) ? BitForPin(pin) : 0;
    return hardware::DecodePca9555Occupancy64(exp0, exp1, exp2, exp3, mapping);
}

void CheckMappingVariant(const char* variant_name,
                         const uint8_t mapping[hardware::PCA9555_EXPANDER_COUNT][hardware::PCA9555_PINS_PER_EXPANDER]) {
    for (uint8_t expander = 0; expander < hardware::PCA9555_EXPANDER_COUNT; ++expander) {
        for (uint8_t pin = 0; pin < hardware::PCA9555_PINS_PER_EXPANDER; ++pin) {
            const uint8_t square = mapping[expander][pin];
            const uint64_t occ = DecodeSingleBit(expander, pin, mapping);
            const uint64_t expected = (1ULL << square);
            char msg[96];
            std::snprintf(msg,
                          sizeof(msg),
                          "%s exp%u pin%u maps to square %u",
                          variant_name,
                          static_cast<unsigned>(expander),
                          static_cast<unsigned>(pin),
                          static_cast<unsigned>(square));
            Check(occ == expected, msg);
        }
    }

    const uint64_t full_occ = hardware::DecodePca9555Occupancy64(0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, mapping);
    char full_msg[96];
    std::snprintf(full_msg, sizeof(full_msg), "%s full occupancy covers all 64 squares", variant_name);
    Check(full_occ == 0xFFFFFFFFFFFFFFFFULL, full_msg);
}

} // namespace

int main() {
    std::puts("[PCA9555_OCC] start");

    // Default native path should continue to use the legacy-compatible C3 mapping.
    {
        const uint64_t occ = hardware::DecodePca9555Occupancy64(BitForPin(0), 0, 0, 0);
        Check(((occ >> 19) & 1ULL) == 1ULL, "exp0 pin0 maps to square 19");
    }

    {
        const uint64_t occ = hardware::DecodePca9555Occupancy64(0, BitForPin(15), 0, 0);
        Check(((occ >> 7) & 1ULL) == 1ULL, "exp1 pin15 maps to square 7");
    }

    {
        const uint64_t occ = hardware::DecodePca9555Occupancy64(0, 0, BitForPin(8), 0);
        Check(((occ >> 32) & 1ULL) == 1ULL, "exp2 pin8 maps to square 32");
    }

    {
        const uint64_t occ = hardware::DecodePca9555Occupancy64(0, 0, 0, BitForPin(3));
        Check(((occ >> 60) & 1ULL) == 1ULL, "exp3 pin3 maps to square 60");
    }

    CheckMappingVariant("c3", hardware::EXPANDER_PIN_TO_SQUARE_C3);
    CheckMappingVariant("s3", hardware::EXPANDER_PIN_TO_SQUARE_S3);

    if (g_failures == 0) {
        std::puts("[PCA9555_OCC] ok");
        return 0;
    }

    std::printf("[PCA9555_OCC] failures=%d\n", g_failures);
    return 1;
}
