#include <array>
#include <cstdio>
#include <string>
#include "console.hpp"
#include "modules/hint_button_gesture_controller.hpp"
#include "modules/reset_hold_gesture_controller.hpp"
#include "hardware/hardware.hpp"
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/manufacturing_test_session.hpp"
#include "firmware/console_led_diagnostic_frame.hpp"
#include "owner_pairing_console_test_transport.hpp"
#include "firmware/sleep_transition.hpp"
#define ARDUINO 1
#include "firmware/compliance_test.hpp"
#undef ARDUINO
#include "firmware/led_animation_presentation.hpp"

namespace compliance_fixture {
int failures = 0;
void Check(bool ok, const char* why) { if (!ok) { ++failures; std::printf("[FAIL] %s\n", why); } }
class Leds final : public modules::ILedRenderer {
 public:
    std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT> pixels{};
    unsigned shows = 0U;
    bool transferCompletes = true, lastShowCompleted = false;
    uint16_t GetLedCount() const override { return 288U; }
    void Clear() override { pixels.fill({}); }
    void SetLedRgb(uint16_t i, uint8_t r, uint8_t g, uint8_t b) override { pixels[i] = {r,g,b}; }
    void Show() override { ++shows; lastShowCompleted = transferCompletes; }
    void RestoreOutputPinRouting() {}
    bool LastShowCompleted() const { return lastShowCompleted; }
};
// The existing controlled output sink completes owner commands synchronously;
// production Console/session/rail policy runs, while task scheduling is bypassed.
struct LedAnimation {
    Leds& output;
    bool quiescent = false;
    bool IsStarted() const { return true; }
    bool IsQuiescent() const { return quiescent; }
    bool Submit(const firmware::RawDieLedCommand& command) {
        Check(command.value == 0U, "compliance uses raw commands only for output clearing");
        if (quiescent) return false;
        output.Clear(); output.Show();
        return output.LastShowCompleted();
    }
    bool Quiesce() {
        if (quiescent) return true;
        output.Clear(); output.Show();
        quiescent = output.LastShowCompleted();
        return quiescent;
    }
    void Resume() { quiescent = false; }
};
struct FirmwareSystem {
    bool factoryBootOnly = false;
    bool rebootPersistencePending = false;
    struct { firmware::SleepState GetState() const { return firmware::SleepState::Awake; } } sleep;
    bool recoveryFrameInitialized = false;
    struct { bool on=true; bool IsPoweredOn() const {return on;} void ShutDown(){on=false;} void PowerOn(){on=true;} } ambientLights;
    struct { bool on=true; bool Is5vEnableRequested() const { return on; } bool Is5vEnabled() const{return on;} void Disable5v(){on=false;} void Enable5v(){on=true;} } powerRail;
    struct Inactive { bool IsActive() const{return false;} } hardwareTest, amptest, amptestVerify;
    struct { bool boardSettings=true;
        struct { bool running=false; bool IsRunning() const { return running; } } clock;
        const auto& GetGameClock() const { return clock; }
        bool IsEngineLaneActive() const { return false; }
        void SetBoardSettingsActive(bool v){boardSettings=v;}
        void RequestCancelAllThinking() {} Inactive& Litchcraft(){ static Inactive x; return x; } } gamePersistence;
    struct { void Stop(){} } menuOverlay;
    struct Ble {
        bool enabled=true, connected=true, session=true, ota=false, override=false;
        bool configured=true, quiescent=true;
        uint32_t ingress=0U, notify=0U; unsigned buttonResets=0U;
        void ResetConnectionButtonState(){++buttonResets;}
        bool IsOtaInProgress()const{return ota;} bool IsConnected()const{return connected;}
        bool IsEnabled()const{return enabled;} bool IsAdvertising()const{return enabled&&!connected;}
        bool IsConnectionSessionActive()const{return enabled&&connected&&session;}
        bool QuiesceFactoryBleDut(){return quiescent;}
        void SetFactoryVerifierRuntimeAllowed(bool){} void SetFactoryVerifierConfiguredEnabled(bool v){configured=v;}
        void SetDiagnosticRadioOverride(bool a,bool v){override=a;if(a)enabled=v;}
        void Update(bool){if(!override)enabled=true;if(!enabled)connected=false;}
        uint32_t SessionNotifyCount()const{return notify;} uint32_t SessionIngressCount()const{return ingress;}
        bool RefreshDiagnosticPhysicalInputState(){if(!IsConnectionSessionActive())return false;++notify;return true;}
    } ble;
    struct Radio {
        void Update(bool,bool,modules::ManufacturingTestStage,Ble&,uint32_t){}
    } factoryRadio;
    bool quickSettingsOverlayActive=false,gameResetHoldActive=false,gameResetHoldTriggered=false;
    modules::UiOwner quickSettingsOverlayOwner=modules::UiOwner::P1;
    struct {void Reset(){}} quickSettingsOverlayController;
    modules::HintButtonGestureController hintButtonController{};
    modules::ResetHoldGestureController resetHoldController{};
    unsigned reseeds=0U;
    struct Gpio {unsigned* count; void SuppressHeldControlsUntilRelease(){++*count;}} gpioUserInput{&reseeds};
    struct {void Clear(){}} buttonInputQueue;
    bool settings=true,usbCcSampleValid=false;
    hardware::UsbCcLevel lastUsbCcAdvertisedLevel=hardware::UsbCcLevel::Unknown;
    struct {struct {hardware::BoardRevision revision=hardware::BoardRevision::V3S3;}resolved;} runtimeHardware;
    uint16_t ledPowerLimitMa=216U; modules::BudgetAwareLedPipelineStats ledPowerStats{};
    compliance_fixture::Leds leds;
    compliance_fixture::LedAnimation ledAnimation{leds};
    bool ledLastCommandCompleted = false;
    compliance_fixture::Leds& Leds(){return leds;}
    firmware::compliance::Session comptest;
};
FirmwareSystem* system=nullptr;
FirmwareSystem& GetSystem(){return *system;}
bool QuiesceLedRail(FirmwareSystem& sys);
bool EnableLedRail(FirmwareSystem& sys);
bool ClearLedOutput(FirmwareSystem& sys) {
    if (sys.ledAnimation.IsQuiescent() && !sys.powerRail.Is5vEnabled()) return true;
    sys.ledLastCommandCompleted = sys.ledAnimation.Submit(firmware::RawDieLedCommand{});
    return sys.ledLastCommandCompleted;
}
uint16_t GetActiveTotalCurrentLimitMa(const FirmwareSystem&){
    return modules::led_render_policy::MakePowerBudgetPolicyConfig({true,false}).maxTotalCurrentMa;
}
void RenderPhysicalFrameWithPowerBudget(FirmwareSystem& sys,const hardware::PhysicalLedFrame& frame,uint16_t scale){
    const auto config=firmware::compliance::LimitBudget(
        modules::led_render_policy::MakePowerBudgetPolicyConfig({true,false}),
        sys.usbCcSampleValid?sys.lastUsbCcAdvertisedLevel:hardware::UsbCcLevel::Unknown,
        sys.comptest.CurrentLoad().ceilingMa);
    modules::PowerBudgetPolicy policy(config);
    modules::BudgetAwareLedPipeline pipeline(sys.leds,policy,modules::led_render_policy::MakePipelineConfig(false,0U));
    pipeline.SetBrightnessScale01(scale/256.0f);pipeline.RenderFrame(frame.data(),sys.leds.GetLedCount());
    sys.ledPowerStats=pipeline.GetLastStats();
    sys.ledPowerLimitMa=std::min<uint16_t>(config.maxLedCurrentMa,config.maxTotalCurrentMa-config.fixedSystemCurrentMa);
}
}
void ComplianceFixtureDelay(unsigned long ms){fxe::ManualMillis::AdvanceMs(ms);}
namespace hardware { constexpr bool COMPLIANCE_FIXTURE_IS_S3=true, COMPLIANCE_FIXTURE_HAS_5V=true; }
#define ARDUINO 1
#define firmware_internal compliance_fixture
#define delay ComplianceFixtureDelay
#include "../../src/compliance_test_session.cpp"
#define HW_IS_S3 COMPLIANCE_FIXTURE_IS_S3
#define HW_HAS_5V_EN COMPLIANCE_FIXTURE_HAS_5V
#include "../../src/firmware_console_comptest.cpp"

#include "../../src/firmware_led_rail_quiesce.cpp"
#include "../../src/firmware_console_rail.cpp"
#undef HW_HAS_5V_EN
#undef HW_IS_S3
#undef delay
#undef firmware_internal
#undef ARDUINO

int ComplianceProductionJourney(){
    using namespace compliance_fixture;
    fxe::ScopedMillisProviderOverride clock{&fxe::ManualMillisProvider};fxe::ManualMillis::SetNowMs(0U);
    FirmwareSystem sys;compliance_fixture::system=&sys;
    test_support::OwnerPairingConsoleTransport transport;
    Console console(transport.hybrid);std::string out;
    console.RegisterCommand("comptest","production compliance session",[&](const Console::Args& args){return console_commands::ComptestCommand(console,args);});
    console.RegisterCommand("5ven", "production rail command", [&](const Console::Args& args){return console_commands::FiveVEnCommand(console,args);});
    auto send=[&](const char* line, bool remote=false){
        transport.usb.output.clear();
        (remote ? transport.ble.input : transport.usb.input) += std::string(line) + '\n';
        console.Update();out=transport.usb.output;
    };
    sys.gamePersistence.clock.running = true;
    send("comptest combined");
    Check(!sys.comptest.IsActive() && sys.leds.shows == 0U && sys.powerRail.on,
          "compliance entry cannot interrupt a running game clock");
    sys.gamePersistence.clock.running = false;
    send("comptest core", true);
    Check(sys.comptest.IsActive() && !sys.powerRail.on && !sys.ambientLights.on && !sys.ble.enabled,
          "Bluetooth Console can start the existing governed core load");
    if (sys.comptest.IsActive()) send("comptest stop");
    const auto priorReseeds = sys.reseeds;
    const auto priorButtonResets = sys.ble.buttonResets;
    sys.ble.connected = true;
    const auto initialShows = sys.leds.shows;
    send("5ven off", true);
    Check(!sys.powerRail.on && sys.leds.shows > initialShows,
          "Bluetooth Console rail-off still requires completed output quiescence");
    send("5ven on");
    const auto repeatedOnShows = sys.leds.shows;
    sys.recoveryFrameInitialized = true;
    send("\x1b[A");
    Check(sys.powerRail.on && !sys.ledAnimation.IsQuiescent() &&
              sys.leds.shows == repeatedOnShows && sys.recoveryFrameInitialized &&
              out.find("OK") != std::string::npos,
          "real Console history replays On without clearing live output or recovery cache");
    modules::HintButtonAction hintAction; modules::ResetHoldAction resetAction;
    sys.hintButtonController.HandleButtonEvent({modules::UserButton::P1O,fxe::Button::Event::Press,modules::UiOwner::P1},hintAction,0U);
    sys.resetHoldController.HandleButtonEvent({modules::UserButton::P2X,fxe::Button::Event::Press,modules::UiOwner::P2},resetAction,0U);
    sys.resetHoldController.HandleButtonEvent({modules::UserButton::P2O,fxe::Button::Event::Press,modules::UiOwner::P2},resetAction,0U);
    sys.leds.transferCompletes = false;
    send("5ven off");
    Check(sys.powerRail.on && out.find("LED clear incomplete") != std::string::npos &&
              out.find("OK") == std::string::npos,
          "real Console ingress rejects an unconfirmed clear without false OK or rail-off");
    sys.leds.transferCompletes = true;
    send("5ven off");
    Check(!sys.powerRail.on && sys.ledAnimation.IsQuiescent() && out.find("OK") != std::string::npos,
          "real Console ingress accepts confirmed output-owner quiescence before rail off");
    send("5ven on");
    Check(sys.powerRail.on && !sys.ledAnimation.IsQuiescent(),
          "real Console rail enable resumes the output owner");
    send("comptest combined");
    fxe::ManualMillis::AdvanceMs(2500U);
    Check(!sys.hintButtonController.Tick(hintAction) && !sys.resetHoldController.Tick(resetAction),
          "actual Console entry cancels armed gameplay holds before no-edge deadline expiry");
    Check(sys.comptest.IsActive()&&sys.powerRail.on&&!sys.ambientLights.on&&sys.ble.enabled,"Console preset takes one temporary production owner");
    send("comptest core", true);
    Check(!sys.comptest.CurrentLoad().rail && !sys.powerRail.on && !sys.ble.enabled,
          "Bluetooth Console can change the current governed load");
    send("comptest combined");
    // Core intentionally disabled BLE; the controlled transport now models
    // the owner's reconnect after USB restored the radio-enabled preset.
    sys.ble.connected = true;
    send("comptest status", true);
    Check(out.find("active=1") != std::string::npos,
          "Bluetooth retains read-only compliance status");
    sys.leds.transferCompletes = false;
    send("comptest core");
    Check(sys.comptest.CurrentLoad().rail && sys.powerRail.on && sys.ble.enabled &&
              out.find("clear incomplete") != std::string::npos,
          "denied powered clear retains the accepted load and rail/radio state");
    sys.leds.transferCompletes = true;
    Check(!sys.gamePersistence.boardSettings, "closing quick settings releases the persistence/clock owner");
    Check(sys.ledPowerStats.appliedLedCurrentMa + modules::S3_FIXED_SYSTEM_CURRENT_MA <=
              firmware::compliance::SourceCeilingMa(hardware::UsbCcLevel::Unknown),
          "combined diagnostic render retains its separate unknown-source ceiling");
    send("comptest status");
    Check(out.find("detected_usb=unknown")!=std::string::npos,"status reports unknown source truthfully");
    send("comptest ble traffic");fxe::ManualMillis::AdvanceMs(50U);sys.comptest.Update();
    Check(sys.ble.notify==1U,"traffic uses the existing shared physical-input state adapter");
    sys.ble.session=false;fxe::ManualMillis::AdvanceMs(50U);sys.comptest.Update();
    send("comptest status");Check(out.find("traffic_skipped=1")!=std::string::npos,"disconnected/inactive session does not claim transmitted traffic");
    send("comptest level 0");Check(sys.ledPowerStats.appliedLedCurrentMa==0U,"diagnostic zero removes LED load");
    send("comptest level 5");Check(sys.comptest.CurrentLoad().level==5U,"diagnostic five is accepted");
    send("comptest level 6");Check(sys.comptest.CurrentLoad().level==5U,"malformed input keeps active state");
    send("comptest rail off");const auto shows=sys.leds.shows;sys.comptest.Render();
    Check(!sys.powerRail.on&&sys.leds.shows==shows,"rail off drains then stays silent under regular render calls");
    send("comptest core");send("5ven on");
    Check(!sys.powerRail.on && out.find("stop comptest") != std::string::npos, "real competing rail command rejects mutation at Console ingress");
    Check(!sys.ble.enabled&&!sys.powerRail.on,"core preset shuts down optional LED/radio load");
    send("comptest stop");Check(!sys.comptest.IsActive()&&sys.powerRail.on&&sys.ambientLights.on&&sys.ble.enabled,"stop restores captured rail/ambient and runtime BLE policy");
    Check(sys.reseeds == priorReseeds + 1U && sys.ble.buttonResets == priorButtonResets + 1U, "comptest stop requests the production held-control reseed boundary");
    sys.hintButtonController.HandleButtonEvent({modules::UserButton::P1O,fxe::Button::Event::Press,modules::UiOwner::P1},hintAction,fxe::Millis());
    fxe::ManualMillis::AdvanceMs(1000U);
    Check(sys.hintButtonController.Tick(hintAction) && hintAction.type==modules::HintButtonActionType::BeginnerModeHold,
          "a fresh normal gesture works after diagnostic ownership is released");
    sys.hintButtonController.Reset();
    sys.powerRail.on=false;sys.ambientLights.on=false;send("comptest led");
    Check(sys.ledPowerStats.appliedLedCurrentMa>0U,"lit fixture records a nonzero LED load before restoration");
    sys.leds.transferCompletes=false;send("comptest stop");
    Check(sys.comptest.IsActive() && sys.powerRail.on && !sys.ambientLights.on &&
              out.find("clear incomplete") != std::string::npos,
          "unconfirmed exit clear retains ownership and permits an explicit retry");
    sys.leds.transferCompletes=true;
    send("comptest stop");
    Check(!sys.powerRail.on&&!sys.ambientLights.on,"stop preserves a preexisting diagnostic off state");
    Check(out.find("applied_led_ma=0")!=std::string::npos,
          "stopped Console status reports zero LED current on the restored off rail");
    sys.ble.quiescent=false;send("comptest combined");Check(!sys.comptest.IsActive(),"busy factory client refuses diagnostic entry");
    sys.ble.quiescent=true;
    send("comptest combined");
    const auto current = sys.comptest.CurrentLoad();
    send("comptest espnow");
    Check(sys.comptest.IsActive() && sys.comptest.CurrentLoad().level == current.level &&
          sys.comptest.CurrentLoad().ble == current.ble && out.find("comptest: start|core|rail|led|ble|combined") != std::string::npos,
          "removed radio preset rejects at actual Console ingress without changing the active load");
    send("comptest espnow on");
    Check(sys.comptest.CurrentLoad().ble == current.ble,
          "removed radio modifier cannot create a hidden diagnostic load");
    send("comptest stop");
    std::printf("[COMPLIANCE_PRODUCTION] Console/parser/session/LED-budget exercised; SDK RF and driver pins bypassed; failures=%d\n",failures);
    return failures;
}
