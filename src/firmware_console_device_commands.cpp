#ifdef ARDUINO

#include <Arduino.h>

#include "firmware/firmware_console_internal.hpp"

#include "firmware/firmware_internal.hpp"

#include "modules/initial_fen.hpp"
#include "modules/string_parsing.hpp"

namespace {

constexpr uint16_t SET_LED_BRIGHTNESS_SCALE256 = 256U;

void RenderConsoleLedDiagnosticFrame(
    firmware_internal::FirmwareSystem& sys) {
    firmware_internal::RenderPhysicalFrameWithPowerBudget(
        sys,
        sys.consoleLedDiagnosticFrame.GetFrame(),
        SET_LED_BRIGHTNESS_SCALE256);
}

void PrintSetLedResult(Console& console,
                       const firmware_internal::FirmwareSystem& sys) {
    console.PrintLn(
        "OK requested_led_ma=%u applied_led_ma=%u limit_ma=%u",
        static_cast<unsigned>(sys.ledPowerStats.estimatedLedCurrentMa),
        static_cast<unsigned>(sys.ledPowerStats.appliedLedCurrentMa),
        static_cast<unsigned>(sys.ledPowerLimitMa));
}

bool SetConsoleBoardLed(Console& console,
                        const Console::Args& args,
                        firmware_internal::FirmwareSystem& sys) {
    uint16_t index = 0U;
    if (!firmware_internal::console_commands::TryParseU16(args.Get(2), index)) {
        console.PrintLn("setled: invalid index");
        return false;
    }

    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;
    if (!modules::string_parsing::TryParseRgbHex(args.Get(3), r, g, b)) {
        console.PrintLn("setled: invalid color (expected RRGGBB)");
        return false;
    }

    if (!sys.consoleLedDiagnosticFrame.SetBoardLed(
            index, modules::Rgb{r, g, b})) {
        console.PrintLn(
            "setled: index out of range (0..%u)",
            static_cast<unsigned>(hardware::BOARD_LED_COUNT - 1U));
        return false;
    }

    RenderConsoleLedDiagnosticFrame(sys);
    PrintSetLedResult(console, sys);
    return true;
}

} // namespace

namespace firmware_internal::console_commands {

bool SetLedCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();
    if (sys.comptest.IsActive()) { console.PrintLn("stop comptest before changing its hardware lane"); return false; }

    // Diagnostic compatibility exception: this bounded frame intentionally
    // bypasses semantic gameplay rendering and never mutates settings.
    if (args.argc == 2 && args.Get(1) == std::string_view("clear")) {
        sys.consoleLedDiagnosticFrame.Clear();
        RenderConsoleLedDiagnosticFrame(sys);
        PrintSetLedResult(console, sys);
        return true;
    }

    if (args.argc != 4 || args.Get(1) != std::string_view("board")) {
        console.PrintLn("usage: setled <board <index> <RRGGBB>|clear>");
        return false;
    }

    return SetConsoleBoardLed(console, args, sys);
}

bool AmbientCommand(Console& console, const Console::Args& args) {
    if (!hardware::HW_HAS_LIGHT_SENSORS) {
        console.PrintLn("light sensors: not present on this target");
        return true;
    }

    auto& ambient = GetSystem().ambientLights;
    if (args.argc > 2) {
        console.PrintLn("light sensors: usage: ambient [status|on|off]");
        return false;
    }

    const std::string_view sub = (args.argc == 2) ? args.Get(1) : std::string_view("status");
    if (!(sub == "status" || sub == "on" || sub == "off")) {
        console.PrintLn("light sensors: usage: ambient [status|on|off]");
        return false;
    }

    if (GetSystem().comptest.IsActive() && sub != "status") {
        console.PrintLn("ambient: stop comptest first"); return false;
    }
    // Diagnostic compatibility exception: sensor power is volatile bring-up
    // state and is not a registered setting or board-equivalent action.
    if (sub == "on") {
        ambient.PowerOn();
    } else if (sub == "off") {
        ambient.ShutDown();
    }

    console.PrintLn("light sensors: init=%u powered=%u s0_ok=%u s1_ok=%u has_sample=%u",
                    static_cast<unsigned>(ambient.IsInitialized() ? 1U : 0U),
                    static_cast<unsigned>(ambient.IsPoweredOn() ? 1U : 0U),
                    static_cast<unsigned>(ambient.Sensor0Ok() ? 1U : 0U),
                    static_cast<unsigned>(ambient.Sensor1Ok() ? 1U : 0U),
                    static_cast<unsigned>(ambient.HasSample() ? 1U : 0U));

    if (ambient.HasSample()) {
        console.PrintLn("light sensors: lux=%.2f lux0=%lu lux1=%lu valid=%u age_ms=%lu",
                        static_cast<double>(ambient.GetLastLux()),
                        static_cast<unsigned long>(ambient.GetLastLux0()),
                        static_cast<unsigned long>(ambient.GetLastLux1()),
                        static_cast<unsigned>(ambient.GetLastValidCount()),
                        static_cast<unsigned long>(ambient.GetLastSampleAgeMs()));
    }

    return true;
}

bool FlipCommand(Console& console, const Console::Args&) {
    auto& sys = GetSystem();
    auto& settings = sys.settings;

    const bool next = !GetFlipSidesSetting(settings);
    StaticJsonDocument<16> value;
    value.set(next);
    const modules::ControlMutationOutcome outcome =
        sys.controlMutation.MutateSetting(
            "BOARD_ROTATE", value.as<JsonVariantConst>());
    if (outcome.status == modules::ControlMutationStatus::Rejected) {
        console.PrintLn("rotate: blocked (%s)",
                        modules::ControlMutationReasonToString(outcome.reason));
        return false;
    }
    console.PrintLn("rotate=%s (save queued)", next ? "true" : "false");

    if (next) {
        console.PrintLn("rotate: rotate the physical board 180 degrees");
        console.PrintLn("rotate: swap king/queen on each side (starting position)");
        console.PrintLn(
            "rotate: physical control banks now emit the opposite logical P1/P2 owner");
    }
    return true;
}

bool FirstPlayerCommand(Console& console, const Console::Args&) {
    auto& sys = GetSystem();
    auto& settings = sys.settings;

    const uint32_t cur = (settings.ReadValue("GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U) ? 2U : 1U;
    const uint32_t next = (cur == 2U) ? 1U : 2U;
    StaticJsonDocument<16> value;
    value.set(next);
    const modules::ControlMutationOutcome outcome =
        sys.controlMutation.MutateSetting(
            "GAME_FIRST_PLAYER", value.as<JsonVariantConst>());
    if (outcome.status == modules::ControlMutationStatus::Rejected) {
        console.PrintLn("firstplayer: blocked (%s)",
                        modules::ControlMutationReasonToString(outcome.reason));
        return false;
    }
    console.PrintLn("firstplayer=%u (save queued)", static_cast<unsigned>(next));
    return true;
}

} // namespace firmware_internal::console_commands

#endif
