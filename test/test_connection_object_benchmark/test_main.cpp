#include <array>
#include <cstdio>
#include <cstring>

#include "modules/connection_object_benchmark.hpp"

namespace {
using Bench = modules::ConnectionObjectBenchmark;
constexpr Bench::Lifetime LIVE{7U, 11U, 13U, 17U};
int failures = 0;
void Check(const bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}

void FullForwardAndReverse(const size_t body) {
    Bench forward;
    Bench reverse;
    Check(forward.Begin(LIVE, Bench::Direction::Forward, 1U, 300000U, 0x5a17U),
          "forward starts only within the exact current lifetime");
    Check(reverse.Begin(LIVE, Bench::Direction::Reverse, 2U, 300000U, 0x5a17U),
          "reverse starts without a full-body allocation");
    std::array<uint8_t, 502U> scratch{};
    uint32_t received = 0U;
    while (reverse.IsReverse()) {
        size_t size = 0U;
        Check(reverse.PeekReverse(LIVE, scratch.data(), body, size), "peek");
        if (size == 0U) break;
        if (scratch[0U] == Bench::DATA_TAG) {
            const size_t count = size - Bench::DATA_HEADER_BYTES;
            for (size_t i = 0U; i < count; ++i)
                Check(scratch[Bench::DATA_HEADER_BYTES + i] ==
                      Bench::Pattern(received + i, 0x5a17U), "every payload byte checked");
            Check(forward.ConsumeForward(LIVE, scratch.data() + 3U, count), "sink accepts exact pattern");
            received += count;
        } else {
            Check(size == 10U && fxe::ReadU16LE(scratch.data() + 1U) == 2U,
                  "final proof fits the minimum ATT20 body and names this run");
            Check((uint32_t(scratch[3U]) | uint32_t(scratch[4U]) << 8U |
                   uint32_t(scratch[5U]) << 16U) == 300000U, "end counts unique payload bytes");
            Check(fxe::ReadU32LE(scratch.data() + 6U) == forward.Crc(), "full CRC agrees");
        }
        Check(reverse.AcceptReverse(LIVE, scratch.data(), size), "only copied ownership advances source");
    }
    Check(received == 300000U && reverse.Finished(), "full 300000-byte producer completed");
    Check(reverse.Crc() == 0x8c064b0bU, "independent Python zlib full-pattern vector");
    Check(Bench::Pattern(0U, 0x5a17U) == 0x17U &&
          Bench::Pattern(1U, 0x5a17U) == 0x38U &&
          Bench::Pattern(256U, 0x5a17U) == 0x18U, "independent pattern byte vectors");
    Check(forward.FinishForward(LIVE, reverse.Crc()), "forward full count and CRC verified");
}

void RefusalCancellationAndLifetime() {
    Bench source;
    Check(source.Begin(LIVE, Bench::Direction::Reverse, 9U, 30U, 12U), "start bounded run");
    std::array<uint8_t, 10U> first{}, retry{};
    size_t a = 0U, b = 0U;
    Check(source.PeekReverse(LIVE, first.data(), first.size(), a), "first peek");
    Check(source.PeekReverse(LIVE, retry.data(), retry.size(), b) && first == retry &&
          a == b && source.Count() == 0U && source.Crc() == 0U,
          "full shared window/refused publish cannot advance source or invent completion");
    source.Cancel();
    Check(!source.AcceptReverse(LIVE, first.data(), a) &&
          !source.Begin(LIVE, Bench::Direction::Reverse, 9U, 30U, 12U),
          "cancel fences borrowed work and duplicate run cannot restart");
    Check(source.Begin(LIVE, Bench::Direction::Reverse, 10U, 30U, 12U), "new explicit run only");
    const Bench::Lifetime changed{7U, 11U, 13U, 19U};
    Check(!source.PeekReverse(changed, first.data(), first.size(), a), "changed reverse token refuses");
    source.Cancel();
    Check(!source.Begin(LIVE, Bench::Direction::Reverse, 9U, 30U, 12U),
          "earlier run cannot be reused after a later accepted run");
    source.EndLifetime();
    Check(!source.IsActive() && !source.Finished(), "session close/disconnect clears diagnostics");
    Check(!source.Begin(LIVE, Bench::Direction::Reverse, 0U, 30U, 12U) &&
          !source.Begin(LIVE, Bench::Direction::Reverse, 10U, 300001U, 12U),
          "run and size limits fail closed");
}

void PatternErrorIsSticky() {
    Bench sink;
    Check(sink.Begin(LIVE, Bench::Direction::Forward, 1U, 3U, 2U), "pattern run starts");
    uint8_t bytes[3U] = {Bench::Pattern(0U, 2U), Bench::Pattern(1U, 2U), Bench::Pattern(2U, 2U)};
    bytes[1U] ^= 1U;
    Check(!sink.ConsumeForward(LIVE, bytes, sizeof(bytes)) && sink.Count() == 0U,
          "one corrupt byte rejects whole chunk without advancing count");
    bytes[1U] ^= 1U;
    Check(!sink.ConsumeForward(LIVE, bytes, sizeof(bytes)) &&
          !sink.FinishForward(LIVE, fxe::Crc32(bytes, sizeof(bytes))),
          "pattern error is sticky and CRC cannot invent success");
}
} // namespace

int main() {
    FullForwardAndReverse(10U);
    FullForwardAndReverse(234U);
    FullForwardAndReverse(502U);
    RefusalCancellationAndLifetime();
    PatternErrorIsSticky();
    return failures == 0 ? 0 : 1;
}
