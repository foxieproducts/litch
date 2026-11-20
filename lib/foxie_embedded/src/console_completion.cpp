#include <cstring>

#include "foxie_embedded/console.hpp"

namespace fxe {

namespace {

const char* SafeStr(const char* s) {
    return s ? s : "";
}

size_t CommonPrefixLenForMatches(const char** matches, const size_t matchCount) {
    if (!matches || matchCount == 0 || !matches[0]) {
        return 0;
    }

    size_t commonLen = std::strlen(matches[0]);
    for (size_t i = 1; i < matchCount; ++i) {
        const char* match = SafeStr(matches[i]);
        size_t j = 0;
        while (j < commonLen && matches[0][j] != '\0' && match[j] != '\0' && matches[0][j] == match[j]) {
            ++j;
        }
        commonLen = j;
        if (commonLen == 0) {
            break;
        }
    }

    return commonLen;
}

} // namespace

bool Console::StartsWith(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return false;
    }
    const size_t n = std::strlen(prefix);
    return std::strncmp(s, prefix, n) == 0;
}

size_t Console::FindCompletions(const char* prefix, const char** outMatches, const size_t outMatchesCap) const {
    if (!prefix || !outMatches || outMatchesCap == 0) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < m_commandCount; ++i) {
        const auto& c = m_commands[i];
        if (!c.name) {
            continue;
        }
        if (StartsWith(c.name, prefix)) {
            if (count < outMatchesCap) {
                outMatches[count] = c.name;
            }
            ++count;
        }
    }

    return (count > outMatchesCap) ? outMatchesCap : count;
}

void Console::OnTab() {
    if (m_inputLen == 0 || m_cursorPos != m_inputLen) {
        return;
    }

    std::strncpy(m_execLine, m_input, INPUT_CAPACITY);
    m_execLine[INPUT_CAPACITY - 1] = '\0';

    const bool endsWithSpace = m_input[m_inputLen - 1] == ' ' || m_input[m_inputLen - 1] == '\t';
    Args args = SplitArgsInPlace(m_execLine);

    size_t argIndex = 0;
    const char* prefix = "";
    if (endsWithSpace) {
        argIndex = static_cast<size_t>(args.argc);
    } else {
        if (args.argc == 0) {
            return;
        }
        argIndex = static_cast<size_t>(args.argc - 1);
        prefix = SafeStr(args.CStr(argIndex));
    }

    const char* matches[16] = {};
    size_t matchCount = 0;
    if (argIndex == 0) {
        matchCount = FindCompletions(prefix, matches, 16);
    } else {
        const Command* command = nullptr;
        for (size_t i = 0; i < m_commandCount; ++i) {
            if (m_commands[i].name && args.Has(0) && args.Get(0) == std::string_view(m_commands[i].name)) {
                command = &m_commands[i];
                break;
            }
        }

        if (!command || !command->completion) {
            return;
        }
        matchCount = command->completion(args, argIndex, prefix, matches, 16);
    }

    if (matchCount == 0) {
        return;
    }

    size_t tokenStart = m_inputLen;
    size_t prefixLen = 0;
    if (!endsWithSpace) {
        prefixLen = std::strlen(prefix);
        if (prefixLen <= m_inputLen) {
            tokenStart = m_inputLen - prefixLen;
        }
    }

    if (matchCount == 1) {
        const char* replacement = matches[0];
        const size_t replacementLen = replacement ? std::strlen(replacement) : 0;
        if (!replacement || tokenStart + replacementLen >= INPUT_CAPACITY) {
            return;
        }

        std::memcpy(m_input + tokenStart, replacement, replacementLen);
        m_inputLen = tokenStart + replacementLen;
        if (m_inputLen + 1 < INPUT_CAPACITY) {
            m_input[m_inputLen++] = ' ';
        }
        m_input[m_inputLen] = '\0';
        m_cursorPos = m_inputLen;
        RedrawPrompt();
        return;
    }

    const size_t commonLen = CommonPrefixLenForMatches(matches, matchCount);
    if (commonLen > prefixLen) {
        const char* replacement = matches[0];
        if (replacement && tokenStart + commonLen < INPUT_CAPACITY) {
            std::memcpy(m_input + tokenStart, replacement, commonLen);
            m_inputLen = tokenStart + commonLen;
            m_input[m_inputLen] = '\0';
            m_cursorPos = m_inputLen;
            RedrawPrompt();
        }
    }

    PrintLn("");
    for (size_t i = 0; i < matchCount; ++i) {
        PrintLn("%s", SafeStr(matches[i]));
    }
}

} // namespace fxe
