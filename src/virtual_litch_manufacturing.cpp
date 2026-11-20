#include "virtual_litch_session_internal.hpp"

#ifdef LITCH_NATIVE
#include <algorithm>

#include "firmware/manufacturing_test_led_frame.hpp"
#include "foxie_embedded/frame_hash.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/led_render_policy.hpp"

namespace runtime::virtual_litch_internal {
namespace {

class ManufacturingBoardObservation final : public modules::IBoardInput {
  public:
    ManufacturingBoardObservation(bool valid, uint64_t occupied)
        : m_valid(valid), m_occupied(occupied) {}
    bool TryReadSnapshot(uint64_t& occupied) override { occupied = m_occupied; return m_valid; }
    bool TryReadDelta(modules::SquareDelta&) override { return false; }
    void Resync() override {}
  private:
    bool m_valid;
    uint64_t m_occupied;
};

} // namespace

void Session::RunManufacturingIngress(
    void (*callback)(void*, firmware::HardwareTestMode&, uint32_t), void* context) {
    fxe::ScopedMillisProviderOverride timeOverride(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(m_nowMs);
    if (callback != nullptr) { callback(context, m_hardwareTest, m_nowMs); }
    if (m_hardwareTest.IsActive()) {
        ObserveManufacturingBoard(m_manufacturingOccupancy);
    }
    CaptureFrame();
}

void Session::ObserveManufacturingBoard(uint64_t occupiedMask) {
    m_manufacturingOccupancy = occupiedMask;
    ManufacturingBoardObservation input(m_manufacturingHallValid, occupiedMask);
    m_hardwareTest.UpdateSensors(input, m_nowMs);
}

void Session::SetManufacturingHallSnapshot(bool valid, uint64_t occupiedMask) {
    m_manufacturingHallValid = valid;
    m_manufacturingOccupancy = occupiedMask;
}

void Session::CaptureManufacturingFrame() {
    firmware::ManufacturingTestFrameInputs inputs;
    inputs.revision = m_hardwareTest.GetRevision();
    inputs.step = m_hardwareTest.GetStep();
    inputs.stepElapsedMs = m_hardwareTest.GetStepElapsedMs(m_nowMs);
    inputs.runElapsedMs = m_hardwareTest.GetRunElapsedMs(m_nowMs);
    inputs.ledChannel = m_hardwareTest.GetLedChannel();
    inputs.ledScanElapsedMs = m_hardwareTest.GetLedScanElapsedMs(m_nowMs);
    inputs.automaticVerifier = m_hardwareTest.IsAutomaticVerifier();
    inputs.operatorHoldProgress = m_hardwareTest.GetOperatorHoldProgress();
    inputs.occupiedMask = m_manufacturingOccupancy;
    inputs.progress = m_hardwareTest.GetProgress();
    inputs.verifierFeedback = m_hardwareTest.GetVerifierFeedback();
    inputs.verifierFeedbackElapsedMs = m_hardwareTest.GetVerifierFeedbackElapsedMs(m_nowMs);
    const auto topology = hardware::BoardLedTopologyFor(inputs.revision);
    const uint16_t count = topology.physicalBoardLedCount + topology.physicalStatusLedCount;
    const auto frame = firmware::ComposeManufacturingTestLedFrame(inputs);
    // Production scale/gain/channel semantics with a disclosed conservative
    // power source. Actual board source sensing, GPIO, RF and storage are not simulated proof.
    const bool rgbw = inputs.revision != hardware::BoardRevision::V3_3S3;
    CaptureLedRenderer renderer(count, rgbw);
    modules::PowerBudgetPolicy policy(
        modules::led_render_policy::MakeConservativePowerBudgetPolicyConfig({true, false}));
    auto config = modules::led_render_policy::MakePipelineConfig(rgbw, 32U);
    config.outputBrightnessScale256 =
        hardware::BoardHardwareProfileFor(inputs.revision).ledOutputScale256;
    if (!rgbw) { config.channelMaxCurrentMa = 20U; }
    config.enableS3OpticalColorCalibration = false;
    config.brightnessScale01 = static_cast<float>(m_hardwareTest.GetBrightnessScale256()) / 256.0f;
    modules::BudgetAwareLedPipeline pipeline(renderer, policy, config);
    pipeline.RenderFrame(frame.data(), count,
        firmware::ManufacturingTestOutputAccent());
    const auto stats = pipeline.GetLastStats();
    ++m_frameSequence;
    m_current.semantic = {};
    m_current.semantic.now_ms = m_nowMs;
    m_current.semantic.frame_sequence = m_frameSequence;
    auto& out = m_current.physical;
    out = {};
    out.now_ms = m_nowMs;
    out.frame_sequence = m_frameSequence;
    out.led_count = count;
    out.channels_per_led = rgbw ? 4U : 3U;
    out.estimated_led_current_ma = stats.estimatedLedCurrentMa;
    out.applied_led_current_ma = stats.appliedLedCurrentMa;
    out.applied_brightness_scale_65535 = static_cast<uint16_t>(stats.appliedBrightnessScale01 *
        stats.appliedAccentScale256 / 256.0f * 65535.0f);
    out.power_limited = stats.powerLimited;
    std::copy_n(renderer.Pixels().begin(), count, out.pixels);
    uint64_t hash = fxe::Fnv1a64Update(fxe::FNV1A64_OFFSET_BASIS, &out.channels_per_led, 1U);
    for (uint16_t index = 0U; index < count; ++index) {
        const auto& pixel = out.pixels[index];
        const uint8_t channels[] = {pixel.r, pixel.g, pixel.b, pixel.w};
        hash = fxe::Fnv1a64Update(hash, channels, 4U);
    }
    out.frame_hash = hash;
    CaptureState();
    m_current.state.physical_occupied_mask = m_manufacturingOccupancy;
    AppendHistory();
}

} // namespace runtime::virtual_litch_internal
#endif
