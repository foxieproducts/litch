#pragma once

#if defined(ARDUINO) && defined(CONFIG_IDF_TARGET_ESP32C3)

#include <NeoPixelBus.h>
#include <soc/soc_caps.h>

namespace hardware {

// The V1 chain is the only RMT transmitter. Borrow channel 1's unused memory
// so the ISR has twice as long to refill each half of the transmit buffer.
// Keep NeoPixelBus 2.8.4's buffers, WS2812x translator, and async Update path.
class C3NeoPixelRmtMethod final : public NeoEsp32Rmt0Ws2812xMethod {
  public:
    C3NeoPixelRmtMethod(uint8_t pin, uint16_t pixelCount, size_t elementSize, size_t settingsSize)
        : NeoEsp32Rmt0Ws2812xMethod(pin, pixelCount, elementSize, settingsSize),
          m_pin(pin) {}

    void Initialize() {
        static_assert(SOC_RMT_TX_CANDIDATES_PER_GROUP >= 2, "C3 needs two TX memory blocks");
        static_assert(LITCH_EDGE_LED_COUNT == 0, "C3 board RMT owns channel 1 memory");

        rmt_config_t config = {};
        config.rmt_mode = RMT_MODE_TX;
        config.channel = RMT_CHANNEL_0;
        config.gpio_num = static_cast<gpio_num_t>(m_pin);
        config.mem_block_num = 2;
        config.tx_config.loop_en = false;
        config.tx_config.idle_output_en = true;
        config.tx_config.idle_level = NeoEsp32RmtSpeedWs2812x::IdleLevel;
        config.tx_config.carrier_en = false;
        config.tx_config.carrier_level = RMT_CARRIER_LEVEL_LOW;
        config.clk_div = NeoEsp32RmtSpeedWs2812x::RmtClockDivider;

        // Configure memory before translator_init allocates its scratch buffer.
        // Increasing it after NeoPixelBus::Begin would leave that buffer too small.
        ESP_ERROR_CHECK(rmt_config(&config));
        ESP_ERROR_CHECK(rmt_driver_install(RMT_CHANNEL_0, 0, NEOPIXELBUS_RMT_INT_FLAGS));
        ESP_ERROR_CHECK(rmt_translator_init(RMT_CHANNEL_0, NeoEsp32RmtSpeedWs2812x::Translate));
    }

  private:
    const uint8_t m_pin;
};

} // namespace hardware

#endif
