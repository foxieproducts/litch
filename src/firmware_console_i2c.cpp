#ifdef ARDUINO

#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"
#include "hardware/i2c_clock_settings.hpp"

namespace firmware_internal::console_commands {
namespace {
constexpr const char* I2C_USAGE =
    "usage: i2c <status|set <100k|400k|1m|100000|400000|1000000>> (1m experimental)";
}

bool I2cCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();
    const std::string_view sub = args.Has(1) ? args.Get(1) : std::string_view("status");

    if (sub == "status" && args.argc <= 2) {
        console.PrintLn("i2c: clock_hz=%lu", static_cast<unsigned long>(sys.i2cClockHz));
        return true;
    }

    if (sub == "set") {
        if (args.argc != 3) {
            console.PrintLn("%s", I2C_USAGE);
            return false;
        }

        uint32_t hz = 0U;
        if (!hardware::i2c_clock_settings::TrySelect(sys.settings, args.Get(2), hz)) {
            console.PrintLn("i2c: invalid rate '%.*s'", static_cast<int>(args.Get(2).size()), args.Get(2).data());
            console.PrintLn("%s", I2C_USAGE);
            return false;
        }

        // Diagnostic command: retain the existing Console/session transport
        // and I2C owner; persistence uses the ordinary deferred settings lane.
        SetSystemI2cClock(sys, hz);
        console.PrintLn("i2c: clock_hz=%lu; %s",
                        static_cast<unsigned long>(sys.i2cClockHz),
                        sys.settings.IsDisabled()
                            ? "session only; saving unavailable"
                            : "save deferred");
        return true;
    }

    console.PrintLn("%s", I2C_USAGE);
    return false;
}

} // namespace firmware_internal::console_commands

#endif
