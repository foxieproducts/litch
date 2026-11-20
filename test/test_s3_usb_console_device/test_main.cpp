#include <cstdio>
#include <string>

#include "firmware/hybrid_console_transport.hpp"
#include "firmware/usb_cdc_console_endpoint.hpp"
#include "foxie_embedded/millis.hpp"

// Compile the actual S3 owner against the native SDK boundary below.
#define ARDUINO 1
#define LITCH_HW_VARIANT_S3 1
#define ARDUINO_USB_MODE 0
#include "firmware/s3_usb_console_device.hpp"
#undef ARDUINO_USB_MODE
#undef LITCH_HW_VARIANT_S3
#undef ARDUINO

namespace {

struct CdcBoundary {
    bool ready = true;
    bool busy = false;
    bool suspendAtAdmission = false;
    std::string fifo;
    std::string inFlight;
    std::string delivered;
    unsigned flushCalls = 0U;
};

CdcBoundary g_cdc;
unsigned g_failures = 0U;

void Check(const bool condition, const char* label) {
    if (!condition) {
        ++g_failures;
        std::printf("[FAIL] s3_usb_console: %s\n", label);
    }
}

void CompleteInTransfer() {
    g_cdc.delivered += g_cdc.inFlight;
    g_cdc.inFlight.clear();
    g_cdc.busy = false;
    // The linked SDK services the remaining FIFO after an IN completion.
    (void)tud_cdc_n_write_flush(0U);
}

struct ArduinoSerial {
    int available() const { return 0; }
    int read() const { return -1; }
};

struct BleEndpoint {
    bool TryReadConsoleInputChar(char&) { return false; }
    void WriteConsoleOutput(const char*, size_t) {}
};

} // namespace

bool tud_ready() { return g_cdc.ready; }
uint8_t tud_cdc_n_get_line_state(const uint8_t itf) {
    Check(itf == 0U, "CDC interface remains zero");
    return 1U;
}
uint32_t tud_cdc_n_write_available(const uint8_t itf) {
    Check(itf == 0U, "capacity uses the existing CDC interface");
    return static_cast<uint32_t>(64U - g_cdc.fifo.size());
}
uint32_t tud_cdc_n_write(const uint8_t itf, const void* buffer, const uint32_t size) {
    Check(itf == 0U && size <= 64U - g_cdc.fifo.size(), "admission is FIFO-bounded");
    g_cdc.fifo.append(static_cast<const char*>(buffer), size);
    if (g_cdc.suspendAtAdmission) {
        g_cdc.ready = false;
        g_cdc.suspendAtAdmission = false;
    }
    // Controlled fault from the compiled SDK: suspend can skip this flush
    // after FIFO admission. Resume alone does not pump the CDC FIFO.
    if (g_cdc.fifo.size() == 64U) (void)tud_cdc_n_write_flush(0U);
    return size;
}
uint32_t tud_cdc_n_write_flush(const uint8_t itf) {
    Check(itf == 0U, "flush uses the existing CDC interface");
    ++g_cdc.flushCalls;
    if (!g_cdc.ready || g_cdc.busy || g_cdc.fifo.empty()) return 0U;
    g_cdc.inFlight = g_cdc.fifo;
    g_cdc.fifo.clear();
    g_cdc.busy = true;
    return static_cast<uint32_t>(g_cdc.inFlight.size());
}

namespace {

void TestMissedFlushResumesThroughProductionPump() {
    g_cdc = {};
    g_cdc.suspendAtAdmission = true;
    firmware_internal::S3UsbConsoleDevice usb;
    ArduinoSerial serial;
    BleEndpoint ble;
    firmware_internal::UsbCdcConsoleEndpoint endpoint(serial, usb);
    firmware_internal::HybridConsoleTransport transport(endpoint, ble);
    const std::string admitted(64U, 'a');
    transport.Write(admitted.data(), admitted.size());
    Check(!g_cdc.ready && g_cdc.fifo == admitted && g_cdc.inFlight.empty(),
          "suspend skips automatic and explicit flush after admission");
    transport.Flush();
    g_cdc.ready = true;
    transport.Flush(); // Consume the adapter's close boundary before new input.
    transport.Write("reply\n", 6U);
    for (unsigned pump = 0U; pump < 5U; ++pump) transport.Flush();
    Check(transport.PendingSerialBytes() == 0U && g_cdc.inFlight == admitted &&
              g_cdc.fifo == "reply\n" && g_cdc.delivered.empty(),
          "resumed full FIFO is serviced without pretending host delivery");
    CompleteInTransfer();
    CompleteInTransfer();
    Check(g_cdc.delivered == admitted + "reply\n" && g_cdc.fifo.empty() &&
              g_cdc.inFlight.empty(),
          "subsequent IN completions deliver admitted data and the new reply once");
}

void TestFullBusyAndNotReadyRemainBackpressured() {
    g_cdc = {};
    g_cdc.fifo.assign(64U, 'b');
    g_cdc.busy = true;
    firmware_internal::S3UsbConsoleDevice usb;
    Check(usb.AvailableForWrite() == 0U && g_cdc.flushCalls == 1U &&
              g_cdc.fifo.size() == 64U && g_cdc.inFlight.empty(),
          "busy IN gets one attempt without a host-wait loop or fake capacity");
    g_cdc.ready = false;
    const unsigned before = g_cdc.flushCalls;
    Check(usb.AvailableForWrite() == 0U && g_cdc.flushCalls == before,
          "not-ready full FIFO is left for the next ready pump");
    g_cdc.ready = true;
    g_cdc.fifo.resize(32U);
    Check(usb.AvailableForWrite() == 32U && g_cdc.flushCalls == before,
          "ordinary available capacity does not add a service attempt");
}

} // namespace

int main() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    TestMissedFlushResumesThroughProductionPump();
    TestFullBusyAndNotReadyRemainBackpressured();
    std::printf("[S3_USB_CONSOLE] failures=%u\n", g_failures);
    return g_failures == 0U ? 0 : 1;
}
