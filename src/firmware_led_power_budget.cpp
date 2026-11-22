#ifdef ARDUINO

#include "firmware/firmware_internal.hpp"
#include "firmware/firmware_tasks_internal.hpp"

#include "foxie_embedded/millis.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/led_power_envelope.hpp"

namespace firmware_internal {

namespace {

class NullLedRenderer final : public modules::ILedRenderer {
  public:
    explicit NullLedRenderer(const uint16_t ledCount)
        : m_ledCount(ledCount) {}

    uint16_t GetLedCount() const override { return m_ledCount; }
    void Clear() override {}
    void SetLedRgb(uint16_t, uint8_t, uint8_t, uint8_t) override {}
    void Show() override {}

  private:
    uint16_t m_ledCount = 0U;
};

uint16_t ActiveTotalCurrentLimitMa(const FirmwareSystem& sys) {
    return modules::led_render_policy::MakePowerBudgetPolicyConfig({
        hardware::HW_IS_S3,
        IsLedBrownoutLimitLatched(sys.settings),
    }).maxTotalCurrentMa;
}

modules::PowerBudgetPolicyConfig MakePowerPolicyConfig(const FirmwareSystem& sys) {
    return modules::led_render_policy::MakePowerBudgetPolicyConfig({
        hardware::HW_IS_S3,
        IsLedBrownoutLimitLatched(sys.settings),
    });
}

modules::PowerBudgetPolicyConfig MakeConservativePowerPolicyConfig(
    const FirmwareSystem& sys) {
    return modules::led_render_policy::MakeConservativePowerBudgetPolicyConfig({
        hardware::HW_IS_S3,
        IsLedBrownoutLimitLatched(sys.settings),
    });
}

modules::PowerBudgetPolicyConfig MakeRecoveryPowerPolicyConfig() {
    modules::PowerBudgetPolicyConfig policyCfg;
    policyCfg.maxLedCurrentMa = firmware::RECOVERY_LED_CURRENT_CAP_MA;
    policyCfg.fixedSystemCurrentMa =
        hardware::HW_IS_S3 ? modules::S3_FIXED_SYSTEM_CURRENT_MA : 0U;
    policyCfg.maxTotalCurrentMa = static_cast<uint16_t>(
        policyCfg.fixedSystemCurrentMa +
        firmware::RECOVERY_LED_CURRENT_CAP_MA);
    policyCfg.allowUsbPower = false;
    policyCfg.hysteresisMa = 0U;
    return policyCfg;
}

uint16_t EffectiveLedCurrentLimitMa(const modules::PowerBudgetPolicyConfig& cfg) {
    uint16_t totalHeadroomMa = UINT16_MAX;
    if (cfg.maxTotalCurrentMa != UINT16_MAX) {
        totalHeadroomMa = (cfg.fixedSystemCurrentMa < cfg.maxTotalCurrentMa)
                                 ? static_cast<uint16_t>(cfg.maxTotalCurrentMa -
                                                         cfg.fixedSystemCurrentMa)
                                 : 0U;
    }
    return (cfg.maxLedCurrentMa < totalHeadroomMa)
                   ? cfg.maxLedCurrentMa
                   : totalHeadroomMa;
}

modules::BudgetAwareLedPipelineConfig MakePipelineConfig(FirmwareSystem& sys) {
    bool enableRgbwAutoMapping = false;
    uint8_t rgbwCoolToWarmThreshold = 0U;
#if defined(LITCH_HW_VARIANT_S3)
    enableRgbwAutoMapping =
        sys.runtimeHardware.resolved.revision != hardware::BoardRevision::V3_3S3;
    rgbwCoolToWarmThreshold = static_cast<uint8_t>(std::min<uint32_t>(255U,
        sys.settings.ReadValue("RGBW_COOL_TO_WARM_THRESHOLD", uint32_t{0U})));
#endif
    auto config = modules::led_render_policy::MakePipelineConfig(
        enableRgbwAutoMapping, rgbwCoolToWarmThreshold);
    config.outputBrightnessScale256 = sys.runtimeHardware.resolved.ledOutputScale256;
#if defined(LITCH_HW_VARIANT_S3)
    if (sys.runtimeHardware.resolved.revision ==
        hardware::BoardRevision::V3_3S3) {
        // V2/V3's measured 6 mA die limit does not apply to V3.3 RGB parts.
        config.channelMaxCurrentMa = 20U;
    }
#endif
    const bool litchcraftActive = sys.gamePersistence.Litchcraft().IsCanvasVisible();
    config.rgbwWhiteMixScale256 = sys.hardwareTest.IsActive() ? 256U : (litchcraftActive ? 0U : 256U);
    if (sys.hardwareTest.IsActive()) config.enableS3OpticalColorCalibration = false;
    // Preserve canonical document RGB and keep the white die off, but retain
    // the shared V3 optical presentation transform. Exact numeric channels
    // looked washed toward white on the physical emitters even at lower scale.
    return config;
}

void RenderPhysicalFrameWithPolicy(
    FirmwareSystem& sys,
    const PhysicalLedFrame& physicalFrame,
    const uint16_t brightnessScale256,
    const modules::PowerBudgetPolicyConfig& policyCfg,
    const modules::LedFrameAccent& accent = {}) {
    if (sys.ledAnimation.StartAttempted()) {
        firmware::PhysicalLedCommand command;
        command.frame = physicalFrame;
        command.output = CaptureLedOutputPresentation(sys);
        command.output.power = policyCfg;
        command.brightnessScale256 = brightnessScale256;
        command.accentScale256 = accent.scale256;
        command.accentCeilingScale256 = accent.ceilingScale256;
        if (accent.mask != nullptr) {
            for (size_t i = 0U; i < command.accentMask.size() && i < accent.maskCount; ++i)
                command.accentMask[i] = accent.mask[i];
        }
        sys.ledLastCommandCompleted = sys.ledAnimation.Submit(command);
        ConsumeLedPresentation(sys);
        return;
    }
    modules::PowerBudgetPolicy policy(policyCfg);
#if defined(LITCH_HW_VARIANT_S3)
    sys.Leds().SetRgbwWhiteMixScale256(
        sys.hardwareTest.IsActive() ? 256U : (sys.gamePersistence.Litchcraft().IsCanvasVisible() ? 0U : 256U));
#endif
    modules::BudgetAwareLedPipeline pipeline(
        sys.Leds(), policy, MakePipelineConfig(sys));
    pipeline.SetBrightnessScale01(
        static_cast<float>(brightnessScale256) / 256.0f);
    pipeline.RenderFrame(physicalFrame.data(), sys.Leds().GetLedCount(), accent);

    sys.ledPowerStats = pipeline.GetLastStats();
    sys.ledPowerLimitMa = EffectiveLedCurrentLimitMa(policyCfg);
}

PhysicalLedFrame ExpandLegacyPhysicalFrameForRuntime(
    const FirmwareSystem& sys,
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& legacyFrame) {
    return hardware::MapLegacyLedFrame(
        {sys.runtimeHardware.resolved.revision}, legacyFrame);
}

} // namespace

firmware::LedOutputPresentation CaptureLedOutputPresentation(FirmwareSystem& sys,
    const bool conservative) {
    firmware::LedOutputPresentation output;
    output.power = conservative ? MakeConservativePowerPolicyConfig(sys) : MakePowerPolicyConfig(sys);
    if (sys.comptest.IsActive()) output.power = firmware::compliance::LimitBudget(output.power,
        sys.usbCcSampleValid ? sys.lastUsbCcAdvertisedLevel : hardware::UsbCcLevel::Unknown,
        sys.comptest.CurrentLoad().ceilingMa);
    output.pipeline = MakePipelineConfig(sys);
    output.bootFade = sys.startupLeds.Fade();
    output.autoBrightness = sys.settings.ReadValue("AUTO_BRIGHTNESS", true);
    output.brightnessLevel = GetBrightnessLevel0To8(sys.settings);
    output.ambientUsable = sys.ambientLights.HasUsableSampleForBrightness();
    output.rawAmbientFactor01 = sys.ambientLux.ComputeAmbientFactor01(fxe::Millis());
    output.canvasVisible = sys.gamePersistence.Litchcraft().IsCanvasVisible();
    output.exactOpticalTest = sys.hardwareTest.IsActive();
    output.sourceResetEpoch = sys.ledSourceResetEpoch;
    output.sourceResetLimitMa = sys.ledSourceResetLimitMa;
    return output;
}

bool ClearLedOutput(FirmwareSystem& sys) {
    if (sys.ledAnimation.IsQuiescent() && !sys.powerRail.Is5vEnabled()) return true;
    if (sys.ledAnimation.StartAttempted()) {
        sys.ledLastCommandCompleted = sys.ledAnimation.Submit(firmware::RawDieLedCommand{});
        ConsumeLedPresentation(sys);
    } else {
        sys.Leds().Clear();
        sys.Leds().Show();
        sys.ledLastCommandCompleted = sys.Leds().LastShowCompleted();
    }
    return sys.ledLastCommandCompleted;
}
bool LedOutputCompleted(const FirmwareSystem& sys) {
    return sys.ledAnimation.IsStarted() ? sys.ledLastCommandCompleted : sys.Leds().LastShowCompleted();
}

uint16_t GetActiveLedCurrentLimitMa(const FirmwareSystem& sys) {
    return EffectiveLedCurrentLimitMa(MakePowerPolicyConfig(sys));
}

uint16_t GetActiveTotalCurrentLimitMa(const FirmwareSystem& sys) {
    return ActiveTotalCurrentLimitMa(sys);
}

uint16_t GetFixedSystemCurrentMa() {
    return hardware::HW_IS_S3 ? modules::S3_FIXED_SYSTEM_CURRENT_MA : 0U;
}

uint16_t EstimatePhysicalFrameLedCurrentMa(FirmwareSystem& sys,
                                           const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& physicalFrame,
                                           const uint16_t brightnessScale256) {
    const PhysicalLedFrame expanded =
        ExpandLegacyPhysicalFrameForRuntime(sys, physicalFrame);
    return EstimatePhysicalFrameLedCurrentMa(
        sys, expanded, brightnessScale256);
}

uint16_t EstimatePhysicalFrameLedCurrentMa(
    FirmwareSystem& sys,
    const PhysicalLedFrame& physicalFrame,
    const uint16_t brightnessScale256) {
    modules::PowerBudgetPolicyConfig policyCfg;
    modules::PowerBudgetPolicy policy(policyCfg);
    NullLedRenderer renderer(sys.Leds().GetLedCount());

    modules::BudgetAwareLedPipeline pipeline(renderer, policy, MakePipelineConfig(sys));
    pipeline.SetBrightnessScale01(static_cast<float>(brightnessScale256) / 256.0f);
    pipeline.RenderFrame(physicalFrame.data(), renderer.GetLedCount());
    return pipeline.GetLastStats().estimatedLedCurrentMa;
}

void RenderPhysicalFrameWithPowerBudget(FirmwareSystem& sys,
                                        const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& physicalFrame,
                                        const uint16_t brightnessScale256) {
    const PhysicalLedFrame expanded =
        ExpandLegacyPhysicalFrameForRuntime(sys, physicalFrame);
    RenderPhysicalFrameWithPowerBudget(sys, expanded, brightnessScale256);
}

void RenderPhysicalFrameWithPowerBudget(
    FirmwareSystem& sys,
    const PhysicalLedFrame& physicalFrame,
    const uint16_t brightnessScale256,
    const modules::LedFrameAccent& accent) {
    // Source acquisition is serviced incrementally by the main loop. Rendering
    // consumes only already-authorized state and must never wait on ADC settling.
    modules::PowerBudgetPolicyConfig policyCfg = MakePowerPolicyConfig(sys);
    if (sys.comptest.IsActive()) {
        policyCfg = firmware::compliance::LimitBudget(policyCfg,
            sys.usbCcSampleValid ? sys.lastUsbCcAdvertisedLevel : hardware::UsbCcLevel::Unknown,
            sys.comptest.CurrentLoad().ceilingMa);
    }
    if (hardware::HW_IS_S3 && !sys.ledAnimation.IsStarted()) {
        policyCfg.maxTotalCurrentMa = sys.sourceOutputLimit.Update(
            policyCfg.maxTotalCurrentMa, fxe::Millis());
    }
    RenderPhysicalFrameWithPolicy(
        sys, physicalFrame, brightnessScale256, policyCfg, accent);
}

void RenderPhysicalFrameWithConservativePowerBudget(
    FirmwareSystem& sys,
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& physicalFrame,
    const uint16_t brightnessScale256) {
    const PhysicalLedFrame expanded =
        ExpandLegacyPhysicalFrameForRuntime(sys, physicalFrame);
    RenderPhysicalFrameWithConservativePowerBudget(
        sys, expanded, brightnessScale256);
}

void RenderPhysicalFrameWithConservativePowerBudget(
    FirmwareSystem& sys,
    const PhysicalLedFrame& physicalFrame,
    const uint16_t brightnessScale256) {
    const modules::PowerBudgetPolicyConfig policyCfg =
        MakeConservativePowerPolicyConfig(sys);
    RenderPhysicalFrameWithPolicy(
        sys, physicalFrame, brightnessScale256, policyCfg);
}

void RenderPhysicalFrameWithRecoveryPowerBudget(
    FirmwareSystem& sys,
    const PhysicalLedFrame& physicalFrame,
    const uint16_t brightnessScale256) {
    const modules::PowerBudgetPolicyConfig policyCfg =
        MakeRecoveryPowerPolicyConfig();
    RenderPhysicalFrameWithPolicy(
        sys, physicalFrame, brightnessScale256, policyCfg);
}

} // namespace firmware_internal

#endif
