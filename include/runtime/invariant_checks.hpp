#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "runtime/board_constants.hpp"
#include "runtime/square_mapping.hpp"

namespace runtime {

struct InvariantViolation {
    const char* name = "";
    uint32_t a = 0;
    uint32_t b = 0;
};

template <size_t MAX_VIOLATIONS>
class InvariantReport {
  public:
    bool IsOk() const { return m_count == 0; }

    size_t Count() const { return m_count; }

    const InvariantViolation& At(const size_t idx) const { return m_violations[idx]; }

    void Add(const char* name, const uint32_t a = 0, const uint32_t b = 0) {
        if (m_count >= MAX_VIOLATIONS) {
            return;
        }

        m_violations[m_count] = InvariantViolation{name ? name : "?", a, b};
        ++m_count;
    }

  private:
    std::array<InvariantViolation, MAX_VIOLATIONS> m_violations{};
    size_t m_count = 0;
};

// Basic invariant: stable square index mapping, including flip-sides.
// See docs/spec/board_coordinates.md items (1) and (2).
inline void CheckSquareIndexMappingStable(InvariantReport<8>& outReport) {
    // Non-flipped identity.
    for (uint8_t i = 0; i < BOARD_SQUARE_COUNT; ++i) {
        if (MapSquareIndex(i, false) != i) {
            outReport.Add("square_mapping_identity", i, MapSquareIndex(i, false));
            break;
        }
    }

    // Flipped is 63-i and is an involution.
    for (uint8_t i = 0; i < BOARD_SQUARE_COUNT; ++i) {
        const uint8_t mapped = MapSquareIndex(i, true);
        const uint8_t expected = static_cast<uint8_t>((BOARD_SQUARE_COUNT - 1U) - i);
        if (mapped != expected) {
            outReport.Add("square_mapping_flip", i, mapped);
            break;
        }

        if (MapSquareIndex(mapped, true) != i) {
            outReport.Add("square_mapping_flip_involution", i, mapped);
            break;
        }
    }
}

} // namespace runtime
