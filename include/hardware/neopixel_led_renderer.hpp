#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "hardware/rmt_flash_arbiter.hpp"
#include "hardware/led_delivery_telemetry.hpp"
#include "modules/led_renderer.hpp"

#ifdef ARDUINO
#include <NeoPixelBus.h>
#endif

namespace hardware {

#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3)
template <typename RgbMethod, typename RgbwMethod>
class S3SelectableStrip {
  public:
    S3SelectableStrip(uint16_t count, uint8_t pin, bool rgbOnly) {
        if (rgbOnly) {
            m_rgb.emplace(count, pin);
        } else {
            m_rgbw.emplace(count, pin);
        }
    }

    void Begin() {
        if (m_rgb) m_rgb->Begin(); else m_rgbw->Begin();
    }
    void ClearToBlack() {
        if (m_rgb) m_rgb->ClearTo(RgbColor(0, 0, 0));
        else m_rgbw->ClearTo(RgbwColor(0, 0, 0, 0));
    }
    void SetPixelColor(uint16_t index, uint8_t r, uint8_t g,
                       uint8_t b, uint8_t w) {
        if (m_rgb) m_rgb->SetPixelColor(index, RgbColor(r, g, b));
        else m_rgbw->SetPixelColor(index, RgbwColor(r, g, b, w));
    }
    bool IsDirty() const {
        return m_rgb ? m_rgb->IsDirty() : m_rgbw->IsDirty();
    }
    bool CanShow() {
        return m_rgb ? m_rgb->CanShow() : m_rgbw->CanShow();
    }
    void Show() {
        if (m_rgb) m_rgb->Show(); else m_rgbw->Show();
    }

  private:
    std::optional<NeoPixelBus<NeoGrbFeature, RgbMethod>> m_rgb;
    std::optional<NeoPixelBus<NeoGrbwFeature, RgbwMethod>> m_rgbw;
};
#endif

// Minimal NeoPixelBus-backed ILedRenderer for ESP32 hardware.
class NeoPixelLedRenderer final : public modules::ILedRenderer {
  public:
    NeoPixelLedRenderer(uint16_t ledCount, uint8_t pin);
    NeoPixelLedRenderer(uint16_t boardLedCount, uint8_t boardPin,
                        uint16_t edgeLedCount, uint8_t edgePin,
                        bool rgbOnly = false);

    void Begin();

    void SetRmtFlashArbiter(RmtFlashArbiter* arbiter) {
        m_rmtFlashArbiter = arbiter;
    }

    // The rail owner outlives this renderer; nullptr means an always-on supply.
    void SetPowerEnabledState(const std::atomic<bool>* enabled) { m_powerEnabled = enabled; }
    bool IsOutputEnabled() const {
        return m_powerEnabled == nullptr || m_powerEnabled->load(std::memory_order_acquire);
    }

    uint16_t GetLedCount() const override { return m_totalLedCount; }

    void Clear() override;

    void SetLedRgb(uint16_t ledIndex, uint8_t r, uint8_t g, uint8_t b) override;

    // S3 RGBW behavior:
    // - By default, RGB writes are auto-mapped to RGBW using the W channel.
    // - The cool->warm threshold controls how aggressively we use W for
    //   cooler (blue-biased) whites.
    // - These are no-ops on non-RGBW hardware.
    void SetRgbwCoolToWarmThreshold(uint8_t threshold);
    uint8_t GetRgbwCoolToWarmThreshold() const { return m_rgbwCoolToWarmThreshold; }
    void SetRgbwWhiteMixScale256(uint16_t scale256) {
        m_rgbwWhiteMixScale256 = scale256 > 256U ? 256U : scale256;
    }

    // Direct RGBW write for controlled channel characterization (raw amptest).
    void SetLedRgbwRaw(uint16_t ledIndex, uint8_t r, uint8_t g, uint8_t b, uint8_t w);

    // Restores the RMT/GPIO routing after external code temporarily drives the
    // data pins as plain GPIO during power sequencing.
    void RestoreOutputPinRouting();

    void Show() override;

    // Software transfer evidence only; does not establish optical output.
    bool LastShowCompleted() const { return IsOutputEnabled() && m_lastShowCompleted; }
    LedDeliverySnapshot GetDeliverySnapshot() const {
        return m_deliveryTelemetry.GetSnapshot();
    }

  private:
    static constexpr uint8_t DEFAULT_RGBW_COOL_TO_WARM_THRESHOLD = 32;

    uint16_t m_boardLedCount = 0;
    uint16_t m_edgeLedCount = 0;

    uint16_t m_physicalBoardLedCount = 0;

    uint8_t m_boardPin = 0;
    uint8_t m_edgePin = 0;

    bool m_splitStrips = false;

    uint16_t m_totalLedCount = 0;
    bool m_rgbOnly = false;

    uint8_t m_rgbwCoolToWarmThreshold = DEFAULT_RGBW_COOL_TO_WARM_THRESHOLD;
    uint16_t m_rgbwWhiteMixScale256 = 256U;

    RmtFlashArbiter* m_rmtFlashArbiter = nullptr;
    const std::atomic<bool>* m_powerEnabled = nullptr;
    bool m_lastShowCompleted = false;
    LedDeliveryTelemetry m_deliveryTelemetry;
    bool m_failureLogged = false;
    uint32_t m_lastFailureLogMs = 0U;
    void LogShowFailure(const char* reason);

#ifdef ARDUINO
#if defined(LITCH_HW_VARIANT_S3)
    // RMT0 drives the board and RMT1 drives the separate status strip.
    // V3.3 SIDE-A requires >200 us reset; WS2812x uses the same data pulse
    // widths as SK6812 here and provides a 300 us reset. V2/V3 keep 80 us.
    using BoardStripType = S3SelectableStrip<
        NeoEsp32Rmt0Ws2812xMethod, NeoEsp32Rmt0Sk6812Method>;
    using EdgeStripType = S3SelectableStrip<
        NeoEsp32Rmt1Ws2812xMethod, NeoEsp32Rmt1Sk6812Method>;
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    // V1 has one GRB chain. Send each frame without interrupt gaps between
    // pixels, which can make the WS2812B-4020 latch a partial frame.
    using BoardStripType = NeoPixelBus<NeoGrbFeature, NeoEsp32BitBangWs2812xNoIntrMethod>;
    using EdgeStripType = BoardStripType;
#endif

    BoardStripType m_boardStrip;
    EdgeStripType m_edgeStrip;
#endif
};

} // namespace hardware
