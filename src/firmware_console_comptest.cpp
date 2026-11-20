#ifdef ARDUINO
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"

namespace firmware_internal::console_commands {
bool ComptestCommand(Console& console, const Console::Args& args) {
    if (!hardware::HW_IS_S3 || args.argc > 3) {
        console.PrintLn("comptest: ESP32-S3 only; use comptest <action> [value]");
        return false;
    }
    const auto action = args.Has(1) ? args.Get(1) : std::string_view("status");
    return GetSystem().comptest.Command(console, action,
        args.Has(2) ? args.Get(2) : std::string_view());
}
} // namespace firmware_internal::console_commands
#endif
