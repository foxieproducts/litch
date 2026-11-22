#include <array>
#include <cstdio>
#include "foxie_embedded/millis.hpp"
namespace gpio_exit_fixture {
std::array<bool, 64U> rawDown{};
int Read(unsigned pin) { return rawDown[pin] ? 0 : 1; }
void PinMode(unsigned, unsigned) {}
}
// Compile the shipping GPIO owner; only pin voltage/timing is controlled.
#define ARDUINO 1
#define LITCH_HW_VARIANT_S3 1
#define GpioUserInput ComplianceGpioUserInput
#define digitalRead gpio_exit_fixture::Read
#define pinMode gpio_exit_fixture::PinMode
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

int ComplianceGpioExitJourney() {
    using namespace gpio_exit_fixture;
    int failures = 0;
    auto check = [&](bool ok, const char* why) { if (!ok) { ++failures; std::printf("[FAIL] %s\n", why); } };
    fxe::ScopedMillisProviderOverride clock{&fxe::ManualMillisProvider};
    fxe::ManualMillis::SetNowMs(0U);
    const auto profile = hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3S3);
    hardware::ComplianceGpioUserInput input;
    input.ConfigureHardwareProfile(profile);
    input.Begin();
    modules::ButtonEvent event;
    auto drain = [&]() { unsigned count=0U; while(input.TryReadButtonEvent(event))++count; return count; };
    auto advance = [&](uint32_t delta) { fxe::ManualMillis::AdvanceMs(delta); return drain(); };
    rawDown[profile.p1.up]=true;
    advance(1U);
    check(advance(25U)==1U && event.button==modules::UserButton::Up && event.event==fxe::Button::Event::Press,
          "actual GPIO/Button owner admits the initial held direction");
    input.SuppressHeldControlsUntilRelease();
    for(unsigned step=0U;step<20U;++step)
        check(advance(100U)==0U, "factory/comptest exit reseed suppresses held direction repeats at unchanged orientation");
    rawDown[profile.p1.up]=false;advance(1U);
    check(advance(25U)==0U, "suppressed gesture release does not leak into normal action");
    rawDown[profile.p1.up]=true;advance(1U);
    check(advance(25U)==1U && event.event==fxe::Button::Event::Press,
          "release followed by a fresh press returns normal control admission");
    std::printf("[COMPLIANCE_GPIO_EXIT] shipping GPIO/Button raw-down reseed; pin voltage stubbed; failures=%d\n",failures);
    return failures;
}
