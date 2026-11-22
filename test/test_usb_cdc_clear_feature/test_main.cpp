#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <tusb.h>

namespace {

bool g_mounted = true;
bool g_halted = false;
bool g_busy = true;
bool g_claimed = true;
unsigned g_queuedBytes = 64U;
unsigned g_dataToggle = 1U;
unsigned g_clearCalls = 0U;
unsigned g_forwardCalls = 0U;
unsigned g_failures = 0U;
bool g_forwardResult = false;
uint8_t g_lastPort = 0U;
uint8_t g_lastEndpoint = 0U;
const tusb_control_request_t* g_lastRequest = nullptr;
uint8_t g_lastStage = 0U;

bool ObservedMounted() { return g_mounted; }
extern "C" void ObservedClear(const uint8_t port, const uint8_t endpoint) {
    ++g_clearCalls;
    g_lastPort = port;
    g_lastEndpoint = endpoint;
    g_dataToggle = 0U;
}
extern "C" bool ObservedForward(const uint8_t port, const uint8_t stage,
                                const tusb_control_request_t* request) {
    ++g_forwardCalls;
    g_lastPort = port;
    g_lastStage = stage;
    g_lastRequest = request;
    return g_forwardResult;
}
void Check(const bool condition, const char* label) {
    if (condition) return;
    if (g_failures < 8U) std::printf("[FAIL] usb_clear_feature: %s\n", label);
    ++g_failures;
}

} // namespace

// Exercise the actual production callback at its SDK boundary. The observed
// DCD reset models only the pinned driver's bounded DATA0 register write.
#define ARDUINO 1
#define LITCH_HW_VARIANT_S3 1
#define ARDUINO_USB_MODE 0
#define tud_mounted ObservedMounted
#define dcd_edpt_clear_stall ObservedClear
#define __real_cdcd_control_xfer_cb ObservedForward
#define __wrap_cdcd_control_xfer_cb ObservedControl
#include "../../src/usb_cdc_clear_feature.cpp"
#undef __wrap_cdcd_control_xfer_cb
#undef __real_cdcd_control_xfer_cb
#undef dcd_edpt_clear_stall
#undef tud_mounted
#undef ARDUINO_USB_MODE
#undef LITCH_HW_VARIANT_S3
#undef ARDUINO

namespace {

bool HostClearFeature(const tusb_control_request_t& request) {
    // Equivalent controlled fault: actual pinned USBD calls DCD only when
    // software halt is set, then invokes the CDC control callback. This does
    // not simulate macOS packets, endpoint completion or a physical stall.
    if (g_halted) {
        ObservedClear(0U, static_cast<uint8_t>(request.wIndex));
        g_halted = false;
        g_busy = false;
    }
    return ObservedControl(0U, CONTROL_STAGE_SETUP, &request);
}

void CheckIgnored(tusb_control_request_t request, const uint8_t port = 0U,
                  const uint8_t stage = CONTROL_STAGE_SETUP) {
    const auto before = g_clearCalls;
    const auto forwards = g_forwardCalls;
    const bool result = ObservedControl(port, stage, &request);
    Check(g_clearCalls == before, "unrelated/invalid request cannot reset an endpoint");
    Check(g_forwardCalls == forwards + 1U && result == g_forwardResult &&
              g_lastRequest == &request && g_lastStage == stage,
          "original callback receives unchanged request exactly once");
}

} // namespace

int main() {
    tusb_control_request_t request{0x02U, 1U, 0U, 0x84U, 0U};
    for (const uint16_t endpoint : {uint16_t{0x03U}, uint16_t{0x84U}, uint16_t{0x85U}}) {
        request.wIndex = endpoint;
        g_dataToggle = 1U;
        g_halted = false;
        g_busy = true;
        const auto clears = g_clearCalls;
        Check(!HostClearFeature(request), "original standard-request return remains false");
        Check(g_dataToggle == 0U && g_clearCalls == clears + 1U &&
                  g_lastEndpoint == endpoint && g_lastPort == 0U,
              "unhalted configured CDC endpoint resets DATA0");
        Check(g_busy && g_claimed && g_queuedBytes == 64U,
              "repair preserves active transfer/claim and pending bytes");
    }
    g_halted = true;
    g_dataToggle = 1U;
    HostClearFeature(request);
    Check(!g_halted && !g_busy && g_dataToggle == 0U,
          "SDK halted-endpoint handling remains authoritative and repeated DATA0 is harmless");

    request.wIndex = 0x84U;
    CheckIgnored(request, 1U);
    CheckIgnored(request, 0U, CONTROL_STAGE_IDLE);
    CheckIgnored(request, 0U, CONTROL_STAGE_DATA);
    CheckIgnored(request, 0U, CONTROL_STAGE_ACK);
    for (const uint16_t endpoint : {uint16_t{0U}, uint16_t{0x80U}, uint16_t{0x04U},
                                   uint16_t{0x83U}, uint16_t{0x184U}, uint16_t{0xFFFFU}}) {
        request.wIndex = endpoint;
        CheckIgnored(request);
    }
    request.wIndex = 0x84U;
    for (const uint8_t type : {uint8_t{0x82U}, uint8_t{0x22U}, uint8_t{0x42U}, uint8_t{0x01U}}) {
        request.bmRequestType = type;
        CheckIgnored(request);
    }
    request.bmRequestType = 0x02U;
    request.bRequest = 3U;
    CheckIgnored(request);
    request.bRequest = 1U;
    request.wValue = 1U;
    CheckIgnored(request);
    request.wValue = 0U;
    request.wLength = 1U;
    CheckIgnored(request);
    request.wLength = 0U;
    g_mounted = false;
    CheckIgnored(request);
    g_mounted = true;
    g_forwardResult = true;
    const auto clears = g_clearCalls;
    const auto forwards = g_forwardCalls;
    Check(ObservedControl(0U, CONTROL_STAGE_SETUP, nullptr), "null request preserves forward result");
    Check(g_clearCalls == clears && g_forwardCalls == forwards + 1U && g_lastRequest == nullptr,
          "null request cannot touch endpoint state");
    std::printf("[USB_CLEAR_FEATURE] failures=%u clears=%u forwards=%u\n",
                g_failures, g_clearCalls, g_forwardCalls);
    return g_failures == 0U ? 0 : 1;
}
