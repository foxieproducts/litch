#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "foxie_embedded/millis.hpp"

namespace firmware_internal {

// The console owns TX; Arduino retains RX buffering and upload handshakes.
template <typename SerialEndpoint, typename UsbDevice>
class UsbCdcConsoleEndpoint final {
  public:
    // Cooperative-owner observations survive a host-only USB reset in RAM.
    // Accepted TX bytes describe FIFO admission, not delivery to the host.
    struct Diagnostics {
        uint32_t rxBytes = 0U;
        uint32_t txAcceptedBytes = 0U;
        uint32_t rxQueuedMax = 0U;
        uint32_t notReadySamples = 0U;
        uint32_t notReadyMaxMs = 0U;
        uint32_t txZeroSamples = 0U;
        uint32_t txZeroMaxMs = 0U;
        uint32_t lastRxMs = 0U;
        uint32_t lastTxMs = 0U;
        uint8_t lineState = 0U;
        bool ready = false;
    };

    UsbCdcConsoleEndpoint(SerialEndpoint& serial, UsbDevice& usb)
        : m_serial(serial), m_usb(usb) {}

    int available() {
        if (!RefreshConnection()) return 0;
        const int queued = m_serial.available();
        ObserveRxQueue(queued);
        return queued;
    }
    int read() {
        const int value = m_serial.read();
        if (value >= 0) {
            AddCount(m_diagnostics.rxBytes, 1U);
            m_diagnostics.lastRxMs = fxe::Millis();
        }
        return value;
    }
    Diagnostics GetDiagnostics() const { return m_diagnostics; }
    explicit operator bool() {
        const bool connected = RefreshConnection();
        // HybridConsoleTransport discards its queue on a false boolean check.
        // RX/capacity polls must not consume this boundary before that purge.
        if (m_outputResetPending) {
            m_outputResetPending = false;
            return false;
        }
        return connected;
    }
    int availableForWrite() {
        if (!RefreshConnection()) return 0;
        const size_t capacity = m_usb.AvailableForWrite();
        ObserveCapacity(capacity);
        return static_cast<int>(capacity);
    }
    size_t write(const uint8_t* data, const size_t size) {
        if (data == nullptr || size == 0U || !RefreshConnection()) return 0U;
        const size_t capacity = m_usb.AvailableForWrite();
        ObserveCapacity(capacity);
        const size_t requested = size < capacity ? size : capacity;
        if (requested == 0U) return 0U;
        const size_t written = m_usb.Write(data, requested);
        AddCount(m_diagnostics.txAcceptedBytes, written);
        if (written != 0U) m_diagnostics.lastTxMs = fxe::Millis();
        m_usb.Flush();
        return written;
    }

  private:
    struct WaitObservation {
        uint32_t startedMs = 0U;
        bool active = false;
    };

    static void AddCount(uint32_t& count, const size_t amount) {
        const uint32_t remaining = std::numeric_limits<uint32_t>::max() - count;
        count += amount > remaining ? remaining : static_cast<uint32_t>(amount);
    }

    static void ObserveWait(const bool waiting, WaitObservation& wait,
                            uint32_t& longestMs) {
        if (!waiting && !wait.active) return;
        const uint32_t nowMs = fxe::Millis();
        if (!wait.active) wait.startedMs = nowMs;
        const uint32_t elapsedMs = nowMs - wait.startedMs;
        if (elapsedMs > longestMs) longestMs = elapsedMs;
        wait.active = waiting;
    }

    void ObserveRxQueue(const int queued) {
        if (queued > 0 && static_cast<uint32_t>(queued) > m_diagnostics.rxQueuedMax) {
            m_diagnostics.rxQueuedMax = static_cast<uint32_t>(queued);
        }
    }

    void ObserveCapacity(const size_t capacity) {
        if (capacity == 0U) AddCount(m_diagnostics.txZeroSamples, 1U);
        ObserveWait(capacity == 0U, m_txZeroWait, m_diagnostics.txZeroMaxMs);
    }

    bool RefreshConnection() {
        const bool ready = m_usb.Ready();
        m_diagnostics.ready = ready;
        m_diagnostics.lineState = ready ? m_usb.LineState() : 0U;
        if (!ready) AddCount(m_diagnostics.notReadySamples, 1U);
        ObserveWait(!ready, m_notReadyWait, m_diagnostics.notReadyMaxMs);
        const bool dtr = ready && (m_diagnostics.lineState & 1U) != 0U;
        if (!ready || (m_dtr && !dtr)) {
            ObserveWait(false, m_txZeroWait, m_diagnostics.txZeroMaxMs);
            m_receivedInput = false;
            m_outputResetPending = true;
        }
        m_dtr = dtr;
        if (m_outputResetPending) return false;
        // RX proves that the host wants replies even when its terminal never
        // asserts modem lines. Do not depend on Arduino's separate RTS/reboot
        // handshake state. A falling DTR still closes an ordinary session.
        if (ready) {
            const int queued = m_serial.available();
            ObserveRxQueue(queued);
            if (queued > 0) m_receivedInput = true;
        }
        return ready && (dtr || m_receivedInput);
    }

    SerialEndpoint& m_serial;
    UsbDevice& m_usb;
    bool m_dtr = false;
    bool m_receivedInput = false;
    bool m_outputResetPending = false;
    Diagnostics m_diagnostics;
    WaitObservation m_notReadyWait;
    WaitObservation m_txZeroWait;
};

} // namespace firmware_internal
