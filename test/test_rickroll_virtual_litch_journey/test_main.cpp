#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/byte_order.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/litchcraft_animation.hpp"
#include "modules/litchcraft_frame_store.hpp"
#include "modules/litchcraft_frame_store_receiver.hpp"
#include "modules/litchcraft_frame_store_layout.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "modules/litchcraft_session.hpp"
#include "modules/litchcraft_wire_codec.hpp"
#include "virtual_litch_session.hpp"

#include "bouncing_balls_rgb_fixture.hpp"
#include "rickroll_rgb_fixture.hpp"

namespace store = modules::litchcraft_frame_store;
namespace stream = modules::litchcraft_frame_stream;

namespace {
int failures = 0;

void Check(const bool value, const char* const message) {
    if (value) return;
    ++failures;
    std::printf("[FAIL] %s\n", message);
}

struct Medium final {
    std::vector<uint8_t> bytes =
        std::vector<uint8_t>(store::ARTWORK_FILE_CAPACITY, 0xffU);
    bool failRootSyncOnce = false;
    bool rootWritePending = false;
    void (*accessHook)(void*, const char*, size_t, size_t) = nullptr;
    void* accessContext = nullptr;

    void Access(const char* operation, size_t offset, size_t size) {
        if (accessHook != nullptr) accessHook(accessContext, operation, offset, size);
    }

    static bool Read(void* context, const size_t offset, uint8_t* const out,
                     const size_t size) {
        auto& self = *static_cast<Medium*>(context);
        if (out == nullptr || offset > self.bytes.size() ||
            size > self.bytes.size() - offset) {
            return false;
        }
        self.Access("read", offset, size);
        std::memcpy(out, self.bytes.data() + offset, size);
        return true;
    }

    static bool Write(void* context, const size_t offset,
                      const uint8_t* const bytes, const size_t size) {
        auto& self = *static_cast<Medium*>(context);
        if (bytes == nullptr || offset > self.bytes.size() ||
            size > self.bytes.size() - offset) {
            return false;
        }
        self.Access("write", offset, size);
        std::memcpy(self.bytes.data() + offset, bytes, size);
        if (size == store::ROOT_BYTES &&
            offset < store::FrameStore::BodyOffset(0U)) {
            self.rootWritePending = true;
        }
        return true;
    }

    static bool Sync(void* context) {
        auto& self = *static_cast<Medium*>(context);
        self.Access("sync", 0U, 0U);
        if (self.rootWritePending && self.failRootSyncOnce) {
            self.failRootSyncOnce = false;
            self.rootWritePending = false;
            return false;
        }
        self.rootWritePending = false;
        return true;
    }
    store::FrameStore::Ops Ops() { return {this, &Read, &Write, &Sync}; }
};

uint8_t DecodeBase64(const char value) {
    if (value >= 'A' && value <= 'Z') return static_cast<uint8_t>(value - 'A');
    if (value >= 'a' && value <= 'z') return static_cast<uint8_t>(value - 'a' + 26);
    if (value >= '0' && value <= '9') return static_cast<uint8_t>(value - '0' + 52);
    return value == '+' ? 62U : 63U;
}

template <size_t Size>
std::array<uint8_t, Size> DecodeRgbFixture(const char* const encoded,
                                           const uint32_t expectedCrc) {
    std::array<uint8_t, Size> output{};
    size_t written = 0U;
    uint32_t accumulator = 0U;
    uint8_t bits = 0U;
    for (const char* cursor = encoded; *cursor != '\0'; ++cursor) {
        const char value = *cursor;
        if (value == '\0' || value == '=') break;
        accumulator = (accumulator << 6U) | DecodeBase64(value);
        bits = static_cast<uint8_t>(bits + 6U);
        if (bits < 8U) continue;
        bits = static_cast<uint8_t>(bits - 8U);
        if (written < output.size()) {
            output[written++] = static_cast<uint8_t>(accumulator >> bits);
        }
        accumulator &= bits == 0U ? 0U : (UINT32_C(1) << bits) - 1U;
    }
    Check(written == output.size(), "exact app RGB fixture decodes completely");
    Check(fxe::Crc32(output.data(), output.size()) == expectedCrc,
          "VirtualLitch fixture matches the exact app preset payload CRC");
    return output;
}

std::array<uint8_t, rickroll_fixture::RGB_BYTES> DecodeRgb() {
    return DecodeRgbFixture<rickroll_fixture::RGB_BYTES>(
        rickroll_fixture::RGB_BASE64, rickroll_fixture::RGB_CRC32);
}

std::array<uint8_t, bouncing_balls_fixture::RGB_BYTES>
DecodeBouncingBallsRgb() {
    return DecodeRgbFixture<bouncing_balls_fixture::RGB_BYTES>(
        bouncing_balls_fixture::RGB_BASE64,
        bouncing_balls_fixture::RGB_CRC32);
}

store::Frame MakeFrame(
    const std::array<uint8_t, rickroll_fixture::RGB_BYTES>& rgb,
    const uint16_t frameIndex) {
    store::Frame frame{};
    frame.durationMs = 50U;
    for (uint16_t emitter = 0U; emitter < store::EMITTER_COUNT; ++emitter) {
        const size_t source =
            (static_cast<size_t>(frameIndex) * store::EMITTER_COUNT + emitter) * 3U;
        const uint8_t square = static_cast<uint8_t>(emitter / 4U);
        const uint8_t column = static_cast<uint8_t>(square % 8U);
        const uint8_t side = static_cast<uint8_t>(emitter % 4U);
        const bool backdrop = (column == 0U && side == 0U) ||
                              (column == 7U && side == 2U);
        auto& target = frame.emitters[emitter];
        target.color = backdrop
            ? modules::Rgb{8U, 16U, 24U}
            : modules::Rgb{rgb[source], rgb[source + 1U], rgb[source + 2U]};
        target.effect = backdrop ? 1U : 0U;
        target.effectDurationMs = backdrop ? 1600U : 0U;
    }
    return frame;
}

store::Frame MakeBouncingBallsFrame(
    const std::array<uint8_t, bouncing_balls_fixture::RGB_BYTES>& rgb,
    const uint16_t frameIndex) {
    store::Frame frame{};
    frame.durationMs = 50U;
    for (uint16_t emitter = 0U; emitter < store::EMITTER_COUNT; ++emitter) {
        const size_t source =
            (static_cast<size_t>(frameIndex) * store::EMITTER_COUNT + emitter) * 3U;
        frame.emitters[emitter].color = {
            rgb[source], rgb[source + 1U], rgb[source + 2U]};
    }
    return frame;
}

bool SameFrame(const store::Frame& left, const store::Frame& right) {
    if (left.durationMs != right.durationMs) return false;
    for (size_t index = 0U; index < left.emitters.size(); ++index) {
        const auto& a = left.emitters[index];
        const auto& b = right.emitters[index];
        if (a.color.r != b.color.r || a.color.g != b.color.g ||
            a.color.b != b.color.b || a.effect != b.effect ||
            a.effectDurationMs != b.effectDurationMs) {
            return false;
        }
    }
    return true;
}

stream::FrameStream::ServiceResult ServiceOne(
    stream::FrameStream& pager, store::FrameStore& frames) {
    stream::FrameStream::ServiceResult result{};
    do {
        result = pager.Service(frames, false);
    } while (result == stream::FrameStream::ServiceResult::Loading);
    return result;
}

store::Frame MakeNormalFrame(const uint16_t frameIndex) {
    store::Frame frame{};
    frame.durationMs = 50U;
    // Four distinct directional markers on d5; every frame has a different
    // red/green level. Sparse asymmetric artwork exposes stale output and
    // wrong orientation without depending on whole-board power limiting.
    frame.emitters[108U].color = {static_cast<uint8_t>(64U + frameIndex * 4U), 0U, 0U};
    frame.emitters[109U].color = {0U, static_cast<uint8_t>(96U + frameIndex * 4U), 0U};
    frame.emitters[110U].color = {0U, 0U, static_cast<uint8_t>(160U + frameIndex * 2U)};
    frame.emitters[111U].color = {80U, 80U, 80U};
    return frame;
}

void CheckNormalPhysicalFrame(runtime::VirtualLitchSession& board,
                              const uint16_t frame, const char* checkpoint) {
    const auto& out = board.PhysicalFrame();
    const auto& red = out.pixels[108U];
    const auto& green = out.pixels[109U];
    const auto& blue = out.pixels[110U];
    const auto& neutral = out.pixels[111U];
    // Contract: half-scale canvas, S3 blue-chroma gain 144/256, no white
    // extraction in Litchcraft. These expected channels are independent of
    // the production mapping/composition helpers under test.
    const uint8_t expectedBlue = static_cast<uint8_t>(
        ((80U + frame) * 144U + 128U) / 256U);
    size_t lit = 0U;
    for (size_t index = 0U; index < 256U; ++index) {
        const auto& pixel = out.pixels[index];
        if (pixel.r || pixel.g || pixel.b || pixel.w) ++lit;
    }
    const bool exact = out.led_count == 288U && out.channels_per_led == 4U &&
        red.r == 32U + frame * 2U && red.g == 0U && red.b == 0U && red.w == 0U &&
        green.r == 0U && green.g == 48U + frame * 2U && green.b == 0U && green.w == 0U &&
        blue.r == 0U && blue.g == 0U && blue.b == expectedBlue && blue.w == 0U &&
        neutral.r == 40U && neutral.g == 40U && neutral.b == 40U && neutral.w == 0U &&
        lit == 4U;
    std::printf("[NORMAL_STORE_CHECKPOINT] %s occupancy=empty mode=%s "
                "selected=%u playing=%u expected_frame=%u "
                "d5_rgbw=%u/%u/%u/%u,%u/%u/%u/%u,%u/%u/%u/%u,%u/%u/%u/%u "
                "lit=%zu scale=%u hash=%016llx result=%s\n",
                checkpoint, board.Litchcraft().IsActive() ? "Litchcraft" : "inactive",
                board.Litchcraft().SelectedFrame(), board.Litchcraft().IsAnimationPlaying(), frame,
                red.r,red.g,red.b,red.w, green.r,green.g,green.b,green.w,
                blue.r,blue.g,blue.b,blue.w, neutral.r,neutral.g,neutral.b,neutral.w,
                lit, out.applied_brightness_scale_65535,
                static_cast<unsigned long long>(out.frame_hash), exact ? "pass" : "FAIL");
    Check(exact, "normal Store frame reaches exact integrated RGBW output");
}

std::array<uint8_t, 8U> LegacyRequest(const modules::LitchcraftSession::Op op,
                                      const uint32_t session,
                                      const uint16_t request) {
    return {modules::LitchcraftSession::MAGIC, static_cast<uint8_t>(op),
            static_cast<uint8_t>(session), static_cast<uint8_t>(session >> 8U),
            static_cast<uint8_t>(session >> 16U), static_cast<uint8_t>(session >> 24U),
            static_cast<uint8_t>(request), static_cast<uint8_t>(request >> 8U)};
}

std::array<uint8_t, 16U> RuntimeRequest(const uint32_t session,
                                        const uint16_t request,
                                        const uint32_t generation,
                                        const uint8_t action,
                                        const uint16_t frame) {
    return {0x4dU, 2U, 23U,
            static_cast<uint8_t>(session), static_cast<uint8_t>(session >> 8U),
            static_cast<uint8_t>(session >> 16U), static_cast<uint8_t>(session >> 24U),
            static_cast<uint8_t>(request), static_cast<uint8_t>(request >> 8U),
            static_cast<uint8_t>(generation), static_cast<uint8_t>(generation >> 8U),
            static_cast<uint8_t>(generation >> 16U), static_cast<uint8_t>(generation >> 24U),
            action, static_cast<uint8_t>(frame), static_cast<uint8_t>(frame >> 8U)};
}

bool DrainStoreRuntimeReceipt(modules::LitchcraftSession& art,
                              const uint16_t request, const bool playing,
                              const uint16_t selected,
                              const uint8_t status = 0U) {
    modules::LitchcraftSession::Packet packet{};
    bool found = false;
    while (art.PeekPacket(packet)) {
        if (packet.size == 19U && packet.bytes[0] == 0x4dU &&
            packet.bytes[2] == 0x8dU &&
            (static_cast<uint16_t>(packet.bytes[11]) |
             (static_cast<uint16_t>(packet.bytes[12]) << 8U)) == request) {
            const uint16_t count = static_cast<uint16_t>(packet.bytes[15]) |
                (static_cast<uint16_t>(packet.bytes[16]) << 8U);
            const uint16_t actualSelected = static_cast<uint16_t>(packet.bytes[17]) |
                (static_cast<uint16_t>(packet.bytes[18]) << 8U);
            found = packet.bytes[13] == status && count == 32U &&
                actualSelected == selected &&
                ((packet.bytes[14] & 2U) != 0U) == playing;
        }
        art.AcceptPacket();
    }
    return found;
}

void CheckDirectionalGeometry(const store::Frame& frame,
                              const uint16_t frameIndex) {
    modules::BoardLedFrame semantic{};
    for (uint8_t square = 0U; square < 64U; ++square) {
        std::array<store::Emitter, 4U> stored{};
        for (uint8_t side = 0U; side < 4U; ++side) {
            stored[side] = frame.emitters[square * 4U + side];
        }
        const auto composed = modules::litchcraft_animation::ComposeStoredSquare(
            stored, static_cast<uint64_t>(frameIndex) * 50U, 1U,
            frameIndex, static_cast<uint16_t>(square * 4U));
        semantic[square] = {
            composed[0], composed[1], composed[2], composed[3]};
    }
    const auto physical = hardware::MapSemanticBoardFrame(
        {hardware::BoardRevision::V3S3, false, false, 0ULL}, semantic);
    for (uint16_t emitter = 0U; emitter < 256U; ++emitter) {
        const auto& expected = semantic[emitter / 4U];
        const modules::Rgb roles[4] = {
            expected.left, expected.top, expected.right, expected.bottom};
        const auto actual = physical[emitter];
        const auto role = roles[emitter % 4U];
        if (actual.r != role.r || actual.g != role.g || actual.b != role.b) {
            Check(false, "V3 physical geometry preserves left/top/right/bottom");
            return;
        }
    }
}

void RickrollStreamTraversesVirtualLitchV3() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    const auto rgb = DecodeRgb();

    Medium medium;
    store::FrameStore frames(medium.Ops());
    Check(frames.Recover() == store::Status::NoArtwork,
          "Rickroll fixture starts from blank frame storage");
    store::Manifest manifest{};
    manifest.frameCount = 32U;
    manifest.topology = 3U;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    manifest.sparkleSeed = 1U;
    store::TransactionToken token{};
    Check(frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "Rickroll schema-4 transaction begins");
    std::array<store::Frame, 5U> checkpoints{};
    constexpr std::array<uint16_t, 5U> CHECKPOINTS{0U, 7U, 15U, 23U, 31U};
    for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
        const auto frame = MakeFrame(rgb, index);
        Check(frames.WriteFrame(token, index, frame) == store::Status::Ok,
              "all 32 exact Rickroll frames enter the bounded frame store");
        for (size_t checkpoint = 0U; checkpoint < CHECKPOINTS.size(); ++checkpoint) {
            if (CHECKPOINTS[checkpoint] == index) checkpoints[checkpoint] = frame;
        }
    }
    Check(frames.Commit(token) == store::Status::Ok,
          "Rickroll schema-4 transaction publishes one complete root");

    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()),
          "authoritative stream adopts committed Rickroll root");
    Check(ServiceOne(pager, frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested &&
          ServiceOne(pager, frames) ==
              stream::FrameStream::ServiceResult::PrefetchedNext,
          "worker fully loads frame zero and prefetches frame one");

    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.brightness_scale_256 = 256U;
    config.max_led_current_ma = 12000U;
    config.strong_power_source = 1U;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(&pager);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.Litchcraft().IsActive() &&
              board.PhysicalFrame().led_count == 288U &&
              board.PhysicalFrame().channels_per_led == 4U,
          "frame zero reaches production VirtualLitch V3 RGBW output");
    const uint8_t playStored[] = {1U, 0U, 1U, 0U, 0U, 0U};
    Check(!board.Litchcraft().IsAnimationPlaying() &&
              board.Litchcraft().ApplyConnectionOrdered(
                  2U, playStored, sizeof(playStored)) ==
                  modules::LitchcraftSession::Status::Ok,
          "Rickroll stays paused until explicit Play");

    std::array<uint64_t, CHECKPOINTS.size()> physicalHashes{};
    physicalHashes[0] = board.PhysicalFrame().frame_hash;
    CheckDirectionalGeometry(checkpoints[0], 0U);
    size_t nextCheckpoint = 1U;
    for (uint16_t frame = 1U; frame < manifest.frameCount; ++frame) {
        Check(board.Advance(50U) == VIRTUAL_LITCH_OK &&
                  board.Litchcraft().SelectedFrame() == frame,
              "each 50 ms boundary publishes its prefetched complete frame");
        if (nextCheckpoint < CHECKPOINTS.size() &&
            CHECKPOINTS[nextCheckpoint] == frame) {
            physicalHashes[nextCheckpoint] = board.PhysicalFrame().frame_hash;
            CheckDirectionalGeometry(checkpoints[nextCheckpoint], frame);
            ++nextCheckpoint;
        }
        if (frame + 1U < manifest.frameCount) {
            Check(ServiceOne(pager, frames) ==
                      stream::FrameStream::ServiceResult::PrefetchedNext,
                  "bounded storage service prefetches the following frame");
        }
        if (frame == 15U) {
            Check(board.SetButtonDown(1U,
                      runtime::VirtualLitchPhysicalButton::Up, true) ==
                      VIRTUAL_LITCH_OK &&
                  board.SetButtonDown(1U,
                      runtime::VirtualLitchPhysicalButton::Up, false) ==
                      VIRTUAL_LITCH_OK,
                  "physical input remains admitted during streamed playback");
        }
    }
    Check(nextCheckpoint == CHECKPOINTS.size(),
          "frame 0 and four later boundary frames traverse VirtualLitch");
    Check(std::all_of(physicalHashes.begin(), physicalHashes.end(),
                      [](const uint64_t hash) { return hash != 0U; }),
          "selected physical-output frames have deterministic nonzero hashes");
    for (size_t index = 1U; index < physicalHashes.size(); ++index) {
        Check(physicalHashes[index] != physicalHashes[index - 1U],
              "distinct Rickroll checkpoint images change actual RGBW output");
    }

    const uint8_t lastSelected = board.Litchcraft().SelectedFrame();
    Check(lastSelected != 0U && board.Litchcraft().IsAnimationPlaying(),
          "Rickroll is playing a later Store frame before root loss");
    Check(pager.RetireDocument(), "stream retirement accepts cancellation");
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK,
          "stream retirement returns on the same production input turn");
    Check(pager.Generation() == 0U && pager.FrameCount() == 0U,
          "stream retirement removes Store authority immediately");
    Check(board.Litchcraft().FrameCount() == 0U &&
              board.Litchcraft().SelectedFrame() == 0U &&
              !board.Litchcraft().IsAnimationPlaying(),
          "stream retirement immediately stops and clears the selected runtime frame");
    Check(std::all_of(std::begin(board.SemanticFrame().board),
                      std::end(board.SemanticFrame().board),
                      [](const auto& square) {
                          return std::all_of(std::begin(square), std::end(square),
                              [](const auto color) {
                                  return color.r == 0U && color.g == 0U && color.b == 0U;
                              });
                      }),
          "retired Store presents blank artwork instead of a stale animated frame");
    Check(board.SetButtonDown(1U, runtime::VirtualLitchPhysicalButton::Right,
                              true) == VIRTUAL_LITCH_OK,
          "retirement immediately admits the next physical press");
    Check(board.SetButtonDown(1U, runtime::VirtualLitchPhysicalButton::Right,
                              false) == VIRTUAL_LITCH_OK,
          "retirement immediately admits the matching physical release");
    const auto statusEnergy = [&]() {
        unsigned energy = 0U;
        for (const auto color : board.SemanticFrame().status) {
            energy += color.r + color.g + color.b;
        }
        return energy;
    };
    Check(statusEnergy() == 0U,
          "stream retirement starts the paused corner fade from settled black");
    Check(board.Advance(125U) == VIRTUAL_LITCH_OK && statusEnergy() != 0U,
          "retired stream restores the paint cue partway after 125 ms");
    const unsigned halfwayEnergy = statusEnergy();
    Check(board.Advance(125U) == VIRTUAL_LITCH_OK &&
              statusEnergy() > halfwayEnergy &&
              !board.Litchcraft().IsAnimationPlaying() &&
              board.Litchcraft().FrameCount() == 0U,
          "retired stream restores paint corners after 250 ms without reviving authority");
    auto paintPreview = modules::ScaleRgbSaturatingBy256(
        board.Litchcraft().Color(), 48U);
    if (paintPreview.r == 0U && paintPreview.g == 0U && paintPreview.b == 0U) {
        paintPreview = {12U, 12U, 12U};
    }
    for (size_t led = 0U; led < modules::EDGE_LED_COUNT; ++led) {
        const bool corner = led % 8U == 0U || led % 8U == 7U;
        const auto color = board.SemanticFrame().status[led];
        Check(color.r == (corner ? paintPreview.r : 0U) &&
                  color.g == (corner ? paintPreview.g : 0U) &&
                  color.b == (corner ? paintPreview.b : 0U),
              "retired stream settles exactly on the current paint color at eight corners");
    }
    std::printf("[RICKROLL_VIRTUAL_LITCH] root-loss selected=%u playing=0 "
                "frames=0 board=blank status=tool-preview next=restore-Store\n",
                lastSelected);

    std::printf(
        "[RICKROLL_VIRTUAL_LITCH] frames=32 duration_ms=50 emitters=256 "
        "physical_leds=288 hashes=%016llx,%016llx,%016llx,%016llx,%016llx "
        "return=nonblocking\n",
        static_cast<unsigned long long>(physicalHashes[0]),
        static_cast<unsigned long long>(physicalHashes[1]),
        static_cast<unsigned long long>(physicalHashes[2]),
        static_cast<unsigned long long>(physicalHashes[3]),
        static_cast<unsigned long long>(physicalHashes[4]));
}

void BouncingBalls256TraversesVirtualLitchV3() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    const auto rgb = DecodeBouncingBallsRgb();

    Medium medium;
    store::FrameStore frames(medium.Ops());
    Check(frames.Recover() == store::Status::NoArtwork,
          "Balls256 fixture starts from blank frame storage");
    store::Manifest manifest{};
    manifest.frameCount = 256U;
    manifest.topology = 3U;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    manifest.sparkleSeed = 1U;
    store::TransactionToken token{};
    Check(frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "Balls256 normal schema-4 transaction begins");
    constexpr std::array<uint16_t, 7U> CHECKPOINTS{
        0U, 57U, 117U, 137U, 197U, 217U, 255U};
    std::array<store::Frame, CHECKPOINTS.size()> checkpoints{};
    for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
        const auto frame = MakeBouncingBallsFrame(rgb, index);
        Check(frames.WriteFrame(token, index, frame) == store::Status::Ok,
              "all 256 exact Balls frames enter the normal frame store");
        for (size_t checkpoint = 0U; checkpoint < CHECKPOINTS.size(); ++checkpoint) {
            if (CHECKPOINTS[checkpoint] == index) checkpoints[checkpoint] = frame;
        }
    }
    Check(frames.Commit(token) == store::Status::Ok,
          "Balls256 schema-4 transaction publishes one complete root");
    Check(SameFrame(checkpoints.front(), checkpoints.back()),
          "Balls256 stored frame 255 exactly equals stored frame zero");

    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()),
          "authoritative stream adopts committed Balls256 root");
    Check(ServiceOne(pager, frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested &&
          ServiceOne(pager, frames) ==
              stream::FrameStream::ServiceResult::PrefetchedNext,
          "worker loads Balls256 frame zero and prefetches frame one");

    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.brightness_scale_256 = 256U;
    config.max_led_current_ma = 12000U;
    config.strong_power_source = 1U;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(&pager);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.Litchcraft().IsActive() &&
              board.PhysicalFrame().led_count == 288U,
          "Balls256 frame zero reaches production VirtualLitch V3 output");
    const uint8_t playStored[] = {1U, 0U, 1U, 0U, 0U, 0U};
    Check(!board.Litchcraft().IsAnimationPlaying() &&
              board.Litchcraft().ApplyConnectionOrdered(
                  2U, playStored, sizeof(playStored)) ==
                  modules::LitchcraftSession::Status::Ok,
          "Balls256 stays paused until explicit Play");

    std::array<uint64_t, CHECKPOINTS.size()> physicalHashes{};
    physicalHashes[0] = board.PhysicalFrame().frame_hash;
    CheckDirectionalGeometry(checkpoints[0], 0U);
    size_t nextCheckpoint = 1U;
    for (uint16_t frame = 1U; frame < manifest.frameCount; ++frame) {
        Check(board.Advance(50U) == VIRTUAL_LITCH_OK &&
                  board.Litchcraft().SelectedFrame() == frame,
              "each Balls256 boundary publishes one complete stored frame");
        if (nextCheckpoint < CHECKPOINTS.size() &&
            CHECKPOINTS[nextCheckpoint] == frame) {
            physicalHashes[nextCheckpoint] = board.PhysicalFrame().frame_hash;
            CheckDirectionalGeometry(checkpoints[nextCheckpoint], frame);
            ++nextCheckpoint;
        }
        if (frame + 1U < manifest.frameCount) {
            Check(ServiceOne(pager, frames) ==
                      stream::FrameStream::ServiceResult::PrefetchedNext,
                  "bounded worker prefetches the next Balls256 frame");
        }
        if (frame == 128U) {
            Check(board.SetButtonDown(1U,
                      runtime::VirtualLitchPhysicalButton::Up, true) ==
                      VIRTUAL_LITCH_OK &&
                  board.SetButtonDown(1U,
                      runtime::VirtualLitchPhysicalButton::Up, false) ==
                      VIRTUAL_LITCH_OK,
                  "physical input remains admitted during Balls256 playback");
        }
    }
    Check(nextCheckpoint == CHECKPOINTS.size(),
          "normal playback reaches every Balls256 checkpoint");
    for (size_t index = 1U; index + 1U < physicalHashes.size(); ++index) {
        Check(physicalHashes[index] != physicalHashes[index - 1U],
              "Balls256 motion and starbursts change physical RGBW output");
    }
    Check(board.Advance(50U) == VIRTUAL_LITCH_OK &&
              board.Litchcraft().SelectedFrame() == 255U,
          "Balls256 holds its exact loop endpoint while wrapped frame zero loads");
    Check(ServiceOne(pager, frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested &&
              board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.Litchcraft().SelectedFrame() == 0U,
          "bounded worker publishes wrapped Balls256 frame zero");
    const uint64_t wrappedFrameZeroHash = board.PhysicalFrame().frame_hash;
    Check(physicalHashes.back() == wrappedFrameZeroHash,
          "Balls256 steady-state physical output closes exactly across 255 to zero");
    std::printf(
        "[BALLS256_VIRTUAL_LITCH] frames=256 duration_ms=50 emitters=256 "
        "physical_leds=288 loop=exact input=nonblocking "
        "hashes=%016llx,%016llx,%016llx,%016llx,%016llx,%016llx,%016llx,%016llx\n",
        static_cast<unsigned long long>(physicalHashes[0]),
        static_cast<unsigned long long>(physicalHashes[1]),
        static_cast<unsigned long long>(physicalHashes[2]),
        static_cast<unsigned long long>(physicalHashes[3]),
        static_cast<unsigned long long>(physicalHashes[4]),
        static_cast<unsigned long long>(physicalHashes[5]),
        static_cast<unsigned long long>(physicalHashes[6]),
        static_cast<unsigned long long>(wrappedFrameZeroHash));
}

void NormalStorePlaybackControlsTraverseVirtualLitchV3() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);

    Medium medium;
    store::FrameStore frames(medium.Ops());
    Check(frames.Recover() == store::Status::NoArtwork,
          "normal Store journey starts with no persisted artwork");
    store::Manifest manifest{};
    manifest.frameCount = 32U;
    manifest.topology = 3U;
    manifest.active = true;
    // A normal Play command, not a diagnostic/preset entry shortcut, starts it.
    manifest.playbackOnEntry = false;
    manifest.sparkleSeed = 17U;
    store::TransactionToken token{};
    Check(frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "normal 32-frame Store transaction begins");
    for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
        Check(frames.WriteFrame(token, index, MakeNormalFrame(index)) ==
                  store::Status::Ok,
              "normal Store transaction admits every frame");
    }
    Check(frames.Commit(token) == store::Status::Ok,
          "normal 32-frame Store transaction commits one authority root");

    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()),
          "normal Store root configures the production pager");
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.brightness_scale_256 = 256U;
    config.max_led_current_ma = 12000U;
    config.strong_power_source = 1U;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    auto& art = board.Litchcraft();
    art.BeginConnection();
    const auto enter = LegacyRequest(modules::LitchcraftSession::Op::Enter,
                                     0U, 1U);
    board.SendLitchcraftPacket(enter.data(), enter.size());
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && art.IsActive(),
          "normal Litchcraft Enter owns the board before Store playback");
    modules::LitchcraftSession::Packet ignored{};
    while (art.PeekPacket(ignored)) art.AcceptPacket();
    art.AttachFrameStream(&pager);

    Check(ServiceOne(pager, frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested &&
              board.Advance(0U) == VIRTUAL_LITCH_OK &&
              art.SelectedFrame() == 0U &&
              board.PhysicalFrame().frame_hash != 0U,
          "committed frame zero reaches optical V3 output without a render stall");
    Check(ServiceOne(pager, frames) ==
              stream::FrameStream::ServiceResult::PrefetchedNext,
          "worker prefetches the next normal Store frame");
    Check(board.Advance(1000U) == VIRTUAL_LITCH_OK,
          "normal Store frame settles after ordinary Litchcraft entry");
    CheckNormalPhysicalFrame(board, 0U, "paused-entry");

    const auto play = RuntimeRequest(art.SessionId(), 2U, frames.Generation(), 1U, 0U);
    board.SendLitchcraftPacket(play.data(), play.size());
    Check(DrainStoreRuntimeReceipt(art, 2U, true, 0U) &&
              board.Advance(0U) == VIRTUAL_LITCH_OK,
          "normal Play returns deterministic Store state");
    const auto advanced = board.Advance(50U);
    Check(advanced == VIRTUAL_LITCH_OK && art.SelectedFrame() == 1U &&
              board.SemanticFrame().board[27][0].r == 68U,
          "normal Play advances the V3 compositor source exactly at 50 ms");
    CheckNormalPhysicalFrame(board, 1U, "play-boundary");
    const auto pause = RuntimeRequest(art.SessionId(), 3U, frames.Generation(), 0U, 1U);
    board.SendLitchcraftPacket(pause.data(), pause.size());
    Check(DrainStoreRuntimeReceipt(art, 3U, false, 1U) &&
              board.Advance(0U) == VIRTUAL_LITCH_OK,
          "normal Pause returns a deterministic paused Store state");
    Check(board.Advance(250U) == VIRTUAL_LITCH_OK && art.SelectedFrame() == 1U &&
              board.SemanticFrame().board[27][0].r == 68U,
          "Pause holds the same optical frame beyond multiple 50 ms boundaries");
    CheckNormalPhysicalFrame(board, 1U, "pause-held-250ms");
    const auto select = RuntimeRequest(art.SessionId(), 4U, frames.Generation(), 2U, 29U);
    board.SendLitchcraftPacket(select.data(), select.size());
    Check(DrainStoreRuntimeReceipt(art, 4U, false, 29U) &&
              ServiceOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              board.Advance(0U) == VIRTUAL_LITCH_OK && art.SelectedFrame() == 29U,
          "normal Select seeks a Store frame through its generation-fenced receipt");
    CheckNormalPhysicalFrame(board, 29U, "select-late-frame");
    const auto stale = RuntimeRequest(art.SessionId(), 5U,
                                      frames.Generation() + 1U, 2U, 7U);
    board.SendLitchcraftPacket(stale.data(), stale.size());
    Check(DrainStoreRuntimeReceipt(art, 5U, false, 29U, 9U) &&
              board.Advance(0U) == VIRTUAL_LITCH_OK,
          "stale-generation Select is rejected without changing selection");
    CheckNormalPhysicalFrame(board, 29U, "stale-select-rejected-next=resume");
    Check(ServiceOne(pager, frames) ==
              stream::FrameStream::ServiceResult::PrefetchedNext,
          "selected Store frame prefetches its normal playback successor");
    const auto resume = RuntimeRequest(art.SessionId(), 6U, frames.Generation(), 1U, 29U);
    board.SendLitchcraftPacket(resume.data(), resume.size());
    Check(DrainStoreRuntimeReceipt(art, 6U, true, 29U) &&
              board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.Advance(50U) == VIRTUAL_LITCH_OK && art.SelectedFrame() == 30U,
          "Play resumes from the paused frame through the normal control path");
    CheckNormalPhysicalFrame(board, 30U, "resume-boundary");

    const auto exit = LegacyRequest(modules::LitchcraftSession::Op::Exit,
                                    art.SessionId(), 7U);
    board.SendLitchcraftPacket(exit.data(), exit.size());
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && !art.IsActive(),
          "normal Exit stops Store playback without retiring its document");
    Check(board.Advance(300U) == VIRTUAL_LITCH_OK &&
              !art.IsAnimationPlaying() && pager.FrameCount() == 32U &&
              board.PhysicalFrame().pixels[108U].r != 92U,
          "Exit expires the canvas presentation and preserves stored artwork");
    std::printf("[NORMAL_STORE_CHECKPOINT] exit occupancy=empty mode=inactive "
                "playing=0 stored_frames=32 canvas=retired next=Enter\n");
    art.EndConnection();
    art.BeginConnection();
    store::FrameStore restored(medium.Ops());
    Check(restored.Recover() == store::Status::Ok &&
              pager.Configure(restored.CurrentManifest(), restored.Generation()),
          "reconnect restores the same committed Store root from persistence");
    const auto reenter = LegacyRequest(modules::LitchcraftSession::Op::Enter,
                                       0U, 1U);
    board.SendLitchcraftPacket(reenter.data(), reenter.size());
    Check(ServiceOne(pager, restored) ==
              stream::FrameStream::ServiceResult::LoadedRequested &&
              board.Advance(0U) == VIRTUAL_LITCH_OK && art.IsActive() &&
              art.SelectedFrame() == 0U && !art.IsAnimationPlaying(),
          "reconnect restores paused frame zero with no parallel document owner");
    Check(board.Advance(250U) == VIRTUAL_LITCH_OK, "reentry presentation settles");
    CheckNormalPhysicalFrame(board, 0U, "reconnect-paused-entry");
    const auto replay = RuntimeRequest(art.SessionId(), 2U, restored.Generation(), 1U, 0U);
    board.SendLitchcraftPacket(replay.data(), replay.size());
    Check(DrainStoreRuntimeReceipt(art, 2U, true, 0U) &&
              board.Advance(0U) == VIRTUAL_LITCH_OK,
          "restored normal document restarts through the same Play receipt");
    Check(ServiceOne(pager, restored) ==
              stream::FrameStream::ServiceResult::PrefetchedNext &&
              board.Advance(50U) == VIRTUAL_LITCH_OK,
          "restored normal document presents its next complete frame");
    CheckNormalPhysicalFrame(board, 1U, "reconnect-play-boundary");
    Check(ServiceOne(pager, restored) ==
              stream::FrameStream::ServiceResult::PrefetchedNext,
          "worker prefetches frame two before the physical paint boundary");
    const uint8_t beforePaint = board.SemanticFrame().board[9U][0U].r;
    Check(board.ObserveHallMask(UINT64_C(1) << 9U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              pager.HasUnpersistedEdit() &&
              board.SemanticFrame().board[9U][0U].r != beforePaint,
          "raw Hall square9 paints the playing frame through the V3 output");
    Check(board.Advance(50U) == VIRTUAL_LITCH_OK &&
              art.SelectedFrame() == 1U && pager.DisplayedIndex() == 1U &&
              !art.IsAnimationPlaying(),
          "physical paint pauses animation on the edited frame");
    std::printf("[NORMAL_STORE_CHECKPOINT] occupancy=square9 input=Hall-paint "
                "mode=paused frame=1 result=accepted LEDs=painted "
                "next=save-or-Play expiry=until-explicit-Play\n");
    const auto paintSave = pager.Service(restored, false);
    Check(paintSave != stream::FrameStream::ServiceResult::IoFailure &&
              board.Advance(0U) == VIRTUAL_LITCH_OK &&
              art.SelectedFrame() == 1U && !pager.HasUnpersistedEdit(),
          "durable save preserves the paused painted frame");
    store::FrameStore afterPaint(medium.Ops());
    store::Frame savedPaint{};
    Check(afterPaint.Recover() == store::Status::Ok &&
              afterPaint.ReadFrame(1U, savedPaint) == store::Status::Ok &&
              savedPaint.emitters[36U].color.r != beforePaint,
          "reboot recovers the exact frame painted before animation resumed");
    std::printf("[NORMAL_STORE_VIRTUAL_LITCH] frames=32 play=50ms "
                "pause_resume=reconnect_restore nonblocking=yes\n");
}

void StoreBrushCheckpoint(runtime::VirtualLitchSession& board,
                          const char* stage, const char* result,
                          const char* next, const char* expiry) {
    const auto display = board.DisplayText();
    const auto source = board.SemanticFrame().board[4U][0U];
    const auto destination = board.SemanticFrame().board[5U][0U];
    size_t litStatus = 0U;
    for (const auto led : board.SemanticFrame().status) {
        if (led.r != 0U || led.g != 0U || led.b != 0U) ++litStatus;
    }
    std::printf("[STORE_BRUSH_PICK_VIRTUAL_LITCH] stage=%s t=%u occupancy=%016llx "
                "mode=%s result=%s display=\"%.*s\" source=%u,%u,%u "
                "destination=%u,%u,%u status_lit=%zu next=%s expiry=%s\n",
                stage, board.State().now_ms,
                static_cast<unsigned long long>(board.Litchcraft().Occupancy()),
                board.Litchcraft().IsActive() ? "Litchcraft" : "Setup/Play",
                result, static_cast<int>(display.size()),
                display.empty() ? "" : display.data(),
                source.r, source.g, source.b,
                destination.r, destination.g, destination.b,
                litStatus, next, expiry);
}

void FreshBlankStorePaintTraversesVirtualLitchV3() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.RecoverOrCreateBlank({1U, 0U, 3U, true, false}) ==
              store::Status::Ok &&
              frames.Generation() == 1U &&
              frames.CurrentManifest().frameCount == 1U,
          "fresh Store creates exactly one blank schema-4 document");
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()),
          "fresh blank frame configures the production Store owner");

    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.brightness_scale_256 = 256U;
    config.max_led_current_ma = 12000U;
    config.strong_power_source = 1U;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    auto& art = board.Litchcraft();
    modules::LitchcraftSession::Canvas retired{};
    retired.fill({99U, 0U, 0U});
    art.RestoreArtwork(retired, true, 0U, nullptr, true);
    art.AttachFrameStream(&pager);
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK && art.IsActive() &&
              board.SemanticFrame().board[9U][0U].r == 0U,
          "unpaged Store hides retained static artwork during first render");
    StoreBrushCheckpoint(board, "unpaged-first-frame", "pending",
                         "wait-for-first-frame", "none");
    Check(ServiceOne(pager, frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested,
          "fresh blank frame pages through the production Store owner");
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK && art.IsActive() &&
              board.SemanticFrame().board[9U][0U].r == 0U,
          "new blank Store hides retained static artwork on the V3 canvas");
    StoreBrushCheckpoint(board, "fresh-blank", "accepted", "place-square9", "none");
    Check(board.ObserveHallMask(UINT64_C(1) << 9U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[9U][0U].r != 0U &&
              pager.HasUnpersistedEdit(),
          "first physical placement paints the new Store frame immediately");
    StoreBrushCheckpoint(board, "first-placement", "accepted", "lift-or-paint", "5s-save");
}

void MissingStoreRootCannotUseRetiredCanvasOnVirtualLitchV3() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    auto& art = board.Litchcraft();
    modules::LitchcraftSession::Canvas retired{};
    retired.fill({99U, 0U, 0U});
    art.RestoreArtwork(retired, true, 0U, nullptr, true);
    stream::FrameStream pager;
    art.AttachFrameStream(&pager);
    art.BeginConnection();
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK && art.IsActive() &&
              art.FrameCount() == 0U && art.UndoCount() == 0U &&
              art.RedoCount() == 0U &&
              board.SemanticFrame().board[9U][0U].r == 0U,
          "missing Store root cannot render retired static artwork");
    const auto refusedRequest = [&](const uint16_t request) {
        modules::LitchcraftSession::Packet reply{};
        bool refused = false;
        while (art.PeekPacket(reply)) {
            if (reply.size >= 13U && reply.bytes[0] ==
                    modules::LitchcraftSession::MAGIC &&
                reply.bytes[1] == 0x81U &&
                (static_cast<uint16_t>(reply.bytes[10]) |
                 (static_cast<uint16_t>(reply.bytes[11]) << 8U)) == request) {
                refused = reply.bytes[12] == static_cast<uint8_t>(
                    modules::LitchcraftSession::Status::Unsupported);
            }
            art.AcceptPacket();
        }
        return refused;
    };
    const auto clear = LegacyRequest(modules::LitchcraftSession::Op::Clear,
                                     art.SessionId(), 1U);
    art.HandlePacket(clear.data(), clear.size(), true, 0U);
    Check(refusedRequest(1U) && art.Artwork()[9U * 4U].r == 99U,
          "missing Store root refuses old Clear without mutating retired art");
    std::array<uint8_t, 15U> bulk{};
    bulk[0] = modules::LitchcraftSession::MAGIC;
    bulk[1] = modules::LitchcraftSession::BULK_REPLACE;
    fxe::WriteU32LE(art.SessionId(), bulk.data() + 2U);
    fxe::WriteU16LE(2U, bulk.data() + 6U);
    bulk[10] = 1U;
    bulk[11] = 1U;
    bulk[12] = 0U;
    bulk[13] = 180U;
    bulk[14] = 0U;
    art.HandleBulkPacket(bulk.data(), bulk.size(), true, 0U);
    Check(refusedRequest(2U) && art.Artwork()[9U * 4U].r == 99U,
          "missing Store root refuses old bulk replacement");
    Check(board.ObserveHallMask(UINT64_C(1) << 9U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[9U][0U].r == 0U &&
              art.Artwork()[9U * 4U].r == 99U,
          "physical placement waits for Store authority without ghost paint");
    StoreBrushCheckpoint(board, "missing-root", "rejected",
                         "retry-store-bootstrap", "none");
}

void LostStoreDuringPickCannotRestoreRetiredCanvasOnVirtualLitchV3() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    stream::FrameStream pager;
    store::Manifest manifest{};
    manifest.frameCount = 1U;
    manifest.topology = 3U;
    manifest.active = true;
    store::Frame first{};
    first.durationMs = 1000U;
    first.emitters[9U * 4U].color = {23U, 67U, 101U};
    Check(pager.StageFirstFrame(manifest, 1U, first),
          "pick recovery starts with one displayed Store frame");
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    auto& art = board.Litchcraft();
    modules::LitchcraftSession::Canvas retired{};
    retired.fill({99U, 0U, 0U});
    art.RestoreArtwork(retired, true, 0U, nullptr, true);
    art.AttachFrameStream(&pager);
    art.BeginConnection();
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(UINT64_C(1) << 9U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.Advance(1100U) == VIRTUAL_LITCH_OK,
          "held physical piece starts Store color-pick preview");
    modules::LitchcraftSession::Packet packet{};
    while (art.PeekPacket(packet)) art.AcceptPacket();
    const auto beforeTool = art.Color();
    pager.Clear();
    Check(board.Advance(1000U) == VIRTUAL_LITCH_OK &&
              art.FrameCount() == 0U &&
              board.SemanticFrame().board[9U][0U].r == 0U &&
              art.Artwork()[9U * 4U].r == 99U &&
              art.Color().r == beforeTool.r &&
              art.Color().g == beforeTool.g &&
              art.Color().b == beforeTool.b,
          "lost Store cancels pending pick without restoring static art or tool");
    bool legacyCell = false;
    while (art.PeekPacket(packet)) {
        legacyCell = legacyCell ||
            (packet.size >= 2U && packet.bytes[0] ==
                modules::LitchcraftSession::MAGIC && packet.bytes[1] == 0x83U);
        art.AcceptPacket();
    }
    Check(!legacyCell, "lost Store pick never publishes a legacy Cell");
    StoreBrushCheckpoint(board, "lost-store-pick", "cancelled",
                         "restore-store-then-lift", "none");
    store::Frame blank{};
    blank.durationMs = 1000U;
    Check(pager.StageFirstFrame(manifest, 2U, blank) &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(UINT64_C(1) << 9U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[9U][0U].r != 0U &&
              pager.HasUnpersistedEdit(),
          "new Store frame accepts the next physical paint after lift");
    StoreBrushCheckpoint(board, "restored-store-paint", "accepted",
                         "lift-or-paint", "5s-save");
}

void StoreBrushPickAndDelayedSaveTraversesVirtualLitchV3() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    store::Manifest manifest{};
    manifest.frameCount = 1U;
    manifest.topology = 3U;
    manifest.active = true;
    store::Frame first{};
    constexpr uint8_t source = 4U;
    constexpr uint8_t destination = 5U;
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        first.emitters[source * 4U + slot].color = {23U, 67U, 101U};
    }
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok &&
              frames.WriteFrame(token, 0U, first) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "Store pick journey begins with one durable saved frame");
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              ServiceOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "saved frame loads through the production Store pager");

    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.brightness_scale_256 = 256U;
    config.max_led_current_ma = 12000U;
    config.strong_power_source = 1U;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    auto& art = board.Litchcraft();
    art.RestoreArtwork({}, true, 0U, nullptr, true);
    art.AttachFrameStream(&pager);
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK && art.IsActive(),
          "known-empty boot shows saved Litchcraft artwork and accepts input");
    StoreBrushCheckpoint(board, "enter", "accepted", "place-source", "none");
    const uint32_t savedGeneration = frames.Generation();
    Check(board.ObserveHallMask(UINT64_C(1) << source) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[source][0U].r == 255U &&
              pager.HasUnpersistedEdit() &&
              !pager.EditSaveReady(fxe::ManualMillis::NowMs()),
          "first Hall placement paints visibly before any flash save");
    StoreBrushCheckpoint(board, "first-brush", "accepted", "hold-or-lift", "2s-pick");
    Check(board.Advance(1000U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[source][0U].r >= 250U &&
              frames.Generation() == savedGeneration,
          "one-second hold keeps paint visible and leaves Store untouched");
    StoreBrushCheckpoint(board, "held-1s", "pending", "keep-holding", "2s-pick");
    Check(board.Advance(500U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[source][0U].r > 23U &&
              board.SemanticFrame().board[source][0U].r < 255U,
          "mid-hold rendering fades touched LEDs toward sampled artwork");
    StoreBrushCheckpoint(board, "held-1.5s", "pending", "keep-holding", "2s-pick");
    Check(board.Advance(500U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[source][0U].r == 23U &&
              art.Color().r == 23U && art.Color().g == 67U &&
              art.Color().b == 101U,
          "qualified hold restores saved square and selects sampled color");
    StoreBrushCheckpoint(board, "picked-2s", "accepted", "lift-and-place", "5s-save");
    Check(board.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(UINT64_C(1) << destination) ==
                  VIRTUAL_LITCH_OK && board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[destination][0U].r == 23U,
          "next Hall placement paints sampled color on timed V3 LEDs");
    StoreBrushCheckpoint(board, "sampled-brush", "accepted", "lift", "5s-save");
    Check(board.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK,
          "lifting the destination ends its stroke before another color pick");
    Check(frames.Generation() == savedGeneration &&
              !pager.EditSaveReady(fxe::ManualMillis::NowMs() + 4900U),
          "multiple gestures remain one quiet-period Store write");
    const bool advancedToSave = board.Advance(5000U) == VIRTUAL_LITCH_OK;
    const bool saveReady = pager.EditSaveReady(fxe::ManualMillis::NowMs());
    const auto saveResult = ServiceOne(pager, frames);
    Check(advancedToSave && saveReady &&
              saveResult != stream::FrameStream::ServiceResult::IoFailure &&
              !pager.HasUnpersistedEdit(),
          "background Store service commits after five quiet seconds");
    StoreBrushCheckpoint(board, "saved", "accepted", "paint-or-exit", "none");
    store::FrameStore restarted(medium.Ops());
    store::Frame saved{};
    Check(restarted.Recover() == store::Status::Ok &&
              restarted.ReadFrame(0U, saved) == store::Status::Ok &&
              saved.emitters[source * 4U].color.r == 23U &&
              saved.emitters[destination * 4U].color.r == 23U,
          "reboot recovery retains sampled source and destination artwork");
}

void StoreCommitPaintRetryTraversesVirtualLitchV3() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    store::Manifest manifest{};
    manifest.frameCount = 2U;
    manifest.topology = 3U;
    manifest.active = true;
    manifest.sparkleSeed = 1U;
    store::TransactionToken seed{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(manifest, 0U, 1U, seed) == store::Status::Ok &&
              frames.WriteFrame(seed, 0U, MakeNormalFrame(0U)) == store::Status::Ok &&
              frames.WriteFrame(seed, 1U, MakeNormalFrame(1U)) == store::Status::Ok &&
              frames.Commit(seed) == store::Status::Ok,
          "Store-paint journey starts with two durable old frames");
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              ServiceOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "old artwork is available to timed board rendering");
    Check(pager.Request(1U) &&
              ServiceOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              pager.BeginHistoryStroke(1U) &&
              pager.PaintDisplayed(1U, 1U, 1U, {77U, 88U, 99U}, true),
          "a previous physical stroke creates Undo on old frame one");
    pager.EndHistoryStroke();
    Check(pager.UndoCount() == 1U &&
              ServiceOne(pager, frames) !=
                  stream::FrameStream::ServiceResult::IoFailure &&
              !pager.HasUnpersistedEdit() && pager.Request(0U) &&
              ServiceOne(pager, frames) !=
                  stream::FrameStream::ServiceResult::IoFailure &&
              pager.TryPresentRequested() && pager.DisplayedIndex() == 0U,
          "the old stroke is saved before a replacement reserves authority");
    const uint32_t oldGeneration = frames.Generation();

    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.brightness_scale_256 = 256U;
    config.max_led_current_ma = 12000U;
    config.strong_power_source = 1U;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    auto& art = board.Litchcraft();
    art.BeginConnection();
    const auto enter = LegacyRequest(modules::LitchcraftSession::Op::Enter,
                                     0U, 1U);
    board.SendLitchcraftPacket(enter.data(), enter.size());
    art.AttachFrameStream(&pager);
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK && art.IsActive() &&
              art.SelectedFrame() == 0U,
          "board enters Litchcraft with old artwork and empty occupancy");
    const uint64_t oldHash = board.PhysicalFrame().frame_hash;

    store::StoreReceiver receiver(frames);
    const uint32_t session = art.SessionId();
    Check(receiver.Recover({session, 0U, 3U, true, false}) ==
              store::Status::Ok,
          "production Store receiver sees the same old root and mode session");
    receiver.SetCommitCallbacks(
        [](void* context, uint32_t, uint32_t expected,
           const store::Manifest& incoming, uint32_t generation,
           const store::Frame&) {
            auto& live = *static_cast<stream::FrameStream*>(context);
            return live.TryReserveReplacement(expected, incoming, generation);
        },
        [](void* context, uint32_t, uint32_t expected,
           const store::Manifest& incoming, uint32_t generation,
           const store::Frame& first) {
            auto& live = *static_cast<stream::FrameStream*>(context);
            return live.PublishReservedReplacement(
                expected, incoming, generation, first);
        }, &pager);

    const auto nextFrame = MakeNormalFrame(7U);
    std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
    size_t encodedSize = 0U;
    Check(modules::litchcraft_wire_codec::FrameCodec::Encode(
              nullptr, nextFrame, encoded.data(), encoded.size(), encodedSize),
          "replacement frame uses the production wire codec");
    std::vector<uint8_t> body(store::WIRE_RECORD_HEADER_BYTES + encodedSize);
    fxe::WriteU16LE(static_cast<uint16_t>(encodedSize), body.data());
    fxe::WriteU32LE(fxe::Crc32(encoded.data(), encodedSize), body.data() + 2U);
    std::copy_n(encoded.data(), encodedSize,
                body.data() + store::WIRE_RECORD_HEADER_BYTES);
    const auto packet = [session](const uint8_t op, const uint16_t transfer,
                                  const size_t size) {
        std::vector<uint8_t> bytes(size, 0U);
        bytes[0U] = 0x4dU;
        bytes[1U] = 2U;
        bytes[2U] = op;
        fxe::WriteU32LE(session, bytes.data() + 3U);
        fxe::WriteU16LE(transfer, bytes.data() + 7U);
        return bytes;
    };
    const auto begin = [&](const uint16_t transfer, const uint32_t crc) {
        auto bytes = packet(store::STORE_BEGIN, transfer, 53U);
        fxe::WriteU32LE(frames.Generation(), bytes.data() + 9U);
        fxe::WriteU32LE(static_cast<uint32_t>(body.size()), bytes.data() + 13U);
        fxe::WriteU32LE(crc, bytes.data() + 17U);
        fxe::WriteU16LE(1U, bytes.data() + 21U);
        bytes[23U] = 3U;
        bytes[24U] = 1U;
        fxe::WriteU32LE(1U, bytes.data() + 25U);
        return bytes;
    };
    const auto chunk = [&](const uint16_t transfer) {
        auto bytes = packet(store::STORE_CHUNK, transfer, 13U + body.size());
        fxe::WriteU32LE(0U, bytes.data() + 9U);
        std::copy(body.begin(), body.end(), bytes.begin() + 13U);
        return bytes;
    };
    const auto commit = [&](const uint16_t transfer) {
        auto bytes = packet(store::STORE_COMMIT, transfer, 13U);
        fxe::WriteU32LE(static_cast<uint32_t>(body.size()), bytes.data() + 9U);
        return bytes;
    };
    const auto badBegin = begin(41U, fxe::Crc32(body.data(), body.size()) ^ 1U);
    const auto badChunk = chunk(41U);
    const auto badCommit = commit(41U);
    store::StoreIngressResult response{};
    receiver.Handle(badBegin.data(), badBegin.size(), 7U, response);
    receiver.Handle(badChunk.data(), badChunk.size(), 7U, response);
    store::StoreIngressResult rejected{};
    receiver.Handle(badCommit.data(), badCommit.size(), 7U, rejected);
    const auto rejectedAdvance = board.Advance(20U);
    std::printf("[STORE_COMMIT_PAINT_CHECKPOINT] invalid status=%u root=%lu live=%lu old_led=%016llx current_led=%016llx semantic_red=%u\n",
                rejected.responseReady ? rejected.response[13U] : 255U,
                static_cast<unsigned long>(frames.Generation()),
                static_cast<unsigned long>(pager.Generation()),
                static_cast<unsigned long long>(oldHash),
                static_cast<unsigned long long>(board.PhysicalFrame().frame_hash),
                board.SemanticFrame().board[27U][0U].r);
    Check(rejected.responseReady &&
              rejected.response[13U] ==
                  static_cast<uint8_t>(store::WireStatus::IntegrityFailure) &&
              rejectedAdvance == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[27U][0U].r == 64U &&
              board.PhysicalFrame().led_count == 288U &&
              pager.Generation() == oldGeneration,
          "corrupt complete upload is rejected with old LEDs and next action Cancel");
    const auto cancel = packet(store::STORE_CANCEL, 41U, 9U);
    receiver.Handle(cancel.data(), cancel.size(), 7U, response);

    const auto goodBegin = begin(42U, fxe::Crc32(body.data(), body.size()));
    const auto goodChunk = chunk(42U);
    const auto goodCommit = commit(42U);
    receiver.Handle(goodBegin.data(), goodBegin.size(), 7U, response);
    receiver.Handle(goodChunk.data(), goodChunk.size(), 7U, response);
    medium.failRootSyncOnce = true;
    store::StoreIngressResult pending{};
    receiver.Handle(goodCommit.data(), goodCommit.size(), 7U, pending);
    Check(!pending.responseReady && receiver.CommitPending() &&
              pager.Generation() == oldGeneration &&
              frames.Generation() == oldGeneration &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[27U][0U].r == 64U &&
              board.PhysicalFrame().led_count == 288U,
          "failed root publication keeps the old timed V3 LEDs visible");
    Check(pager.SaveModeRoot(frames, false) ==
              store::Status::RevisionConflict &&
              frames.Generation() == oldGeneration,
          "a mode save cannot publish over the prepared replacement root");
    const uint8_t beforePaint = board.SemanticFrame().board[9U][0U].r;
    Check(board.ObserveHallMask(UINT64_C(1) << 9U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              !pager.HasUnpersistedEdit() &&
              board.SemanticFrame().board[9U][0U].r == beforePaint,
          "Hall input cannot paint the unpublished replacement");
    Check(pager.HistoryTargetFrame(false) == 1U &&
              board.SetButtonDown(1U, runtime::VirtualLitchPhysicalButton::X,
                                  true) == VIRTUAL_LITCH_OK &&
              board.Advance(30U) == VIRTUAL_LITCH_OK &&
              board.SetButtonDown(1U, runtime::VirtualLitchPhysicalButton::X,
                                  false) == VIRTUAL_LITCH_OK &&
              art.SelectedFrame() == 0U &&
              pager.RequestedIndex() == 0U &&
              pager.UndoCount() == 1U &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[27U][0U].r == 64U,
          "physical X Undo cannot change the selected old frame during root retry");
    const auto lateCancel = packet(store::STORE_CANCEL, 42U, 9U);
    store::StoreIngressResult refusedCancel{};
    receiver.Handle(lateCancel.data(), lateCancel.size(), 7U, refusedCancel);
    Check(refusedCancel.responseReady &&
              refusedCancel.response[13U] ==
                  static_cast<uint8_t>(store::WireStatus::Busy) &&
              receiver.CommitPending(),
          "late Cancel cannot revoke the reserved replacement");
    receiver.EndOwner(7U);
    store::StoreIngressResult accepted{};
    receiver.RetryPendingCommit(accepted);
    Check(accepted.responseReady && accepted.response[13U] == 0U &&
              !receiver.CommitPending() &&
              frames.Generation() == oldGeneration + 1U &&
              pager.Generation() == oldGeneration + 1U &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[27U][0U].r == 92U,
          "root retry durably publishes before adopting the new LEDs");
    Check(board.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(UINT64_C(1) << 9U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              pager.HasUnpersistedEdit() &&
              board.SemanticFrame().board[9U][0U].r != beforePaint,
          "a new Hall placement paints the durable replacement and timed V3 LEDs");
    const auto exit = LegacyRequest(modules::LitchcraftSession::Op::Exit,
                                    session, 2U);
    board.SendLitchcraftPacket(exit.data(), exit.size());
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK && !art.IsActive(),
          "same-session Exit takes mode authority after replacement");
    receiver.SetProjection({session, 0U, 3U, false, false});
    const auto finalSave = pager.Service(frames, false);
    Check(finalSave !=
              stream::FrameStream::ServiceResult::IoFailure &&
              !pager.HasUnpersistedEdit(),
          "worker saves the Hall paint on the accepted generation after retry");
    store::FrameStore rebooted(medium.Ops());
    store::Frame painted{};
    Check(rebooted.Recover() == store::Status::Ok &&
              rebooted.ReadFrame(0U, painted) == store::Status::Ok &&
              painted.emitters[36U].color.r != 0U,
          "reboot recovers the accepted artwork and physically painted emitter");
    std::printf("[STORE_COMMIT_PAINT_VIRTUAL_LITCH] occupancy=empty->square9 "
                "input=bad-Commit+Cancel+good-Commit+Hall9+Undo+late-Cancel+retry+Hall9+Exit "
                "result=IntegrityFailure->Busy->Ok mode=active->inactive "
                "LEDs=old-during-retry->new->painted next=retry-or-enter expiry=none\n");
}

enum class SlowSaveProbe : uint8_t { Original, WarmWire, Reopen, MissingSummary, LongWrite, NetUnchangedSync };

void UploadRickrollWire(store::FrameStore& frames,
                       const std::array<uint8_t, rickroll_fixture::RGB_BYTES>& rgb) {
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({1U, 0U, 3U, true, false}) == store::Status::NoArtwork,
          "fresh production receiver has no artwork before actual RR wire ingress");
    std::vector<uint8_t> body;
    store::Frame previous{};
    std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> record{};
    for (uint16_t index = 0U; index < 32U; ++index) {
        const auto frame = MakeFrame(rgb, index);
        size_t size = 0U;
        Check(modules::litchcraft_wire_codec::FrameCodec::Encode(
                  index % store::KEYFRAME_INTERVAL == 0U ? nullptr : &previous,
                  frame, record.data(), record.size(), size),
              "RR ingress uses the app's adjacent-predecessor canonical record policy");
        const size_t offset = body.size();
        body.resize(offset + store::WIRE_RECORD_HEADER_BYTES + size);
        fxe::WriteU16LE(static_cast<uint16_t>(size), body.data() + offset);
        fxe::WriteU32LE(fxe::Crc32(record.data(), size), body.data() + offset + 2U);
        std::copy_n(record.data(), size, body.data() + offset + store::WIRE_RECORD_HEADER_BYTES);
        previous = frame;
    }
    Check(body.size() == 27823U, "real RR ingress has exact 27823-byte identity");
    const auto packet = [](uint8_t op, size_t size) {
        std::vector<uint8_t> bytes(size, 0U);
        bytes[0U] = 0x4dU; bytes[1U] = 2U; bytes[2U] = op;
        fxe::WriteU32LE(1U, bytes.data() + 3U);
        fxe::WriteU16LE(1U, bytes.data() + 7U);
        return bytes;
    };
    auto begin = packet(store::STORE_BEGIN, 53U);
    fxe::WriteU32LE(frames.Generation(), begin.data() + 9U);
    fxe::WriteU32LE(static_cast<uint32_t>(body.size()), begin.data() + 13U);
    fxe::WriteU32LE(fxe::Crc32(body.data(), body.size()), begin.data() + 17U);
    fxe::WriteU16LE(32U, begin.data() + 21U);
    begin[23U] = 3U; begin[24U] = 7U; // active + playback-on-entry + predecessor
    fxe::WriteU32LE(1U, begin.data() + 25U);
    for (size_t i = 0U; i < modules::LITCHCRAFT_DEFAULT_QUICK_PALETTE.size(); ++i) {
        const auto color = modules::LITCHCRAFT_DEFAULT_QUICK_PALETTE[i];
        begin[29U + i * 3U] = color.r;
        begin[30U + i * 3U] = color.g;
        begin[31U + i * 3U] = color.b;
    }
    store::StoreIngressResult result{};
    receiver.Handle(begin.data(), begin.size(), 7U, result);
    Check(!result.responseReady && receiver.TransferActive(),
          "RR Begin enters the production StoreReceiver transaction without a receipt");
    for (size_t cursor = 0U; cursor < body.size();) {
        const size_t count = std::min<size_t>(180U, body.size() - cursor);
        auto chunk = packet(store::STORE_CHUNK, 13U + count);
        fxe::WriteU32LE(static_cast<uint32_t>(cursor), chunk.data() + 9U);
        std::copy_n(body.data() + cursor, count, chunk.data() + 13U);
        receiver.Handle(chunk.data(), chunk.size(), 7U, result);
        Check(!result.responseReady || result.response[13U] == 0U,
              "actual RR chunks preserve production parser/checkpoint authority");
        cursor += count;
    }
    auto commit = packet(store::STORE_COMMIT, 13U);
    fxe::WriteU32LE(static_cast<uint32_t>(body.size()), commit.data() + 9U);
    receiver.Handle(commit.data(), commit.size(), 7U, result);
    if (receiver.CommitPending()) receiver.RetryPendingCommit(result);
    Check(result.responseReady && result.response[13U] == 0U &&
              !receiver.TransferActive() && frames.CurrentBodyBytes() == body.size() &&
              frames.CurrentManifest().predecessorDeltas,
          "real RR Commit verifies persisted predecessor summaries before edit readiness");
}

struct TimedPlayback final {
    runtime::VirtualLitchSession& board;
    stream::FrameStream& pager;
    store::FrameStore& frames;
    uint32_t accessMs = 50U;
    uint32_t lastChangedMs = 0U;
    uint32_t longestHoldMs = 0U;
    uint32_t lastPresentedMs = 0U;
    uint32_t longestFrameHoldMs = 0U;
    uint16_t displayed = 0U;
    uint64_t subjectHash = 0U;
    uint32_t accesses = 0U;
    uint32_t reads = 0U;
    uint32_t writes = 0U;
    uint32_t syncs = 0U;
    uint32_t readBytes = 0U;
    bool allPlaying = true;
    bool inSave = false;
    bool slowBodyReads = false;
    bool spikeWrite = false;
    bool spikeSync = false;
    uint32_t maximumPrimitiveMs = 0U;
    std::array<uint32_t, 4U> phaseMs{};
    std::array<uint32_t, 4U> phaseCalls{};

    static void Observe(void* context, store::FrameStore::TimingPhase phase, uint32_t ms) {
        auto& self = *static_cast<TimedPlayback*>(context);
        const size_t index = static_cast<size_t>(phase);
        if (index < self.phaseMs.size()) {
            self.phaseMs[index] += ms;
            ++self.phaseCalls[index];
        }
    }

    uint64_t SubjectHash() const {
        uint64_t hash = UINT64_C(14695981039346656037);
        for (uint16_t emitter = 0U; emitter < 256U; ++emitter) {
            const auto square = static_cast<uint8_t>(emitter / 4U);
            const auto column = static_cast<uint8_t>(square % 8U);
            const auto side = static_cast<uint8_t>(emitter % 4U);
            if (square == 0U || square == 7U || square == 56U || square == 63U ||
                (column == 0U && side == 0U) || (column == 7U && side == 2U)) continue;
            const auto& value = board.PhysicalFrame().pixels[emitter];
            for (const auto channel : {value.r, value.g, value.b, value.w}) {
                hash = (hash ^ channel) * UINT64_C(1099511628211);
            }
        }
        return hash;
    }

    void Advance(uint32_t ms) {
        while (ms != 0U) {
            const uint32_t slice = std::min(ms, 10U);
            Check(board.Advance(slice) == VIRTUAL_LITCH_OK,
                  "render/input clock continues during primitive flash latency");
            ms -= slice;
            const uint32_t now = fxe::Millis();
            const uint64_t hash = SubjectHash();
            if (hash != subjectHash) { subjectHash = hash; lastChangedMs = now; }
            longestHoldMs = std::max(longestHoldMs, now - lastChangedMs);
            if (pager.DisplayedIndex() != displayed) {
                displayed = pager.DisplayedIndex();
                lastPresentedMs = now;
            }
            longestFrameHoldMs = std::max(longestFrameHoldMs, now - lastPresentedMs);
            allPlaying = allPlaying && board.Litchcraft().IsAnimationPlaying();
        }
    }

    static void Access(void* context, const char* operation,
                       size_t offset, size_t size) {
        auto& self = *static_cast<TimedPlayback*>(context);
        ++self.accesses;
        if (operation[0] == 'r') { ++self.reads; self.readBytes += size; }
        if (operation[0] == 'w') ++self.writes;
        if (operation[0] == 's') ++self.syncs;
        const uint32_t before = fxe::Millis();
        // Retained clean-slot reads remain fast enough to prefetch 50ms RR.
        // Root/summary/verification access, writes and flushes model the slow
        // flash boundaries omitted by the old instantaneous native medium.
        uint32_t cost = operation[0] == 'r' && size == store::STAGE_SLOT_BYTES
            ? (self.inSave && self.slowBodyReads ? self.accessMs : 1U) : self.accessMs;
        if (self.inSave && self.spikeWrite && operation[0] == 'w') {
            cost = 6000U;
            self.spikeWrite = false;
        }
        if (self.inSave && self.spikeSync && operation[0] == 's') {
            cost = 4098U;
            self.spikeSync = false;
        }
        self.maximumPrimitiveMs = std::max(self.maximumPrimitiveMs, cost);
        self.Advance(cost);
        if (self.accesses % 10U == 0U) {
            std::printf("[RR_EDIT_IO] t=%u op=%s offset=%zu bytes=%zu calls=%u "
                        "requested=%u displayed=%u selected=%u elapsed=%llu "
                        "playing=%u dirty=%u hold_ms=%u root=%u live=%u/%u\n",
                before, operation, offset, size, self.accesses,
                self.pager.RequestedIndex(), self.pager.DisplayedIndex(),
                self.board.Litchcraft().SelectedFrame(),
                static_cast<unsigned long long>(self.board.Litchcraft().AnimationElapsedMs()),
                self.board.Litchcraft().IsAnimationPlaying(),
                self.pager.HasUnpersistedEdit(), self.longestHoldMs,
                self.frames.Generation(), self.pager.Generation(), self.pager.LiveRevision());
        }
    }
};

void RickrollEraseUndoPlayAcrossSlowEditSaveVirtualLitchV3(
    const bool staged, const SlowSaveProbe probe = SlowSaveProbe::Original) {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    const auto rgb = DecodeRgb();
    Medium medium;
    store::FrameStore frames(medium.Ops());
    store::Manifest manifest{};
    manifest.frameCount = 32U;
    manifest.topology = 3U;
    manifest.active = true;
    manifest.predecessorDeltas = true;
    manifest.sparkleSeed = 1U;
    manifest.playbackOnEntry = true;
    manifest.favorites = modules::LITCHCRAFT_DEFAULT_QUICK_PALETTE;
    store::TransactionToken token{};
    if (probe == SlowSaveProbe::Original) {
        Check(frames.Recover() == store::Status::NoArtwork &&
                  frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
              "slow-save journey starts from the actual 32-frame RR document");
        for (uint16_t index = 0U; index < 32U; ++index) {
            Check(frames.WriteFrame(token, index, MakeFrame(rgb, index)) == store::Status::Ok,
                  "all exact app RR frames enter production Store");
        }
        Check(frames.Commit(token) == store::Status::Ok && frames.CurrentBodyBytes() == 27823U,
              "exact RR body matches the physical 27823-byte edit-save evidence");
    } else {
        UploadRickrollWire(frames, rgb);
    }
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()),
          "RR document configures the production three-slot pager");
    ServiceOne(pager, frames);
    Check(pager.TryPresentRequested(), "first RR frame is presented");
    if (staged) {
        Check(pager.PaintDisplayed(0U, 0U, 1U, {77U, 88U, 99U}, false),
              "prior-stage fixture makes a real durable RR edit");
        Check(ServiceOne(pager, frames) != stream::FrameStream::ServiceResult::IoFailure &&
                  !pager.HasUnpersistedEdit(), "previous real RR edit is durable");
        namespace fixed = modules::litchcraft_frame_store_layout;
        std::array<uint8_t, fixed::FRAME_SLOT_BYTES> scratch{};
        fixed::Layout layout(medium.Ops(), scratch.data(), scratch.size());
        fixed::Root root{};
        uint8_t copy = 0U;
        Check(layout.LatestRoot(0U, root, copy) == fixed::Status::Ok &&
                  root.stageIndex == 0U && root.generation == frames.Generation(),
              "prior-stage fixture actually retains the committed stage");
    }
    if (probe == SlowSaveProbe::Reopen || probe == SlowSaveProbe::MissingSummary) {
        if (probe == SlowSaveProbe::MissingSummary) {
            // Explicit fault, NOT established by the physical RR capture.
            // Both derived copies are absent; all authoritative roots/body remain valid.
            std::fill(medium.bytes.begin() + store::FRAME_FILES_CAPACITY,
                      medium.bytes.end(), 0xffU);
        }
        Check(frames.Recover(store::FrameStore::RecoveryPurpose::RootOnly) == store::Status::Ok,
              "worker-equivalent reopen recovers root only while live stream retains authority");
    }
    ServiceOne(pager, frames);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.brightness_scale_256 = 256U;
    config.max_led_current_ma = 12000U;
    config.strong_power_source = 1U;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    auto& art = board.Litchcraft();
    art.RestoreArtwork({}, true, 0U, nullptr, true);
    art.AttachFrameStream(&pager);
    board.Advance(1000U);
    const auto play = [&]() {
        uint8_t payload[6U] = {1U, static_cast<uint8_t>(art.SelectedFrame()), 0U, 0U, 0U, 0U};
        fxe::WriteU32LE(pager.Generation(), payload + 2U);
        return art.ApplyConnectionOrdered(2U, payload, sizeof(payload), 1U);
    };
    Check(play() == modules::LitchcraftSession::Status::Ok,
          "first ordinary Play starts RR");
    for (uint32_t elapsed = 0U; elapsed < 6000U; elapsed += 10U) {
        pager.Service(frames, false, stream::FrameStream::ServiceMode::PagingOnly);
        board.Advance(10U);
    }
    // Replay accepted capture-Pause, Guard, Erase-all, Guard, Undo, Play.
    // This native path bypasses the mounted pointer gesture and RF transport.
    uint8_t select[6U] = {2U, static_cast<uint8_t>(art.SelectedFrame()), 0U, 0U, 0U, 0U};
    fxe::WriteU32LE(pager.Generation(), select + 2U);
    Check(art.ApplyConnectionOrdered(2U, select, sizeof(select), 1U) ==
              modules::LitchcraftSession::Status::Ok,
          "long-hold capture pins the current authored frame");
    board.Advance(650U);
    const uint16_t edited = art.SelectedFrame();
    auto expected = MakeFrame(rgb, edited);
    store::Frame durableBeforeErase{};
    Check(frames.ReadFrame(edited, durableBeforeErase) == store::Status::Ok &&
              SameFrame(durableBeforeErase, expected),
          "reported RR Erase/Undo trigger begins with the exact durable semantic frame");
    const uint32_t epoch = pager.DocumentEpoch();
    const uint32_t revision = pager.LiveRevision();
    const auto guard = [&]() {
        uint8_t payload[8U]{};
        fxe::WriteU32LE(pager.Generation(), payload);
        fxe::WriteU32LE(pager.LiveRevision(), payload + 4U);
        return art.ApplyConnectionOrdered(8U, payload, sizeof(payload), 1U);
    };
    const uint8_t erase[] = {static_cast<uint8_t>(edited), 15U, 0U, 0U, 0U, 1U};
    Check(guard() == modules::LitchcraftSession::Status::Ok &&
              art.ApplyConnectionOrdered(9U, erase, sizeof(erase), 1U) ==
                  modules::LitchcraftSession::Status::Ok &&
              pager.LiveRevision() == revision + 1U && pager.UndoCount() >= 1U,
          "actual Guard and Erase-all apply exactly one live revision");
    Check(guard() == modules::LitchcraftSession::Status::Ok &&
              art.ApplyConnectionOrdered(6U, nullptr, 0U, 1U) ==
                  modules::LitchcraftSession::Status::Ok &&
              pager.LiveRevision() == revision + 2U,
          "actual guarded Undo applies its RR inverse exactly once");
    store::Frame undoFrame{};
    uint16_t undoIndex = 0U;
    Check(pager.CopyDisplayed(undoFrame, undoIndex) && undoIndex == edited &&
              SameFrame(undoFrame, expected) && pager.DocumentEpoch() == epoch,
          "Undo restores RGB/effects/duration in the same current document");
    // Keep slow-fold/summary/long-primitive coverage on an actual unsaved edit,
    // instead of requiring a redundant inverse to republish the same root.
    const bool realEdit = probe != SlowSaveProbe::NetUnchangedSync;
    if (realEdit) {
        Check(pager.PaintDisplayed(edited, 3U, 1U, {91U, 73U, 27U}, false),
              "slow save probes retain a genuine semantic edit after Undo");
        expected.emitters[12U].color = {91U, 73U, 27U};
    }
    Check(play() == modules::LitchcraftSession::Status::Ok && art.IsAnimationPlaying(),
          "second ordinary Play starts both clock and presented RR image");
    const uint32_t playAt = fxe::Millis();
    const uint32_t generation = frames.Generation();
    TimedPlayback timing{board, pager, frames};
    timing.subjectHash = timing.SubjectHash();
    timing.lastChangedMs = playAt;
    timing.lastPresentedMs = playAt;
    timing.displayed = pager.DisplayedIndex();
    timing.slowBodyReads = probe == SlowSaveProbe::MissingSummary;
    timing.spikeWrite = probe == SlowSaveProbe::LongWrite;
    timing.spikeSync = probe == SlowSaveProbe::NetUnchangedSync;
    frames.SetTimingObserver(&TimedPlayback::Observe, &timing);
    medium.accessContext = &timing;
    medium.accessHook = &TimedPlayback::Access;
    uint32_t saveStarted = 0U;
    uint32_t saveElapsed = 0U;
    uint32_t saveCalls = 0U;
    uint32_t saveReads = 0U;
    uint32_t saveWrites = 0U;
    uint32_t saveSyncs = 0U;
    while (fxe::Millis() - playAt < 20000U) {
        const bool save = pager.HasUnpersistedEdit() && pager.EditSaveReady(fxe::Millis());
        const uint32_t started = fxe::Millis();
        const uint32_t calls = timing.accesses;
        const uint32_t reads = timing.reads, writes = timing.writes, syncs = timing.syncs;
        timing.inSave = save;
        const auto result = pager.Service(frames, false, save
            ? stream::FrameStream::ServiceMode::PagingAndEdits
            : stream::FrameStream::ServiceMode::PagingOnly);
        Check(result != stream::FrameStream::ServiceResult::IoFailure,
              "slow primitive storage retains valid production transaction authority");
        if (saveStarted == 0U && (frames.Generation() != generation ||
            (save && !pager.HasUnpersistedEdit()))) {
            saveStarted = started - playAt;
            saveElapsed = fxe::Millis() - started;
            saveCalls = timing.accesses - calls;
            saveReads = timing.reads - reads;
            saveWrites = timing.writes - writes;
            saveSyncs = timing.syncs - syncs;
        }
        timing.inSave = false;
        timing.Advance(10U);
    }
    medium.accessHook = nullptr;
    frames.SetTimingObserver(nullptr, nullptr);
    std::printf("[RR_SLOW_SAVE] prior_stage=%u play_t=%u save_start_after_play=%u "
                "save_ms=%u calls=%u reads=%u writes=%u syncs=%u read_bytes=%u "
                "frame_ms=50 render_tick_ms=10 slow_io_ms=%u clean_slot_ms=1 "
                "max_subject_hold_ms=%u max_frame_hold_ms=%u playing=%u root=%u->%u live_revision=%u "
                "document_epoch=%u elapsed=%llu\n",
        staged, playAt, saveStarted, saveElapsed, saveCalls, saveReads, saveWrites,
        saveSyncs, timing.readBytes, timing.accessMs, timing.longestHoldMs,
        timing.longestFrameHoldMs, timing.allPlaying,
        generation, frames.Generation(), pager.LiveRevision(), pager.DocumentEpoch(),
        static_cast<unsigned long long>(art.AnimationElapsedMs()));
    if (probe == SlowSaveProbe::NetUnchangedSync) {
        Check(!pager.HasUnpersistedEdit() && frames.Generation() == generation &&
                  pager.LiveRevision() == revision + 2U && pager.DocumentEpoch() == epoch,
              "Erase then Undo retains durable generation while acknowledging both live actions");
        Check(timing.writes == 0U && timing.syncs == 0U && timing.spikeSync &&
                  saveReads == 1U && saveElapsed == 1U && timing.phaseCalls[3U] == 0U &&
                  pager.RedoCount() >= 1U,
              "identical Undo-restored RR frame never enters a body/root write or slow Sync");
        Check(timing.longestFrameHoldMs <= 110U && timing.longestHoldMs < 1000U,
              "RR keeps advancing when identical Erase/Undo would otherwise hit measured 4098ms Sync");
    } else Check(saveStarted != 0U && !pager.HasUnpersistedEdit() &&
              frames.Generation() == generation + 1U &&
              pager.LiveRevision() == revision + 3U && pager.DocumentEpoch() == epoch,
          "one asynchronous durable save retains Undo exactly-once/current-document authority");
    Check(timing.allPlaying, "save does not issue an implicit Pause or replace command");
    // A cold successor may wait one primitive beyond its authored deadline.
    // A 50ms RR dwell plus the controlled 50ms operation and 10ms render sample
    // gives a meaningful 110ms limit, independent of the field's ~1s symptom.
    const uint32_t transactionMs = timing.phaseMs[3U] + timing.phaseMs[1U] + timing.phaseMs[2U];
    std::printf("[RR_SAVE_PHASE_PROBE] profile=%u stage=%u pre_begin_ms=%u begin_calls=%u begin_ms=%u "
                "prepare_ms=%u publish_ms=%u max_primitive_ms=%u subject_hold_ms=%u "
                "frame_hold_ms=%u saved=%u elapsed=%llu\n",
        static_cast<unsigned>(probe), staged, saveElapsed - transactionMs,
        timing.phaseCalls[3U], timing.phaseMs[3U],
        timing.phaseMs[1U], timing.phaseMs[2U], timing.maximumPrimitiveMs,
        timing.longestHoldMs, timing.longestFrameHoldMs, !pager.HasUnpersistedEdit(),
        static_cast<unsigned long long>(art.AnimationElapsedMs()));
    if (probe != SlowSaveProbe::Original && probe != SlowSaveProbe::NetUnchangedSync) {
        Check(timing.phaseCalls[3U] == 1U && timing.phaseCalls[1U] == 1U &&
                  timing.phaseCalls[2U] == 1U &&
                  transactionMs + (timing.slowBodyReads ? timing.accessMs : 1U) == saveElapsed,
              "validated pre-Begin slot read plus transaction phases account for the edit save");
    }
    if (probe == SlowSaveProbe::Original || probe == SlowSaveProbe::WarmWire) {
    Check(timing.longestFrameHoldMs <= 50U + timing.accessMs + 10U,
          "each RR frame advances within authored dwell plus one primitive/render sample");
    Check(timing.longestHoldMs < 1000U,
          "RR subject must keep advancing through slow primitive edit-save access");
    } else if (probe != SlowSaveProbe::NetUnchangedSync) {
        Check(timing.longestHoldMs >= (probe == SlowSaveProbe::LongWrite ? 6000U : 250U),
              "explicit unpaged summary/long primitive probe retains its observable image hold");
    }
    store::FrameStore recovered(medium.Ops());
    store::Frame restored{};
    Check(recovered.Recover() == store::Status::Ok &&
              recovered.ReadFrame(edited, restored) == store::Status::Ok &&
              SameFrame(restored, expected),
          "saved root recovers the exact Undo-restored RR frame");
    const auto& savedManifest = recovered.CurrentManifest();
    Check(savedManifest.frameCount == 32U && savedManifest.topology == 3U &&
              savedManifest.active && savedManifest.playbackOnEntry &&
              savedManifest.sparkleSeed == 1U && savedManifest.predecessorDeltas,
          "no-op and real edits preserve app RR root preferences");
    for (size_t i = 0U; i < savedManifest.favorites.size(); ++i) {
        const auto a = savedManifest.favorites[i];
        const auto b = modules::LITCHCRAFT_DEFAULT_QUICK_PALETTE[i];
        Check(a.r == b.r && a.g == b.g && a.b == b.b,
              "worker preserves the loaded library favorite metadata");
    }
}

} // namespace

int main() {
    RickrollEraseUndoPlayAcrossSlowEditSaveVirtualLitchV3(false, SlowSaveProbe::NetUnchangedSync);
    RickrollEraseUndoPlayAcrossSlowEditSaveVirtualLitchV3(true, SlowSaveProbe::NetUnchangedSync);
    if (std::getenv("LITCH_NET_UNCHANGED_EDIT_ONLY") != nullptr) {
        std::printf("[RR_NET_UNCHANGED] %s\n", failures == 0 ? "passed" : "failed");
        return failures == 0 ? 0 : 1;
    }
    RickrollEraseUndoPlayAcrossSlowEditSaveVirtualLitchV3(false);
    RickrollEraseUndoPlayAcrossSlowEditSaveVirtualLitchV3(true);
    RickrollEraseUndoPlayAcrossSlowEditSaveVirtualLitchV3(false, SlowSaveProbe::WarmWire);
    RickrollEraseUndoPlayAcrossSlowEditSaveVirtualLitchV3(true, SlowSaveProbe::Reopen);
    RickrollEraseUndoPlayAcrossSlowEditSaveVirtualLitchV3(true, SlowSaveProbe::MissingSummary);
    RickrollEraseUndoPlayAcrossSlowEditSaveVirtualLitchV3(true, SlowSaveProbe::LongWrite);
    MissingStoreRootCannotUseRetiredCanvasOnVirtualLitchV3();
    LostStoreDuringPickCannotRestoreRetiredCanvasOnVirtualLitchV3();
    FreshBlankStorePaintTraversesVirtualLitchV3();
    RickrollStreamTraversesVirtualLitchV3();
    BouncingBalls256TraversesVirtualLitchV3();
    NormalStorePlaybackControlsTraverseVirtualLitchV3();
    StoreBrushPickAndDelayedSaveTraversesVirtualLitchV3();
    StoreCommitPaintRetryTraversesVirtualLitchV3();
    std::printf("[RICKROLL_VIRTUAL_LITCH] %s\n",
                failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
