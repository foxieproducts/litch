#include <cstring>

#include "foxie_embedded/console.hpp"

namespace fxe {

void Console::HandleChar(const char c, const ConsoleInputSource source) {
    // The LF following CR is not a second submitted line and must not taint
    // the next command's provenance, even if it arrives on another transport.
    if (!m_pasteModeActive && !m_inEscape && c == '\n' && m_lastWasCR) {
        m_lastWasCR = false;
        return;
    }
    if (source == ConsoleInputSource::Remote &&
        m_inputSource != ConsoleInputSource::Remote &&
        m_inputSource != ConsoleInputSource::Mixed) {
        m_inputRemoteGeneration = m_transport.RemoteInputGeneration();
    }
    if (!m_inputSourceObserved) {
        m_inputSource = source;
        m_inputSourceObserved = true;
    } else if (m_inputSource != source) {
        m_inputSource = ConsoleInputSource::Mixed;
    }
    if (c == '\0') {
        if (m_pasteModeActive) {
            m_pasteLineOverflow = true;
        } else {
            m_inputRejected = true;
        }
        return;
    }
    if (m_pasteModeActive) {
        HandlePasteChar(c);
        return;
    }

    if (m_inEscape) {
        HandleEscapeSequenceChar(c);
        return;
    }

    // Many serial monitors send CRLF; treat as a single Enter.
    if (c != '\n') {
        m_lastWasCR = false;
    }

    if (c == '\x1b') {
        m_inEscape = true;
        m_inCsi = false;
        m_csiLastParamChar = '\0';
        return;
    }

    switch (c) {
        // Common terminal/Emacs-style control shortcuts.
        case 0x01: // Ctrl+A: Home
            if (m_cursorPos != 0) {
                m_cursorPos = 0;
                RedrawPrompt();
            }
            return;
        case 0x02: // Ctrl+B: Left
            if (m_cursorPos > 0) {
                --m_cursorPos;
                RedrawPrompt();
            }
            return;
        case 0x04: // Ctrl+D: Delete at cursor
            if (m_cursorPos < m_inputLen) {
                for (size_t i = m_cursorPos; i + 1 < m_inputLen; ++i) {
                    m_input[i] = m_input[i + 1];
                }
                --m_inputLen;
                m_input[m_inputLen] = '\0';
                RedrawPrompt();
            }
            return;
        case 0x05: // Ctrl+E: End
            if (m_cursorPos != m_inputLen) {
                m_cursorPos = m_inputLen;
                RedrawPrompt();
            }
            return;
        case 0x06: // Ctrl+F: Right
            if (m_cursorPos < m_inputLen) {
                ++m_cursorPos;
                RedrawPrompt();
            }
            return;
        case 0x0b: // Ctrl+K: Kill to end
            if (m_cursorPos < m_inputLen) {
                m_inputLen = m_cursorPos;
                m_input[m_inputLen] = '\0';
                RedrawPrompt();
            }
            return;
        case 0x0c: // Ctrl+L: Clear screen (no ANSI; just add space)
            WriteRaw("\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n");
            RedrawPrompt();
            return;
        case 0x0e: // Ctrl+N: History next
            HistoryNext();
            return;
        case 0x10: // Ctrl+P: History prev
            HistoryPrev();
            return;
        case 0x15: // Ctrl+U: Kill line
            m_inputRejected = false;
            if (m_inputLen != 0 || m_cursorPos != 0) {
                m_inputLen = 0;
                m_cursorPos = 0;
                m_input[0] = '\0';
                RedrawPrompt();
            }
            return;
        case 0x17: // Ctrl+W: Delete word backward
            if (m_inputLen == 0 || m_cursorPos == 0) {
                return;
            }
            while (m_cursorPos > 0 && (m_input[m_cursorPos - 1] == ' ' || m_input[m_cursorPos - 1] == '\t')) {
                OnBackspace();
            }
            while (m_cursorPos > 0 && !(m_input[m_cursorPos - 1] == ' ' || m_input[m_cursorPos - 1] == '\t')) {
                OnBackspace();
            }
            return;
        case '\r':
            m_lastWasCR = true;
            OnEnter();
            return;
        case '\n':
            OnEnter();
            return;
        case '\b':
        case 127:
            OnBackspace();
            return;
        case '\t':
            OnTab();
            return;
        default:
            break;
    }

    if (static_cast<unsigned char>(c) < 0x20) {
        return;
    }

    if (m_inputLen + 1 >= INPUT_CAPACITY) {
        m_inputRejected = true;
        return;
    }

    // Insert at cursor.
    if (m_cursorPos > m_inputLen) {
        m_cursorPos = m_inputLen;
    }

    for (size_t i = m_inputLen; i > m_cursorPos; --i) {
        m_input[i] = m_input[i - 1];
    }
    m_input[m_cursorPos] = c;
    ++m_inputLen;
    ++m_cursorPos;
    m_input[m_inputLen] = '\0';
    RedrawPrompt();
}

void Console::HandleEscapeSequenceChar(const char c) {
    if (!m_inCsi) {
        // Arrow keys are typically ESC [ A/B/C/D (CSI) or ESC O A/B/C/D (SS3).
        if (c == '[' || c == 'O') {
            m_inCsi = true;
            m_csiLastParamChar = '\0';
            return;
        }
        m_inEscape = false;
        return;
    }

    // Minimal CSI parsing:
    // - Ignore parameter bytes (digits, ';', '?')
    // - Act on final bytes A/B/C/D (arrows)
    // - Act on final bytes H/F (Home/End)
    // - Act on '~' with last param '3' (Delete)
    // - Act on '~' with last param '1'/'7' (Home) and '4'/'8' (End)
    const bool isParam = (c >= '0' && c <= '9') || c == ';' || c == '?';
    if (isParam) {
        if (c >= '0' && c <= '9') {
            m_csiLastParamChar = c;
        }
        return;
    }

    switch (c) {
        case 'A':
            HistoryPrev();
            break;
        case 'B':
            HistoryNext();
            break;
        case 'C':
            if (m_cursorPos < m_inputLen) {
                ++m_cursorPos;
                RedrawPrompt();
            }
            break;
        case 'D':
            if (m_cursorPos > 0) {
                --m_cursorPos;
                RedrawPrompt();
            }
            break;
        case 'H':
            if (m_cursorPos != 0) {
                m_cursorPos = 0;
                RedrawPrompt();
            }
            break;
        case 'F':
            if (m_cursorPos != m_inputLen) {
                m_cursorPos = m_inputLen;
                RedrawPrompt();
            }
            break;
        case '~':
            if (m_csiLastParamChar == '3') {
                // Delete at cursor.
                if (m_cursorPos < m_inputLen) {
                    for (size_t i = m_cursorPos; i + 1 < m_inputLen; ++i) {
                        m_input[i] = m_input[i + 1];
                    }
                    --m_inputLen;
                    m_input[m_inputLen] = '\0';
                    RedrawPrompt();
                }
            } else if (m_csiLastParamChar == '1' || m_csiLastParamChar == '7') {
                if (m_cursorPos != 0) {
                    m_cursorPos = 0;
                    RedrawPrompt();
                }
            } else if (m_csiLastParamChar == '4' || m_csiLastParamChar == '8') {
                if (m_cursorPos != m_inputLen) {
                    m_cursorPos = m_inputLen;
                    RedrawPrompt();
                }
            }
            break;
        default:
            break;
    }

    m_inEscape = false;
    m_inCsi = false;
    m_csiLastParamChar = '\0';
}

void Console::OnEnter() {
    WriteRaw("\r\n");

    const auto source = m_inputSource;
    const uint64_t remoteGeneration = m_inputRemoteGeneration;
    m_inputRemoteGeneration = 0U;
    m_inputSource = ConsoleInputSource::Unknown;
    m_inputSourceObserved = false;

    const bool inputRejected = m_inputRejected;
    const bool hadLine = (m_inputLen > 0) && !inputRejected;

    if (hadLine) {
        const size_t len = (m_inputLen < (INPUT_CAPACITY - 1)) ? m_inputLen : (INPUT_CAPACITY - 1);
        if (len != 0 && !HistoryEqualsLast(m_input, len)) {
            PushHistoryLine(m_input, len);
        }
    }

    m_historyIndex = m_historyCount;
    m_historyScratch[0] = '\0';

    if (hadLine) {
        std::strncpy(m_execLine, m_input, INPUT_CAPACITY);
        m_execLine[INPUT_CAPACITY - 1] = '\0';
    }

    m_input[0] = '\0';
    m_inputLen = 0;
    m_inputRejected = false;
    m_cursorPos = 0;
    m_lastRenderLen = 0;

    if (inputRejected) {
        PrintLn("Invalid command: NUL input or line too long");
        RedrawPrompt();
        return;
    }

    const char* submittedLine = hadLine ? m_execLine : "";
    if (m_enteredLineCb) {
        struct SourceScope {
            ConsoleInputSource& current;
            ConsoleInputSource previous;
            uint64_t& generation;
            uint64_t previousGeneration;
            ~SourceScope() { current = previous; generation = previousGeneration; }
        } scope{m_commandInputSource, m_commandInputSource,
                m_commandRemoteInputGeneration, m_commandRemoteInputGeneration};
        m_commandInputSource = source;
        m_commandRemoteInputGeneration = remoteGeneration;
        if (m_enteredLineCb(submittedLine)) {
            RedrawPrompt();
            return;
        }
    }

    if (hadLine) {
        ExecuteLine(m_execLine, source, remoteGeneration);
    }

    RedrawPrompt();
}

void Console::OnBackspace() {
    if (m_inputLen == 0 || m_cursorPos == 0) {
        return;
    }

    // Remove char before cursor.
    const size_t removeIndex = m_cursorPos - 1;
    for (size_t i = removeIndex; i + 1 < m_inputLen; ++i) {
        m_input[i] = m_input[i + 1];
    }
    --m_inputLen;
    --m_cursorPos;
    m_input[m_inputLen] = '\0';
    RedrawPrompt();
}

void Console::RedrawPrompt() {
    // No ANSI. Use CR + overwrite + pad with spaces to clear the previous render.
    const size_t promptLen = std::strlen(m_prompt);
    const size_t newLen = promptLen + m_inputLen;

    WriteRaw("\r");
    WriteRaw(m_prompt);
    WriteRaw(m_input);

    if (m_lastRenderLen > newLen) {
        const size_t toClear = m_lastRenderLen - newLen;
        for (size_t i = 0; i < toClear; ++i) {
            WriteRaw(" ");
        }

        WriteRaw("\r");
        WriteRaw(m_prompt);
        WriteRaw(m_input);
    }

    m_lastRenderLen = newLen;

    // Position the cursor (caret) by moving left with backspace.
    if (m_cursorPos > m_inputLen) {
        m_cursorPos = m_inputLen;
    }
    const size_t back = m_inputLen - m_cursorPos;
    for (size_t i = 0; i < back; ++i) {
        WriteRaw("\b");
    }
    m_transport.Flush();
}

void Console::BeginOutput() {
    // Clear the current prompt/input render so output doesn't "commit" it as a line.
    // No ANSI: overwrite with spaces.
    WriteRaw("\r");
    for (size_t i = 0; i < m_lastRenderLen; ++i) {
        WriteRaw(" ");
    }
    WriteRaw("\r");
    m_lastRenderLen = 0;
}

void Console::BeginBulkOutput() {
    if (m_bulkOutputDepth == 0) {
        BeginOutput();
    }

    if (m_bulkOutputDepth != 255U) {
        ++m_bulkOutputDepth;
    }
}

void Console::EndBulkOutput() {
    if (m_bulkOutputDepth == 0) {
        return;
    }

    --m_bulkOutputDepth;
    if (m_bulkOutputDepth == 0) {
        RedrawPrompt();
    }
}

void Console::WriteRaw(const char* data) {
    if (!data) {
        return;
    }
    WriteRaw(data, std::strlen(data));
}

void Console::WriteRaw(const char* data, const size_t len) {
    if (!data || len == 0) {
        return;
    }
    m_transport.Write(data, len);
}

} // namespace fxe
