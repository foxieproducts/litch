#include <cassert>
#include "hardware/neopixel_led_renderer.hpp"
#include "rmt_fixture.hpp"

namespace fxe { uint32_t Millis() { return fixture::nowUs / 1000U; } }

int main() {
    hardware::RmtFlashArbiter arbiter;
    hardware::NeoPixelLedRenderer renderer(192U, 2U, 32U, 2U);
    renderer.SetRmtFlashArbiter(&arbiter);
    fixture::arbiter = &arbiter;

    renderer.Begin();
    assert(renderer.LastShowCompleted() && fixture::bitBangFrames == 1U);
    assert(fixture::nowUs >= 224U * 30U + 300U);
    assert(renderer.GetLedCount() == 192U);

    // Preemption immediately after sending can consume the entire latch.
    fixture::bitBangReturnDelayUs = 500U;
    renderer.SetLedRgb(191U, 17U, 251U, 3U);
    renderer.SetLedRgb(192U, 1U, 2U, 3U);
    assert(fixture::pixelWrites.size() == 1U);
    const auto& last = fixture::pixelWrites.back();
    assert(last.pin == 2U && last.count == 224U && last.index == 191U);
    assert(last.featureChannels == 3U && last.r == 17U && last.g == 251U && last.b == 3U);
    renderer.Show();
    assert(renderer.LastShowCompleted() && renderer.GetDeliverySnapshot().confirmations == 2U);
    renderer.Show();
    assert(renderer.LastShowCompleted() && fixture::bitBangFrames == 2U);

    assert(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()));
    renderer.Clear();
    renderer.Show();
    assert(!renderer.LastShowCompleted() && fixture::bitBangFrames == 2U);
    arbiter.EndFlash();

    fixture::timeoutAfterSubmission = true;
    renderer.Show();
    assert(!renderer.LastShowCompleted() && fixture::bitBangFrames == 3U);
    assert(!arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()));
    fixture::forceTimeout = false;
    renderer.Show();
    assert(renderer.LastShowCompleted() && fixture::bitBangFrames == 3U);
    assert(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()));
    arbiter.EndFlash();
    assert(fixture::flashOverlaps == 0U);

    renderer.RestoreOutputPinRouting();
    for (const auto& event : fixture::events) {
        assert(event.kind != fixture::EventKind::Route);
    }
}
