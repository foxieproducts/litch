#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/fixed_string.hpp"

namespace fxe {

Console::Console(IConsoleTransport& transport) : Console(transport, {}) {}

Console::Console(IConsoleTransport& transport, const Config& cfg) : m_transport(transport) {
    if (cfg.installBuiltins) {
        InstallBuiltins();
    }

    m_input[0] = '\0';
    m_inputLen = 0;
    m_historyCount = 0;
    m_historyOldestIndex = 0;
    m_historyIndex = 0;
    m_historyScratch[0] = '\0';

    m_pasteModeActive = false;
    m_pasteLineOverflow = false;
    m_pasteHandlerFailed = false;
    m_pasteInEscape = false;
    m_pasteInCsi = false;
    m_pasteHandler = nullptr;

    m_lastWasCR = false;
    m_cursorPos = 0;
    m_csiLastParamChar = '\0';
}

bool Console::StartPasteMode(PasteHandler handler) {
    if (m_pasteModeActive) {
        return false;
    }

    m_pasteModeActive = true;
    m_pasteLineOverflow = false;
    m_pasteHandlerFailed = false;
    m_pasteInEscape = false;
    m_pasteInCsi = false;
    m_input[0] = '\0';
    m_inputLen = 0;
    m_cursorPos = 0;
    m_inputRejected = false;
    m_lastRenderLen = 0;
    m_pasteHandler = std::move(handler);
    m_inputSource = m_commandInputSource;
    m_inputSourceObserved = true;
    m_inputRemoteGeneration = m_commandRemoteInputGeneration;
    return true;
}

void Console::SetSettingsStore(fxe::SettingsStore* settings) {
    m_settings = settings;
}

bool Console::RegisterCommand(const char* name, const char* help, CommandHandler handler) {
    return RegisterCommand(name, help, std::move(handler), nullptr);
}

bool Console::RegisterCommand(const char* name,
                              const char* help,
                              CommandHandler handler,
                              CompletionHandler completion,
                              const char* usage) {
    if (!name || name[0] == '\0' || !handler) {
        return false;
    }

    for (size_t i = 0; i < m_commandCount; ++i) {
        if (m_commands[i].name && std::string_view(m_commands[i].name) == std::string_view(name)) {
            m_commands[i].help = help;
            m_commands[i].usage = usage;
            m_commands[i].handler = std::move(handler);
            m_commands[i].completion = std::move(completion);
            return true;
        }
    }

    if (m_commandCount >= MAX_COMMANDS) {
        return false;
    }

    m_commands[m_commandCount].name = name;
    m_commands[m_commandCount].help = help;
    m_commands[m_commandCount].usage = usage;
    m_commands[m_commandCount].handler = std::move(handler);
    m_commands[m_commandCount].completion = std::move(completion);
    ++m_commandCount;
    return true;
}

void Console::DiscardRetiredRemoteInput() {
    const bool remote = m_inputSource == ConsoleInputSource::Remote ||
        m_inputSource == ConsoleInputSource::Mixed || m_inputRemoteGeneration != 0U;
    if (!remote || m_inputRemoteGeneration == m_transport.RemoteInputGeneration()) return;
    m_input[0] = m_historyScratch[0] = '\0';
    m_inputLen = m_cursorPos = m_lastRenderLen = 0U;
    m_historyIndex = m_historyCount;
    m_inputSource = ConsoleInputSource::Unknown;
    m_inputSourceObserved = m_inputRejected = m_lastWasCR = false;
    m_inputRemoteGeneration = 0U;
    m_inEscape = m_inCsi = m_pasteModeActive = false;
    m_pasteInEscape = m_pasteInCsi = m_pasteLineOverflow = false;
    m_pasteHandler = nullptr;
}

void Console::Update() {
    DiscardRetiredRemoteInput();
    char c = 0;
    while (m_transport.TryReadChar(c)) {
        DiscardRetiredRemoteInput();
        HandleChar(c, m_transport.LastInputSource());
    }

    if (m_settingsPasteRebootPending &&
        m_settingsPasteRebootTime.HasElapsed(
            m_settingsPasteRebootDelayMs)) {
        m_settingsPasteRebootPending = false;
        (void)ExecuteLine("reboot");
    }
}

void Console::Print(const char* fmt, ...) {
    fxe::FixedString<256> buf;

    va_list args;
    va_start(args, fmt);
    (void)buf.VFormat(fmt, args);
    va_end(args);

    if (m_bulkOutputDepth == 0) {
        BeginOutput();
    }

    WriteRaw(buf.CStr());

    if (m_bulkOutputDepth == 0) {
        RedrawPrompt();
    }
}

void Console::PrintLn(const char* fmt, ...) {
    fxe::FixedString<256> buf;

    va_list args;
    va_start(args, fmt);
    (void)buf.VFormat(fmt, args);
    va_end(args);

    if (m_bulkOutputDepth == 0) {
        BeginOutput();
    }

    WriteRaw(buf.CStr());
    WriteRaw("\r\n");

    if (m_bulkOutputDepth == 0) {
        RedrawPrompt();
    }
}

void Console::PrintRaw(const char* data, const size_t len) {
    if (!data || len == 0) {
        return;
    }

    if (m_bulkOutputDepth == 0) {
        BeginOutput();
    }

    WriteRaw(data, len);

    if (m_bulkOutputDepth == 0) {
        RedrawPrompt();
    }
}

void Console::PrintWelcome() {
    PrintLn("Console");
    PrintLn("Type 'help' for commands");
}

void Console::InstallBuiltins() {
    InstallBuiltinHelp();
    InstallBuiltinLogs();
    InstallBuiltinDebug();
    InstallBuiltinSettings();
    InstallBuiltinSet();
}

} // namespace fxe
