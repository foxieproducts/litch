#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_frame_store_receiver.hpp"
#include "modules/litchcraft_frame_store_layout.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "modules/litchcraft_session.hpp"
#include "modules/litchcraft_wire_codec.hpp"

namespace {
namespace store = modules::litchcraft_frame_store;
namespace wire = modules::litchcraft_wire_codec;

int failures = 0;
size_t checks = 0U;

store::StoreIngressResult Handle(store::StoreReceiver& receiver,
                                 const uint8_t* bytes, const size_t size,
                                 const uint32_t ownerEpoch) {
    store::StoreIngressResult result{};
    receiver.Handle(bytes, size, ownerEpoch, result);
    return result;
}

store::StoreIngressResult ContinueRead(store::StoreReceiver& receiver,
                                       const uint32_t ownerEpoch) {
    store::StoreIngressResult result{};
    receiver.ContinueRead(ownerEpoch, result);
    return result;
}

store::StoreIngressResult RetryPendingCommit(store::StoreReceiver& receiver) {
    store::StoreIngressResult result{};
    receiver.RetryPendingCommit(result);
    return result;
}

void Check(const bool condition, const char* const message) {
    ++checks;
    if (condition) return;
    ++failures;
    std::printf("[FAIL] %s\n", message);
}

struct MemoryFile final {
    std::vector<uint8_t> bytes =
        std::vector<uint8_t>(store::ARTWORK_FILE_CAPACITY, 0U);
    std::vector<uint8_t> durable = bytes;
    size_t bodyWriteCalls = 0U;
    size_t writeCalls = 0U;
    size_t readCalls = 0U;
    size_t frameLoads = 0U;
    size_t readBytes = 0U;
    bool failRootSyncOnce = false;
    bool failReads = false;
    modules::litchcraft_frame_stream::FrameStream* mutateDuringFrameRead = nullptr;
    bool rootWritePending = false;
    modules::litchcraft_frame_stream::FrameStream* renderDuringRootWrite = nullptr;

    static bool Read(void* context, const size_t offset, uint8_t* out,
                     const size_t size) {
        auto& self = *static_cast<MemoryFile*>(context);
        if (out == nullptr || offset > self.bytes.size() ||
            size > self.bytes.size() - offset) return false;
        if (self.failReads) return false;
        ++self.readCalls;
        namespace fixed = modules::litchcraft_frame_store_layout;
        if (size == store::STAGE_SLOT_BYTES &&
            (offset == fixed::Layout::StageOffset() ||
             (offset >= store::FrameStore::BodyOffset(0U) &&
              offset < store::FRAME_FILES_CAPACITY))) ++self.frameLoads;
        self.readBytes += size;
        if (self.mutateDuringFrameRead != nullptr && size == store::STAGE_SLOT_BYTES &&
            offset >= store::FrameStore::BodyOffset(0U)) {
            auto* stream = self.mutateDuringFrameRead;
            self.mutateDuringFrameRead = nullptr;
            (void)stream->PaintDisplayed(stream->DisplayedIndex(), 1U, 15U,
                                         {34U, 56U, 78U}, true);
        }
        std::memcpy(out, self.bytes.data() + offset, size);
        return true;
    }

    static bool Write(void* context, const size_t offset,
                      const uint8_t* source, const size_t size) {
        auto& self = *static_cast<MemoryFile*>(context);
        if (source == nullptr || offset > self.bytes.size() ||
            size > self.bytes.size() - offset) return false;
        ++self.writeCalls;
        if (offset >= store::FrameStore::BodyOffset(0U) &&
            offset < store::FRAME_FILES_CAPACITY) {
            ++self.bodyWriteCalls;
        } else if (size == store::ROOT_BYTES) {
            self.rootWritePending = true;
            if (self.renderDuringRootWrite != nullptr) {
                std::atomic<bool> rendered{false};
                std::thread render([&] {
                    store::Frame frame{};
                    uint16_t index = 0U;
                    (void)self.renderDuringRootWrite->CopyDisplayed(frame, index);
                    rendered.store(true, std::memory_order_release);
                });
                for (uint16_t attempt = 0U; attempt < 200U &&
                     !rendered.load(std::memory_order_acquire); ++attempt) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (!rendered.load(std::memory_order_acquire)) {
                    std::fputs("[FAIL] mode root blocked render publication\n", stderr);
                    std::abort();
                }
                render.join();
            }
        }
        std::memcpy(self.bytes.data() + offset, source, size);
        return true;
    }

    static bool Sync(void* context) {
        auto& self = *static_cast<MemoryFile*>(context);
        if (self.rootWritePending && self.failRootSyncOnce) {
            self.failRootSyncOnce = false;
            self.rootWritePending = false;
            return false;
        }
        self.durable = self.bytes;
        self.rootWritePending = false;
        return true;
    }

    store::FrameStore::Ops Ops() { return {this, &Read, &Write, &Sync}; }
};

store::Frame RepeatedFrame() {
    store::Frame frame{};
    frame.durationMs = 50U;
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        frame.emitters[index].color = {
            static_cast<uint8_t>(index),
            static_cast<uint8_t>(index * 3U),
            static_cast<uint8_t>(index * 7U)};
        frame.emitters[index].effect = static_cast<uint8_t>(index % 9U);
        frame.emitters[index].effectDurationMs =
            frame.emitters[index].effect == 0U ? 0U :
            static_cast<uint16_t>(500U + (index % 96U) * 100U);
    }
    return frame;
}

bool Same(const store::Frame& left, const store::Frame& right) {
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

std::vector<uint8_t> RepeatedBody(
    const uint16_t frameCount = store::FRAME_COUNT) {
    const store::Frame frame = RepeatedFrame();
    std::vector<uint8_t> body;
    std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
    for (uint16_t index = 0U; index < frameCount; ++index) {
        size_t encodedSize = 0U;
        const store::Frame* previous =
            index % store::KEYFRAME_INTERVAL == 0U ? nullptr : &frame;
        Check(wire::FrameCodec::Encode(previous, frame, encoded.data(),
                                       encoded.size(), encodedSize),
              "256-frame receiver fixture encodes");
        const size_t start = body.size();
        body.resize(start + store::WIRE_RECORD_HEADER_BYTES + encodedSize);
        fxe::WriteU16LE(static_cast<uint16_t>(encodedSize),
                        body.data() + start);
        fxe::WriteU32LE(fxe::Crc32(encoded.data(), encodedSize),
                        body.data() + start + 2U);
        std::copy_n(encoded.data(), encodedSize,
                    body.data() + start + store::WIRE_RECORD_HEADER_BYTES);
    }
    return body;
}

store::Frame DenseFrame(const uint16_t marker) {
    store::Frame frame{};
    frame.durationMs = 50U;
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        auto& emitter = frame.emitters[index];
        emitter.color = {
            static_cast<uint8_t>(marker + index * 3U),
            static_cast<uint8_t>(marker * 5U + index * 7U),
            static_cast<uint8_t>(marker * 11U + index * 13U)};
        emitter.effect = static_cast<uint8_t>((marker + index) % 9U);
        emitter.effectDurationMs = emitter.effect == 0U ? 0U :
            static_cast<uint16_t>(500U + ((marker + index) % 96U) * 100U);
    }
    return frame;
}

std::vector<uint8_t> DenseBody() {
    std::vector<uint8_t> body;
    std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
    for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
        const store::Frame frame = DenseFrame(index);
        size_t encodedSize = 0U;
        Check(wire::FrameCodec::Encode(nullptr, frame, encoded.data(),
                                       encoded.size(), encodedSize),
              "dense 256-frame receiver fixture encodes");
        const size_t start = body.size();
        body.resize(start + store::WIRE_RECORD_HEADER_BYTES + encodedSize);
        fxe::WriteU16LE(static_cast<uint16_t>(encodedSize),
                        body.data() + start);
        fxe::WriteU32LE(fxe::Crc32(encoded.data(), encodedSize),
                        body.data() + start + 2U);
        std::copy_n(encoded.data(), encodedSize,
                    body.data() + start + store::WIRE_RECORD_HEADER_BYTES);
    }
    return body;
}

std::vector<uint8_t> Packet(const uint8_t operation, const uint32_t session,
                            const uint16_t transfer,
                            const size_t payloadBytes) {
    std::vector<uint8_t> packet(9U + payloadBytes, 0U);
    packet[0] = 0x4dU;
    packet[1] = 2U;
    packet[2] = operation;
    fxe::WriteU32LE(session, packet.data() + 3U);
    fxe::WriteU16LE(transfer, packet.data() + 7U);
    return packet;
}

std::vector<uint8_t> BeginPacket(const uint32_t session,
                                 const uint16_t transfer,
                                 const uint32_t generation,
                                 const std::vector<uint8_t>& body,
                                 const uint16_t frameCount =
                                     store::FRAME_COUNT,
                                 const bool predecessorDeltas = true) {
    auto packet = Packet(store::STORE_BEGIN, session, transfer, 44U);
    fxe::WriteU32LE(generation, packet.data() + 9U);
    fxe::WriteU32LE(static_cast<uint32_t>(body.size()), packet.data() + 13U);
    fxe::WriteU32LE(fxe::Crc32(body.data(), body.size()),
                    packet.data() + 17U);
    fxe::WriteU16LE(frameCount, packet.data() + 21U);
    packet[23] = 3U;
    packet[24] = static_cast<uint8_t>(3U | (predecessorDeltas ? 4U : 0U));
    fxe::WriteU32LE(0x13572468U, packet.data() + 25U);
    for (uint8_t index = 0U; index < 8U; ++index) {
        packet[29U + index * 3U] = index;
        packet[30U + index * 3U] = static_cast<uint8_t>(index + 10U);
        packet[31U + index * 3U] = static_cast<uint8_t>(index + 20U);
    }
    return packet;
}

std::vector<uint8_t> CursorPacket(const uint8_t operation,
                                  const uint32_t session,
                                  const uint16_t transfer,
                                  const uint32_t cursor) {
    auto packet = Packet(operation, session, transfer, 4U);
    fxe::WriteU32LE(cursor, packet.data() + 9U);
    return packet;
}

uint8_t Status(const store::StoreIngressResult& result) {
    return result.responseReady && result.responseSize >= 14U
        ? result.response[13] : 0xffU;
}

void BoundedRepeatIngressMatchesDurableRootAndCancelsBetweenFrames() {
    constexpr uint32_t SESSION = 42U, OWNER = 7U;
    const store::StoreProjection projection{SESSION, 0U, 3U, true, false};
    MemoryFile slicedFile, referenceFile;
    store::FrameStore frames(slicedFile.Ops()), reference(referenceFile.Ops());
    store::StoreReceiver receiver(frames), direct(reference);
    Check(receiver.Recover(projection) == store::Status::NoArtwork &&
        direct.Recover(projection) == store::Status::NoArtwork, "bounded repeat starts from equal blank stores");
    const auto body = RepeatedBody();
    const auto begin = BeginPacket(SESSION, 1U, 0U, body);
    auto result = Handle(receiver, begin.data(), begin.size(), OWNER);
    (void)Handle(direct, begin.data(), begin.size(), OWNER);
    size_t slices = 0U;
    for (size_t cursor = 0U; cursor < body.size(); cursor += 217U) {
        auto chunk = CursorPacket(store::STORE_CHUNK, SESSION, 1U, static_cast<uint32_t>(cursor));
        const size_t count = std::min<size_t>(217U, body.size() - cursor);
        chunk.insert(chunk.end(), body.begin() + cursor, body.begin() + cursor + count);
        uint16_t budget = 1U;
        receiver.Handle(chunk.data(), chunk.size(), OWNER, result, &budget);
        (void)Handle(direct, chunk.data(), chunk.size(), OWNER);
        Check(receiver.BodyCursor() == cursor + count && !result.responseReady,
            "bounded admission retains the complete Chunk before staging");
        while (receiver.BodyPending()) {
            const auto before = slicedFile.bodyWriteCalls;
            const auto stagedBefore = receiver.StagedBodyCursor();
            budget = 1U; receiver.ContinueBody(budget); ++slices;
            Check(slicedFile.bodyWriteCalls - before <= 1U && receiver.StagedBodyCursor() > stagedBefore,
                "REPEAT-heavy body advances consumed prefix with at most one complete frame per slice");
            const auto stoppedAt = receiver.StagedBodyCursor();
            budget = 0U; receiver.ContinueBody(budget);
            Check(receiver.StagedBodyCursor() == stoppedAt, "spent frame budget does not consume or CRC the suffix again");
        }
    }
    const auto commit = CursorPacket(store::STORE_COMMIT, SESSION, 1U, static_cast<uint32_t>(body.size()));
    result = Handle(receiver, commit.data(), commit.size(), OWNER);
    const auto expected = Handle(direct, commit.data(), commit.size(), OWNER);
    Check(Status(result) == 0U && Status(expected) == 0U && slicedFile.bodyWriteCalls == 256U &&
        frames.CurrentBodyCrc32() == fxe::Crc32(body.data(), body.size()) && slicedFile.durable == referenceFile.durable,
        "bounded parser preserves exact CRC, slots, summaries and durable root bytes");
    for (const uint16_t index : {0U, 31U, 32U, 255U}) {
        store::Frame frame{};
        Check(frames.ReadFrame(index, frame) == store::Status::Ok && Same(frame, RepeatedFrame()),
            "bounded predecessor and independent anchor boundaries retain exact frame semantics");
    }
    const auto replacement = BeginPacket(SESSION, 2U, frames.Generation(), body);
    (void)Handle(receiver, replacement.data(), replacement.size(), OWNER);
    auto chunk = CursorPacket(store::STORE_CHUNK, SESSION, 2U, 0U);
    chunk.insert(chunk.end(), body.begin(), body.begin() + 4096U);
    uint16_t budget = 1U;
    receiver.Handle(chunk.data(), chunk.size(), OWNER, result, &budget);
    receiver.ContinueBody(budget);
    Check(receiver.BodyPending() && receiver.StagedBodyCursor() < receiver.BodyCursor(),
        "compressed suffix remains owned between frame slices");
    const auto cancel = Packet(store::STORE_CANCEL, SESSION, 2U, 0U);
    result = Handle(receiver, cancel.data(), cancel.size(), OWNER);
    store::Frame preserved{};
    Check(Status(result) == 0U && !receiver.TransferActive() && !receiver.BodyPending() &&
        frames.ReadFrame(255U, preserved) == store::Status::Ok && Same(preserved, RepeatedFrame()),
        "Cancel between slices drops only incoming work and restores committed cache authority");
    const auto failing = BeginPacket(SESSION, 3U, frames.Generation(), body);
    (void)Handle(receiver, failing.data(), failing.size(), OWNER);
    chunk = CursorPacket(store::STORE_CHUNK, SESSION, 3U, 0U);
    chunk.insert(chunk.end(), body.begin(), body.begin() + 4096U);
    chunk[13U + 6U] ^= 1U; // Corrupt first record while retaining a queued suffix.
    budget = 1U; receiver.Handle(chunk.data(), chunk.size(), OWNER, result, &budget);
    receiver.ContinueBody(budget);
    Check(!receiver.BodyPending(), "terminal record integrity failure cannot keep continuation wake spinning");
    const auto failedCommit = CursorPacket(store::STORE_COMMIT, SESSION, 3U, 4096U);
    result = Handle(receiver, failedCommit.data(), failedCommit.size(), OWNER);
    Check(Status(result) == uint8_t(store::WireStatus::IntegrityFailure) && frames.Generation() == 1U,
        "queued Commit reaches exact integrity refusal and preserves the prior root after a failed slice");
    std::printf("[BOUNDED_REPEAT_256] slices=%zu consumed=%zu crc=%08x cancel=preserved\n",
        slices, body.size(), frames.CurrentBodyCrc32());
}

void ReusedOutputDoesNotRetainPacketsOrAliasTerminalReceipt() {
    constexpr uint32_t SESSION = 77U;
    constexpr uint32_t OWNER = 9U;
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) ==
              store::Status::NoArtwork, "reused output starts without artwork");
    const auto body = RepeatedBody(1U);
    const auto begin = BeginPacket(SESSION, 1U, 0U, body, 1U, false);
    store::StoreIngressResult output{};
    output.handled = output.responseReady = output.playbackReady = true;
    output.response.fill(0xa5U);
    output.responseSize = store::STORE_RESPONSE_MAX_BYTES;
    receiver.Handle(begin.data(), begin.size(), OWNER, output);
    Check(output.handled && !output.responseReady && !output.playbackReady &&
              output.responseSize == 0U &&
              std::all_of(output.response.begin(), output.response.end(),
                          [](uint8_t byte) { return byte == 0U; }),
          "silent Begin resets every field in a previously used output slot");
    for (uint32_t cursor = 0U; cursor < body.size();) {
        const size_t count = std::min<size_t>(400U, body.size() - cursor);
        auto chunk = CursorPacket(store::STORE_CHUNK, SESSION, 1U, cursor);
        chunk.insert(chunk.end(), body.begin() + cursor,
                     body.begin() + cursor + count);
        receiver.Handle(chunk.data(), chunk.size(), OWNER, output);
        cursor += static_cast<uint32_t>(count);
    }
    const auto commit = CursorPacket(store::STORE_COMMIT, SESSION, 1U,
                                     static_cast<uint32_t>(body.size()));
    receiver.Handle(commit.data(), commit.size(), OWNER, output);
    const auto receipt = receiver.Receipt();
    Check(Status(output) == 0U && receipt.operation == store::STORE_COMMIT,
          "commit retains its terminal receipt independently of output slot");
    Check(output.responseSize == 66U && output.response[20U] == 1U &&
              output.response[19U] == output.response[21U] &&
              fxe::ReadU32LE(output.response.data() + 22U) == frames.Generation(),
          "successful Commit includes its selected resident proof without another lookup");
    auto inspect = Packet(1U, SESSION, 2U, 1U);
    inspect[9U] = store::RESIDENT_LISTING_PROBE;
    receiver.Handle(inspect.data(), inspect.size(), OWNER, output);
    Check(output.responseSize == 66U && output.response[20U] == 1U &&
              receiver.Receipt().response.response == receipt.response.response,
          "Inspect reuses one slot without changing the retained commit bytes");
    receiver.Handle(commit.data(), commit.size(), OWNER, output);
    Check(output.responseSize == receipt.response.responseSize &&
              output.response == receipt.response.response,
          "terminal replay copies exact original bytes into reused output");
    receiver.Handle(nullptr, 0U, OWNER, output);
    Check(!output.handled && !output.responseReady &&
              output.responseSize == 0U && !output.playbackReady,
          "ignored ingress cannot publish flags left by a previous response");
    output.responseReady = true;
    output.responseSize = 99U;
    receiver.ContinueRead(OWNER, output);
    Check(!output.responseReady && output.responseSize == 0U,
          "inactive read clears a reused response instead of replaying it");
}

void RgbWireCapabilityAndTransientBegin() {
    constexpr uint32_t SESSION = 0x12345678U;
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) ==
              store::Status::NoArtwork,
          "RGB receiver begins with no resident");
    auto inspect = Packet(1U, SESSION, 12U, 1U);
    inspect[9U] = store::RESIDENT_LISTING_PROBE;
    auto result = Handle(receiver, inspect.data(), inspect.size(), 7U);
    Check(Status(result) == 0U &&
              (result.response[14U] & 0x20U) != 0U &&
              (result.response[14U] & 0x18U) == 0x10U,
          "Inspect advertises RGB wire without changing topology bits");

    auto frame = RepeatedFrame();
    for (auto& emitter : frame.emitters) {
        emitter.effect = 0U;
        emitter.effectDurationMs = 0U;
    }
    std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
    size_t encodedSize = 0U;
    Check(wire::FrameCodec::Encode(nullptr, frame, encoded.data(),
              encoded.size(), encodedSize, wire::RgbOnlyMode::Enabled),
          "receiver fixture encodes an RGB raw anchor");
    std::vector<uint8_t> rgbBody(6U + encodedSize);
    fxe::WriteU16LE(static_cast<uint16_t>(encodedSize), rgbBody.data());
    fxe::WriteU32LE(fxe::Crc32(encoded.data(), encodedSize),
                    rgbBody.data() + 2U);
    std::copy_n(encoded.data(), encodedSize, rgbBody.data() + 6U);
    auto invalid = BeginPacket(SESSION, 41U, 0U, rgbBody, 1U, false);
    invalid[24U] |= 0x10U;
    result = Handle(receiver, invalid.data(), invalid.size(), 7U);
    Check(Status(result) == static_cast<uint8_t>(store::WireStatus::Malformed) &&
              !receiver.TransferActive(),
          "reserved Begin flags remain rejected");
    auto begin = BeginPacket(SESSION, 41U, 0U, rgbBody, 1U, false);
    begin[24U] |= 0x08U;
    result = Handle(receiver, begin.data(), begin.size(), 7U);
    Check(result.handled && !result.responseReady && receiver.TransferActive(),
          "Begin 0x08 admits transient RGB wire body");
    uint32_t cursor = 0U;
    while (cursor < rgbBody.size()) {
        const size_t count = std::min<size_t>(231U, rgbBody.size() - cursor);
        auto chunk = CursorPacket(store::STORE_CHUNK, SESSION, 41U, cursor);
        chunk.insert(chunk.end(), rgbBody.begin() + cursor,
                     rgbBody.begin() + cursor + count);
        (void)Handle(receiver, chunk.data(), chunk.size(), 7U);
        cursor += static_cast<uint32_t>(count);
    }
    auto commit = CursorPacket(store::STORE_COMMIT, SESSION, 41U, cursor);
    result = Handle(receiver, commit.data(), commit.size(), 7U);
    Check(Status(result) == 0U && frames.CurrentBodyBytes() != rgbBody.size() &&
              !frames.CurrentManifest().predecessorDeltas &&
              (result.response[14U] & 0x20U) != 0U,
          "Commit publishes legacy identity and retains RGB capability");
    store::FrameStore reboot(file.Ops());
    Check(reboot.Recover() == store::Status::Ok &&
              reboot.CurrentBodyBytes() == frames.CurrentBodyBytes(),
          "transient RGB Begin leaves a reboot-readable old root");
}

void SameSessionExitRetiresQueuedStoreTraffic() {
    for (const bool previewBeforeExit : {false, true}) {
        MemoryFile file;
        store::FrameStore frames(file.Ops());
        store::StoreReceiver receiver(frames);
        modules::litchcraft_frame_stream::FrameStream stream;
        modules::LitchcraftSession session;
        session.Configure(3U, 100U);
        const std::array<uint8_t, 8U> enter{0x4cU, 1U, 0U, 0U, 0U, 0U, 1U, 0U};
        session.HandlePacket(enter.data(), enter.size(), true, 0U);
        session.AttachFrameStream(&stream);
        const uint32_t ownerSession = session.SessionId();
        Check(session.IsActive() && ownerSession != 0U,
              "Exit journey enters through the production compact command");
        (void)receiver.Recover({ownerSession, 0U, 3U, true, false});
        const auto body = RepeatedBody();
        const auto begin = BeginPacket(ownerSession, 41U, 0U, body);
        (void)Handle(receiver, begin.data(), begin.size(), 7U);
        uint32_t cursor = 0U;
        auto deliverChunk = [&] {
            const size_t count = std::min<size_t>(231U, body.size() - cursor);
            auto packet = CursorPacket(store::STORE_CHUNK, ownerSession, 41U, cursor);
            packet.insert(packet.end(), body.begin() + cursor,
                          body.begin() + cursor + count);
            const auto result = Handle(receiver, packet.data(), packet.size(), 7U);
            if (result.playbackReady && receiver.IncomingPreviewReady()) {
                (void)stream.StageFirstFrame(receiver.IncomingManifest(),
                    receiver.IncomingGeneration(), receiver.IncomingFirstFrame());
            }
            cursor += static_cast<uint32_t>(count);
            session.Update();
        };
        if (previewBeforeExit) {
            while (!receiver.IncomingPreviewReady()) deliverChunk();
        }
        std::array<uint8_t, 8U> exit{0x4cU,
            static_cast<uint8_t>(modules::LitchcraftSession::Op::Exit),
            0U, 0U, 0U, 0U, 2U, 0U};
        fxe::WriteU32LE(ownerSession, exit.data() + 2U);
        session.HandlePacket(exit.data(), exit.size(), true, 0U);
        Check(!session.IsActive() && session.SessionId() == ownerSession,
              "compact Exit accepts without rolling the session id");
        // This is the production worker's projection seam. Queued BLE body
        // packets still arrive after this mode-authority notification.
        receiver.SetProjection({session.SessionId(), 0U, 3U, session.IsActive(), false});
        Check(!receiver.TransferActive() && !receiver.IncomingPreviewReady(),
              "same-session Exit revokes staging and its preview");
        while (cursor < body.size()) deliverChunk();
        const auto commit = CursorPacket(store::STORE_COMMIT, ownerSession, 41U, cursor);
        const auto result = Handle(receiver, commit.data(), commit.size(), 7U);
        session.Update();
        Check(Status(result) != 0U && frames.Generation() == 0U &&
                  !session.IsActive(),
              "queued chunks and Commit after Exit cannot reactivate or persist artwork");
        const auto replay = Handle(receiver, begin.data(), begin.size(), 7U);
        Check(Status(replay) != 0U && !receiver.TransferActive(),
              "late active Begin cannot reacquire the exited mode");
    }
}

void StreamsExact256FrameBodyAndPublishesOneRoot() {
    constexpr uint32_t SESSION = 0x12345678U;
    constexpr uint32_t OWNER = 7U;
    constexpr uint16_t TRANSFER = 41U;
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) ==
              store::Status::NoArtwork,
          "fresh receiver recovers with no artwork");

    auto inspect = Packet(1U, SESSION, 9U, 1U);
    inspect[9] = store::RESIDENT_LISTING_PROBE;
    auto result = Handle(receiver, inspect.data(), inspect.size(), OWNER);
    Check(Status(result) == 0U && fxe::ReadU32LE(result.response.data() + 7U) == 0U &&
              fxe::ReadU16LE(result.response.data() + 15U) == 0U &&
              fxe::ReadU16LE(result.response.data() + 17U) == 0U &&
              (result.response[14] & 4U) == 0U,
          "current Inspect reports a coherent fresh Store state");
    Check(Status(result) == 0U && result.responseSize == 21U &&
              result.response[19U] == 0xffU && result.response[20U] == 0U,
          "resident-listing Inspect includes an empty resident listing");
    for (const uint8_t oldProbe : {4U, 5U, 6U, 8U}) {
        inspect[9] = oldProbe;
        result = Handle(receiver, inspect.data(), inspect.size(), OWNER);
        Check(Status(result) == 4U,
              "noncurrent Store capability probes request a board update");
    }

    const auto body = RepeatedBody();
    Check(body.size() > 3424U &&
              body.size() <= store::WIRE_BODY_MAX_BYTES,
          "receiver fixture includes canonical 32-frame keyframes");
    const auto begin = BeginPacket(SESSION, TRANSFER, 0U, body);
    result = Handle(receiver, begin.data(), begin.size(), OWNER);
    Check(begin.size() == 53U && result.handled && !result.responseReady &&
              receiver.TransferActive(),
          "exact 53-byte Store begin admits optimistically without an ACK barrier");
    result = Handle(receiver, begin.data(), begin.size(), OWNER);
    Check(result.handled && !result.responseReady &&
              receiver.BodyCursor() == 0U,
          "exact duplicate Store begin remains silent without resetting progress");

    uint32_t cursor = 0U;
    uint16_t chunks = 0U;
    uint16_t previewSignals = 0U;
    while (cursor < body.size()) {
        const size_t count = std::min<size_t>(231U, body.size() - cursor);
        auto chunk = CursorPacket(
            store::STORE_CHUNK, SESSION, TRANSFER, cursor);
        chunk.insert(chunk.end(), body.begin() + cursor,
                     body.begin() + cursor + count);
        result = Handle(receiver, chunk.data(), chunk.size(), OWNER);
        Check(result.handled && !result.responseReady,
              "Store chunk is accepted without notification pressure");
        cursor += static_cast<uint32_t>(count);
        ++chunks;
        if (result.playbackReady) {
            ++previewSignals;
        }
    }
    Check(chunks == (body.size() + 230U) / 231U &&
              receiver.BodyCursor() == body.size(),
          "all 256 repeated frames cross bounded ATT244 packets");
    Check(previewSignals == 0U &&
              receiver.IncomingPreviewReady() &&
              receiver.IncomingGeneration() == 1U &&
              receiver.IncomingManifest().frameCount == store::FRAME_COUNT &&
              Same(receiver.IncomingFirstFrame(), RepeatedFrame()),
          "frame zero is decoded internally but never presented before complete validation");
    Check(file.bodyWriteCalls == store::FRAME_COUNT,
          "complete radio upload writes one fixed slot per frame before Commit");
    auto checkpoint = CursorPacket(
        store::STORE_CHECKPOINT, SESSION, TRANSFER, cursor);
    result = Handle(receiver, checkpoint.data(), checkpoint.size(), OWNER);
    Check(Status(result) == 0U && receiver.TransferActive() &&
              file.bodyWriteCalls == store::FRAME_COUNT,
          "checkpoint adds no write after final-chunk completion");
    receiver.SetProjection({SESSION, 7U, 3U, true, false});
    result = Handle(receiver, checkpoint.data(), checkpoint.size(), OWNER);
    Check(Status(result) == 0U &&
              result.response[2] == store::STORE_CHECKPOINT_RECEIPT &&
              result.responseSize == 18U &&
              fxe::ReadU32LE(result.response.data() + 14U) == cursor,
          "lost Checkpoint receipt replays its exact frozen cursor");
    auto commit = CursorPacket(
        store::STORE_COMMIT, SESSION, TRANSFER, cursor);
    result = Handle(receiver, commit.data(), commit.size(), OWNER);
    Check(Status(result) == 0U && !receiver.TransferActive() &&
              fxe::ReadU32LE(result.response.data() + 7U) == 1U &&
              fxe::ReadU16LE(result.response.data() + 15U) == 256U &&
              fxe::ReadU16LE(result.response.data() + 17U) == 0U &&
              (result.response[14] & 7U) == 5U,
          "commit publishes generation one and exact 256-frame Store state");
    inspect[9] = store::RESIDENT_LISTING_PROBE;
    result = Handle(receiver, inspect.data(), inspect.size(), OWNER);
    Check(Status(result) == 0U && result.responseSize == 66U &&
              result.response[20U] == 1U && result.response[19U] < 2U &&
              fxe::ReadU32LE(result.response.data() + 22U) == 1U &&
              fxe::ReadU32LE(result.response.data() + 26U) ==
                  frames.CurrentBodyCrc32(),
          "resident-listing Inspect includes the committed resident descriptor");
    result = Handle(receiver, commit.data(), commit.size(), OWNER);
    Check(Status(result) == 0U && frames.Generation() == 1U &&
              fxe::ReadU32LE(result.response.data() + 7U) == 1U,
          "lost Commit receipt replays without publishing another root");
    store::Frame decoded{};
    Check(frames.ReadFrame(255U, decoded) == store::Status::Ok &&
              Same(decoded, RepeatedFrame()),
          "committed frame 255 decodes losslessly from the production store");
}

void CommitsDense256BodyWithoutCheckpoint() {
    constexpr uint32_t SESSION = 0x2468ace0U;
    constexpr uint32_t OWNER = 11U;
    constexpr uint16_t TRANSFER = 91U;
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) ==
              store::Status::NoArtwork,
          "dense no-checkpoint fixture starts without artwork");

    const auto body = DenseBody();
    Check(body.size() > 54U * 231U,
          "dense 256-frame body crosses the high-MTU 54-object window");
    const auto begin = BeginPacket(SESSION, TRANSFER, 0U, body,
                                   store::FRAME_COUNT, false);
    auto result = Handle(receiver, begin.data(), begin.size(), OWNER);
    Check(result.handled && !result.responseReady && receiver.TransferActive(),
          "dense no-checkpoint Begin is admitted optimistically");

    uint32_t cursor = 0U;
    while (cursor < body.size()) {
        const size_t count = std::min<size_t>(231U, body.size() - cursor);
        auto chunk = CursorPacket(
            store::STORE_CHUNK, SESSION, TRANSFER, cursor);
        chunk.insert(chunk.end(), body.begin() + cursor,
                     body.begin() + cursor + count);
        result = Handle(receiver, chunk.data(), chunk.size(), OWNER);
        Check(result.handled && !result.responseReady,
              "dense body chunk remains an admission-only event");
        cursor += static_cast<uint32_t>(count);
    }

    const auto commit = CursorPacket(
        store::STORE_COMMIT, SESSION, TRANSFER, cursor);
    result = Handle(receiver, commit.data(), commit.size(), OWNER);
    Check(Status(result) == 0U && !receiver.TransferActive() &&
              frames.Generation() == 1U &&
              fxe::ReadU16LE(result.response.data() + 15U) == 256U,
          "dense body commits directly after cumulative Object admission");
    Check(file.bodyWriteCalls == store::FRAME_COUNT,
          "dense 256-frame body writes one fixed slot per frame");
}

void PredecessorBodyKeepsFrameZeroPreviewStable() {
    constexpr uint32_t SESSION = 0x12345678U;
    constexpr uint32_t OWNER = 9U;
    constexpr uint16_t TRANSFER = 73U;
    const store::Frame first = RepeatedFrame();
    store::Frame second = first;
    second.durationMs = 137U;
    second.emitters[91U].color = {0xffU, 0x01U, 0x80U};
    std::vector<uint8_t> body;
    std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
    const store::Frame frames[]{first, second};
    for (size_t index = 0U; index < std::size(frames); ++index) {
        size_t encodedSize = 0U;
        const store::Frame* previous = index == 0U ? nullptr : &frames[index - 1U];
        Check(wire::FrameCodec::Encode(previous, frames[index], encoded.data(),
                                       encoded.size(), encodedSize),
              "preview predecessor fixture encodes");
        const size_t start = body.size();
        body.resize(start + store::WIRE_RECORD_HEADER_BYTES + encodedSize);
        fxe::WriteU16LE(static_cast<uint16_t>(encodedSize), body.data() + start);
        fxe::WriteU32LE(fxe::Crc32(encoded.data(), encodedSize),
                        body.data() + start + 2U);
        std::copy_n(encoded.data(), encodedSize,
                    body.data() + start + store::WIRE_RECORD_HEADER_BYTES);
    }

    MemoryFile file;
    store::FrameStore frameStore(file.Ops());
    store::StoreReceiver receiver(frameStore);
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) ==
              store::Status::NoArtwork,
          "predecessor preview fixture starts blank");
    auto begin = BeginPacket(
        SESSION, TRANSFER, 0U, body, 2U, true);
    auto result = Handle(receiver, begin.data(), begin.size(), OWNER);
    auto chunk = CursorPacket(store::STORE_CHUNK, SESSION, TRANSFER, 0U);
    chunk.insert(chunk.end(), body.begin(), body.end());
    result = Handle(receiver, chunk.data(), chunk.size(), OWNER);
    auto checkpoint = CursorPacket(
        store::STORE_CHECKPOINT, SESSION, TRANSFER,
        static_cast<uint32_t>(body.size()));
    result = Handle(receiver, checkpoint.data(), checkpoint.size(), OWNER);
    Check(Status(result) == 0U && receiver.IncomingPreviewReady() &&
              receiver.IncomingManifest().predecessorDeltas &&
              Same(receiver.IncomingFirstFrame(), first),
          "frame-zero preview remains stable after later predecessor records decode");
}

void DirectChunkAdmissionUsesTheSamePrefixDrain() {
    constexpr uint32_t SESSION = 0x12345678U;
    constexpr uint32_t OWNER = 7U;
    constexpr uint16_t TRANSFER = 42U;
    constexpr uint16_t FRAME_COUNT = 5U;
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) ==
              store::Status::NoArtwork,
          "small-body receiver starts without artwork");

    const auto body = RepeatedBody(FRAME_COUNT);
    Check(body.size() < store::StoreReceiver::WRITE_BATCH_BYTES,
          "five-frame body fits the shared ingress workspace");
    const auto begin = BeginPacket(
        SESSION, TRANSFER, 0U, body, FRAME_COUNT);
    auto result = Handle(receiver, begin.data(), begin.size(), OWNER);
    Check(result.handled && !result.responseReady,
          "small-body Begin is admitted optimistically");

    uint32_t cursor = 0U;
    while (cursor < body.size()) {
        const size_t count = std::min<size_t>(231U, body.size() - cursor);
        auto chunk = CursorPacket(
            store::STORE_CHUNK, SESSION, TRANSFER, cursor);
        chunk.insert(chunk.end(), body.begin() + cursor,
                     body.begin() + cursor + count);
        result = Handle(receiver, chunk.data(), chunk.size(), OWNER);
        cursor += static_cast<uint32_t>(count);
    }
    Check(!result.playbackReady && receiver.IncomingPreviewReady() &&
              file.bodyWriteCalls == FRAME_COUNT,
          "small artwork stays invisible until Commit after writing its fixed slots");
}

void AcceptedArtworkSurvivesFailedRootAndLatePaint() {
    constexpr uint32_t SESSION = 0x12345678U;
    constexpr uint32_t OWNER = 7U;
    constexpr uint16_t TRANSFER = 83U;
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    modules::litchcraft_frame_stream::FrameStream stream;
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) ==
              store::Status::NoArtwork,
          "root fault fixture starts without artwork");
    receiver.SetCommitCallbacks(
        [](void* context, uint32_t, uint32_t expected,
           const store::Manifest& manifest, uint32_t generation,
           const store::Frame&) {
            auto& live = *static_cast<modules::litchcraft_frame_stream::FrameStream*>(context);
            return live.TryReserveReplacement(expected, manifest, generation);
        },
        [](void* context, uint32_t, uint32_t expected,
           const store::Manifest& manifest, uint32_t generation,
           const store::Frame& first) {
            auto& live = *static_cast<modules::litchcraft_frame_stream::FrameStream*>(context);
            return live.PublishReservedReplacement(
                expected, manifest, generation, first);
        }, &stream);
    const auto body = RepeatedBody(1U);
    const auto begin = BeginPacket(SESSION, TRANSFER, 0U, body, 1U);
    (void)Handle(receiver, begin.data(), begin.size(), OWNER);
    auto chunk = CursorPacket(store::STORE_CHUNK, SESSION, TRANSFER, 0U);
    chunk.insert(chunk.end(), body.begin(), body.end());
    auto result = Handle(receiver, chunk.data(), chunk.size(), OWNER);
    Check(!result.playbackReady && stream.Generation() == 0U,
          "complete body stays invisible until Commit admission");
    file.failRootSyncOnce = true;
    const auto commit = CursorPacket(store::STORE_COMMIT, SESSION, TRANSFER,
                                     static_cast<uint32_t>(body.size()));
    result = Handle(receiver, commit.data(), commit.size(), OWNER);
    Check(!result.responseReady && receiver.CommitPending() &&
              stream.Generation() == 0U && frames.Generation() == 0U,
          "failed root sync keeps the old live artwork without success receipt");
    Check(!stream.PaintDisplayed(0U, 0U, 1U, {241U, 19U, 7U}, true) &&
              !stream.HasUnpersistedEdit(),
          "physical paint cannot enter the unpublished replacement");
    auto inspect = Packet(1U, SESSION, 90U, 1U);
    inspect[9U] = store::RESIDENT_LISTING_PROBE;
    result = Handle(receiver, inspect.data(), inspect.size(), OWNER);
    Check(Status(result) == static_cast<uint8_t>(store::WireStatus::ResourceLimit),
          "Inspect does not report the old root as current during publication");
    auto snapshot = Packet(store::STORE_SNAPSHOT, SESSION, 91U, 8U);
    fxe::WriteU32LE(0U, snapshot.data() + 9U);
    fxe::WriteU32LE(0U, snapshot.data() + 13U);
    result = Handle(receiver, snapshot.data(), snapshot.size(), OWNER);
    Check(result.responseReady &&
              result.response[2U] == store::STORE_SNAPSHOT_CHUNK &&
              Status(result) ==
                  static_cast<uint8_t>(store::WireStatus::ResourceLimit),
          "Snapshot cannot read the old root as the replacement during publication");
    snapshot.resize(19U);
    fxe::WriteU16LE(0xffffU, snapshot.data() + 17U);
    result = Handle(receiver, snapshot.data(), snapshot.size(), OWNER);
    Check(Status(result) == static_cast<uint8_t>(store::WireStatus::ResourceLimit) &&
              result.responseSize == 60U && !receiver.ReadActive(),
          "preview cannot read an old root while replacement publication is pending");
    auto cancel = Packet(store::STORE_CANCEL, SESSION, TRANSFER, 0U);
    result = Handle(receiver, cancel.data(), cancel.size(), OWNER);
    Check(Status(result) == static_cast<uint8_t>(store::WireStatus::Busy),
          "late Cancel cannot discard accepted artwork");
    receiver.EndOwner(OWNER);
    receiver.SetProjection({SESSION, 0U, 3U, false, false});
    Check(receiver.CommitPending() && stream.Generation() == 0U,
          "disconnect and projection change preserve the old live artwork until publication");
    MemoryFile beforeCut;
    beforeCut.bytes = file.durable;
    store::FrameStore beforeCutStore(beforeCut.Ops());
    Check(beforeCutStore.Recover() == store::Status::NoArtwork,
          "power cut before durable root recovers the prior authority");
    result = RetryPendingCommit(receiver);
    Check(Status(result) == 0U && !receiver.CommitPending() &&
              frames.Generation() == stream.Generation() &&
              (result.response[14U] & 3U) == 0U,
          "retry publishes and adopts once without reversing same-session Exit");
    Check(stream.PaintDisplayed(0U, 0U, 1U, {241U, 19U, 7U}, true) &&
              stream.HasUnpersistedEdit(),
          "physical paint after durable publication belongs to the new generation");
    const auto service = stream.Service(frames, false);
    Check(service != modules::litchcraft_frame_stream::FrameStream::ServiceResult::IoFailure &&
              !stream.HasUnpersistedEdit(),
          "late physical paint persists after replacement root becomes durable");
    MemoryFile afterCut;
    afterCut.bytes = file.durable;
    store::FrameStore afterCutStore(afterCut.Ops());
    store::Frame painted{};
    Check(afterCutStore.Recover() == store::Status::Ok &&
              afterCutStore.ReadFrame(0U, painted) == store::Status::Ok &&
              painted.emitters[0].color.r == 241U,
          "power cut after retry recovers the replacement and later paint");
}

void RejectsOrderingCorruptionAndStaleOwnersWithoutReplacingRoot() {
    constexpr uint32_t SESSION = 0x12345678U;
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) ==
              store::Status::NoArtwork,
          "failure receiver starts blank");
    const auto body = RepeatedBody();

    auto stale = BeginPacket(SESSION + 1U, 1U, 0U, body);
    auto result = Handle(receiver, stale.data(), stale.size(), 1U);
    Check(Status(result) == 2U && !receiver.TransferActive(),
          "wrong session cannot acquire Store authority");

    auto begin = BeginPacket(SESSION, 2U, 0U, body);
    result = Handle(receiver, begin.data(), begin.size(), 1U);
    Check(result.handled && !result.responseReady,
          "valid failure-fixture Begin is optimistic");
    auto wrong = CursorPacket(store::STORE_CHUNK, SESSION, 2U, 1U);
    wrong.push_back(body[0]);
    result = Handle(receiver, wrong.data(), wrong.size(), 1U);
    auto checkpoint = CursorPacket(store::STORE_CHECKPOINT, SESSION, 2U, 0U);
    result = Handle(receiver, checkpoint.data(), checkpoint.size(), 1U);
    Check(Status(result) == 6U && frames.Generation() == 0U,
          "out-of-order chunk is invisible and checkpoint reports incomplete");
    auto cancel = Packet(store::STORE_CANCEL, SESSION, 2U, 0U);
    result = Handle(receiver, cancel.data(), cancel.size(), 1U);
    Check(Status(result) == 0U && !receiver.TransferActive(),
          "priority cancel retires the incomplete transfer");
    result = Handle(receiver, cancel.data(), cancel.size(), 1U);
    Check(Status(result) == 0U && !receiver.TransferActive(),
          "lost Cancel receipt replays without acquiring a transaction");

    begin = BeginPacket(SESSION, 4U, 0U, body);
    result = Handle(receiver, begin.data(), begin.size(), 3U);
    Check(result.handled && !result.responseReady &&
              receiver.TransferActive(),
          "old session starts a transfer before rollover");
    receiver.SetProjection({SESSION + 1U, 0U, 3U, true, false});
    begin = BeginPacket(SESSION + 1U, 5U, 0U, body);
    result = Handle(receiver, begin.data(), begin.size(), 3U);
    Check(result.handled && !result.responseReady &&
              receiver.Transfer() == 5U,
          "fresh session aborts stale transfer and admits immediately");
    cancel = Packet(store::STORE_CANCEL, SESSION + 1U, 5U, 0U);
    (void)Handle(receiver, cancel.data(), cancel.size(), 3U);
    receiver.SetProjection({SESSION, 0U, 3U, true, false});

    begin = BeginPacket(SESSION, 3U, 0U, body);
    result = Handle(receiver, begin.data(), begin.size(), 2U);
    Check(result.handled && !result.responseReady,
          "corruption fixture Begin is optimistic");
    auto corrupt = body;
    corrupt[store::WIRE_RECORD_HEADER_BYTES + 17U] ^= 0x80U;
    uint32_t cursor = 0U;
    while (cursor < corrupt.size()) {
        const size_t count = std::min<size_t>(231U, corrupt.size() - cursor);
        auto chunk = CursorPacket(store::STORE_CHUNK, SESSION, 3U, cursor);
        chunk.insert(chunk.end(), corrupt.begin() + cursor,
                     corrupt.begin() + cursor + count);
        (void)Handle(receiver, chunk.data(), chunk.size(), 2U);
        cursor += static_cast<uint32_t>(count);
    }
    checkpoint = CursorPacket(store::STORE_CHECKPOINT, SESSION, 3U,
                              receiver.BodyCursor());
    result = Handle(receiver, checkpoint.data(), checkpoint.size(), 2U);
    Check(Status(result) == 11U && frames.Generation() == 0U,
          "per-frame corruption reports integrity failure before publication");
    receiver.EndOwner(2U);
    Check(!receiver.TransferActive() && frames.Generation() == 0U,
          "disconnect owner retirement preserves the prior root");
}

void DownloadsCommittedBodyWithoutCreatingAnotherAuthority() {
    constexpr uint32_t SESSION = 0x24681357U;
    constexpr uint32_t OWNER = 9U;
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) ==
              store::Status::NoArtwork,
          "snapshot fixture starts blank");
    const auto body = RepeatedBody(256U);
    const auto begin = BeginPacket(SESSION, 10U, 0U, body, 256U, true);
    (void)Handle(receiver, begin.data(), begin.size(), OWNER);
    uint32_t cursor = 0U;
    while (cursor < body.size()) {
        const size_t count = std::min<size_t>(231U, body.size() - cursor);
        auto chunk = CursorPacket(store::STORE_CHUNK, SESSION, 10U, cursor);
        chunk.insert(chunk.end(), body.begin() + cursor,
                     body.begin() + cursor + count);
        (void)Handle(receiver, chunk.data(), chunk.size(), OWNER);
        cursor += static_cast<uint32_t>(count);
    }
    const auto commit = CursorPacket(store::STORE_COMMIT, SESSION, 10U, cursor);
    Check(Status(Handle(receiver, commit.data(), commit.size(), OWNER)) == 0U &&
              frames.Generation() == 1U,
          "snapshot fixture commits one root");

    std::vector<uint8_t> downloaded;
    cursor = 0U;
    constexpr uint16_t request = 20U;
    auto snapshot = Packet(store::STORE_SNAPSHOT, SESSION, request, 8U);
    fxe::WriteU32LE(1U, snapshot.data() + 9U);
    fxe::WriteU32LE(0U, snapshot.data() + 13U);
    auto result = Handle(receiver, snapshot.data(), snapshot.size(), OWNER);
    size_t reversePackets = 0U;
    while (cursor < body.size()) {
        ++reversePackets;
        Check(result.responseReady &&
                  result.response[2U] == store::STORE_SNAPSHOT_CHUNK &&
                  result.response[13U] == 0U &&
                  result.responseSize > 60U &&
                  result.responseSize <= 498U &&
                  fxe::ReadU32LE(result.response.data() + 7U) == 1U &&
                  fxe::ReadU32LE(result.response.data() + 20U) == body.size() &&
                  fxe::ReadU32LE(result.response.data() + 28U) == cursor,
              "snapshot chunk carries coherent root metadata and cursor");
        downloaded.insert(downloaded.end(), result.response.begin() + 60U,
                          result.response.begin() + result.responseSize);
        cursor += static_cast<uint32_t>(result.responseSize - 60U);
        if (cursor < body.size()) {
            Check(receiver.ReadActive(),
                  "256-frame download keeps one bounded read owner");
            result = ContinueRead(receiver, OWNER);
        }
    }
    Check(downloaded == body && frames.Generation() == 1U &&
              !receiver.TransferActive() && !receiver.ReadActive(),
          "one request streams 256 frames without new authority");
    Check(reversePackets == (body.size() + 437U) / 438U,
          "256-frame read uses one bounded reverse Object packet per chunk");

    receiver.SetSnapshotPacketMaximum(20U);
    const size_t tailCursor = body.size() - 120U;
    auto smallLink = Packet(store::STORE_SNAPSHOT, SESSION, request + 1U, 8U);
    fxe::WriteU32LE(1U, smallLink.data() + 9U);
    fxe::WriteU32LE(static_cast<uint32_t>(tailCursor),
                    smallLink.data() + 13U);
    auto smallResult = Handle(receiver,
        smallLink.data(), smallLink.size(), OWNER);
    size_t smallCursor = tailCursor;
    std::vector<uint8_t> smallTail;
    while (smallCursor < body.size()) {
        Check(smallResult.responseReady && smallResult.responseSize > 60U &&
                  smallResult.responseSize <= 100U &&
                  fxe::ReadU32LE(smallResult.response.data() + 28U) ==
                      smallCursor,
              "minimum-MTU read bounds each complete response and cursor");
        smallTail.insert(smallTail.end(),
                         smallResult.response.begin() + 60U,
                         smallResult.response.begin() +
                             smallResult.responseSize);
        smallCursor += smallResult.responseSize - 60U;
        if (smallCursor < body.size()) {
            smallResult = ContinueRead(receiver, OWNER);
        }
    }
    Check(std::equal(smallTail.begin(), smallTail.end(),
                     body.begin() + tailCursor) && !receiver.ReadActive(),
          "minimum-MTU fragmented read completes a verified suffix");

    auto stale = Packet(store::STORE_SNAPSHOT, SESSION, request + 1U, 8U);
    fxe::WriteU32LE(2U, stale.data() + 9U);
    fxe::WriteU32LE(0U, stale.data() + 13U);
    const auto rejected = Handle(receiver, stale.data(), stale.size(), OWNER);
    Check(rejected.responseReady && rejected.responseSize == 60U &&
              rejected.response[13U] == 9U,
          "snapshot generation change fails closed before body read");
    result = Handle(receiver, snapshot.data(), snapshot.size(), OWNER);
    Check(receiver.ReadActive() && result.responseReady,
          "restarted full read owns a generation-fenced suffix");
    receiver.EndOwner(OWNER);
    Check(!receiver.ReadActive() &&
              !ContinueRead(receiver, OWNER).responseReady,
          "disconnect retires the read without publishing partial artwork");
}

std::vector<uint8_t> KeyframeRecord(const store::Frame& frame,
                                    const store::Frame* previous = nullptr) {
    std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
    size_t encodedSize = 0U;
    Check(wire::FrameCodec::Encode(previous, frame, encoded.data(),
                                   encoded.size(), encodedSize),
          "current-frame preview fixture encodes");
    std::vector<uint8_t> record(store::WIRE_RECORD_HEADER_BYTES + encodedSize);
    fxe::WriteU16LE(static_cast<uint16_t>(encodedSize), record.data());
    fxe::WriteU32LE(fxe::Crc32(encoded.data(), encodedSize), record.data() + 2U);
    std::copy_n(encoded.data(), encodedSize,
                record.data() + store::WIRE_RECORD_HEADER_BYTES);
    return record;
}

std::vector<uint8_t> CommitDenseFrames(MemoryFile& file,
                                       const uint16_t frameCount) {
    store::FrameStore frames(file.Ops());
    Check(frames.Recover() == store::Status::NoArtwork,
          "preview fixture starts without a root");
    store::Manifest manifest{};
    manifest.frameCount = frameCount;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    manifest.predecessorDeltas = true;
    manifest.sparkleSeed = 0x13572468U;
    store::TransactionToken token{};
    Check(frames.Begin(manifest, 0U, 7U, token) == store::Status::Ok,
          "preview fixture begins its ordinary Store transaction");
    store::Frame previous{};
    std::vector<uint8_t> body;
    for (uint16_t index = 0U; index < frameCount; ++index) {
        const auto frame = DenseFrame(index);
        const auto record = KeyframeRecord(frame,
            index % store::KEYFRAME_INTERVAL == 0U ? nullptr : &previous);
        body.insert(body.end(), record.begin(), record.end());
        Check(frames.WriteFrame(token, index, frame) == store::Status::Ok,
              "preview fixture writes each independent fixed slot");
        previous = frame;
    }
    Check(frames.Commit(token) == store::Status::Ok &&
              frames.CurrentBodyBytes() == body.size() &&
              frames.CurrentBodyCrc32() == fxe::Crc32(body.data(), body.size()),
          "preview fixture commits the canonical predecessor body");
    return body;
}

std::vector<uint8_t> PreviewPacket(const uint32_t session,
                                   const uint16_t request,
                                   const uint32_t generation,
                                   const uint16_t frameIndex,
                                   const uint32_t cursor = 0U) {
    auto packet = Packet(store::STORE_SNAPSHOT, session, request, 10U);
    fxe::WriteU32LE(generation, packet.data() + 9U);
    fxe::WriteU32LE(cursor, packet.data() + 13U);
    fxe::WriteU16LE(frameIndex, packet.data() + 17U);
    return packet;
}

void Cold256FrameCurrentPreviewIsBoundedAndFrozenDuringPlayback() {
    constexpr uint32_t SESSION = 0x24681357U;
    constexpr uint32_t OWNER = 9U;
    constexpr uint16_t SELECTED = 108U;
    MemoryFile file;
    const auto fullBody = CommitDenseFrames(file, 256U);
    const auto expected = KeyframeRecord(DenseFrame(SELECTED));
    const auto originalBytes = file.bytes;
    const size_t writes = file.writeCalls;
    // Reconstruct from durable roots so the observed frame is genuinely cold.
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) == store::Status::Ok,
          "cold 256-frame chooser fixture recovers its existing root");
    receiver.SetProjection({SESSION, SELECTED, 3U, true, true});
    file.readCalls = 0U;
    file.readBytes = 0U;
    const auto preview = PreviewPacket(SESSION, 20U, 1U, 0xffffU);
    auto result = Handle(receiver, preview.data(), preview.size(), OWNER);
    Check(expected.size() > 438U && expected.size() <=
              store::WIRE_RECORD_HEADER_BYTES + store::WIRE_FRAME_MAX_BYTES,
          "cold chosen frame requires multiple bounded reverse receipts");
    std::vector<uint8_t> downloaded;
    uint32_t cursor = 0U;
    size_t packets = 0U;
    while (cursor < expected.size() && packets < 4U) {
        ++packets;
        Check(result.responseReady && result.responseSize > 60U &&
                  result.responseSize <= 498U &&
                  result.response[2U] == store::STORE_SNAPSHOT_CHUNK &&
                  Status(result) == 0U &&
                  (result.response[14U] & 0x28U) == 0x20U &&
                  fxe::ReadU16LE(result.response.data() + 16U) == 256U &&
                  fxe::ReadU16LE(result.response.data() + 18U) == SELECTED &&
                  fxe::ReadU32LE(result.response.data() + 20U) == expected.size() &&
                  fxe::ReadU32LE(result.response.data() + 24U) ==
                      fxe::Crc32(expected.data(), expected.size()) &&
                  fxe::ReadU32LE(result.response.data() + 28U) == cursor,
              "preview freezes frame 108 with actual count and one-record identity");
        if (!result.responseReady || result.responseSize <= 60U) break;
        downloaded.insert(downloaded.end(), result.response.begin() + 60U,
                          result.response.begin() + result.responseSize);
        cursor += result.responseSize - 60U;
        // The thumbnail observes the original frame while production playback
        // remains free to select the next frame and later pause.
        receiver.SetProjection({SESSION, SELECTED + 1U, 3U, true,
                                 packets == 1U});
        if (cursor < expected.size()) result = ContinueRead(receiver, OWNER);
    }
    store::Frame decoded{};
    Check(downloaded == expected &&
              fxe::ReadU16LE(downloaded.data()) +
                  store::WIRE_RECORD_HEADER_BYTES == downloaded.size() &&
              fxe::ReadU32LE(downloaded.data() + 2U) == fxe::Crc32(
                  downloaded.data() + store::WIRE_RECORD_HEADER_BYTES,
                  downloaded.size() - store::WIRE_RECORD_HEADER_BYTES) &&
              wire::FrameCodec::Decode(nullptr,
                  downloaded.data() + store::WIRE_RECORD_HEADER_BYTES,
                  downloaded.size() - store::WIRE_RECORD_HEADER_BYTES, decoded) &&
              Same(decoded, DenseFrame(SELECTED)),
          "preview is exactly one CRC-verified independently decodable keyframe");
    Check(file.readCalls == 1U && file.readBytes == store::STAGE_SLOT_BYTES &&
              packets == (expected.size() + 437U) / 438U &&
              !receiver.ReadActive() && !ContinueRead(receiver, OWNER).responseReady,
          "cold 256-frame chooser reads one fixed slot and closes at its record end");
    Check(frames.Generation() == 1U && frames.CurrentBodyBytes() == fullBody.size() &&
              file.writeCalls == writes && file.bytes == originalBytes,
          "read-only preview preserves document, durable bytes and generation");
    auto inspect = Packet(1U, SESSION, 21U, 1U);
    inspect[9U] = store::RESIDENT_LISTING_PROBE;
    result = Handle(receiver, inspect.data(), inspect.size(), OWNER);
    Check(fxe::ReadU16LE(result.response.data() + 17U) == SELECTED + 1U &&
              (result.response[14U] & 3U) == 1U,
          "preview does not mutate current selection or playback state");

    const auto resume = PreviewPacket(SESSION, 22U, 1U, SELECTED, 438U);
    result = Handle(receiver, resume.data(), resume.size(), OWNER);
    Check(Status(result) == 0U &&
              fxe::ReadU16LE(result.response.data() + 18U) == SELECTED &&
              std::equal(result.response.begin() + 60U,
                  result.response.begin() + result.responseSize,
                  expected.begin() + 438U),
          "concrete captured frame resumes while current selection has advanced");
    receiver.EndOwner(OWNER);

    // Returning to full Snapshot must discard the preview's shared wire page.
    auto full = Packet(store::STORE_SNAPSHOT, SESSION, 23U, 8U);
    fxe::WriteU32LE(1U, full.data() + 9U);
    fxe::WriteU32LE(438U, full.data() + 13U);
    result = Handle(receiver, full.data(), full.size(), OWNER);
    Check(Status(result) == 0U && (result.response[14U] & 0x28U) == 8U &&
              fxe::ReadU32LE(result.response.data() + 20U) == fullBody.size() &&
              fxe::ReadU32LE(result.response.data() + 24U) ==
                  fxe::Crc32(fullBody.data(), fullBody.size()) &&
              std::equal(result.response.begin() + 60U,
                  result.response.begin() + result.responseSize,
                  fullBody.begin() + 438U),
          "unchanged 17-byte Snapshot resumes canonical full body after preview");
    receiver.EndOwner(OWNER);
    receiver.SetSnapshotPacketMaximum(20U);
    const auto small = PreviewPacket(SESSION, 24U, 1U, SELECTED,
        static_cast<uint32_t>(expected.size() - 45U));
    result = Handle(receiver, small.data(), small.size(), OWNER);
    Check(Status(result) == 0U && result.responseSize == 100U &&
              receiver.ReadActive(),
          "minimum-MTU preview uses existing bounded reverse credit owner");
    result = ContinueRead(receiver, OWNER);
    Check(Status(result) == 0U && result.responseSize == 65U &&
              !receiver.ReadActive() && std::equal(result.response.begin() + 60U,
                  result.response.begin() + 65U, expected.end() - 5U),
          "minimum-MTU preview stops at the exact record end");
}

void PreviewRejectsMalformedStaleAndDepartedOwnersWithoutMutation() {
    constexpr uint32_t SESSION = 1234U;
    constexpr uint32_t OWNER = 9U;
    MemoryFile file;
    (void)CommitDenseFrames(file, 4U);
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) == store::Status::Ok,
          "preview guard fixture recovers");
    const auto bytes = file.bytes;
    const size_t writes = file.writeCalls;
    file.readCalls = 0U;
    auto preview = PreviewPacket(SESSION, 30U, 1U, 2U);
    for (const size_t size : {18U, 20U}) {
        auto malformed = preview;
        malformed.resize(size);
        Check(Status(Handle(receiver, malformed.data(), malformed.size(), OWNER)) ==
                  static_cast<uint8_t>(store::WireStatus::Malformed),
              "preview rejects every partial or oversized selector suffix");
    }
    for (const uint16_t index : {4U, 256U, 0xfffeU}) {
        const auto invalid = PreviewPacket(SESSION, 31U, 1U, index);
        Check(Status(Handle(receiver, invalid.data(), invalid.size(), OWNER)) ==
                  static_cast<uint8_t>(store::WireStatus::Malformed),
              "preview rejects a selector outside the actual frame count");
    }
    const auto invalidCapture = PreviewPacket(SESSION, 31U, 1U, 0xffffU, 1U);
    Check(Status(Handle(receiver, invalidCapture.data(), invalidCapture.size(), OWNER)) ==
              static_cast<uint8_t>(store::WireStatus::Malformed),
          "current-selection capture is admitted only at cursor zero");
    const auto stale = PreviewPacket(SESSION, 32U, 2U, 2U);
    Check(Status(Handle(receiver, stale.data(), stale.size(), OWNER)) ==
              static_cast<uint8_t>(store::WireStatus::RevisionConflict),
          "preview generation mismatch fails before any frame read");
    auto wrongSession = PreviewPacket(SESSION + 1U, 32U, 1U, 2U);
    Check(Status(Handle(receiver, wrongSession.data(), wrongSession.size(), OWNER)) ==
              static_cast<uint8_t>(store::WireStatus::StaleSession) &&
              Status(Handle(receiver, preview.data(), preview.size(), 0U)) ==
                  static_cast<uint8_t>(store::WireStatus::StaleSession) &&
              file.readCalls == 0U && file.writeCalls == writes && file.bytes == bytes,
          "malformed generation/session/owner input cannot read or mutate artwork");

    auto result = Handle(receiver, preview.data(), preview.size(), OWNER);
    Check(Status(result) == 0U && receiver.ReadActive(),
          "concrete noncurrent frame uses the same observational read owner");
    Check(!ContinueRead(receiver, OWNER + 1U).responseReady && !receiver.ReadActive(),
          "changed connection owner retires captured preview");
    (void)Handle(receiver, preview.data(), preview.size(), OWNER);
    receiver.EndOwner(OWNER);
    Check(!receiver.ReadActive() && !ContinueRead(receiver, OWNER).responseReady,
          "disconnect retires preview");
    for (const store::StoreProjection departure : {
            store::StoreProjection{SESSION + 1U, 0U, 3U, true, false},
            store::StoreProjection{SESSION, 0U, 3U, false, false},
            store::StoreProjection{SESSION, 0U, 2U, true, false}}) {
        receiver.SetProjection({SESSION, 0U, 3U, true, false});
        (void)Handle(receiver, preview.data(), preview.size(), OWNER);
        receiver.SetProjection(departure);
        Check(!receiver.ReadActive() && !ContinueRead(receiver, OWNER).responseReady,
              "mode session, active authority or topology departure retires preview");
    }
    receiver.SetProjection({SESSION, 0U, 3U, true, false});
    (void)Handle(receiver, preview.data(), preview.size(), OWNER);
    store::TransactionToken token{};
    Check(frames.BeginEdit(1U, OWNER, token) == store::Status::Ok,
          "ordinary edit takes Store scratch ownership");
    result = ContinueRead(receiver, OWNER);
    Check(Status(result) == static_cast<uint8_t>(store::WireStatus::Incomplete) &&
              result.responseSize == 60U && !receiver.ReadActive(),
          "in-flight preview fails closed while a transaction owns scratch");
    Check(frames.Abort(token) == store::Status::Ok,
          "preview guard fixture releases its edit transaction");
    (void)Handle(receiver, preview.data(), preview.size(), OWNER);
    Check(frames.UpdateModeActive(false, 1U) == store::Status::Ok,
          "root publication advances committed generation");
    result = ContinueRead(receiver, OWNER);
    Check(Status(result) == static_cast<uint8_t>(store::WireStatus::RevisionConflict) &&
              result.responseSize == 60U && !receiver.ReadActive(),
          "root generation change rejects further preview bytes");
}

void PreviewScratchAndResourceFailuresAreBounded() {
    MemoryFile file;
    const auto body = CommitDenseFrames(file, 4U);
    store::FrameStore frames(file.Ops());
    Check(frames.Recover() == store::Status::Ok,
          "preview scratch fixture recovers");
    std::array<uint8_t, 438U> bytes{};
    size_t size = 0U;
    uint32_t total = 0U;
    uint32_t crc = 0U;
    Check(frames.ReadWireBody(1U, 0U, bytes.data(), bytes.size(), size) ==
              store::Status::Ok,
          "full read starts on shared wire scratch");
    Check(frames.ReadWireFrame(1U, 2U, 0U, bytes.data(), bytes.size(), size,
                               total, crc) == store::Status::Ok,
          "frame preview takes shared wire scratch");
    const auto expected = KeyframeRecord(DenseFrame(2U));
    store::Frame unrelated{};
    Check(frames.ReadFrame(3U, unrelated) == store::Status::Ok &&
              frames.ReadWireFrame(1U, 2U, 438U, bytes.data(), bytes.size(), size,
                                   total, crc) == store::Status::Ok &&
              std::equal(bytes.begin(), bytes.begin() + size,
                          expected.begin() + 438U),
          "playback decode scratch reuse preserves the captured encoded record");
    Check(frames.ReadWireBody(1U, 438U, bytes.data(), bytes.size(), size) ==
              store::Status::Ok &&
              std::equal(bytes.begin(), bytes.begin() + size, body.begin() + 438U),
          "full-body cursor restarts coherently after preview scratch reuse");
    store::FrameStore absent({});
    Check(absent.ReadWireFrame(1U, 0U, 0U, bytes.data(), bytes.size(), size,
                               total, crc) == store::Status::NotConfigured &&
              size == 0U && total == 0U && crc == 0U,
          "missing Store resources return no preview identity or bytes");
    for (const uint32_t cursor : {
            static_cast<uint32_t>(expected.size()),
            static_cast<uint32_t>(store::WIRE_FRAME_MAX_BYTES +
                                  store::WIRE_RECORD_HEADER_BYTES), UINT32_MAX}) {
        Check(frames.ReadWireFrame(1U, 2U, cursor, bytes.data(), bytes.size(), size,
                                   total, crc) == store::Status::InvalidArgument &&
                  size == 0U && total == 0U && crc == 0U,
              "preview cursor cannot escape the bounded single-record body");
    }
    // A damaged selected fixed slot must never become an apparently valid
    // preview. Reconstruct to invalidate the previously encoded record cache.
    file.bytes[store::FrameStore::BodyOffset(0U) +
               2U * store::STAGE_SLOT_BYTES + 7U] ^= 1U;
    store::FrameStore corrupt(file.Ops());
    store::StoreReceiver receiver(corrupt);
    Check(receiver.Recover({1234U, 0U, 3U, true, false}) == store::Status::Ok,
          "damaged frame fixture keeps its metadata root");
    const auto preview = PreviewPacket(1234U, 40U, 1U, 2U);
    const auto result = Handle(receiver, preview.data(), preview.size(), 9U);
    Check(Status(result) == static_cast<uint8_t>(store::WireStatus::IntegrityFailure) &&
              result.responseSize == 60U && !receiver.ReadActive(),
          "selected-slot CRC failure publishes no artwork preview body");
}

void EnumeratesAndSelectsTwoResidentRootsWithoutWrites() {
    constexpr uint32_t SESSION = 0x31415926U;
    constexpr uint32_t OWNER = 12U;
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 0U, 3U, true, true}) ==
              store::Status::NoArtwork,
          "resident selection fixture starts blank");
    const auto body = RepeatedBody(1U);
    auto commitBody = [&](const uint16_t transfer,
                          const uint32_t expectedGeneration) {
        const auto begin = BeginPacket(
            SESSION, transfer, expectedGeneration, body, 1U, true);
        (void)Handle(receiver, begin.data(), begin.size(), OWNER);
        auto chunk = CursorPacket(store::STORE_CHUNK, SESSION, transfer, 0U);
        chunk.insert(chunk.end(), body.begin(), body.end());
        (void)Handle(receiver, chunk.data(), chunk.size(), OWNER);
        const auto commit = CursorPacket(
            store::STORE_COMMIT, SESSION, transfer,
            static_cast<uint32_t>(body.size()));
        return Handle(receiver, commit.data(), commit.size(), OWNER);
    };
    Check(Status(commitBody(1U, 0U)) == 0U &&
              Status(commitBody(2U, 1U)) == 0U &&
              frames.ResidentCount() == 2U,
          "two committed roots remain resident");
    const auto& committed = receiver.Receipt().response;
    Check(committed.responseSize == 111U && committed.response[20U] == 2U &&
              committed.response[19U] < 2U,
          "Commit carries both resident descriptors within the existing State bound");

    auto residents = Packet(store::STORE_RESIDENTS, SESSION, 3U, 0U);
    auto result = Handle(receiver, residents.data(), residents.size(), OWNER);
    Check(result.responseReady &&
              result.response[2U] == store::STORE_RESIDENTS_STATE &&
              result.response[13U] == 0U && result.response[15U] == 2U &&
              result.responseSize == 106U && result.response[14U] < 2U &&
              fxe::ReadU32LE(result.response.data() + 33U) == 0x13572468U,
          "resident query returns selected root and both immutable descriptors");
    const uint8_t previousSlot = result.response[14U] ^ 1U;
    store::FrameStore::ResidentDescriptor previous{};
    Check(frames.Resident(previousSlot, previous),
          "alternate resident descriptor is addressable by root slot");
    receiver.SetProjection({SESSION, 0U, 3U, true, true});
    const auto preview = PreviewPacket(SESSION, 6U, frames.Generation(), 0xffffU);
    result = Handle(receiver, preview.data(), preview.size(), OWNER);
    Check(Status(result) == 0U && receiver.ReadActive(),
          "resident switch fixture owns a bounded preview before selection");
    const size_t writesBefore = file.bodyWriteCalls;
    auto select = Packet(store::STORE_SELECT, SESSION, 4U, 9U);
    select[9U] = previous.rootSlot;
    fxe::WriteU32LE(previous.generation, select.data() + 10U);
    fxe::WriteU32LE(previous.bodyCrc32, select.data() + 14U);
    result = Handle(receiver, select.data(), select.size(), OWNER);
    Check(result.responseReady && result.playbackReady &&
              result.response[13U] == 0U &&
              result.response[14U] == previous.rootSlot &&
              frames.Generation() == previous.generation &&
              file.bodyWriteCalls == writesBefore,
          "select swaps authority and requests playback with zero flash writes");
    Check(!receiver.ReadActive() && !ContinueRead(receiver, OWNER).responseReady,
          "resident selection retires the previous root preview owner");
    auto inspect = Packet(1U, SESSION, 5U, 1U);
    inspect[9U] = store::RESIDENT_LISTING_PROBE;
    result = Handle(receiver, inspect.data(), inspect.size(), OWNER);
    Check(Status(result) == 0U && previous.manifest.playbackOnEntry &&
              (result.response[14U] & 2U) == 0U &&
              fxe::ReadU16LE(result.response.data() + 17U) == 0U,
          "resident selection stops prior playback and shows paused frame zero");

    fxe::WriteU32LE(previous.bodyCrc32 ^ 1U, select.data() + 14U);
    result = Handle(receiver, select.data(), select.size(), OWNER);
    Check(result.responseReady && !result.playbackReady &&
              result.response[13U] == 9U &&
              frames.Generation() == previous.generation,
          "stale resident identity cannot switch playback authority");
}

void CreatesOneBlankStoreRootBeforeConnectionAdmission() {
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    constexpr store::StoreProjection projection{29U, 0U, 3U, true, false};
    Check(receiver.RecoverOrCreateBlank({0U, 0U, 3U, false, false}) ==
              store::Status::NoArtwork &&
              !frames.HasArtwork(),
          "boot without mode authority leaves a fresh Store blank");
    Check(receiver.RecoverOrCreateBlank(projection) == store::Status::Ok &&
              frames.HasArtwork() && frames.Generation() == 1U &&
              frames.CurrentManifest().frameCount == 1U,
          "empty Store becomes one schema-4 frame before connection admission");
    store::Frame first{};
    Check(frames.ReadFrame(0U, first) == store::Status::Ok &&
              first.emitters[0U].color.r == 0U &&
              first.emitters[255U].color.b == 0U,
          "new Store frame is a complete blank canvas");
    const size_t writes = file.bodyWriteCalls;
    Check(receiver.RecoverOrCreateBlank(projection) == store::Status::Ok &&
              frames.Generation() == 1U && file.bodyWriteCalls == writes,
          "recovery preserves the same blank root without another bank write");

    modules::litchcraft_frame_stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              pager.Service(frames, false) ==
                  modules::litchcraft_frame_stream::FrameStream::ServiceResult::
                      LoadedRequested &&
              pager.TryPresentRequested(),
          "mode-only root fixture presents the first frame");
    file.renderDuringRootWrite = &pager;
    Check(pager.SaveModeRoot(frames, false) == store::Status::Ok &&
              frames.Generation() == 2U &&
              !frames.CurrentManifest().active &&
              !pager.Active() && pager.Generation() == 2U &&
              file.bodyWriteCalls == writes,
          "Exit changes only the root while render can read its live frame");
    file.renderDuringRootWrite = nullptr;

    Check(store::FrameStore::Retire(file.Ops(), frames.Generation()) ==
              store::Status::Ok,
          "retired prior Store root models development artwork loss");
    store::FrameStore retired(file.Ops());
    store::StoreReceiver replacement(retired);
    Check(replacement.RecoverOrCreateBlank(projection) == store::Status::Ok &&
              retired.HasArtwork() && retired.Generation() == 4U &&
              retired.CurrentManifest().frameCount == 1U,
          "retired root generation admits a fresh one-frame Store");
}


void CorruptRecoveryPreservesStoreBeforeActiveAdmission() {
    MemoryFile file;
    file.bytes[0] = 0xa5U;
    file.durable = file.bytes;
    const auto original = file.bytes;
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    const store::StoreProjection active{1234U, 0U, 3U, true, false};
    Check(receiver.RecoverOrCreateBlank(active) == store::Status::Corrupt &&
              !frames.HasArtwork() && frames.Generation() == 0U &&
              file.bytes == original && file.durable == original &&
              file.bodyWriteCalls == 0U,
          "active Corrupt recovery preserves bytes without a blank replacement");
    Check(receiver.RecoverOrCreateBlank({0U, 0U, 3U, false, false}) ==
              store::Status::Corrupt && file.bytes == original &&
              file.durable == original && file.bodyWriteCalls == 0U,
          "boot Corrupt recovery also preserves damaged Store bytes");
}

void PartialIngressCancelRestoresCachedEditWorkspace() {
    constexpr uint32_t SESSION = 0x12345678U;
    constexpr uint32_t OWNER = 31U;
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::Manifest manifest{};
    manifest.frameCount = 3U;
    manifest.active = true;
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    Check(frames.Begin(manifest, 0U, OWNER, token) == store::Status::Ok,
          "phase-reuse fixture begins");
    for (uint16_t index = 0U; index < 3U; ++index) {
        Check(frames.WriteFrame(token, index, DenseFrame(index + 1U)) == store::Status::Ok,
              "phase-reuse fixture writes original frames");
    }
    Check(frames.Commit(token) == store::Status::Ok, "phase-reuse fixture commits");
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 0U, 3U, true, false}) == store::Status::Ok,
          "phase-reuse receiver loads current bound metadata");
    const auto body = DenseBody();
    auto begin = BeginPacket(SESSION, 71U, 1U, body, store::FRAME_COUNT, false);
    (void)Handle(receiver, begin.data(), begin.size(), OWNER);
    uint32_t cursor = 0U;
    while (cursor < 7000U) {
        const size_t count = std::min<size_t>(231U, 7000U - cursor);
        auto chunk = CursorPacket(store::STORE_CHUNK, SESSION, 71U, cursor);
        chunk.insert(chunk.end(), body.begin() + cursor, body.begin() + cursor + count);
        (void)Handle(receiver, chunk.data(), chunk.size(), OWNER);
        cursor += static_cast<uint32_t>(count);
    }
    Check(receiver.TransferActive() && receiver.BodyCursor() == 7000U &&
              frames.BeginEdit(1U, 32U, token) == store::Status::Incomplete &&
              frames.UpdateModeActive(false, 1U) == store::Status::Incomplete,
          "partial multi-batch ingress refuses competing cache/edit lifetimes");
    const auto cancel = Packet(store::STORE_CANCEL, SESSION, 71U, 0U);
    const auto result = Handle(receiver, cancel.data(), cancel.size(), OWNER);
    const size_t before = file.frameLoads;
    Check(Status(result) == 0U && !receiver.TransferActive() &&
              frames.BeginEdit(1U, 32U, token) == store::Status::Ok &&
              frames.DeleteFrame(token, 2U) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok && file.frameLoads == before,
          "cancel reloads bound summaries and the next tail delete scans no artwork");
    store::Frame shown{};
    Check(frames.ReadFrame(1U, shown) == store::Status::Ok && Same(shown, DenseFrame(2U)),
          "borrowed ingress bytes cannot poison the surviving original frame");
}


std::vector<uint8_t> LivePacket(const uint32_t session, const uint16_t request,
                               const uint32_t generation, const uint32_t revision,
                               const uint16_t selector, const uint32_t cursor = 0U) {
    auto packet = Packet(store::STORE_SNAPSHOT, session, request, 14U);
    fxe::WriteU32LE(generation, packet.data() + 9U);
    fxe::WriteU32LE(cursor, packet.data() + 13U);
    fxe::WriteU16LE(selector, packet.data() + 17U);
    fxe::WriteU32LE(revision, packet.data() + 19U);
    return packet;
}

void PresentLive(modules::litchcraft_frame_stream::FrameStream& stream,
                 store::FrameStore& frames, const uint16_t index) {
    stream.Request(index);
    for (size_t step = 0U; step < 12U; ++step) {
        (void)stream.Service(frames, false);
        (void)stream.TryPresentRequested();
    }
    Check(stream.DisplayedIndex() == index, "LIVE fixture presents exact frame");
}

std::vector<uint8_t> DrainLive(store::StoreReceiver& receiver,
                              store::StoreIngressResult result, const uint32_t owner) {
    std::vector<uint8_t> body;
    const uint32_t total = fxe::ReadU32LE(result.response.data() + 20U);
    const uint32_t crc = fxe::ReadU32LE(result.response.data() + 24U);
    for (size_t chunk = 0U; chunk < 100U; ++chunk) {
        Check(result.responseReady && result.responseSize > 78U && Status(result) == 0U,
              "LIVE actual chunk executes successfully");
        if (!result.responseReady || result.responseSize <= 78U || Status(result) != 0U) break;
        Check(fxe::ReadU32LE(result.response.data() + 28U) == body.size(),
              "LIVE Object suffix preserves exact cursor");
        body.insert(body.end(), result.response.begin() + 78U,
                    result.response.begin() + result.responseSize);
        if (!receiver.ReadActive()) break;
        result = ContinueRead(receiver, owner);
    }
    Check(body.size() == total && fxe::Crc32(body.data(), body.size()) == crc,
          "LIVE frozen body length and full CRC validate");
    return body;
}

void ModeExitAndReentryPreserveLiveArtworkAuthority() {
    const int priorFailures = failures;
    constexpr uint32_t OWNER = 80U;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(10U);
    for (const uint16_t count : {32U, 256U}) {
        MemoryFile file;
        (void)CommitDenseFrames(file, count);
        store::FrameStore frames(file.Ops());
        store::StoreReceiver receiver(frames);
        Check(receiver.Recover({61U, 0U, 3U, true, false},
                  store::FrameStore::RecoveryPurpose::RootOnly) == store::Status::Ok,
              "mode journey restores the production root and bound summary sidecar");
        modules::litchcraft_frame_stream::FrameStream stream;
        receiver.BindLiveStream(stream);
        Check(stream.Configure(frames.CurrentManifest(), frames.Generation()),
              "mode journey binds the existing artwork");
        PresentLive(stream, frames, 0U);
        modules::LitchcraftSession session;
        session.Configure(3U, 120U);
        session.AttachFrameStream(&stream);
        session.Update();
        Check(session.IsActive() && session.SessionId() != 0U,
              "mode journey adopts persisted active authority");
        receiver.SetProjection({session.SessionId(), 0U, 3U, true, false});
        const uint32_t revision = stream.LiveRevision();
        const uint32_t documentEpoch = stream.DocumentEpoch();
        const uint32_t generation = stream.Generation();
        const auto manifest = frames.CurrentManifest();
        const uint32_t bodyBytes = frames.CurrentBodyBytes();
        const uint32_t bodyCrc = frames.CurrentBodyCrc32();
        auto request = LivePacket(session.SessionId(), 110U, generation, revision, 0xfffeU);
        const auto baseline = DrainLive(receiver,
            Handle(receiver, request.data(), request.size(), OWNER), OWNER);
        const size_t bodyWrites = file.bodyWriteCalls;
        const size_t frameLoads = file.frameLoads;

        std::array<uint8_t, 8U> exit{0x4cU,
            static_cast<uint8_t>(modules::LitchcraftSession::Op::Exit),
            0U, 0U, 0U, 0U, 111U, 0U};
        const uint32_t oldSession = session.SessionId();
        fxe::WriteU32LE(oldSession, exit.data() + 2U);
        session.HandlePacket(exit.data(), exit.size(), true, 0U);
        Check(!session.IsActive(), "compact production Exit changes mode immediately");
        receiver.SetProjection({oldSession, 0U, 3U, false, false});
        // The FreeRTOS/BLE queues are bypassed; this is the same mode-only
        // publication called by SettingsPersistenceWorker after clean edits.
        Check(stream.SaveModeRoot(frames, false) == store::Status::Ok &&
                  stream.Generation() == generation + 1U && !stream.Active() &&
                  stream.LiveRevision() == revision && stream.DocumentEpoch() == documentEpoch,
              "Exit persists mode generation without inventing an artwork revision");
        store::LiveRead exitedRead{};
        exitedRead.snapshot.generation = stream.Generation();
        exitedRead.snapshot.revision = stream.LiveRevision();
        Check(frames.CaptureLiveRead(stream, 0xfffeU, exitedRead) == store::Status::Ok &&
                  file.frameLoads == frameLoads,
              "Exit root retains valid bound live summaries without a payload scan");
        session.Update();
        Check(!session.IsActive(), "saved Exit cannot reactivate the departed mode");

        const std::array<uint8_t, 8U> enter{0x4cU, 1U, 0U, 0U, 0U, 0U, 112U, 0U};
        session.HandlePacket(enter.data(), enter.size(), true, 0U);
        Check(session.IsActive() && session.SessionId() != oldSession,
              "compact production Enter establishes a fresh mode session");
        receiver.SetProjection({session.SessionId(), 0U, 3U, true, false});
        Check(stream.SaveModeRoot(frames, true) == store::Status::Ok &&
                  stream.Generation() == generation + 2U && stream.Active() &&
                  stream.LiveRevision() == revision && stream.DocumentEpoch() == documentEpoch,
              "re-entry persists mode generation while common artwork remains authoritative");
        session.Update();
        request = LivePacket(session.SessionId(), 116U, generation, stream.LiveRevision(), 0xfffeU);
        Check(Status(Handle(receiver, request.data(), request.size(), OWNER)) == 9U,
              "prior saved generation cannot resume a live Snapshot after mode publication");
        request = LivePacket(session.SessionId(), 113U, stream.Generation(),
                             stream.LiveRevision(), 0xfffeU);
        const auto result = Handle(receiver, request.data(), request.size(), OWNER);
        const auto layout = DrainLive(receiver, result, OWNER);
        Check(Status(result) == 0U && layout == baseline && file.frameLoads == frameLoads,
              "immediate re-entry live Object layout is exact and never scans artwork payloads");
        const auto& current = frames.CurrentManifest();
        Check(current.frameCount == manifest.frameCount &&
                  current.topology == manifest.topology &&
                  current.playbackOnEntry == manifest.playbackOnEntry &&
                  current.predecessorDeltas == manifest.predecessorDeltas &&
                  current.sparkleSeed == manifest.sparkleSeed &&
                  std::memcmp(current.favorites.data(), manifest.favorites.data(),
                              sizeof(manifest.favorites)) == 0 &&
                  frames.CurrentBodyBytes() == bodyBytes && frames.CurrentBodyCrc32() == bodyCrc &&
                  file.bodyWriteCalls == bodyWrites,
              "mode-only roots preserve artwork identity, settings and every frame body");
        const size_t writes = file.writeCalls;
        Check(stream.SaveModeRoot(frames, true) == store::Status::Ok &&
                  stream.LiveRevision() == revision && stream.Generation() == generation + 2U &&
                  file.writeCalls == writes,
              "repeated mode save is an idempotent publication");
        std::printf("[MODE_LIVE] frames=%u oldG=%u newG=%u commonR=%u liveR=%u status=%u layout=%zu crc=%08x\n",
            count, generation, stream.Generation(), revision, stream.LiveRevision(),
            Status(result), layout.size(), fxe::Crc32(layout.data(), layout.size()));

        store::FrameStore restored(file.Ops());
        store::StoreReceiver restoredReceiver(restored);
        Check(restoredReceiver.Recover({71U, 0U, 3U, true, false},
                  store::FrameStore::RecoveryPurpose::RootOnly) == store::Status::Ok &&
                  restored.Generation() == stream.Generation() && restored.CurrentManifest().active,
              "cold root-only restore retains the re-entered mode and generation");
        modules::litchcraft_frame_stream::FrameStream restoredStream;
        restoredReceiver.BindLiveStream(restoredStream);
        Check(restoredStream.Configure(restored.CurrentManifest(), restored.Generation()),
              "cold restore binds the existing production stream");
        PresentLive(restoredStream, restored, 0U);
        const size_t restoredLoads = file.frameLoads;
        request = LivePacket(71U, 114U, restoredStream.Generation(),
                             restoredStream.LiveRevision(), 0xfffeU);
        const auto restoredResult = Handle(restoredReceiver, request.data(), request.size(), OWNER);
        Check(DrainLive(restoredReceiver, restoredResult, OWNER) == baseline &&
                  file.frameLoads == restoredLoads && file.bodyWriteCalls == bodyWrites,
              "cold immediate live layout reloads the rebound summary and exact artwork without scan/write");
        request = LivePacket(oldSession, 115U, stream.Generation(), stream.LiveRevision(), 0xfffeU);
        Check(Status(Handle(receiver, request.data(), request.size(), OWNER)) == 2U,
              "a retired mode session receives typed StaleSession without damaging current authority");
    }
    std::printf("[MODE_LIVE] Exit re-entry cold restore failures=%d\n", failures - priorFailures);
}

void LiveDirtyClearUndoDurationAndLayoutNeverReadsSavedPayload() {
    const int priorFailures = failures;
    constexpr uint32_t SESSION = 0x87654321U;
    constexpr uint32_t OWNER = 77U;
    constexpr uint16_t SELECTED = 108U;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(10U);
    MemoryFile file;
    (void)CommitDenseFrames(file, 256U);
    store::FrameStore frames(file.Ops());
    store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, SELECTED, 3U, true, false}) == store::Status::Ok,
          "LIVE recovers accepted checksum root");
    modules::litchcraft_frame_stream::FrameStream stream;
    receiver.BindLiveStream(stream);
    Check(stream.Configure(frames.CurrentManifest(), frames.Generation(), SELECTED),
          "LIVE binds existing document owner");
    PresentLive(stream, frames, SELECTED);
    auto authority = stream.ReadLiveAuthority();
    const size_t reads = file.frameLoads;
    Check(stream.ClearDisplayedIfRevision(authority.revision, SELECTED) ==
              decltype(stream)::GuardedEditResult::Changed,
          "human Clear changes selected dense frame");
    auto request = LivePacket(SESSION, 81U, authority.generation, stream.LiveRevision(), 0xffffU);
    auto result = Handle(receiver, request.data(), request.size(), OWNER);
    Check((result.response[14U] & (64U | 32U | 8U)) == (64U | 32U) &&
              fxe::ReadU16LE(result.response.data() + 16U) == 256U &&
              fxe::ReadU16LE(result.response.data() + 18U) == SELECTED &&
              fxe::ReadU32LE(result.response.data() + 68U) == authority.revision &&
              result.response[72U] == 1U &&
              fxe::ReadU16LE(result.response.data() + 73U) == SELECTED,
          "LIVE header covers real frame Clear and exact frozen index");
    auto clearFrame = DenseFrame(SELECTED);
    for (auto& emitter : clearFrame.emitters) emitter = {};
    Check(DrainLive(receiver, result, OWNER) == KeyframeRecord(clearFrame, nullptr),
          "LIVE Clear is dirty current bytes before five-second save");
    Check(stream.StepHistoryIfRevision(stream.LiveRevision(), SELECTED, false) ==
              decltype(stream)::GuardedEditResult::Changed,
          "real ordinary Undo restores dense Clear inverse");
    request = LivePacket(SESSION, 82U, authority.generation, stream.LiveRevision(), SELECTED);
    result = Handle(receiver, request.data(), request.size(), OWNER);
    Check(DrainLive(receiver, result, OWNER) == KeyframeRecord(DenseFrame(SELECTED), nullptr) &&
              file.frameLoads == reads && stream.HasUnpersistedEdit(),
          "LIVE Undo returns authoritative dirty bytes without saved artwork payload read");
    const uint32_t beforeDuration = stream.LiveRevision();
    Check(stream.EditDisplayedDurationIfRevision({stream.Generation(), beforeDuration,
              SELECTED, 1750U, decltype(stream)::FrameEditAction::Duration}) ==
              decltype(stream)::GuardedEditResult::Changed,
          "LIVE duration uses existing production mutation");
    request = LivePacket(SESSION, 83U, stream.Generation(), stream.LiveRevision(), 0xfffeU);
    result = Handle(receiver, request.data(), request.size(), OWNER);
    Check((result.response[14U] & (128U | 64U | 32U | 8U)) == (128U | 64U) &&
              fxe::ReadU32LE(result.response.data() + 20U) == 1536U &&
              fxe::ReadU32LE(result.response.data() + 68U) == beforeDuration &&
              result.response[72U] == 1U,
          "LIVE layout has exact bounded rows and duration coverage");
    const auto layout = DrainLive(receiver, result, OWNER);
    bool exact = layout.size() == 1536U;
    for (uint16_t index = 0U; exact && index < 256U; ++index) {
        auto frame = DenseFrame(index);
        if (index == SELECTED) frame.durationMs = 1750U;
        const auto record = KeyframeRecord(frame, nullptr);
        exact = fxe::ReadU16LE(layout.data() + index * 6U) == record.size() &&
            fxe::ReadU32LE(layout.data() + index * 6U + 2U) ==
                fxe::Crc32(record.data(), record.size());
    }
    Check(exact && file.frameLoads == reads,
          "all 256 LIVE absolute Legacy signatures overlay dirty duration without payload scan");
    request = LivePacket(SESSION, 84U, stream.Generation(), stream.LiveRevision(), SELECTED);
    auto durationFrame = DenseFrame(SELECTED); durationFrame.durationMs = 1750U;
    Check(DrainLive(receiver, Handle(receiver, request.data(), request.size(), OWNER), OWNER) ==
              KeyframeRecord(durationFrame, nullptr), "LIVE duration body is authoritative");
    auto saved = PreviewPacket(SESSION, 85U, frames.Generation(), SELECTED);
    auto savedResult = Handle(receiver, saved.data(), saved.size(), OWNER);
    std::vector<uint8_t> savedBody;
    do {
        if (savedResult.responseSize <= 60U || Status(savedResult) != 0U) break;
        savedBody.insert(savedBody.end(), savedResult.response.begin() + 60U,
                         savedResult.response.begin() + savedResult.responseSize);
        if (!receiver.ReadActive()) break;
        savedResult = ContinueRead(receiver, OWNER);
    } while (true);
    Check(savedBody == KeyframeRecord(DenseFrame(SELECTED), nullptr) &&
              (savedResult.response[14U] & 64U) == 0U,
          "saved 19-byte preview cannot reuse or expose dirty LIVE wire scratch");
    std::printf("[LIVE_STORE] dirty Clear Undo duration layout failures=%d\n", failures - priorFailures);
}

void LiveHiddenStructuralHistoryAndReadFences() {
    const int priorFailures = failures;
    constexpr uint32_t SESSION = 42U;
    constexpr uint32_t OWNER = 78U;
    MemoryFile file; (void)CommitDenseFrames(file, 8U);
    store::FrameStore frames(file.Ops()); store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 3U, 3U, true, false}) == store::Status::Ok,
          "LIVE structural fixture recovers");
    modules::litchcraft_frame_stream::FrameStream stream; receiver.BindLiveStream(stream);
    Check(stream.Configure(frames.CurrentManifest(), frames.Generation(), 3U), "LIVE configures");
    PresentLive(stream, frames, 3U);
    const size_t reads = file.frameLoads;
    auto request = LivePacket(SESSION, 91U, stream.Generation(), stream.LiveRevision(), 7U);
    auto result = Handle(receiver, request.data(), request.size(), OWNER);
    Check(fxe::ReadU32LE(result.response.data() + 68U) == 0U && result.response[72U] == 0U,
          "new document has explicitly unavailable mutation coverage");
    Check(DrainLive(receiver, result, OWNER) == KeyframeRecord(DenseFrame(7U), nullptr) &&
              stream.DisplayedIndex() == 3U && file.frameLoads == reads + 1U,
          "LIVE clean hidden frame reads one exact slot without seeking");
    Check(stream.PaintDisplayed(3U, 0U, 15U, {11U, 22U, 33U}, true),
          "dirty hidden fixture paints current frame");
    stream.Request(4U);
    Check(stream.TryPresentRequested() && stream.DisplayedIndex() == 4U,
          "already cached neighbor presents without saving dirty hidden frame");
    Check(stream.PaintDisplayed(4U, 2U, 15U, {44U, 55U, 66U}, true),
          "missed second frame mutation creates multi-frame dirty gap");
    const size_t hiddenReads = file.frameLoads;
    auto hiddenFrame = DenseFrame(3U);
    for (size_t emitter = 0U; emitter < 4U; ++emitter) {
        hiddenFrame.emitters[emitter] = {{11U, 22U, 33U}, 0U, 0U};
    }
    request = LivePacket(SESSION, 90U, stream.Generation(), stream.LiveRevision(), 3U);
    result = Handle(receiver, request.data(), request.size(), OWNER);
    Check(DrainLive(receiver, result, OWNER) == KeyframeRecord(hiddenFrame, nullptr) &&
              file.frameLoads == hiddenReads && stream.HasUnpersistedEdit() &&
              stream.DisplayedIndex() == 4U,
          "LIVE hidden dirty cache beats old durable slot without seeking or saving");
    request = LivePacket(SESSION, 88U, stream.Generation(), stream.LiveRevision(), 0xfffeU);
    const auto dirtyLayout = DrainLive(receiver,
        Handle(receiver, request.data(), request.size(), OWNER), OWNER);
    auto secondDirty = DenseFrame(4U);
    for (size_t emitter = 8U; emitter < 12U; ++emitter) {
        secondDirty.emitters[emitter] = {{44U, 55U, 66U}, 0U, 0U};
    }
    const auto firstDirtyRecord = KeyframeRecord(hiddenFrame, nullptr);
    const auto secondDirtyRecord = KeyframeRecord(secondDirty, nullptr);
    Check(dirtyLayout.size() == 48U &&
              fxe::ReadU32LE(dirtyLayout.data() + 3U * 6U + 2U) ==
                  fxe::Crc32(firstDirtyRecord.data(), firstDirtyRecord.size()) &&
              fxe::ReadU32LE(dirtyLayout.data() + 4U * 6U + 2U) ==
                  fxe::Crc32(secondDirtyRecord.data(), secondDirtyRecord.size()) &&
              file.frameLoads == hiddenReads,
          "unknown multi-frame gap signatures overlay both dirty cache slots without scanning");
    stream.Request(3U);
    Check(stream.TryPresentRequested(), "dirty frame remains cached for ordinary Undo");
    request = LivePacket(SESSION, 92U, stream.Generation(), stream.LiveRevision(), 3U);
    result = Handle(receiver, request.data(), request.size(), OWNER);
    Check(result.responseSize > 78U && receiver.ReadActive(), "LIVE dense capture streams chunks");
    const uint32_t frozenRevision = stream.LiveRevision();
    const uint32_t frozenEpoch = fxe::ReadU32LE(result.response.data() + 64U);
    auto resume = LivePacket(SESSION, 93U, stream.Generation(), frozenRevision, 3U, 25U);
    auto resumed = Handle(receiver, resume.data(), resume.size(), OWNER);
    Check(Status(resumed) == 0U && fxe::ReadU32LE(resumed.response.data() + 28U) == 25U &&
              fxe::ReadU32LE(resumed.response.data() + 64U) == frozenEpoch,
          "LIVE explicit resume uses frozen bytes and epoch");
    Check(stream.PaintDisplayed(3U, 0U, 15U, {9U, 8U, 7U}, true), "foreign paint mutates");
    auto stale = ContinueRead(receiver, OWNER);
    Check(Status(stale) == 9U && stale.responseSize == 78U && !receiver.ReadActive(),
          "foreign live mutation rejects a captured suffix without old bytes");
    Check(Status(Handle(receiver, resume.data(), resume.size(), OWNER)) == 9U,
          "stale resume cannot recapture silently");
    file.mutateDuringFrameRead = &stream;
    request = LivePacket(SESSION, 89U, stream.Generation(), stream.LiveRevision(), 7U);
    Check(Status(Handle(receiver, request.data(), request.size(), OWNER)) == 9U &&
              !receiver.ReadActive(),
          "foreign mutation during hidden-slot I/O rejects coherent LIVE capture");
    request = LivePacket(SESSION, 94U, stream.Generation(), stream.LiveRevision(), 3U);
    result = Handle(receiver, request.data(), request.size(), OWNER);
    receiver.EndOwner(OWNER);
    Check(!ContinueRead(receiver, OWNER).responseReady &&
              Status(Handle(receiver, resume.data(), resume.size(), OWNER + 1U)) == 9U,
          "disconnect/reconnect invalidates LIVE capture");
    result = Handle(receiver, request.data(), request.size(), OWNER);
    auto cancel = Packet(store::STORE_CANCEL, SESSION, 94U, 0U);
    Check(Status(Handle(receiver, cancel.data(), cancel.size(), OWNER)) == 0U &&
              !ContinueRead(receiver, OWNER).responseReady,
          "typed Cancel retires LIVE read without a write transaction");
    result = Handle(receiver, request.data(), request.size(), OWNER);
    receiver.SetProjection({SESSION, 3U, 3U, false, false});
    Check(!ContinueRead(receiver, OWNER).responseReady, "Exit invalidates LIVE capture");
    receiver.SetProjection({SESSION, 3U, 3U, true, false});
    // Persist the dirty frame before structural admission, preserving ordinary save behavior.
    stream.RequestImmediateSave();
    for (size_t step = 0U; step < 8U; ++step) (void)stream.Service(frames, false);
    auto authority = stream.ReadLiveAuthority();
    Check(stream.QueueStructuralEdit({authority.generation, authority.revision, 3U, 0U,
              decltype(stream)::FrameEditAction::AddCopy}) == decltype(stream)::StructuralResult::Pending,
          "middle insert enters existing structural owner");
    (void)stream.Service(frames, false);
    uint16_t selected = 0U; uint32_t revision = 0U;
    Check(stream.TakeStructuralResult(selected, revision) == decltype(stream)::StructuralResult::Changed &&
              selected == 4U && stream.FrameCount() == 9U, "middle insert publishes");
    receiver.SetProjection({SESSION, selected, 3U, true, false});
    request = LivePacket(SESSION, 95U, stream.Generation(), stream.LiveRevision(), 0xfffeU);
    result = Handle(receiver, request.data(), request.size(), OWNER);
    Check(result.response[72U] == 2U && fxe::ReadU16LE(result.response.data() + 73U) == 4U &&
              fxe::ReadU32LE(result.response.data() + 68U) == authority.revision,
          "insert scope records exact base and shifted insertion index");
    const auto insertedLayout = DrainLive(receiver, result, OWNER);
    const auto hidden7 = KeyframeRecord(DenseFrame(7U), nullptr);
    Check(insertedLayout.size() == 54U &&
              fxe::ReadU32LE(insertedLayout.data() + 8U * 6U + 2U) ==
                  fxe::Crc32(hidden7.data(), hidden7.size()), "insert remaps hidden signatures");
    const auto insertionRevision = stream.LiveRevision();
    Check(stream.QueueStructuralHistory(insertionRevision, false) == decltype(stream)::StructuralResult::Pending,
          "real insert Undo enters structural owner");
    (void)stream.Service(frames, false);
    Check(stream.TakeStructuralResult(selected, revision) == decltype(stream)::StructuralResult::Changed &&
              stream.FrameCount() == 8U, "real insert Undo removes inserted frame");
    receiver.SetProjection({SESSION, selected, 3U, true, false});
    request = LivePacket(SESSION, 96U, stream.Generation(), stream.LiveRevision(), 0xfffeU);
    result = Handle(receiver, request.data(), request.size(), OWNER);
    Check(result.response[72U] == 3U && fxe::ReadU16LE(result.response.data() + 73U) == 4U &&
              fxe::ReadU32LE(result.response.data() + 68U) == insertionRevision,
          "structural Undo exposes precise Delete scope");
    (void)DrainLive(receiver, result, OWNER);
    authority = stream.ReadLiveAuthority();
    Check(stream.QueueStructuralEdit({authority.generation, authority.revision, selected, 0U,
              decltype(stream)::FrameEditAction::Delete}) == decltype(stream)::StructuralResult::Pending,
          "middle Delete enters structural owner");
    (void)stream.Service(frames, false);
    Check(stream.TakeStructuralResult(selected, revision) == decltype(stream)::StructuralResult::Changed &&
              stream.FrameCount() == 7U, "middle Delete publishes");
    Check(stream.QueueStructuralHistory(stream.LiveRevision(), false) == decltype(stream)::StructuralResult::Pending,
          "real Delete Undo enters existing owner");
    (void)stream.Service(frames, false);
    Check(stream.TakeStructuralResult(selected, revision) == decltype(stream)::StructuralResult::Changed &&
              stream.FrameCount() == 8U, "Delete Undo restores exact layout");
    receiver.SetProjection({SESSION, selected, 3U, true, false});
    request = LivePacket(SESSION, 97U, stream.Generation(), stream.LiveRevision(), 0xfffeU);
    result = Handle(receiver, request.data(), request.size(), OWNER);
    Check(result.response[72U] == 2U && fxe::ReadU16LE(result.response.data() + 73U) == 3U,
          "Delete Undo scope is exact restored insertion index");
    (void)DrainLive(receiver, result, OWNER);
    request = LivePacket(SESSION, 98U, stream.Generation(), stream.LiveRevision(), selected);
    result = Handle(receiver, request.data(), request.size(), OWNER);
    auto replacement = frames.CurrentManifest();
    const auto pendingEpoch = stream.DocumentEpoch();
    Check(!stream.Configure(replacement, stream.Generation(), selected) &&
              stream.DocumentEpoch() == pendingEpoch && stream.FrameCount() == 8U &&
              Status(ContinueRead(receiver, OWNER)) == 0U,
          "saved-layout synchronization cannot replace accepted RAM edits or invalidate their captured live bytes");
    for (uint8_t step = 0U; step < 32U && stream.HasUnpersistedEdit(); ++step) {
        (void)stream.Service(frames, false);
    }
    Check(!stream.HasUnpersistedEdit(), "structural saves converge before deliberate replacement");
    request = LivePacket(SESSION, 99U, stream.Generation(), stream.LiveRevision(), selected);
    result = Handle(receiver, request.data(), request.size(), OWNER);
    Check(Status(result) == 0U && receiver.ReadActive(), "replacement fixture captures the current saved live frame");
    replacement = frames.CurrentManifest();
    Check(stream.Configure(replacement, stream.Generation(), selected) && stream.DocumentEpoch() != pendingEpoch,
          "replacement establishes a new document epoch");
    Check(Status(ContinueRead(receiver, OWNER)) == 9U, "replacement invalidates captured LIVE bytes");
    std::printf("[LIVE_STORE] hidden structural history fences failures=%d\n", failures - priorFailures);
}

void LiveCacheAndUnsupportedShapesFailWithoutArtworkScan() {
    const int priorFailures = failures;
    constexpr uint32_t SESSION = 51U;
    constexpr uint32_t OWNER = 79U;
    MemoryFile file; (void)CommitDenseFrames(file, 256U);
    store::FrameStore frames(file.Ops()); store::StoreReceiver receiver(frames);
    Check(receiver.Recover({SESSION, 2U, 3U, true, false},
              store::FrameStore::RecoveryPurpose::RootOnly) == store::Status::Ok,
          "LIVE cache-fault fixture uses worker RootOnly recovery");
    modules::litchcraft_frame_stream::FrameStream stream; receiver.BindLiveStream(stream);
    Check(stream.Configure(frames.CurrentManifest(), frames.Generation(), 2U), "LIVE cache-fault configures");
    PresentLive(stream, frames, 2U);
    const size_t reads = file.frameLoads;
    file.failReads = true;
    auto fault = LivePacket(SESSION, 100U, stream.Generation(), stream.LiveRevision(), 0xfffeU);
    Check(Status(Handle(receiver, fault.data(), fault.size(), OWNER)) == 10U &&
              file.frameLoads == reads, "LIVE cache I/O failure is bounded ResourceLimit");
    file.failReads = false;
    const auto originalCache = file.bytes;
    // Flip each optional page CRC without touching the root or payload.
    for (size_t page = 0U; page < 4U; ++page) {
        file.bytes[store::FRAME_FILES_CAPACITY + page * store::WIRE_SUMMARY_PAGE_BYTES +
                   store::WIRE_SUMMARY_PAGE_BYTES - 1U] ^= 1U;
    }
    Check(Status(Handle(receiver, fault.data(), fault.size(), OWNER)) == 11U &&
              file.frameLoads == reads, "LIVE corrupt bound cache is IntegrityFailure without scan");
    file.bytes = originalCache;
    // Cache is derived, never root authority. Remove all optional cache pages.
    std::fill(file.bytes.begin() + store::FRAME_FILES_CAPACITY, file.bytes.end(), 0xffU);
    auto request = LivePacket(SESSION, 101U, stream.Generation(), stream.LiveRevision(), 0xfffeU);
    auto result = Handle(receiver, request.data(), request.size(), OWNER);
    Check(Status(result) == 10U && file.frameLoads == reads && !receiver.ReadActive() &&
              frames.Generation() == stream.Generation(),
          "missing bound LIVE cache reports resource failure without payload scan/root mutation");
    for (size_t size : {20U, 21U, 22U, 24U}) {
        request.resize(size, 0U);
        result = Handle(receiver, request.data(), request.size(), OWNER);
        Check(Status(result) == 3U && !receiver.TransferActive() && !receiver.ReadActive(),
              "unsupported Snapshot shape cannot become another command");
    }
    request = LivePacket(SESSION, 102U, stream.Generation(), 0U, 0xffffU);
    Check(Status(Handle(receiver, request.data(), request.size(), OWNER)) == 9U,
          "zero live revision cannot authorize a read");
    std::printf("[LIVE_STORE] cache unavailable malformed admission failures=%d\n", failures - priorFailures);
}

} // namespace

int main() {
    BoundedRepeatIngressMatchesDurableRootAndCancelsBetweenFrames();
    std::printf("[LIVE_RESOURCE] store=%zu receiver=%zu stream=%zu Frame=%zu context=%zu read=%zu\n",
        sizeof(store::FrameStore), sizeof(store::StoreReceiver),
        sizeof(modules::litchcraft_frame_stream::FrameStream), sizeof(store::Frame),
        sizeof(store::LiveSnapshot), sizeof(store::LiveRead));
    ModeExitAndReentryPreserveLiveArtworkAuthority();
    LiveDirtyClearUndoDurationAndLayoutNeverReadsSavedPayload();
    LiveHiddenStructuralHistoryAndReadFences();
    LiveCacheAndUnsupportedShapesFailWithoutArtworkScan();
    PartialIngressCancelRestoresCachedEditWorkspace();
    std::puts("[LITCHCRAFT_FRAME_STORE_RECEIVER] start");
    ReusedOutputDoesNotRetainPacketsOrAliasTerminalReceipt();
    RgbWireCapabilityAndTransientBegin();
    CreatesOneBlankStoreRootBeforeConnectionAdmission();
    CorruptRecoveryPreservesStoreBeforeActiveAdmission();
    SameSessionExitRetiresQueuedStoreTraffic();
    StreamsExact256FrameBodyAndPublishesOneRoot();
    CommitsDense256BodyWithoutCheckpoint();
    PredecessorBodyKeepsFrameZeroPreviewStable();
    DirectChunkAdmissionUsesTheSamePrefixDrain();
    AcceptedArtworkSurvivesFailedRootAndLatePaint();
    RejectsOrderingCorruptionAndStaleOwnersWithoutReplacingRoot();
    DownloadsCommittedBodyWithoutCreatingAnotherAuthority();
    Cold256FrameCurrentPreviewIsBoundedAndFrozenDuringPlayback();
    PreviewRejectsMalformedStaleAndDepartedOwnersWithoutMutation();
    PreviewScratchAndResourceFailuresAreBounded();
    EnumeratesAndSelectsTwoResidentRootsWithoutWrites();
    std::printf("[LITCHCRAFT_FRAME_STORE_RECEIVER] failures=%d checks=%zu new_cases=4\n", failures, checks);
    return failures == 0 ? 0 : 1;
}
