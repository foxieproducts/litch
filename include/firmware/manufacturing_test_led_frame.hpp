#pragma once

#include <array>
#include <cstdint>

#include "firmware/hardware_test_mode.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/manufacturing_test_session.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/rgb.hpp"

namespace firmware {

using ManufacturingTestLedFrame =
    std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT>;

struct ManufacturingTestFrameInputs {
    hardware::BoardRevision revision = hardware::BoardRevision::Unknown;
    HardwareTestMode::Step step = HardwareTestMode::Step::LedRainbow;
    uint32_t runElapsedMs = 0U;
    uint32_t stepElapsedMs = 0U;
    uint32_t ledScanElapsedMs = 0U;
    uint8_t ledChannel = 0U;
    bool automaticVerifier = false;
    uint8_t operatorHoldProgress = 0U;
    uint64_t occupiedMask = 0ULL;
    modules::ManufacturingTestProgress progress{};
    HardwareTestMode::VerifierFeedback verifierFeedback =
        HardwareTestMode::VerifierFeedback::Waiting;
    uint32_t verifierFeedbackElapsedMs = 0U;
};

// Reuse the shared pipeline's bounded post-baseline accent for every fitted
// factory emitter. Pre-budget scaling alone cannot dim current-limited fields.
inline modules::LedFrameAccent ManufacturingTestOutputAccent() {
    static constexpr auto mask = []() {
        std::array<uint8_t, hardware::MAX_PHYSICAL_LED_COUNT> pixels{};
        for (auto& pixel : pixels) pixel = 1U;
        return pixels;
    }();
    return {mask.data(), mask.size(), 192U};
}

ManufacturingTestLedFrame ComposeManufacturingTestLedFrame(
    const ManufacturingTestFrameInputs& inputs);

} // namespace firmware
