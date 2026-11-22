#include "modules/motion_console_commands.hpp"

#include <cstdlib>
#include <cstring>
#include <string_view>

#include "firmware/motion_wake_controller.hpp"
#include "hardware/lis2dw12_settings.hpp"
#include "modules/settings_store.hpp"

namespace modules {
namespace {

const char* ModeName(const hardware::Lis2dw12WakeMode mode) {
    switch (mode) {
        case hardware::Lis2dw12WakeMode::Motion:
            return "motion";
        case hardware::Lis2dw12WakeMode::Tap:
            return "tap";
        case hardware::Lis2dw12WakeMode::MotionAndTap:
            return "motion+tap";
    }
    return "unknown";
}

bool ParseSampleCount(const Console::Args& args, uint16_t& count) {
    count = 16U;
    if (!args.Has(2)) {
        return true;
    }

    const char* text = args.CStr(2);
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (end == text || end == nullptr || *end != '\0' ||
        parsed < 4UL || parsed > 64UL) {
        return false;
    }
    count = static_cast<uint16_t>(parsed);
    return true;
}

size_t AddCompletion(const char* candidate,
                     const char* prefix,
                     const char** matches,
                     const size_t count,
                     const size_t capacity) {
    const size_t prefixLength = prefix == nullptr ? 0U : std::strlen(prefix);
    if (prefixLength > 0U &&
        std::strncmp(candidate, prefix, prefixLength) != 0) {
        return count;
    }
    if (matches != nullptr && count < capacity) {
        matches[count] = candidate;
    }
    return count + 1U;
}

size_t CompleteMotion(const size_t argumentIndex,
                      const char* prefix,
                      const char** matches,
                      const size_t capacity) {
    if (argumentIndex != 1U) {
        return 0U;
    }

    static constexpr const char* COMMANDS[] = {
        "status",
        "sample",
        "events",
        "calibrate",
        "apply",
    };
    size_t count = 0U;
    for (const char* command : COMMANDS) {
        count = AddCompletion(command, prefix, matches, count, capacity);
    }
    return count;
}

bool PrintStatus(MotionConsoleCommandContext& context) {
    auto& controller = *context.controller;
    const auto& sensor = controller.GetSensor();
    const auto& diagnostics = controller.GetDiagnostics();
    const hardware::Lis2dw12Config requested =
        hardware::lis2dw12_settings::Read(*context.settings);
    const hardware::Lis2dw12AppliedConfig& applied =
        sensor.GetAppliedConfig();

    context.console->PrintLn(
        "motion: supported=%u available=%u wake_ready=%u pin=%u id=0x%02x error=%u int=%s",
        static_cast<unsigned>(controller.IsSupported() ? 1U : 0U),
        static_cast<unsigned>(controller.IsAvailable() ? 1U : 0U),
        static_cast<unsigned>(controller.IsWakeReady() ? 1U : 0U),
        static_cast<unsigned>(controller.GetInterruptPin()),
        static_cast<unsigned>(sensor.GetDeviceId()),
        static_cast<unsigned>(sensor.GetLastError()),
        controller.IsInterruptReleased() ? "high" : "low");
    context.console->PrintLn(
        "motion: requested enabled=%u mode=%s threshold_mg=%u duration_ms=%u tap_threshold_mg=%u tap_shock_ms=%u",
        static_cast<unsigned>(requested.enabled ? 1U : 0U),
        ModeName(requested.mode),
        static_cast<unsigned>(requested.motionThresholdMg),
        static_cast<unsigned>(requested.motionDurationMs),
        static_cast<unsigned>(requested.tapThresholdMg),
        static_cast<unsigned>(requested.tapShockMs));
    context.console->PrintLn(
        "motion: applied enabled=%u mode=%s odr_hz=%u.%u threshold_mg=%u duration_ms=%u tap_threshold_mg=%u tap_shock_ms=%u experimental=%u",
        static_cast<unsigned>(applied.enabled ? 1U : 0U),
        ModeName(applied.mode),
        static_cast<unsigned>(applied.odrTenthsHz / 10U),
        static_cast<unsigned>(applied.odrTenthsHz % 10U),
        static_cast<unsigned>(applied.motionThresholdMg),
        static_cast<unsigned>(applied.motionDurationMs),
        static_cast<unsigned>(applied.tapThresholdMg),
        static_cast<unsigned>(applied.tapShockMs),
        static_cast<unsigned>(applied.tapExperimental ? 1U : 0U));
    context.console->PrintLn(
        "motion: configs=%lu events=%lu errors=%lu stuck_low=%lu",
        static_cast<unsigned long>(diagnostics.configurationCount),
        static_cast<unsigned long>(diagnostics.eventCount),
        static_cast<unsigned long>(diagnostics.errorCount),
        static_cast<unsigned long>(diagnostics.stuckLowCount));
    return true;
}

bool PrintSample(MotionConsoleCommandContext& context) {
    hardware::Lis2dw12Sample sample;
    if (!context.controller->GetSensor().ReadSample(sample)) {
        context.console->PrintLn("FAIL: accelerometer sample unavailable");
        return false;
    }
    context.console->PrintLn(
        "motion: raw=%d,%d,%d mg=%ld,%ld,%ld",
        static_cast<int>(sample.rawX),
        static_cast<int>(sample.rawY),
        static_cast<int>(sample.rawZ),
        static_cast<long>(sample.milliGx),
        static_cast<long>(sample.milliGy),
        static_cast<long>(sample.milliGz));
    return true;
}

bool PrintEvents(MotionConsoleCommandContext& context) {
    if (!context.controller->PollEvent()) {
        context.console->PrintLn("FAIL: accelerometer event read failed");
        return false;
    }
    const auto& diagnostics = context.controller->GetDiagnostics();
    const auto& event = diagnostics.lastEvent;
    context.console->PrintLn(
        "motion: wake=%u xyz=%u%u%u tap=%u double=%u tap_xyz=%u%u%u raw=%02x,%02x,%02x events=%lu stuck_low=%lu",
        static_cast<unsigned>(event.wakeUp ? 1U : 0U),
        static_cast<unsigned>(event.wakeX ? 1U : 0U),
        static_cast<unsigned>(event.wakeY ? 1U : 0U),
        static_cast<unsigned>(event.wakeZ ? 1U : 0U),
        static_cast<unsigned>(event.singleTap ? 1U : 0U),
        static_cast<unsigned>(event.doubleTap ? 1U : 0U),
        static_cast<unsigned>(event.tapX ? 1U : 0U),
        static_cast<unsigned>(event.tapY ? 1U : 0U),
        static_cast<unsigned>(event.tapZ ? 1U : 0U),
        static_cast<unsigned>(event.rawWake),
        static_cast<unsigned>(event.rawTap),
        static_cast<unsigned>(event.rawAll),
        static_cast<unsigned long>(diagnostics.eventCount),
        static_cast<unsigned long>(diagnostics.stuckLowCount));
    return true;
}

bool Calibrate(MotionConsoleCommandContext& context,
               const Console::Args& args) {
    uint16_t sampleCount = 0U;
    if (!ParseSampleCount(args, sampleCount)) {
        context.console->PrintLn("usage: motion calibrate [4..64]");
        return false;
    }

    firmware::MotionCalibrationAccumulator calibration;
    for (uint16_t index = 0U; index < sampleCount; ++index) {
        hardware::Lis2dw12Sample sample;
        if (!context.controller->GetSensor().ReadSample(sample)) {
            context.console->PrintLn(
                "FAIL: accelerometer sample %u unavailable",
                static_cast<unsigned>(index + 1U));
            return false;
        }
        calibration.Add(sample);
        if (context.delay != nullptr && index + 1U < sampleCount) {
            context.delay(context.delayContext, 80U);
        }
    }

    const firmware::MotionCalibrationResult result = calibration.GetResult();
    context.console->PrintLn(
        "motion: calibration samples=%u peak_to_peak_mg=%u recommended_threshold_mg=%u",
        static_cast<unsigned>(result.sampleCount),
        static_cast<unsigned>(result.peakToPeakMg),
        static_cast<unsigned>(result.recommendedMotionThresholdMg));
    return true;
}

} // namespace

bool MotionCommand(MotionConsoleCommandContext& context,
                   const Console::Args& args) {
    if (context.console == nullptr || context.settings == nullptr ||
        context.controller == nullptr) {
        return false;
    }

    const std::string_view command =
        args.Has(1) ? args.Get(1) : std::string_view("status");
    if (command == "status") {
        return PrintStatus(context);
    }
    if (command == "sample") {
        return PrintSample(context);
    }
    if (command == "events") {
        return PrintEvents(context);
    }
    if (command == "calibrate") {
        return Calibrate(context, args);
    }
    if (command == "apply") {
        const bool applied = context.controller->ApplyNow(*context.settings);
        context.console->PrintLn(applied ? "OK" : "FAIL");
        return applied;
    }

    context.console->PrintLn(
        "usage: motion <status|sample|events|calibrate [4..64]|apply>");
    return false;
}

void RegisterMotionConsoleCommands(
    Console& console,
    fxe::SettingsStore& settings,
    firmware::MotionWakeController& controller,
    MotionConsoleCommandContext& context) {
    context.console = &console;
    context.settings = &settings;
    context.controller = &controller;
    console.RegisterCommand(
        "motion",
        "Motion: status|sample|events|calibrate|apply",
        [&context](const Console::Args& args) {
            return MotionCommand(context, args);
        },
        [](const Console::Args&,
           const size_t argumentIndex,
           const char* prefix,
           const char** matches,
           const size_t capacity) {
            return CompleteMotion(
                argumentIndex, prefix, matches, capacity);
        },
        "Usage: motion <status|sample|events|calibrate [4..64]|apply>");
}

} // namespace modules
