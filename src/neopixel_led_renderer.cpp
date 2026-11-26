#include "hardware/neopixel_led_renderer.hpp"

#ifdef ARDUINO

#include <Arduino.h>

#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>

#include <driver/rmt.h>

#include "foxie_embedded/elapsed_time.hpp"
#include "logger.hpp"
#include "modules/rgbw_auto_map.hpp"

namespace hardware {

namespace {

portMUX_TYPE g_neopixelShowMux = portMUX_INITIALIZER_UNLOCKED;

constexpr uint32_t CAN_SHOW_TIMEOUT_MS = 100U;
constexpr uint32_t RENDER_ACQUIRE_TIMEOUT_MS = 100U;

#if defined(LITCH_ESP32S3)
class RmtTransferSchedulingGuard final {
  public:
    RmtTransferSchedulingGuard() { vTaskSuspendAll(); }
    ~RmtTransferSchedulingGuard() { (void)xTaskResumeAll(); }
    RmtTransferSchedulingGuard(const RmtTransferSchedulingGuard&) = delete;
    RmtTransferSchedulingGuard& operator=(const RmtTransferSchedulingGuard&) = delete;
};
#endif

class RmtRenderLease final {
  public:
    explicit RmtRenderLease(RmtFlashArbiter* arbiter)
        : m_arbiter(arbiter), m_acquired(arbiter == nullptr) {
        if (m_arbiter == nullptr) return;
        // This complete frame is ready now. Queue its physical transfer,
        // yielding while the current primitive/transfer finishes. The request
        // never survives this bounded acquisition or asks for a future frame.
        const TickType_t started = xTaskGetTickCount();
        m_arbiter->RequestRender();
        while (!m_arbiter->TryBeginRequestedRender()) {
            if (static_cast<TickType_t>(xTaskGetTickCount() - started) >=
                pdMS_TO_TICKS(RENDER_ACQUIRE_TIMEOUT_MS)) {
                m_arbiter->CancelRenderRequest();
                return;
            }
            vTaskDelay(1U);
        }
        m_acquired = true;
    }

    ~RmtRenderLease() {
        if (m_arbiter != nullptr && m_acquired) {
            m_arbiter->EndRender(m_completed);
        }
    }

    bool IsAcquired() const { return m_acquired; }
    void MarkCompleted() { m_completed = true; }

  private:
    RmtFlashArbiter* m_arbiter = nullptr;
    bool m_acquired = false;
    bool m_completed = false;
};

#if defined(LITCH_HW_VARIANT_S3)
modules::RgbwChannels MakePixelColorMapped(uint8_t r, uint8_t g, uint8_t b,
                                          const uint8_t coolToWarmThreshold,
                                          const uint16_t whiteMixScale256) {
    return modules::AutoMapRgbToRgbw(
        modules::Rgb{r, g, b}, coolToWarmThreshold, whiteMixScale256);
}
#else
using PixelColor = RgbColor;

PixelColor MakePixelColor(const uint8_t r, const uint8_t g, const uint8_t b) {
    return PixelColor(r, g, b);
}

PixelColor MakeBlack() {
    return PixelColor(0, 0, 0);
}
#endif

template <typename TStrip>
bool WaitCanShowTrueBounded(TStrip& strip, const uint32_t timeoutMs) {
    const fxe::ElapsedTime elapsed = fxe::ElapsedTime::StartNow();
    uint32_t spins = 0;
    while (!strip.CanShow()) {
        if (elapsed.HasElapsed(timeoutMs)) {
            return false;
        }
        // Keep the poll interval short, but yield often enough that we don't
        // starve other work (e.g. think task, BLE) during the ~7.7ms RMT
        // transfer window.  vTaskDelay(1) yields to ALL ready tasks regardless
        // of priority, unlike delay(0) which only yields to equal-or-higher.
        delayMicroseconds(50);
        if ((++spins & 0x7U) == 0U) {
            vTaskDelay(1);
        }
    }
    return true;
}

template <typename TStrip>
bool SubmitStrip(TStrip& strip, const bool unchanged) {
    strip.Show();
    if (unchanged) return true;
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    // NoIntr bitbang returns after the last data bit. Its 300 us latch may
    // already have elapsed if this task was preempted on return.
    return true;
#else
    // RMT starts asynchronously. Confirm the busy edge before waiting for idle.
    for (uint16_t i = 0; i < 2000; ++i) {
        if (!strip.CanShow()) {
            return true;
        }
    }
    return false;
#endif
}

} // namespace

NeoPixelLedRenderer::NeoPixelLedRenderer(const uint16_t ledCount, const uint8_t pin)
    : NeoPixelLedRenderer(ledCount, pin, 0, pin, false) {}

NeoPixelLedRenderer::NeoPixelLedRenderer(const uint16_t boardLedCount,
                                         const uint8_t boardPin,
                                         const uint16_t edgeLedCount,
                                         const uint8_t edgePin,
                                         const bool rgbOnly)
    : m_boardLedCount(boardLedCount),
      m_edgeLedCount(0),
      m_physicalBoardLedCount(static_cast<uint16_t>(boardLedCount + ((edgeLedCount > 0 && edgePin == boardPin) ? edgeLedCount : 0))),
      m_boardPin(boardPin),
      m_edgePin(edgePin),
      m_splitStrips(edgeLedCount > 0 && edgePin != boardPin),
      m_totalLedCount(static_cast<uint16_t>(m_boardLedCount + (m_splitStrips ? edgeLedCount : 0))),
      m_rgbOnly(rgbOnly),
      // IMPORTANT: if board+edge are wired to the same pin, keep the logical LED count board-only
      // (edge/status are virtual on squares), but still clear any physically-present pixels beyond 192.
      m_boardStrip(m_physicalBoardLedCount, boardPin
#if defined(LITCH_HW_VARIANT_S3)
                   , rgbOnly
#endif
                   ),
      m_edgeStrip(m_splitStrips ? edgeLedCount : 0, edgePin
#if defined(LITCH_HW_VARIANT_S3)
                  , rgbOnly
#endif
                  ) {
    if (m_splitStrips) {
        m_edgeLedCount = edgeLedCount;
    }

    if (!m_splitStrips) {
        m_totalLedCount = m_boardLedCount;
    }
}

void NeoPixelLedRenderer::Begin() {
    m_boardStrip.Begin();
#if defined(LITCH_HW_VARIANT_S3)
    m_boardStrip.ClearToBlack();
#else
    m_boardStrip.ClearTo(MakeBlack());
#endif
    if (m_splitStrips) {
        m_edgeStrip.Begin();
#if defined(LITCH_HW_VARIANT_S3)
        m_edgeStrip.ClearToBlack();
#else
        m_edgeStrip.ClearTo(MakeBlack());
#endif
    }
    // Boot clearing is a real transmission and follows the same interrupt,
    // flash exclusion and completion policy as every later frame.
    Show();
}

void NeoPixelLedRenderer::Clear() {
    portENTER_CRITICAL(&g_neopixelShowMux);
#if defined(LITCH_HW_VARIANT_S3)
    m_boardStrip.ClearToBlack();
#else
    m_boardStrip.ClearTo(MakeBlack());
#endif
    if (m_splitStrips) {
#if defined(LITCH_HW_VARIANT_S3)
        m_edgeStrip.ClearToBlack();
#else
        m_edgeStrip.ClearTo(MakeBlack());
#endif
    }
    portEXIT_CRITICAL(&g_neopixelShowMux);
}

#if defined(LITCH_HW_VARIANT_S3)
void NeoPixelLedRenderer::SetRgbwCoolToWarmThreshold(const uint8_t threshold) {
    m_rgbwCoolToWarmThreshold = threshold;
}

void NeoPixelLedRenderer::SetLedRgbwRaw(const uint16_t ledIndex,
                                        const uint8_t r,
                                        const uint8_t g,
                                        const uint8_t b,
                                        const uint8_t w) {
    if (ledIndex >= m_totalLedCount) {
        return;
    }

    portENTER_CRITICAL(&g_neopixelShowMux);
    if (m_splitStrips) {
        if (ledIndex < m_boardLedCount) {
            m_boardStrip.SetPixelColor(ledIndex, r, g, b, w);
            portEXIT_CRITICAL(&g_neopixelShowMux);
            return;
        }

        const uint16_t edgeIndex = static_cast<uint16_t>(ledIndex - m_boardLedCount);
        if (edgeIndex < m_edgeLedCount) {
            m_edgeStrip.SetPixelColor(edgeIndex, r, g, b, w);
        }
        portEXIT_CRITICAL(&g_neopixelShowMux);
        return;
    }

    m_boardStrip.SetPixelColor(ledIndex, r, g, b, w);
    portEXIT_CRITICAL(&g_neopixelShowMux);
}
#else
void NeoPixelLedRenderer::SetRgbwCoolToWarmThreshold(const uint8_t) {}

void NeoPixelLedRenderer::SetLedRgbwRaw(const uint16_t,
                                        const uint8_t,
                                        const uint8_t,
                                        const uint8_t,
                                        const uint8_t) {}
#endif

void NeoPixelLedRenderer::SetLedRgb(const uint16_t ledIndex, const uint8_t r, const uint8_t g, const uint8_t b) {
    if (ledIndex >= m_totalLedCount) {
        return;
    }

    portENTER_CRITICAL(&g_neopixelShowMux);
#if defined(LITCH_HW_VARIANT_S3)
    const auto mapped = m_rgbOnly
        ? modules::RgbwChannels{r, g, b, 0U}
        : MakePixelColorMapped(r, g, b, m_rgbwCoolToWarmThreshold,
                               m_rgbwWhiteMixScale256);
#endif
    if (m_splitStrips) {
        if (ledIndex < m_boardLedCount) {
#if defined(LITCH_HW_VARIANT_S3)
            m_boardStrip.SetPixelColor(
                ledIndex, mapped.r, mapped.g, mapped.b, mapped.w);
#else
            m_boardStrip.SetPixelColor(ledIndex, MakePixelColor(r, g, b));
#endif
            portEXIT_CRITICAL(&g_neopixelShowMux);
            return;
        }

        const uint16_t edgeIndex = static_cast<uint16_t>(ledIndex - m_boardLedCount);
        if (edgeIndex < m_edgeLedCount) {
#if defined(LITCH_HW_VARIANT_S3)
            m_edgeStrip.SetPixelColor(
                edgeIndex, mapped.r, mapped.g, mapped.b, mapped.w);
#else
            m_edgeStrip.SetPixelColor(edgeIndex, MakePixelColor(r, g, b));
#endif
        }
        portEXIT_CRITICAL(&g_neopixelShowMux);
        return;
    }

#if defined(LITCH_HW_VARIANT_S3)
    m_boardStrip.SetPixelColor(
        ledIndex, mapped.r, mapped.g, mapped.b, mapped.w);
#else
    m_boardStrip.SetPixelColor(ledIndex, MakePixelColor(r, g, b));
#endif
    portEXIT_CRITICAL(&g_neopixelShowMux);
}

void NeoPixelLedRenderer::RestoreOutputPinRouting() {
#if defined(ARDUINO_ARCH_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32C3)
    // Only RMT needs reconnecting after the power sequencer drives the pins
    // as plain GPIO. The C3 bitbang driver uses that GPIO route directly.
    if (!IsOutputEnabled()) {
        return;
    }
    if (rmt_set_pin(RMT_CHANNEL_0, RMT_MODE_TX, static_cast<gpio_num_t>(m_boardPin)) != ESP_OK) {
        logger::Error("led: failed to restore board RMT pin routing");
    }

    if (m_splitStrips &&
        rmt_set_pin(RMT_CHANNEL_1, RMT_MODE_TX, static_cast<gpio_num_t>(m_edgePin)) != ESP_OK) {
        logger::Error("led: failed to restore edge RMT pin routing");
    }
#endif
}

void NeoPixelLedRenderer::LogShowFailure(const char* reason) {
    const uint32_t nowMs = fxe::Millis();
    if (!m_failureLogged || nowMs - m_lastFailureLogMs >= 1000U) {
        logger::Warn("led: %s", reason);
        m_failureLogged = true;
        m_lastFailureLogMs = nowMs;
    }
}

void NeoPixelLedRenderer::Show() {
    m_lastShowCompleted = false;
    // Every producer shares this last submission boundary, including raw
    // diagnostics and boot clearing. Off is neither denial nor confirmation.
    if (!IsOutputEnabled()) {
        return;
    }
    m_deliveryTelemetry.OnAttempt(fxe::Millis());
    RmtRenderLease renderLease(m_rmtFlashArbiter);
    if (!renderLease.IsAcquired()) {
        m_deliveryTelemetry.OnOutcome(LedDeliveryOutcome::FlashDenied, fxe::Millis());
        return;
    }

    // Do not overlap Show calls; wait until the bus is ready.
    if (!WaitCanShowTrueBounded(m_boardStrip, CAN_SHOW_TIMEOUT_MS)) {
        LogShowFailure("timeout waiting CanShow before Show(board)");
        m_deliveryTelemetry.OnOutcome(LedDeliveryOutcome::BusWaitTimedOut, fxe::Millis());
        return;
    }

    if (m_splitStrips && !WaitCanShowTrueBounded(m_edgeStrip, CAN_SHOW_TIMEOUT_MS)) {
        LogShowFailure("timeout waiting CanShow before Show(edge)");
        m_deliveryTelemetry.OnOutcome(LedDeliveryOutcome::BusWaitTimedOut, fxe::Millis());
        return;
    }

    // Completion is now proven for both channels. A clean buffer needs no
    // new start, including after a prior completion check timed out.
    const bool boardUnchanged = !m_boardStrip.IsDirty();
    const bool edgeUnchanged = !m_splitStrips || !m_edgeStrip.IsDirty();

    if (boardUnchanged && edgeUnchanged) {
        m_deliveryTelemetry.OnOutcome(LedDeliveryOutcome::Unchanged, fxe::Millis());
        m_lastShowCompleted = true;
        renderLease.MarkCompleted();
        return;
    }
    // On S3, interrupts must remain enabled while starting the second RMT
    // strip so the first strip can refill its 24-symbol half every 30 us.
    m_deliveryTelemetry.OnSubmission(fxe::Millis());
    const bool boardStarted = SubmitStrip(m_boardStrip, boardUnchanged);
    const bool edgeStarted = !m_splitStrips || SubmitStrip(m_edgeStrip, edgeUnchanged);

    // Even a failed start must settle both buses before releasing the lease.
    bool boardCompleted = false;
    bool edgeCompleted = !m_splitStrips;
#if defined(LITCH_ESP32S3)
    {
        // RMT refills need interrupts, but another task on this core can
        // mask them longer than one refill interval. Suspend only this core's
        // scheduling during TX; the think task on core 0 continues normally.
        // Show, leases, task locks and logging stay outside this scope.
        RmtTransferSchedulingGuard schedulingGuard;
        const fxe::ElapsedTime elapsed = fxe::ElapsedTime::StartNow();
        while (true) {
            boardCompleted = m_boardStrip.CanShow();
            edgeCompleted = !m_splitStrips || m_edgeStrip.CanShow();
            if ((boardCompleted && edgeCompleted) ||
                elapsed.HasElapsed(CAN_SHOW_TIMEOUT_MS)) break;
            delayMicroseconds(50U);
        }
    }
#else
    boardCompleted = WaitCanShowTrueBounded(m_boardStrip, CAN_SHOW_TIMEOUT_MS);
    edgeCompleted = !m_splitStrips ||
        WaitCanShowTrueBounded(m_edgeStrip, CAN_SHOW_TIMEOUT_MS);
#endif
    if (!boardStarted || !edgeStarted || !boardCompleted || !edgeCompleted) {
        LogShowFailure("transfer start or completion unconfirmed");
        m_deliveryTelemetry.OnOutcome(LedDeliveryOutcome::Unconfirmed, fxe::Millis());
        return;
    }
    m_deliveryTelemetry.OnOutcome(LedDeliveryOutcome::Confirmed, fxe::Millis());
    m_lastShowCompleted = true;
    renderLease.MarkCompleted();
}

} // namespace hardware

#else

namespace hardware {

NeoPixelLedRenderer::NeoPixelLedRenderer(const uint16_t ledCount, const uint8_t pin)
    : NeoPixelLedRenderer(ledCount, pin, 0, pin, false) {}

NeoPixelLedRenderer::NeoPixelLedRenderer(const uint16_t boardLedCount,
                                         const uint8_t boardPin,
                                         const uint16_t edgeLedCount,
                                         const uint8_t edgePin,
                                         const bool rgbOnly)
    : m_boardLedCount(boardLedCount),
      m_edgeLedCount((edgeLedCount > 0 && edgePin != boardPin) ? edgeLedCount : 0),
      m_boardPin(boardPin),
      m_edgePin(edgePin),
      m_splitStrips(m_edgeLedCount > 0),
      m_totalLedCount(static_cast<uint16_t>(m_boardLedCount + (m_splitStrips ? m_edgeLedCount : 0))),
      m_rgbOnly(rgbOnly) {}

void NeoPixelLedRenderer::Begin() {}

void NeoPixelLedRenderer::Clear() {}

void NeoPixelLedRenderer::SetLedRgb(const uint16_t, const uint8_t, const uint8_t, const uint8_t) {}

void NeoPixelLedRenderer::SetRgbwCoolToWarmThreshold(const uint8_t) {}

void NeoPixelLedRenderer::SetLedRgbwRaw(const uint16_t,
                                        const uint8_t,
                                        const uint8_t,
                                        const uint8_t,
                                        const uint8_t) {}

void NeoPixelLedRenderer::Show() {}

} // namespace hardware

#endif
