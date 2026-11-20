#include "modules/ota_led_presentation.hpp"

#include "foxie_embedded/millis.hpp"

namespace modules {

void ComposeOtaLedPresentation(
    const OtaLedPresentation presentation,
    BoardLedFrame& board) {
    board.fill(BoardSquareLeds{});
    if (!presentation.active) {
        return;
    }

    // A circulating amber pulse indicates activity; steady purple edge LEDs
    // carry byte progress. Neither cue depends on reading board orientation.
    constexpr std::array<uint8_t, 12> ring = {
        18U, 19U, 20U, 21U, 29U, 37U, 45U, 44U, 43U, 42U, 34U, 26U};
    constexpr uint32_t STEP_MS = 200U;
    const uint32_t phase = fxe::Millis() % (STEP_MS * ring.size());
    const uint8_t head = phase / STEP_MS;
    const uint16_t fraction = (phase % STEP_MS) * 256U / STEP_MS;
    for (uint8_t i = 0U; i < ring.size(); ++i) {
        const uint8_t behind = (head + ring.size() - i) % ring.size();
        const uint16_t level = behind == 0U ? 256U - fraction / 2U
                             : behind == 1U ? 128U - fraction / 4U
                             : behind == 2U ? 64U - fraction / 4U
                             : behind == ring.size() - 1U ? fraction : 0U;
        const auto color = ScaleRgbSaturatingBy256(Rgb{255U, 96U, 0U}, level);
        board[ring[i]].left = color;
        board[ring[i]].right = color;
    }
}

void ApplyOtaPhysicalStatusPresentation(
    const OtaLedPresentation presentation,
    const hardware::BoardRevision revision,
    hardware::PhysicalLedFrame& physical) {
    if (!presentation.active) {
        return;
    }
    const auto topology = hardware::BoardLedTopologyFor(revision);
    const uint16_t usable = topology.physicalStatusLedCount != 0U
                                ? modules::EDGE_LED_COUNT : 0U;
    const uint16_t lit = (presentation.percent * usable) / 100U;
    for (uint8_t status = 0U; status < lit; ++status) {
        hardware::ApplyFixedPhysicalStatusPixel(
            revision, status, Rgb{112U, 0U, 255U}, physical);
    }
}

} // namespace modules
