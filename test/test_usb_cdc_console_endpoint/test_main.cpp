#include <algorithm>
#include <cstdio>
#include <string>

#include "firmware/hybrid_console_transport.hpp"
#include "firmware/usb_cdc_console_endpoint.hpp"
#include "foxie_embedded/console.hpp"
#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/settings_store.hpp"

namespace {
int failures = 0;
void Check(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::printf("[FAIL] usb_console: %s\n", message);
    }
}

// Hardware boundary: RX remains available with inactive modem lines, whereas
// pinned Arduino bool/availableForWrite/write gate TX on modem-line state.
struct UsbDevice {
    bool ready = true;
    uint8_t lineState = 0U;
    size_t capacity = 64U;
    size_t maxAcceptance = 64U;
    size_t writeCalls = 0U;
    std::string output;
    bool Ready() const { return ready; }
    uint8_t LineState() const { return lineState; }
    size_t AvailableForWrite() const { return capacity; }
    size_t Write(const uint8_t* data, const size_t size) {
        ++writeCalls;
        Check(size <= capacity, "one write never exceeds FIFO capacity");
        const size_t accepted = std::min({size, capacity, maxAcceptance});
        output.append(reinterpret_cast<const char*>(data), accepted);
        return accepted;
    }
    void Flush() {}
};

struct ArduinoSerial {
    UsbDevice& usb;
    std::string input;
    size_t offset = 0U;
    int available() const { return static_cast<int>(input.size() - offset); }
    int read() { return available() > 0 ? input[offset++] : -1; }
    explicit operator bool() const { return usb.ready && usb.lineState == 3U; }
    int availableForWrite() const {
        return usb.ready && (usb.lineState & 1U) ? static_cast<int>(usb.capacity) : 0;
    }
    size_t write(const uint8_t* data, const size_t size) {
        return availableForWrite() > 0 ? usb.Write(data, size) : 0U;
    }
};

struct BleEndpoint {
    std::string input;
    size_t offset = 0U;
    std::string output;
    bool TryReadConsoleInputChar(char& value) {
        if (offset == input.size()) return false;
        value = input[offset++];
        return true;
    }
    void WriteConsoleOutput(const char* data, const size_t size) {
        output.append(data, size);
    }
};

void TestCommandRepliesAcrossLineStates() {
    for (const uint8_t lineState : {0U, 1U, 2U, 3U}) {
        for (const auto* ending : {"\r", "\n", "\r\n"}) {
            UsbDevice usb;
            usb.lineState = lineState;
            ArduinoSerial serial{usb};
            BleEndpoint ble;
            firmware_internal::UsbCdcConsoleEndpoint endpoint(serial, usb);
            firmware_internal::HybridConsoleTransport transport(endpoint, ble);
            fxe::SettingsStoreConfig config;
            fxe::SettingsStore settings(config);
            settings.Doc()["MOVE_CONFIRM"] = "usb-regression";
            fxe::Console console(transport);
            console.SetSettingsStore(&settings);
            serial.input = std::string("settings get MOVE_CONFIRM") + ending;
            console.Update();
            for (unsigned i = 0U; i < 20U; ++i) console.Update();
            Check(serial.available() == 0, "real Console consumes the received command");
            Check(usb.output.find("usb-regression") != std::string::npos,
                  "settings response reaches USB for every modem state and CR/LF ending");
            Check(ble.output == "> settings get MOVE_CONFIRM\n\"usb-regression\"\n",
                  "BLE receives one command echo and exactly one complete response");
        }
    }
}

void TestBackpressureAndDisconnect() {
    UsbDevice usb;
    ArduinoSerial serial{usb};
    BleEndpoint ble;
    firmware_internal::UsbCdcConsoleEndpoint endpoint(serial, usb);
    firmware_internal::HybridConsoleTransport transport(endpoint, ble);
    Check(!static_cast<bool>(endpoint), "mounted idle USB alone does not enable output");
    serial.input = "x";
    char value = 0;
    Check(transport.TryReadChar(value) && value == 'x', "inactive-line RX enables replies");
    usb.capacity = 0U;
    transport.Write("reply\n", 6U);
    Check(usb.writeCalls == 0U && transport.PendingSerialBytes() == 6U,
          "stalled USB remains bounded without writing");
    ble.input = "b";
    Check(transport.TryReadChar(value) && value == 'b', "BLE RX remains live while USB stalls");
    Check(ble.output == "reply\n", "BLE TX remains live while USB stalls");
    usb.capacity = 3U;
    usb.maxAcceptance = 1U;
    for (unsigned i = 0U; i < 6U; ++i) transport.Flush();
    Check(usb.output == "reply\n" && transport.PendingSerialBytes() == 0U,
          "partial writes drain exactly once after capacity recovers");
    usb.capacity = 0U;
    transport.Write("stale\n", 6U);
    usb.ready = false;
    transport.Flush();
    Check(transport.PendingSerialBytes() == 0U && transport.DroppedSerialBytes() == 6U,
          "USB detach drops queued output");
    usb.ready = true;
    Check(!static_cast<bool>(endpoint), "reattachment requires a new host request or DTR");
    usb.lineState = 1U;
    Check(static_cast<bool>(endpoint), "DTR alone enables output without Arduino RTS gate");
    transport.Write("closed\n", 7U);
    usb.lineState = 0U;
    transport.Flush();
    Check(transport.PendingSerialBytes() == 0U && !static_cast<bool>(endpoint),
          "ordinary DTR close clears session and queued output");
    serial.input += "y";
    Check(transport.TryReadChar(value) && value == 'y' && static_cast<bool>(endpoint),
          "new inactive-line request reopens output after DTR close");
    usb.capacity = 64U;
    usb.maxAcceptance = 64U;
    transport.Write("new\n", 4U);
    Check(usb.output == "reply\nnew\n", "reconnect never replays discarded output");
}

void TestCloseWithBufferedInputDropsPreviousSession() {
    for (const bool pollInputFirst : {false, true}) {
        UsbDevice usb;
        usb.lineState = 1U;
        usb.capacity = 0U;
        ArduinoSerial serial{usb};
        BleEndpoint ble;
        firmware_internal::UsbCdcConsoleEndpoint endpoint(serial, usb);
        firmware_internal::HybridConsoleTransport transport(endpoint, ble);
        transport.Write("stale\n", 6U);
        usb.lineState = 0U;
        usb.capacity = 64U;
        serial.input = "n";
        char value = 0;
        if (pollInputFirst) {
            Check(!transport.TryReadChar(value), "close purges output before consuming new RX");
        } else {
            transport.Flush();
        }
        Check(usb.output.empty() && transport.DroppedSerialBytes() == 6U,
              "DTR close with buffered RX cannot replay the prior session");
        Check(transport.TryReadChar(value) && value == 'n', "next poll admits buffered new RX");
        transport.Write("new\n", 4U);
        Check(usb.output == "new\n", "new response survives the close boundary intact");
    }
}
void TestRetainedDiagnosticsDescribeStallAndHostRecovery() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 10U);
    UsbDevice usb;
    usb.lineState = 1U;
    usb.capacity = 0U;
    ArduinoSerial serial{usb};
    BleEndpoint ble;
    firmware_internal::UsbCdcConsoleEndpoint endpoint(serial, usb);
    firmware_internal::HybridConsoleTransport transport(endpoint, ble);
    serial.input = "v";
    char value = 0;
    Check(transport.TryReadChar(value) && value == 'v', "diagnostics leave RX behavior intact");
    transport.Write("reply\n", 6U);
    fxe::ManualMillis::SetNowMs(9U);
    transport.Flush();
    auto stats = endpoint.GetDiagnostics();
    Check(stats.rxBytes == 1U && stats.rxQueuedMax == 1U && stats.txAcceptedBytes == 0U,
          "stalled TX is distinct from a command reaching firmware");
    Check(stats.txZeroSamples >= 2U && stats.txZeroMaxMs == 20U && stats.ready && stats.lineState == 1U,
          "TX wait is retained and clock rollover is safe");
    usb.ready = false;
    transport.Flush();
    fxe::ManualMillis::SetNowMs(39U);
    transport.Flush();
    usb.ready = true;
    usb.capacity = 64U;
    transport.Flush();
    serial.input += "n";
    Check(transport.TryReadChar(value) && value == 'n', "host reset admits new input normally");
    transport.Write("new\n", 4U);
    stats = endpoint.GetDiagnostics();
    Check(stats.notReadySamples > 0U && stats.notReadyMaxMs == 30U && stats.txZeroMaxMs == 20U,
          "host recovery preserves prior mounted and FIFO wait observations");
    Check(stats.rxBytes == 2U && stats.txAcceptedBytes == 4U && stats.lastRxMs == 39U && stats.lastTxMs == 39U,
          "recovered traffic is counted without claiming host receipt");
    Check(usb.output == "new\n" && transport.DroppedSerialBytes() == 6U,
          "instrumentation preserves purge and exact output behavior");
    Check(sizeof(stats) <= 48U, "diagnostics remain a bounded RAM snapshot");
}
} // namespace

int main() {
    TestCommandRepliesAcrossLineStates();
    TestBackpressureAndDisconnect();
    TestCloseWithBufferedInputDropsPreviousSession();
    TestRetainedDiagnosticsDescribeStallAndHostRecovery();
    std::printf("[USB_CONSOLE] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
