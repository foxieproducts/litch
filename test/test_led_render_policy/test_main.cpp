#include <cstdio>

#include "modules/led_render_policy.hpp"
#include "modules/rgb_optical_calibration.hpp"

namespace {
int failures = 0;
void Check(const bool value, const char* message) {
    if (!value) {
        std::printf("[FAIL] %s\n", message);
        ++failures;
    }
}
} // namespace

int main() {
    using namespace modules;
    using namespace modules::led_render_policy;
    std::puts("[LED_RENDER_POLICY] start");
    const auto c3 = MakePowerBudgetPolicyConfig({false, false});
    Check(c3.maxLedCurrentMa == S3_MAX_LED_CURRENT_MA && c3.maxTotalCurrentMa == S3_MAX_LED_CURRENT_MA,
          "C3 normal envelope is direct LED limit");
    const auto c3Brownout = MakePowerBudgetPolicyConfig({false, true});
    Check(c3Brownout.maxLedCurrentMa == LEGACY_BROWNOUT_LED_CURRENT_LIMIT_MA,
          "C3 brownout uses legacy limit");
    const auto weak = MakePowerBudgetPolicyConfig({true, false});
    Check(weak.maxTotalCurrentMa == S3_NORMAL_TOTAL_CURRENT_MA && weak.fixedSystemCurrentMa == S3_FIXED_SYSTEM_CURRENT_MA,
          "unadvertised S3 source attempts the full normal budget until a confirmed brownout");
    const auto strong = MakePowerBudgetPolicyConfig({true, false});
    Check(strong.maxTotalCurrentMa == S3_NORMAL_TOTAL_CURRENT_MA,
          "S3 normal policy does not depend on CC advertisement");
    const auto rgb = MakePipelineConfig(false, 0U);
    Check(rgb.channelMaxCurrentMa == S3_CHANNEL_MAX_CURRENT_MA && !rgb.enableRgbwAutoMapping,
          "RGB production fallback is six milliamps per channel");
    const auto rgbw = MakePipelineConfig(true, 41U);
    Check(rgbw.enableRgbwAutoMapping && rgbw.enableS3OpticalColorCalibration &&
              rgbw.rgbwCoolToWarmThreshold == 41U &&
              rgbw.whiteChannelMaxCurrentMaTimes100 == S3_WHITE_CURRENT_MA_TIMES100,
          "RGBW config carries shared optical calibration, live threshold and measured dies");
    const auto neutral = rgb_optical_calibration::PresentForS3({96U, 96U, 96U});
    Check(neutral.r == 96U && neutral.g == 96U && neutral.b == 96U,
          "S3 optical calibration preserves neutral RGB for white extraction");
    const auto warm = rgb_optical_calibration::PresentForS3({255U, 116U, 48U});
    Check(warm.r == 255U && warm.g == 97U && warm.b == 20U,
          "S3 optical calibration increases warm-color chroma without changing its peak");
    const auto red = rgb_optical_calibration::PresentForS3({255U, 52U, 82U});
    const auto green = rgb_optical_calibration::PresentForS3({55U, 215U, 105U});
    const auto violet = rgb_optical_calibration::PresentForS3({174U, 83U, 255U});
    Check(red.r == 255U && red.g == 22U && red.b == 41U,
          "S3 optical calibration raises red chroma without moving its peak");
    Check(green.r == 25U && green.g == 215U && green.b == 58U,
          "S3 optical calibration removes cyan contamination from green");
    Check(violet.r == 153U && violet.g == 39U && violet.b == 161U,
          "S3 optical calibration balances violet red and blue chroma");
    std::printf("[LED_RENDER_POLICY] %s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
