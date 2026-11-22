#include <device/dcd.h>
#include <device/usbd_pvt.h>
#include <esp_intr_alloc.h>
#include <esp_system.h>
#include <soc/usb_reg.h>
#include <soc/usb_struct.h>
#include <tusb.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "firmware/native_usb_reset_recovery.hpp"
#include "foxie_embedded/console.hpp"
#include "foxie_embedded/millis.hpp"
extern "C" BaseType_t __wrap_xQueueGenericSend(QueueHandle_t, const void*, TickType_t, BaseType_t);
extern "C" BaseType_t __wrap_xQueueGenericSendFromISR(QueueHandle_t, const void*, BaseType_t*, BaseType_t);
extern "C" void __wrap_dcd_event_handler(const dcd_event_t*, bool);
extern "C" void __wrap_dcd_init(uint8_t);
extern "C" void __wrap_dcd_connect(uint8_t);
extern "C" void __wrap_dcd_int_enable(uint8_t);
extern "C" esp_err_t __wrap_esp_intr_alloc(int, int, intr_handler_t, void*, intr_handle_t*);
extern "C" void __real_dcd_event_handler(const dcd_event_t*, bool);
usb_dev_t USB0;
bool irqEnabled = true, driverCleanup = false, drainRx = false, emitReset = false, eager = false;
unsigned driverCalls = 0, initCalls = 0;
bool persistInit = false, failEnable = false, failDisable = false;
bool injectRawAtConnect = false;
TickType_t lastQueueWait = 0;
std::mutex blockedMutex;
std::condition_variable blockedCondition;
bool blockStop = false, stopEntered = false, releaseStop = false;
bool blockDriver = false, driverEntered = false, releaseDriver = false;
void (*oldBarrier)(void*) = nullptr;
void* oldBarrierParameter = nullptr;
void Noop(void*);
intr_handler_t allocatedIrq = nullptr;
void Drain();
void Driver(void*) {
    ++driverCalls;
    if (blockDriver) {
        std::unique_lock<std::mutex> lock(blockedMutex);
        driverEntered = true;
        blockedCondition.notify_all();
        blockedCondition.wait(lock, [] { return releaseDriver; });
    }
    if (driverCleanup) {
        USB0.dcfg &= ~0x7F0U;
        USB0.daintmsk = 0x10001U;
    }
    if (drainRx)
        USB0.gintsts &= ~USB_RXFLVI_M;
    if (emitReset)
        dcd_event_bus_reset(0, TUSB_SPEED_FULL, true);
}
struct Queue {
    unsigned depth, size;
    std::deque<std::vector<unsigned char>> events;
};
unsigned failures = 0, arms = 0, completed = 0, detaches = 0, attaches = 0, stops = 0, sends = 0, refused = 0, barrierAcks = 0;
bool connected = true, completeStops = true;
std::array<unsigned char*, 16> buffers{};
std::array<unsigned, 16> lengths{};
void Check(bool b, const char* label) {
    if (!b) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
extern "C" {
QueueHandle_t xQueueCreate(unsigned depth, unsigned size) {
    return new Queue{depth, size, {}};
}
BaseType_t __real_xQueueGenericSend(QueueHandle_t h, const void* p, TickType_t wait, BaseType_t) {
    auto& q = *static_cast<Queue*>(h);
    ++sends;
    lastQueueWait = wait;
    if (q.size == 0)
        return 1;
    if (q.events.size() == q.depth) {
        ++refused;
        return 0;
    }
    const auto* event = static_cast<const dcd_event_t*>(p);
    if (event->event_id == USBD_EVENT_FUNC_CALL && event->func_call.func != Noop) {
        oldBarrier = event->func_call.func;
        oldBarrierParameter = event->func_call.param;
    }
    auto b = static_cast<const unsigned char*>(p);
    q.events.emplace_back(b, b + q.size);
    if (eager)
        Drain();
    return 1;
}
BaseType_t xQueueReceive(QueueHandle_t h, void* p, TickType_t) {
    auto& q = *static_cast<Queue*>(h);
    if (q.events.empty())
        return 0;
    std::memcpy(p, q.events.front().data(), q.size);
    q.events.pop_front();
    return 1;
}
BaseType_t xQueueGenericSend(QueueHandle_t h, const void* p, TickType_t wait, BaseType_t position) {
#ifdef LITCH_USB_OLD_SOURCE
    return __real_xQueueGenericSend(h, p, wait, position);
#else
    return __wrap_xQueueGenericSend(h, p, wait, position);
#endif
}
BaseType_t __real_xQueueGenericSendFromISR(QueueHandle_t h, const void* p, BaseType_t*, BaseType_t position) {
    return __real_xQueueGenericSend(h, p, 0, position);
}
BaseType_t xQueueSendToBackFromISR(QueueHandle_t h, const void* p, BaseType_t* w) {
#ifdef LITCH_USB_OLD_SOURCE
    return __real_xQueueGenericSendFromISR(h, p, w, 0);
#else
    return __wrap_xQueueGenericSendFromISR(h, p, w, 0);
#endif
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t h) {
    return static_cast<Queue*>(h)->events.size();
}
BaseType_t xQueueReset(QueueHandle_t h) {
    static_cast<Queue*>(h)->events.clear();
    return 1;
}
SemaphoreHandle_t xSemaphoreCreateBinary() {
    return reinterpret_cast<void*>(1);
}
SemaphoreHandle_t xSemaphoreCreateMutex() {
    return reinterpret_cast<void*>(1);
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t) {
    return 1;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) {
    return 1;
}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t, BaseType_t*) {
    return 1;
}
void vTaskDelay(TickType_t) {}
void __real_dcd_connect(uint8_t);
void __real_dcd_init(uint8_t p) {
    ++initCalls;
    USB0.gintsts = 0;
    if (persistInit) {
        dcd_event_bus_reset(0, TUSB_SPEED_FULL, true);
        tusb_control_request_t request{};
        request.bRequest = 9;
        request.wValue = 1;
        dcd_event_setup_received(0, reinterpret_cast<uint8_t*>(&request), true);
    } else
        __real_dcd_connect(p);
}
void dcd_init(uint8_t p) {
    __wrap_dcd_init(p);
}
void dcd_event_handler(const dcd_event_t* e, bool isr) {
    __wrap_dcd_event_handler(e, isr);
}
void dcd_int_enable(uint8_t p) {
    __wrap_dcd_int_enable(p);
}
void __real_dcd_int_enable(uint8_t) {
    intr_handle_t h = nullptr;
    __wrap_esp_intr_alloc(38, 14, Driver, nullptr, &h);
}
esp_err_t __real_esp_intr_alloc(int, int, intr_handler_t h, void*, intr_handle_t* out) {
    allocatedIrq = h;
    *out = reinterpret_cast<void*>(1);
    return ESP_OK;
}
esp_err_t esp_intr_disable(intr_handle_t) {
    if (failDisable)
        return -1;
    irqEnabled = false;
    return ESP_OK;
}
esp_err_t esp_intr_enable(intr_handle_t) {
    if (failEnable)
        return -1;
    irqEnabled = true;
    return ESP_OK;
}
void dcd_int_disable(uint8_t) {}
void __real_dcd_connect(uint8_t) {
    if (!connected)
        ++attaches;
    connected = true;
    if (injectRawAtConnect) {
        USB0.gintsts |= USB_USBRST_M;
        const auto before = driverCalls;
        allocatedIrq(nullptr);
        Check(driverCalls == before, "pending internal-attach IRQ cannot enter before FreshReset publication");
    }
}
void dcd_connect(uint8_t p) {
    __wrap_dcd_connect(p);
}
void dcd_disconnect(uint8_t) {
    connected = false;
    ++detaches;
}
void dcd_set_address(uint8_t, uint8_t) {}
void dcd_remote_wakeup(uint8_t) {}
void dcd_sof_enable(uint8_t, bool) {}
bool dcd_edpt_open(uint8_t, const tusb_desc_endpoint_t*) {
    return true;
}
void dcd_edpt_close_all(uint8_t) {}
void dcd_edpt_close(uint8_t, uint8_t) {}
bool dcd_edpt_iso_alloc(uint8_t, uint8_t, uint16_t) {
    return false;
}
bool dcd_edpt_iso_activate(uint8_t, const tusb_desc_endpoint_t*) {
    return false;
}
bool dcd_edpt_xfer_fifo(uint8_t, uint8_t, tu_fifo_t*, uint16_t) {
    return false;
}
usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* n) {
    *n = 0;
    return nullptr;
}
uint8_t const* tud_descriptor_bos_cb() {
    return nullptr;
}
uint8_t const* tud_descriptor_device_qualifier_cb() {
    return nullptr;
}
uint8_t const* tud_descriptor_other_speed_configuration_cb(uint8_t) {
    return nullptr;
}
void tud_mount_cb() {}
void tud_suspend_cb(bool) {}
void tud_resume_cb() {}
bool tud_vendor_control_xfer_cb(uint8_t, uint8_t, const tusb_control_request_t*) {
    return false;
}
void tud_cdc_rx_cb(uint8_t) {}
void tud_cdc_rx_wanted_cb(uint8_t, char) {}
void tud_cdc_tx_complete_cb(uint8_t) {}
void tud_cdc_line_state_cb(uint8_t, bool, bool) {}
void tud_cdc_line_coding_cb(uint8_t, const cdc_line_coding_t*) {}
void tud_cdc_send_break_cb(uint8_t, uint16_t) {}

void dcd_edpt_stall(uint8_t, uint8_t) {}
void dcd_edpt_clear_stall(uint8_t, uint8_t) {}
bool dcd_edpt_xfer(uint8_t, uint8_t ep, uint8_t* b, uint16_t n) {
    buffers[tu_edpt_number(ep) + (tu_edpt_dir(ep) ? 8 : 0)] = b;
    lengths[tu_edpt_number(ep) + (tu_edpt_dir(ep) ? 8 : 0)] = n;
    if (ep == 0x84)
        ++arms;
    return true;
}
uint8_t const* tud_descriptor_device_cb() {
    static const tusb_desc_device_t desc = {18, TUSB_DESC_DEVICE, 0x0200, 0, 0, 0, 64, 0x303a, 0x4001, 0x100, 1, 2, 3, 1};
    return reinterpret_cast<const uint8_t*>(&desc);
}
uint8_t const* tud_descriptor_configuration_cb(uint8_t) {
    static const uint8_t d[] = {TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN, 0, 100), TUD_CDC_DESCRIPTOR(0, 0, 0x83, 8, 0x04, 0x84, 64)};
    return d;
}
uint16_t const* tud_descriptor_string_cb(uint8_t, uint16_t) {
    return nullptr;
}
void tud_umount_cb() {
    if (blockStop) {
        std::unique_lock<std::mutex> lock(blockedMutex);
        stopEntered = true;
        blockedCondition.notify_all();
        blockedCondition.wait(lock, [] { return releaseStop; });
    }
    if (completeStops)
        ++stops;
}
}
extern "C" void __real_dcd_edpt_clear_stall(uint8_t p, uint8_t e) {
    dcd_edpt_clear_stall(p, e);
}
extern "C" bool __real_dcd_edpt_xfer(uint8_t p, uint8_t e, uint8_t* b, uint16_t n) {
    return dcd_edpt_xfer(p, e, b, n);
}
extern "C" bool __real_tud_connect() {
    return tud_connect();
}
extern "C" bool __real_tud_disconnect() {
    return tud_disconnect();
}
extern "C" void __real_tud_mount_cb() {
    tud_mount_cb();
}
unsigned ObservedResetReason() {
    return 3U;
}
#define ESP_RST_SW 3U
#define ESP_RST_DEEPSLEEP 8U
#define ARDUINO 1
#define LITCH_HW_VARIANT_S3 1
#define ARDUINO_USB_MODE 0
#define esp_reset_reason ObservedResetReason
#include LITCH_USB_RECOVERY_SOURCE
#undef esp_reset_reason
#include LITCH_USB_IRQ_SOURCE
#include LITCH_USB_EVENT_SOURCE
void fxe::Console::PrintLn(const char*, ...) {}
void Drain() {
    while (tud_task_event_ready())
        tud_task_ext(0, false);
}
void Setup(uint8_t type, uint8_t req, uint16_t value, uint16_t index) {
    tusb_control_request_t r{};
    r.bmRequestType = type;
    r.bRequest = req;
    r.wValue = value;
    r.wIndex = index;
    dcd_event_setup_received(0, reinterpret_cast<uint8_t*>(&r), false);
    Drain();
    dcd_event_xfer_complete(0, 0x80, 0, XFER_RESULT_SUCCESS, false);
    Drain();
}
void Mount() {
    dcd_event_bus_reset(0, TUSB_SPEED_FULL, false);
    Drain();
    Setup(0, 9, 1, 0);
    Setup(0x21, 0x22, 1, 0);
    Check(tud_mounted(), "actual USBD mounted");
    Check(tud_cdc_n_connected(0), "actual CDC DTR owned");
}
void Noop(void*) {}
void Barrier(void*) {
    ++barrierAcks;
}
int main() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    Queue semaphore{1, 0, {}};
    xQueueGenericSend(&semaphore, nullptr, 37, 0);
    Check(lastQueueWait == 37, "unrelated null semaphore send retains caller wait");
    tud_init(0);
    Mount();
    const char reply[] = "version:Litch\n";
    tud_cdc_n_write(0, reply, sizeof(reply));
    tud_cdc_n_write_flush(0);
    Check(usbd_edpt_busy(0, 0x84), "bulk reply owns actual busy before fault");
    USB0.dcfg = 0x17;
    USB0.daintmsk = 0x90031;
    USB0.gintmsk = ~0U;
    driverCleanup = true;
    allocatedIrq(nullptr);
    Check(!irqEnabled, "paired cleanup masks captured existing handle");
    fxe::ManualMillis::SetNowMs(1000);
    firmware_internal::PollNativeUsbResetRecovery();
    Check(!connected && irqEnabled, "detach resumes only quarantined original drain");
    USB0.gintsts = USB_RXFLVI_M;
    const auto priorAttach = attaches;
    for (unsigned i = 0; i < 5; ++i) {
        firmware_internal::PollNativeUsbResetRecovery();
        Check(attaches == priorAttach, "pending RX never reconnects");
    }
    drainRx = true;
#ifndef LITCH_USB_OLD_SOURCE
    blockDriver = true;
    std::thread draining([] { allocatedIrq(nullptr); });
    {
        std::unique_lock<std::mutex> lock(blockedMutex);
        Check(blockedCondition.wait_for(lock, std::chrono::seconds(1), [] { return driverEntered; }), "controlled original IRQ starts drain");
    }
    firmware_internal::PollNativeUsbResetRecovery();
    Check(attaches == priorAttach, "task cannot quiesce an admitted original IRQ before it returns");
    {
        std::lock_guard<std::mutex> lock(blockedMutex);
        releaseDriver = true;
    }
    blockedCondition.notify_all();
    draining.join();
    blockDriver = false;
#else
    allocatedIrq(nullptr);
#endif
    firmware_internal::PollNativeUsbResetRecovery();
    const auto calls = driverCalls;
    allocatedIrq(nullptr);
    Check(driverCalls == calls, "pending IRQ cannot call original handler under Quiesced");
    dcd_event_t noop{};
    noop.event_id = USBD_EVENT_FUNC_CALL;
    noop.func_call.func = Noop;
    for (unsigned i = 0; i < 4; ++i)
        __real_dcd_event_handler(&noop, false);
    firmware_internal::PollNativeUsbResetRecovery();
    Check(refused == 1 && !tud_mounted() && usbd_edpt_busy(0, 0x84), "full cleanup admission retains actual busy despite immediate not-mounted");
    Drain();
    firmware_internal::PollNativeUsbResetRecovery();
    for (unsigned i = 0; i < 3; ++i)
        __real_dcd_event_handler(&noop, false);
    firmware_internal::PollNativeUsbResetRecovery();
    Check(refused == 2, "full barrier queue retries separately");
    Drain();
    eager = true;
    firmware_internal::PollNativeUsbResetRecovery();
    eager = false;
    fxe::ManualMillis::SetNowMs(1499);
    firmware_internal::PollNativeUsbResetRecovery();
    Check(attaches == priorAttach, "acknowledged cleanup still preserves dwell");
    fxe::ManualMillis::SetNowMs(1500);
    injectRawAtConnect = true;
    firmware_internal::PollNativeUsbResetRecovery();
    injectRawAtConnect = false;
    Check((USB0.gintsts & USB_USBRST_M) != 0U, "raw reset arriving at internal attach survives software witness clear");
    Check(attaches == priorAttach + 1 && initCalls == 2, "SDK internal connect follows cleanup and dwell; public connect is idempotent");
    Check(!usbd_edpt_busy(0, 0x84) && !tud_mounted() && stops == 1, "actual task resets CDC control and endpoint before attachment");
    dcd_event_bus_reset(0, TUSB_SPEED_FULL, true);
    Setup(0, 9, 1, 0);
    Check(!tud_mounted(), "stale reset and setup cannot grant fresh configuration");
    emitReset = true;
    driverCleanup = false;
    USB0.gintsts = 0x2000;
    allocatedIrq(nullptr);
    Drain();
    Check(!tud_mounted(), "early ENUMDONE without fresh hardware reset remains quarantined");
    for (unsigned i = 0; i < 4; ++i)
        __real_dcd_event_handler(&noop, false);
    USB0.gintsts = USB_USBRST_M;
    allocatedIrq(nullptr);
    Check(refused == 3, "fresh reset admission failure does not grant authority");
    Setup(0, 9, 1, 0);
    Check(!tud_mounted(), "setup stays blocked after dropped normalized reset");
    Drain();
    allocatedIrq(nullptr);
    Drain();
    Setup(0, 9, 1, 0);
    Setup(0x21, 0x22, 1, 0);
    Check(tud_mounted() && tud_cdc_n_connected(0), "accepted fresh reset permits new actual CDC lifetime");
    Check(tud_cdc_n_write(0, reply, sizeof(reply)) == sizeof(reply), "recovered actual console reply accepted");
    Check(tud_cdc_n_write_flush(0) == sizeof(reply), "recovered actual console reply armed");
    dcd_event_xfer_complete(0, 0x84, sizeof(reply), XFER_RESULT_SUCCESS, true);
    Drain();
    Check(!usbd_edpt_busy(0, 0x84), "recovered actual console reply completed");

#ifndef LITCH_USB_OLD_SOURCE
    // Genuine configured USBRST+ENUMDONE can use the same paired-loss path.
    const auto firstBarrier = oldBarrier;
    const auto firstParameter = oldBarrierParameter;
    USB0.dcfg = 0x17;
    USB0.daintmsk = 0x90031;
    USB0.gintsts = USB_USBRST_M | 0x2000U;
    driverCleanup = true;
    emitReset = true;
    allocatedIrq(nullptr);
    Drain();
    Check(!irqEnabled, "configured genuine reset takes documented extra detach");
    fxe::ManualMillis::SetNowMs(2000);
    firmware_internal::PollNativeUsbResetRecovery();
    failDisable = true;
    firmware_internal::PollNativeUsbResetRecovery();
    Check(!connected, "failed IRQ mask cannot reconnect a failed lifetime");
    failDisable = false;
    // A failed mask leaves admission closed; a later poll must retry masking.
    firmware_internal::PollNativeUsbResetRecovery();
    firmware_internal::PollNativeUsbResetRecovery();
    firmware_internal::PollNativeUsbResetRecovery();
    blockStop = true;
    stopEntered = false;
    releaseStop = false;
    std::thread stopped([] { Drain(); });
    {
        std::unique_lock<std::mutex> lock(blockedMutex);
        Check(blockedCondition.wait_for(lock, std::chrono::seconds(1), [] { return stopEntered; }), "actual SDK reaches blocked Arduino STOPPED callback");
    }
    firstBarrier(firstParameter);
    fxe::ManualMillis::SetNowMs(2500);
    const auto beforeSecond = attaches;
    firmware_internal::PollNativeUsbResetRecovery();
    Check(attaches == beforeSecond, "stale acknowledgement and blocked STOPPED cannot reconnect");
    {
        std::lock_guard<std::mutex> lock(blockedMutex);
        releaseStop = true;
    }
    blockedCondition.notify_all();
    stopped.join();
    blockStop = false;
    persistInit = true;
    failEnable = true;
    firmware_internal::PollNativeUsbResetRecovery();
    Check(!connected && !tud_mounted(), "failed IRQ enable stays detached and SDK persistence setup is quarantined");
    failEnable = false;
    firmware_internal::PollNativeUsbResetRecovery();
    persistInit = false;
    Check(connected, "same captured IRQ permits retry after failed reattachment admission");
    driverCleanup = false;
    USB0.gintsts = USB_USBRST_M;
    allocatedIrq(nullptr);
    tusb_control_request_t request{};
    request.bRequest = 9;
    request.wValue = 1;
    dcd_event_setup_received(0, reinterpret_cast<uint8_t*>(&request), true);
    Check(!tud_mounted(), "accepted reset and later setup await existing FIFO task owner");
    Drain();
    Check(tud_mounted() && !usbd_edpt_busy(0, 0x84), "actual FIFO processes fresh reset before subsequent configuration");
#endif
    std::printf("USB_ACTUAL_SDK_RECOVERY failures=%u irq=%u init=%u attach=%u stop=%u refused=%u capacity=%u\n", failures, driverCalls, initCalls, attaches, stops, refused, tud_cdc_n_write_available(0));
    return failures ? 1 : 0;
}
