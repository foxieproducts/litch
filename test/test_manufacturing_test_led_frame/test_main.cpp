#include <array>
#include <cstdio>
#include <cmath>
#include "firmware/manufacturing_test_led_frame.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/rgbw_auto_map.hpp"
namespace {
int failures=0;
void Check(bool ok,const char* msg){if(!ok){++failures;std::printf("[FAIL] %s\n",msg);}}
bool Equal(modules::Rgb a,modules::Rgb b){return a.r==b.r&&a.g==b.g&&a.b==b.b;}
class CaptureRenderer final:public modules::ILedRenderer {
  public:
    explicit CaptureRenderer(uint16_t count=1U,bool rgbw=false):count(count),rgbw(rgbw){}
    uint16_t GetLedCount()const override{return count;}
    void Clear()override{pixels.fill({});}
    void SetLedRgb(uint16_t i,uint8_t r,uint8_t g,uint8_t b)override{pixels[i]=rgbw?modules::AutoMapRgbToRgbw({r,g,b},32U):modules::RgbwChannels{r,g,b,0U};}
    void Show()override{}
    std::array<modules::RgbwChannels,300U> pixels{};
    uint16_t count;
    bool rgbw;
};
void CheckAmbientResponseCues(hardware::BoardRevision revision) {
    using Mode = firmware::HardwareTestMode;
    constexpr uint8_t corners[] = {7U, 56U};
    constexpr modules::Rgb amber{32U, 16U, 0U};
    constexpr modules::Rgb green{0U, 40U, 0U};
    const uint8_t slots = hardware::BoardLedTopologyFor(revision).physicalLedsPerSquare;
    firmware::ManufacturingTestFrameInputs input;
    input.revision = revision;
    input.step = Mode::Step::Buttons;
    input.progress.lightSensorLinkMask = 3U;

    for (uint8_t passed = 0U; passed < 2U; ++passed) {
        input.progress.lightSensorResponseMask = 1U << passed;
        input.progress.lightSensors[0].lastLux = 0U;
        input.progress.lightSensors[1].lastLux = 800U;
        input.runElapsedMs = 0U;
        const auto frame = firmware::ComposeManufacturingTestLedFrame(input);
        input.progress.lightSensors[0].lastLux = 1200U;
        input.progress.lightSensors[1].lastLux = 0U;
        input.runElapsedMs = 400U;
        const auto later = firmware::ComposeManufacturingTestLedFrame(input);
        for (uint8_t sensor = 0U; sensor < 2U; ++sensor) {
            for (uint8_t slot = 0U; slot < slots; ++slot) {
                const bool inward = slot == 1U || slot == 3U;
                const modules::Rgb expected = sensor == passed
                    ? (inward ? green : modules::Rgb{})
                    : (inward ? modules::Rgb{} : amber);
                const auto pixel = corners[sensor] * slots + slot;
                Check(Equal(frame[pixel], expected),
                      "ALS response keeps pending outward and passed inward at the addressed corner");
                Check(Equal(later[pixel], expected),
                      "ALS response mark stays steady across zero lux, brighter samples, and time");
            }
        }
    }

    input.progress.hallSnapshotValid = true;
    for (auto step : {Mode::Step::HallSensors, Mode::Step::Radio}) {
        input.step = step;
        const auto frame = firmware::ComposeManufacturingTestLedFrame(input);
        const modules::Rgb expected = step == Mode::Step::HallSensors
            ? modules::Rgb{128U, 64U, 0U} : modules::Rgb{};
        for (const uint8_t corner : corners) {
            for (uint8_t slot = 0U; slot < slots; ++slot) {
                Check(Equal(frame[corner * slots + slot], expected),
                      "ALS response progress stays out of Hall and Radio frames");
            }
        }
    }
}

void CheckAmbientFaultCues(hardware::BoardRevision revision){
    using Mode=firmware::HardwareTestMode;
    const auto topology=hardware::BoardLedTopologyFor(revision);
    const uint8_t failed=1U<<static_cast<uint8_t>(modules::ManufacturingTestLocalCheck::LightSensors);
    for(auto step:{Mode::Step::HallSensors,Mode::Step::Buttons,Mode::Step::Radio,Mode::Step::Failed})for(uint8_t missing:{0U,1U}){
        firmware::ManufacturingTestFrameInputs i;i.revision=revision;i.step=step;
        i.progress.failedAutomatedCheckMask=failed;i.progress.lightSensorLinkMask=3U&~(1U<<missing);
        i.progress.lightSensorResponseMask=3U;
        const auto frame=firmware::ComposeManufacturingTestLedFrame(i);
        const uint8_t corners[]={7U,56U};bool correct=true;
        for(uint8_t slot=0U;slot<topology.physicalLedsPerSquare;++slot)
            correct&=Equal(frame[corners[missing]*topology.physicalLedsPerSquare+slot],{255U,0U,0U});
        Check(correct,"completed missing ALS communication paints its exact physical full-square corner red in every fault view");
        if (step == Mode::Step::Buttons) {
            const auto healthyPixel = corners[1U - missing] * topology.physicalLedsPerSquare;
            Check(Equal(frame[healthyPixel], {}) && Equal(frame[healthyPixel + 1U], {0U, 40U, 0U}),
                  "a missing ALS keeps the other corner's completed response marker");
        }
        auto notAttempted = i;
        notAttempted.progress.failedAutomatedCheckMask = 0U;
        notAttempted.step = Mode::Step::Buttons;
        const auto unknown = firmware::ComposeManufacturingTestLedFrame(notAttempted);
        for (uint8_t slot = 0U; slot < topology.physicalLedsPerSquare; ++slot) {
            Check(Equal(unknown[corners[missing] * topology.physicalLedsPerSquare + slot], {}),
                  "unknown ALS stays dark with neither a known link nor a failed check");
        }
        i.progress.failedAutomatedCheckMask=failed;i.operatorHoldProgress=128U;const auto held=firmware::ComposeManufacturingTestLedFrame(i);
        Check(Equal(held[corners[missing]*topology.physicalLedsPerSquare],{}),"exit hold owns priority over failed ALS corner");
        i.operatorHoldProgress=0U;i.step=Mode::Step::LedRainbow;i.ledChannel=1U;const auto optical=firmware::ComposeManufacturingTestLedFrame(i);
        Check(Equal(optical[corners[missing]*topology.physicalLedsPerSquare],{0U,255U,0U}),"optical field excludes ALS failure overlay");
        i.step=Mode::Step::Passed;const auto saved=firmware::ComposeManufacturingTestLedFrame(i);
        Check(Equal(saved[corners[missing]*topology.physicalLedsPerSquare],{}),"saved PASS never gains stale ALS corner marks");
    }
}
void CheckFactoryBrightness(firmware::HardwareTestMode& mode){
    const auto revision=mode.GetRevision();
    auto config=modules::led_render_policy::MakePipelineConfig(revision!=hardware::BoardRevision::V3_3S3,32U);
    config.enableS3OpticalColorCalibration=false;
    if(revision==hardware::BoardRevision::V3_3S3)config.channelMaxCurrentMa=20U;
    config.outputBrightnessScale256=hardware::BoardHardwareProfileFor(revision).ledOutputScale256;
    auto policyConfig=modules::led_render_policy::MakeConservativePowerBudgetPolicyConfig({true,false});
    modules::PowerBudgetPolicy policy(policyConfig);CaptureRenderer renderer;
    modules::BudgetAwareLedPipeline pipeline(renderer,policy,config);
    const modules::Rgb white{255U,255U,255U};
    pipeline.SetBrightnessScale01(230.0f/256.0f);pipeline.RenderFrame(&white,1U);
    const auto old=pipeline.GetLastStats();const auto oldPixel=renderer.pixels[0];
    pipeline.SetBrightnessScale01(static_cast<float>(mode.GetBrightnessScale256())/256.0f);
    pipeline.RenderFrame(&white,1U,firmware::ManufacturingTestOutputAccent());
    const auto now=pipeline.GetLastStats();const auto newPixel=renderer.pixels[0];
    const float target=revision==hardware::BoardRevision::V3_3S3?0.3375f:0.675f;
    const float effective=now.appliedBrightnessScale01*now.appliedAccentScale256/256.0f;
    Check(!old.powerLimited&&!now.powerLimited&&std::fabs(effective-target)<1.0f/512.0f&&
        std::fabs(static_cast<float>(newPixel.r)/oldPixel.r-0.75f)<0.01f,
        "real unlimited pixel intensity is75% of old request within quantization and retains revision gain");
    std::printf("[FACTORY_BRIGHTNESS] revision=%u old=%.6f requested=%.6f ratio=%.6f gain=%u limited=%u\n",unsigned(revision),old.appliedBrightnessScale01,effective,effective/old.appliedBrightnessScale01,config.outputBrightnessScale256,now.powerLimited);
    const auto topology=hardware::BoardLedTopologyFor(revision);
    CaptureRenderer full(topology.physicalBoardLedCount+topology.physicalStatusLedCount,revision!=hardware::BoardRevision::V3_3S3);
    modules::PowerBudgetPolicy densePolicy(policyConfig);modules::BudgetAwareLedPipeline dense(full,densePolicy,config);
    dense.SetBrightnessScale01(static_cast<float>(mode.GetBrightnessScale256())/256.0f);
    for(uint8_t channel=0U;channel<4U;++channel){
        firmware::ManufacturingTestFrameInputs input;input.revision=revision;input.step=firmware::HardwareTestMode::Step::LedRainbow;input.ledChannel=channel;
        const auto frame=firmware::ComposeManufacturingTestLedFrame(input);dense.RenderFrame(frame.data(),full.count);
        const auto before=full.pixels;const auto oldStats=dense.GetLastStats();
        dense.RenderFrame(frame.data(),full.count,firmware::ManufacturingTestOutputAccent());const auto afterStats=dense.GetLastStats();
        bool dimmed=true;for(uint16_t n=0U;n<full.count;++n){const auto a=before[n],b=full.pixels[n];
            const uint8_t oldChannels[]={a.r,a.g,a.b,a.w},newChannels[]={b.r,b.g,b.b,b.w};
            for(unsigned die=0U;die<4U;++die)dimmed&=std::abs(int(newChannels[die])-int((oldChannels[die]*3U+2U)/4U))<=1;
        }
        Check(oldStats.powerLimited&&dimmed&&afterStats.appliedLedCurrentMa<=oldStats.appliedLedCurrentMa,
            "real RGB/RGBW board/status optical output follows75% post-budget scalar within one PWM code and retains the hard ceiling");
        std::printf("[FACTORY_LIMITED_DIM] revision=%u channel=%u count=%u old=%u new=%u old_ma=%u new_ma=%u pair=%u\n",unsigned(revision),channel,full.count,before[0].r+before[0].g+before[0].b+before[0].w,full.pixels[0].r+full.pixels[0].g+full.pixels[0].b+full.pixels[0].w,oldStats.appliedLedCurrentMa,afterStats.appliedLedCurrentMa,dimmed);
    }
}
}
int RunFactorySensorCueRegression();
int main(){
    failures += RunFactorySensorCueRegression();
    using Mode=firmware::HardwareTestMode;
    for(auto revision:{hardware::BoardRevision::V2S3,hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3}){
        Mode mode;mode.Enter(revision,0U);Check(mode.IsActive(),"all supported S3 revisions enter factory");
        const auto topology=hardware::BoardLedTopologyFor(revision);
        const unsigned count=topology.physicalBoardLedCount+topology.physicalStatusLedCount;
        const auto gain=hardware::BoardHardwareProfileFor(revision).ledOutputScale256;
        Check(mode.GetBrightnessScale256()==230U&&gain==(revision==hardware::BoardRevision::V3_3S3?128U:256U),"established90%/45% safe baseline stays; shared post-budget accent supplies25% output reduction");
        CheckFactoryBrightness(mode);
        CheckAmbientResponseCues(revision);
        CheckAmbientFaultCues(revision);
        firmware::ManufacturingTestFrameInputs i;i.revision=revision;i.step=Mode::Step::LedRainbow;i.automaticVerifier=true;
        for(uint8_t channel=0U;channel<4U;++channel){
            i.ledChannel=channel; i.runElapsedMs=900000U;
            const auto frame=firmware::ComposeManufacturingTestLedFrame(i);
            const modules::Rgb expected=channel==0U?modules::Rgb{255U,0U,0U}:channel==1U?modules::Rgb{0U,255U,0U}:channel==2U?modules::Rgb{0U,0U,255U}:modules::Rgb{255U,255U,255U};
            for(unsigned n=0U;n<count;++n)Check(Equal(frame[n],expected),"every fitted output belongs to the selected field; no verifier masking or automatic timer");
            for(unsigned n=count;n<frame.size();++n)Check(Equal(frame[n],{}),"nonfitted outputs remain absent");
            if(channel==3U&&revision!=hardware::BoardRevision::V3_3S3){auto c=modules::AutoMapRgbToRgbw(frame[0],32U);Check(c.r==255U&&c.g==255U&&c.b==255U&&c.w==255U,"existing neutral fast path drives RGB and W together");}
        }
        i.step=Mode::Step::Buttons;i.progress.expectedButtonMask=0x37EFU;
        const auto pending=firmware::ComposeManufacturingTestLedFrame(i);auto frame=pending;
        i.runElapsedMs=500U;frame=firmware::ComposeManufacturingTestLedFrame(i);
        bool steady=true;for(unsigned n=0U;n<count;++n)steady&=Equal(frame[n],pending[n]);
        Check(steady,"pending controls stay steady at old pulse boundaries with no verifier overlay");
        constexpr uint8_t bits[]={0U,1U,2U,3U,5U,6U,7U,8U,9U,10U,12U,13U};
        constexpr uint8_t squares[][2]={{35U,36U},{59U,60U},{50U,42U},{53U,45U},{57U,255U},{62U,255U},
            {27U,28U},{3U,4U},{13U,21U},{10U,18U},{6U,255U},{1U,255U}};
        constexpr uint8_t pendingNavigation[]={49U,54U,14U,9U};
        constexpr uint8_t dedicated[]={27U,29U,30U,28U,31U,26U,5U,7U,8U,6U,9U,4U};
        for(unsigned control=0U;control<12U;++control){
            const bool navigation=control%6U>=4U;
            const modules::Rgb pendingColor{128U,64U,0U};
            bool visible=true;for(uint8_t square:squares[control])if(square<64U){
                const unsigned base=square*topology.physicalLedsPerSquare;
                visible&=Equal(pending[base],pendingColor)&&Equal(pending[base+2U],pendingColor);
            }
            if(revision==hardware::BoardRevision::V3_3S3)visible&=Equal(pending[256U+dedicated[control]],pendingColor);
            if(navigation){const auto square=pendingNavigation[(control/6U)*2U+(control%6U-4U)];visible&=Equal(pending[square*topology.physicalLedsPerSquare],{128U,64U,0U});}
            Check(visible,"untested directions stay on; X/O retain action mark plus extra-square pending extent");
            i.progress.observedButtonMask=i.progress.pressedButtonMask=1U<<bits[control];
            frame=firmware::ComposeManufacturingTestLedFrame(i);
            bool pair=true;for(uint8_t square:squares[control])if(square<64U){
                const unsigned base=square*topology.physicalLedsPerSquare;
                pair&=Equal(frame[base],{160U,160U,160U})&&Equal(frame[base+2U],{160U,160U,160U});
                if(topology.physicalLedsPerSquare==4U)pair&=Equal(frame[base+1U],{})&&Equal(frame[base+3U],{});
            }
            if(revision==hardware::BoardRevision::V3_3S3)pair&=Equal(frame[256U+dedicated[control]],{160U,160U,160U});
            Check(pair,"all twelve named control footprints acknowledge both outward sides; P2 axes face north player");
            i.progress.pressedButtonMask=0U;frame=firmware::ComposeManufacturingTestLedFrame(i);
            bool dark=true;for(uint8_t square:squares[control])if(square<64U){
                const unsigned base=square*topology.physicalLedsPerSquare;
                const modules::Rgb released=navigation?modules::Rgb{0U,160U,0U}:modules::Rgb{};
                dark&=Equal(frame[base],released)&&Equal(frame[base+2U],released);
            }
            if(revision==hardware::BoardRevision::V3_3S3)dark&=Equal(frame[256U+dedicated[control]],navigation?modules::Rgb{0U,160U,0U}:modules::Rgb{});
            if(navigation){const auto square=pendingNavigation[(control/6U)*2U+(control%6U-4U)];dark&=Equal(frame[square*topology.physicalLedsPerSquare],{});}
            Check(dark,"tested directions go off; tested X/O keep one-square action mark and lose the extra pending square");
        }
        bool ringDark=true;for(uint8_t n=0U;n<32U;++n){const unsigned physical=revision==hardware::BoardRevision::V3_3S3?hardware::V3_3PhysicalIndexForSemanticStatus(n):n;ringDark&=Equal(frame[topology.physicalBoardLedCount+physical],{});}
        Check(ringDark,"local button diagrams leave fixed32 perimeter quiet while dedicated V3.3 controls retain contact evidence");
        i.progress.observedButtonMask=i.progress.pressedButtonMask=i.progress.expectedButtonMask=0U;
        i.step=Mode::Step::HallSensors;i.progress.hallSnapshotValid=true;i.progress.hallActiveMask=1ULL;i.progress.hallClearMask=2ULL;
        frame=firmware::ComposeManufacturingTestLedFrame(i);
        Check(Equal(frame[0],{128U,64U,0U})&&Equal(frame[topology.physicalLedsPerSquare],{128U,64U,0U}),"Hall remains orange pending both occupied and clear observations");
        i.progress.hallActiveMask|=2ULL;i.progress.hallClearMask|=1ULL;frame=firmware::ComposeManufacturingTestLedFrame(i);
        for(unsigned n=0U;n<2U*topology.physicalLedsPerSquare;++n)Check(Equal(frame[n],{255U,255U,255U}),"both historical electrical states produce white on all square emitters");
        i.progress.hallSnapshotValid=false;i.runElapsedMs=0U;frame=firmware::ComposeManufacturingTestLedFrame(i);Check(!Equal(frame[0],{255U,255U,255U}),"invalid read cannot present valid Hall completion");
        i.step=Mode::Step::Radio;i.automaticVerifier=false;i.stepElapsedMs=0U;
        const auto breathLow=firmware::ComposeManufacturingTestLedFrame(i);
        i.stepElapsedMs=1500U;const auto breathHigh=firmware::ComposeManufacturingTestLedFrame(i);
        Check(breathLow[18U*topology.physicalLedsPerSquare].r<breathHigh[18U*topology.physicalLedsPerSquare].r,"waiting DUT amber outline visibly breathes over slow three-second period");
        i.automaticVerifier=true;i.stepElapsedMs=250U;const auto elected=firmware::ComposeManufacturingTestLedFrame(i);
        Check(Equal(elected[19U*topology.physicalLedsPerSquare],{160U,160U,160U})&&Equal(elected[27U*topology.physicalLedsPerSquare],{}),"unsaved automatic verifier has a clockwise white boundary marker and dark center");
        // Election and partial proof keep the outline pending; only role motion differs.
        i.verifierFeedback=Mode::VerifierFeedback::Success;
        i.step=Mode::Step::Radio;i.progress.expectedButtonMask=0x37EFU;i.progress.observedButtonMask=0x37EFU;
        for(uint8_t proof:{0U,1U,2U,3U})for(uint32_t phase:{0U,250U,500U,1000U,1500U}){
            i.progress.radioChannelMask=proof;i.stepElapsedMs=phase;frame=firmware::ComposeManufacturingTestLedFrame(i);
            bool central=true;for(uint8_t row=2U;row<6U;++row)for(uint8_t col=2U;col<6U;++col){const bool boundary=row==2U||row==5U||col==2U||col==5U;const unsigned first=(row*8U+col)*topology.physicalLedsPerSquare;
                constexpr uint8_t border[]={18U,19U,20U,21U,29U,37U,45U,44U,43U,42U,34U,26U};
                const bool marker=boundary&&row*8U+col==border[(phase/250U)%12U];
                central&=Equal(frame[first],boundary?(marker?modules::Rgb{160U,160U,160U}:modules::Rgb{128U,64U,0U}):modules::Rgb{})&&Equal(frame[first+2U],frame[first]);
            }
            Check(central,"pending/elected/partial-proof Radio is the central4x4 amber outline, not green success");
            bool quiet=true;for(uint8_t status=0U;status<32U;++status){const auto physical=revision==hardware::BoardRevision::V3_3S3?hardware::V3_3PhysicalIndexForSemanticStatus(status):status;quiet&=Equal(frame[topology.physicalBoardLedCount+physical],{});}
            Check(quiet,"four unexplained radio status lights are removed; fixed32 ring stays dark");
            for(auto step:{Mode::Step::Radio,Mode::Step::Persisting,Mode::Step::Passed})for(uint16_t held:{0U,1U<<12U}){
                auto noNavigation=i;noNavigation.step=step;noNavigation.progress.pressedButtonMask=held;
                const auto clean=firmware::ComposeManufacturingTestLedFrame(noNavigation);
                bool absent=true;
                for(uint8_t square:{57U,62U,1U,6U,49U,54U,9U,14U})for(uint8_t slot=0U;slot<topology.physicalLedsPerSquare;++slot)
                    absent&=Equal(clean[square*topology.physicalLedsPerSquare+slot],{});
                if(revision==hardware::BoardRevision::V3_3S3)for(uint8_t bank=0U;bank<2U;++bank)for(uint8_t role=0U;role<6U;++role)
                    absent&=Equal(clean[256U+hardware::V3_3PhysicalIndexForControl(bank==1U,static_cast<hardware::ControlLedRole>(role))],{});
                Check(absent,"Radio/persistence/saved role cues have no X/O marks or dedicated controls, including held X");
            }
        }
        i.step=Mode::Step::HallSensors;i.progress.hallSnapshotValid=true;
        i.progress.hallActiveMask=i.progress.hallClearMask=0ULL;
        frame=firmware::ComposeManufacturingTestLedFrame(i);
        bool hallPending=true;for(unsigned n=0U;n<topology.physicalBoardLedCount;++n)hallPending&=Equal(frame[n],{128U,64U,0U});
        for(unsigned n=topology.physicalBoardLedCount;n<count;++n)hallPending&=Equal(frame[n],{});
        Check(hallPending,"all untested Hall squares are orange with no verifier/radio overlays");
        i.verifierFeedback=Mode::VerifierFeedback::Waiting;
        i.step=Mode::Step::Passed;i.automaticVerifier=true;
        for(uint32_t phase:{0U,249U,250U,500U,2750U,3000U}){
            i.stepElapsedMs=phase;frame=firmware::ComposeManufacturingTestLedFrame(i);
            unsigned green=0U,white=0U;bool limited=true;
            for(uint8_t square=0U;square<64U;++square){const bool central=square/8U>=2U&&square/8U<6U&&square%8U>=2U&&square%8U<6U;const auto c=frame[square*topology.physicalLedsPerSquare];
                if(central){green+=Equal(c,{0U,160U,0U});white+=Equal(c,{160U,160U,160U});}else limited&=Equal(c,{});
            }
            Check(green==15U&&white==1U&&limited,"saved verifier is filled central green square with one moving white boundary cell, never full-board sparkle");
        }
        i.automaticVerifier=false;frame=firmware::ComposeManufacturingTestLedFrame(i);
        bool verified=true;for(uint8_t square=0U;square<64U;++square){const bool central=square/8U>=2U&&square/8U<6U&&square%8U>=2U&&square%8U<6U;verified&=Equal(frame[square*topology.physicalLedsPerSquare],central?modules::Rgb{0U,160U,0U}:modules::Rgb{});}
        Check(verified,"durably verified DUT is a steady filled green central4x4 with no ordinary navigation or sparkle");
    }
    std::printf("[MANUFACTURING_TEST_LED_FRAME] independent named fields/coverage/Hall/cadences/gain failures=%d\n",failures);return failures?1:0;
}
