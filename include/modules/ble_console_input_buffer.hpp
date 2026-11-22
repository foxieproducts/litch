#pragma once

#include <cstddef>
#include <cstdint>

namespace modules {

class BleConsoleInputBuffer {
  public:
    static constexpr size_t kCapacity = 256;

    void Clear() {
        m_head = 0;
        m_tail = 0;
        m_stagedHead = 0;
        m_stagedExpected = 0;
        m_stagedWritten = 0;
        m_staging = false;
    }

    size_t Enqueue(const char* data, const size_t len) {
        if (data == nullptr || len == 0 || m_staging) {
            return 0;
        }

        size_t written = 0;
        for (size_t i = 0; i < len; ++i) {
            const size_t nextHead = (m_head + 1U) % kCapacity;
            if (nextHead == m_tail) {
                break;
            }

            m_buffer[m_head] = data[i];
            m_head = nextHead;
            ++written;
        }

        return written;
    }

    bool BeginStagedRecord(const uint8_t expectedLength) {
        if (m_staging || expectedLength == 0U ||
            expectedLength > Available()) {
            return false;
        }
        m_stagedHead = m_head;
        m_stagedExpected = expectedLength;
        m_stagedWritten = 0U;
        m_staging = true;
        return true;
    }

    bool AppendStaged(const uint8_t* const data, const size_t len) {
        if (!m_staging || data == nullptr || len == 0U ||
            m_stagedWritten + len > m_stagedExpected) {
            return false;
        }
        for (size_t index = 0U; index < len; ++index) {
            m_buffer[m_stagedHead] = static_cast<char>(data[index]);
            m_stagedHead = (m_stagedHead + 1U) % kCapacity;
        }
        m_stagedWritten += len;
        return true;
    }

    bool CommitStagedRecord() {
        if (!m_staging || m_stagedWritten != m_stagedExpected) {
            return false;
        }
        m_head = m_stagedHead;
        ResetStaging();
        return true;
    }

    void DiscardStagedRecord() { ResetStaging(); }

    bool HasStagedRecord() const { return m_staging; }

    bool TryDequeue(char& outChar) {
        if (m_head == m_tail) {
            return false;
        }

        outChar = m_buffer[m_tail];
        m_tail = (m_tail + 1U) % kCapacity;
        return true;
    }

  private:
    size_t Available() const {
        if (m_head >= m_tail) {
            return kCapacity - (m_head - m_tail) - 1U;
        }
        return m_tail - m_head - 1U;
    }

    void ResetStaging() {
        m_stagedHead = m_head;
        m_stagedExpected = 0U;
        m_stagedWritten = 0U;
        m_staging = false;
    }

    char m_buffer[kCapacity] = {};
    size_t m_head = 0;
    size_t m_tail = 0;
    uint16_t m_stagedHead = 0;
    uint16_t m_stagedExpected = 0;
    uint16_t m_stagedWritten = 0;
    bool m_staging = false;
};

} // namespace modules
