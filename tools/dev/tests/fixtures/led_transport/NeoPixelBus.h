#pragma once
#include <type_traits>
#include <vector>
#include "rmt_fixture.hpp"
struct RgbwColor {
    static constexpr unsigned CHANNELS = 4U;
    unsigned r, g, b, w;
    RgbwColor(unsigned red, unsigned green, unsigned blue, unsigned white)
        : r(red), g(green), b(blue), w(white) {}
};
struct RgbColor {
    static constexpr unsigned CHANNELS = 3U;
    unsigned r, g, b, w = 0U;
    RgbColor(unsigned red, unsigned green, unsigned blue)
        : r(red), g(green), b(blue) {}
};
struct NeoGrbwFeature { static constexpr unsigned CHANNELS = 4U; };
struct NeoGrbFeature { static constexpr unsigned CHANNELS = 3U; };
struct NeoEsp32Rmt0Sk6812Method { static constexpr unsigned CHANNEL = 0U, RESET_US = 80U; };
struct NeoEsp32Rmt1Sk6812Method { static constexpr unsigned CHANNEL = 1U, RESET_US = 80U; };
struct NeoEsp32Rmt0Ws2812xMethod { static constexpr unsigned CHANNEL = 0U, RESET_US = 300U; };
struct NeoEsp32Rmt1Ws2812xMethod { static constexpr unsigned CHANNEL = 1U, RESET_US = 300U; };
struct NeoEsp32BitBangWs2812xNoIntrMethod { static constexpr unsigned CHANNEL = 0U, RESET_US = 300U; };

namespace fixture {
// Observe the SDK call boundary; this does not simulate wire bytes or optics.
struct PixelWrite {
    unsigned channel, pin, count, index, featureChannels, colorChannels;
    unsigned r, g, b, w;
};
inline std::vector<PixelWrite> pixelWrites;
inline unsigned bitBangFrames = 0U;
inline unsigned bitBangReturnDelayUs = 0U;
}

template <typename Feature, typename Method> class NeoPixelBus {
  public:
    NeoPixelBus(unsigned count, unsigned pin) : m_count(count), m_pin(pin) {}
    void Begin() {}
    template <typename Color> void ClearTo(Color) { m_dirty = true; }
    template <typename Color> void SetPixelColor(unsigned index, Color color) {
        fixture::pixelWrites.push_back({Method::CHANNEL, m_pin, m_count, index,
            Feature::CHANNELS, Color::CHANNELS, color.r, color.g, color.b, color.w});
        m_dirty = true;
    }
    bool IsDirty() const { return m_dirty; }
    bool CanShow() {
        fixture::Advance(1U);
        return !fixture::forceTimeout &&
            fixture::nowUs >= fixture::channels[Method::CHANNEL].endUs;
    }
    void Show() {
        if (!m_dirty) return;
        auto& channel = fixture::channels[Method::CHANNEL];
        if constexpr (std::is_same_v<Method, NeoEsp32BitBangWs2812xNoIntrMethod>) {
            // At 800 kbit/s each byte occupies 10 us. Only the latch is
            // outstanding on return, and a scheduling delay can hide it.
            fixture::Advance(m_count * Feature::CHANNELS * 10U);
            channel = {fixture::nowUs + Method::RESET_US, 0U, false, Method::RESET_US};
            fixture::Advance(fixture::bitBangReturnDelayUs);
            ++fixture::bitBangFrames;
        } else {
            // Stress an SDK submission/buffer copy while a previous channel runs.
            fixture::Advance(fixture::submissionDelayUs);
            channel = {fixture::nowUs + m_count * 40U + Method::RESET_US,
                       0U, false, Method::RESET_US};
        }
        fixture::events.push_back({fixture::EventKind::Submit, m_pin});
        ++fixture::starts;
        m_dirty = false;
        if (fixture::timeoutAfterSubmission) fixture::forceTimeout = true;
    }
  private:
    unsigned m_count;
    unsigned m_pin;
    bool m_dirty = false;
};
