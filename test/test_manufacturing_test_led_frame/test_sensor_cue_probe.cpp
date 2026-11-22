#include "firmware/manufacturing_test_led_frame.hpp"
#include <cstdio>
int RunFactorySensorCueRegression() {
    firmware::ManufacturingTestFrameInputs input;
    input.revision=hardware::BoardRevision::V3S3;
    input.step=firmware::HardwareTestMode::Step::Buttons;
    input.automaticVerifier=true;
    input.progress.lightSensorLinkMask=3U;
    input.progress.lightSensorResponseMask=2U;
    input.progress.motionPresenceKnown=true;
    input.progress.motionPresent=false;
    input.progress.expanderLinksKnown=true;
    input.progress.expanderLinkMask=0x0EU;
    const auto square=firmware::ComposeManufacturingTestLedFrame(input);
    const bool correct=square[7U*4U].r==32U && square[7U*4U].g==16U &&
        square[56U*4U].r==0U && square[56U*4U+1U].g==40U &&
        square[40U*4U].r==255U && square[40U*4U].g==0U &&
        square[8U*4U].r==255U && square[256U+30U].r==0U;
    input.runElapsedMs=400U;
    const auto edge=firmware::ComposeManufacturingTestLedFrame(input);
    const bool alternates=edge[8U*4U].r==0U && edge[256U+30U].r==255U && edge[40U*4U].r==255U;
    std::printf("[FACTORY_SENSOR_REGRESSION] H8 pending rails, A1 passed center, A3 solid motion, A7/status alternating: %s\n",correct&&alternates?"PASS":"FAIL");
    return correct&&alternates?0:1;
}
#ifdef FACTORY_SENSOR_CUE_PROBE_STANDALONE
int main(){return RunFactorySensorCueRegression();}
#endif
