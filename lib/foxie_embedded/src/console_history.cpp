#include <cstring>

#include "foxie_embedded/console.hpp"

namespace fxe {

namespace {

size_t ClampCopyLen(const size_t len, const size_t outCap) {
    if (outCap == 0) {
        return 0;
    }
    const size_t maxCopy = outCap - 1;
    return (len <= maxCopy) ? len : maxCopy;
}

} // namespace

void Console::HistoryPrev() {
    if (m_historyCount == 0) {
        return;
    }

    if (m_historyIndex == m_historyCount) {
        std::strncpy(m_historyScratch, m_input, INPUT_CAPACITY);
        m_historyScratch[INPUT_CAPACITY - 1] = '\0';
    }

    if (m_historyIndex == 0) {
        return;
    }

    --m_historyIndex;
    SetInputFromHistory();
}

void Console::HistoryNext() {
    if (m_historyCount == 0) {
        return;
    }

    if (m_historyIndex >= m_historyCount) {
        return;
    }

    ++m_historyIndex;
    SetInputFromHistory();
}

void Console::SetInputFromHistory() {
    // Shared history and scratch text can originate on another transport.
    // A privileged action requires a freshly typed complete local line.
    m_inputSource = ConsoleInputSource::Unknown;
    m_inputSourceObserved = true;
    if (m_historyIndex >= m_historyCount) {
        std::strncpy(m_input, m_historyScratch, INPUT_CAPACITY);
        m_input[INPUT_CAPACITY - 1] = '\0';
        m_inputLen = std::strlen(m_input);
    } else {
        m_inputLen = HistoryGetLine(m_historyIndex, m_input, INPUT_CAPACITY);
    }

    m_cursorPos = m_inputLen;

    RedrawPrompt();
}

bool Console::HistoryEqualsLast(const char* line, const size_t len) const {
    if (!line || len == 0 || m_historyCount == 0) {
        return false;
    }

    const size_t lastIndex =
        (m_historyOldestIndex + m_historyCount - 1U) % HISTORY_CAPACITY;
    const HistoryEntry& e = m_historyEntries[lastIndex];
    if (e.len != len) {
        return false;
    }
    return std::memcmp(e.line, line, len) == 0;
}

size_t Console::HistoryGetLine(const size_t indexFromOldest, char* out, const size_t outCap) const {
    if (!out || outCap == 0) {
        return 0;
    }
    out[0] = '\0';

    if (indexFromOldest >= m_historyCount) {
        return 0;
    }

    const size_t metaIndex = (m_historyOldestIndex + indexFromOldest) % HISTORY_CAPACITY;
    const HistoryEntry& e = m_historyEntries[metaIndex];

    const size_t copyLen = ClampCopyLen(e.len, outCap);
    if (copyLen == 0) {
        out[0] = '\0';
        return 0;
    }

    std::memcpy(out, e.line, copyLen);
    out[copyLen] = '\0';
    return copyLen;
}

void Console::PushHistoryLine(const char* line, const size_t len) {
    if (!line || len == 0) {
        return;
    }
    const size_t copyLen = ClampCopyLen(len, INPUT_CAPACITY);
    const size_t writeIndex = m_historyCount < HISTORY_CAPACITY
                                  ? (m_historyOldestIndex + m_historyCount) %
                                        HISTORY_CAPACITY
                                  : m_historyOldestIndex;
    if (m_historyCount < HISTORY_CAPACITY) {
        ++m_historyCount;
    } else {
        m_historyOldestIndex =
            (m_historyOldestIndex + 1U) % HISTORY_CAPACITY;
    }

    HistoryEntry& entry = m_historyEntries[writeIndex];
    std::memcpy(entry.line, line, copyLen);
    entry.line[copyLen] = '\0';
    entry.len = static_cast<uint8_t>(copyLen);
}

} // namespace fxe
