#include <cstdio>
#include <string>

#include "console.hpp"
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"
#include "hardware/i2c_clock_settings.hpp"
#include "owner_pairing_console_test_transport.hpp"

namespace i2c_console_fixture {
struct FirmwareSystem {
    fxe::SettingsStore settings{{}};
    uint32_t i2cClockHz = 100000U;
    unsigned applied = 0U;
};
FirmwareSystem* system = nullptr;
FirmwareSystem& GetSystem() { return *system; }
void SetSystemI2cClock(FirmwareSystem& sys, uint32_t hz) {
    sys.i2cClockHz = hz;
    ++sys.applied;
}
} // namespace i2c_console_fixture

// The production handler and parser run; only the physical I2C adapter is stubbed.
#define ARDUINO 1
#define firmware_internal i2c_console_fixture
#include "../../src/firmware_console_i2c.cpp"
#undef firmware_internal
#undef ARDUINO

int I2cConsoleJourney() {
    i2c_console_fixture::FirmwareSystem sys;
    i2c_console_fixture::system = &sys;
    test_support::OwnerPairingConsoleTransport transport;
    Console console(transport.hybrid);
    bool accepted = false;
    console.RegisterCommand("i2c", "production I2C handler", [&](const Console::Args& args) {
        accepted = i2c_console_fixture::console_commands::I2cCommand(console, args);
        return accepted;
    });
    auto send = [&](const char* text, bool remote) {
        (remote ? transport.ble.input : transport.usb.input) += std::string(text) + '\n';
        console.Update();
        return accepted;
    };
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::printf("[FAIL] %s\n", message); }
    };
    for (const char* spelling : {"i2c set 1m", "i2c set 1000000", "i2c set 1M"}) {
        check(send(spelling, true) && sys.i2cClockHz == 1000000U &&
                  hardware::i2c_clock_settings::Read(sys.settings) == 1000000U,
              "experimental aliases retain normal validation over Bluetooth");
    }
    check(send("i2c set 400k", true) && sys.i2cClockHz == 400000U,
          "rated 400 kHz remains available over Bluetooth");
    transport.usb.input += "i2c set "; console.Update();
    transport.ble.input += "1m\n"; console.Update();
    check(accepted && sys.i2cClockHz == 1000000U,
          "valid mixed-source Console lines use the same rate validation");
    check(send("i2c set 1m", false) && sys.i2cClockHz == 1000000U &&
              hardware::i2c_clock_settings::Read(sys.settings) == 1000000U,
          "fresh USB command retains experimental rate and normal deferred-save behavior");
    const auto beforeRecall = sys.applied;
    transport.usb.output.clear();
    transport.usb.input += "\x1b[A\n";
    console.Update();
    check(accepted && sys.applied == beforeRecall + 1U &&
              sys.i2cClockHz == 1000000U &&
              transport.usb.output.find("fresh USB Console line") == std::string::npos,
          "history-recalled Console command remains usable");
    check(send("i2c set 1m", false) && sys.applied == beforeRecall + 2U,
          "fresh and history-recalled Console commands share validation");
    check(send("i2c status", true), "Bluetooth keeps rate inspection");
    check(send("i2c set 100k", true) && sys.i2cClockHz == 100000U,
          "Bluetooth can restore the conservative rated rate");
    std::printf("[I2C_CONSOLE] failures=%d; GPIO/I2C adapter stubbed\n", failures);
    return failures;
}
