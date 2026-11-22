#ifdef ARDUINO

#include <Arduino.h>

#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"
#include "firmware/local_control_requests.hpp"
#include "firmware/firmware_tasks_internal.hpp"
#include "modules/console_jokes.hpp"
#include "modules/game_console_commands.hpp"
#include "modules/fox_console_commands.hpp"
#include "modules/motion_console_commands.hpp"
#include "modules/owner_pairing_console_commands.hpp"
#if defined(LITCH_ESP32S3) && defined(LITCH_PANIC_DIAGNOSTIC)
#include "runtime/panic_diagnostic.hpp"
#endif

namespace firmware_internal {
namespace {

void MotionConsoleDelay(void*, const uint32_t milliseconds) {
    delay(milliseconds);
}

} // namespace

void PrintLitchcraftClearResult(FirmwareSystem& sys, Console& console) {
#if defined(LITCH_ESP32S3)
    using State = firmware::SettingsPersistenceWorker::FrameClearState;
    const auto result = sys.settingsPersistence.TakeLitchcraftFrameClearResult();
    if (result == State::Succeeded) {
        console.PrintLn("litchcraft clear: Store cleared; playback paused");
    } else if (result == State::Failed) {
        console.PrintLn("litchcraft clear: storage failure; retry litchcraft clear");
    }
#else
    (void)sys;
    (void)console;
#endif
}

void RegisterFeatureConsoleCommands(FirmwareSystem& sys,
                                    Console& console) {
#if defined(LITCH_ESP32S3) && defined(LITCH_PANIC_DIAGNOSTIC)
    runtime::panic_diagnostic::RegisterStackProbeCommand(console);
#endif
    console.SetSettingsResetCallback(&SettingsResetCallback);

    Console* const consolePtr = &console;
    console.RegisterCommand(
        "hwtest",
        "Manufacturing test: hwtest [start|restart|status|forceexit|confirm leds|stop|verifier on|off|fault motion|light1|light2|expander1..4|clear]",
        [consolePtr](const Console::Args& args) {
            return console_commands::HardwareTestCommand(*consolePtr, args);
        },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::HardwareTestComplete(
                args, argIndex, prefix, outMatches, outMatchesCap);
        });

    console.RegisterCommand("factory", "Reset only factory completion: factory clear", [consolePtr](const Console::Args& args) {
        return console_commands::FactoryCommand(*consolePtr, args);
    });

    if (sys.factoryBootOnly) return;

    console.SetUnknownCommandHandler(
        [](fxe::Console& out, const fxe::Console::Args& args, void* context) {
            return static_cast<modules::ConsoleJokes*>(context)->Handle(out, args);
        }, &sys.consoleJokes);

    if (!modules::RegisterFoxConsoleCommand(console)) {
        console.PrintLn("fox: command table full");
    }

    (void)modules::RegisterOwnerPairingConsoleCommands(console, sys.boardOwnerPairing);

    console.RegisterCommand(
        "litchcraft", "Clear board-resident artwork: litchcraft clear",
        [&sys, consolePtr](const Console::Args& args) {
            PrintLitchcraftClearResult(sys, *consolePtr);
            if (!args.Has(1U) || args.Get(1U) != "clear" || args.Has(2U)) {
                consolePtr->PrintLn("usage: litchcraft clear");
                return false;
            }
#if defined(LITCH_ESP32S3)
            if (sys.ble.IsOtaInProgress() ||
                sys.sleep.GetState() != firmware::SleepState::Awake) {
                consolePtr->PrintLn("litchcraft clear: unavailable during update or sleep");
                return false;
            }
            const bool queued = sys.settingsPersistence.RequestLitchcraftFrameClear();
            consolePtr->PrintLn(queued ? "litchcraft clear: queued"
                                     : "litchcraft clear: busy or storage not ready");
            return queued;
#else
            consolePtr->PrintLn("litchcraft clear: unavailable on this board");
            return false;
#endif
        });

    static modules::GameConsoleCommandContext gameContext;
    modules::RegisterGameConsoleCommands(
        console,
        sys.settings,
        sys.gamePersistence,
        sys.stateMachine,
        gameContext,
        &sys.controlMutation);
    gameContext.asyncControlTrackingContext = &sys;
    gameContext.trackAsyncControlRequest = [](void* context,
                                              const uint16_t requestId) {
        if (context == nullptr) {
            return;
        }
        (void)TrackLocalControlRequest(
            *static_cast<FirmwareSystem*>(context),
            requestId,
            LocalControlRequestOrigin::Console);
    };

    static modules::MotionConsoleCommandContext motionContext;
    motionContext.delay = &MotionConsoleDelay;
    modules::RegisterMotionConsoleCommands(
        console, sys.settings, sys.motionWake, motionContext);
}

} // namespace firmware_internal

#endif
