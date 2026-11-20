#include <cstring>
#include <string_view>

#include "foxie_embedded/console.hpp"

namespace fxe {

namespace {

const char* SafeStr(const char* s) {
    return s ? s : "";
}

} // namespace

bool Console::ExecuteLine(const char* line, const ConsoleInputSource source,
                          const uint64_t remoteGeneration) {
    struct SourceScope {
        ConsoleInputSource& current;
        ConsoleInputSource previous;
        uint64_t& generation;
        uint64_t previousGeneration;
        ~SourceScope() { current = previous; generation = previousGeneration; }
    } sourceScope{m_commandInputSource, m_commandInputSource,
                  m_commandRemoteInputGeneration, m_commandRemoteInputGeneration};
    m_commandInputSource = source;
    m_commandRemoteInputGeneration = remoteGeneration;
    if (!line || line[0] == '\0') {
        return true;
    }

    size_t length = 0U;
    while (length < INPUT_CAPACITY && line[length] != '\0') {
        ++length;
    }
    if (length == INPUT_CAPACITY) {
        PrintLn("Invalid command: line too long");
        return false;
    }
    std::memmove(m_execLine, line, length + 1U);

    if (m_cmdLineCb) {
        m_cmdLineCb(m_execLine);
    }

    Args args = SplitArgsInPlace(m_execLine);
    if (args.overflow) {
        PrintLn("Invalid command: too many arguments");
        return false;
    }
    if (args.argc == 0 || !args.Has(0)) {
        return true;
    }

    // If the second parameter is "help", redirect to "help <cmd>".
    if (args.argc >= 2 && args.Get(1) == std::string_view("help")) {
        Args helpArgs;
        helpArgs.argc = 2;
        helpArgs.argv[0].ptr = const_cast<char*>("help");
        helpArgs.argv[0].len = 4;
        helpArgs.argv[1] = args.argv[0];

        for (size_t i = 0; i < m_commandCount; ++i) {
            const auto& c = m_commands[i];
            if (c.name && std::string_view("help") == std::string_view(c.name)) {
                return c.handler(helpArgs);
            }
        }
    }

    for (size_t i = 0; i < m_commandCount; ++i) {
        const auto& c = m_commands[i];
        if (c.name && args.Get(0) == std::string_view(c.name)) {
            return c.handler(args);
        }
    }

    if (m_unknownCommandHandler != nullptr &&
        m_unknownCommandHandler(*this, args, m_unknownCommandContext)) {
        return true;
    }

    PrintLn("Unknown command: %s", SafeStr(args.CStr(0)));
    return false;
}

Console::Args Console::SplitArgsInPlace(char* line) const {
    Args out;

    if (!line) {
        return out;
    }

    bool inQuote = false;
    char* write = line;
    const char* read = line;
    char* tokenStart = nullptr;
    size_t tokenLen = 0;

    auto FinishToken = [&]() {
        if (!tokenStart) {
            return;
        }

        *write = '\0';
        if (out.argc < MAX_ARGS) {
            out.argv[out.argc].ptr = tokenStart;
            out.argv[out.argc].len = static_cast<uint8_t>((tokenLen > 255) ? 255 : tokenLen);
            ++out.argc;
        } else {
            out.overflow = true;
        }
        tokenStart = nullptr;
        tokenLen = 0;
        ++write;
    };

    for (; *read != '\0'; ++read) {
        const char c = *read;

        if (c == '"') {
            inQuote = !inQuote;
            continue;
        }

        const bool isSpace = (!inQuote && (c == ' ' || c == '\t'));
        if (isSpace) {
            if (tokenStart) {
                FinishToken();
            }
            continue;
        }

        if (!tokenStart) {
            tokenStart = write;
            tokenLen = 0;
        }

        *write++ = c;
        ++tokenLen;
        if (static_cast<size_t>(write - line) >= (INPUT_CAPACITY - 1)) {
            break;
        }
    }

    if (tokenStart) {
        *write = '\0';
        if (out.argc < MAX_ARGS) {
            out.argv[out.argc].ptr = tokenStart;
            out.argv[out.argc].len = static_cast<uint8_t>((tokenLen > 255) ? 255 : tokenLen);
            ++out.argc;
        } else {
            out.overflow = true;
        }
    } else {
        *write = '\0';
    }

    return out;
}

} // namespace fxe
