#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "firmware/sleep_countdown_led_frame.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "hardware/led_layout.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/status_led_frame_adapter.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

bool IsBlack(const modules::Rgb color) {
    return color.r == 0U && color.g == 0U && color.b == 0U;
}

size_t CountLit(const firmware::SleepCountdownLedFrame& frame) {
    size_t count = 0U;
    for (const auto color : frame.semanticStatus) {
        if (!IsBlack(color)) {
            ++count;
        }
    }
    return count;
}

class FrameRenderer final : public modules::ILedRenderer {
  public:
    uint16_t GetLedCount() const override {
        return static_cast<uint16_t>(pixels.size());
    }
    void Clear() override { pixels.fill(modules::RGB_BLACK); }
    void SetLedRgb(const uint16_t index,
                   const uint8_t r,
                   const uint8_t g,
                   const uint8_t b) override {
        if (index < pixels.size()) {
            pixels[index] = modules::Rgb{r, g, b};
        }
    }
    void Show() override {}

    std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT> pixels{};
};

void CheckSemanticGathering() {
    const auto initial = firmware::ComposeSleepCountdownLedFrame(
        firmware::SleepState::Countdown,
        firmware::SleepTransition::DEFAULT_COUNTDOWN_MS);
    const auto middle = firmware::ComposeSleepCountdownLedFrame(
        firmware::SleepState::Countdown, 1500U);
    const auto final = firmware::ComposeSleepCountdownLedFrame(
        firmware::SleepState::Countdown, 1U);
    const auto preparing = firmware::ComposeSleepCountdownLedFrame(
        firmware::SleepState::Preparing, 0U);

    Check(initial.active && initial.gatheredLedCount == 3U,
          "countdown starts in a fixed informative spatial phase");
    Check(CountLit(initial) == initial.gatheredLedCount,
          "initial semantic count matches emitted pixels");
    Check(middle.gatheredLedCount > initial.gatheredLedCount &&
              middle.gatheredLedCount < modules::EDGE_LED_COUNT,
          "perimeter gathers monotonically during countdown");
    Check(final.gatheredLedCount == modules::EDGE_LED_COUNT &&
              CountLit(final) == modules::EDGE_LED_COUNT,
          "one millisecond remaining gathers the complete perimeter");
    Check(preparing.active && CountLit(preparing) == modules::EDGE_LED_COUNT,
          "preparing retains full ring semantics behind the completed black fade");
    for (const auto state : {firmware::SleepState::Fading,
                             firmware::SleepState::Preparing}) {
        const auto appFade = firmware::ComposeSleepCountdownLedFrame(state, 0U, 0U);
        Check(!appFade.active && CountLit(appFade) == 0U,
              "direct app fade never replaces the current frame with an orange perimeter");
    }
}

void CheckC3VirtualPerimeterAndRotation() {
    const auto cue = firmware::ComposeSleepCountdownLedFrame(
        firmware::SleepState::Countdown, 2200U);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> normal{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> rotated{};
    normal.fill(modules::RGB_BLACK);
    rotated.fill(modules::RGB_BLACK);

    const uint16_t protectedPiece = hardware::LedIndexForSquare(
        7U, hardware::SquareLedSlot::Piece);
    normal[protectedPiece] = modules::Rgb{70U, 0U, 0U};
    rotated[protectedPiece] = modules::Rgb{70U, 0U, 0U};
    modules::status_led_frame_adapter::ApplyToVirtualPerimeter(
        cue.semanticStatus, false, normal);
    modules::status_led_frame_adapter::ApplyToVirtualPerimeter(
        cue.semanticStatus, true, rotated);

    const uint16_t semanticZeroNormal =
        modules::status_led_frame_adapter::VirtualLedIndexForPhysicalStatusIndex(
            0U);
    const uint16_t semanticZeroRotated =
        modules::status_led_frame_adapter::VirtualLedIndexForPhysicalStatusIndex(
            16U);
    Check(!IsBlack(normal[semanticZeroNormal]),
          "C3 cue reaches the unrotated virtual perimeter");
    Check(!IsBlack(rotated[semanticZeroRotated]),
          "C3 cue rotates by half a semantic ring");
    Check(IsBlack(rotated[semanticZeroNormal]),
          "asymmetric cue proves rotation changed the physical target");
    Check(normal[protectedPiece].r == 70U &&
              rotated[protectedPiece].r == 70U,
          "virtual perimeter leaves normal piece LEDs intact");
}

void CheckS3DedicatedPerimeters() {
    const auto cue = firmware::ComposeSleepCountdownLedFrame(
        firmware::SleepState::Countdown, 2200U);
    const std::array<hardware::BoardRevision, 2U> revisions = {
        hardware::BoardRevision::V2S3,
        hardware::BoardRevision::V3S3,
    };

    for (const auto revision : revisions) {
        const auto topology = hardware::BoardLedTopologyFor(revision);
        std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT> normal{};
        std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT> rotated{};
        for (uint8_t semantic = 0U;
             semantic < modules::EDGE_LED_COUNT;
             ++semantic) {
            const uint8_t normalStatus =
                modules::status_led_frame_adapter::PhysicalIndexForSemanticIndex(
                    semantic, false);
            const uint8_t rotatedStatus =
                modules::status_led_frame_adapter::PhysicalIndexForSemanticIndex(
                    semantic, true);
            normal[static_cast<uint16_t>(topology.physicalBoardLedCount +
                                         normalStatus)] =
                cue.semanticStatus[semantic];
            rotated[static_cast<uint16_t>(topology.physicalBoardLedCount +
                                          rotatedStatus)] =
                cue.semanticStatus[semantic];
        }

        Check(!IsBlack(normal[topology.physicalBoardLedCount]),
              "S3 semantic zero reaches its dedicated physical perimeter");
        Check(!IsBlack(rotated[static_cast<uint16_t>(
                  topology.physicalBoardLedCount + 16U)]),
              "S3 dedicated perimeter rotates by half a ring");
        Check(IsBlack(rotated[topology.physicalBoardLedCount]),
              "S3 rotation moves the asymmetric countdown anchor");
    }
}

void CheckBudgetAndZeroOutputHonesty() {
    const auto cue = firmware::ComposeSleepCountdownLedFrame(
        firmware::SleepState::Countdown, 1U);
    std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT> physical{};
    const auto topology =
        hardware::BoardLedTopologyFor(hardware::BoardRevision::V2S3);
    for (uint8_t semantic = 0U;
         semantic < modules::EDGE_LED_COUNT;
         ++semantic) {
        physical[static_cast<uint16_t>(topology.physicalBoardLedCount +
                                       semantic)] =
            cue.semanticStatus[semantic];
    }

    modules::PowerBudgetPolicyConfig zeroBudget;
    zeroBudget.maxLedCurrentMa = 0U;
    zeroBudget.maxTotalCurrentMa = 0U;
    modules::PowerBudgetPolicy zeroPolicy(zeroBudget);
    FrameRenderer renderer;
    modules::BudgetAwareLedPipeline zeroPipeline(renderer, zeroPolicy);
    zeroPipeline.RenderFrame(
        physical.data(),
        static_cast<size_t>(topology.physicalBoardLedCount +
                            topology.physicalStatusLedCount));
    for (const auto pixel : renderer.pixels) {
        Check(IsBlack(pixel),
              "zero power budget honestly emits no forced countdown pixel");
    }
    Check(zeroPipeline.GetLastStats().appliedLedCurrentMa == 0U,
          "zero-budget stats report zero applied current");

    modules::PowerBudgetPolicyConfig tightBudget;
    tightBudget.maxLedCurrentMa = 1U;
    tightBudget.maxTotalCurrentMa = 1U;
    modules::PowerBudgetPolicy tightPolicy(tightBudget);
    modules::BudgetAwareLedPipeline tightPipeline(renderer, tightPolicy);
    tightPipeline.RenderFrame(
        physical.data(),
        static_cast<size_t>(topology.physicalBoardLedCount +
                            topology.physicalStatusLedCount));
    Check(tightPipeline.GetLastStats().appliedLedCurrentMa <= 1U,
          "power-limited countdown stays within the final output budget");

    modules::PowerBudgetPolicyConfig openBudget;
    modules::PowerBudgetPolicy openPolicy(openBudget);
    modules::BudgetAwareLedPipeline zeroBrightness(renderer, openPolicy);
    zeroBrightness.SetBrightnessScale01(0.0F);
    zeroBrightness.RenderFrame(
        physical.data(),
        static_cast<size_t>(topology.physicalBoardLedCount +
                            topology.physicalStatusLedCount));
    for (const auto pixel : renderer.pixels) {
        Check(IsBlack(pixel),
              "zero brightness stays black without an arbitrary cue floor");
    }
}

} // namespace

int main() {
    std::puts("[SLEEP_COUNTDOWN_LED_FRAME] start");
    CheckSemanticGathering();
    CheckC3VirtualPerimeterAndRotation();
    CheckS3DedicatedPerimeters();
    CheckBudgetAndZeroOutputHonesty();

    if (g_failures == 0) {
        std::puts("[SLEEP_COUNTDOWN_LED_FRAME] ok");
        return 0;
    }
    std::printf("[SLEEP_COUNTDOWN_LED_FRAME] failures=%d\n", g_failures);
    return 1;
}
