#include "firmware/native_usb_reset_recovery.hpp"
#include <cassert>
#include <cstdint>

#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3) && \
    defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
#include <device/dcd.h>
#include <esp_attr.h>
#include <esp_intr_alloc.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <soc/usb_reg.h>
#include <soc/usb_struct.h>
#include <tusb.h>
#include <atomic>
#include "foxie_embedded/millis.hpp"

namespace {
constexpr uint32_t NATIVE_USB_HOST_RECONNECT_DELAY_MS = 500U;
enum class IrqState : uint32_t { Healthy,
                                 Held,
                                 Drain,
                                 Draining,
                                 Quiesced,
                                 FreshReset };
enum class TaskStage { Idle,
                       Drain,
                       Unplug,
                       Barrier,
                       Acknowledge,
                       Reconnect };
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<void*>::is_always_lock_free);
DRAM_ATTR std::atomic<IrqState> g_irqState{IrqState::Healthy};
DRAM_ATTR std::atomic<void*> g_irqHandle{nullptr};
DRAM_ATTR std::atomic<uint32_t> g_generation{0U};
DRAM_ATTR std::atomic<uint32_t> g_acknowledged{0U};
DRAM_ATTR std::atomic<uint32_t> g_freshHardwareReset{0U};
DRAM_ATTR std::atomic<uint32_t> g_explicitConnect{0U};
static_assert(std::atomic<IrqState>::is_always_lock_free);
DRAM_ATTR std::atomic<const void*> g_taskSubmission{nullptr};
DRAM_ATTR std::atomic<uint32_t> g_taskAccepted{0U};
DRAM_ATTR std::atomic<const void*> g_isrSubmission{nullptr};
DRAM_ATTR std::atomic<uint32_t> g_isrAccepted{0U};
TaskStage g_stage = TaskStage::Idle;
bool g_warmRecoveryPending = false;
uint32_t g_detachedAtMs = 0U;
uint32_t g_taskGeneration = 0U;

intr_handle_t IRAM_ATTR Handle() {
    return static_cast<intr_handle_t>(g_irqHandle.load(std::memory_order_acquire));
}
void CleanupAcknowledged(void* value) {
    const auto generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value));
    if (generation == g_generation.load(std::memory_order_acquire)) {
        g_acknowledged.store(generation, std::memory_order_release);
    }
}
bool Submit(dcd_event_t& event) {
    g_taskAccepted.store(0U, std::memory_order_relaxed);
    g_taskSubmission.store(&event, std::memory_order_release);
    dcd_event_handler(&event, false);
    g_taskSubmission.store(nullptr, std::memory_order_release);
    return g_taskAccepted.load(std::memory_order_acquire) != 0U;
}
} // namespace

extern "C" BaseType_t __real_xQueueGenericSend(QueueHandle_t, const void*, TickType_t, BaseType_t);
extern "C" BaseType_t __wrap_xQueueGenericSend(QueueHandle_t queue, const void* item, TickType_t wait, BaseType_t position) {
    const auto* submission = g_taskSubmission.load(std::memory_order_acquire);
    const bool recovery = submission != nullptr && item == submission;
    const auto result = __real_xQueueGenericSend(queue, item, recovery ? 0U : wait, position);
    if (recovery)
        g_taskAccepted.store(result != pdFALSE ? 1U : 0U, std::memory_order_release);
    return result;
}
extern "C" BaseType_t __real_xQueueGenericSendFromISR(QueueHandle_t, const void*, BaseType_t*, BaseType_t);
extern "C" BaseType_t IRAM_ATTR __wrap_xQueueGenericSendFromISR(QueueHandle_t queue, const void* item, BaseType_t* wake, BaseType_t position) {
    const auto result = __real_xQueueGenericSendFromISR(queue, item, wake, position);
    const auto* submission = g_isrSubmission.load(std::memory_order_acquire);
    if (submission != nullptr && item == submission) {
        g_isrAccepted.store(result != pdFALSE ? 1U : 0U, std::memory_order_release);
    }
    return result;
}
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif

namespace firmware_internal {

void CaptureNativeUsbDcdInterrupt(void* handle) {
#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3) && defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
    g_irqHandle.store(handle, std::memory_order_release);
#else
    (void)handle;
#endif
}
bool IRAM_ATTR NativeUsbDcdInterruptBegin(uint32_t raw, uint32_t mask) {
#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3) && defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
    auto state = g_irqState.load(std::memory_order_acquire);
    if (state == IrqState::Drain) {
        return g_irqState.compare_exchange_strong(state, IrqState::Draining, std::memory_order_acq_rel);
    }
    if (state == IrqState::FreshReset) {
        if ((raw & mask & (USB_USBRST_M | USB_RESETDET_M)) != 0U) {
            g_freshHardwareReset.store(1U, std::memory_order_release);
        }
        return true;
    }
    return state == IrqState::Healthy;
#else
    (void)raw;
    (void)mask;
    return true;
#endif
}
void IRAM_ATTR NativeUsbDcdInterruptEnd(bool configuredLoss) {
#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3) && defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
    auto state = g_irqState.load(std::memory_order_acquire);
    if (state == IrqState::Draining) {
        g_irqState.store(IrqState::Drain, std::memory_order_release);
    } else if (state == IrqState::Healthy && configuredLoss && Handle()) {
        g_generation.fetch_add(1U, std::memory_order_relaxed);
        g_irqState.store(IrqState::Held, std::memory_order_release);
        (void)esp_intr_disable(Handle());
    }
#else
    (void)configuredLoss;
#endif
}
bool NativeUsbRecoveryBlocksConnect() {
#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3) && defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
    const auto state = g_irqState.load(std::memory_order_acquire);
    return state != IrqState::Healthy && state != IrqState::FreshReset &&
           g_explicitConnect.load(std::memory_order_acquire) == 0U;
#else
    return false;
#endif
}
bool IRAM_ATTR BeforeNativeUsbEvent(const void* value, bool inIsr) {
#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3) && defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
    if (value == g_taskSubmission.load(std::memory_order_acquire))
        return true;
    const auto state = g_irqState.load(std::memory_order_acquire);
    if (state == IrqState::Healthy)
        return true;
    const auto* event = static_cast<const dcd_event_t*>(value);
    if (state == IrqState::FreshReset && inIsr && event->event_id == DCD_EVENT_BUS_RESET &&
        g_freshHardwareReset.load(std::memory_order_acquire) != 0U) {
        g_isrAccepted.store(0U, std::memory_order_relaxed);
        g_isrSubmission.store(value, std::memory_order_release);
        return true;
    }
    return false;
#else
    (void)value;
    (void)inIsr;
    return true;
#endif
}
void IRAM_ATTR AfterNativeUsbEvent(const void* value) {
#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3) && defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
    if (value == g_isrSubmission.load(std::memory_order_acquire)) {
        g_isrSubmission.store(nullptr, std::memory_order_release);
        if (g_isrAccepted.load(std::memory_order_acquire) != 0U) {
            g_irqState.store(IrqState::Healthy, std::memory_order_release);
        }
    }
#else
    (void)value;
#endif
}
void BeginNativeUsbResetRecovery() {
#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3) && defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
    const auto reason = esp_reset_reason();
    if (reason != ESP_RST_SW && reason != ESP_RST_DEEPSLEEP)
        return;
    (void)tud_disconnect();
    g_detachedAtMs = fxe::Millis();
    g_warmRecoveryPending = true;
#endif
}
void PollNativeUsbResetRecovery() {
#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3) && defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
    if (g_stage == TaskStage::Idle && g_irqState.load(std::memory_order_acquire) == IrqState::Held) {
        (void)tud_disconnect();
        g_detachedAtMs = fxe::Millis();
        g_warmRecoveryPending = false;
        g_taskGeneration = g_generation.load(std::memory_order_acquire);
        g_stage = TaskStage::Drain;
        g_irqState.store(IrqState::Drain, std::memory_order_release);
        (void)esp_intr_enable(Handle());
        return;
    }
    if (g_stage == TaskStage::Drain) {
        auto expected = g_irqState.load(std::memory_order_acquire);
        if (expected == IrqState::Drain) {
            if (!g_irqState.compare_exchange_strong(expected, IrqState::Quiesced, std::memory_order_acq_rel))
                return;
        } else if (expected != IrqState::Quiesced) {
            return;
        }
        if (esp_intr_disable(Handle()) != ESP_OK)
            return;
        // Soft-detached producers cannot refill RX. Pending IRQs cannot enter
        // the original handler after the admission CAS. A stranded RX source
        // remains detached; each application poll does only this one check.
        if ((USB0.gintsts & USB_RXFLVI_M) != 0U) {
            g_irqState.store(IrqState::Drain, std::memory_order_release);
            (void)esp_intr_enable(Handle());
            return;
        }
        g_stage = TaskStage::Unplug;
        return;
    }
    if (g_stage == TaskStage::Unplug || g_stage == TaskStage::Barrier) {
        dcd_event_t event{};
        event.rhport = 0U;
        if (g_stage == TaskStage::Unplug)
            event.event_id = DCD_EVENT_UNPLUGGED;
        else {
            event.event_id = USBD_EVENT_FUNC_CALL;
            event.func_call.func = CleanupAcknowledged;
            event.func_call.param = reinterpret_cast<void*>(static_cast<uintptr_t>(g_taskGeneration));
        }
        if (Submit(event))
            g_stage = g_stage == TaskStage::Unplug ? TaskStage::Barrier : TaskStage::Acknowledge;
        return;
    }
    if (g_stage == TaskStage::Acknowledge) {
        if (g_acknowledged.load(std::memory_order_acquire) != g_taskGeneration ||
            static_cast<uint32_t>(fxe::Millis() - g_detachedAtMs) < NATIVE_USB_HOST_RECONNECT_DELAY_MS)
            return;
        // This SDK owner clears pending GINT without reallocating its IRQ.
        // Its normal branch attaches internally after cleanup and the dwell.
        // The same-TU SDK connect bypasses the linker wrapper; IRQ admission
        // stays closed, and persistence events remain quarantined.
        dcd_init(0U);
        g_freshHardwareReset.store(0U, std::memory_order_release);
        g_stage = TaskStage::Reconnect;
    }
    if (g_stage == TaskStage::Reconnect) {
        // Idempotent after normal SDK init, required after persistence init.
        // IRQ admission stays closed. A reset observed after
        // enabling the same handle therefore belongs to this new attachment.
        g_explicitConnect.store(1U, std::memory_order_release);
        const bool connected = tud_connect();
        g_explicitConnect.store(0U, std::memory_order_release);
        if (!connected)
            return;
        g_irqState.store(IrqState::FreshReset, std::memory_order_release);
        if (esp_intr_enable(Handle()) != ESP_OK) {
            g_irqState.store(IrqState::Quiesced, std::memory_order_release);
            (void)tud_disconnect();
            return;
        }
        g_stage = TaskStage::Idle;
        return;
    }
    if (g_warmRecoveryPending && static_cast<uint32_t>(fxe::Millis() - g_detachedAtMs) >= NATIVE_USB_HOST_RECONNECT_DELAY_MS) {
        (void)tud_connect();
        g_warmRecoveryPending = false;
    }
#endif
}
} // namespace firmware_internal
