#include <array>
#include <cstdio>

#include "hardware/board_led_output_mapping.hpp"
#include "modules/brightness_scale_policy.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/edge_led_renderer.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/orientation.hpp"
#include "modules/rgbw_auto_map.hpp"
#include "modules/rgb_optical_calibration.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "modules/ui_color_presets.hpp"
#include "modules/ui_color_wheel.hpp"

namespace {

int failures = 0;

void Check(const bool value, const char* message) {
    if (!value) {
        std::printf("[FAIL] %s\n", message);
        ++failures;
    }
}

bool Equal(const modules::RgbwChannels lhs, const modules::RgbwChannels rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.w == rhs.w;
}

bool IsBlack(const modules::RgbwChannels color) {
    return color.r == 0U && color.g == 0U && color.b == 0U && color.w == 0U;
}

class CaptureRenderer final : public modules::ILedRenderer {
  public:
    CaptureRenderer(const uint16_t count, const bool rgbw, const uint8_t threshold)
        : count_(count), rgbw_(rgbw), threshold_(threshold) {}

    uint16_t GetLedCount() const override { return count_; }
    void Clear() override { pixels_.fill({}); }
    void SetLedRgb(const uint16_t index, const uint8_t r, const uint8_t g, const uint8_t b) override {
        if (index < count_) {
            pixels_[index] = rgbw_ ? modules::AutoMapRgbToRgbw({r, g, b}, threshold_)
                                   : modules::RgbwChannels{r, g, b, 0U};
        }
    }
    void Show() override {}
    const auto& Pixels() const { return pixels_; }

  private:
    uint16_t count_;
    bool rgbw_;
    uint8_t threshold_;
    std::array<modules::RgbwChannels, hardware::MAX_PHYSICAL_LED_COUNT> pixels_{};
};

struct RenderResult {
    modules::BudgetAwareLedPipelineStats stats{};
    std::array<modules::RgbwChannels, hardware::MAX_PHYSICAL_LED_COUNT> pixels{};
    uint16_t count = 0U;
};

std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT> MapFrame(
    const hardware::BoardRevision revision,
    const bool squareRotate,
    const bool boardRotate,
    const modules::Rgb squareColor,
    const modules::Rgb ownerColor,
    const modules::Rgb statusColor) {
    std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT> physical{};
    const auto topology = hardware::BoardLedTopologyFor(revision);
    // An asymmetric nonblack edge sentinel catches square rotation separately
    // from the status-chain half-ring rotation.
    constexpr uint8_t sentinelSquare = 2U;
    const auto squareTargets = hardware::MapSemanticSquareLedTargets(
        revision, sentinelSquare, hardware::SquareLedSlot::Square0, squareRotate);
    for (uint8_t i = 0U; i < squareTargets.count; ++i) {
        physical[squareTargets.indices[i]] = squareColor;
    }
    const auto ownerTargets = hardware::MapSemanticSquareLedTargets(
        revision, 60U, hardware::SquareLedSlot::Piece, squareRotate);
    for (uint8_t i = 0U; i < ownerTargets.count; ++i) {
        physical[ownerTargets.indices[i]] = ownerColor;
    }

    constexpr uint8_t statusSentinel = 3U;
    if (topology.physicalStatusLedCount == 0U) {
        const uint8_t statusIndex = modules::status_led_frame_adapter::
            PhysicalIndexForSemanticIndex(statusSentinel, boardRotate);
        const uint16_t target = modules::status_led_frame_adapter::
            VirtualLedIndexForPhysicalStatusIndex(statusIndex);
        physical[target] = modules::status_led_frame_adapter::BlendVirtualStatus(
            physical[target], statusColor);
    } else {
        const uint16_t index = static_cast<uint16_t>(
            topology.physicalBoardLedCount + modules::status_led_frame_adapter::
                PhysicalIndexForSemanticIndex(statusSentinel, boardRotate));
        physical[index] = statusColor;
    }
    return physical;
}

RenderResult Render(const hardware::BoardRevision revision,
                    const std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT>& frame,
                    const uint16_t scale256,
                    const bool normalBudget) {
    const auto topology = hardware::BoardLedTopologyFor(revision);
    const bool rgbw = revision != hardware::BoardRevision::V1C3;
    // The setting is a runtime input to the production factory; zero is a
    // supported setting value and avoids copying a firmware default here.
    auto cfg = modules::led_render_policy::MakePipelineConfig(rgbw, 0U);
    auto policyCfg = modules::led_render_policy::MakePowerBudgetPolicyConfig(
        {rgbw, !normalBudget});
    modules::PowerBudgetPolicy policy(policyCfg);
    CaptureRenderer renderer(static_cast<uint16_t>(topology.physicalBoardLedCount +
                                                   topology.physicalStatusLedCount),
                             rgbw, cfg.rgbwCoolToWarmThreshold);
    modules::BudgetAwareLedPipeline pipeline(renderer, policy, cfg);
    pipeline.SetBrightnessScale01(static_cast<float>(scale256) / 256.0f);
    pipeline.RenderFrame(frame.data(), renderer.GetLedCount());
    return {pipeline.GetLastStats(), renderer.Pixels(), renderer.GetLedCount()};
}

uint16_t StatusTarget(const hardware::BoardRevision revision,
                      const bool boardRotate) {
    const auto topology = hardware::BoardLedTopologyFor(revision);
    constexpr uint8_t semanticStatus = 3U;
    const uint8_t status = modules::status_led_frame_adapter::
        PhysicalIndexForSemanticIndex(semanticStatus, boardRotate);
    if (topology.physicalStatusLedCount == 0U) {
        return modules::status_led_frame_adapter::
            VirtualLedIndexForPhysicalStatusIndex(status);
    }
    return static_cast<uint16_t>(topology.physicalBoardLedCount + status);
}

std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT> MapStatusFrame(
    const hardware::BoardRevision revision,
    const bool boardRotate,
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& semantic) {
    std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT> physical{};
    const auto topology = hardware::BoardLedTopologyFor(revision);
    for (uint8_t semanticIndex = 0U;
         semanticIndex < modules::EDGE_LED_COUNT;
         ++semanticIndex) {
        const uint8_t statusIndex = modules::status_led_frame_adapter::
            PhysicalIndexForSemanticIndex(semanticIndex, boardRotate);
        if (topology.physicalStatusLedCount == 0U) {
            const uint16_t target = modules::status_led_frame_adapter::
                VirtualLedIndexForPhysicalStatusIndex(statusIndex);
            physical[target] = modules::status_led_frame_adapter::
                BlendVirtualStatus(physical[target], semantic[semanticIndex]);
        } else {
            physical[static_cast<uint16_t>(
                topology.physicalBoardLedCount + statusIndex)] =
                semantic[semanticIndex];
        }
    }
    return physical;
}

uint16_t CountLit(
    const std::array<modules::RgbwChannels,
                     hardware::MAX_PHYSICAL_LED_COUNT>& pixels,
    const uint16_t count) {
    uint16_t lit = 0U;
    for (uint16_t i = 0U; i < count; ++i) {
        if (!IsBlack(pixels[i])) {
            ++lit;
        }
    }
    return lit;
}

bool AnyDifferent(
    const std::array<modules::RgbwChannels,
                     hardware::MAX_PHYSICAL_LED_COUNT>& lhs,
    const std::array<modules::RgbwChannels,
                     hardware::MAX_PHYSICAL_LED_COUNT>& rhs,
    const uint16_t count) {
    for (uint16_t i = 0U; i < count; ++i) {
        if (!Equal(lhs[i], rhs[i])) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    using modules::Rgb;
    using modules::brightness_scale_policy::ComputeEffectiveScale256;
    std::puts("[LOW_BRIGHTNESS_CHARACTERIZATION] start");

    for (uint8_t level = 1U; level <= 8U; ++level) {
        const uint16_t scale = ComputeEffectiveScale256(level, false, 0.0f, 256U);
        Check(scale == ComputeEffectiveScale256(level, false, 0.0f, 256U),
              "brightness level scale is deterministic");
    }
    Check(ComputeEffectiveScale256(1U, true, 0.0f, 256U) == 12U &&
              ComputeEffectiveScale256(1U, true, 1.0f, 256U) ==
                  29U,
          "ambient endpoints preserve dark fallback and chosen level-one ceiling");
    Check(ComputeEffectiveScale256(8U, false, 0.0f, 0U) == 0U &&
              ComputeEffectiveScale256(8U, false, 0.0f, 256U) ==
                  modules::brightness_scale_policy::AUTOMATIC_BASE_MAX_SCALE256,
          "fade endpoints intentionally span black through the user cap");

    // This is a production-pipeline characterization for V3. The V2 journey
    // is covered by test_near_dark_virtual_litch_journey through VirtualLitch.
    constexpr std::array<uint8_t, 9> expectedOrdinaryCodes{
        1U, 1U, 1U, 1U, 2U, 2U, 3U, 3U, 4U};
    constexpr std::array<uint8_t, 4> representativeWheels{
        147U, 213U, 140U, 101U};
    constexpr std::array<Rgb, 4> expectedPieceSemantic{
        Rgb{0U, 103U, 255U}, Rgb{191U, 0U, 188U},
        Rgb{0U, 134U, 245U}, Rgb{0U, 255U, 71U}};
    const Rgb ordinarySemantic = modules::ScaleRgbSaturatingBy256(
        modules::ui_color_presets::SQUARE_COLOR_PRESETS[0], 320U);
    Check(ordinarySemantic.r == 16U && ordinarySemantic.g == 16U &&
              ordinarySemantic.b == 16U,
          "ordinary light-square preset reaches semantic code 16");
    for (size_t wheelIndex = 0U;
         wheelIndex < representativeWheels.size(); ++wheelIndex) {
        const Rgb wheelSemantic = modules::ScaleRgbSaturatingBy256(
            modules::ScaleRgbSaturatingBy256(
                modules::ui_color_wheel::WheelToRgb(
                    representativeWheels[wheelIndex]),
                304U),
            320U);
        Check(wheelSemantic.r == expectedPieceSemantic[wheelIndex].r &&
                  wheelSemantic.g == expectedPieceSemantic[wheelIndex].g &&
                  wheelSemantic.b == expectedPieceSemantic[wheelIndex].b,
              "representative piece semantic code remains unchanged");
        for (uint8_t level = 0U; level <= 8U; ++level) {
            const auto frame = MapFrame(
                hardware::BoardRevision::V3S3, false, false,
                ordinarySemantic, expectedPieceSemantic[wheelIndex],
                modules::RGB_BLACK);
            const uint16_t scale = ComputeEffectiveScale256(
                level, false, 0.0f, 256U);
            const auto result = Render(
                hardware::BoardRevision::V3S3, frame, scale, true);
            const auto backgroundTargets =
                hardware::MapSemanticSquareLedTargets(
                    hardware::BoardRevision::V3S3, 2U,
                    hardware::SquareLedSlot::Square0, false);
            const auto ownerTargets = hardware::MapSemanticSquareLedTargets(
                hardware::BoardRevision::V3S3, 60U,
                hardware::SquareLedSlot::Piece, false);
            const uint8_t expectedCode = expectedOrdinaryCodes[level];
            Check(result.stats.decision.ledBrightnessScale01 == 1.0f &&
                      backgroundTargets.count > 0U &&
                      Equal(result.pixels[backgroundTargets.indices[0]],
                            modules::RgbwChannels{
                                expectedCode, expectedCode,
                                expectedCode, expectedCode}),
                  "unthrottled V3 background retains the characterized 0-8 code");
            const Rgb expectedOwnerRgb = modules::ScaleRgbSaturatingBy256(
                expectedPieceSemantic[wheelIndex], scale);
            const Rgb expectedPhysicalOwnerRgb =
                modules::rgb_optical_calibration::PresentForS3(expectedOwnerRgb);
            Check(ownerTargets.count > 0U &&
                      Equal(result.pixels[ownerTargets.indices[0]],
                            modules::AutoMapRgbToRgbw(expectedPhysicalOwnerRgb,
                                                     0U)),
                  "unthrottled V3 calibrated piece code is unchanged by the background preset");
        }
    }

    const std::array<Rgb, 4> colors = {
        Rgb{9U, 2U, 1U}, Rgb{48U, 8U, 3U}, Rgb{255U, 255U, 255U}, Rgb{184U, 184U, 188U}};
    const std::array<hardware::BoardRevision, 3> revisions = {
        hardware::BoardRevision::V1C3, hardware::BoardRevision::V2S3, hardware::BoardRevision::V3S3};
    uint16_t summaries = 0U;
    for (const auto revision : revisions) {
        for (const bool boardRotate : {false, true}) {
            for (const bool firstPlayerIsP2 : {false, true}) {
                const bool squareRotate = modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2);
                for (const bool normalBudget : {false, true}) {
                    const auto frame = MapFrame(revision, squareRotate, boardRotate,
                                                colors[0], colors[1], colors[3]);
                    const auto result = Render(revision, frame,
                                               ComputeEffectiveScale256(1U, true, 0.0f, 256U),
                                               normalBudget);
                    Check(result.stats.appliedLedCurrentMa <= result.stats.decision.allowedLedCurrentMa,
                          "final quantized physical frame remains within its policy limit");
                    const auto owner = hardware::MapSemanticSquareLedTargets(
                        revision, 60U, hardware::SquareLedSlot::Piece, squareRotate);
                    Check(owner.count > 0U && result.pixels[owner.indices[0]].r != 0U,
                          "owner sentinel survives mapping, brightness, and power limiting");
                    if (!boardRotate && !firstPlayerIsP2 && !normalBudget) {
                        const auto code = result.pixels[owner.indices[0]];
                        std::printf("[CHAR] rev=%u board=%u firstP2=%u budget=%s code=%u,%u,%u,%u current=%u\n",
                                    static_cast<unsigned>(revision), boardRotate, firstPlayerIsP2,
                                    normalBudget ? "normal" : "protection", code.r, code.g, code.b, code.w,
                                    result.stats.appliedLedCurrentMa);
                        ++summaries;
                    }
                }
            }
        }
    }

    for (const auto revision : revisions) {
        const uint16_t lowScale =
            ComputeEffectiveScale256(1U, true, 0.0f, 256U);
        for (uint8_t level = 1U; level <= 8U; ++level) {
            const auto frame = MapFrame(revision, false, false,
                                        colors[0], colors[1], colors[3]);
            const auto result = Render(
                revision, frame,
                ComputeEffectiveScale256(level, false, 0.0f, 256U), false);
            const uint16_t ownerTarget = hardware::MapSemanticSquareLedTargets(
                revision, 60U, hardware::SquareLedSlot::Piece, false).indices[0];
            Check(Equal(result.pixels[ownerTarget],
                        Render(revision, frame,
                               ComputeEffectiveScale256(level, false, 0.0f, 256U),
                               false).pixels[ownerTarget]),
                  "final level output is deterministic");
        }

        const auto normal = MapFrame(revision, false, false, colors[0], colors[1], colors[3]);
        const auto squareOnly = MapFrame(revision, true, false, colors[0], colors[1], colors[3]);
        const auto statusRotated = MapFrame(revision, false, true, colors[0], colors[1], colors[3]);
        const auto normalFinal = Render(revision, normal, lowScale, false);
        const auto squareOnlyFinal = Render(revision, squareOnly, lowScale, false);
        const auto statusRotatedFinal = Render(revision, statusRotated, lowScale, false);
        const uint16_t normalTarget = StatusTarget(revision, false);
        const uint16_t rotatedTarget = StatusTarget(revision, true);
        Check(!IsBlack(normalFinal.pixels[normalTarget]) &&
                  Equal(normalFinal.pixels[normalTarget],
                        squareOnlyFinal.pixels[normalTarget]),
              "first-player square rotation does not rotate the status sentinel");
        Check(normalTarget != rotatedTarget &&
                  !IsBlack(statusRotatedFinal.pixels[rotatedTarget]) &&
                  !Equal(normalFinal.pixels[normalTarget],
                         statusRotatedFinal.pixels[normalTarget]),
                  "final post-budget board rotation shifts the C3 virtual or S3 dedicated status sentinel");
    }

    modules::EdgeLedRenderer healthRenderer;
    modules::EdgeLedState healthState;
    healthState.mode = modules::EdgeUiMode::HealthPaused;
    std::array<Rgb, modules::EDGE_LED_COUNT> healthPhaseA{};
    std::array<Rgb, modules::EDGE_LED_COUNT> healthPhaseB{};
    healthState.nowMs = 1600U;
    healthRenderer.Render(healthState, healthPhaseA);
    healthState.nowMs = 2400U;
    healthRenderer.Render(healthState, healthPhaseB);
    const uint16_t minimumNonzeroScale =
        ComputeEffectiveScale256(1U, true, 0.0f, 256U);
    for (const auto revision : revisions) {
        for (const bool boardRotate : {false, true}) {
            const auto mappedA = MapStatusFrame(
                revision, boardRotate, healthPhaseA);
            const auto mappedB = MapStatusFrame(
                revision, boardRotate, healthPhaseB);
            const auto renderedA = Render(
                revision, mappedA, minimumNonzeroScale, false);
            const auto renderedB = Render(
                revision, mappedB, minimumNonzeroScale, false);
            const auto renderedOff = Render(revision, mappedA, 0U, false);
            Check(CountLit(renderedA.pixels, renderedA.count) == 16U &&
                      CountLit(renderedB.pixels, renderedB.count) == 16U,
                  "paused health dashes survive final minimum-output mapping");
            Check(AnyDifferent(renderedA.pixels, renderedB.pixels,
                               renderedA.count),
                  "paused health cadence survives final output quantization");
            Check(CountLit(renderedOff.pixels, renderedOff.count) == 0U,
                  "paused health remains honestly black at zero output");
        }
    }

    healthState.mode = modules::EdgeUiMode::HealthDegradedUsable;
    for (const auto revision : revisions) {
        for (const bool boardRotate : {false, true}) {
            healthState.nowMs = 0U;
            std::array<Rgb, modules::EDGE_LED_COUNT> usablePhaseA{};
            healthRenderer.Render(healthState, usablePhaseA);
            healthState.nowMs = 250U;
            std::array<Rgb, modules::EDGE_LED_COUNT> usablePhaseB{};
            healthRenderer.Render(healthState, usablePhaseB);
            const auto mappedA = MapStatusFrame(
                revision, boardRotate, usablePhaseA);
            const auto mappedB = MapStatusFrame(
                revision, boardRotate, usablePhaseB);
            const auto renderedA = Render(
                revision, mappedA, minimumNonzeroScale, false);
            const auto renderedB = Render(
                revision, mappedB, minimumNonzeroScale, false);
            const auto renderedOff = Render(revision, mappedA, 0U, false);
            Check(CountLit(renderedA.pixels, renderedA.count) == 1U &&
                      CountLit(renderedB.pixels, renderedB.count) == 1U,
                  "usable health USB-side marker survives V1/V2/V3 final mapping");
            Check(AnyDifferent(renderedA.pixels, renderedB.pixels,
                               renderedA.count),
                  "usable health yellow/orange alternation survives final output quantization");
            Check(CountLit(renderedOff.pixels, renderedOff.count) == 0U,
                  "usable health remains honestly black at zero output");
        }
    }

    auto v2 = MapFrame(hardware::BoardRevision::V2S3, false, false,
                       colors[2], colors[3], colors[1]);
    auto v3 = MapFrame(hardware::BoardRevision::V3S3, false, false,
                       colors[2], colors[3], colors[1]);
    v2.fill(colors[2]);
    v3.fill(colors[3]);
    const uint16_t hardwareBright = ComputeEffectiveScale256(1U, true, 1.0f, 256U);
    const auto constrained = Render(hardware::BoardRevision::V3S3, v3, hardwareBright, false);
    const auto open = Render(hardware::BoardRevision::V2S3, v2, hardwareBright, true);
    Check(constrained.stats.decision.allowedLedCurrentMa == modules::S3_CONSERVATIVE_LED_HEADROOM_MA,
          "brownout protection uses constrained physical LED headroom");
    Check(open.stats.decision.allowedLedCurrentMa <= modules::S3_MAX_LED_CURRENT_MA,
          "normal S3 budget remains bounded by the measured LED ceiling");
    const auto v3Owner = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3S3, 60U, hardware::SquareLedSlot::Piece, false);
    Check(v3Owner.count > 0U && !Equal(constrained.pixels[v3Owner.indices[0]], modules::RgbwChannels{}),
          "representative near-neutral V3 output has a final RGBW code");

    std::printf("[LOW_BRIGHTNESS_CHARACTERIZATION] %s; %u compact samples; code separation is measurement evidence, not dark-room legibility\n",
                failures == 0 ? "ok" : "failed", summaries);
    return failures == 0 ? 0 : 1;
}
