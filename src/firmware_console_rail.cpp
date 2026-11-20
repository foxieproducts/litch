#ifdef ARDUINO
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"
namespace firmware_internal::console_commands {
bool FiveVEnCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();
    if (sys.comptest.IsActive()) { console.PrintLn("stop comptest before changing its hardware lane"); return false; }

    if (!hardware::HW_HAS_5V_EN) {
        console.PrintLn("5ven: unsupported on this hardware");
        return false;
    }

    if (args.argc != 2) {
        console.PrintLn("usage: 5ven <on|off>");
        return false;
    }

    // Diagnostic compatibility exception: direct rail control is volatile
    // hardware bring-up state with immediate acknowledgement and no persistence.
    const std::string_view mode = args.Get(1);
    if (mode == "on") {
        if (!EnableLedRail(sys)) {
            console.PrintLn("5ven: output handoff incomplete; retry");
            return false;
        }
        console.PrintLn("OK");
        return true;
    }
    if (mode == "off") {
        if (!QuiesceLedRail(sys)) {
            console.PrintLn("5ven: LED clear incomplete; rail remains on; retry");
            return false;
        }
        console.PrintLn("OK");
        return true;
    }

    console.PrintLn("usage: 5ven <on|off>");
    return false;
}

}
#endif
