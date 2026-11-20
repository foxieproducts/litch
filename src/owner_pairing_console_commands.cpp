#include "modules/owner_pairing_console_commands.hpp"

#include <array>

#include "modules/board_owner_pairing_feature.hpp"

namespace modules {
namespace {

constexpr size_t CONSENT_ARGUMENT_COUNT = 8U;

bool ParseUnsigned32(const std::string_view text, uint32_t& out) {
    if (text.empty()) return false;
    uint32_t value = 0U;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') return false;
        const auto part = static_cast<uint32_t>(digit - '0');
        if (value > (UINT32_MAX - part) / 10U) return false;
        value = value * 10U + part;
    }
    out = value;
    return true;
}

bool OwnerPairCommand(Console& console, BoardOwnerPairingRuntime& runtime,
                      const Console::Args& args) {
    if (!OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED ||
        console.CommandInputSource() != fxe::ConsoleInputSource::Local) {
        console.PrintLn("ownerpair: private USB console only");
        return false;
    }
    runtime.Poll();
    if (args.argc == 2U && args.Equals(1U, "status")) {
        const auto status = runtime.Status();
        const auto pending = runtime.ReadPendingConsent();
        console.PrintLn("ownerpair: state=%u reason=%u attempt=%u end=%u handle=%u epoch=%lu session=%lu prompt=%lu",
                        static_cast<unsigned>(status.state),
                        static_cast<unsigned>(status.reason),
                        static_cast<unsigned>(status.attempt),
                        static_cast<unsigned>(status.physicalEnd),
                        static_cast<unsigned>(pending.connection.handle),
                        static_cast<unsigned long>(pending.connection.epoch),
                        static_cast<unsigned long>(pending.connection.sessionToken),
                        static_cast<unsigned long>(pending.prompt));
        if (pending.prompt != 0U) {
            console.PrintLn("ownerpair consent %u %u %u %lu %lu %lu",
                static_cast<unsigned>(pending.attempt),
                static_cast<unsigned>(pending.physicalEnd <= 1U ? pending.physicalEnd : 0U),
                static_cast<unsigned>(pending.connection.handle),
                static_cast<unsigned long>(pending.connection.epoch),
                static_cast<unsigned long>(pending.connection.sessionToken),
                static_cast<unsigned long>(pending.prompt));
        } else console.PrintLn("ownerpair: no live consent prompt");
        return true;
    }
    if (args.argc == 2U && args.Equals(1U, "clear")) {
        const bool accepted = runtime.RequestLocalPairingClear();
        console.PrintLn(accepted ? "ownerpair: pairing clear accepted; cleanup pending; use ownerpair status"
                                 : "ownerpair: pairing clear rejected; busy or unavailable");
        return accepted;
    }
    std::array<uint32_t, 6U> tuple{};
    bool parsed = args.argc == CONSENT_ARGUMENT_COUNT && args.Equals(1U, "consent");
    for (size_t index = 0U; parsed && index < tuple.size(); ++index)
        parsed = ParseUnsigned32(args.Get(index + 2U), tuple[index]);
    if (!parsed || tuple[0U] == 0U || tuple[0U] > UINT16_MAX || tuple[1U] > 1U ||
        tuple[2U] > UINT16_MAX || tuple[3U] == 0U || tuple[4U] == 0U || tuple[5U] == 0U) {
        console.PrintLn("usage: ownerpair status | clear | consent <attempt> <end> <handle> <epoch> <session> <prompt>");
        return false;
    }
    const BoardOwnerPairingRuntime::PendingConsent consent{
        {static_cast<uint16_t>(tuple[2U]), tuple[3U], tuple[4U]},
        static_cast<uint16_t>(tuple[0U]), static_cast<uint8_t>(tuple[1U]), tuple[5U]};
    const bool accepted = runtime.ConfirmPendingConsent(consent);
    console.PrintLn(accepted ? "ownerpair: consent accepted; native security pending"
                             : "ownerpair: rejected; no matching live consent prompt");
    return accepted;
}

} // namespace

bool RegisterOwnerPairingConsoleCommands(Console& console,
                                        BoardOwnerPairingRuntime& runtime) {
    if (!OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED) return false;
    return console.RegisterCommand("ownerpair",
        "Private USB: status | clear | consent <attempt> <end> <handle> <epoch> <session> <prompt>",
        [&console, &runtime](const Console::Args& args) {
            return OwnerPairCommand(console, runtime, args);
        });
}

} // namespace modules
