#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "modules/litchcraft_animation.hpp"
#include "modules/litchcraft_animation_codec.hpp"
#include "modules/litchcraft_frame_store.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "modules/litchcraft_persistence.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/litchcraft_physical_presentation.hpp"
#include "modules/rgb_optical_calibration.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"
#include "bounded_hint_engine.hpp"
#include "virtual_litch_session.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const std::string& message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message.c_str());
    ++g_failures;
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

modules::Rgb ParseColor(const std::string& value) {
    const uint32_t color = static_cast<uint32_t>(std::stoul(value, nullptr, 16));
    return {static_cast<uint8_t>(color >> 16U),
            static_cast<uint8_t>(color >> 8U), static_cast<uint8_t>(color)};
}

bool Equal(const modules::Rgb left, const modules::Rgb right) {
    return left.r == right.r && left.g == right.g && left.b == right.b;
}

modules::litchcraft_animation::Document CanonicalDocument() {
    using namespace modules::litchcraft_animation;
    Document document{};
    document.topology = 3U;
    document.playbackOnEntry = true;
    document.sparkleSeed = 0x78563412U;
    document.favorites = {{{255U, 116U, 48U}, {255U, 52U, 82U},
                           {255U, 207U, 45U}, {55U, 215U, 105U},
                           {34U, 195U, 226U}, {63U, 106U, 255U},
                           {174U, 83U, 255U}, {255U, 255U, 255U}}};
    document.frames[0].durationMs = 500U;
    document.frames[0].emitters[0] =
        Emitter{{17U, 34U, 51U}, Effect::Breathe, 500U};
    document.frames[0].emitters[1] =
        Emitter{{0U, 0U, 0U}, Effect::Sparkle, 10000U};
    document.frames[0].emitters[255] =
        Emitter{{171U, 205U, 239U}, Effect::Rainbow, 2000U};
    return document;
}

modules::litchcraft_animation::ValidatedDocument Adopt(
    const modules::litchcraft_animation::Document& document) {
    modules::litchcraft_animation::ValidatedDocument adopted;
    Check(modules::litchcraft_animation::ValidatedDocument::TryAdopt(document,
                                                                     adopted),
          "test document adopts once before use");
    return adopted;
}

modules::SettingsStoreConfig RenderConfig() {
    modules::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_animation_render.json";
    return config;
}

struct RenderBackend {
    static fxe::SettingsStore::AsyncSaveAdmission Enqueue(
        void*, const JsonDocument&, uint32_t& receipt) {
        receipt = 1U;
        return fxe::SettingsStore::AsyncSaveAdmission::Accepted;
    }
    static modules::LitchcraftPersistence::SaveOutcome Poll(void*, uint32_t) {
        return modules::LitchcraftPersistence::SaveOutcome::Saved;
    }
    static void Release(void*, uint32_t) {}
    static modules::LitchcraftPersistence::AdvancedReadOutcome Read(
        void*, modules::LitchcraftPersistence::AdvancedSlot, uint8_t*, size_t,
        size_t&) {
        return modules::LitchcraftPersistence::AdvancedReadOutcome::Missing;
    }
    static fxe::SettingsStore::AsyncSaveAdmission EnqueueAdvanced(
        void*, const uint8_t*, size_t, uint32_t& receipt) {
        receipt = 1U;
        return fxe::SettingsStore::AsyncSaveAdmission::Accepted;
    }
};

struct ProductionRenderFixture {
    modules::SettingsStore settings{RenderConfig()};
    modules::LitchcraftSession session;
    modules::LitchcraftPersistence persistence{settings, session};
    modules::LitchcraftPersistence::AdvancedStorage storage;
    uint16_t nextPlaybackRequest = 2U;

    explicit ProductionRenderFixture(
        const modules::litchcraft_animation::ValidatedDocument& document) {
        session.Configure(3U, 7U);
        session.BeginConnection();
        modules::LitchcraftSession::Packet enter{};
        enter.bytes[0] = modules::LitchcraftSession::MAGIC;
        enter.bytes[1] = static_cast<uint8_t>(modules::LitchcraftSession::Op::Enter);
        enter.bytes[6] = 1U;
        enter.size = 8U;
        session.HandlePacket(enter.bytes.data(), enter.size, true, 0U);
        persistence.Configure({nullptr, &RenderBackend::Enqueue,
                               &RenderBackend::Poll, &RenderBackend::Release,
                               &RenderBackend::Read,
                               &RenderBackend::EnqueueAdvanced});
        persistence.ConfigureAdvancedStorage(storage);
        Check(persistence.SetAdvancedDocument(document, true),
              "production render fixture admits schema-3 authority");
    }

    void SelectFrame(const uint8_t frame) {
        Playback(2U, frame);
    }

    void Playback(const uint8_t action, const uint8_t frame) {
        modules::LitchcraftSession::BulkPacket request{};
        request.bytes[0] = 0x4dU;
        request.bytes[1] = 2U;
        request.bytes[2] = 2U;
        for (uint8_t byte = 0U; byte < 4U; ++byte) {
            request.bytes[3U + byte] = session.SessionId() >> (byte * 8U);
        }
        request.bytes[7] = nextPlaybackRequest;
        request.bytes[8] = nextPlaybackRequest++ >> 8U;
        request.bytes[9] = action;
        request.bytes[10] = frame;
        request.size = 11U;
        session.HandlePacket(request.bytes.data(), request.size, true, 0U);
        modules::LitchcraftSession::Packet response{};
        while (session.PeekPacket(response)) session.AcceptPacket();
    }
};

void RunEffectsWithoutFramePlayback() {
    using namespace modules::litchcraft_animation;
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Document document{};
    document.frameCount = 2U;
    document.playbackOnEntry = false;
    for (uint8_t frame = 0U; frame < 2U; ++frame) {
        document.frames[frame].durationMs = 1000U;
        document.frames[frame].emitters[0] =
            Emitter{{128U, 64U, 32U}, Effect::Breathe, 500U};
    }
    ProductionRenderFixture fixture(Adopt(document));
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    Square square{};
    square[0] = document.frames[0].emitters[0];
    const auto expect = [&](const uint32_t now, const uint8_t frame) {
        fxe::ManualMillis::SetNowMs(now);
        fixture.session.Compose(board, status);
        Check(fixture.session.SelectedFrame() == frame,
              "frame pause/selection preserves the intended frame");
        Check(Equal(board[0].left,
                    ComposeSquare(square, now, document.sparkleSeed, frame, 0U)[0]),
              "applied square effect advances independently of frame Play/Pause/Select");
    };
    expect(0U, 0U);
    expect(125U, 0U);
    Check(!fixture.session.IsAnimationPlaying() &&
              fixture.session.AnimationElapsedMs() == 0U,
          "effect motion does not start the frame animation");
    fixture.Playback(1U, 0U);
    expect(250U, 0U);
    fixture.Playback(0U, 0U);
    expect(375U, 0U);
    fixture.SelectFrame(1U);
    expect(625U, 1U);
    Check(!fixture.session.IsAnimationPlaying() &&
              fixture.session.AnimationElapsedMs() == 1000U,
          "frame seek freezes only the frame cursor while effects continue");
}

void RestoreStaticCanvas(modules::LitchcraftSession& session,
                         const modules::LitchcraftSession::Canvas& canvas) {
    session.Configure(3U, 7U);
    session.RestoreArtwork(canvas, true, 0U);
}

struct AttachedStaticRenderFixture {
    modules::SettingsStore settings{RenderConfig()};
    modules::LitchcraftSession session;
    modules::LitchcraftPersistence persistence{settings, session};
    modules::LitchcraftPersistence::AdvancedStorage storage;

    explicit AttachedStaticRenderFixture(
        const modules::LitchcraftSession::Canvas& canvas) {
        persistence.Configure({nullptr, &RenderBackend::Enqueue,
                               &RenderBackend::Poll, &RenderBackend::Release,
                               &RenderBackend::Read,
                               &RenderBackend::EnqueueAdvanced});
        persistence.ConfigureAdvancedStorage(storage);
        RestoreStaticCanvas(session, canvas);
    }
};

void RunGoldenVectors() {
    using namespace modules::litchcraft_animation;
    std::ifstream input("test/fixtures/litchcraft_animation_vectors.csv");
    Check(input.is_open(), "shared animation fixture opens from repository root");
    std::string line;
    size_t vectorCount = 0U;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto fields = Split(line);
        Check(fields.size() == 16U, "fixture row has sixteen fields: " + line);
        if (fields.size() != 16U) {
            continue;
        }
        const auto effect = static_cast<Effect>(std::stoul(fields[1]));
        const uint16_t duration = static_cast<uint16_t>(std::stoul(fields[2]));
        const uint64_t elapsed = std::stoull(fields[3]);
        const uint32_t seed = static_cast<uint32_t>(std::stoul(fields[4], nullptr, 16));
        const uint8_t frame = static_cast<uint8_t>(std::stoul(fields[5]));
        const uint16_t emitterBase = static_cast<uint16_t>(std::stoul(fields[6]));
        const uint8_t mask = static_cast<uint8_t>(std::stoul(fields[7]));
        Square square{};
        for (size_t index = 0U; index < square.size(); ++index) {
            square[index].color = ParseColor(fields[8U + index]);
            if ((mask & (1U << index)) != 0U) {
                square[index].effect = effect;
                square[index].durationMs = duration;
            }
        }
        const ComposedSquare output =
            ComposeSquare(square, elapsed, seed, frame, emitterBase);
        for (size_t index = 0U; index < output.size(); ++index) {
            Check(Equal(output[index], ParseColor(fields[12U + index])),
                  fields[0] + " emitter " + std::to_string(index));
        }
        ++vectorCount;
    }
    Check(vectorCount == 23U, "all shared animation vectors ran");
}

void RunSchemaBounds() {
    using namespace modules::litchcraft_animation;
    for (uint32_t duration = 0U; duration <= UINT16_MAX; ++duration) {
        const EffectDuration stored(static_cast<uint16_t>(duration));
        for (uint8_t effect = 0U; effect <= 8U; ++effect) {
            const bool valid = IsValidEffectDuration(static_cast<Effect>(effect), duration);
            Check(IsValidEffectDuration(static_cast<Effect>(effect), stored) == valid &&
                      (!valid || static_cast<uint16_t>(stored) == duration),
                  "compact effect timing preserves every valid millisecond value and rejects every invalid value");
        }
    }
    Check(IsValidDocumentHeader(3U, 1U, 1U, 0U, 1U),
          "minimum schema-3 header is valid");
    Check(IsValidDocumentHeader(3U, 8U, 3U, 1U, 0xffffffffU),
          "maximum schema-3 header is valid");
    Check(!IsValidDocumentHeader(2U, 1U, 1U, 0U, 1U), "old schema is rejected");
    Check(!IsValidDocumentHeader(3U, 0U, 1U, 0U, 1U), "zero frames rejected");
    Check(!IsValidDocumentHeader(3U, 9U, 1U, 0U, 1U), "ninth frame rejected");
    Check(!IsValidDocumentHeader(3U, 1U, 0U, 0U, 1U), "topology zero rejected");
    Check(!IsValidDocumentHeader(3U, 1U, 4U, 0U, 1U), "unknown topology rejected");
    Check(!IsValidDocumentHeader(3U, 1U, 1U, 2U, 1U), "reserved flag rejected");
    Check(!IsValidDocumentHeader(3U, 1U, 1U, 0U, 0U), "zero seed rejected");
    Check(IsValidFrameDuration(50U) && IsValidFrameDuration(137U) &&
              IsValidFrameDuration(5000U),
          "frame duration endpoints valid");
    Check(!IsValidFrameDuration(49U) && !IsValidFrameDuration(5001U),
          "frame duration exact integer range enforced");
    Check(IsValidEffectDuration(Effect::Off, 0U), "Off requires zero duration");
    Check(!IsValidEffectDuration(Effect::Off, 500U), "Off rejects active duration");
    Check(IsValidEffectDuration(Effect::Sparkle, 500U) &&
              IsValidEffectDuration(Effect::Sparkle, 10000U),
          "effect duration endpoints valid");
    Check(!IsValidEffectDuration(Effect::Sparkle, 499U) &&
              !IsValidEffectDuration(Effect::Sparkle, 501U) &&
              !IsValidEffect(9U),
          "effect range, step, and enum enforced");
}

void RunMaskCoverage() {
    using namespace modules::litchcraft_animation;
    for (uint8_t effectId = 1U; effectId <= 8U; ++effectId) {
        for (uint8_t memberCount = 1U; memberCount <= 4U; ++memberCount) {
            Square square{};
            for (size_t index = 0U; index < square.size(); ++index) {
                square[index].color = index < memberCount
                                          ? modules::Rgb{128U, 64U, 32U}
                                          : modules::Rgb{7U, 9U, 11U};
                if (index < memberCount) {
                    square[index].effect = static_cast<Effect>(effectId);
                    square[index].durationMs = 2000U;
                }
            }
            for (const uint64_t elapsed : {0ULL, 499ULL, 500ULL, 999ULL,
                                           1000ULL, 1500ULL, 1999ULL, 2000ULL}) {
                const ComposedSquare output =
                    ComposeSquare(square, elapsed, 0x12345678U, 3U, 40U);
                size_t litMembers = 0U;
                for (size_t index = 0U; index < output.size(); ++index) {
                    const modules::Rgb color = output[index];
                    Check(color.r <= 128U && color.g <= 128U && color.b <= 128U,
                          "effect output never exceeds source maximum channel");
                    if (index >= memberCount) {
                        Check(Equal(color, square[index].color),
                              "effect mask does not spill to another emitter");
                    } else if (color.r != 0U || color.g != 0U || color.b != 0U) {
                        ++litMembers;
                    }
                }
                if (effectId >= 5U && effectId <= 7U) {
                    Check(litMembers == 1U,
                          "orbit and bounce keep exactly one group member lit");
                }
            }
        }
    }

    Square invalid{};
    invalid[0].color = {255U, 255U, 255U};
    invalid[0].effect = Effect::Flash;
    invalid[0].durationMs = 0U;
    const ComposedSquare failed = ComposeSquare(invalid, 0U, 1U, 0U, 0U);
    Check(std::all_of(failed.begin(), failed.end(), [](const modules::Rgb color) {
              return color.r == 0U && color.g == 0U && color.b == 0U;
          }),
          "invalid renderer input fails dark without division by zero");
}

void RunTimelineAndClock() {
    using namespace modules::litchcraft_animation;
    Document document = CanonicalDocument();
    document.frameCount = 3U;
    document.frames[0].durationMs = 100U;
    document.frames[1].durationMs = 200U;
    document.frames[2].durationMs = 300U;
    document.frames[0].emitters[0] = Emitter{{1U, 0U, 0U}, Effect::Off, 0U};
    document.frames[1].emitters[0] = Emitter{{2U, 0U, 0U}, Effect::Off, 0U};
    document.frames[2].emitters[0] = Emitter{{3U, 0U, 0U}, Effect::Off, 0U};
    const ValidatedDocument adopted = Adopt(document);
    uint64_t start0 = 99U;
    uint64_t start1 = 99U;
    uint64_t start2 = 99U;
    Check(TryFrameStartElapsed(adopted, 0U, start0) && start0 == 0U &&
              TryFrameStartElapsed(adopted, 1U, start1) && start1 == 100U &&
              TryFrameStartElapsed(adopted, 2U, start2) && start2 == 300U,
          "frame seeks use cumulative starts");
    uint64_t rejectedStart = 77U;
    Check(!TryFrameStartElapsed(adopted, 3U, rejectedStart) &&
              rejectedStart == 77U,
          "invalid frame seek is unambiguous and preserves output");
    const std::array<uint64_t, 9U> elapsed{{0U, 99U, 100U, 299U, 300U,
                                           599U, 600U, 700U, 900U}};
    const std::array<uint8_t, 9U> expected{{0U, 0U, 1U, 1U, 2U,
                                           2U, 0U, 1U, 2U}};
    for (size_t index = 0U; index < elapsed.size(); ++index) {
        Check(FrameIndexAt(adopted, elapsed[index]) == expected[index],
              "hard-cut timeline boundary " + std::to_string(index));
        const ComposedFrame composed = ComposeDocument(adopted, elapsed[index]);
        Check(composed.frameIndex == expected[index] &&
                  composed.emitters[0].r == expected[index] + 1U,
              "one elapsed sample selects and composes the same frame");
    }

    PlaybackCursor cursor;
    Check(cursor.Sample(50U) == 0U, "paused clock remains frozen");
    cursor.Resume(100U);
    Check(cursor.Sample(175U) == 75U, "playing clock accumulates monotonic time");
    cursor.Resume(175U);
    Check(cursor.Sample(200U) == 100U,
          "repeat Resume while playing is idempotent");
    Check(cursor.Pause(200U) == 100U && cursor.Sample(900U) == 100U,
          "pause captures once and freezes");
    cursor.Seek(start2, 900U);
    Check(cursor.Sample(999U) == 300U, "paused seek lands at frame start");
    cursor.Resume(1000U);
    Check(cursor.Sample(1050U) == 350U, "resume continues from seek");
    Check(cursor.SelectFrame(adopted, 1U, 1060U) && !cursor.IsPlaying() &&
              cursor.Sample(9999U) == 100U,
          "public frame selection pauses and seeks while playing");
    Check(!cursor.SelectFrame(adopted, 3U, 10000U) &&
              cursor.Sample(10001U) == 100U,
          "rejected selection leaves cursor unchanged");

    for (Frame& frame : document.frames) {
        for (Emitter& emitter : frame.emitters) {
            emitter = Emitter{{128U, 64U, 32U}, Effect::Bounce, 2000U};
        }
    }
    const ValidatedDocument denseDocument = Adopt(document);
    const ComposedFrame dense = ComposeDocument(denseDocument, 777U);
    Check(dense.emittersEvaluated == 256U &&
              dense.groupCandidatesVisited == 1024U,
          "dense render has one emitter pass and bounded four-member group scans");
}

void RunCodec() {
    using namespace modules::litchcraft_animation;
    const Document document = CanonicalDocument();
    const ValidatedDocument adopted = Adopt(document);
    EncodedDocument encoded{};
    Check(EncodeDocument(adopted, encoded), "canonical schema-3 document encodes");
    Document staging{};

    std::ifstream input("test/fixtures/litchcraft_animation_codec_bytes.csv");
    Check(input.is_open(), "shared codec byte fixture opens");
    std::vector<uint8_t> expected;
    uint32_t expectedCrc = 0U;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto fields = Split(line);
        if (fields[0] == "length") {
            expected.assign(static_cast<size_t>(std::stoul(fields[1])), 0U);
        } else if (fields[0] == "crc32") {
            expectedCrc = static_cast<uint32_t>(std::stoul(fields[1], nullptr, 16));
        } else {
            const size_t offset = static_cast<size_t>(std::stoul(fields[0]));
            for (size_t index = 0U; index < fields[1].size(); index += 2U) {
                expected[offset + index / 2U] = static_cast<uint8_t>(
                    std::stoul(fields[1].substr(index, 2U), nullptr, 16));
            }
        }
    }
    Check(encoded.size == expected.size() &&
              std::equal(expected.begin(), expected.end(), encoded.bytes.begin()),
          "native codec matches every shared canonical byte");
    Check(encoded.crc32 == expectedCrc, "native codec matches shared CRC32");
    const uint8_t crcOracle[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    Check(fxe::Crc32(crcOracle, sizeof(crcOracle)) == 0xcbf43926U,
          "codec uses repository IEEE CRC32 definition");

    ValidatedDocument decoded;
    DecodeWork decodeWork{};
    Check(DecodeDocument(encoded.bytes.data(), encoded.size, encoded.crc32,
                         staging, decoded, &decodeWork) &&
              decoded.Value().frameCount == 1U &&
              decoded.Value().sparkleSeed == 0x78563412U &&
              decoded.Value().frames[0].emitters[255].effect == Effect::Rainbow,
          "validated canonical body round trips");
    Check(decodeWork.checksumCalls == 1U &&
              decodeWork.bytesDecoded == encoded.size,
          "accepted decode reports one checksum and one bounded body decode");
    Document exactDurationSource = CanonicalDocument();
    exactDurationSource.frameCount = 3U;
    exactDurationSource.frames[0].durationMs = 50U;
    exactDurationSource.frames[1].durationMs = 137U;
    exactDurationSource.frames[2].durationMs = 5000U;
    const ValidatedDocument exactDurationAdopted = Adopt(exactDurationSource);
    EncodedDocument exactDurationEncoded{};
    Check(EncodeDocument(exactDurationAdopted, exactDurationEncoded) &&
              DecodeDocument(exactDurationEncoded.bytes.data(),
                             exactDurationEncoded.size,
                             exactDurationEncoded.crc32, staging, decoded) &&
              decoded.Value().frames[0].durationMs == 50U &&
              decoded.Value().frames[1].durationMs == 137U &&
              decoded.Value().frames[2].durationMs == 5000U,
          "50 ms, arbitrary exact, and 5000 ms frame durations round trip");
    Document replacementSource = CanonicalDocument();
    replacementSource.sparkleSeed = 99U;
    const ValidatedDocument replacementAdopted = Adopt(replacementSource);
    EncodedDocument replacementEncoded{};
    Check(EncodeDocument(replacementAdopted, replacementEncoded) &&
              DecodeDocument(replacementEncoded.bytes.data(),
                             replacementEncoded.size, replacementEncoded.crc32,
                             staging, decoded) &&
              decoded.Value().sparkleSeed == 99U &&
              staging.sparkleSeed == 0x78563412U,
          "successful replacement swaps the prior body into reusable staging");

    Document sentinelSource = CanonicalDocument();
    sentinelSource.sparkleSeed = 99U;
    ValidatedDocument sentinel = Adopt(sentinelSource);
    auto damaged = encoded;
    damaged.bytes[34] ^= 1U;
    Check(!DecodeDocument(damaged.bytes.data(), damaged.size, encoded.crc32,
                          staging, sentinel) &&
              sentinel.Value().sparkleSeed == 99U,
          "CRC failure leaves prior authority untouched");
    damaged = encoded;
    damaged.bytes[37] = 9U;
    const uint32_t damagedCrc = fxe::Crc32(damaged.bytes.data(), damaged.size);
    Check(!DecodeDocument(damaged.bytes.data(), damaged.size, damagedCrc,
                          staging, sentinel) &&
              sentinel.Value().sparkleSeed == 99U,
          "unknown effect with valid CRC leaves prior authority untouched");
    damaged = encoded;
    damaged.bytes[3] = 2U;
    Check(!DecodeDocument(damaged.bytes.data(), damaged.size,
                          fxe::Crc32(damaged.bytes.data(), damaged.size), staging,
                          sentinel),
          "reserved flags fail closed");
    damaged = encoded;
    damaged.bytes[4] = damaged.bytes[5] = damaged.bytes[6] = damaged.bytes[7] = 0U;
    Check(!DecodeDocument(damaged.bytes.data(), damaged.size,
                          fxe::Crc32(damaged.bytes.data(), damaged.size), staging,
                          sentinel),
          "zero Sparkle seed with valid CRC fails closed");
    damaged = encoded;
    damaged.bytes[11] = damaged.bytes[8];
    damaged.bytes[12] = damaged.bytes[9];
    damaged.bytes[13] = damaged.bytes[10];
    ValidatedDocument duplicateFavorites;
    Check(DecodeDocument(damaged.bytes.data(), damaged.size,
                          fxe::Crc32(damaged.bytes.data(), damaged.size), staging,
                          duplicateFavorites),
          "ordered duplicate favorites are valid document data");
    EncodedDocument duplicateEncoded;
    Check(EncodeDocument(duplicateFavorites, duplicateEncoded) &&
              duplicateEncoded.size == damaged.size &&
              std::equal(damaged.bytes.begin(), damaged.bytes.begin() + damaged.size,
                         duplicateEncoded.bytes.begin()),
          "duplicate favorites round-trip byte-exactly without normalization");
    damaged = encoded;
    damaged.bytes[32] = 49U;
    damaged.bytes[33] = 0U;
    Check(!DecodeDocument(damaged.bytes.data(), damaged.size,
                          fxe::Crc32(damaged.bytes.data(), damaged.size), staging,
                          sentinel),
          "below-minimum frame duration with valid CRC fails closed");
    damaged = encoded;
    damaged.bytes[38] = 0xf5U;
    damaged.bytes[39] = 0x01U;
    Check(!DecodeDocument(damaged.bytes.data(), damaged.size,
                          fxe::Crc32(damaged.bytes.data(), damaged.size), staging,
                          sentinel),
          "off-step effect duration with valid CRC fails closed");
    Check(!DecodeDocument(encoded.bytes.data(), encoded.size - 1U, encoded.crc32,
                          staging, sentinel),
          "truncated body fails closed");
    Check(sentinel.Value().sparkleSeed == 99U,
          "every rejected decode preserves the previously adopted document");

    std::array<uint8_t, MAX_ENCODED_BYTES + 1U> oversized{};
    oversized[0] = SCHEMA;
    oversized[1] = 8U;
    oversized[2] = 3U;
    oversized[4] = 1U;
    DecodeWork rejectedWork{};
    Check(!DecodeDocument(oversized.data(), oversized.size(), 0U, staging,
                          sentinel, &rejectedWork) &&
              rejectedWork.checksumCalls == 0U && rejectedWork.bytesDecoded == 0U,
          "oversized input is rejected before CRC or adoption");
    std::array<uint8_t, ENCODED_HEADER_BYTES> impossibleHeader{};
    impossibleHeader[0] = SCHEMA;
    impossibleHeader[1] = 9U;
    impossibleHeader[2] = 3U;
    impossibleHeader[4] = 1U;
    Check(!DecodeDocument(impossibleHeader.data(), impossibleHeader.size(), 0U,
                          staging, sentinel, &rejectedWork) &&
              rejectedWork.checksumCalls == 0U && rejectedWork.bytesDecoded == 0U,
          "frame count nine is rejected at bounded admission");
    impossibleHeader[1] = 255U;
    Check(!DecodeDocument(impossibleHeader.data(), impossibleHeader.size(), 0U,
                          staging, sentinel, &rejectedWork) &&
              rejectedWork.checksumCalls == 0U && rejectedWork.bytesDecoded == 0U,
          "frame count 255 is rejected at bounded admission");

    Document maximum = CanonicalDocument();
    maximum.frameCount = 8U;
    for (size_t frame = 0U; frame < maximum.frameCount; ++frame) {
        for (size_t emitter = 0U; emitter < EMITTER_COUNT; ++emitter) {
            auto& value = maximum.frames[frame].emitters[emitter];
            value.effect = static_cast<Effect>(1U + frame);
            value.durationMs = static_cast<uint16_t>(500U + (emitter % 96U) * 100U);
        }
    }
    const ValidatedDocument maximumAdopted = Adopt(maximum);
    EncodedDocument maximumEncoded{};
    ValidatedDocument maximumDecoded;
    Check(EncodeDocument(maximumAdopted, maximumEncoded) &&
              maximumEncoded.size == MAX_ENCODED_BYTES &&
              DecodeDocument(maximumEncoded.bytes.data(), maximumEncoded.size,
                             maximumEncoded.crc32, staging, maximumDecoded) &&
              maximumDecoded.Value().frameCount == 8U &&
              DocumentsEqual(maximum, maximumDecoded.Value()),
          "maximum eight-frame body encodes and decodes at the exact bound");
    Check(sizeof(ValidatedDocument) * 2U + sizeof(Document) +
                  sizeof(EncodedDocument) +
                  sizeof(ComposedFrame) <=
              64U * 1024U,
          "three owned documents, one codec buffer, and one render fit 64 KiB");
}

void RunIndependentEffectEdges() {
    using namespace modules::litchcraft_animation;
    auto sample = [](const modules::Rgb color, const Effect effect,
                     const uint16_t duration, const uint64_t elapsed) {
        Square square{};
        square[0] = Emitter{color, effect, duration};
        return ComposeSquare(square, elapsed, 1U, 0U, 0U)[0];
    };
    Check(Equal(sample({255U, 255U, 0U}, Effect::Rainbow, 2000U, 0U),
                {255U, 255U, 0U}) &&
              Equal(sample({0U, 255U, 255U}, Effect::Rainbow, 2000U, 0U),
                    {0U, 255U, 255U}) &&
              Equal(sample({255U, 0U, 255U}, Effect::Rainbow, 2000U, 0U),
                    {255U, 0U, 255U}),
          "rainbow max-channel ties retain exact source at phase zero");
    Check(Equal(sample({1U, 2U, 3U}, Effect::Breathe, 500U, 0U),
                {0U, 1U, 1U}),
          "channel multiplication uses round-to-nearest at the trough");
    Check(Equal(sample({17U, 34U, 51U}, Effect::Flash, 500U, 0U),
                sample({17U, 34U, 51U}, Effect::Flash, 500U, 500U)) &&
              Equal(sample({17U, 34U, 51U}, Effect::FadeToBlack, 10000U, 0U),
                    sample({17U, 34U, 51U}, Effect::FadeToBlack, 10000U, 10000U)),
          "minimum and maximum periods repeat exactly at t plus duration");

    Square group{};
    for (size_t index = 0U; index < group.size(); ++index) {
        group[index] = Emitter{{static_cast<uint8_t>(index + 1U), 0U, 0U},
                               Effect::Bounce, 600U};
    }
    const std::array<uint64_t, 7U> times{{0U, 101U, 201U, 301U, 401U, 501U, 600U}};
    const std::array<size_t, 7U> path{{0U, 1U, 2U, 3U, 2U, 1U, 0U}};
    for (size_t step = 0U; step < path.size(); ++step) {
        const ComposedSquare output = ComposeSquare(group, times[step], 1U, 0U, 0U);
        for (size_t index = 0U; index < output.size(); ++index) {
            Check((output[index].r != 0U) == (index == path[step]),
                  "bounce follows exact forward/reverse path");
        }
    }
    for (Emitter& emitter : group) {
        emitter.effect = Effect::OrbitClockwise;
        emitter.durationMs = 2000U;
    }
    const ComposedSquare clockwise = ComposeSquare(group, 0U, 1U, 0U, 0U);
    for (Emitter& emitter : group) {
        emitter.effect = Effect::OrbitCounterclockwise;
    }
    const ComposedSquare counterclockwise = ComposeSquare(group, 0U, 1U, 0U, 0U);
    Check(clockwise[0].r != 0U && counterclockwise[3].r != 0U,
          "clockwise and counterclockwise start at opposite group ends");

    const uint64_t wrappedCycle = (uint64_t{1U} << 32U) * 2000U + 500U;
    Check(Equal(sample({128U, 64U, 32U}, Effect::Sparkle, 2000U, 500U),
                sample({128U, 64U, 32U}, Effect::Sparkle, 2000U, wrappedCycle)),
          "Sparkle hashes only the low 32 bits of the cycle");
    Check(Equal(sample({12U, 34U, 56U}, Effect::Off, 0U, UINT64_MAX),
                {12U, 34U, 56U}),
          "static Off remains exact at uint64 boundary");
    Check(Equal(sample({12U, 34U, 56U}, Effect::Flash, 500U, UINT64_MAX),
                {12U, 34U, 56U}),
          "effect phase remains wrap-safe at uint64 boundary");
}

modules::litchcraft_animation::Document ProductionDocument() {
    using namespace modules::litchcraft_animation;
    Document document{};
    document.frameCount = 8U;
    document.topology = 3U;
    document.playbackOnEntry = true;
    document.sparkleSeed = 0x12345678U;
    for (uint8_t frame = 0U; frame < document.frameCount; ++frame) {
        document.frames[frame].durationMs =
            static_cast<uint16_t>(100U * (frame + 1U));
        for (uint16_t emitter = 0U; emitter < EMITTER_COUNT; ++emitter) {
            document.frames[frame].emitters[emitter].color = {
                static_cast<uint8_t>(20U + (emitter % 71U)),
                static_cast<uint8_t>(40U + (frame * 13U)),
                static_cast<uint8_t>(80U + (emitter % 53U))};
        }
    }
    for (uint8_t effect = 1U; effect <= 8U; ++effect) {
        const uint8_t mask = static_cast<uint8_t>((1U << ((effect - 1U) % 4U + 1U)) - 1U);
        const uint16_t square = effect;
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            if ((mask & (1U << slot)) == 0U) {
                continue;
            }
            auto& emitter = document.frames[0].emitters[square * 4U + slot];
            emitter.effect = static_cast<Effect>(effect);
            emitter.durationMs = static_cast<uint16_t>(500U + effect * 100U);
        }
    }
    return document;
}

bool EqualBoard(const modules::BoardLedFrame& board,
                const modules::litchcraft_animation::ComposedFrame& expected) {
    for (uint8_t square = 0U; square < 64U; ++square) {
        const auto& actual = board[square];
        const size_t base = square * 4U;
        if (!Equal(actual.left, expected.emitters[base]) ||
            !Equal(actual.top, expected.emitters[base + 1U]) ||
            !Equal(actual.right, expected.emitters[base + 2U]) ||
            !Equal(actual.bottom, expected.emitters[base + 3U])) {
            return false;
        }
    }
    return true;
}

bool EqualBoard(const modules::BoardLedFrame& left,
                const modules::BoardLedFrame& right) {
    for (size_t square = 0U; square < left.size(); ++square) {
        if (!Equal(left[square].left, right[square].left) ||
            !Equal(left[square].top, right[square].top) ||
            !Equal(left[square].right, right[square].right) ||
            !Equal(left[square].bottom, right[square].bottom)) {
            return false;
        }
    }
    return true;
}

void RunAttachedStaticParity() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1337U);
    modules::LitchcraftSession::Canvas canvas{};
    for (size_t emitter = 0U; emitter < canvas.size(); ++emitter) {
        canvas[emitter] = {static_cast<uint8_t>(emitter),
                           static_cast<uint8_t>(255U - emitter),
                           static_cast<uint8_t>((emitter * 17U) % 256U)};
    }
    modules::LitchcraftSession ordinary;
    RestoreStaticCanvas(ordinary, canvas);
    AttachedStaticRenderFixture attached(canvas);
    modules::BoardLedFrame ordinaryBoard{};
    modules::BoardLedFrame attachedBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> ordinaryStatus{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> attachedStatus{};
    ordinary.Compose(ordinaryBoard, ordinaryStatus);
    attached.session.Compose(attachedBoard, attachedStatus);
    Check(EqualBoard(ordinaryBoard, attachedBoard) &&
              std::equal(ordinaryStatus.begin(), ordinaryStatus.end(),
                         attachedStatus.begin(),
                         [](const modules::Rgb left, const modules::Rgb right) {
                             return Equal(left, right);
                         }),
          "configured but Missing advanced backend keeps complete static semantic/status parity");
}

modules::litchcraft_animation::Document PipelineDocument() {
    using namespace modules::litchcraft_animation;
    Document document{};
    document.topology = 3U;
    document.playbackOnEntry = true;
    document.sparkleSeed = 1U;
    document.frames[0].durationMs = 1000U;
    const std::array<modules::Rgb, 4U> colors{{
        {180U, 80U, 32U}, {48U, 160U, 96U},
        {48U, 96U, 192U}, {160U, 64U, 128U}}};
    for (uint8_t emitter = 0U; emitter < colors.size(); ++emitter) {
        document.frames[0].emitters[emitter] =
            Emitter{colors[emitter], Effect::Breathe, 500U};
    }
    return document;
}

void EnterLitchcraftThroughProductionIngress(
    runtime::VirtualLitchSession& session) {
    session.Litchcraft().BeginConnection();
    modules::LitchcraftSession::Packet enter{};
    enter.bytes[0] = modules::LitchcraftSession::MAGIC;
    enter.bytes[1] = static_cast<uint8_t>(modules::LitchcraftSession::Op::Enter);
    enter.bytes[6] = 1U;
    enter.size = 8U;
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    Check(session.Litchcraft().IsActive(),
          "production VirtualLitch ingress enters an empty Litchcraft session");
    session.Advance(250U); // The separate mode journey verifies this presentation-only fade.
}

void RunVirtualS3PipelineCapture() {
    using namespace modules::litchcraft_animation;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    const Document document = PipelineDocument();
    const ValidatedDocument adopted = Adopt(document);
    for (const bool rotated : {false, true}) {
        virtual_litch_config config{};
        virtual_litch_default_config(&config);
        config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
        config.board_rotate = rotated ? 1U : 0U;
        config.brightness_scale_256 = 256U;
        test_support::BoundedHintEngine engine;
        runtime::VirtualLitchSession session(config, engine);
        session.ObserveBootHallMask(0U);
        session.Advance(3000U);
        EnterLitchcraftThroughProductionIngress(session);
        modules::LitchcraftPersistence::AdvancedStorage storage;
        session.ArtPersistence().Configure(
            {nullptr, &RenderBackend::Enqueue, &RenderBackend::Poll,
             &RenderBackend::Release, &RenderBackend::Read,
             &RenderBackend::EnqueueAdvanced});
        session.ArtPersistence().ConfigureAdvancedStorage(storage);
        // This fixture directly adopts persistence outside VirtualLitch's
        // ingress time scope, so give the shared clock the board's current time.
        fxe::ManualMillis::SetNowMs(session.State().now_ms);
        Check(session.ArtPersistence().SetAdvancedDocument(adopted, true),
              "VirtualLitch installs advanced authority through its production persistence owner");
        Check(!session.Litchcraft().IsAnimationPlaying(),
              "directly adopted artwork starts with frame playback paused");
        const uint8_t play[] = {1U, 0U};
        Check(session.Litchcraft().ApplyConnectionOrdered(
                  2U, play, sizeof(play)) ==
                  modules::LitchcraftSession::Status::Ok,
              "VirtualLitch starts the authored clock through explicit Play");
        session.Advance(137U);
        Check(session.Litchcraft().AnimationElapsedMs() == 137U,
              "VirtualLitch direct-adoption fixture starts the document at board time");

        const ComposedFrame expected = ComposeDocument(adopted, 137U);
        const auto& semantic = session.SemanticFrame();
        const auto& physical = session.PhysicalFrame();
        const modules::BoardSquareLeds expectedSquare{
            expected.emitters[0U], expected.emitters[1U],
            expected.emitters[2U], expected.emitters[3U]};
        const modules::Rgb expectedCenter =
            modules::RoundedPieceAverage(expectedSquare);
        Check(semantic.board[0U][0U].r == expectedSquare.left.r &&
                  semantic.board[0U][0U].g == expectedSquare.left.g &&
                  semantic.board[0U][0U].b == expectedSquare.left.b &&
                  semantic.board[0U][1U].r == expectedCenter.r &&
                  semantic.board[0U][1U].g == expectedCenter.g &&
                  semantic.board[0U][1U].b == expectedCenter.b &&
                  semantic.board[0U][2U].r == expectedSquare.right.r &&
                  semantic.board[0U][2U].g == expectedSquare.right.g &&
                  semantic.board[0U][2U].b == expectedSquare.right.b,
              "captured production semantic frame retains legacy V1/V2 detail projection before V3 topology expansion");
        const uint16_t base = rotated ? 63U * 4U : 0U;
        const std::array<uint8_t, 4U> offsets = rotated
            ? std::array<uint8_t, 4U>{{2U, 3U, 0U, 1U}}
            : std::array<uint8_t, 4U>{{0U, 1U, 2U, 3U}};
        for (uint8_t emitter = 0U; emitter < 4U; ++emitter) {
            const modules::Rgb expectedSemantic = expected.emitters[emitter];
            const modules::Rgb expectedPhysical =
                modules::rgb_optical_calibration::PresentForS3(
                    modules::ScaleRgbSaturatingBy256(
                        expectedSemantic,
                        modules::litchcraft_physical_presentation::CANVAS_SCALE256));
            const auto& pixel = physical.pixels[base + offsets[emitter]];
            const bool matches = pixel.r == expectedPhysical.r &&
                      pixel.g == expectedPhysical.g &&
                      pixel.b == expectedPhysical.b && pixel.w == 0U;
            Check(matches,
                  "captured production S3 pipeline preserves topology/orientation, bound, optical transform, and W=0");
        }
        Check(physical.applied_brightness_scale_65535 == 65535U &&
                  physical.applied_led_current_ma <= 216U,
              "captured production S3 pipeline uses full requested brightness within its conservative current envelope");

        // Named production journey: PauseFramesKeepsAppliedEffectsMoving.
        // Existing persistence setup bypasses file decoding only; pause enters
        // through the real protocol and presentation reaches physical mapping.
        auto& art = session.Litchcraft();
        modules::LitchcraftSession::Packet response{};
        while (art.PeekPacket(response)) art.AcceptPacket();
        std::array<uint8_t, 11U> pause{{0x4dU, 2U, 2U}};
        for (uint8_t byte = 0U; byte < 4U; ++byte) {
            pause[3U + byte] = art.SessionId() >> (byte * 8U);
        }
        pause[7U] = 2U;
        session.SendLitchcraftPacket(pause.data(), pause.size());
        bool acknowledged = false;
        while (art.PeekPacket(response)) {
            if (response.bytes[0U] == 0x4dU && response.bytes[2U] == 0x81U) {
                acknowledged = response.bytes[13U] == 0U;
            }
            art.AcceptPacket();
        }
        Check(acknowledged && !art.IsAnimationPlaying(),
              "PauseFramesKeepsAppliedEffectsMoving: pause acknowledged, frame transport stopped");
        session.Advance(125U);
        const auto continued = ComposeDocument(adopted, 137U, 262U);
        const auto& afterPause = session.SemanticFrame().board[0U][0U];
        Check(art.SelectedFrame() == 0U && art.AnimationElapsedMs() == 137U &&
                  afterPause.r == continued.emitters[0U].r &&
                  afterPause.g == continued.emitters[0U].g &&
                  afterPause.b == continued.emitters[0U].b,
              "PauseFramesKeepsAppliedEffectsMoving: paused frame retains timed Breathe guidance");
        std::printf("[JOURNEY] PauseFramesKeepsAppliedEffectsMoving rotated=%u occupancy=0 mode=Litchcraft pause=accepted frame=0 frame_ms=137 effect_ms=262 next=paint_or_select effects=continue\n",
                    static_cast<unsigned>(rotated));
    }

    Document dense = document;
    for (auto& emitter : dense.frames[0].emitters) {
        emitter = Emitter{{255U, 255U, 255U}, Effect::Off, 0U};
    }
    const ValidatedDocument denseAdopted = Adopt(dense);
    for (const bool rotated : {false, true}) {
        virtual_litch_config config{};
        virtual_litch_default_config(&config);
        config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
        config.board_rotate = rotated ? 1U : 0U;
        config.brightness_scale_256 = 256U;
        config.max_led_current_ma = 80U;
        test_support::BoundedHintEngine engine;
        runtime::VirtualLitchSession session(config, engine);
        session.ObserveBootHallMask(0U);
        session.Advance(3000U);
        EnterLitchcraftThroughProductionIngress(session);
        modules::LitchcraftPersistence::AdvancedStorage storage;
        session.ArtPersistence().Configure(
            {nullptr, &RenderBackend::Enqueue, &RenderBackend::Poll,
             &RenderBackend::Release, &RenderBackend::Read,
             &RenderBackend::EnqueueAdvanced});
        session.ArtPersistence().ConfigureAdvancedStorage(storage);
        Check(session.ArtPersistence().SetAdvancedDocument(denseAdopted, true),
              "dense VirtualLitch artwork adopts through production persistence");
        session.Advance(1U);
        const auto& physical = session.PhysicalFrame();
        const bool noWhiteDie = std::all_of(
            physical.pixels, physical.pixels + physical.led_count,
            [](const virtual_litch_color pixel) { return pixel.w == 0U; });
        Check(physical.power_limited != 0U && physical.applied_led_current_ma <= 80U &&
                  noWhiteDie,
              "dense production S3 capture remains current-capped with the RGBW white die suppressed");
    }
}

void RunStoreToStillVirtualJourney() {
    using Art = modules::LitchcraftSession;
    namespace store = modules::litchcraft_frame_store;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);

    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.brightness_scale_256 = 256U;
    test_support::BoundedHintEngine engine;
    modules::litchcraft_frame_stream::FrameStream stream;
    runtime::VirtualLitchSession board(config, engine);
    board.ObserveBootHallMask(0U);
    Check(board.Advance(3000U) == VIRTUAL_LITCH_OK,
          "StoreToStill: board boot reaches ordinary input/render loop");
    EnterLitchcraftThroughProductionIngress(board);
    modules::LitchcraftPersistence::AdvancedStorage storage;
    board.ArtPersistence().Configure(
        {nullptr, &RenderBackend::Enqueue, &RenderBackend::Poll,
         &RenderBackend::Release, &RenderBackend::Read,
         &RenderBackend::EnqueueAdvanced});
    board.ArtPersistence().ConfigureAdvancedStorage(storage);

    store::Manifest manifest{};
    manifest.frameCount = 256U;
    manifest.topology = 3U;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    store::Frame frame{};
    frame.durationMs = 50U;
    frame.emitters[0U].color = {180U, 0U, 0U};
    Check(stream.StageFirstFrame(manifest, 1U, frame),
          "StoreToStill: complete frame zero stages before presentation");
    board.Litchcraft().AttachFrameStream(&stream);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              !board.Litchcraft().IsAnimationPlaying() &&
              board.Litchcraft().FrameCount() == 256U,
          "StoreToStill: retained 256-frame Store starts paused");
    const uint8_t playStored[] = {1U, 0U, 1U, 0U, 0U, 0U};
    Check(board.Litchcraft().ApplyConnectionOrdered(
              2U, playStored, sizeof(playStored)) ==
                  modules::LitchcraftSession::Status::Ok &&
              board.Litchcraft().IsAnimationPlaying(),
          "StoreToStill: explicit Play starts retained Store");
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK,
          "StoreToStill: explicit Play reaches the physical render pipeline");
    const auto oldHash = board.PhysicalFrame().frame_hash;
    Check(board.PhysicalFrame().pixels[0U].r != 0U &&
              board.PhysicalFrame().pixels[0U].g == 0U,
          "StoreToStill: red stream frame reaches physical V3 LEDs");

    modules::litchcraft_animation::Document still{};
    still.topology = 3U;
    still.frameCount = 1U;
    still.frames[0U].durationMs = 1000U;
    still.frames[0U].emitters[0U].color = {0U, 180U, 0U};
    modules::litchcraft_animation::EncodedDocument encoded{};
    Check(modules::litchcraft_animation::EncodeDocument(Adopt(still), encoded),
          "StoreToStill: one-frame artwork encodes through schema-3 codec");
    auto& art = board.Litchcraft();
    uint16_t requestId = 2U;
    const auto send = [&](Art::BulkPacket& packet,
                          const Art::Status expected = Art::Status::Ok) {
        packet.bytes[0U] = 0x4dU;
        packet.bytes[1U] = 2U;
        for (uint8_t index = 0U; index < 4U; ++index) {
            packet.bytes[3U + index] = art.SessionId() >> (index * 8U);
        }
        const uint16_t request = requestId++;
        packet.bytes[7U] = static_cast<uint8_t>(request);
        packet.bytes[8U] = static_cast<uint8_t>(request >> 8U);
        board.SendLitchcraftPacket(packet.bytes.data(), packet.size);
        Art::Packet response{};
        bool matched = false;
        uint8_t status = 0xffU;
        while (art.PeekPacket(response)) {
            if (response.bytes[0U] == 0x4dU &&
                response.bytes[2U] == 0x81U &&
                response.bytes[11U] == static_cast<uint8_t>(request) &&
                response.bytes[12U] == static_cast<uint8_t>(request >> 8U)) {
                matched = true;
                status = response.bytes[13U];
            }
            art.AcceptPacket();
        }
        Check(matched && status == static_cast<uint8_t>(expected),
              "StoreToStill: exact replacement packet/status acknowledged");
        return request;
    };
    const auto put16 = [](Art::BulkPacket& packet, const uint16_t value) {
        packet.bytes[packet.size++] = static_cast<uint8_t>(value);
        packet.bytes[packet.size++] = static_cast<uint8_t>(value >> 8U);
    };
    const auto put32 = [&](Art::BulkPacket& packet, const uint32_t value) {
        put16(packet, static_cast<uint16_t>(value));
        put16(packet, static_cast<uint16_t>(value >> 16U));
    };
    Art::BulkPacket begin{};
    begin.bytes[2U] = 9U;
    begin.size = 9U;
    put32(begin, board.ArtPersistence().AdvancedGeneration());
    put16(begin, static_cast<uint16_t>(encoded.size));
    put32(begin, encoded.crc32);
    const uint16_t incompleteTransfer = send(begin);
    Art::BulkPacket incomplete{};
    incomplete.bytes[2U] = 11U;
    incomplete.size = 9U;
    put16(incomplete, incompleteTransfer);
    send(incomplete, Art::Status::Incomplete);
    Check(stream.Generation() == 1U &&
              board.PhysicalFrame().frame_hash == oldHash,
          "StoreToStill: incomplete Commit leaves the old stream visible");
    const uint16_t transfer = send(begin);
    for (size_t offset = 0U; offset < encoded.size;) {
        Art::BulkPacket chunk{};
        chunk.bytes[2U] = 10U;
        chunk.size = 9U;
        put16(chunk, transfer);
        put16(chunk, static_cast<uint16_t>(offset));
        const size_t count = std::min<size_t>(231U, encoded.size - offset);
        std::copy_n(encoded.bytes.begin() + offset, count,
                    chunk.bytes.begin() + chunk.size);
        chunk.size += count;
        send(chunk);
        offset += count;
    }
    Check(stream.Generation() == 1U &&
              board.PhysicalFrame().frame_hash == oldHash,
          "StoreToStill: staged replacement never displaces playing Store");
    Art::BulkPacket commit{};
    commit.bytes[2U] = 11U;
    commit.size = 9U;
    put16(commit, transfer);
    send(commit);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              stream.Generation() == 0U &&
              art.IsActive() && !art.IsAnimationPlaying() &&
              art.FrameCount() == 1U &&
              board.PhysicalFrame().frame_hash != oldHash &&
              board.PhysicalFrame().pixels[0U].r == 0U &&
              board.PhysicalFrame().pixels[0U].g != 0U,
          "StoreToStill: accepted Commit retires Store and renders static green immediately");
    const auto staticHash = board.PhysicalFrame().frame_hash;
    Check(board.Advance(75U) == VIRTUAL_LITCH_OK &&
              board.PhysicalFrame().pixels[0U].r == 0U &&
              board.PhysicalFrame().pixels[0U].g != 0U,
          "StoreToStill: old 50 ms stream deadline cannot restore red playback");
    Art::BulkPacket paint{};
    paint.bytes[2U] = 3U;
    paint.size = 9U;
    constexpr std::array<uint8_t, 9U> BLUE_PAINT{
        0U, 1U, 0U, 3U, 1U, 0U, 0U, 180U, 1U};
    std::copy(BLUE_PAINT.begin(), BLUE_PAINT.end(),
              paint.bytes.begin() + paint.size);
    paint.size += BLUE_PAINT.size();
    send(paint);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && art.IsActive() &&
              board.PhysicalFrame().pixels[4U].b != 0U,
          "StoreToStill: ordinary edit ACK changes a physical LED after handoff");
    std::printf("[JOURNEY] StoreToStillVirtualV3 occupancy=empty mode=Litchcraft "
                "before=Store256 after=Static1 ack=accepted old_hash=%016llx "
                "static_hash=%016llx edit_hash=%016llx next=edit_or_select\n",
                static_cast<unsigned long long>(oldHash),
                static_cast<unsigned long long>(staticHash),
                static_cast<unsigned long long>(board.PhysicalFrame().frame_hash));
}

void RunProductionComposition() {
    using namespace modules::litchcraft_animation;
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    const Document document = ProductionDocument();
    const ValidatedDocument adopted = Adopt(document);
    ProductionRenderFixture fixture(adopted);
    Check(!fixture.session.IsAnimationPlaying(),
          "production composition keeps saved autoplay flag inert");
    fixture.Playback(1U, 0U);
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};

    fxe::ManualMillis::SetNowMs(137U);
    fixture.session.Compose(board, status);
    const ComposedFrame phase137 = ComposeDocument(adopted, 137U);
    Check(EqualBoard(board, phase137),
          "production Compose forwards one playing elapsed sample through all eight effects");
    Check(!Equal(board[1U].left, document.frames[0].emitters[4U].color),
          "production frame zero samples a nontrivial Breathe phase rather than intended RGB");

    fixture.SelectFrame(4U);
    uint64_t frameStart = 0U;
    Check(TryFrameStartElapsed(adopted, 4U, frameStart),
          "production selected frame has a deterministic start");
    fxe::ManualMillis::SetNowMs(9876U);
    fixture.session.Compose(board, status);
    Check(EqualBoard(board, ComposeDocument(adopted, frameStart, 9876U)),
          "selected frame pauses while its effects retain continuously elapsed time");

    const auto staticFrame = ComposeDocument(adopted, frameStart, 9876U);
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> blankStatus{};
    for (const auto revision : {hardware::BoardRevision::V1C3,
                                hardware::BoardRevision::V2S3,
                                hardware::BoardRevision::V3S3}) {
        const auto physical = hardware::MapSemanticLedFrame(
            {revision}, board, blankStatus);
        const auto topology = hardware::BoardLedTopologyFor(revision);
        const auto& square = board[4U];
        const uint16_t base = static_cast<uint16_t>(4U * topology.physicalLedsPerSquare);
        const uint16_t rightOffset = revision == hardware::BoardRevision::V3S3
            ? 2U
            : static_cast<uint16_t>(topology.physicalLedsPerSquare - 1U);
        const bool railsMatch = Equal(physical[base], square.left) &&
            Equal(physical[base + rightOffset], square.right);
        Check(railsMatch,
              "production animation reaches each V1/V2/V3 topology through the existing mapper");
        if (revision == hardware::BoardRevision::V3S3) {
            Check(Equal(physical[base + 1U], square.top) &&
                      Equal(physical[base + 3U], square.bottom),
                  "V3 retains independent top/bottom animated emitters");
        } else {
            Check(Equal(physical[base + 1U], modules::RoundedPieceAverage(square)),
                  "V1/V2 apply top/bottom detail mapping after effects");
        }
    }

    auto physical = hardware::MapSemanticLedFrame(
        {hardware::BoardRevision::V3S3}, board, blankStatus);
    modules::litchcraft_physical_presentation::ApplyCanvasScale(physical, 256U);
    Check(Equal(physical[16U], modules::ScaleRgbSaturatingBy256(
              staticFrame.emitters[16U],
              modules::litchcraft_physical_presentation::CANVAS_SCALE256)),
          "animation preserves intended RGB until the established physical canvas bound");

    Document dense = document;
    for (auto& frame : dense.frames) {
        for (auto& emitter : frame.emitters) {
            emitter.effect = Effect::Bounce;
            emitter.durationMs = 2000U;
        }
    }
    const ValidatedDocument denseAdopted = Adopt(dense);
    const ComposedFrame denseFrame = ComposeDocument(denseAdopted, 777U);
    Check(denseFrame.emittersEvaluated == EMITTER_COUNT &&
              denseFrame.groupCandidatesVisited == EMITTER_COUNT * EMITTERS_PER_SQUARE,
          "S3-facing bounded measure caps dense composition at 256 emitters and four-member groups");

    constexpr size_t SAMPLES = 128U;
    std::array<uint64_t, SAMPLES> nanos{};
    fxe::ManualMillis::SetNowMs(0U);
    ProductionRenderFixture denseFixture(denseAdopted);
    denseFixture.Playback(1U, 0U);
    fxe::ManualMillis::SetNowMs(777U);
    denseFixture.session.Compose(board, status);
    Check(EqualBoard(board, denseFrame),
          "dense production session composition matches the bounded renderer result");
    for (size_t sample = 0U; sample < nanos.size(); ++sample) {
        fxe::ManualMillis::SetNowMs(static_cast<uint32_t>(sample * 17U));
        const auto started = std::chrono::steady_clock::now();
        denseFixture.session.Compose(board, status);
        const auto finished = std::chrono::steady_clock::now();
        nanos[sample] = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
    }
    std::sort(nanos.begin(), nanos.end());
    std::printf("[LITCHCRAFT_ANIMATION_BENCH] dense_8frame_256emitter p99_ns=%llu max_ns=%llu samples=%zu\n",
                static_cast<unsigned long long>(nanos[(SAMPLES * 99U) / 100U]),
                static_cast<unsigned long long>(nanos.back()), nanos.size());
}

void RunRetiredTransferRefusedWithStore() {
    using Art = modules::LitchcraftSession;
    namespace store = modules::litchcraft_frame_store;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession board(config, engine);
    board.ObserveBootHallMask(0U);
    Check(board.Advance(3000U) == VIRTUAL_LITCH_OK,
          "RetiredTransfer: V3 reaches its production input/render loop");
    EnterLitchcraftThroughProductionIngress(board);
    auto& art = board.Litchcraft();
    modules::litchcraft_frame_stream::FrameStream stream;
    store::Manifest manifest{};
    manifest.frameCount = 256U;
    manifest.active = true;
    store::Frame frame{};
    frame.emitters[0U].color = {180U, 0U, 0U};
    Check(stream.StageFirstFrame(manifest, 47U, frame),
          "RetiredTransfer: schema-4 Store frame is resident");
    art.AttachFrameStream(&stream);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              art.FrameCount() == 256U,
          "RetiredTransfer: Store owns the visible 256-frame timeline");
    Art::Packet reply{};
    while (art.PeekPacket(reply)) art.AcceptPacket();

    Art::BulkPacket begin{};
    begin.bytes[0U] = 0x4dU;
    begin.bytes[1U] = 2U;
    begin.bytes[2U] = 9U;
    for (uint8_t index = 0U; index < 4U; ++index) {
        begin.bytes[3U + index] = art.SessionId() >> (index * 8U);
    }
    begin.bytes[7U] = 29U;
    const uint16_t length = static_cast<uint16_t>(
        modules::litchcraft_animation::ENCODED_HEADER_BYTES +
        modules::litchcraft_animation::ENCODED_FRAME_BYTES);
    begin.bytes[13U] = static_cast<uint8_t>(length);
    begin.bytes[14U] = static_cast<uint8_t>(length >> 8U);
    begin.size = 19U;
    board.SendLitchcraftPacket(begin.bytes.data(), begin.size);
    bool refused = false;
    while (art.PeekPacket(reply)) {
        refused |= reply.bytes[0U] == 0x4dU && reply.bytes[2U] == 0x81U &&
            reply.bytes[11U] == 29U &&
            reply.bytes[13U] == static_cast<uint8_t>(Art::Status::Unsupported);
        art.AcceptPacket();
    }
    Check(refused && art.FrameCount() == 256U && stream.Generation() == 47U,
          "RetiredTransfer: old valid Begin is refused without disturbing Store");
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.PhysicalFrame().pixels[0U].r != 0U,
          "RetiredTransfer: schema-4 frame remains visible after refusal");
}

} // namespace

int main() {
    std::puts("[LITCHCRAFT_ANIMATION] start");
    RunGoldenVectors();
    RunSchemaBounds();
    RunMaskCoverage();
    RunTimelineAndClock();
    RunCodec();
    RunIndependentEffectEdges();
    RunAttachedStaticParity();
    RunProductionComposition();
    RunEffectsWithoutFramePlayback();
    RunVirtualS3PipelineCapture();
    RunStoreToStillVirtualJourney();
    RunRetiredTransferRefusedWithStore();
    std::printf("[LITCHCRAFT_ANIMATION] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
