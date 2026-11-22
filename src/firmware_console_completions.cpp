#ifdef ARDUINO

#include "firmware/firmware_console_internal.hpp"

namespace {

template <size_t N>
size_t CompleteChoices(const size_t expectedArgIndex,
                       const size_t argIndex,
                       const char* prefix,
                       const char** outMatches,
                       const size_t outMatchesCap,
                       const char* const (&choices)[N]) {
    if (argIndex != expectedArgIndex) {
        return 0;
    }

    size_t matchCount = 0;
    for (const char* choice : choices) {
        matchCount = firmware_internal::console_commands::AddMatchIfPrefix(choice,
                                                                           prefix,
                                                                           outMatches,
                                                                           matchCount,
                                                                           outMatchesCap);
    }
    return matchCount;
}

} // namespace

namespace firmware_internal::console_commands {

size_t BleComplete(const Console::Args&,
                   const size_t argIndex,
                   const char* prefix,
                   const char** outMatches,
                   const size_t outMatchesCap) {
    static constexpr const char* kChoices[] = {"on", "off", "status", "test"};
    return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kChoices);
}

size_t InputComplete(const Console::Args&,
                     const size_t argIndex,
                     const char* prefix,
                     const char** outMatches,
                     const size_t outMatchesCap) {
    static constexpr const char* kChoices[] = {"scan"};
    return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kChoices);
}

size_t AmptestComplete(const Console::Args& args,
                       const size_t argIndex,
                       const char* prefix,
                       const char** outMatches,
                       const size_t outMatchesCap) {
    static constexpr const char* kChoices[] = {"start", "status", "stop", "restart", "verify"};
    static constexpr const char* kVerifyChoices[] = {"start", "status", "stop", "restart"};

    if (argIndex == 1) {
        return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kChoices);
    }
    if (args.Equals(1, "verify")) {
        return CompleteChoices(2, argIndex, prefix, outMatches, outMatchesCap, kVerifyChoices);
    }
    return 0;
}

size_t LedPowerComplete(const Console::Args&,
                        const size_t argIndex,
                        const char* prefix,
                        const char** outMatches,
                        const size_t outMatchesCap) {
    static constexpr const char* kChoices[] = {"status", "clear", "latch"};
    return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kChoices);
}

size_t UsbCcComplete(const Console::Args&,
                     const size_t argIndex,
                     const char* prefix,
                     const char** outMatches,
                     const size_t outMatchesCap) {
    static constexpr const char* kChoices[] = {"read", "show", "set", "clear"};
    return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kChoices);
}

size_t I2cComplete(const Console::Args& args,
                   const size_t argIndex,
                   const char* prefix,
                   const char** outMatches,
                   const size_t outMatchesCap) {
    static constexpr const char* kChoices[] = {"status", "set"};
    static constexpr const char* kRates[] = {"100k", "400k", "1m"};

    if (argIndex == 1) {
        return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kChoices);
    }
    if (args.Equals(1, "set")) {
        return CompleteChoices(2, argIndex, prefix, outMatches, outMatchesCap, kRates);
    }
    return 0;
}

size_t FiveVEnComplete(const Console::Args&,
                       const size_t argIndex,
                       const char* prefix,
                       const char** outMatches,
                       const size_t outMatchesCap) {
    static constexpr const char* kChoices[] = {"on", "off"};
    return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kChoices);
}

size_t SetLedComplete(const Console::Args&,
                      const size_t argIndex,
                      const char* prefix,
                      const char** outMatches,
                      const size_t outMatchesCap) {
    static constexpr const char* kChoices[] = {"board", "clear"};
    return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kChoices);
}

size_t RebootComplete(const Console::Args&,
                      const size_t argIndex,
                      const char* prefix,
                      const char** outMatches,
                      const size_t outMatchesCap) {
    static constexpr const char* kChoices[] = {"nosave"
#if defined(LITCH_ESP32S3)
        , "bootloader"
#endif
    };
    return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kChoices);
}

size_t SafeModeComplete(const Console::Args&,
                        const size_t argIndex,
                        const char* prefix,
                        const char** outMatches,
                        const size_t outMatchesCap) {
    static constexpr const char* kChoices[] = {"status", "clear", "reboot"};
    return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kChoices);
}

size_t AmbientComplete(const Console::Args&,
                       const size_t argIndex,
                       const char* prefix,
                       const char** outMatches,
                       const size_t outMatchesCap) {
    static constexpr const char* kChoices[] = {"status", "on", "off"};
    return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kChoices);
}

} // namespace firmware_internal::console_commands

#endif
