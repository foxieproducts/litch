#pragma once

#ifdef ARDUINO

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "console.hpp"
#include "modules/settings_store.hpp"
#include "modules/string_parsing.hpp"

namespace firmware_internal {

struct FirmwareSystem;

void ConfigureConsoleLogger();
void PrintLitchcraftClearResult(FirmwareSystem& sys, Console& console);
size_t SettingsKeyEnumCallback(const char* prefix,
                               const char** outMatches,
                               size_t outMatchesCap);
bool SettingsWritePolicyCallback(std::string_view key, bool* outForceSaveNow);
void BeforeSettingsMutationCallback(std::string_view key);
bool RequestSettingsClear(Console& console);
const fxe::SettingDescriptor* SettingsDescriptorLookupCallback(std::string_view key);
fxe::Console::SettingsSetResult SettingsValidateAndSetCallback(fxe::SettingsStore& settings,
                                                               std::string_view key,
                                                               JsonVariantConst value);
fxe::Console::SettingsSetResult SettingsResetCallback(fxe::SettingsStore& settings,
                                                      std::string_view key);

namespace console_commands {

bool PerfCommand(Console& console, const Console::Args& args);
bool HeapCommand(Console& console, const Console::Args& args);
bool EngineCommand(Console& console, const Console::Args& args);
bool RebootCommand(Console& console, const Console::Args& args);
bool SleepCommand(Console& console, const Console::Args& args);
size_t SleepComplete(const Console::Args& args,
                     size_t argIndex,
                     const char* prefix,
                     const char** outMatches,
                     size_t outMatchesCap);
bool AmbientCommand(Console& console, const Console::Args& args);
bool FlipCommand(Console& console, const Console::Args& args);
bool FirstPlayerCommand(Console& console, const Console::Args& args);
bool BoardCommand(Console& console, const Console::Args& args);
bool BleCommand(Console& console, const Console::Args& args);
bool InputCommand(Console& console, const Console::Args& args);
bool ComptestCommand(Console& console, const Console::Args& args);
bool AmptestCommand(Console& console, const Console::Args& args);
bool LedPowerCommand(Console& console, const Console::Args& args);
bool UsbCcCommand(Console& console, const Console::Args& args);
bool I2cCommand(Console& console, const Console::Args& args);
bool FiveVEnCommand(Console& console, const Console::Args& args);
bool SetLedCommand(Console& console, const Console::Args& args);
bool SafeModeCommand(Console& console, const Console::Args& args);
bool VersionCommand(Console& console, const Console::Args& args);
bool FactoryCommand(Console& console, const Console::Args& args);
bool HardwareTestCommand(Console& console, const Console::Args& args);

bool StartsWithStr(const char* s, const char* prefix);
size_t AddMatchIfPrefix(const char* candidate,
                        const char* prefix,
                        const char** outMatches,
                        size_t matchCount,
                        size_t outMatchesCap);
size_t EngineComplete(const Console::Args& args,
                      size_t argIndex,
                      const char* prefix,
                      const char** outMatches,
                      size_t outMatchesCap);
size_t BleComplete(const Console::Args& args,
                   size_t argIndex,
                   const char* prefix,
                   const char** outMatches,
                   size_t outMatchesCap);
size_t InputComplete(const Console::Args& args,
                     size_t argIndex,
                     const char* prefix,
                     const char** outMatches,
                     size_t outMatchesCap);
size_t AmptestComplete(const Console::Args& args,
                       size_t argIndex,
                       const char* prefix,
                       const char** outMatches,
                       size_t outMatchesCap);
size_t LedPowerComplete(const Console::Args& args,
                        size_t argIndex,
                        const char* prefix,
                        const char** outMatches,
                        size_t outMatchesCap);
size_t UsbCcComplete(const Console::Args& args,
                     size_t argIndex,
                     const char* prefix,
                     const char** outMatches,
                     size_t outMatchesCap);
size_t I2cComplete(const Console::Args& args,
                   size_t argIndex,
                   const char* prefix,
                   const char** outMatches,
                   size_t outMatchesCap);
size_t FiveVEnComplete(const Console::Args& args,
                       size_t argIndex,
                       const char* prefix,
                       const char** outMatches,
                       size_t outMatchesCap);
size_t SetLedComplete(const Console::Args& args,
                      size_t argIndex,
                      const char* prefix,
                      const char** outMatches,
                      size_t outMatchesCap);
size_t RebootComplete(const Console::Args& args,
                      size_t argIndex,
                      const char* prefix,
                      const char** outMatches,
                      size_t outMatchesCap);
size_t SafeModeComplete(const Console::Args& args,
                        size_t argIndex,
                        const char* prefix,
                        const char** outMatches,
                        size_t outMatchesCap);
size_t AmbientComplete(const Console::Args& args,
                       size_t argIndex,
                       const char* prefix,
                       const char** outMatches,
                       size_t outMatchesCap);
size_t HardwareTestComplete(const Console::Args& args,
                            size_t argIndex,
                            const char* prefix,
                            const char** outMatches,
                            size_t outMatchesCap);
size_t BoardComplete(const Console::Args& args,
                     size_t argIndex,
                     const char* prefix,
                     const char** outMatches,
                     size_t outMatchesCap);
bool TryParseU16(std::string_view s, uint16_t& out);
bool TryParseU32(std::string_view s, uint32_t& out);

inline bool TryParseU16(const std::string_view s, uint16_t& out) {
    return modules::string_parsing::TryParseU16(s, out);
}

inline bool TryParseU32(const std::string_view s, uint32_t& out) {
    return modules::string_parsing::TryParseU32(s, out);
}

} // namespace console_commands

void RegisterFeatureConsoleCommands(FirmwareSystem& sys, Console& console);

} // namespace firmware_internal

#endif
