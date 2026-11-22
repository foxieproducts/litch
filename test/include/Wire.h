#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Bounded Wire boundary for the PCA95x5 transport/scanner contract. Register
// values model electrical input and polarity; scan/retry policy stays in the
// production driver compiled by the test.
class TwoWire final {
  public:
    void begin() {}
    void begin(uint8_t, uint8_t) {}
    void setClock(uint32_t) {}
    void end() { ++busResetCount; }

    void beginTransmission(const uint8_t address) {
        m_address = address;
        m_written = 0U;
    }
    size_t write(const uint8_t value) {
        if (m_written >= m_bytes.size()) return 0U;
        m_bytes[m_written++] = value;
        return 1U;
    }
    uint8_t endTransmission() {
        if (txStatus != 0U) return txStatus;
        if (!modelExpanders) return 0U;
        const uint8_t index = m_address - 0x20U;
        if (index >= electricalInput.size()) return 4U;
        if (m_written == 1U && m_bytes[0U] == 0U && readFailures[index] != 0U) {
            --readFailures[index];
            return 4U;
        }
        if (m_written != 3U) return 0U;
        const uint8_t reg = m_bytes[0U];
        if (reg == 4U) ++polarityWrites[index];
        if (reg == 6U) ++directionWrites[index];
        if (index == failingExpander && reg == failingRegister &&
            configurationFailures != 0U &&
            (!failOnlyAfterBusReset || busResetCount != 0U)) {
            --configurationFailures;
            return 4U;
        }
        if (reg == 4U) {
            polarity[index] = static_cast<uint16_t>(
                m_bytes[1U] | (uint16_t{m_bytes[2U]} << 8U));
        }
        return 0U;
    }
    size_t requestFrom(const uint8_t address, uint8_t) {
        readIndex = 0U;
        if (modelExpanders) {
            const uint8_t index = address - 0x20U;
            const uint16_t value = electricalInput[index] ^ polarity[index];
            bytes[0U] = static_cast<uint8_t>(value);
            bytes[1U] = static_cast<uint8_t>(value >> 8U);
        }
        return requestedBytes;
    }
    int available() const { return readIndex < availableBytes ? 1 : 0; }
    int read() { return bytes[readIndex++]; }

    uint8_t txStatus = 0U;
    size_t requestedBytes = 2U;
    size_t availableBytes = 2U;
    size_t readIndex = 0U;
    uint8_t bytes[2U] = {0x34U, 0x12U};
    bool modelExpanders = false;
    std::array<uint16_t, 4U> electricalInput{0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU};
    std::array<uint16_t, 4U> polarity{};
    std::array<uint32_t, 4U> polarityWrites{};
    std::array<uint32_t, 4U> directionWrites{};
    std::array<uint8_t, 4U> readFailures{};
    uint8_t failingExpander = 0U;
    uint8_t failingRegister = 4U;
    uint32_t configurationFailures = 0U;
    bool failOnlyAfterBusReset = false;
    uint32_t busResetCount = 0U;

  private:
    uint8_t m_address = 0U;
    size_t m_written = 0U;
    std::array<uint8_t, 3U> m_bytes{};
};
