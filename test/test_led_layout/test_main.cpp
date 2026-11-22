#include <cstdio>

#include "hardware/led_layout.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[LED_LAYOUT] start");

    Check(hardware::SQUARE_COUNT == 64, "square count is 64");
    Check(hardware::LEDS_PER_SQUARE == 3, "leds per square is 3");
    Check(hardware::BOARD_LED_COUNT == 192, "board led count is 64*3");
    Check(hardware::FirstEdgeLedIndex() == hardware::BOARD_LED_COUNT, "edge leds start after board leds");
    Check(hardware::TOTAL_LED_COUNT == hardware::BOARD_LED_COUNT + hardware::EDGE_LED_COUNT,
          "total led count matches board+edge");

    for (uint8_t sq = 0; sq < hardware::SQUARE_COUNT; ++sq) {
        const uint16_t base = static_cast<uint16_t>(sq) * hardware::LEDS_PER_SQUARE;

        const uint16_t idx0 = hardware::LedIndexForSquare(sq, hardware::SquareLedSlot::Square0);
        const uint16_t idx1 = hardware::LedIndexForSquare(sq, hardware::SquareLedSlot::Piece);
        const uint16_t idx2 = hardware::LedIndexForSquare(sq, hardware::SquareLedSlot::Square2);

        Check(idx0 == base + 0, "square0 index is contiguous");
        Check(idx1 == base + 1, "piece index is contiguous");
        Check(idx2 == base + 2, "square2 index is contiguous");

        Check(idx2 < hardware::BOARD_LED_COUNT, "square led indices are within board range");
    }

    if (g_failures == 0) {
        std::puts("[LED_LAYOUT] ok");
        return 0;
    }

    std::printf("[LED_LAYOUT] failures=%d\n", g_failures);
    return 1;
}
