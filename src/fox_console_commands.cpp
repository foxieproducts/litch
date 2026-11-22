#include <array>
#include <string_view>

#include "modules/fox_console_commands.hpp"

namespace modules {
namespace {

constexpr std::array<const char*, 6U> FORTUNES = {
    "Before you move, ask what changed.",
    "Give your least useful piece a better job.",
    "A quiet move can make a loud threat.",
    "Look at your opponent's idea before choosing your own.",
    "A spare tempo is a small treasure.",
    "Every master was once surprised by a knight.",
};

size_t CompleteFox(const fxe::Console::Args&, size_t argument,
                   const char* prefix, const char** matches, size_t capacity) {
    if (argument != 1U || prefix == nullptr || matches == nullptr) return 0U;
    size_t count = 0U;
    for (const char* word : {"hello", "fortune", "quack"}) {
        if (std::string_view(word).substr(0U, std::string_view(prefix).size()) == prefix &&
            count < capacity) {
            matches[count++] = word;
        }
    }
    return count;
}

} // namespace

bool RegisterFoxConsoleCommand(fxe::Console& console) {
    return console.RegisterCommand(
        "fox", "Small curiosities: fox [hello|fortune|quack]",
        [&console, fortune = uint8_t{0U}](const fxe::Console::Args& args) mutable {
            const auto action = args.Has(1U) ? args.Get(1U) : std::string_view("hello");
            if (args.argc > 2U ||
                (action != "hello" && action != "fortune" && action != "quack")) {
                console.PrintLn("Usage: fox [hello|fortune|quack]");
                return false;
            }
            if (action == "fortune") {
                console.PrintLn("Foxie: %s", FORTUNES[fortune]);
                fortune = static_cast<uint8_t>((fortune + 1U) % FORTUNES.size());
            } else if (action == "quack") {
                console.PrintLn("  __");
                console.PrintLn("<(o )___");
                console.PrintLn(" (_____/  The duck declines to comment.");
            } else {
                console.PrintLn(" /\\   /\\");
                console.PrintLn("( o\\_/o )  Foxie says hello.");
                console.PrintLn(" \\_ v _/   Try 'fox fortune' or 'fox quack'.");
            }
            return true;
        }, &CompleteFox);
}

} // namespace modules
