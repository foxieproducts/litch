#include <array>
#include <algorithm>
#include <cstdio>

#include "foxie_embedded/checksum.hpp"
#include "modules/litchcraft_frame_slot_codec.hpp"
#include "modules/litchcraft_wire_codec.hpp"

namespace wire = modules::litchcraft_wire_codec;
namespace store = modules::litchcraft_frame_store;
namespace slot = modules::litchcraft_frame_slot_codec;

namespace {

int failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", message);
}

bool Equal(const store::Frame& left, const store::Frame& right) {
    if (left.durationMs != right.durationMs) return false;
    for (size_t index = 0U; index < left.emitters.size(); ++index) {
        const auto& a = left.emitters[index];
        const auto& b = right.emitters[index];
        if (a.color.r != b.color.r || a.color.g != b.color.g ||
            a.color.b != b.color.b || a.effect != b.effect ||
            a.effectDurationMs != b.effectDurationMs) return false;
    }
    return true;
}

store::Frame BaseFrame() {
    store::Frame frame;
    frame.durationMs = 1000U;
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        frame.emitters[index].color = {
            static_cast<uint8_t>(index),
            static_cast<uint8_t>(index * 3U),
            static_cast<uint8_t>(index * 7U)};
    }
    return frame;
}

void IdenticalFrameUsesThreeBytesAndRoundTrips() {
    const store::Frame previous = BaseFrame();
    store::Frame current = previous;
    current.durationMs = 50U;
    std::array<uint8_t, wire::MAX_ENCODED_FRAME_BYTES> encoded{};
    size_t size = 0U;

    Check(wire::FrameCodec::Encode(&previous, current, encoded.data(),
                                   encoded.size(), size),
          "identical emitters encode");
    Check(size == 3U, "identical emitters use only mode and frame duration");

    store::Frame decoded;
    Check(wire::FrameCodec::Decode(&previous, encoded.data(), size, decoded),
          "identical emitter delta decodes");
    Check(Equal(current, decoded),
          "identical emitter delta preserves exact frame duration and emitters");
}

void OneChangedEmitterUsesSparseDeltaAndRoundTrips() {
    const store::Frame previous = BaseFrame();
    store::Frame current = previous;
    current.durationMs = 5000U;
    current.emitters[173U].color = {0xffU, 0x01U, 0x80U};
    current.emitters[173U].effect = 8U;
    current.emitters[173U].effectDurationMs = 10000U;
    std::array<uint8_t, wire::MAX_ENCODED_FRAME_BYTES> encoded{};
    size_t size = 0U;

    Check(wire::FrameCodec::Encode(&previous, current, encoded.data(),
                                   encoded.size(), size),
          "one changed emitter encodes");
    Check(size == 10U,
          "one changed emitter uses a compact indexed exact value");

    store::Frame decoded;
    Check(wire::FrameCodec::Decode(&previous, encoded.data(), size, decoded),
          "one changed emitter decodes");
    Check(Equal(current, decoded),
          "sparse delta preserves exact RGB, effect, and both durations");
}

void DenseFirstFrameUsesBoundedRawFallbackAndRoundTrips() {
    store::Frame current = BaseFrame();
    current.durationMs = 4950U;
    for (size_t index = 0U; index < current.emitters.size(); ++index) {
        auto& emitter = current.emitters[index];
        emitter.color = {static_cast<uint8_t>(index * 11U + 1U),
                         static_cast<uint8_t>(index * 17U + 3U),
                         static_cast<uint8_t>(index * 23U + 5U)};
        emitter.effect = static_cast<uint8_t>(index % 9U);
        emitter.effectDurationMs = emitter.effect == 0U
            ? 0U : static_cast<uint16_t>(500U + (index % 96U) * 100U);
    }
    std::array<uint8_t, wire::MAX_ENCODED_FRAME_BYTES> encoded{};
    size_t size = 0U;

    Check(wire::FrameCodec::Encode(nullptr, current, encoded.data(),
                                   encoded.size(), size),
          "first frame encodes without a temporal predecessor");
    Check(size == wire::MAX_ENCODED_FRAME_BYTES,
          "adversarial dense frame is bounded by exact raw fallback");
    std::printf("[LITCHCRAFT_WIRE_FIXTURE] raw_crc32=%08lx\n",
                static_cast<unsigned long>(fxe::Crc32(encoded.data(), size)));
    Check(fxe::Crc32(encoded.data(), size) == 0x77821a36U,
          "raw fallback matches the shared app/firmware fixture CRC32");

    store::Frame decoded;
    Check(wire::FrameCodec::Decode(nullptr, encoded.data(), size, decoded),
          "raw fallback decodes without a temporal predecessor");
    Check(Equal(current, decoded),
          "raw fallback preserves every exact emitter and duration");
}

void OptInRgbFramesUseExactBytesAndRoundTrip() {
    const store::Frame first = BaseFrame();
    std::array<uint8_t, wire::MAX_ENCODED_FRAME_BYTES> encoded{};
    size_t size = 0U;
    Check(wire::FrameCodec::Encode(nullptr, first, encoded.data(),
                                   encoded.size(), size,
                                   wire::RgbOnlyMode::Enabled),
          "opt-in RGB raw anchor encodes");
    Check(size == 771U && encoded[0] == 4U && encoded[1] == 0xe8U &&
              encoded[2] == 0x03U && encoded[3] == 0U &&
              encoded[6] == 1U && encoded[7] == 3U && encoded[8] == 7U,
          "opt-in RGB raw anchor matches app byte fixture");
    Check(fxe::Crc32(encoded.data(), size) == 0x9da85fb6U,
          "opt-in RGB raw anchor matches full app fixture CRC32");
    store::Frame decoded{};
    Check(wire::FrameCodec::Decode(nullptr, encoded.data(), size, decoded,
                                   wire::RgbOnlyMode::Enabled) &&
              Equal(first, decoded),
          "opt-in RGB raw anchor round-trips");
    store::Frame sentinel = first;
    sentinel.durationMs = 3000U;
    decoded = sentinel;
    Check(!wire::FrameCodec::Decode(nullptr, encoded.data(), size, decoded) &&
              Equal(decoded, sentinel),
          "default decoder rejects RGB raw without changing output");
    Check(!wire::FrameCodec::Decode(nullptr, encoded.data(), size - 1U,
                                    decoded, wire::RgbOnlyMode::Enabled) &&
              Equal(decoded, sentinel),
          "truncated RGB raw leaves output unchanged");
    encoded[size] = 0U;
    Check(!wire::FrameCodec::Decode(nullptr, encoded.data(), size + 1U,
                                    decoded, wire::RgbOnlyMode::Enabled) &&
              Equal(decoded, sentinel),
          "extra RGB raw byte leaves output unchanged");

    store::Frame previousWithEffect = first;
    previousWithEffect.emitters[0U].effect = 1U;
    previousWithEffect.emitters[0U].effectDurationMs = 500U;
    store::Frame next = first;
    for (size_t index = 0U; index < 16U; ++index) {
        next.emitters[index].color = {0x12U, 0U,
                                       static_cast<uint8_t>(index)};
    }
    Check(wire::FrameCodec::Encode(&previousWithEffect, next, encoded.data(),
                                   encoded.size(), size,
                                   wire::RgbOnlyMode::Enabled),
          "opt-in RGB bitmap delta encodes");
    std::array<uint8_t, 83U> expected{};
    expected[0] = 5U;
    expected[1] = 0xe8U;
    expected[2] = 0x03U;
    expected[3] = 0xffU;
    expected[4] = 0xffU;
    for (size_t index = 0U; index < 16U; ++index) {
        expected[35U + index * 3U] = 0x12U;
        expected[35U + index * 3U + 2U] =
            static_cast<uint8_t>(index);
    }
    Check(size == expected.size() &&
              std::equal(expected.begin(), expected.end(), encoded.begin()),
          "opt-in RGB bitmap delta matches app byte fixture");
    Check(wire::FrameCodec::Decode(&previousWithEffect, encoded.data(), size, decoded,
                                   wire::RgbOnlyMode::Enabled) &&
              Equal(next, decoded),
          "opt-in RGB bitmap clears predecessor effect and round-trips");
    std::array<uint8_t, 35U> omittedActiveEffect{};
    omittedActiveEffect[0] = 5U;
    omittedActiveEffect[1] = 0xe8U;
    omittedActiveEffect[2] = 0x03U;
    decoded = sentinel;
    Check(!wire::FrameCodec::Decode(&previousWithEffect,
                                    omittedActiveEffect.data(),
                                    omittedActiveEffect.size(), decoded,
                                    wire::RgbOnlyMode::Enabled) &&
              Equal(decoded, sentinel),
          "RGB bitmap cannot inherit omitted active effect");
    decoded = sentinel;
    Check(!wire::FrameCodec::Decode(&first, encoded.data(), size, decoded) &&
              Equal(decoded, sentinel),
          "default decoder rejects RGB bitmap without changing output");
    Check(!wire::FrameCodec::Decode(&first, encoded.data(), size - 1U,
                                    decoded, wire::RgbOnlyMode::Enabled) &&
              Equal(decoded, sentinel),
          "truncated RGB bitmap leaves output unchanged");
    encoded[size] = 0U;
    Check(!wire::FrameCodec::Decode(&first, encoded.data(), size + 1U,
                                    decoded, wire::RgbOnlyMode::Enabled) &&
              Equal(decoded, sentinel),
          "extra RGB bitmap byte leaves output unchanged");
    Check(!wire::FrameCodec::Decode(nullptr, encoded.data(), size, decoded,
                                    wire::RgbOnlyMode::Enabled) &&
              Equal(decoded, sentinel),
          "RGB bitmap requires predecessor");
    encoded[0] = 6U;
    Check(!wire::FrameCodec::Decode(&first, encoded.data(), size, decoded,
                                    wire::RgbOnlyMode::Enabled) &&
              Equal(decoded, sentinel),
          "unknown RGB-adjacent tag leaves output unchanged");

    store::Frame mixed = first;
    mixed.emitters[0U].effect = 1U;
    mixed.emitters[0U].effectDurationMs = 500U;
    Check(wire::FrameCodec::Encode(&first, mixed, encoded.data(),
                                   encoded.size(), size,
                                   wire::RgbOnlyMode::Enabled) &&
              encoded[0] == 3U &&
              wire::FrameCodec::Decode(&first, encoded.data(), size, decoded,
                                        wire::RgbOnlyMode::Enabled) &&
              Equal(mixed, decoded),
          "mixed effect stays on exact legacy indexed encoding");

    store::Frame legacyBitmap = first;
    for (size_t index = 0U; index < 40U; ++index) {
        ++legacyBitmap.emitters[index].color.r;
    }
    Check(wire::FrameCodec::Encode(&first, legacyBitmap, encoded.data(),
                                   encoded.size(), size) &&
              encoded[0] == 1U &&
              wire::FrameCodec::Decode(&first, encoded.data(), size, decoded) &&
              Equal(legacyBitmap, decoded),
          "legacy bitmap tag remains valid with default codec");
}

size_t EncodeCorpus(const size_t changesPerFrame) {
    store::Frame current = BaseFrame();
    store::Frame decoded;
    store::Frame previousDecoded;
    std::array<uint8_t, wire::MAX_ENCODED_FRAME_BYTES> encoded{};
    size_t total = 0U;
    for (size_t frameIndex = 0U; frameIndex < 256U; ++frameIndex) {
        if (frameIndex != 0U) {
            for (size_t change = 0U; change < changesPerFrame; ++change) {
                const size_t emitter = (frameIndex * 37U + change) % 256U;
                ++current.emitters[emitter].color.r;
                current.emitters[emitter].color.g ^= 0x5aU;
            }
        }
        size_t size = 0U;
        const store::Frame* const source =
            frameIndex == 0U ? nullptr : &previousDecoded;
        Check(wire::FrameCodec::Encode(source, current, encoded.data(),
                                       encoded.size(), size),
              "corpus frame encodes");
        Check(wire::FrameCodec::Decode(source, encoded.data(), size, decoded),
              "corpus frame decodes");
        Check(Equal(current, decoded), "corpus frame round-trip is exact");
        total += size;
        previousDecoded = decoded;
    }
    return total;
}

void ReportsDeterministic256FrameCompressionBands() {
    const size_t repeatedBytes = EncodeCorpus(0U);
    const size_t oneChangeBytes = EncodeCorpus(1U);
    const size_t tenChangeBytes = EncodeCorpus(10U);
    const size_t denseBytes = EncodeCorpus(256U);

    Check(repeatedBytes == 1888U, "256 repeated frames encode to 1888 bytes");
    Check(oneChangeBytes == 3673U,
          "256 one-emitter deltas encode to 3673 bytes");
    Check(tenChangeBytes == 17443U,
          "256 ten-emitter deltas encode to 17443 bytes");
    Check(denseBytes == 287488U,
          "256 adversarial frames remain bounded by raw fallback");
    std::printf(
        "[LITCHCRAFT_WIRE_BENCH] repeat=%zuB/%zu_ATT244 one=%zuB/%zu_ATT244 "
        "ten=%zuB/%zu_ATT244 dense=%zuB/%zu_ATT244\n",
        repeatedBytes, (repeatedBytes + 243U) / 244U,
        oneChangeBytes, (oneChangeBytes + 243U) / 244U,
        tenChangeBytes, (tenChangeBytes + 243U) / 244U,
        denseBytes, (denseBytes + 243U) / 244U);
}

void RejectsMalformedOrTruncatedFramesWithoutChangingOutput() {
    const store::Frame previous = BaseFrame();
    store::Frame current = previous;
    current.emitters[7U].color = {1U, 2U, 3U};
    current.emitters[7U].effect = 1U;
    current.emitters[7U].effectDurationMs = 500U;
    std::array<uint8_t, wire::MAX_ENCODED_FRAME_BYTES> encoded{};
    size_t size = 0U;
    Check(wire::FrameCodec::Encode(&previous, current, encoded.data(),
                                   encoded.size(), size),
          "malformed fixture first encodes");

    store::Frame output = previous;
    output.durationMs = 3000U;
    const store::Frame sentinel = output;
    Check(!wire::FrameCodec::Decode(&previous, encoded.data(), size - 1U,
                                    output),
          "truncated indexed delta is rejected");
    Check(Equal(output, sentinel), "truncated delta leaves output unchanged");

    encoded[8U] = 0x0fU;
    encoded[9U] = 0U;
    Check(!wire::FrameCodec::Decode(&previous, encoded.data(), size, output),
          "invalid effect encoding is rejected");
    Check(Equal(output, sentinel), "invalid effect leaves output unchanged");

    size_t rawSize = 99U;
    Check(!wire::FrameCodec::Encode(nullptr, current, encoded.data(),
                                    encoded.size() - 1U, rawSize),
          "raw frame refuses insufficient capacity");
    Check(rawSize == 0U, "failed encode publishes no partial length");
}

void PackedStorageSlotRoundTripsEveryValidEffectState() {
    std::array<uint8_t, slot::PACKED_FRAME_BYTES> packed{};
    for (uint16_t batch = 0U; batch < 4U; ++batch) {
        store::Frame source{};
        source.durationMs = batch == 0U ? 50U : 5000U;
        for (size_t index = 0U; index < source.emitters.size(); ++index) {
            auto& emitter = source.emitters[index];
            emitter.color = {static_cast<uint8_t>(index + batch),
                             static_cast<uint8_t>(index * 7U + batch),
                             static_cast<uint8_t>(index * 13U + batch)};
            const uint16_t code = static_cast<uint16_t>(
                (batch * source.emitters.size() + index) % 769U);
            if (code != 0U) {
                const uint16_t value = code - 1U;
                emitter.effect = static_cast<uint8_t>(value / 96U + 1U);
                emitter.effectDurationMs = static_cast<uint16_t>(
                    500U + (value % 96U) * 100U);
            }
        }
        size_t encodedSize = 0U;
        Check(slot::FrameSlotCodec::Encode(source, packed.data(),
                                            packed.size(), encodedSize) &&
                  encodedSize == slot::PACKED_FRAME_BYTES,
              "fixed-size storage slot accepts every valid effect state");
        if (batch == 0U) {
            Check(fxe::Crc32(packed.data(), packed.size()) == 0x02cdac7bU,
                  "packed slot bytes retain the on-disk fixture identity");
        }
        store::Frame decoded{};
        Check(slot::FrameSlotCodec::Decode(packed.data(), encodedSize,
                                            decoded) && Equal(source, decoded),
              "independent packed slot restores exact color, effect, and duration");
    }
}

void DurationRepresentationsPreserveContractAtCodecBoundaries() {
    // Values near domain edges and one-byte aliases must not round into validity.
    constexpr std::array<uint16_t, 15U> durations{{
        0U, 1U, 100U, 400U, 499U, 500U, 501U, 10000U,
        10001U, 10100U, 25500U, 25600U, 26000U, 65500U, UINT16_MAX}};
    for (const uint8_t effect : {0U, 1U, 8U, 9U}) {
        for (const uint16_t duration : durations) {
            store::Frame source = BaseFrame();
            source.emitters[255U].effect = effect;
            source.emitters[255U].effectDurationMs = duration;
            const bool valid = effect <= 8U &&
                ((effect == 0U && duration == 0U) ||
                 (effect != 0U && duration >= 500U && duration <= 10000U &&
                  duration % 100U == 0U));
            std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
            size_t size = 99U;
            const bool wireAccepted = wire::FrameCodec::Encode(
                nullptr, source, encoded.data(), encoded.size(), size);
            Check(wireAccepted == valid && (wireAccepted || size == 0U),
                  "wire boundary preserves duration acceptance without narrowing or rounding");
            if (wireAccepted) {
                store::Frame decoded{};
                Check(wire::FrameCodec::Decode(nullptr, encoded.data(), size,
                                                decoded) && Equal(source, decoded) &&
                          decoded.emitters[255U].effectDurationMs == duration,
                      "accepted wire effect duration retains exact milliseconds");
            }
            size = 99U;
            const bool slotAccepted = slot::FrameSlotCodec::Encode(
                source, encoded.data(), encoded.size(), size);
            Check(slotAccepted == valid && (slotAccepted || size == 0U),
                  "persisted slot boundary preserves duration acceptance without narrowing or rounding");
            if (slotAccepted) {
                store::Frame decoded{};
                Check(slot::FrameSlotCodec::Decode(encoded.data(), size, decoded) &&
                          Equal(source, decoded) &&
                          decoded.emitters[255U].effectDurationMs == duration,
                      "accepted persisted effect duration retains exact milliseconds");
            }
        }
    }
}

void PackedStorageSlotRejectsDamageWithoutPublishingPixels() {
    store::Frame source = BaseFrame();
    source.emitters[255U].effect = 8U;
    source.emitters[255U].effectDurationMs = 10000U;
    std::array<uint8_t, slot::PACKED_FRAME_BYTES> packed{};
    size_t size = 0U;
    Check(slot::FrameSlotCodec::Encode(source, packed.data(), packed.size(),
                                        size),
          "damage fixture encodes a packed frame");
    store::Frame output = BaseFrame();
    output.durationMs = 3000U;
    const store::Frame sentinel = output;
    Check(!slot::FrameSlotCodec::Decode(packed.data(), size - 1U, output) &&
              Equal(output, sentinel),
          "truncated storage slot leaves displayed frame untouched");
    packed[0] = 0U;
    Check(!slot::FrameSlotCodec::Decode(packed.data(), size, output) &&
              Equal(output, sentinel),
          "unknown storage tag leaves displayed frame untouched");
    packed[0] = 1U;
    packed[1] = 0U;
    packed[2] = 0U;
    Check(!slot::FrameSlotCodec::Decode(packed.data(), size, output) &&
              Equal(output, sentinel),
          "invalid frame duration leaves displayed frame untouched");
    packed[1] = 0xe8U;
    packed[2] = 0x03U;
    const size_t effects = 3U + store::EMITTER_COUNT * 3U;
    for (uint8_t bit = 0U; bit < 10U; ++bit) {
        const size_t position = 255U * 10U + bit;
        packed[effects + position / 8U] |=
            static_cast<uint8_t>(1U << (position % 8U));
    }
    Check(!slot::FrameSlotCodec::Decode(packed.data(), size, output) &&
              Equal(output, sentinel),
          "unused ten-bit effect codes cannot enter the displayed frame");

    size = 99U;
    Check(!slot::FrameSlotCodec::Encode(source, packed.data(),
                                         packed.size() - 1U, size) && size == 0U,
          "packed slot refuses short write capacity without publishing a length");
    source.emitters[0U].effect = 1U;
    source.emitters[0U].effectDurationMs = 400U;
    Check(!slot::FrameSlotCodec::Encode(source, packed.data(), packed.size(),
                                         size) && size == 0U,
          "packed slot refuses invalid semantic effects");
}

} // namespace

int main() {
    IdenticalFrameUsesThreeBytesAndRoundTrips();
    OneChangedEmitterUsesSparseDeltaAndRoundTrips();
    DenseFirstFrameUsesBoundedRawFallbackAndRoundTrips();
    OptInRgbFramesUseExactBytesAndRoundTrip();
    ReportsDeterministic256FrameCompressionBands();
    RejectsMalformedOrTruncatedFramesWithoutChangingOutput();
    PackedStorageSlotRoundTripsEveryValidEffectState();
    DurationRepresentationsPreserveContractAtCodecBoundaries();
    PackedStorageSlotRejectsDamageWithoutPublishingPixels();
    std::printf("[LITCHCRAFT_WIRE_CODEC] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
