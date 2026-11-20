#include <cstring>
#include <string_view>

#include "foxie_embedded/console.hpp"

#include "console_builtins_helpers.hpp"

namespace fxe {

namespace {

char ToLowerAscii(const char value) {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }

    return value;
}

bool ContainsCaseInsensitive(const char* haystack, const std::string_view needle) {
    if (!haystack) {
        return false;
    }

    if (needle.empty()) {
        return true;
    }

    const size_t haystackLen = std::strlen(haystack);
    if (needle.size() > haystackLen) {
        return false;
    }

    for (size_t start = 0; start <= (haystackLen - needle.size()); ++start) {
        bool matched = true;
        for (size_t index = 0; index < needle.size(); ++index) {
            if (ToLowerAscii(haystack[start + index]) != ToLowerAscii(needle[index])) {
                matched = false;
                break;
            }
        }

        if (matched) {
            return true;
        }
    }

    return false;
}

bool MatchesSearchTerms(const Console::Args& args, const char* name, const char* help,
                        const char* usage) {
    for (size_t index = 1; index < args.argc; ++index) {
        const std::string_view term = args.Get(index);
        if (!ContainsCaseInsensitive(name, term) && !ContainsCaseInsensitive(help, term) &&
            !ContainsCaseInsensitive(usage, term)) {
            return false;
        }
    }

    return true;
}

} // namespace

void Console::InstallBuiltinHelp() {
    auto helpCmd = [this](const Args& args) -> bool {
        if (args.argc < 2) {
            BeginBulkOutput();
            PrintLn("Commands:");
            for (size_t i = 0; i < m_commandCount; ++i) {
                const auto& c = m_commands[i];
                PrintLn("  %s - %s", fxe::SafeStr(c.name), fxe::SafeStr(c.help));
            }
            PrintLn("Tip: 'help <command>' shows details, 'help <idea>' searches commands");
            EndBulkOutput();
            return true;
        }

        if (args.argc == 2) {
            const std::string_view query = args.Get(1);
            for (size_t i = 0; i < m_commandCount; ++i) {
                const auto& c = m_commands[i];
                if (c.name && query == std::string_view(c.name)) {
                    PrintLn("%s - %s", fxe::SafeStr(c.name), fxe::SafeStr(c.help));

                    if (c.usage) PrintLn("%s", c.usage);

                    return true;
                }
            }
        }

        size_t matchCount = 0;
        BeginBulkOutput();
        for (size_t i = 0; i < m_commandCount; ++i) {
            const auto& c = m_commands[i];
            if (!MatchesSearchTerms(args, c.name, c.help, c.usage)) {
                continue;
            }

            if (matchCount == 0) {
                PrintLn("Matches:");
            }
            PrintLn("  %s - %s", fxe::SafeStr(c.name), fxe::SafeStr(c.help));
            if (c.usage) PrintLn("%s", c.usage);
            ++matchCount;
        }

        if (matchCount > 0) {
            PrintLn("Tip: use 'help <command>' for exact details");
            EndBulkOutput();
            return true;
        }

        EndBulkOutput();
        PrintLn("No commands matching: %s", fxe::SafeStr(args.CStr(1)));
        return true;
    };

    auto helpComplete = [this](const Args&,
                               const size_t argIndex,
                               const char* prefix,
                               const char** outMatches,
                               const size_t outMatchesCap) -> size_t {
        if (argIndex != 1) {
            return 0;
        }
        return FindCompletions(prefix, outMatches, outMatchesCap);
    };

    RegisterCommand("help", "List/search commands (help <cmd|idea>)", helpCmd, helpComplete);
}

} // namespace fxe
