#include "firmware/led_animation_renderer.hpp"

#include <algorithm>

#include "firmware/led_brightness_attempt.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/neopixel_led_renderer.hpp"
#include "modules/brightness_scale_policy.hpp"
#include "modules/led_render_policy.hpp"

namespace firmware {

void LedAnimationRenderer::Bind(hardware::NeoPixelLedRenderer& output,
    const hardware::BoardHardwareProfile& profile,
    const modules::litchcraft_presentation::StreamPort& artwork) {
    m_output = &output;
    m_profile = &profile;
    m_artworkPort = artwork;
}
void LedAnimationRenderer::InitializeBrightness(
    const modules::AmbientLuxAverager& ambient,
    const modules::UpwardOutputSlew& source) {
    m_ambientConfig = ambient.Config();
    m_ambient = ambient.CaptureSmoothing();
    m_sourceLimit = source;
}
uint16_t LedAnimationRenderer::Brightness(const LedOutputPresentation& presentation,
                                         const uint32_t nowMs) {
    const float smoothed = modules::AmbientLuxAverager::SampleSmoothing(m_ambientConfig, m_ambient,
        nowMs, presentation.rawAmbientFactor01);
    return modules::brightness_scale_policy::ComputeEffectiveScale256(
        presentation.brightnessLevel, presentation.ambientUsable,
        presentation.ambientUsable ? smoothed : 0.0F,
        presentation.bootFade.GetFadeScale256(), presentation.autoBrightness);
}
bool LedAnimationRenderer::Output(const hardware::PhysicalLedFrame& frame,
    const LedOutputPresentation& presentation, const uint16_t scale256,
    const modules::LedFrameAccent& accent, const bool useSourceSlew) {
    if (m_output == nullptr) return false;
    auto config = presentation.power;
    if (useSourceSlew && m_profile != nullptr && m_profile->isS3) {
        if (m_sourceResetEpoch != presentation.sourceResetEpoch) {
            m_sourceLimit.Reset(presentation.sourceResetLimitMa, fxe::Millis());
            m_sourceResetEpoch = presentation.sourceResetEpoch;
        }
        config.maxTotalCurrentMa = m_sourceLimit.Update(
            config.maxTotalCurrentMa, fxe::Millis());
    }
    modules::PowerBudgetPolicy policy(config);
    auto pipelineConfig = presentation.pipeline;
    if (!presentation.exactOpticalTest)
        pipelineConfig.rgbwWhiteMixScale256 = m_crossfade.WhiteMix(presentation.canvasVisible);
    m_output->SetRgbwCoolToWarmThreshold(pipelineConfig.rgbwCoolToWarmThreshold);
    m_output->SetRgbwWhiteMixScale256(pipelineConfig.rgbwWhiteMixScale256);
    modules::BudgetAwareLedPipeline pipeline(*m_output, policy, pipelineConfig);
    pipeline.SetBrightnessScale01(static_cast<float>(scale256) / 256.0F);
    pipeline.RenderFrame(frame.data(), m_output->GetLedCount(), accent);
    if (m_output->LastShowCompleted()) {
        m_powerStats = pipeline.GetLastStats();
        const uint16_t headroom = config.maxTotalCurrentMa > config.fixedSystemCurrentMa
            ? config.maxTotalCurrentMa - config.fixedSystemCurrentMa : 0U;
        m_currentLimitMa = std::min(config.maxLedCurrentMa, headroom);
    }
    return m_output->LastShowCompleted();
}
bool LedAnimationRenderer::RenderPhysical(const PhysicalLedCommand& command) {
    m_boot.Cancel();
    m_startupAccent.Cancel();
    LedBrightnessAttempt attempt(m_ambient, m_sourceLimit);
    const uint32_t sourceEpochBefore = m_sourceResetEpoch;
    const modules::LedFrameAccent accent{command.accentMask.data(),
        command.accentMask.size(), command.accentScale256,
        command.accentCeilingScale256};
    const bool completed = attempt.Finish(Output(command.frame, command.output,
        command.brightnessScale256, accent, command.useSourceSlew));
    if (!completed) m_sourceResetEpoch = sourceEpochBefore;
    return completed;
}
bool LedAnimationRenderer::RenderRawDie(const RawDieLedCommand& command) {
    m_boot.Cancel();
    m_startupAccent.Cancel();
    if (m_output == nullptr) return false;
    m_output->Clear();
    for (uint16_t pixel = 0U; pixel < m_output->GetLedCount(); ++pixel) {
        const uint8_t red = command.channel == 0U ? command.value : 0U;
        const uint8_t green = command.channel == 1U ? command.value : 0U;
        const uint8_t blue = command.channel == 2U ? command.value : 0U;
        if (command.channel == 3U) m_output->SetLedRgbwRaw(pixel, 0U, 0U, 0U, command.value);
        else m_output->SetLedRgb(pixel, red, green, blue);
    }
    m_output->Show();
    if (m_output->LastShowCompleted() && command.value == 0U) m_powerStats = {};
    return m_output->LastShowCompleted();
}
bool LedAnimationRenderer::Quiesce() {
    m_boot.Cancel();
    m_startupAccent.Cancel();
    if (m_output == nullptr) return true;
    m_output->Clear();
    m_output->Show();
    // Failure leaves the existing RMT quarantine and power rail intact.
    return m_output->LastShowCompleted();
}
void LedAnimationRenderer::CancelArtwork() {
    if (!m_artworkSceneActive) return;
    m_artwork.Reset(m_artworkPort);
    m_artworkSceneActive = false;
}
void LedAnimationRenderer::NoteArtworkPresented(
    modules::litchcraft_presentation::Feedback& feedback) {
    if (m_artworkSceneActive && feedback.ready &&
        !m_artwork.NotePresented(feedback, m_artworkPort))
        feedback.ready = false;
}
hardware::LedDeliverySnapshot LedAnimationRenderer::Delivery() const {
    return m_output != nullptr ? m_output->GetDeliverySnapshot()
                               : hardware::LedDeliverySnapshot{};
}
bool LedAnimationRenderer::OutputEnabled() const {
    return m_output != nullptr && m_output->IsOutputEnabled();
}
void LedAnimationRenderer::BeginResult(const LedAnimationPresentation& presentation,
                                      LedAnimationFrameResult& result) {
    result.receipt = {};
    result.grid.fill({});
    result.retainPriorHint = false;
    result.attempted = false;
    auto& receipt = result.receipt;
    receipt.sequence = presentation.sequence;
    receipt.contextEpoch = presentation.contextEpoch;
    receipt.hintEpoch = presentation.hintEpoch;
    receipt.positionRevision = presentation.positionRevision;
    receipt.frameStartedMs = fxe::Millis();
    receipt.outputEnabled = OutputEnabled();
}
bool LedAnimationRenderer::ArtworkCurrent(
    const modules::litchcraft_presentation::Inputs& inputs) const {
    return inputs.source != modules::litchcraft_presentation::Source::Stream ||
        (m_artworkPort.stream != nullptr &&
         inputs.context.documentEpoch == m_artworkPort.stream->DocumentEpoch() &&
         m_artworkPort.stream->IsPresentationContextCurrent(inputs.presentationEpoch));
}
void LedAnimationRenderer::Render(LedAnimationPresentation& presentation,
                                 LedAnimationFrameResult& result) {
    BeginResult(presentation, result);
    if (!std::holds_alternative<modules::litchcraft_presentation::Inputs>(presentation.scene))
        CancelArtwork();
    if (!OutputEnabled()) return;
    LedBrightnessAttempt attempt(m_ambient, m_sourceLimit);
    const uint32_t sourceEpochBefore = m_sourceResetEpoch;
    if (auto* game = std::get_if<GameLedPresentation>(&presentation.scene)) {
        RenderGame(presentation, *game, result);
    } else if (auto* artwork = std::get_if<modules::litchcraft_presentation::Inputs>(&presentation.scene)) {
        RenderArtwork(presentation, *artwork, result);
    } else RenderSpecial(presentation, result);
    if (!result.receipt.completed && result.receipt.artwork.ready)
        m_artwork.Cancel(result.receipt.artwork, m_artworkPort);
    (void)attempt.Finish(result.receipt.completed);
    if (!result.receipt.completed) m_sourceResetEpoch = sourceEpochBefore;
    result.receipt.completedAtMs = fxe::Millis();
    result.receipt.delivery = Delivery();
    result.receipt.power = m_powerStats;
    result.receipt.currentLimitMa = m_currentLimitMa;
    result.receipt.boot = m_boot;
    result.receipt.startupComplete = !presentation.initialStoreModePending && StartupComplete();
    if (result.receipt.completed && result.receipt.startupComplete &&
        m_startupCompletedAtMs == UINT32_MAX)
        m_startupCompletedAtMs = result.receipt.completedAtMs;
    result.receipt.startupCompletedAtMs = m_startupCompletedAtMs;
}

void LedAnimationRenderer::RenderSpecial(LedAnimationPresentation& presentation,
    LedAnimationFrameResult& result) {
    m_boot.Cancel();
    m_startupAccent.Cancel();
    if (const auto* ota = std::get_if<OtaLedScene>(&presentation.scene)) {
        m_startupAccent.Cancel();
        modules::ComposeOtaLedPresentation(ota->presentation, m_board);
        auto physical = hardware::MapSemanticLedFrame(
            {m_profile->revision, presentation.boardRotate, presentation.boardRotate},
            m_board, std::array<modules::Rgb, modules::EDGE_LED_COUNT>{});
        modules::ApplyOtaPhysicalStatusPresentation(ota->presentation, m_profile->revision, physical);
        const uint16_t scale = modules::OtaLedBrightnessScale(
            Brightness(presentation.output, result.receipt.frameStartedMs));
        result.attempted = true;
        result.receipt.completed = Output(physical, presentation.output, scale, {}, false);
        result.receipt.brightnessScale256 = scale;
    } else if (const auto* test = std::get_if<ManufacturingLedPresentation>(&presentation.scene)) {
        auto inputs = test->inputs;
        const uint32_t elapsed = result.receipt.frameStartedMs - presentation.capturedAtMs;
        inputs.runElapsedMs += elapsed;
        inputs.stepElapsedMs += elapsed;
        inputs.ledScanElapsedMs += elapsed;
        inputs.verifierFeedbackElapsedMs += elapsed;
        const auto frame = ComposeManufacturingTestLedFrame(inputs);
        result.attempted = true;
        result.receipt.completed = Output(frame, presentation.output,
            test->brightnessScale256, ManufacturingTestOutputAccent());
        result.receipt.brightnessScale256 = test->brightnessScale256;
    } else if (const auto* test = std::get_if<ComplianceLedPresentation>(&presentation.scene)) {
        const auto physical = compliance::ComposeLoad(test->load, m_profile->revision,
            result.receipt.frameStartedMs - test->startedMs);
        result.attempted = true;
        result.receipt.completed = Output(physical, presentation.output, 256U, {});
        result.receipt.brightnessScale256 = 256U;
    } else if (std::holds_alternative<HoldLedPresentation>(presentation.scene)) {
        // Test/calibration output remains latched until its next explicit
        // command. Only that command's exact completion can acknowledge it;
        // merely publishing a hold must never acknowledge a new scene.
        result.receipt.completed = false;
    }
}

} // namespace firmware
