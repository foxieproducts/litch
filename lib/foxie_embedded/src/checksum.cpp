#include "foxie_embedded/checksum.hpp"

#include <array>

namespace fxe {
namespace {

constexpr uint32_t MatrixTimes(const uint32_t* const matrix, uint32_t vector) {
    uint32_t result = 0U;
    size_t index = 0U;
    while (vector != 0U) {
        if ((vector & 1U) != 0U) result ^= matrix[index];
        vector >>= 1U;
        ++index;
    }
    return result;
}

constexpr void MatrixSquare(uint32_t* const square,
                             const uint32_t* const matrix) {
    for (size_t index = 0U; index < 32U; ++index) {
        square[index] = MatrixTimes(matrix, matrix[index]);
    }
}

constexpr auto MakeShiftPowers() {
    std::array<std::array<uint32_t, 32U>, sizeof(size_t) * 8U> powers{};
    std::array<uint32_t, 32U> bit{};
    std::array<uint32_t, 32U> twoBits{};
    std::array<uint32_t, 32U> fourBits{};
    bit[0U] = 0xedb88320U;
    uint32_t row = 1U;
    for (size_t index = 1U; index < bit.size(); ++index) {
        bit[index] = row;
        row <<= 1U;
    }
    MatrixSquare(twoBits.data(), bit.data());
    MatrixSquare(fourBits.data(), twoBits.data());
    MatrixSquare(powers[0U].data(), fourBits.data());
    for (size_t index = 1U; index < powers.size(); ++index) {
        MatrixSquare(powers[index].data(), powers[index - 1U].data());
    }
    return powers;
}

// Flash-resident shift powers avoid rebuilding matrices for every small
// cached record. No initializer, heap allocation or mutable shared cache.
constexpr auto CRC_SHIFT_POWERS = MakeShiftPowers();

} // namespace

uint32_t CombineCrc32(uint32_t firstCrc, const uint32_t secondCrc,
                      size_t secondBytes) {
    if (secondBytes == 0U) return firstCrc;
    size_t power = 0U;
    while (secondBytes != 0U) {
        if ((secondBytes & 1U) != 0U) {
            firstCrc = MatrixTimes(CRC_SHIFT_POWERS[power].data(), firstCrc);
        }
        secondBytes >>= 1U;
        ++power;
    }
    return firstCrc ^ secondCrc;
}

} // namespace fxe
