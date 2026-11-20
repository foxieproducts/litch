#include <utility>

#include "foxie_embedded/console.hpp"

namespace fxe {

void Console::HandlePasteChar(const char c) {
    if (m_pasteInEscape) {
        if (!m_pasteInCsi) {
            if (c == '[' || c == 'O') {
                m_pasteInCsi = true;
                return;
            }

            m_pasteInEscape = false;
            return;
        }

        const bool isParam = (c >= '0' && c <= '9') || c == ';' || c == '?';
        if (isParam) {
            return;
        }

        m_pasteInEscape = false;
        m_pasteInCsi = false;
        return;
    }

    if (c == '\x1b') {
        m_pasteInEscape = true;
        m_pasteInCsi = false;
        return;
    }

    if (c == 0x04) {
        FinishPasteMode();
        return;
    }

    if (c == '\r') {
        return;
    }

    if (c == '\b' || static_cast<unsigned char>(c) == 127) {
        if (m_pasteLineOverflow || m_inputLen == 0) {
            return;
        }

        --m_inputLen;
        m_input[m_inputLen] = '\0';
        WriteRaw("\b \b", 3);
        return;
    }

    if (c == '\n') {
        SubmitPasteLine();
        WriteRaw("\r\n");
        return;
    }

    if (m_pasteLineOverflow) {
        return;
    }

    if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
        return;
    }

    if (m_inputLen + 1 >= INPUT_CAPACITY) {
        m_pasteLineOverflow = true;
        return;
    }

    m_input[m_inputLen] = c;
    ++m_inputLen;
    m_input[m_inputLen] = '\0';

    const char out[2] = {c, '\0'};
    WriteRaw(out, 1);
}

void Console::SubmitPasteLine() {
    if (m_pasteHandler) {
        // Paste retains the begin command and every consumed payload byte's
        // provenance. A local begin cannot grant local authority to later
        // remote, unknown or recalled input. Expose it only during the callback.
        struct SourceScope {
            ConsoleInputSource& current;
            ConsoleInputSource previous;
            uint64_t& generation;
            uint64_t previousGeneration;
            ~SourceScope() { current = previous; generation = previousGeneration; }
        } sourceScope{m_commandInputSource, m_commandInputSource,
                      m_commandRemoteInputGeneration, m_commandRemoteInputGeneration};
        m_commandInputSource = m_inputSource;
        m_commandRemoteInputGeneration = m_inputRemoteGeneration;
        const PasteEvent event = m_pasteLineOverflow ? PasteEvent::LineOverflow : PasteEvent::Line;
        char* line = m_pasteLineOverflow ? nullptr : m_input;
        const size_t len = m_pasteLineOverflow ? 0 : m_inputLen;
        if (!m_pasteHandler(event, line, len)) {
            m_pasteHandlerFailed = true;
        }
    }

    m_pasteLineOverflow = false;
    m_input[0] = '\0';
    m_inputLen = 0;
}

void Console::FinishPasteMode() {
    if (m_pasteLineOverflow || m_inputLen != 0) {
        SubmitPasteLine();
    }

    PasteHandler handler = std::move(m_pasteHandler);
    const bool handlerFailed = m_pasteHandlerFailed;
    m_pasteModeActive = false;
    m_pasteLineOverflow = false;
    m_pasteHandlerFailed = false;
    m_pasteInEscape = false;
    m_pasteInCsi = false;
    m_input[0] = '\0';
    m_inputLen = 0;
    m_cursorPos = 0;
    m_pasteHandler = nullptr;
    m_inputSource = ConsoleInputSource::Unknown;
    m_inputSourceObserved = false;
    m_inputRemoteGeneration = 0U;

    bool ok = !handlerFailed;
    if (handler) {
        ok = handler(PasteEvent::Finish, nullptr, 0) && ok;
    }

    if (!ok) {
        PrintLn("FAIL");
    }
    RedrawPrompt();
}

} // namespace fxe
