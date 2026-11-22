#include <array>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/quick_settings_overlay_controller.hpp"
#include "firmware/hardware_test_mode.hpp"
#include "firmware/manufacturing_test_led_frame.hpp"

namespace gpio_control_mapping_fixture {
std::array<bool, 64U> rawDown{};
std::array<unsigned, 64U> reads{}, modes{};
int Read(unsigned pin) { ++reads[pin]; return rawDown[pin] ? 0 : 1; }
void PinMode(unsigned pin, unsigned) { ++modes[pin]; }
}

// Exercise the shipping GPIO owner; only electrical levels/time are stubbed.
#define ARDUINO 1
#define LITCH_HW_VARIANT_S3 1
#define GpioUserInput MappingFixtureGpioUserInput
#define digitalRead gpio_control_mapping_fixture::Read
#define pinMode gpio_control_mapping_fixture::PinMode
#define LOW 0
#define INPUT_PULLUP 2
#include "../../src/gpio_user_input.cpp"
#undef INPUT_PULLUP
#undef LOW
#undef pinMode
#undef digitalRead
#undef GpioUserInput
#undef LITCH_HW_VARIANT_S3
#undef ARDUINO

namespace {
int g_failures = 0;
void Check(const bool ok, const char* message) {
    if (ok) return;
    ++g_failures;
    std::printf("[FAIL] %s\n", message);
}

void CheckUnusedContacts(const hardware::BoardRevision revision) {
    using namespace gpio_control_mapping_fixture;
    rawDown.fill(false); reads.fill(0U); modes.fill(0U);
    fxe::ManualMillis::SetNowMs(0U);
    hardware::MappingFixtureGpioUserInput input;
    input.ConfigureHardwareProfile(hardware::BoardHardwareProfileFor(revision));
    input.Begin();
    const uint8_t unusedP2 = revision == hardware::BoardRevision::V2S3 ? 18U : 16U;
    rawDown[8U] = rawDown[unusedP2] = true;
    modules::ButtonEvent event;
    for (uint8_t step=0U;step<10U;++step) {
        fxe::ManualMillis::AdvanceMs(20U);
        Check(!input.TryReadButtonEvent(event), "unused physical contacts emit no events");
    }
    Check(reads[8U]==0U && reads[unusedP2]==0U && modes[8U]==0U && modes[unusedP2]==0U &&
              input.AreCachedControlsReleased(),
          "shipping GPIO owner neither configures nor samples unused contacts or counts them as held");
    rawDown[7U] = true;
    fxe::ManualMillis::AdvanceMs(1U);
    (void)input.TryReadButtonEvent(event);
    fxe::ManualMillis::AdvanceMs(20U);
    const bool admitted = input.TryReadButtonEvent(event) && event.button==modules::UserButton::Up;
    Check(admitted, "held unused contacts cannot suppress a real directional action");
    std::printf("[UNUSED_CONTACT_GPIO] revision=%u unused_reads=%u,%u unused_modes=%u,%u real_up=%u\n",
        unsigned(revision), reads[8U], reads[unusedP2], modes[8U], modes[unusedP2], admitted);
}

void CheckContacts(const hardware::BoardRevision revision,
                   const bool rotateOwners) {
    using modules::UserButton;
    using modules::UiOwner;
    using modules::QuickSettingsActionType;
    using namespace gpio_control_mapping_fixture;
    rawDown.fill(false);
    fxe::ManualMillis::SetNowMs(0U);
    hardware::MappingFixtureGpioUserInput input;
    input.ConfigureHardwareProfile(hardware::BoardHardwareProfileFor(revision));
    input.SetRotate180(rotateOwners);
    input.Begin();
    modules::QuickSettingsOverlayController menu;
    const std::array<UserButton, 4U> buttons{
        UserButton::Up, UserButton::Down, UserButton::Left, UserButton::Right};
    const std::array<QuickSettingsActionType, 4U> actions{
        QuickSettingsActionType::AdjustIncrease,
        QuickSettingsActionType::AdjustDecrease,
        QuickSettingsActionType::SelectPrevious,
        QuickSettingsActionType::SelectNext};
    const std::array<uint8_t, 4U> p1Pins{7U, 6U, 5U, 9U};
    // Independent electrical expectations, rather than pins read back from
    // the implementation under test. V3.3: shipped 2026-09-23 probe nets
    // SW5/7/6/12 -> U2 pads19/18/17/21 -> GPIO15/14/13/17.
    const std::array<uint8_t, 4U> p2Pins =
        revision == hardware::BoardRevision::V2S3
            ? std::array<uint8_t, 4U>{17U, 16U, 15U, 21U}
            : revision == hardware::BoardRevision::V3S3
                  ? std::array<uint8_t, 4U>{14U, 15U, 17U, 13U}
                  : std::array<uint8_t, 4U>{15U, 14U, 13U, 17U};
    for (const bool physicalP2 : {false, true}) {
        const auto& pins = physicalP2 ? p2Pins : p1Pins;
        const UiOwner owner = physicalP2 != rotateOwners ? UiOwner::P2 : UiOwner::P1;
        for (size_t direction = 0U; direction < pins.size(); ++direction) {
            modules::ButtonEvent event;
            const bool sideways = direction >= 2U;
            if (sideways) fxe::ManualMillis::AdvanceMs(250U);
            rawDown[pins[direction]] = true;
            fxe::ManualMillis::AdvanceMs(1U);
            Check(!input.TryReadButtonEvent(event), "raw contact waits for debounce");
            fxe::ManualMillis::AdvanceMs(20U);
            const bool pressed = input.TryReadButtonEvent(event);
            Check(pressed && event.button == buttons[direction] &&
                      event.owner == owner && event.event == fxe::Button::Event::Press,
                  "PCB contact produces the user-local direction and current owner");
            modules::QuickSettingsAction action;
            Check(pressed && menu.HandleButtonEvent(event, true, action) &&
                      action.type == actions[direction] && action.owner == owner && !action.isRepeat,
                  "actual GPIO Press immediately adjusts or selects the mapped direction");
            Check(!input.TryReadButtonEvent(event), "one contact admits exactly one press");
            const auto repeatAction = sideways ? QuickSettingsActionType::None : actions[direction];
            fxe::ManualMillis::AdvanceMs(249U);
            Check(!input.TryReadButtonEvent(event), "249ms hold has no input repeat");
            fxe::ManualMillis::AdvanceMs(1U);
            Check(input.TryReadButtonEvent(event) && event.event == fxe::Button::Event::Repeat &&
                      event.button == buttons[direction] && event.owner == owner &&
                      menu.HandleButtonEvent(event, true, action) && action.type == repeatAction,
                  "actual GPIO first Repeat adjusts vertically and never selects sideways");
            const uint32_t repeatRateMs = sideways ? 50U : 19U;
            fxe::ManualMillis::AdvanceMs(repeatRateMs - 1U);
            Check(!input.TryReadButtonEvent(event), "held contact does not repeat before its normal cadence");
            fxe::ManualMillis::AdvanceMs(1U);
            Check(input.TryReadButtonEvent(event) && event.event == fxe::Button::Event::Repeat &&
                      event.button == buttons[direction] && event.owner == owner &&
                      menu.HandleButtonEvent(event, true, action) && action.type == repeatAction,
                  "later GPIO Repeat preserves vertical mapping without repeated side selection");
            rawDown[pins[direction]] = false;
            fxe::ManualMillis::AdvanceMs(1U);
            Check(!input.TryReadButtonEvent(event), "release waits for debounce");
            fxe::ManualMillis::AdvanceMs(20U);
            Check(input.TryReadButtonEvent(event) &&
                      event.event == fxe::Button::Event::Release &&
                      event.button == buttons[direction] && event.owner == owner,
                  "release retains contact identity without a delayed action");
            Check(menu.HandleButtonEvent(event, true, action) && action.type == QuickSettingsActionType::None,
                  "GPIO release causes no adjustment or selection");
            Check(!input.TryReadButtonEvent(event), "no extra release event");
            std::printf("[GPIO_DIRECTION_CHECKPOINT] revision=%u physical_bank=%s "
                        "owner_swap=%u gpio=%u direction=%u action=%u next=release-and-next-contact\n",
                        static_cast<unsigned>(revision), physicalP2 ? "P2" : "P1",
                        rotateOwners ? 1U : 0U, pins[direction],
                        static_cast<unsigned>(buttons[direction]),
                        static_cast<unsigned>(actions[direction]));
            if (sideways) continue;

            for (const uint32_t guardAgeMs : {239U, 250U}) {
                rawDown[pins[direction]] = true;
                fxe::ManualMillis::AdvanceMs(1U);
                Check(!input.TryReadButtonEvent(event), "guard trigger waits for GPIO debounce");
                fxe::ManualMillis::AdvanceMs(20U);
                Check(input.TryReadButtonEvent(event) && event.button == buttons[direction] &&
                          event.owner == owner && event.event == fxe::Button::Event::Press &&
                          menu.HandleButtonEvent(event, true, action) && action.type == actions[direction],
                      "mapped vertical Press starts the side guard");
                rawDown[pins[direction]] = false;
                fxe::ManualMillis::AdvanceMs(1U);
                Check(!input.TryReadButtonEvent(event), "guard trigger release waits for debounce");
                fxe::ManualMillis::AdvanceMs(20U);
                Check(input.TryReadButtonEvent(event) && event.event == fxe::Button::Event::Release &&
                          menu.HandleButtonEvent(event, true, action) && action.type == QuickSettingsActionType::None,
                      "vertical release retains the guard without another adjustment");
                // The release consumed 21ms; the fresh side debounce consumes 20ms.
                fxe::ManualMillis::AdvanceMs(guardAgeMs - 41U);
                rawDown[pins[3U]] = true;
                Check(!input.TryReadButtonEvent(event), "guarded side Press waits for GPIO debounce");
                fxe::ManualMillis::AdvanceMs(20U);
                const auto expected = guardAgeMs < 250U ? QuickSettingsActionType::None
                                                        : QuickSettingsActionType::SelectNext;
                Check(input.TryReadButtonEvent(event) && event.button == UserButton::Right &&
                          event.owner == owner && event.event == fxe::Button::Event::Press &&
                          menu.HandleButtonEvent(event, true, action) && action.type == expected,
                      "mapped GPIO side Press is rejected at 239ms and accepted at 250ms after vertical input");
                std::printf("[GPIO_GUARD_CHECKPOINT] revision=%u physical_bank=%s owner_swap=%u "
                            "vertical_gpio=%u side_gpio=%u guard_age_ms=%u action=%u expected=%u next=release-side\n",
                            static_cast<unsigned>(revision), physicalP2 ? "P2" : "P1", rotateOwners,
                            pins[direction], pins[3U], guardAgeMs,
                            static_cast<unsigned>(action.type), static_cast<unsigned>(expected));
                fxe::ManualMillis::AdvanceMs(250U);
                Check(input.TryReadButtonEvent(event) && event.event == fxe::Button::Event::Repeat &&
                          menu.HandleButtonEvent(event, true, action) && action.type == QuickSettingsActionType::None,
                      "mapped held side cannot select or replay an ignored Press after guard expiry");
                rawDown[pins[3U]] = false;
                fxe::ManualMillis::AdvanceMs(1U);
                Check(!input.TryReadButtonEvent(event), "guarded side release waits for debounce");
                fxe::ManualMillis::AdvanceMs(20U);
                Check(input.TryReadButtonEvent(event) && event.event == fxe::Button::Event::Release &&
                          menu.HandleButtonEvent(event, true, action) && action.type == QuickSettingsActionType::None,
                      "guarded side release adds no selection");
            }
        }
    }
}
void CheckFactoryContacts(const hardware::BoardRevision revision) {
    using namespace gpio_control_mapping_fixture;
    using B=modules::UserButton;using O=modules::UiOwner;
    rawDown.fill(false);fxe::ManualMillis::SetNowMs(0U);
    hardware::MappingFixtureGpioUserInput input;input.ConfigureHardwareProfile(hardware::BoardHardwareProfileFor(revision));input.SetRotate180(false);input.Begin();
    firmware::HardwareTestMode mode;mode.Enter(revision,0U);
    mode.ReportLightSensorLinks(true,true,0U);
    if(hardware::HasFourLedSquares(revision))mode.ReportMotionPresence(true,0U);
    const auto edge=[&](unsigned pin,bool down){
        modules::ButtonEvent e;rawDown[pin]=down;fxe::ManualMillis::AdvanceMs(1U);(void)input.TryReadButtonEvent(e);fxe::ManualMillis::AdvanceMs(20U);
        const bool got=input.TryReadButtonEvent(e);Check(got,"factory entry uses real shipping GPIO debounce");if(got)mode.OnButtonEvent(e,fxe::Millis());
    };
    for(unsigned field=0U;field<4U;++field){edge(10U,true);edge(10U,false);}
    struct Hall final:modules::IBoardInput{uint64_t occupied=UINT64_MAX;bool TryReadSnapshot(uint64_t& out)override{out=occupied;return true;}bool TryReadDelta(modules::SquareDelta&)override{return false;}void Resync()override{}} hall;
    mode.UpdateSensors(hall,fxe::Millis());hall.occupied=0ULL;mode.UpdateSensors(hall,fxe::Millis());edge(10U,true);edge(10U,false);
    Check(mode.GetStep()==firmware::HardwareTestMode::Step::Buttons,"shipping GPIO navigation reaches actual factory Buttons renderer");
    constexpr uint8_t footprints[12][2]={{35U,36U},{59U,60U},{50U,42U},{53U,45U},{57U,255U},{62U,255U},
        {27U,28U},{3U,4U},{13U,21U},{10U,18U},{6U,255U},{1U,255U}};
    const std::array<unsigned,6U> p1{7U,6U,5U,9U,0U,10U};
    const std::array<unsigned,6U> p2=revision==hardware::BoardRevision::V2S3?std::array<unsigned,6U>{17U,16U,15U,21U,14U,26U}
        :revision==hardware::BoardRevision::V3S3?std::array<unsigned,6U>{14U,15U,17U,13U,12U,18U}:std::array<unsigned,6U>{15U,14U,13U,17U,12U,18U};
    for(unsigned bank=0U;bank<2U;++bank){
        const auto& pins=bank?p2:p1;const O owner=bank?O::P2:O::P1;
        const std::array<B,6U> names{B::Up,B::Down,B::Left,B::Right,bank?B::P2X:B::P1X,bank?B::P2O:B::P1O};
        for(unsigned c=0U;c<6U;++c){
            modules::ButtonEvent e;rawDown[pins[c]]=true;fxe::ManualMillis::AdvanceMs(1U);(void)input.TryReadButtonEvent(e);fxe::ManualMillis::AdvanceMs(20U);
            const bool press=input.TryReadButtonEvent(e);Check(press&&e.button==names[c]&&e.owner==owner&&e.event==fxe::Button::Event::Press,"independent GPIO pad/net expectation reaches exact factory button and end");
            if(press)mode.OnButtonEvent(e,fxe::Millis());
            firmware::ManufacturingTestFrameInputs frameInput;frameInput.revision=revision;frameInput.step=mode.GetStep();frameInput.progress=mode.GetProgress();
            const auto frame=firmware::ComposeManufacturingTestLedFrame(frameInput);const auto topology=hardware::BoardLedTopologyFor(revision);
            bool correct=true;for(uint8_t square:footprints[bank*6U+c])if(square<64U){
                const auto left=frame[square*topology.physicalLedsPerSquare],right=frame[square*topology.physicalLedsPerSquare+2U];
                correct&=left.r==160U&&left.g==160U&&left.b==160U&&left.r==right.r&&left.g==right.g&&left.b==right.b;
                if(topology.physicalLedsPerSquare==4U)correct&=frame[square*4U+1U].g==0U&&frame[square*4U+3U].g==0U;
            }
            Check(correct,"actual independently named GPIO contact lights both outward square LEDs in its player-facing factory footprint");
            std::printf("[GPIO_FACTORY_FOOTPRINT] revision=%u bank=%u gpio=%u control=%u squares=%u/%u side_slots=0/2 pair=%u pieces_dark=%u\n",unsigned(revision),bank+1U,pins[c],unsigned(names[c]),footprints[bank*6U+c][0],footprints[bank*6U+c][1],correct,correct);
            rawDown[pins[c]]=false;fxe::ManualMillis::AdvanceMs(1U);(void)input.TryReadButtonEvent(e);fxe::ManualMillis::AdvanceMs(20U);
            const bool release=input.TryReadButtonEvent(e);Check(release&&e.owner==owner&&e.event==fxe::Button::Event::Release,"actual factory contact release retains independent physical end");
            if(release)mode.OnButtonEvent(e,fxe::Millis());
            frameInput.step=mode.GetStep();frameInput.progress=mode.GetProgress();const auto releasedFrame=firmware::ComposeManufacturingTestLedFrame(frameInput);
            bool off=true;for(uint8_t square:footprints[bank*6U+c])if(square<64U)for(uint8_t side:{0U,2U}){
                const auto pixel=releasedFrame[square*topology.physicalLedsPerSquare+side];off&=c<4U||mode.GetStep()!=firmware::HardwareTestMode::Step::Buttons?!pixel.r&&!pixel.g&&!pixel.b:pixel.r==0U&&pixel.g>0U&&pixel.b==0U;
            }
            Check(off,"real GPIO release clears directions; verified X/O stays green only while Buttons owns the screen");
            std::printf("[GPIO_FACTORY_CHECKPOINT] revision=%u bank=%u gpio=%u physical_control=%u observed=%04x held=%04x channel=%u next=next-contact\n",unsigned(revision),bank+1U,pins[c],unsigned(names[c]),mode.GetProgress().observedButtonMask,mode.GetProgress().pressedButtonMask,mode.GetLedChannel());
        }
    }
    Check(mode.GetProgress().observedButtonMask==0x37EFU&&mode.GetProgress().pressedButtonMask==0U,"shipping GPIO paths record all twelve physical controls distinctly");
}

}

int main() {
    fxe::ScopedMillisProviderOverride clock{&fxe::ManualMillisProvider};
    for (const auto revision:{hardware::BoardRevision::V2S3,hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3})
        CheckUnusedContacts(revision);
    for (const auto revision : {hardware::BoardRevision::V2S3,
                               hardware::BoardRevision::V3S3,
                               hardware::BoardRevision::V3_3S3}) {
        CheckFactoryContacts(revision);
        CheckContacts(revision, false);
        CheckContacts(revision, true);
    }
    std::printf("[GPIO_CONTROL_MAPPING] failures=%d; seam=shipping-GPIO-debounce-menu; "
                "bypass=electrical-levels; V3.3-expectations=shipped-probe-and-schematic\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
