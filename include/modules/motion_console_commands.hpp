#pragma once

#include <cstddef>
#include <cstdint>

#include "console.hpp"

namespace firmware {
class MotionWakeController;
}

namespace fxe {
class SettingsStore;
}

namespace modules {

struct MotionConsoleCommandContext {
    using DelayFn = void (*)(void* context, uint32_t milliseconds);

    Console* console = nullptr;
    fxe::SettingsStore* settings = nullptr;
    firmware::MotionWakeController* controller = nullptr;
    void* delayContext = nullptr;
    DelayFn delay = nullptr;
};

bool MotionCommand(MotionConsoleCommandContext& context,
                   const Console::Args& args);

void RegisterMotionConsoleCommands(
    Console& console,
    fxe::SettingsStore& settings,
    firmware::MotionWakeController& controller,
    MotionConsoleCommandContext& context);

} // namespace modules
