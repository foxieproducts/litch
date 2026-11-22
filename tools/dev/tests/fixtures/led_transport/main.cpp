#include <cstdio>
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/neopixel_led_renderer.hpp"
#include "rmt_fixture.hpp"

namespace fxe { uint32_t Millis() { return fixture::nowUs / 1000U; } }

bool MatchesWrite(unsigned position, unsigned channel, unsigned pin,
                  unsigned count, unsigned index, unsigned channels,
                  unsigned r, unsigned g, unsigned b, unsigned w = 0U) {
    if (position >= fixture::pixelWrites.size()) return false;
    const auto& write = fixture::pixelWrites[position];
    return write.channel == channel && write.pin == pin && write.count == count &&
        write.index == index && write.featureChannels == channels &&
        write.colorChannels == channels && write.r == r && write.g == g &&
        write.b == b && write.w == w;
}

unsigned RailLifecycleChecks();

int main() {
    unsigned failures = 0U;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::printf("FAIL: %s\n", message); ++failures; }
    };
    struct Topology { unsigned board; unsigned status; bool rgbOnly; };
    for (const auto topology : {Topology{192U, 32U, false},
                                Topology{256U, 32U, false},
                                Topology{256U, 44U, true}}) {
        const unsigned count = topology.board;
        hardware::RmtFlashArbiter arbiter;
        fixture::arbiter = &arbiter;
        hardware::NeoPixelLedRenderer renderer(
            count, 47U, topology.status, 33U, topology.rgbOnly);
        renderer.SetRmtFlashArbiter(&arbiter);
        renderer.Begin();
        check(fixture::channels[0].resetUs == (topology.rgbOnly ? 300U : 80U) &&
                  fixture::channels[1].resetUs == (topology.rgbOnly ? 300U : 80U),
              "each LED revision uses its required reset interval");
        check(fixture::flashOverlaps == 0U,
              "boot clearing excludes flash until both LED strips finish");
        for (const unsigned delayUs : {0U, 10U, 29U, 30U, 60U, 200U}) {
            fixture::submissionDelayUs = delayUs;
            fixture::pixelWrites.clear();
            renderer.SetLedRgb(0U, 1U, 2U, 3U);
            renderer.SetLedRgb(count - 1U, 4U, 0U, 6U);
            renderer.SetLedRgb(count, 7U, 8U, 0U);
            renderer.SetLedRgb(count + topology.status - 1U, 0U, 10U, 11U);
            renderer.SetLedRgb(count + topology.status, 91U, 92U, 93U);
            renderer.Show();
            const unsigned channels = topology.rgbOnly ? 3U : 4U;
            check(fixture::pixelWrites.size() == 4U,
                  "one-past-end RGB write never reaches either bus");
            check(MatchesWrite(0U, 0U, 47U, count, 0U, channels, 1U, 2U, 3U) &&
                      MatchesWrite(1U, 0U, 47U, count, count - 1U, channels, 4U, 0U, 6U),
                  "first/last board writes preserve bus, index, feature and color");
            check(MatchesWrite(2U, 1U, 33U, topology.status, 0U, channels, 7U, 8U, 0U) &&
                      MatchesWrite(3U, 1U, 33U, topology.status, topology.status - 1U,
                                   channels, 0U, 10U, 11U),
                  "first/last status writes subtract only the board offset");
            check(renderer.LastShowCompleted(), "both strips complete");
            check(fixture::corruptions == 0U,
                  "RMT refill stays serviceable during dual-strip submission");
            check(fixture::flashOverlaps == 0U,
                  "flash cannot overlap any active RMT channel");
        }
        fixture::pixelWrites.clear();
        renderer.SetLedRgbwRaw(count, 1U, 2U, 3U, 9U);
        renderer.SetLedRgbwRaw(count + topology.status, 1U, 2U, 3U, 9U);
        check(fixture::pixelWrites.size() == 1U &&
                  MatchesWrite(0U, 1U, 33U, topology.status, 0U,
                               topology.rgbOnly ? 3U : 4U, 1U, 2U, 3U,
                               topology.rgbOnly ? 0U : 9U),
              "raw writes preserve RGBW or omit W on RGB and reject out-of-range");
        renderer.Show();
        if (topology.rgbOnly) {
            // PCB LED282/raw25 and LED289/raw32 bracket six bottom buttons.
            // Expected destinations are independent of the mapper under test.
            std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
            status[19U] = {17U, 29U, 43U};
            status[20U] = {61U, 73U, 89U};
            hardware::PhysicalLedFrame physical{};
            hardware::ApplySemanticStatusFrame(
                {hardware::BoardRevision::V3_3S3, false, false}, status, physical);
            fixture::pixelWrites.clear();
            for (unsigned index = 0U; index < physical.size(); ++index) {
                const auto color = physical[index];
                renderer.SetLedRgb(index, color.r, color.g, color.b);
            }
            renderer.Show();
            check(fixture::pixelWrites.size() == 300U &&
                      MatchesWrite(281U, 1U, 33U, 44U, 25U, 3U, 17U, 29U, 43U) &&
                      MatchesWrite(288U, 1U, 33U, 44U, 32U, 3U, 61U, 73U, 89U),
                  "production mapper and Arduino renderer preserve bottom perimeter boundary");
            for (unsigned index = 26U; index <= 31U; ++index) {
                check(MatchesWrite(256U + index, 1U, 33U, 44U, index, 3U, 0U, 0U, 0U),
                      "bottom button pixels stay black between perimeter samples");
            }
        }
        check(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()),
              "flash resumes after acknowledged output");
        renderer.Clear();
        const unsigned before = fixture::starts;
        renderer.Show();
        check(!renderer.LastShowCompleted() && fixture::starts == before,
              "flash defers output without starting either strip");
        arbiter.EndFlash();
        renderer.Show();
        check(renderer.LastShowCompleted(), "deferred output retries");
        renderer.SetLedRgb(0U, 1U, 1U, 1U);
        fixture::forceTimeout = true;
        renderer.Show();
        check(!renderer.LastShowCompleted(), "timeout is not completed output");
        check(!arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()),
              "unconfirmed completion quarantines flash");
        fixture::forceTimeout = false;
        renderer.Show();
        check(renderer.LastShowCompleted(), "timeout recovery completes output");
        renderer.SetLedRgb(0U, 2U, 2U, 2U);
        renderer.SetLedRgb(count, 2U, 2U, 2U);
        const unsigned beforeInFlight = fixture::starts;
        fixture::timeoutAfterSubmission = true;
        renderer.Show();
        check(fixture::starts == beforeInFlight + 2U &&
                  !renderer.LastShowCompleted(),
              "both strips submit before the in-flight completion timeout");
        check(!arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()),
              "in-flight unconfirmed completion quarantines flash");
        fixture::timeoutAfterSubmission = false;
        fixture::forceTimeout = false;
        renderer.Show();
        check(renderer.LastShowCompleted() &&
                  fixture::starts == beforeInFlight + 2U,
              "confirmed idle clean buffers recover without another transmission");
        check(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()),
              "flash resumes after clean-buffer completion recovery");
        arbiter.EndFlash();
        fixture::arbiter = nullptr;
    }
    failures += RailLifecycleChecks();
    return failures == 0U ? 0 : 1;
}
