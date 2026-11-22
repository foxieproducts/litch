#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "foxie_embedded/console_transport.hpp"

namespace firmware_internal {

template <typename SerialEndpoint, typename BleEndpoint>
class HybridConsoleTransport final : public fxe::IConsoleTransport {
  public:
    HybridConsoleTransport(SerialEndpoint& serial, BleEndpoint& ble) : m_serial(serial), m_ble(ble) {}

    bool TryReadChar(char& outChar) override {
        if (m_serial.available() > 0) {
            outChar = static_cast<char>(m_serial.read());
            m_lastInputSource = fxe::ConsoleInputSource::Local;
            return true;
        }

        if (m_ble.TryReadConsoleInputChar(outChar)) {
            m_lastInputSource = fxe::ConsoleInputSource::Remote;
            return true;
        }

        PumpSerialOutput();
        return false;
    }

    fxe::ConsoleInputSource LastInputSource() const override {
        return m_lastInputSource;
    }

    uint64_t RemoteInputGeneration() const override {
        return ReadRemoteGeneration(m_ble, 0);
    }

    void Write(const char* data, const size_t len) override {
        if (data == nullptr || len == 0) {
            return;
        }

        for (size_t i = 0; i < len; ++i) {
            const char c = data[i];

            if (m_pendingCarriageReturn) {
                if (c == '\n') {
                    FlushBleLineToApp();
                    m_pendingCarriageReturn = false;
                    continue;
                }

                m_bleLineLen = 0;
                m_pendingCarriageReturn = false;
            }

            if (c == '\r') {
                m_pendingCarriageReturn = true;
                continue;
            }
            if (c == '\b' || c == static_cast<char>(0x7F)) {
                if (m_bleLineLen > 0) {
                    --m_bleLineLen;
                }
                continue;
            }
            if (c == '\n') {
                FlushBleLineToApp();
                continue;
            }

            if (m_bleLineLen + 1U >= BLE_LINE_BUFFER_CAPACITY) {
                FlushBleLineToApp();
            }

            if (m_bleLineLen + 1U < BLE_LINE_BUFFER_CAPACITY) {
                m_bleLineBuffer[m_bleLineLen++] = c;
            }
        }

        QueueSerialOutput(data, len);
        PumpSerialOutput();
    }

    // Arduino CDC writes already request an endpoint flush. A blocking
    // transport flush must never be introduced into the cooperative loop.
    void Flush() override { PumpSerialOutput(); }

    size_t PendingSerialBytes() const { return m_serialOutputLen; }
    uint32_t DroppedSerialBytes() const { return m_droppedSerialBytes; }
    static constexpr size_t SerialOutputCapacity() {
        return SERIAL_OUTPUT_BUFFER_CAPACITY;
    }

  private:
    // Complete app console records contain at most 2047 bytes, excluding the
    // newline. Reserve the final byte for that terminator, not an early split.
    static constexpr size_t BLE_LINE_BUFFER_CAPACITY = 2048;
    // Holds one maximum support record after the ordinary welcome and all 16
    // retained 255-byte boot log lines, including terminal redraw overhead.
    static constexpr size_t SERIAL_OUTPUT_BUFFER_CAPACITY = 6144U;

    void RecordDroppedSerialBytes(const size_t count) {
        const uint32_t bounded =
            count > std::numeric_limits<uint32_t>::max()
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(count);
        const uint32_t remaining =
            std::numeric_limits<uint32_t>::max() - m_droppedSerialBytes;
        m_droppedSerialBytes += bounded > remaining ? remaining : bounded;
    }

    void ClearSerialOutputAsDropped() {
        RecordDroppedSerialBytes(m_serialOutputLen);
        m_serialOutputHead = 0U;
        m_serialOutputLen = 0U;
    }

    void QueueSerialOutput(const char* data, const size_t len) {
        if (!static_cast<bool>(m_serial)) {
            ClearSerialOutputAsDropped();
            RecordDroppedSerialBytes(len);
            return;
        }

        const size_t remaining =
            SERIAL_OUTPUT_BUFFER_CAPACITY - m_serialOutputLen;
        const size_t accepted = len < remaining ? len : remaining;
        const size_t tail =
            (m_serialOutputHead + m_serialOutputLen) %
            SERIAL_OUTPUT_BUFFER_CAPACITY;
        const size_t first =
            accepted < (SERIAL_OUTPUT_BUFFER_CAPACITY - tail)
                ? accepted
                : SERIAL_OUTPUT_BUFFER_CAPACITY - tail;
        if (first != 0U) {
            std::memcpy(m_serialOutputBuffer + tail, data, first);
        }
        if (accepted > first) {
            std::memcpy(m_serialOutputBuffer, data + first, accepted - first);
        }
        m_serialOutputLen += accepted;
        RecordDroppedSerialBytes(len - accepted);
    }

    void PumpSerialOutput() {
        if (!static_cast<bool>(m_serial)) {
            ClearSerialOutputAsDropped();
            return;
        }
        if (m_serialOutputLen == 0U) {
            return;
        }

        const int available = m_serial.availableForWrite();
        if (available <= 0) {
            return;
        }
        size_t requested = static_cast<size_t>(available);
        if (requested > m_serialOutputLen) {
            requested = m_serialOutputLen;
        }
        const size_t contiguous =
            SERIAL_OUTPUT_BUFFER_CAPACITY - m_serialOutputHead;
        if (requested > contiguous) {
            requested = contiguous;
        }

        size_t written = m_serial.write(
            reinterpret_cast<const uint8_t*>(
                m_serialOutputBuffer + m_serialOutputHead),
            requested);
        if (written > requested) {
            written = requested;
        }
        m_serialOutputHead =
            (m_serialOutputHead + written) % SERIAL_OUTPUT_BUFFER_CAPACITY;
        m_serialOutputLen -= written;
        if (m_serialOutputLen == 0U) {
            m_serialOutputHead = 0U;
        }
    }

    void FlushBleLineToApp() {
        if (m_bleLineLen == 0) {
            return;
        }

        bool hasVisible = false;
        for (size_t i = 0; i < m_bleLineLen; ++i) {
            const char c = m_bleLineBuffer[i];
            if (c != ' ' && c != '\t') {
                hasVisible = true;
                break;
            }
        }

        if (!hasVisible) {
            m_bleLineLen = 0;
            return;
        }

        if (m_bleLineLen == 2 && m_bleLineBuffer[0] == '>' && m_bleLineBuffer[1] == ' ') {
            m_bleLineLen = 0;
            return;
        }

        m_bleLineBuffer[m_bleLineLen++] = '\n';
        // The sink must synchronously copy these bytes before returning;
        // BleService::WriteConsoleOutput copies into its owned output queue.
        m_ble.WriteConsoleOutput(m_bleLineBuffer, m_bleLineLen);
        m_bleLineLen = 0;
    }

    template <typename Endpoint>
    static auto ReadRemoteGeneration(const Endpoint& endpoint, int)
        -> decltype(endpoint.ConsoleInputGeneration()) {
        return endpoint.ConsoleInputGeneration();
    }
    template <typename Endpoint>
    static uint64_t ReadRemoteGeneration(const Endpoint&, long) { return 0U; }

    SerialEndpoint& m_serial;
    BleEndpoint& m_ble;
    fxe::ConsoleInputSource m_lastInputSource = fxe::ConsoleInputSource::Unknown;
    char m_bleLineBuffer[BLE_LINE_BUFFER_CAPACITY] = {};
    size_t m_bleLineLen = 0;
    bool m_pendingCarriageReturn = false;
    char m_serialOutputBuffer[SERIAL_OUTPUT_BUFFER_CAPACITY] = {};
    size_t m_serialOutputHead = 0U;
    size_t m_serialOutputLen = 0U;
    uint32_t m_droppedSerialBytes = 0U;
};

} // namespace firmware_internal
