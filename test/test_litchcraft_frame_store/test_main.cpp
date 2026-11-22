#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"
#include "firmware/litchcraft_frame_file.hpp"
#include "modules/litchcraft_frame_store.hpp"
#include "modules/litchcraft_frame_store_layout.hpp"
#include "modules/litchcraft_wire_codec.hpp"

namespace {
namespace store = modules::litchcraft_frame_store;
namespace wire = modules::litchcraft_wire_codec;

int failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) return;
    ++failures;
    std::printf("[FAIL] %s\n", message);
}

struct MemoryFile final {
    std::vector<uint8_t> bytes =
        std::vector<uint8_t>(store::ARTWORK_FILE_CAPACITY, 0U);
    std::vector<uint8_t> durable = bytes;
    size_t maximumRead = 0U;
    size_t readCalls = 0U;
    size_t maximumWrite = 0U;
    size_t bodyBytesWritten = 0U;
    size_t summaryBytesWritten = 0U;
    size_t frameLoads = 0U;
    size_t syncs = 0U;
    bool failSync = false;
    bool failRead = false;
    bool failSummaryRead = false;
    bool readsSeeOnlySyncedBytes = false;
    size_t faultAfterEvent = std::numeric_limits<size_t>::max();
    size_t corruptReadAt = std::numeric_limits<size_t>::max();
    size_t events = 0U;
    bool prepareBanks = false;
    bool failPrepare = false;
    bool preparedOnlyRetiredRoots = true;
    size_t prepares = 0U;
    uint8_t preparedResident = 0xffU;

    bool PermitEvent() {
        if (events >= faultAfterEvent) return false;
        ++events;
        return true;
    }

    static bool Read(void* context, const size_t offset, uint8_t* out,
                     const size_t size) {
        auto& self = *static_cast<MemoryFile*>(context);
        ++self.readCalls;
        self.maximumRead = std::max(self.maximumRead, size);
        if (self.failRead ||
            (self.failSummaryRead && offset >= store::FRAME_FILES_CAPACITY) ||
            out == nullptr || offset > self.bytes.size() ||
            size > self.bytes.size() - offset) {
            return false;
        }
        namespace fixed = modules::litchcraft_frame_store_layout;
        if (size == fixed::FRAME_SLOT_BYTES &&
            (offset == fixed::Layout::StageOffset() ||
             (offset >= fixed::Layout::BodyOffset(0U) &&
              offset < fixed::FRAME_FILES_CAPACITY))) ++self.frameLoads;
        const auto& visible = self.readsSeeOnlySyncedBytes
            ? self.durable : self.bytes;
        std::memcpy(out, visible.data() + offset, size);
        if (self.corruptReadAt >= offset && self.corruptReadAt - offset < size) {
            out[self.corruptReadAt - offset] ^= 1U;
        }
        return true;
    }

    static bool Write(void* context, const size_t offset,
                      const uint8_t* source, const size_t size) {
        auto& self = *static_cast<MemoryFile*>(context);
        self.maximumWrite = std::max(self.maximumWrite, size);
        if (source == nullptr || offset > self.bytes.size() ||
            size > self.bytes.size() - offset) {
            return false;
        }
        const size_t stage =
            modules::litchcraft_frame_store_layout::Layout::StageOffset();
        if ((offset >= stage &&
             offset < stage + store::STAGE_SLOT_BYTES) ||
            (offset >= store::FrameStore::BodyOffset(0U) &&
             offset < store::FRAME_FILES_CAPACITY)) {
            self.bodyBytesWritten += size;
        }
        if (offset >= store::FRAME_FILES_CAPACITY) {
            self.summaryBytesWritten += size;
        }
        for (size_t index = 0U; index < size; ++index) {
            if (!self.PermitEvent()) return false;
            self.bytes[offset + index] = source[index];
        }
        return true;
    }

    static bool Sync(void* context) {
        auto& self = *static_cast<MemoryFile*>(context);
        if (self.failSync || !self.PermitEvent()) return false;
        ++self.syncs;
        self.durable = self.bytes;
        return true;
    }

    static bool PrepareReplacementBank(void* context, const uint8_t resident) {
        auto& self = *static_cast<MemoryFile*>(context);
        ++self.prepares;
        self.preparedResident = resident;
        for (uint8_t copy = 0U; copy < 2U; ++copy) {
            const size_t at =
                modules::litchcraft_frame_store_layout::Layout::RootOffset(resident, copy);
            self.preparedOnlyRetiredRoots = self.preparedOnlyRetiredRoots &&
                std::all_of(self.durable.begin() + at,
                            self.durable.begin() + at + store::ROOT_BYTES,
                            [](const uint8_t value) { return value == 0xffU; });
        }
        return !self.failPrepare;
    }

    store::FrameStore::Ops Ops() {
        return {this, &Read, &Write, &Sync,
                prepareBanks ? &PrepareReplacementBank : nullptr};
    }
    void Crash() { bytes = durable; }
};

struct TimingEvents final {
    size_t bodyWrites = 0U;
    size_t bodyPrepare = 0U;
    size_t rootPublish = 0U;
};

void ObserveTiming(
    void* const context,
    const store::FrameStore::TimingPhase phase,
    const uint32_t) {
    auto& events = *static_cast<TimingEvents*>(context);
    switch (phase) {
        case store::FrameStore::TimingPhase::BodyWrite:
            ++events.bodyWrites;
            break;
        case store::FrameStore::TimingPhase::BodyPrepare:
            ++events.bodyPrepare;
            break;
        case store::FrameStore::TimingPhase::RootPublish:
            ++events.rootPublish;
            break;
        case store::FrameStore::TimingPhase::EditBegin:
            break;
    }
}

store::Manifest Manifest(const uint16_t frameCount) {
    store::Manifest manifest{};
    manifest.frameCount = frameCount;
    manifest.topology = 3U;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    manifest.sparkleSeed = 0x13572468U;
    for (size_t index = 0U; index < manifest.favorites.size(); ++index) {
        manifest.favorites[index] =
            {static_cast<uint8_t>(index), static_cast<uint8_t>(index + 10U),
             static_cast<uint8_t>(index + 20U)};
    }
    return manifest;
}

store::Frame Frame(const uint16_t marker) {
    store::Frame frame{};
    frame.durationMs = static_cast<uint16_t>(100U + marker % 100U);
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        frame.emitters[index].color = {
            static_cast<uint8_t>(marker + index),
            static_cast<uint8_t>(marker * 3U + index * 5U),
            static_cast<uint8_t>(marker * 7U + index * 11U)};
        const uint8_t effect = static_cast<uint8_t>((marker + index) % 9U);
        frame.emitters[index].effect = effect;
        frame.emitters[index].effectDurationMs = effect == 0U ? 0U :
            static_cast<uint16_t>(500U +
                ((marker * 13U + index * 17U) % 96U) * 100U);
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
            a.effectDurationMs != b.effectDurationMs) {
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> Body(const std::vector<store::Frame>& frames,
                          const bool predecessorDeltas = false,
                          const bool keyframes = false,
                          const wire::RgbOnlyMode mode = wire::RgbOnlyMode::Disabled) {
    std::vector<uint8_t> body;
    std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
    for (size_t index = 0U; index < frames.size(); ++index) {
        size_t encodedSize = 0U;
        const store::Frame* previous = index == 0U ||
            (keyframes && index % store::KEYFRAME_INTERVAL == 0U) ? nullptr :
            (predecessorDeltas ? &frames[index - 1U] : &frames[0U]);
        Check(wire::FrameCodec::Encode(previous, frames[index],
                                       encoded.data(), encoded.size(),
                                       encodedSize, mode),
              "wire body fixture encodes");
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

bool Same(const store::Frame& left, const store::Frame& right);
size_t RecordOffset(const std::vector<uint8_t>& body, uint16_t frameIndex);

void RgbWireCommitsOnlyLegacyIdentityAndRecovers() {
    std::vector<store::Frame> source;
    source.reserve(store::FRAME_COUNT);
    for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
        auto frame = index == 0U ? Frame(11U) : source.back();
        frame.durationMs = static_cast<uint16_t>(100U + index);
        auto& emitter = frame.emitters[index];
        emitter.color.r ^= static_cast<uint8_t>(index + 1U);
        if (index % 17U == 0U) {
            emitter.effect = 1U;
            emitter.effectDurationMs = 500U;
        } else {
            for (auto& cell : frame.emitters) {
                cell.effect = 0U;
                cell.effectDurationMs = 0U;
            }
        }
        source.push_back(frame);
    }
    const auto rgbBody = Body(source, true, true,
                              wire::RgbOnlyMode::Enabled);
    const auto legacyBody = Body(source, true, true);
    bool sawRgbRaw = false;
    bool sawRgbBitmap = false;
    for (uint16_t index = 0U; index < source.size(); ++index) {
        const uint8_t tag = rgbBody[RecordOffset(rgbBody, index) +
                                    store::WIRE_RECORD_HEADER_BYTES];
        sawRgbRaw = sawRgbRaw || tag == 4U;
        sawRgbBitmap = sawRgbBitmap || tag == 5U;
    }
    Check(rgbBody.size() < legacyBody.size() &&
              sawRgbRaw && sawRgbBitmap &&
              rgbBody[store::WIRE_RECORD_HEADER_BYTES] == 2U &&
              (rgbBody[RecordOffset(rgbBody, 1U) +
                       store::WIRE_RECORD_HEADER_BYTES] == 4U ||
               rgbBody[RecordOffset(rgbBody, 1U) +
                       store::WIRE_RECORD_HEADER_BYTES] == 5U),
          "mixed 256-frame body includes legacy effects and RGB-only records");

    MemoryFile file;
    store::FrameStore frames(file.Ops());
    auto manifest = Manifest(store::FRAME_COUNT);
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.BeginWire(manifest, 0U, 31U,
                  static_cast<uint32_t>(rgbBody.size()),
                  fxe::Crc32(rgbBody.data(), rgbBody.size()), token,
                  store::WireBodyMode::Rgb) == store::Status::Ok,
          "RGB wire transaction begins with transient mode");
    for (size_t cursor = 0U; cursor < rgbBody.size();) {
        const size_t count = std::min<size_t>(231U, rgbBody.size() - cursor);
        Check(frames.WriteBodyChunk(token, static_cast<uint32_t>(cursor),
                  rgbBody.data() + cursor, count) == store::Status::Ok,
              "RGB wire accepts arbitrary chunk boundaries");
        cursor += count;
    }
    Check(frames.Commit(token) == store::Status::Ok &&
              frames.CurrentBodyBytes() == legacyBody.size() &&
              frames.CurrentBodyCrc32() ==
                  fxe::Crc32(legacyBody.data(), legacyBody.size()),
          "committed root identifies canonical legacy body");

    store::FrameStore::ResidentDescriptor resident{};
    std::array<uint8_t, store::STAGE_SLOT_BYTES> scratch{};
    modules::litchcraft_frame_store_layout::Layout roots(
        file.Ops(), scratch.data(), scratch.size());
    store::FixedRoot root{};
    uint8_t copy = 0U;
    Check(frames.CurrentResident(resident) &&
              roots.LatestRoot(resident.rootSlot, root, copy) ==
                  modules::litchcraft_frame_store_layout::Status::Ok &&
              root.manifest.predecessorDeltas &&
              root.bodyBytes == legacyBody.size(),
          "old root layout decodes RGB upload without a new disk flag");
    const size_t rootOffset =
        modules::litchcraft_frame_store_layout::Layout::RootOffset(
            resident.rootSlot, copy);
    Check(file.bytes[rootOffset + 4U] == 6U &&
              (file.bytes[rootOffset + 19U] & ~7U) == 0U,
          "RGB wire mode retains the current bounded-file storage schema and manifest flags");
    for (size_t block = 0U; block < root.blockBytes.size(); ++block) {
        const size_t first = block * store::KEYFRAME_INTERVAL;
        const size_t last = std::min<size_t>(
            source.size(), first + store::KEYFRAME_INTERVAL);
        const size_t begin = RecordOffset(legacyBody,
                                         static_cast<uint16_t>(first));
        const size_t end = last == source.size() ? legacyBody.size() :
            RecordOffset(legacyBody, static_cast<uint16_t>(last));
        Check(root.blockBytes[block] == end - begin &&
                  root.blockCrc32[block] ==
                      fxe::Crc32(legacyBody.data() + begin, end - begin),
              "each durable block has the legacy byte identity");
    }

    store::FrameStore reboot(file.Ops());
    Check(reboot.Recover() == store::Status::Ok &&
              reboot.CurrentBodyCrc32() ==
                  fxe::Crc32(legacyBody.data(), legacyBody.size()),
          "reboot keeps legacy identity");
    std::vector<uint8_t> exported;
    exported.reserve(legacyBody.size());
    while (exported.size() < legacyBody.size()) {
        std::array<uint8_t, 438U> page{};
        size_t count = 0U;
        const bool read = reboot.ReadWireBody(reboot.Generation(),
            static_cast<uint32_t>(exported.size()), page.data(),
            page.size(), count) == store::Status::Ok && count > 0U;
        Check(read,
              "recovered root exports a legacy page");
        if (!read) break;
        exported.insert(exported.end(), page.begin(), page.begin() + count);
    }
    Check(exported == legacyBody,
          "reverse export is byte-exact for the old root decoder");
    for (const uint16_t index : {0U, 1U, 31U, 32U, 127U, 255U}) {
        store::Frame actual{};
        Check(reboot.ReadFrame(index, actual) == store::Status::Ok &&
                  Same(actual, source[index]),
              "reboot decodes mixed RGB and effects from fixed slots");
    }
    Check(file.maximumRead <= store::STAGE_SLOT_BYTES &&
              file.maximumWrite <= store::STAGE_SLOT_BYTES,
          "RGB upload keeps fixed bounded storage IO");
}

void RgbWireRejectsMalformedAndInterruptedReplacement() {
    const auto initial = Body({Frame(41U)});
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.BeginWire(Manifest(1U), 0U, 1U,
                  static_cast<uint32_t>(initial.size()),
                  fxe::Crc32(initial.data(), initial.size()), token) ==
                  store::Status::Ok &&
              frames.WriteBodyChunk(token, 0U, initial.data(),
                                    initial.size()) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "legacy resident exists before rejected RGB replacement");
    const uint32_t originalCrc = frames.CurrentBodyCrc32();
    auto frame = Frame(91U);
    for (auto& cell : frame.emitters) {
        cell.effect = 0U;
        cell.effectDurationMs = 0U;
    }
    const auto valid = Body({frame}, false, false,
                            wire::RgbOnlyMode::Enabled);
    Check(frames.BeginWire(Manifest(1U), frames.Generation(), 2U,
              static_cast<uint32_t>(valid.size()),
              fxe::Crc32(valid.data(), valid.size()), token) ==
                  store::Status::Ok &&
              frames.WriteBodyChunk(token, 0U, valid.data(), valid.size()) ==
                  store::Status::Corrupt &&
              frames.Abort(token) == store::Status::Ok,
          "legacy Begin never admits an RGB-only record");
    for (const uint8_t mutation : {0U, 1U}) {
        auto damaged = valid;
        if (mutation == 0U) {
            damaged[store::WIRE_RECORD_HEADER_BYTES] = 5U;
        } else {
            damaged[store::WIRE_RECORD_HEADER_BYTES + 4U] ^= 1U;
        }
        Check(frames.BeginWire(Manifest(1U), frames.Generation(),
                  2U + mutation, static_cast<uint32_t>(damaged.size()),
                  fxe::Crc32(damaged.data(), damaged.size()), token,
                  store::WireBodyMode::Rgb) == store::Status::Ok &&
                  frames.WriteBodyChunk(token, 0U, damaged.data(),
                      damaged.size()) == store::Status::Corrupt &&
                  frames.Abort(token) == store::Status::Ok &&
                  frames.CurrentBodyCrc32() == originalCrc,
              "RGB malformed anchor or frame CRC cannot publish");
    }
    auto noncanonical = valid;
    noncanonical.insert(noncanonical.end(), valid.begin(), valid.end());
    Check(frames.BeginWire(Manifest(2U), frames.Generation(), 8U,
              static_cast<uint32_t>(noncanonical.size()),
              fxe::Crc32(noncanonical.data(), noncanonical.size()), token,
              store::WireBodyMode::Rgb) == store::Status::Ok &&
              frames.WriteBodyChunk(token, 0U, noncanonical.data(),
                  noncanonical.size()) == store::Status::Corrupt &&
              frames.Abort(token) == store::Status::Ok,
          "valid RGB raw CRC cannot excuse a noncanonical repeat");
    Check(frames.BeginWire(Manifest(1U), frames.Generation(), 9U,
              static_cast<uint32_t>(valid.size()), 0U, token,
              store::WireBodyMode::Rgb) == store::Status::Ok &&
              frames.WriteBodyChunk(token, 0U, valid.data(),
                                    valid.size()) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Corrupt &&
              frames.Abort(token) == store::Status::Ok,
          "wrong Begin wire CRC rejects complete decoded body");
    Check(frames.BeginWire(Manifest(1U), frames.Generation(), 10U,
              static_cast<uint32_t>(valid.size()),
              fxe::Crc32(valid.data(), valid.size()), token,
              store::WireBodyMode::Rgb) == store::Status::Ok &&
              frames.WriteBodyChunk(token, 0U, valid.data(), 13U) ==
                  store::Status::Ok,
          "interrupted RGB upload has only a partial wire record");
    file.Crash();
    store::FrameStore reboot(file.Ops());
    store::Frame actual{};
    Check(reboot.Recover() == store::Status::Ok &&
              reboot.CurrentBodyCrc32() == originalCrc &&
              reboot.ReadFrame(0U, actual) == store::Status::Ok &&
              Same(actual, Frame(41U)),
          "power loss keeps prior legacy resident and decoder");
}

size_t RecordOffset(const std::vector<uint8_t>& body,
                    const uint16_t frameIndex) {
    size_t offset = 0U;
    for (uint16_t index = 0U; index < frameIndex; ++index) {
        offset += store::WIRE_RECORD_HEADER_BYTES +
                  fxe::ReadU16LE(body.data() + offset);
    }
    return offset;
}

void PredecessorDeltaCorpusStreamsAndRecoversLosslessly() {
    std::vector<store::Frame> source;
    source.reserve(store::FRAME_COUNT);
    source.push_back(Frame(17U));
    for (uint16_t index = 1U; index < store::FRAME_COUNT; ++index) {
        source.push_back(source.back());
        source.back().durationMs = static_cast<uint16_t>(50U + index);
        auto& emitter = source.back().emitters[index - 1U];
        emitter.color.r ^= 0x5aU;
        emitter.color.g = static_cast<uint8_t>(emitter.color.g + index);
    }
    const auto body = Body(source, true, true);
    bool anchorsAreIndependent = true;
    for (uint16_t index = 0U; index < store::FRAME_COUNT;
         index += store::KEYFRAME_INTERVAL) {
        anchorsAreIndependent = anchorsAreIndependent &&
            body[RecordOffset(body, index) + store::WIRE_RECORD_HEADER_BYTES] ==
                wire::FrameCodec::RAW_TAG;
    }
    Check(anchorsAreIndependent && body.size() <=
              store::WIRE_BODY_MAX_BYTES,
          "predecessor corpus has every independent 32-frame anchor");

    MemoryFile file;
    store::FrameStore frames(file.Ops());
    auto manifest = Manifest(store::FRAME_COUNT);
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.BeginWire(manifest, 0U, 81U,
                               static_cast<uint32_t>(body.size()),
                               fxe::Crc32(body.data(), body.size()), token) ==
                  store::Status::Ok,
          "predecessor corpus begins on a blank store");
    uint32_t cursor = 0U;
    while (cursor < body.size()) {
        const size_t count = std::min<size_t>(499U, body.size() - cursor);
        Check(frames.WriteBodyChunk(token, cursor, body.data() + cursor,
                                    count) == store::Status::Ok,
              "predecessor corpus streams across ATT512 packet bodies");
        cursor += static_cast<uint32_t>(count);
    }
    Check(cursor == body.size() &&
              frames.Commit(token) == store::Status::Ok,
          "ATT512 chunks commit the canonical predecessor corpus");

    store::FrameStore recovered(file.Ops());
    Check(recovered.Recover() == store::Status::Ok &&
              recovered.CurrentManifest().predecessorDeltas,
          "predecessor root flag survives recovery");
    for (const uint16_t index : {0U, 1U, 127U, 255U}) {
        store::Frame actual{};
        Check(recovered.ReadFrame(index, actual) == store::Status::Ok &&
                  Same(actual, source[index]),
              "cold and sequential predecessor reads remain lossless");
    }
}

void PredecessorRootOnlyCopyAndDeleteKeepPhysicalChain() {
    std::vector<store::Frame> source{Frame(41U)};
    for (uint16_t index = 1U; index < 4U; ++index) {
        source.push_back(source.back());
        source.back().durationMs = static_cast<uint16_t>(100U + index);
        source.back().emitters[index].color.r ^= 0xa5U;
    }
    const auto body = Body(source, true);
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    auto manifest = Manifest(4U);
    manifest.predecessorDeltas = true;
    store::TransactionToken create{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.BeginWire(manifest, 0U, 91U,
                               static_cast<uint32_t>(body.size()),
                               fxe::Crc32(body.data(), body.size()), create) ==
                  store::Status::Ok &&
              frames.WriteBodyChunk(create, 0U, body.data(), body.size()) ==
                  store::Status::Ok &&
              frames.Commit(create) == store::Status::Ok,
          "predecessor root-only edit fixture commits");

    file.bodyBytesWritten = 0U;
    store::TransactionToken copy{};
    Check(frames.BeginEdit(1U, 92U, copy) == store::Status::Ok &&
              frames.CopyFrameAfter(copy, 1U) == store::Status::Ok &&
              frames.Commit(copy) == store::Status::Ok &&
              file.bodyBytesWritten == 0U,
          "predecessor copy inserts one logical offset without rewriting body");
    const uint16_t copiedSource[]{0U, 1U, 1U, 2U, 3U};
    for (uint16_t index = 0U; index < 5U; ++index) {
        store::Frame actual{};
        Check(frames.ReadFrame(index, actual) == store::Status::Ok &&
                  Same(actual, source[copiedSource[index]]),
              "copied predecessor frame decodes through physical chain");
    }

    store::TransactionToken remove{};
    Check(frames.BeginEdit(2U, 93U, remove) == store::Status::Ok &&
              frames.DeleteFrame(remove, 3U) == store::Status::Ok &&
              frames.Commit(remove) == store::Status::Ok &&
              file.bodyBytesWritten == 0U,
          "predecessor middle delete hides one offset without rewriting body");
    const uint16_t deletedSource[]{0U, 1U, 1U, 3U};
    for (uint16_t index = 0U; index < 4U; ++index) {
        store::Frame actual{};
        Check(frames.ReadFrame(index, actual) == store::Status::Ok &&
                  Same(actual, source[deletedSource[index]]),
              "deleted predecessor frame leaves later physical deltas decodable");
    }
}

void WarmCacheCannotPoisonFrameZeroReplacement() {
    std::vector<store::Frame> source{Frame(31U)};
    for (uint16_t index = 1U; index < 4U; ++index) {
        source.push_back(source.back());
        source.back().durationMs = static_cast<uint16_t>(130U + index);
        source.back().emitters[index].color.r ^= 0x5aU;
    }
    const auto body = Body(source, true);
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    auto manifest = Manifest(static_cast<uint16_t>(source.size()));
    manifest.predecessorDeltas = true;
    store::TransactionToken create{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.BeginWire(manifest, 0U, 101U,
                               static_cast<uint32_t>(body.size()),
                               fxe::Crc32(body.data(), body.size()), create) ==
                  store::Status::Ok &&
              frames.WriteBodyChunk(create, 0U, body.data(), body.size()) ==
                  store::Status::Ok &&
              frames.Commit(create) == store::Status::Ok,
          "warm-cache replacement fixture commits predecessor artwork");
    store::Frame warmed{};
    Check(frames.ReadFrame(0U, warmed) == store::Status::Ok &&
              Same(warmed, source[0]),
          "replacement fixture warms the committed frame-zero cache");

    const auto replacement = Frame(91U);
    store::TransactionToken edit{};
    Check(frames.BeginEdit(frames.Generation(), 102U, edit) ==
                  store::Status::Ok &&
              frames.ReplaceFrame(edit, 0U, replacement) ==
                  store::Status::Ok &&
              frames.Commit(edit) == store::Status::Ok,
          "warmed frame-zero replacement commits");
    for (uint16_t index = 0U; index < source.size(); ++index) {
        store::Frame actual{};
        const auto& expected = index == 0U ? replacement : source[index];
        Check(frames.ReadFrame(index, actual) == store::Status::Ok &&
                  Same(actual, expected),
              "frame-zero replacement cannot rebase an untouched suffix onto the replacement");
    }
}

void AbortedTransferCannotPoisonCommittedDecodeCache() {
    std::vector<store::Frame> source{Frame(41U)};
    source.push_back(source.back());
    source.back().durationMs = 142U;
    source.back().emitters[7U].color.g ^= 0xa5U;
    const auto originalBody = Body(source, true);
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    auto manifest = Manifest(static_cast<uint16_t>(source.size()));
    manifest.predecessorDeltas = true;
    store::TransactionToken create{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.BeginWire(manifest, 0U, 111U,
                               static_cast<uint32_t>(originalBody.size()),
                               fxe::Crc32(originalBody.data(), originalBody.size()),
                               create) == store::Status::Ok &&
              frames.WriteBodyChunk(create, 0U, originalBody.data(),
                                    originalBody.size()) == store::Status::Ok &&
              frames.Commit(create) == store::Status::Ok,
          "aborted transfer fixture commits predecessor artwork");
    store::Frame warmed{};
    Check(frames.ReadFrame(0U, warmed) == store::Status::Ok,
          "aborted transfer fixture warms committed frame zero");

    std::vector<store::Frame> replacement{Frame(81U)};
    replacement.push_back(replacement.back());
    replacement.back().durationMs = 182U;
    const auto replacementBody = Body(replacement, true);
    const size_t firstRecordBytes =
        RecordOffset(replacementBody, 1U);
    store::TransactionToken transfer{};
    Check(frames.BeginWire(manifest, frames.Generation(), 112U,
                           static_cast<uint32_t>(replacementBody.size()),
                           fxe::Crc32(replacementBody.data(),
                                      replacementBody.size()),
                           transfer) == store::Status::Ok &&
              frames.WriteBodyChunk(transfer, 0U, replacementBody.data(),
                                    firstRecordBytes) == store::Status::Ok &&
              frames.Abort(transfer) == store::Status::Ok,
          "replacement aborts after its first frame overwrites decode scratch");
    store::Frame actual{};
    Check(frames.ReadFrame(1U, actual) == store::Status::Ok &&
              Same(actual, source[1]),
          "aborted replacement cannot poison subsequent committed reads");
    store::FrameStore::ResidentDescriptor resident{};
    Check(frames.CurrentResident(resident) &&
              frames.SelectResident(resident.rootSlot, resident.generation,
                                     resident.bodyCrc32) == store::Status::Ok &&
              Same(frames.DecodedFrame(), source[0]),
          "fresh resident selection refreshes scratch after an aborted upload");
}

void CommittedPagingPreservesIncomingPreviewAndPredecessor() {
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    const std::vector<store::Frame> original{Frame(17U), Frame(18U)};
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(Manifest(2U), 0U, 71U, token) == store::Status::Ok &&
              frames.WriteFrame(token, 0U, original[0U]) == store::Status::Ok &&
              frames.WriteFrame(token, 1U, original[1U]) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "concurrent committed paging fixture has two durable frames");

    std::vector<store::Frame> incoming{Frame(81U)};
    incoming.push_back(incoming[0U]);
    incoming[1U].durationMs = 182U;
    incoming[1U].emitters[3U].color.g ^= 0x31U;
    const auto body = Body(incoming, true);
    const size_t firstBytes = RecordOffset(body, 1U);
    auto manifest = Manifest(2U);
    manifest.predecessorDeltas = true;
    Check(frames.BeginWire(manifest, frames.Generation(), 72U,
                           static_cast<uint32_t>(body.size()),
                           fxe::Crc32(body.data(), body.size()), token) ==
                  store::Status::Ok &&
              frames.WriteBodyChunk(token, 0U, body.data(), firstBytes) ==
                  store::Status::Ok && frames.IncomingPreviewReady() &&
              Same(frames.IncomingFirstFrame(), incoming[0U]),
          "incoming first frame is validated but remains unpublished");
    store::Frame committedCopy{};
    Check(frames.ReadFrame(1U, committedCopy) == store::Status::Ok &&
              Same(committedCopy, original[1U]) &&
              Same(frames.DecodedFrame(), original[1U]) &&
              Same(frames.IncomingFirstFrame(), incoming[0U]),
          "paging old artwork reuses decode scratch without changing incoming preview");
    Check(frames.WriteBodyChunk(token, static_cast<uint32_t>(firstBytes),
                               body.data() + firstBytes,
                               body.size() - firstBytes) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok &&
              Same(committedCopy, original[1U]),
          "resumed predecessor parsing commits without mutating a caller-owned read");
    store::Frame readback{};
    Check(frames.ReadFrame(0U, readback) == store::Status::Ok &&
              Same(readback, incoming[0U]) &&
              frames.ReadFrame(1U, readback) == store::Status::Ok &&
              Same(readback, incoming[1U]),
          "incoming delta uses its own predecessor despite interleaved committed paging");
}

void CorruptOtherSlotCannotPoisonIndependentFrame() {
    std::vector<store::Frame> source{Frame(51U)};
    source.push_back(source.back());
    source.back().durationMs = 152U;
    source.back().emitters[3U].color.r ^= 0x31U;
    source.push_back(Frame(151U));
    source.push_back(source.back());
    source.back().durationMs = 253U;
    source.back().emitters[7U].color.g ^= 0x73U;
    auto body = Body(source, true);
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    auto manifest = Manifest(static_cast<uint16_t>(source.size()));
    manifest.predecessorDeltas = true;
    store::TransactionToken create{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.BeginWire(manifest, 0U, 121U,
                               static_cast<uint32_t>(body.size()),
                               fxe::Crc32(body.data(), body.size()), create) ==
                  store::Status::Ok &&
              frames.WriteBodyChunk(create, 0U, body.data(), body.size()) ==
                  store::Status::Ok &&
              frames.Commit(create) == store::Status::Ok,
          "independent-slot recovery fixture commits");
    const size_t corruptPayload =
        modules::litchcraft_frame_store_layout::Layout::SlotOffset(0U, 1U) +
        6U;
    file.bytes[corruptPayload] ^= 0x80U;
    store::Frame actual{};
    Check(frames.ReadFrame(1U, actual) == store::Status::Corrupt &&
              frames.ReadFrame(3U, actual) == store::Status::Ok &&
              Same(actual, source[3]),
          "corrupt slot fails closed without poisoning another frame");

    file.bytes[corruptPayload] ^= 0x80U;
    const size_t slotHeader = corruptPayload - 6U;
    file.bytes[slotHeader] = 0U;
    Check(frames.ReadFrame(1U, actual) == store::Status::Corrupt &&
              frames.ReadFrame(3U, actual) == store::Status::Ok &&
              Same(actual, source[3]),
          "malformed slot length cannot shift any other frame boundary");
}

void GeneratedRepeated256StaysCompressed() {
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    const auto manifest = Manifest(store::FRAME_COUNT);
    const auto repeated = Frame(7U);
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "blank compressed store begins");
    for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
        Check(frames.WriteFrame(token, index, repeated) == store::Status::Ok,
              "all repeated frames stream directly to inactive bank");
    }
    Check(frames.Commit(token) == store::Status::Ok &&
              frames.CurrentBodyBytes() == 3424U &&
              file.bodyBytesWritten ==
                  store::FRAME_COUNT * store::STAGE_SLOT_BYTES,
          "256 repeated frames keep a 3424-byte wire body in independent fixed slots");

    store::FrameStore recovered(file.Ops());
    store::Frame decoded{};
    Check(recovered.Recover() == store::Status::Ok &&
              recovered.CurrentManifest().frameCount == 256U,
          "compressed generation recovers all 256 frame offsets");
    file.readCalls = 0U;
    file.maximumRead = 0U;
    Check(recovered.ReadFrame(255U, decoded) == store::Status::Ok &&
              Same(decoded, repeated) && file.readCalls <= 2U &&
              file.maximumRead <= store::STAGE_SLOT_BYTES,
          "cold frame 255 reads only its independent bounded slot");
}

void ExactWireBodyStreamsAcrossArbitraryChunks() {
    const std::vector<store::Frame> source{
        Frame(1U), Frame(1U), Frame(2U), Frame(2U), Frame(9U)};
    const auto body = Body(source);
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.BeginWire(Manifest(source.size()), 0U, 11U,
                               static_cast<uint32_t>(body.size()),
                               fxe::Crc32(body.data(), body.size()),
                               token) == store::Status::Ok,
          "exact app wire body begins");
    uint32_t cursor = 0U;
    constexpr size_t chunks[]{1U, 230U, 7U, 19U, 231U};
    size_t chunk = 0U;
    while (cursor < body.size()) {
        const size_t count =
            std::min(chunks[chunk++ % std::size(chunks)],
                     body.size() - cursor);
        Check(frames.WriteBodyChunk(token, cursor, body.data() + cursor,
                                    count) == store::Status::Ok,
              "arbitrary ATT chunk boundary preserves continuous cursor");
        cursor += static_cast<uint32_t>(count);
    }
    Check(frames.Checkpoint(token, cursor) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok &&
              frames.CurrentBodyBytes() == body.size(),
          "checkpoint and commit validate exact body and frame CRCs");
    std::vector<uint8_t> readback(body.size());
    size_t readbackBytes = 0U;
    Check(frames.ReadWireBody(frames.Generation(), 0U, readback.data(),
                              readback.size(), readbackBytes) ==
                  store::Status::Ok && readbackBytes == body.size() &&
              readback == body,
          "independent slots reconstruct the transferred schema-4 body exactly");
    for (uint16_t index = 0U; index < source.size(); ++index) {
        store::Frame actual{};
        Check(frames.ReadFrame(index, actual) == store::Status::Ok &&
                  Same(actual, source[index]),
              "streamed wire frame reads losslessly");
    }
}

void TrailingWireBytesCannotIndexPastTheBlockSummaries() {
    for (const uint16_t frameCount : {1U, unsigned(store::FRAME_COUNT)}) {
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    const auto valid = Body(std::vector<store::Frame>(frameCount, Frame(1U)), true, true);
    auto extra = valid;
    extra.push_back(0x5aU);
    store::TransactionToken token{};
    auto manifest = Manifest(frameCount);
    manifest.predecessorDeltas = true;
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.BeginWire(manifest, 0U, 11U,
                               static_cast<uint32_t>(extra.size()),
                               fxe::Crc32(extra.data(), extra.size()), token) ==
                  store::Status::Ok &&
              frames.WriteBodyChunk(token, 0U, extra.data(), extra.size()) ==
                  store::Status::Corrupt &&
              frames.Abort(token) == store::Status::Ok &&
              !frames.HasArtwork(),
          "full-consume API rejects trailing bytes even when all 256 valid records exhaust its frame budget");
    }
}

void CorruptTransferCannotReplaceAuthority() {
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::TransactionToken first{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(Manifest(1U), 0U, 1U, first) == store::Status::Ok &&
              frames.WriteFrame(first, 0U, Frame(1U)) == store::Status::Ok &&
              frames.Commit(first) == store::Status::Ok,
          "last-good authority fixture commits");

    auto body = Body({Frame(2U), Frame(3U)});
    body.back() ^= 0x80U;
    store::TransactionToken corrupt{};
    Check(frames.BeginWire(Manifest(2U), 1U, 2U,
                           static_cast<uint32_t>(body.size()),
                           fxe::Crc32(body.data(), body.size()),
                           corrupt) == store::Status::Ok,
          "corrupt frame transfer begins");
    const auto ingest = frames.WriteBodyChunk(
        corrupt, 0U, body.data(), body.size());
    Check(ingest == store::Status::Corrupt &&
              frames.Commit(corrupt) != store::Status::Ok,
          "per-frame CRC rejects corruption before root publication");
    file.Crash();
    store::FrameStore recovered(file.Ops());
    store::Frame actual{};
    Check(recovered.Recover() == store::Status::Ok &&
              recovered.Generation() == 1U &&
              recovered.ReadFrame(0U, actual) == store::Status::Ok &&
              Same(actual, Frame(1U)),
          "failed transfer leaves prior generation authoritative after crash");
}

void UnfinishedReplacementDoesNotMutateDurableAuthority() {
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::TransactionToken first{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(Manifest(1U), 0U, 1U, first) == store::Status::Ok &&
              frames.WriteFrame(first, 0U, Frame(1U)) == store::Status::Ok &&
              frames.Commit(first) == store::Status::Ok,
          "unfinished replacement fixture commits its authority");

    const auto body = Body({Frame(2U)});
    const size_t writesBefore = file.events;
    const size_t syncsBefore = file.syncs;
    store::TransactionToken replacement{};
    Check(frames.BeginWire(Manifest(1U), 1U, 2U,
                           static_cast<uint32_t>(body.size()),
                           fxe::Crc32(body.data(), body.size()),
                           replacement) == store::Status::Ok &&
              file.events == writesBefore && file.syncs == syncsBefore,
          "beginning a replacement performs no redundant durable root write");
    Check(frames.WriteBodyChunk(replacement, 0U, body.data(), body.size()) ==
              store::Status::Ok,
          "unfinished replacement may stream into its inactive body");

    file.Crash();
    store::FrameStore recovered(file.Ops());
    store::Frame actual{};
    Check(recovered.Recover() == store::Status::Ok &&
              recovered.Generation() == 1U &&
              recovered.ReadFrame(0U, actual) == store::Status::Ok &&
              Same(actual, Frame(1U)),
          "crash before root promotion preserves the prior authority");
}

void TwoResidentsSwitchWithoutWritesAndReplaceInactiveSafely() {
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    const std::vector<store::Frame> framesA(store::FRAME_COUNT, Frame(31U));
    const std::vector<store::Frame> framesB(store::FRAME_COUNT, Frame(32U));
    const std::vector<store::Frame> framesC(store::FRAME_COUNT, Frame(33U));
    const auto bodyA = Body(framesA, true);
    const auto bodyB = Body(framesB, true);
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.BeginWire(Manifest(store::FRAME_COUNT), 0U, 31U,
                               static_cast<uint32_t>(bodyA.size()),
                               fxe::Crc32(bodyA.data(), bodyA.size()), token) ==
                  store::Status::Ok &&
              frames.WriteBodyChunk(token, 0U, bodyA.data(), bodyA.size()) ==
                  store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok &&
              frames.BeginWire(Manifest(store::FRAME_COUNT), 1U, 32U,
                               static_cast<uint32_t>(bodyB.size()),
                               fxe::Crc32(bodyB.data(), bodyB.size()), token) ==
                  store::Status::Ok &&
              frames.WriteBodyChunk(token, 0U, bodyB.data(), bodyB.size()) ==
                  store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "two complete resident fixtures commit into distinct banks");

    store::FrameStore recovered(file.Ops());
    store::FrameStore::ResidentDescriptor residentA{};
    store::FrameStore::ResidentDescriptor residentB{};
    Check(recovered.Recover() == store::Status::Ok &&
              recovered.ResidentCount() == 2U &&
              recovered.Resident(0U, residentA) &&
              recovered.Resident(1U, residentB) &&
              residentA.generation == 1U && residentB.generation == 2U,
          "recovery retains both complete resident identities");
    const size_t eventsBeforeSelection = file.events;
    const size_t syncsBeforeSelection = file.syncs;
    const size_t bodyWritesBeforeSelection = file.bodyBytesWritten;
    store::Frame actual{};
    Check(recovered.SelectResident(
              residentA.rootSlot, residentA.generation,
              residentA.bodyCrc32) == store::Status::Ok &&
              recovered.Generation() == residentA.generation &&
              recovered.ReadFrame(255U, actual) == store::Status::Ok &&
              Same(actual, Frame(31U)) &&
              file.events == eventsBeforeSelection &&
              file.syncs == syncsBeforeSelection &&
              file.bodyBytesWritten == bodyWritesBeforeSelection,
          "selecting the older resident changes playback with zero writes");
    Check(recovered.SelectResident(
              residentB.rootSlot, residentB.generation,
              residentB.bodyCrc32 ^ 1U) == store::Status::RevisionConflict &&
              recovered.Generation() == residentA.generation &&
              recovered.SelectResident(
                  residentB.rootSlot, residentB.generation,
                  residentB.bodyCrc32) == store::Status::Ok &&
              Same(recovered.DecodedFrame(), Frame(32U)) &&
              recovered.ReadFrame(255U, actual) == store::Status::Ok &&
              Same(actual, Frame(32U)) &&
              file.events == eventsBeforeSelection &&
              file.syncs == syncsBeforeSelection,
          "identity mismatch preserves output and exact A/B switches stay write-free");

    MemoryFile corrupt = file;
    store::FrameStore corruptStore(corrupt.Ops());
    const size_t alternateFirstPayload =
        modules::litchcraft_frame_store_layout::Layout::SlotOffset(
            residentB.rootSlot, 0U) + store::WIRE_RECORD_HEADER_BYTES;
    corrupt.bytes[alternateFirstPayload] ^= 1U;
    const size_t corruptWritesBefore = corrupt.events;
    Check(corruptStore.Recover() == store::Status::Ok &&
              corruptStore.SelectResident(
                  residentA.rootSlot, residentA.generation,
                  residentA.bodyCrc32) == store::Status::Ok &&
              corruptStore.SelectResident(
                  residentB.rootSlot, residentB.generation,
                  residentB.bodyCrc32) == store::Status::Corrupt &&
              corruptStore.Generation() == residentA.generation &&
              corrupt.events == corruptWritesBefore,
          "corrupt alternate frame zero rejects selection without replacing the displayed root or writing flash");

    Check(recovered.SelectResident(
              residentA.rootSlot, residentA.generation,
              residentA.bodyCrc32) == store::Status::Ok &&
              recovered.UndoResidentSelection(residentB) &&
              recovered.Generation() == residentB.generation &&
              recovered.SelectResident(
                  residentA.rootSlot, residentA.generation,
                  residentA.bodyCrc32) == store::Status::Ok,
          "presentation rejection can restore the prior resident without a write");
    MemoryFile interrupted = file;
    store::FrameStore interruptedStore(interrupted.Ops());
    store::FrameStore::ResidentDescriptor interruptedA{};
    const auto bodyC = Body(framesC, true);
    Check(interruptedStore.Recover() == store::Status::Ok &&
              interruptedStore.Resident(0U, interruptedA) &&
              interruptedStore.SelectResident(
                  interruptedA.rootSlot, interruptedA.generation,
                  interruptedA.bodyCrc32) == store::Status::Ok &&
              interruptedStore.BeginWire(
                  Manifest(store::FRAME_COUNT), interruptedA.generation, 33U,
                  static_cast<uint32_t>(bodyC.size()),
                  fxe::Crc32(bodyC.data(), bodyC.size()), token) ==
                  store::Status::Ok &&
              token.generation == 3U &&
              interruptedStore.WriteBodyChunk(
                  token, 0U, bodyC.data(), bodyC.size()) == store::Status::Ok,
          "replacement after selecting the older resident uses a globally newer generation");
    interrupted.Crash();
    store::FrameStore afterPowerCut(interrupted.Ops());
    Check(afterPowerCut.Recover() == store::Status::Ok &&
              afterPowerCut.Generation() == residentA.generation &&
              afterPowerCut.ResidentCount() == 1U &&
              afterPowerCut.ReadFrame(255U, actual) == store::Status::Ok &&
              Same(actual, Frame(31U)),
          "power cut during inactive replacement preserves the selected resident");

    Check(recovered.BeginWire(
              Manifest(store::FRAME_COUNT), residentA.generation, 34U,
              static_cast<uint32_t>(bodyC.size()),
              fxe::Crc32(bodyC.data(), bodyC.size()), token) ==
                  store::Status::Ok &&
              token.generation == 3U &&
              recovered.WriteBodyChunk(
                  token, 0U, bodyC.data(), bodyC.size()) == store::Status::Ok &&
              recovered.Commit(token) == store::Status::Ok &&
              recovered.Generation() == 3U &&
              recovered.ResidentCount() == 2U,
          "completed replacement keeps selected A beside new resident C");
}

void ReplacementPreparationFollowsDurableRetirementAndRetriesSafely() {
    MemoryFile file;
    file.prepareBanks = true;
    store::FrameStore frames(file.Ops());
    store::TransactionToken token{};
    for (uint32_t generation = 0U; generation < 2U; ++generation) {
        Check(frames.Begin(Manifest(2U), generation, generation + 1U, token) ==
                  store::Status::Ok &&
                  frames.WriteFrame(token, 0U, Frame(generation + 10U)) ==
                      store::Status::Ok &&
                  frames.WriteFrame(token, 1U, Frame(generation + 20U)) ==
                      store::Status::Ok &&
                  frames.Commit(token) == store::Status::Ok,
              "preparation fixture creates two complete real resident roots");
    }
    Check(file.prepares == 2U && file.preparedOnlyRetiredRoots &&
              file.preparedResident == 1U,
          "whole generated replacements prepare even initially blank roots");
    MemoryFile cut = file;
    cut.events = cut.prepares = 0U;
    cut.faultAfterEvent = store::ROOT_BYTES + 1U;
    store::FrameStore interrupted(cut.Ops());
    Check(interrupted.Recover() == store::Status::Ok &&
              interrupted.Begin(Manifest(2U), 2U, 3U, token) == store::Status::Ok &&
              interrupted.WriteFrame(token, 0U, Frame(90U)) == store::Status::IoFailure &&
              cut.prepares == 0U,
          "first root retirement alone cannot permit preparation or replacement bytes");
    cut.Crash();
    cut.faultAfterEvent = std::numeric_limits<size_t>::max();
    store::FrameStore reboot(cut.Ops());
    store::Frame actual{};
    Check(reboot.Recover() == store::Status::Ok && reboot.Generation() == 2U &&
              reboot.ReadFrame(1U, actual) == store::Status::Ok && Same(actual, Frame(21U)),
          "cut between inactive root barriers preserves selected authority");
    const auto selected = std::vector<uint8_t>(
        file.bytes.begin() + store::FrameStore::BodyOffset(1U),
        file.bytes.begin() + store::FrameStore::BodyOffset(1U) + store::BODY_BANK_BYTES);
    file.failPrepare = true;
    file.prepares = 0U;
    Check(frames.Begin(Manifest(2U), 2U, 3U, token) == store::Status::Ok &&
              frames.WriteFrame(token, 0U, Frame(90U)) == store::Status::IoFailure &&
              file.prepares == 1U && file.preparedResident == 0U &&
              file.preparedOnlyRetiredRoots && frames.ResidentCount() == 1U &&
              frames.Commit(token) == store::Status::Incomplete,
          "preparation failure retires only the alternate and cannot publish");
    file.failPrepare = false;
    store::TransactionToken rejected{};
    Check(frames.Begin(Manifest(2U), 2U, 4U, rejected) == store::Status::Incomplete &&
              frames.WriteFrame(token, 0U, Frame(90U)) == store::Status::Ok &&
              frames.WriteFrame(token, 1U, Frame(91U)) == store::Status::Ok &&
              file.prepares == 2U && frames.Abort(token) == store::Status::Ok &&
              std::equal(selected.begin(), selected.end(),
                         file.bytes.begin() + store::FrameStore::BodyOffset(1U)),
          "retry prepares once before bytes; continuation and cancellation preserve selected bank");
    const auto body = Body({Frame(92U), Frame(93U)});
    const size_t prepares = file.prepares;
    Check(frames.BeginWire(Manifest(2U), 2U, 5U, body.size(),
              fxe::Crc32(body.data(), body.size()), token) == store::Status::Ok &&
              frames.WriteBodyChunk(token, 0U, body.data(), 1U) == store::Status::Ok &&
              frames.WriteBodyChunk(token, 1U, body.data() + 1U, body.size() - 1U) ==
                  store::Status::Ok && file.prepares == prepares + 1U &&
              frames.Abort(token) == store::Status::Ok,
          "new wire transaction prepares again without retruncating resumed chunks");
    Check(frames.BeginEdit(2U, 6U, token) == store::Status::Ok &&
              frames.ReplaceFrame(token, 0U, Frame(94U)) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok &&
              file.prepares == prepares + 1U,
          "ordinary edit bypasses replacement preparation entirely");
}

void MiddleEditsRemainAtomicAndCompact() {
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::TransactionToken create{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(Manifest(4U), 0U, 1U, create) == store::Status::Ok,
          "edit fixture begins");
    for (uint16_t index = 0U; index < 4U; ++index) {
        Check(frames.WriteFrame(create, index, Frame(index + 1U)) ==
                  store::Status::Ok,
              "edit fixture frame writes");
    }
    Check(frames.Commit(create) == store::Status::Ok,
          "edit fixture commits");
    file.bodyBytesWritten = 0U;
    store::TransactionToken reorder{};
    Check(frames.BeginEdit(1U, 2U, reorder) == store::Status::Ok &&
              frames.CopyFrameAfter(reorder, 0U) == store::Status::Ok &&
              frames.DeleteFrame(reorder, 3U) == store::Status::Ok &&
              frames.Commit(reorder) == store::Status::Ok &&
              frames.CurrentBodyBytes() <= store::WIRE_BODY_MAX_BYTES &&
              file.bodyBytesWritten == 0U,
          "copy and delete change only the atomic root");

    store::TransactionToken replace{};
    Check(frames.BeginEdit(2U, 3U, replace) == store::Status::Ok &&
              frames.ReplaceFrame(replace, 2U, Frame(9U)) ==
                  store::Status::Ok &&
              frames.Commit(replace) == store::Status::Ok &&
              frames.CurrentBodyBytes() <= store::BODY_BANK_BYTES,
          "single replace rewrites one compact inactive generation");
    const uint16_t expected[]{1U, 1U, 9U, 4U};
    for (uint16_t index = 0U; index < 4U; ++index) {
        store::Frame actual{};
        Check(frames.ReadFrame(index, actual) == store::Status::Ok &&
                  Same(actual, Frame(expected[index])),
              "middle edit preserves exact logical order");
    }

    store::TransactionToken insert{};
    Check(frames.BeginEdit(3U, 4U, insert) == store::Status::Ok &&
              frames.InsertFrame(insert, 2U, Frame(8U)) ==
                  store::Status::Ok &&
              frames.Commit(insert) == store::Status::Ok,
          "middle insertion commits as a compact generation");
    store::TransactionToken append{};
    Check(frames.BeginEdit(4U, 5U, append) == store::Status::Ok &&
              frames.AppendFrame(append, Frame(7U)) == store::Status::Ok &&
              frames.Commit(append) == store::Status::Ok,
          "append commits without a fixed-slot expansion");
}

void Sparse256FramePaintDoesNotReplayTheWholeBody() {
    std::vector<store::Frame> source;
    source.reserve(store::FRAME_COUNT);
    source.push_back(Frame(17U));
    for (uint16_t index = 1U; index < store::FRAME_COUNT; ++index) {
        source.push_back(source.back());
        source.back().durationMs = static_cast<uint16_t>(50U + index);
        source.back().emitters[index - 1U].color.r ^= 0x5aU;
    }
    const auto body = Body(source, true, true);
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    auto manifest = Manifest(store::FRAME_COUNT);
    manifest.predecessorDeltas = true;
    store::TransactionToken initial{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.BeginWire(manifest, 0U, 1U,
                               static_cast<uint32_t>(body.size()),
                               fxe::Crc32(body.data(), body.size()), initial) ==
                  store::Status::Ok &&
              frames.WriteBodyChunk(initial, 0U, body.data(), body.size()) ==
                  store::Status::Ok &&
              frames.Commit(initial) == store::Status::Ok,
          "256-frame predecessor fixture commits before a single paint");

    auto painted = source[1U];
    painted.emitters[0U].color.g ^= 0x7aU;
    const auto baseline = file;
    const size_t eventsBeforeEdit = file.events;
    file.readCalls = 0U;
    file.bodyBytesWritten = 0U;
    store::TransactionToken edit{};
    const bool committed =
        frames.BeginEdit(frames.Generation(), 2U, edit) == store::Status::Ok &&
        frames.ReplaceFrame(edit, 1U, painted) == store::Status::Ok &&
        frames.Commit(edit) == store::Status::Ok;
    std::printf("256-frame single paint storage reads=%zu body bytes written=%zu\n",
                file.readCalls, file.bodyBytesWritten);
    Check(committed && file.readCalls < 200U,
          "single paint must avoid 256-frame predecessor replay before next selection");
    store::FrameStore::ResidentDescriptor editedResident{};
    std::array<uint8_t,
               modules::litchcraft_frame_store_layout::FRAME_SLOT_BYTES>
        rootScratch{};
    modules::litchcraft_frame_store_layout::Layout roots(
        file.Ops(), rootScratch.data(), rootScratch.size());
    modules::litchcraft_frame_store_layout::Root editedRoot{};
    uint8_t editedCopy = 2U;
    Check(frames.CurrentResident(editedResident) &&
              roots.LatestRoot(editedResident.rootSlot, editedRoot,
                               editedCopy) ==
                  modules::litchcraft_frame_store_layout::Status::Ok &&
              editedRoot.manifest.predecessorDeltas &&
              editedRoot.bodyCrc32 == editedResident.bodyCrc32,
          "fast edit publishes a validated fixed-slot root and wire identity");
    const size_t editEvents = file.events - eventsBeforeEdit;
    for (const size_t cut : std::array<size_t, 4U>{
             0U, 800U, editEvents - store::ROOT_BYTES / 2U,
             editEvents - 1U}) {
        auto interrupted = baseline;
        interrupted.faultAfterEvent = interrupted.events + cut;
        store::FrameStore candidate(interrupted.Ops());
        store::TransactionToken interruptedEdit{};
        const bool rejected = candidate.Recover() == store::Status::Ok &&
            candidate.BeginEdit(candidate.Generation(), 7U, interruptedEdit) ==
                store::Status::Ok &&
            candidate.ReplaceFrame(interruptedEdit, 1U, painted) ==
                store::Status::Ok &&
            candidate.Commit(interruptedEdit) == store::Status::IoFailure;
        interrupted.Crash();
        store::FrameStore restored(interrupted.Ops());
        store::Frame oldFrame{};
        Check(rejected && restored.Recover() == store::Status::Ok &&
                  restored.Generation() == 1U &&
                  restored.ReadFrame(1U, oldFrame) == store::Status::Ok &&
                  Same(oldFrame, source[1U]),
              "interrupted fast edit keeps the previous complete root");
    }
    file.Crash();
    store::FrameStore reboot(file.Ops());
    store::Frame actual{};
    Check(reboot.Recover() == store::Status::Ok &&
              reboot.ReadFrame(1U, actual) == store::Status::Ok &&
              Same(actual, painted) &&
              reboot.ReadFrame(255U, actual) == store::Status::Ok &&
              Same(actual, source[255U]),
          "single paint and its untouched successor survive power loss");

    auto latePaint = source[200U];
    latePaint.emitters[199U].color.b ^= 0x55U;
    file.readCalls = 0U;
    store::TransactionToken lateEdit{};
    const bool lateCommitted =
        reboot.BeginEdit(reboot.Generation(), 3U, lateEdit) == store::Status::Ok &&
        reboot.ReplaceFrame(lateEdit, 200U, latePaint) == store::Status::Ok &&
        reboot.Commit(lateEdit) == store::Status::Ok;
    Check(lateCommitted && file.readCalls < 200U,
          "recovered contiguous root keeps a late-frame paint bounded");
    file.Crash();
    store::FrameStore final(file.Ops());
    Check(final.Recover() == store::Status::Ok &&
              final.ReadFrame(1U, actual) == store::Status::Ok &&
              Same(actual, painted) &&
              final.ReadFrame(200U, actual) == store::Status::Ok &&
              Same(actual, latePaint) &&
              final.ReadFrame(255U, actual) == store::Status::Ok &&
              Same(actual, source[255U]),
          "early and late paints remain exact across two power cycles");
}

void Dense256FramePaintWritesOnlyOneStage() {
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    auto manifest = Manifest(store::FRAME_COUNT);
    manifest.predecessorDeltas = true;
    store::TransactionToken initial{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(manifest, 0U, 1U, initial) == store::Status::Ok,
          "dense 256-frame edit fixture begins");
    for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
        Check(frames.WriteFrame(initial, index, Frame(index + 1U)) ==
                  store::Status::Ok,
              "dense 256-frame fixture writes each source frame");
    }
    Check(frames.Commit(initial) == store::Status::Ok,
          "dense 256-frame fixture commits");
    file.readCalls = 0U;
    file.bodyBytesWritten = 0U;
    store::TransactionToken edit{};
    const bool committed =
        frames.BeginEdit(frames.Generation(), 2U, edit) == store::Status::Ok &&
        frames.ReplaceFrame(edit, 1U, Frame(501U)) == store::Status::Ok &&
        frames.Commit(edit) == store::Status::Ok;
    std::printf("[DENSE256_EDIT_IO] reads=%zu body_bytes_written=%zu\n",
                file.readCalls, file.bodyBytesWritten);
    Check(committed && file.readCalls < 500U &&
              file.bodyBytesWritten <= store::STAGE_SLOT_BYTES,
          "dense edit writes only one staged frame, never the full bank");
    file.Crash();
    store::FrameStore reboot(file.Ops());
    store::Frame actual{};
    Check(reboot.Recover() == store::Status::Ok &&
              reboot.ReadFrame(1U, actual) == store::Status::Ok &&
              Same(actual, Frame(501U)) &&
              reboot.ReadFrame(255U, actual) == store::Status::Ok &&
              Same(actual, Frame(256U)),
          "dense edit retains exact first and last pixels after reboot");
}

void DeletingFirstFrameUpdatesOnlyTheRootMap() {
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::TransactionToken create{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(Manifest(4U), 0U, 21U, create) ==
                  store::Status::Ok,
          "frame-zero deletion fixture begins");
    for (uint16_t index = 0U; index < 4U; ++index) {
        Check(frames.WriteFrame(create, index, Frame(index + 1U)) ==
                  store::Status::Ok,
              "asymmetric frame-zero deletion fixture writes");
    }
    Check(frames.Commit(create) == store::Status::Ok,
          "frame-zero deletion fixture commits");

    file.bodyBytesWritten = 0U;
    store::TransactionToken remove{};
    Check(frames.BeginEdit(1U, 22U, remove) == store::Status::Ok &&
              frames.DeleteFrame(remove, 0U) == store::Status::Ok &&
              frames.Commit(remove) == store::Status::Ok &&
              file.bodyBytesWritten == 0U,
          "deleting logical frame zero changes only fixed-slot root metadata");

    store::FrameStore recovered(file.Ops());
    Check(recovered.Recover() == store::Status::Ok &&
              recovered.CurrentManifest().frameCount == 3U,
          "rebased frame-zero deletion recovers");
    for (uint16_t index = 0U; index < 3U; ++index) {
        store::Frame actual{};
        Check(recovered.ReadFrame(index, actual) == store::Status::Ok &&
                  Same(actual, Frame(index + 2U)),
              "every remaining asymmetric frame decodes against the new base");
    }
}

void CapacityAndBoundedReads() {
    Check(store::FRAME_FILES_CAPACITY == 564297U &&
              store::ARTWORK_FILE_CAPACITY == 576729U &&
              store::FILESYSTEM_SETTINGS_RECOVERY_REMAINDER_BYTES == 332345U,
          "unchanged frame offsets plus four derived pages preserve over 320 KiB");
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(Manifest(1U), 0U, 1U, token) == store::Status::Ok &&
              frames.WriteFrame(token, 0U, Frame(4U)) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "bounded read fixture commits");
    store::FrameStore::ReadCursor cursor{};
    Check(frames.BeginReadFrame(0U, cursor) == store::Status::Ok &&
              frames.ContinueReadFrame(cursor, 512U) ==
                  store::Status::Incomplete &&
              frames.ContinueReadFrame(cursor, 512U) ==
                  store::Status::Incomplete &&
              frames.ContinueReadFrame(cursor, 512U) ==
                  store::Status::Ok &&
              file.maximumRead <= store::STAGE_SLOT_BYTES &&
              Same(frames.DecodedFrame(), Frame(4U)),
          "dense frame reading is restartable and bounded");
}

void CommitTimingObserverReceivesStorePhases() {
    MemoryFile file;
    TimingEvents events{};
    store::FrameStore frames(file.Ops());
    frames.SetTimingObserver(&ObserveTiming, &events);
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(Manifest(1U), 0U, 91U, token) == store::Status::Ok &&
              frames.WriteFrame(token, 0U, Frame(17U)) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "timed Store commit succeeds");
    Check(events.bodyWrites >= 1U && events.bodyPrepare == 1U &&
              events.rootPublish == 1U,
          "Store timing observer distinguishes body writes, durability and root publication");
}


void ProductionReadMetricCountsOnlyReturnedFileBytes() {
    using File = firmware::LitchcraftFrameFile;
    struct Probe {
        unsigned acquired = 0U, released = 0U;
        size_t returned = 0U;
        bool active = false;
    } probe;
    File file("build/read_metric.r0", "build/read_metric.r1",
              "build/read_metric.b0", "build/read_metric.b1");
    Check(file.Remove(), "actual-read metric starts with isolated files");
    Check(file.SetAccessOps({&probe,
        [](void* context) {
            auto& state = *static_cast<Probe*>(context);
            Check(!state.active, "metric read does not nest a lease");
            state.active = true; ++state.acquired;
        },
        [](void* context, File::AccessOperation operation, size_t bytes, uint32_t) {
            auto& state = *static_cast<Probe*>(context);
            Check(state.active, "actual read releases exactly its acquired lease");
            state.active = false; ++state.released;
            if (operation == File::AccessOperation::Read) state.returned = bytes;
        }}), "read metric uses the production access callback");
    const auto ops = file.StoreOps();
    const auto offset = store::FrameStore::BodyOffset(0U);
    std::array<uint8_t, 16U> bytes{};
    Check(ops.read(ops.context, offset, bytes.data(), bytes.size()) && probe.returned == 0U,
          "erased missing span padding is never counted as actual file data");
    bytes.fill(0x39U);
    Check(ops.write(ops.context, offset, bytes.data(), bytes.size()) && ops.sync(ops.context),
          "actual read metric seeds a complete real file");
    Check(ops.read(ops.context, offset, bytes.data(), bytes.size()) && probe.returned == bytes.size(),
          "complete read reports exactly returned real file bytes");
    // Close the retained stdio reader before an external fixture mutation;
    // otherwise libc is allowed to return its previous valid read buffer.
    file.CloseCachedHandles();
    Check(::truncate("build/read_metric.b0.00", 4U) == 0,
          "controlled short source contains only four actual file bytes");
    Check(ops.read(ops.context, offset, bytes.data(), bytes.size()) && probe.returned == 4U &&
              bytes[4U] == 0xffU && bytes.back() == 0xffU,
          "sparse short source reports four file bytes and excludes erased span padding");
    file.CloseCachedHandles();
    Check(::unlink("build/read_metric.b0.00") == 0 &&
              ::mkdir("build/read_metric.b0.00", 0700) == 0,
          "controlled directory source makes the production file read fail");
    Check(!ops.read(ops.context, offset, bytes.data(), bytes.size()) && probe.returned == 0U,
          "failed filesystem read reports no fabricated returned bytes");
    Check(::rmdir("build/read_metric.b0.00") == 0,
          "remove the isolated read-failure source");
    Check(probe.acquired == probe.released && !probe.active,
          "read, missing and failure paths each release their lease once");
    Check(file.Remove(), "actual read metric removes isolated files");
}

void ProductionFileMappingHasNoInactiveBankGap() {
    using File = firmware::LitchcraftFrameFile;
    File::PhysicalSpan span{};
    Check(File::ResolvePhysicalSpan(store::FrameStore::RootOffset(0U) + 5U,
                                    16U, span) &&
              span.region == File::Region::Root0 && span.offset == 5U,
          "root 0 maps into its compact physical file");
    Check(File::ResolvePhysicalSpan(store::FrameStore::RootOffset(1U),
                                    store::ROOT_BYTES, span) &&
              span.region == File::Region::Root1 && span.offset == 0U,
          "root 1 starts at physical offset zero");
    Check(File::ResolvePhysicalSpan(store::FrameStore::BodyOffset(0U) + 37U,
                                    512U, span) &&
              span.region == File::Region::Body0 && span.offset == 37U,
          "body bank 0 retains only its local offset");
    Check(File::ResolvePhysicalSpan(store::FrameStore::BodyOffset(1U),
                                    512U, span) &&
              span.region == File::Region::Body1 && span.offset == 0U,
          "first alternate-bank write has no 285 KiB physical gap");
    Check(!File::ResolvePhysicalSpan(
              store::FrameStore::BodyOffset(1U) - 1U, 2U, span),
          "one access cannot cross internal atomic-file boundaries");

    const char* const root0 = "test_frame_mapping.r0";
    const char* const root1 = "test_frame_mapping.r1";
    const char* const body0 = "test_frame_mapping.b0";
    const char* const body1 = "test_frame_mapping.b1";
    File file(root0, root1, body0, body1);
    Check(file.Remove(), "production mapping fixture starts blank");
    store::FrameStore frames(file.StoreOps());
    store::TransactionToken first{};
    store::TransactionToken second{};
    Check(frames.Recover() == store::Status::NoArtwork,
          "production file adapter recovers blank");
    Check(frames.Begin(Manifest(1U), 0U, 71U, first) == store::Status::Ok,
          "production file adapter begins first resident");
    Check(frames.WriteFrame(first, 0U, Frame(31U)) == store::Status::Ok,
          "production file adapter writes first resident");
    Check(frames.Commit(first) == store::Status::Ok,
          "production file adapter commits first resident");
    Check(frames.Begin(Manifest(1U), 1U, 72U, second) == store::Status::Ok,
          "production file adapter begins second resident");
    Check(frames.WriteFrame(second, 0U, Frame(32U)) == store::Status::Ok,
          "production file adapter writes second resident");
    Check(frames.Commit(second) == store::Status::Ok,
          "two real commits alternate through the production file adapter");
    const auto fileSize = [](const char* const path) {
        std::FILE* const handle = std::fopen(path, "rb");
        if (handle == nullptr) return size_t(0U);
        const bool sought = std::fseek(handle, 0L, SEEK_END) == 0;
        const long bytes = sought ? std::ftell(handle) : -1L;
        std::fclose(handle);
        return bytes < 0L ? size_t(0U) : static_cast<size_t>(bytes);
    };
    const size_t bank0Bytes = fileSize((std::string(body0) + ".00").c_str());
    const size_t bank1Bytes = fileSize((std::string(body1) + ".00").c_str());
    store::Frame actual{};
    Check(bank0Bytes > 0U && bank1Bytes > 0U &&
              bank0Bytes <= store::WIRE_RECORD_HEADER_BYTES +
                  store::WIRE_FRAME_MAX_BYTES &&
              bank1Bytes <= store::WIRE_RECORD_HEADER_BYTES +
                  store::WIRE_FRAME_MAX_BYTES &&
              frames.ReadFrame(0U, actual) == store::Status::Ok &&
              Same(actual, Frame(32U)),
          "alternate bank stores only encoded bytes and remains authoritative");
    Check(file.Remove(), "production mapping fixture removes all internal banks");
    File::BenchmarkResult benchmark{};
    std::array<uint8_t, File::ACCESS_BYTES> benchmarkPage{};
    Check(file.Benchmark(2U * File::ACCESS_BYTES + 17U, benchmark,
                         benchmarkPage.data(), benchmarkPage.size()) &&
              benchmark.passed &&
              benchmark.bytes == 2U * File::ACCESS_BYTES + 17U,
          "production frame-file benchmark verifies full and partial reused pages");
    const std::array<uint8_t, 3U> sparse{{1U, 2U, 3U}};
    std::array<uint8_t, 103U> sparseRead{};
    const auto ops = file.StoreOps();
    Check(ops.write(ops.context, store::FrameStore::RootOffset(0U) + 100U,
                    sparse.data(), sparse.size()) &&
              ops.sync(ops.context) &&
              ops.read(ops.context, store::FrameStore::RootOffset(0U),
                       sparseRead.data(), sparseRead.size()) &&
              std::all_of(sparseRead.begin(), sparseRead.begin() + 100U,
                          [](const uint8_t value) { return value == 0xffU; }) &&
              std::equal(sparse.begin(), sparse.end(),
                         sparseRead.begin() + 100U) &&
              file.Remove(),
          "sparse frame-file write fills unwritten bytes as erased flash");

    const size_t bodyOffset = store::FrameStore::BodyOffset(0U);
    const std::array<uint8_t, 4U> firstBytes{{1U, 2U, 3U, 4U}};
    const std::array<uint8_t, 2U> changedBytes{{9U, 8U}};
    std::array<uint8_t, 4U> before{};
    std::array<uint8_t, 4U> after{};
    const uint32_t priorBodyOpens = file.BodyReadOpensForTest();
    Check(ops.write(ops.context, bodyOffset, firstBytes.data(),
                    firstBytes.size()) && ops.sync(ops.context) &&
              ops.read(ops.context, bodyOffset, before.data(), before.size()) &&
              ops.read(ops.context, bodyOffset + 1U,
                       before.data() + 1U, before.size() - 1U) &&
              file.BodyReadOpensForTest() == priorBodyOpens + 1U &&
              before == firstBytes &&
              ops.write(ops.context, bodyOffset + 1U, changedBytes.data(),
                        changedBytes.size()) && ops.sync(ops.context) &&
              ops.read(ops.context, bodyOffset, after.data(), after.size()) &&
              file.BodyReadOpensForTest() == priorBodyOpens + 2U &&
              after == std::array<uint8_t, 4U>{{1U, 9U, 8U, 4U}} &&
              file.Remove(),
          "reused body reader observes a later write and closes before removal");
}

void FrameFileClassifiesPrimitiveLatencyAndLeaseWait() {
    using File = firmware::LitchcraftFrameFile;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 20U);
    File file("frame_timing.r0", "frame_timing.r1",
              "frame_timing.b0", "frame_timing.b1");
    Check(file.Remove(), "latency fixture removes its private native files");
    struct Cost final { uint32_t wait = 7U; uint32_t held = 23U; } cost;
    Check(file.SetAccessOps({&cost,
        [](void* context) { fxe::ManualMillis::AdvanceMs(static_cast<Cost*>(context)->wait); },
        [](void* context, firmware::LitchcraftFrameFile::AccessOperation, size_t, uint32_t) { fxe::ManualMillis::AdvanceMs(static_cast<Cost*>(context)->held); }}),
          "existing file access hooks model lease wait and controlled primitive cost");
    const auto ops = file.StoreOps();
    const size_t offset = store::FrameStore::BodyOffset(0U);
    std::array<uint8_t, 13U> bytes{};
    file.ResetAccessTiming();
    Check(ops.read(ops.context, offset, bytes.data(), bytes.size()),
          "timed native read traverses production file mapping and access hooks");
    const auto read = file.GetAccessTiming();
    Check(read.maximumMs == 30U && read.maximumLeaseWaitMs == 7U &&
              read.maximumOperation == File::AccessOperation::Read &&
              read.maximumBytes == bytes.size() && read.maximumOffset == offset &&
              read.accesses == 1U,
          "read timing attributes lease wait separately and survives millis wrap");
    cost = {3U, 5997U};
    Check(ops.write(ops.context, offset, bytes.data(), bytes.size()),
          "controlled six-second primitive traverses real native WriteAt");
    cost = {11U, 89U};
    Check(ops.sync(ops.context), "timed Sync retains real native durability barrier");
    const auto slow = file.GetAccessTiming();
    Check(slow.maximumMs == 6000U && slow.maximumLeaseWaitMs == 11U &&
              slow.maximumOperation == File::AccessOperation::Write &&
              slow.maximumBytes == bytes.size() && slow.maximumOffset == offset &&
              slow.accesses == 3U,
          "maximum primitive identifies write bytes/offset while max lease wait stays independent");
    file.ResetAccessTiming();
    Check(file.GetAccessTiming().accesses == 0U &&
              file.GetAccessTiming().maximumOperation == File::AccessOperation::None &&
              file.GetAccessTiming().maximumMs == 0U &&
              file.GetAccessTiming().maximumLeaseWaitMs == 0U,
          "a new save slice cannot inherit stale transfer latency");
    cost = {2U, 8U};
    File unavailable("build/frame_timing_absent/NO/r0", "build/frame_timing_absent/NO/r1",
                     "build/frame_timing_absent/NO/b0", "build/frame_timing_absent/NO/b1");
    unavailable.SetAccessOps({&cost,
        [](void* context) { fxe::ManualMillis::AdvanceMs(static_cast<Cost*>(context)->wait); },
        [](void* context, firmware::LitchcraftFrameFile::AccessOperation, size_t, uint32_t) { fxe::ManualMillis::AdvanceMs(static_cast<Cost*>(context)->held); }});
    const auto failure = unavailable.StoreOps();
    Check(!failure.write(failure.context, offset, bytes.data(), bytes.size()) &&
              unavailable.GetAccessTiming().accesses == 1U &&
              unavailable.GetAccessTiming().maximumMs == 10U &&
              unavailable.GetAccessTiming().maximumOperation == File::AccessOperation::Write,
          "failed physical-open path still contributes its attempted primitive latency");
    file.SetAccessOps({});
    Check(file.Remove(), "latency fixture leaves no private files behind");
    std::printf("[FRAME_FILE_LATENCY] wrap_read=30 wait=7 write=6000 max_wait=11 calls=3 "
                "reset=clear failed_open=10 classification=ok\n");
}

void FrameFileSyncProbeIdentifiesRetainedTailAndPhase() {
    using File = firmware::LitchcraftFrameFile;
    using Phase = store::FrameStore::TimingPhase;
    namespace fixed = modules::litchcraft_frame_store_layout;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 20U);
    File file("frame_sync_probe.r0", "frame_sync_probe.r1",
              "frame_sync_probe.b0", "frame_sync_probe.b1");
    const auto ops = file.StoreOps();
    std::array<uint8_t, 13U> bytes{};
    Check(file.Remove() &&
              ops.write(ops.context,
                        store::FrameStore::BodyOffset(1U) +
                            File::BODY_FILE_BYTES - 1U,
                        bytes.data(), 1U) && ops.sync(ops.context),
          "Sync probe seeds a real bounded-file predecessor tail before prefix overwrite");
    struct Cost final { uint32_t wait = 1U; uint32_t held = 0U; } cost;
    Check(file.SetAccessOps({&cost,
        [](void* context) { fxe::ManualMillis::AdvanceMs(static_cast<Cost*>(context)->wait); },
        [](void* context, firmware::LitchcraftFrameFile::AccessOperation, size_t, uint32_t) { fxe::ManualMillis::AdvanceMs(static_cast<Cost*>(context)->held); }}),
          "Sync probe uses the production adapter and existing lease hooks");
    file.ResetAccessTiming();
    Check(ops.write(ops.context, store::FrameStore::BodyOffset(1U) + 23U,
                    bytes.data(), bytes.size()),
          "prefix write retains predecessor file size without changing durability behavior");
    cost = {28U, 4070U};
    Check(ops.sync(ops.context) && ops.sync(ops.context),
          "slow explicit Sync closes the real handle, repeated handle-free Sync is a no-op");
    auto sync = file.GetAccessTiming().sync;
    Check(sync.maximumMs == 4098U && sync.maximumPhase == 0U &&
              sync.maximumRegion == File::Region::Body1 &&
              sync.maximumFileRegion == File::Region::Body1 &&
              sync.maximumFileBytes == File::BODY_FILE_BYTES &&
              sync.maximumCursor == 36U &&
              sync.phases[0U].totalMs == 4098U &&
              sync.phases[0U].maximumMs == 4098U && sync.phases[0U].calls == 1U &&
              file.GetAccessTiming().maximumLeaseWaitMs == 28U,
          "4.098-second Sync identifies bounded retained tail separately from cursor and lease wait");
    cost = {1U, 0U};
    Check(ops.write(ops.context, store::FrameStore::BodyOffset(1U) + 40U,
                    bytes.data(), bytes.size()),
          "same Begin phase reopens the existing physical bank");
    cost = {5U, 25U};
    Check(ops.sync(ops.context), "second Begin barrier uses the same real Sync path");
    file.SetEditTimingPhase(Phase::BodyPrepare);
    file.SetEditTimingPhase(Phase::BodyWrite);
    cost = {1U, 0U};
    Check(ops.write(ops.context, fixed::FRAME_FILES_CAPACITY + 17U,
                    bytes.data(), bytes.size()),
          "summary logical region maps into the existing physical root tail");
    cost = {7U, 93U};
    Check(ops.sync(ops.context), "Prepare barrier records its own total and maximum");
    file.SetEditTimingPhase(Phase::RootPublish);
    cost = {1U, 0U};
    Check(ops.write(ops.context, store::FrameStore::RootOffset(1U) + 10U,
                    bytes.data(), bytes.size()),
          "Publish opens the other real root file");
    cost = {2U, 48U};
    Check(ops.sync(ops.context), "Publish barrier is classified separately from Begin and Prepare");
    sync = file.GetAccessTiming().sync;
    Check(sync.phases[0U].totalMs == 4128U && sync.phases[0U].maximumMs == 4098U &&
              sync.phases[0U].calls == 2U &&
              sync.phases[1U].totalMs == 100U && sync.phases[1U].maximumMs == 100U &&
              sync.phases[1U].calls == 1U &&
              sync.phases[2U].totalMs == 50U && sync.phases[2U].maximumMs == 50U &&
              sync.phases[2U].calls == 1U && sync.maximumPhase == 0U &&
              sync.maximumCursor == 36U && sync.maximumFileBytes == File::BODY_FILE_BYTES,
          "phase totals/maxima preserve the first largest barrier's exact pre-close context");
    file.ResetAccessTiming();
    sync = file.GetAccessTiming().sync;
    Check(sync.maximumPhase == UINT8_MAX && sync.maximumMs == 0U &&
              std::all_of(sync.phases.begin(), sync.phases.end(),
                          [](const File::SyncPhaseTiming& phase) {
                              return phase.totalMs == 0U && phase.maximumMs == 0U &&
                                  phase.calls == 0U;
                          }),
          "new save slice resets every Sync phase and cached maximum context");
    cost = {0U, 0U};
    Check(ops.write(ops.context, fixed::FRAME_FILES_CAPACITY + 17U,
                    bytes.data(), bytes.size()) && ops.sync(ops.context),
          "zero-time Sync still produces valid logical and physical file evidence");
    sync = file.GetAccessTiming().sync;
    Check(sync.maximumPhase == 0U && sync.maximumRegion == File::Region::Summary0 &&
              sync.maximumFileRegion == File::Region::Root0 &&
              sync.maximumFileBytes == fixed::ROOT0_FILE_BYTES + 30U &&
              sync.maximumCursor == fixed::ROOT0_FILE_BYTES + 30U &&
              sync.phases[0U].calls == 1U,
          "summary tail reports its distinct logical region and root file without extra filesystem reads");
    file.SetAccessOps({});
    Check(file.Remove(), "Sync probe removes its private real files");
    std::printf("[FRAME_FILE_SYNC_PROBE] max=4098 lease_wait=28 tail=%zu cursor=36 "
                "phases=4128/4098/2,100/100/1,50/50/1 reset=clear summary=4/0\n",
                File::BODY_FILE_BYTES);
}

void RepeatedOneFrameEditsBoundRenderAdmissionCost() {
    using File = firmware::LitchcraftFrameFile;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    File file("frame_edit_budget.r0", "frame_edit_budget.r1",
              "frame_edit_budget.b0", "frame_edit_budget.b1");
    Check(file.Remove(), "edit budget fixture starts with private empty files");
    auto manifest = Manifest(1U);
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    {
        store::FrameStore frames(file.StoreOps());
        Check(frames.Recover() == store::Status::NoArtwork &&
                  frames.Begin(manifest, 0U, 71U, token) == store::Status::Ok &&
                  frames.WriteFrame(token, 0U, Frame(11U)) == store::Status::Ok &&
                  frames.Commit(token) == store::Status::Ok &&
                  frames.BeginEdit(1U, 72U, token) == store::Status::Ok &&
                  frames.ReplaceFrame(token, 0U, Frame(12U)) == store::Status::Ok &&
                  frames.Commit(token) == store::Status::Ok,
              "first real edit durably leaves a staged one-frame root");
    }
    file.CloseCachedHandles();
    store::FrameStore frames(file.StoreOps());
    Check(frames.Recover(store::FrameStore::RecoveryPurpose::RootOnly) ==
              store::Status::Ok && frames.Generation() == 2U,
          "paused worker reconstruction retains the staged edit and defers summaries");
    // Model the physical capture's 30 ms maximum outer render admission at
    // every existing file boundary; no filesystem or durability call is faked.
    Check(file.SetAccessOps({nullptr,
        [](void*) { fxe::ManualMillis::AdvanceMs(30U); }, [](void*, firmware::LitchcraftFrameFile::AccessOperation, size_t, uint32_t) {}}),
          "controlled admission wait uses the production file access hook");
    file.ResetAccessTiming();
    const uint32_t startedMs = fxe::Millis();
    store::Frame saved{};
    Check(frames.ReadFrame(0U, saved) == store::Status::Ok &&
              Same(saved, Frame(12U)) &&
              frames.BeginEdit(2U, 73U, token) == store::Status::Ok &&
              frames.ReplaceFrame(token, 0U, Frame(13U)) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "second edit compares, folds and publishes through real Store and FrameFile");
    const uint32_t elapsedMs = fxe::Millis() - startedMs;
    const auto access = file.GetAccessTiming();
    uint32_t syncCalls = 0U;
    for (const auto& phase : access.sync.phases) syncCalls += phase.calls;
    Check(elapsedMs <= 2250U,
          "one-frame staged edit stays within 2250 authored ms of render admission");
    Check(access.maximumLeaseWaitMs == 30U && syncCalls == 7U,
          "bounded edit retains all seven real durability barriers");
    file.SetAccessOps({});
    file.CloseCachedHandles();
    store::FrameStore reopened(file.StoreOps());
    Check(reopened.Recover() == store::Status::Ok &&
              reopened.Generation() == 3U &&
              reopened.ReadFrame(0U, saved) == store::Status::Ok &&
              Same(saved, Frame(13U)),
          "bounded edit reopens with the exact fully durable replacement frame");
    Check(file.Remove(), "edit budget fixture removes its private files");
    std::printf("[FRAME_EDIT_ADMISSION_BUDGET] elapsed_ms=%lu operations=%lu "
                "sync_calls=%lu wait_ms=30 budget_ms=2250\n",
                static_cast<unsigned long>(elapsedMs),
                static_cast<unsigned long>(access.accesses),
                static_cast<unsigned long>(syncCalls));
}

void DurableSlotReadbackRejectsEveryCorruptByte() {
    namespace fixed = modules::litchcraft_frame_store_layout;
    MemoryFile file;
    file.readsSeeOnlySyncedBytes = true;
    std::array<uint8_t, fixed::FRAME_SLOT_BYTES> scratch{};
    const fixed::Layout storage(file.Ops(), scratch.data(), scratch.size());
    const auto expected = Frame(37U);
    const size_t offset = fixed::Layout::SlotOffset(1U, 0U);
    for (size_t byte = 0U; byte < fixed::FRAME_SLOT_BYTES; ++byte) {
        file.corruptReadAt = offset + byte;
        Check(storage.WriteSlot(1U, 0U, expected) == fixed::Status::IoFailure,
              "durable slot verification rejects corruption at every byte, including its partial tail");
    }
    file.corruptReadAt = std::numeric_limits<size_t>::max();
    fixed::Root root{};
    root.resident = 1U;
    root.manifest = Manifest(1U);
    root.slots[0U] = 0U;
    store::Frame actual{};
    Check(storage.WriteSlot(1U, 0U, expected) == fixed::Status::Ok &&
              storage.ReadFrame(root, 0U, actual) == fixed::Status::Ok &&
              Same(actual, expected),
          "complete slot verification recovers after the controlled readback fault clears");
    std::printf("[FRAME_SLOT_READBACK] corrupt_byte_positions=%zu clean_readback=exact\n",
                fixed::FRAME_SLOT_BYTES);
}

void ClearHidesBothResidentsAndInvalidatesReaders() {
    MemoryFile file;
    store::FrameStore artwork(file.Ops());
    store::TransactionToken token{};
    Check(artwork.Recover() == store::Status::NoArtwork &&
              artwork.Begin(Manifest(1U), 0U, 1U, token) == store::Status::Ok &&
              artwork.WriteFrame(token, 0U, Frame(31U)) == store::Status::Ok &&
              artwork.Commit(token) == store::Status::Ok &&
              artwork.Begin(Manifest(1U), 1U, 1U, token) == store::Status::Ok &&
              artwork.WriteFrame(token, 0U, Frame(42U)) == store::Status::Ok &&
              artwork.Commit(token) == store::Status::Ok,
          "local clear starts with two durable residents");
    store::FrameStore::ResidentDescriptor older{};
    Check(artwork.Resident(0U, older) && older.generation == 1U &&
              artwork.SelectResident(older.rootSlot, older.generation,
                                     older.bodyCrc32) == store::Status::Ok &&
              artwork.Generation() == 1U && artwork.ResidentCount() == 2U,
          "local clear must advance beyond the newer alternate resident");
    store::Frame shown{};
    store::FrameStore::ReadCursor cursor{};
    std::array<uint8_t, 17U> wirePage{};
    size_t wireRead = 0U;
    Check(artwork.ReadFrame(0U, shown) == store::Status::Ok &&
              Same(shown, Frame(31U)) &&
              artwork.ReadWireBody(1U, 0U, wirePage.data(), wirePage.size(),
                                   wireRead) == store::Status::Ok &&
              wireRead == wirePage.size() &&
              artwork.BeginReadFrame(0U, cursor) == store::Status::Ok,
          "local clear begins with warm frame and wire readers");
    const size_t bodyWrites = file.bodyBytesWritten;
    const size_t events = file.events;
    Check(artwork.Clear(0U) == store::Status::Ok &&
              artwork.Generation() == 3U && !artwork.HasArtwork() &&
              artwork.ResidentCount() == 0U &&
              !artwork.Resident(0U, older) && !artwork.Resident(1U, older) &&
              file.bodyBytesWritten == bodyWrites &&
              file.events == events + store::ROOT_BYTES + 1U,
          "local clear writes one durable root and hides both residents");
    Check(artwork.ReadFrame(0U, shown) == store::Status::NoArtwork &&
              artwork.ContinueReadFrame(cursor, store::STAGE_SLOT_BYTES) ==
                  store::Status::RevisionConflict && !cursor.active &&
              artwork.ReadWireBody(1U, 0U, wirePage.data(), wirePage.size(),
                                   wireRead) == store::Status::NoArtwork &&
              wireRead == 0U && Same(artwork.DecodedFrame(), store::Frame{}),
          "local clear invalidates old cursors and removes cached presentation");
    file.Crash();
    store::FrameStore reboot(file.Ops());
    Check(reboot.Recover() == store::Status::NoArtwork &&
              reboot.Generation() == 3U && reboot.ResidentCount() == 0U &&
              reboot.Begin(Manifest(1U), 3U, 2U, token) == store::Status::Ok &&
              reboot.WriteFrame(token, 0U, Frame(53U)) == store::Status::Ok &&
              reboot.Commit(token) == store::Status::Ok &&
              reboot.Generation() == 4U && reboot.ResidentCount() == 1U,
          "a later artwork advances past clear without reviving either resident");
    file.Crash();
    store::FrameStore newer(file.Ops());
    Check(newer.Recover() == store::Status::Ok && newer.Generation() == 4U &&
              newer.ResidentCount() == 1U &&
              newer.ReadFrame(0U, shown) == store::Status::Ok && Same(shown, Frame(53U)),
          "only artwork committed after local clear returns after reboot");
}

void ClearExplicitlyRecoversCorruptRootsAndHonorsGenerationFence() {
    MemoryFile file;
    file.bytes[0U] = 0xa5U;
    const size_t bodyMarker = store::FrameStore::BodyOffset(0U) + 19U;
    file.bytes[bodyMarker] = 0x5aU;
    file.durable = file.bytes;
    store::FrameStore artwork(file.Ops());
    Check(artwork.Recover() == store::Status::Corrupt &&
              store::FrameStore::Retire(file.Ops(), 40U) == store::Status::Corrupt,
          "automatic recovery and retirement preserve corrupt roots");
    const size_t bodyWrites = file.bodyBytesWritten;
    Check(artwork.Clear(40U) == store::Status::Ok &&
              artwork.Generation() == 41U && !artwork.HasArtwork() &&
              file.bodyBytesWritten == bodyWrites && file.bytes[bodyMarker] == 0x5aU,
          "explicit clear installs a canonical retired root above the live fence");
    file.Crash();
    store::FrameStore reboot(file.Ops());
    Check(reboot.Recover() == store::Status::NoArtwork && reboot.Generation() == 41U &&
              reboot.Clear(100U) == store::Status::Ok && reboot.Generation() == 101U &&
              reboot.Clear(5U) == store::Status::Ok && reboot.Generation() == 102U,
          "repeated local clear preserves and advances durable generation authority");
    MemoryFile blank;
    store::FrameStore wrapped(blank.Ops());
    Check(wrapped.Recover() == store::Status::NoArtwork &&
              wrapped.Clear(std::numeric_limits<uint32_t>::max()) == store::Status::Ok &&
              wrapped.Generation() == 1U,
          "clear uses serial-number wrap without publishing reserved generation zero");
    blank.Crash();
    store::FrameStore wrappedReboot(blank.Ops());
    Check(wrappedReboot.Recover() == store::Status::NoArtwork &&
              wrappedReboot.Generation() == 1U,
          "the wrapped retirement fence is durable");
}

void ClearFailureKeepsAuthorityAndCanRetry() {
    MemoryFile original;
    store::FrameStore seed(original.Ops());
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(Manifest(1U), 0U, 1U, token) == store::Status::Ok &&
              seed.WriteFrame(token, 0U, Frame(31U)) == store::Status::Ok &&
              seed.Commit(token) == store::Status::Ok,
          "clear failure starts with a durable artwork");
    const size_t initialEvents = original.events;
    original.failRead = true;
    Check(seed.Clear(9U) == store::Status::IoFailure && seed.HasArtwork() &&
              seed.Generation() == 1U && original.events == initialEvents,
          "clear cannot turn unreadable storage into a blind retirement");
    original.failRead = false;
    Check(seed.Clear(0x80000001U) == store::Status::RevisionConflict &&
              seed.Generation() == 1U && original.events == initialEvents,
          "clear rejects a fence that cannot be ordered above existing authority");
    for (const size_t cut : std::array<size_t, 4U>{
             0U, 12U, store::ROOT_BYTES - 1U, store::ROOT_BYTES}) {
        auto interrupted = original;
        store::FrameStore artwork(interrupted.Ops());
        store::Frame shown{};
        store::FrameStore::ReadCursor cursor{};
        Check(artwork.Recover() == store::Status::Ok &&
                  artwork.ReadFrame(0U, shown) == store::Status::Ok &&
                  artwork.BeginReadFrame(0U, cursor) == store::Status::Ok,
              "interrupted local clear starts with a warm reader");
        const size_t bodyWrites = interrupted.bodyBytesWritten;
        interrupted.faultAfterEvent = interrupted.events + cut;
        Check(artwork.Clear(9U) == store::Status::IoFailure && artwork.HasArtwork() &&
                  artwork.Generation() == 1U && artwork.ResidentCount() == 1U &&
                  interrupted.bodyBytesWritten == bodyWrites &&
                  artwork.ContinueReadFrame(cursor, store::STAGE_SLOT_BYTES) ==
                      store::Status::Ok && Same(artwork.DecodedFrame(), Frame(31U)),
              "failed local clear retains current authority and existing readers");
        interrupted.Crash();
        store::FrameStore reboot(interrupted.Ops());
        Check(reboot.Recover() == store::Status::Ok && reboot.Generation() == 1U &&
                  reboot.ReadFrame(0U, shown) == store::Status::Ok && Same(shown, Frame(31U)),
              "failed local clear retains committed artwork after power loss");
        interrupted.faultAfterEvent = std::numeric_limits<size_t>::max();
        Check(artwork.Clear(9U) == store::Status::Ok && !artwork.HasArtwork() &&
                  artwork.Generation() == 10U && interrupted.bodyBytesWritten == bodyWrites,
              "the same local-clear owner can retry after a failed publication");
        interrupted.Crash();
        store::FrameStore cleared(interrupted.Ops());
        Check(cleared.Recover() == store::Status::NoArtwork && cleared.Generation() == 10U,
              "successful retry durably retires the prior artwork");
    }
}

void ClearCannotStealActiveOrPreparedTransaction() {
    store::FrameStore unconfigured({});
    Check(unconfigured.Clear(0U) == store::Status::NotConfigured,
          "local clear requires configured storage");
    MemoryFile file;
    store::FrameStore artwork(file.Ops());
    store::TransactionToken token{};
    Check(artwork.Recover() == store::Status::NoArtwork &&
              artwork.Begin(Manifest(1U), 0U, 1U, token) == store::Status::Ok &&
              artwork.WriteFrame(token, 0U, Frame(31U)) == store::Status::Ok &&
              artwork.Commit(token) == store::Status::Ok &&
              artwork.Begin(Manifest(1U), 1U, 2U, token) == store::Status::Ok,
          "transaction safety starts with an incoming replacement");
    const size_t events = file.events;
    Check(artwork.Clear(100U) == store::Status::Incomplete &&
              artwork.Generation() == 1U && file.events == events &&
              artwork.WriteFrame(token, 0U, Frame(42U)) == store::Status::Ok &&
              artwork.PrepareCommit(token) == store::Status::Ok,
          "local clear rejects active ownership without touching flash or its token");
    const size_t preparedEvents = file.events;
    Check(artwork.Clear(100U) == store::Status::Incomplete &&
              artwork.Generation() == 1U && file.events == preparedEvents &&
              artwork.PublishPreparedCommit(token) == store::Status::Ok &&
              artwork.Generation() == 2U && artwork.Clear(100U) == store::Status::Ok &&
              artwork.Generation() == 101U && !artwork.HasArtwork(),
          "local clear waits for prepared publication then advances beyond its authority");
}

void RetirementIsDurableAtomicAndGenerationOrdered() {
    MemoryFile original;
    Check(store::FrameStore::Retire(original.Ops(), 1U) == store::Status::Ok,
          "retiring a provisional stream with no durable root needs no tombstone");
    store::FrameStore seed(original.Ops());
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(Manifest(1U), 0U, 1U, token) == store::Status::Ok &&
              seed.WriteFrame(token, 0U, Frame(31U)) == store::Status::Ok &&
              seed.Commit(token) == store::Status::Ok,
          "retirement starts from a durable schema-4 artwork");
    for (const size_t cut : std::array<size_t, 4U>{
             0U, 12U, store::ROOT_BYTES - 1U, store::ROOT_BYTES}) {
        auto interrupted = original;
        interrupted.faultAfterEvent = interrupted.events + cut;
        Check(store::FrameStore::Retire(interrupted.Ops(), 1U) == store::Status::IoFailure,
              "interrupted root retirement reports failure");
        interrupted.Crash();
        store::FrameStore reboot(interrupted.Ops());
        store::Frame shown{};
        Check(reboot.Recover() == store::Status::Ok && reboot.Generation() == 1U &&
                  reboot.ReadFrame(0U, shown) == store::Status::Ok && Same(shown, Frame(31U)),
              "failed retirement retains last committed stream after reboot");
    }
    const auto bodyWrites = original.bodyBytesWritten;
    Check(store::FrameStore::Retire(original.Ops(), 1U) == store::Status::Ok &&
              original.bodyBytesWritten == bodyWrites,
          "retirement promotes only a CRC root without an artwork copy");
    original.Crash();
    store::FrameStore reboot(original.Ops());
    Check(reboot.Recover() == store::Status::NoArtwork && !reboot.HasArtwork() &&
              reboot.Generation() == 2U,
          "successful retirement prevents reboot from resurrecting older stream");
    Check(store::FrameStore::Retire(original.Ops(), 1U) == store::Status::Ok &&
              reboot.Begin(Manifest(1U), 2U, 2U, token) == store::Status::Ok &&
              reboot.WriteFrame(token, 0U, Frame(42U)) == store::Status::Ok &&
              reboot.Commit(token) == store::Status::Ok && reboot.Generation() == 3U &&
              store::FrameStore::Retire(original.Ops(), 1U) == store::Status::Ok,
          "retirement is idempotent and a later upload retains newer authority");
    store::FrameStore newer(original.Ops());
    Check(newer.Recover() == store::Status::Ok && newer.Generation() == 3U,
          "late retirement retry cannot erase a newer stream");
}

void OwnedRecoveryBlankKeepsCanonicalIdentityAndTransactionGuards() {
    MemoryFile referenceFile;
    MemoryFile ownedFile;
    store::FrameStore reference(referenceFile.Ops());
    store::FrameStore owned(ownedFile.Ops());
    store::TransactionToken referenceToken{};
    store::TransactionToken ownedToken{};
    for (auto* frames : {&reference, &owned}) {
        store::TransactionToken token{};
        Check(frames->Recover() == store::Status::NoArtwork &&
                  frames->Begin(Manifest(1U), 0U, 17U, token) == store::Status::Ok &&
                  frames->WriteFrame(token, 0U, Frame(82U)) == store::Status::Ok &&
                  frames->Commit(token) == store::Status::Ok,
              "owned blank starts with dirty previous-frame scratch");
    }
    Check(reference.Begin(Manifest(1U), 1U, 18U, referenceToken) ==
              store::Status::Ok &&
              owned.Begin(Manifest(1U), 1U, 18U, ownedToken) == store::Status::Ok,
          "both blank writers start the same generated replacement");
    const auto beforeInvalid = ownedFile.bytes;
    auto stale = ownedToken;
    ++stale.nonce;
    Check(owned.WriteBlankFirstFrame(stale) == store::Status::RevisionConflict &&
              ownedFile.bytes == beforeInvalid,
          "blank scratch cannot be reset by a different transaction owner");
    store::Frame blank{};
    Check(reference.WriteFrame(referenceToken, 0U, blank) == store::Status::Ok &&
              owned.WriteBlankFirstFrame(ownedToken) == store::Status::Ok &&
              owned.WriteBlankFirstFrame(ownedToken) ==
                  store::Status::InvalidArgument &&
              reference.Commit(referenceToken) == store::Status::Ok &&
              owned.Commit(ownedToken) == store::Status::Ok &&
              referenceFile.bytes == ownedFile.bytes &&
              referenceFile.durable == ownedFile.durable,
          "owned blank has byte-identical canonical body and durable roots");
    store::Frame shown{};
    Check(owned.ReadFrame(0U, shown) == store::Status::Ok && Same(shown, blank),
          "owned recovery blank resets all colors, effects and duration");
    Check(owned.Begin(Manifest(2U), 2U, 19U, ownedToken) == store::Status::Ok &&
              owned.WriteBlankFirstFrame(ownedToken) ==
                  store::Status::InvalidArgument &&
              owned.Abort(ownedToken) == store::Status::Ok,
          "blank helper refuses a multi-frame transaction without stealing it");
    Check(owned.BeginWire(Manifest(1U), 2U, 20U, 6U, 1U, ownedToken) ==
              store::Status::Ok &&
              owned.WriteBlankFirstFrame(ownedToken) ==
                  store::Status::InvalidArgument &&
              owned.Abort(ownedToken) == store::Status::Ok,
          "blank helper cannot alter a verified wire transfer");
    Check(owned.Begin(Manifest(1U), 2U, 21U, ownedToken) == store::Status::Ok,
          "blank IO failure starts a fresh owner");
    ownedFile.failSync = true;
    Check(owned.WriteBlankFirstFrame(ownedToken) == store::Status::IoFailure &&
              owned.Abort(ownedToken) == store::Status::Ok &&
              owned.Generation() == 2U,
          "blank IO failure retains current authority and can abort");
    ownedFile.failSync = false;
    Check(owned.Begin(Manifest(1U), 2U, 22U, ownedToken) == store::Status::Ok &&
              owned.WriteBlankFirstFrame(ownedToken) == store::Status::Ok &&
              owned.Commit(ownedToken) == store::Status::Ok &&
              owned.ReadFrame(0U, shown) == store::Status::Ok && Same(shown, blank),
          "blank creation retries after aborted IO failure with unchanged output");
}

void IncomingWireBatchesDurabilityAndVerifiesPersistedIdentity() {
    for (const bool rgb : {false, true}) {
        std::vector<store::Frame> source;
        for (uint16_t index = 0U; index < 65U; ++index) {
            source.push_back(Frame(index + 20U));
            for (auto& emitter : source.back().emitters) {
                emitter.effect = 0U;
                emitter.effectDurationMs = 0U;
            }
        }
        const auto body = Body(source, true, true, rgb
            ? wire::RgbOnlyMode::Enabled : wire::RgbOnlyMode::Disabled);
        const auto canonical = Body(source, true, true);
        auto manifest = Manifest(source.size());
        manifest.predecessorDeltas = true;
        MemoryFile file;
        store::FrameStore artwork(file.Ops());
        store::TransactionToken token{};
        Check(artwork.Recover() == store::Status::NoArtwork &&
                  artwork.Begin(Manifest(1U), 0U, 1U, token) == store::Status::Ok &&
                  artwork.WriteFrame(token, 0U, Frame(9U)) == store::Status::Ok &&
                  artwork.Commit(token) == store::Status::Ok,
              "batched incoming starts with a durable old artwork");
        file.readsSeeOnlySyncedBytes = true;
        Check(artwork.BeginWire(manifest, 1U, 2U, body.size(),
                  fxe::Crc32(body.data(), body.size()), token, rgb
                      ? store::WireBodyMode::Rgb : store::WireBodyMode::Legacy) ==
                  store::Status::Ok,
              "incoming wire reserves its unpublished bank");
        // Retiring any old incoming roots is a separate durable prerequisite.
        // Start the wire header first so the measured baseline excludes it.
        Check(artwork.WriteBodyChunk(token, 0U, body.data(), 1U) ==
                  store::Status::Ok,
              "incoming root invalidation is durable before staging begins");
        const size_t syncs = file.syncs;
        const size_t split = body.size() / 2U + 1U;
        Check(artwork.WriteBodyChunk(token, 1U, body.data() + 1U, split - 1U) ==
                  store::Status::Ok && file.syncs == syncs,
              "incoming records stage without one filesystem sync per frame");
        file.failSync = true;
        Check(artwork.Checkpoint(token, split) == store::Status::IoFailure &&
                  artwork.Generation() == 1U,
              "failed partial-record checkpoint never changes authority");
        file.failSync = false;
        Check(artwork.Checkpoint(token, split) == store::Status::Ok &&
                  file.syncs == syncs + 1U &&
                  artwork.WriteBodyChunk(token, split, body.data() + split,
                      body.size() - split) == store::Status::Ok &&
                  file.syncs == syncs + 1U,
              "durable cursor barrier preserves a partially assembled wire record");
        file.failSync = true;
        Check(artwork.PrepareCommit(token) == store::Status::IoFailure &&
                  artwork.PublishPreparedCommit(token) == store::Status::Incomplete &&
                  artwork.Generation() == 1U,
              "failed final sync cannot reserve or publish incoming artwork");
        file.failSync = false;
        Check(artwork.PrepareCommit(token) == store::Status::Ok &&
                  file.syncs == syncs + 2U && artwork.Generation() == 1U &&
                  artwork.PublishPreparedCommit(token) == store::Status::Ok,
              "final barrier verifies synced slots before root publication");
        std::vector<uint8_t> reverse(canonical.size());
        size_t count = 0U;
        Check(artwork.ReadWireBody(artwork.Generation(), 0U, reverse.data(),
                  reverse.size(), count) == store::Status::Ok &&
                  count == canonical.size() && reverse == canonical,
              "legacy and RGB ingress reverse to the exact canonical artwork");
        file.Crash();
        store::FrameStore reboot(file.Ops());
        store::Frame shown{};
        Check(reboot.Recover() == store::Status::Ok &&
                  reboot.ReadFrame(64U, shown) == store::Status::Ok &&
                  Same(shown, source.back()),
              "synced incoming root and its last frame survive power loss");
    }
}

void IncomingWireReadbackFailureCannotPublishOrPoisonOldArtwork() {
    namespace fixed = modules::litchcraft_frame_store_layout;
    for (const bool validWrongFrame : {false, true}) {
        MemoryFile file;
        store::FrameStore artwork(file.Ops());
        store::TransactionToken token{};
        Check(artwork.Recover() == store::Status::NoArtwork &&
                  artwork.Begin(Manifest(1U), 0U, 1U, token) == store::Status::Ok &&
                  artwork.WriteFrame(token, 0U, Frame(9U)) == store::Status::Ok &&
                  artwork.Commit(token) == store::Status::Ok,
              "incoming readback rejection starts with committed authority");
        const auto body = Body({Frame(42U), Frame(43U)});
        Check(artwork.BeginWire(Manifest(2U), 1U, 2U, body.size(),
                  fxe::Crc32(body.data(), body.size()), token) == store::Status::Ok &&
                  artwork.WriteBodyChunk(token, 0U, body.data(), body.size()) ==
                      store::Status::Ok,
              "verified wire records stage before persisted readback");
        file.failRead = true;
        Check(artwork.PrepareCommit(token) == store::Status::IoFailure &&
                  artwork.PublishPreparedCommit(token) == store::Status::Incomplete,
              "failed persisted readback cannot admit a synced incoming bank");
        file.failRead = false;
        const size_t target = fixed::Layout::SlotOffset(1U, 1U);
        if (validWrongFrame) {
            std::array<uint8_t, store::STAGE_SLOT_BYTES> scratch{};
            fixed::Layout layout(file.Ops(), scratch.data(), scratch.size());
            Check(layout.WriteSlot(1U, 1U, Frame(99U)) == fixed::Status::Ok,
                  "controlled storage corruption has a valid different slot CRC");
        } else {
            file.bytes[target + 10U] ^= 1U;
        }
        Check(artwork.PrepareCommit(token) == store::Status::Corrupt &&
                  artwork.PublishPreparedCommit(token) == store::Status::Incomplete &&
                  artwork.Generation() == 1U &&
                  artwork.Abort(token) == store::Status::Ok,
              "persisted corruption or valid-but-wrong identity cannot be admitted");
        file.Crash();
        store::FrameStore reboot(file.Ops());
        store::Frame shown{};
        Check(reboot.Recover() == store::Status::Ok && reboot.Generation() == 1U &&
                  reboot.ReadFrame(0U, shown) == store::Status::Ok && Same(shown, Frame(9U)),
              "rejected incoming bank leaves old artwork durable after power loss");
    }
}


void IncomingWirePowerLossNeverRecoversAnUnpublishedBank() {
    MemoryFile seedFile;
    store::FrameStore seed(seedFile.Ops());
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(Manifest(1U), 0U, 1U, token) == store::Status::Ok &&
              seed.WriteFrame(token, 0U, Frame(9U)) == store::Status::Ok &&
              seed.Commit(token) == store::Status::Ok,
          "incoming power-cut fixture starts with durable authority");
    const auto body = Body({Frame(42U), Frame(43U), Frame(44U)});
    for (size_t phase = 0U; phase < 4U; ++phase) {
        auto file = seedFile;
        store::FrameStore artwork(file.Ops());
        Check(artwork.Recover() == store::Status::Ok &&
                  artwork.BeginWire(Manifest(3U), 1U, 2U, body.size(),
                      fxe::Crc32(body.data(), body.size()), token) == store::Status::Ok &&
                  artwork.WriteBodyChunk(token, 0U, body.data(), body.size()) ==
                      store::Status::Ok,
              "incoming power-cut writes complete unpublished slots");
        if (phase >= 1U) {
            Check(artwork.Checkpoint(token, body.size()) == store::Status::Ok,
                  "incoming power-cut passes its durable body cursor");
        }
        if (phase >= 2U) {
            Check(artwork.PrepareCommit(token) == store::Status::Ok,
                  "incoming power-cut verifies its synced body before publication");
        }
        if (phase == 3U) {
            file.faultAfterEvent = file.events + store::ROOT_BYTES;
            Check(artwork.PublishPreparedCommit(token) == store::Status::IoFailure,
                  "root sync failure cannot publish the prepared incoming bank");
        }
        file.Crash();
        store::FrameStore reboot(file.Ops());
        store::Frame shown{};
        Check(reboot.Recover() == store::Status::Ok && reboot.Generation() == 1U &&
                  reboot.ReadFrame(0U, shown) == store::Status::Ok && Same(shown, Frame(9U)),
              "cuts during staging, checkpoint, prepare and root sync recover only old artwork");
    }
}

// These journeys exercise the public transaction/storage seam. Counters cover
// every full slot load from BeginEdit through durable publication; reference
// encoding and explicit readback happen after those counts are checked.
void CachedStructuralChecksumsStayBoundedAtEveryPosition() {
    for (const bool dense : {false, true}) {
        std::vector<store::Frame> source;
        for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
            auto frame = dense || source.empty() ? Frame(index + 1U) : source.back();
            frame.durationMs = static_cast<uint16_t>(50U + index);
            if (!dense) {
                auto& emitter = frame.emitters[index];
                emitter.color.b ^= 0x5aU;
                emitter.effect = index % 9U;
                emitter.effectDurationMs = emitter.effect == 0U ? 0U : 1500U;
            }
            source.push_back(frame);
        }
        MemoryFile seeded;
        store::FrameStore original(seeded.Ops());
        auto manifest = Manifest(store::FRAME_COUNT);
        manifest.predecessorDeltas = true;
        store::TransactionToken token{};
        Check(original.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
              "bounded structural fixture begins");
        for (uint16_t index = 0U; index < source.size(); ++index) {
            Check(original.WriteFrame(token, index, source[index]) == store::Status::Ok,
                  "bounded structural fixture writes complete frames");
        }
        Check(original.Commit(token) == store::Status::Ok,
              "bounded structural fixture publishes validated cache");
        for (uint16_t index = 0U; index < source.size(); ++index) {
            auto file = seeded;
            store::FrameStore frames(file.Ops());
            Check(frames.Recover() == store::Status::Ok,
                  "cold boot loads derived metadata without a frame scan");
            const size_t before = file.frameLoads;
            const size_t writes = file.bodyBytesWritten;
            Check(frames.BeginEdit(1U, 2U, token) == store::Status::Ok &&
                      frames.DeleteFrame(token, index) == store::Status::Ok &&
                      frames.Commit(token) == store::Status::Ok,
                  "delete at every keyframe and middle position commits");
            Check(file.frameLoads - before <= 2U &&
                      (index != 255U || file.frameLoads == before) &&
                      file.bodyBytesWritten == writes,
                  "delete reads only the new adjacent edge; tail reads no frames");
            auto expected = source;
            expected.erase(expected.begin() + index);
            auto body = Body(expected, true, true);
            Check(frames.CurrentBodyBytes() == body.size() &&
                      frames.CurrentBodyCrc32() == fxe::Crc32(body.data(), body.size()),
                  "shifted keyframe anchors match complete canonical encoding");
            const size_t insertBefore = file.frameLoads;
            Check(frames.BeginEdit(2U, 3U, token) == store::Status::Ok &&
                      frames.InsertFrame(token, index, source[index]) == store::Status::Ok &&
                      frames.Commit(token) == store::Status::Ok &&
                      file.frameLoads - insertBefore <= 2U,
                  "insert restores every middle position with adjacent reads only");
            body = Body(source, true, true);
            Check(frames.CurrentBodyBytes() == body.size() &&
                      frames.CurrentBodyCrc32() == fxe::Crc32(body.data(), body.size()),
                  "insertion checksum restores exact full source identity");
            file.Crash();
            store::FrameStore reboot(file.Ops());
            Check(reboot.Recover() == store::Status::Ok && reboot.Generation() == 3U,
                  "structural cache/root survives a real recovery");
            const size_t replaceBefore = file.frameLoads;
            auto changed = source[index];
            changed.durationMs = 2000U;
            changed.emitters[index].color.r ^= 0x7aU;
            Check(reboot.BeginEdit(3U, 4U, token) == store::Status::Ok &&
                      reboot.ReplaceFrame(token, index, changed) == store::Status::Ok &&
                      reboot.Commit(token) == store::Status::Ok &&
                      file.frameLoads - replaceBefore <= 3U,
                  "staged insertion fold and next paint keep bounded frame reads");
            expected = source;
            expected[index] = changed;
            body = Body(expected, true, true);
            Check(reboot.CurrentBodyBytes() == body.size() &&
                      reboot.CurrentBodyCrc32() == fxe::Crc32(body.data(), body.size()),
                  "changed duration/color and successor delta match full encoding");
        }
    }
}

void CacheLossAndPublicationCutsPreserveAuthority() {
    namespace fixed = modules::litchcraft_frame_store_layout;
    MemoryFile baseline;
    store::FrameStore seeded(baseline.Ops());
    auto manifest = Manifest(4U);
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    Check(seeded.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "cache-cut fixture begins");
    for (uint16_t index = 0U; index < 4U; ++index) {
        Check(seeded.WriteFrame(token, index, Frame(index + 1U)) == store::Status::Ok,
              "cache-cut fixture writes source");
    }
    Check(seeded.Commit(token) == store::Status::Ok, "cache-cut fixture commits");
    for (const size_t cut : {0U, 31U, 32U, 800U, 3107U, 3108U, 3109U, 3300U, 3493U}) {
        auto file = baseline;
        store::FrameStore frames(file.Ops());
        Check(frames.Recover() == store::Status::Ok &&
                  frames.BeginEdit(1U, 2U, token) == store::Status::Ok &&
                  frames.DeleteFrame(token, 2U) == store::Status::Ok &&
                  frames.PrepareCommit(token) == store::Status::Ok,
              "cut happens after preparation and before cache/root publication");
        file.faultAfterEvent = file.events + cut;
        Check(frames.PublishPreparedCommit(token) == store::Status::IoFailure,
              "cut rejects incomplete summary or root publication");
        file.Crash();
        store::FrameStore reboot(file.Ops());
        store::Frame shown{};
        Check(reboot.Recover() == store::Status::Ok && reboot.Generation() == 1U &&
                  reboot.CurrentManifest().frameCount == 4U &&
                  reboot.ReadFrame(2U, shown) == store::Status::Ok && Same(shown, Frame(3U)),
              "cache never advances artwork authority ahead of the durable root");
    }
    for (const bool stale : {false, true}) {
        auto file = baseline;
        const size_t offset = fixed::Layout::SummaryOffset(0U, 0U);
        file.bytes[offset + (stale ? 16U : 100U)] ^= 1U;
        file.durable = file.bytes;
        store::FrameStore frames(file.Ops());
        file.frameLoads = 0U;
        Check(frames.Recover() == store::Status::Ok && frames.Generation() == 1U &&
                  file.frameLoads == 4U,
              "worker prepares missing cache once without rejecting valid artwork");
        file.frameLoads = 0U;
        Check(frames.BeginEdit(1U, 2U, token) == store::Status::Ok &&
                  file.frameLoads == 0U &&
                  frames.DeleteFrame(token, 3U) == store::Status::Ok &&
                  frames.Commit(token) == store::Status::Ok,
              "missing derived evidence is rebuilt once from validated fixed slots");
        file.frameLoads = 0U;
        Check(frames.BeginEdit(2U, 3U, token) == store::Status::Ok &&
                  frames.DeleteFrame(token, 2U) == store::Status::Ok &&
                  frames.Commit(token) == store::Status::Ok && file.frameLoads == 0U,
              "the next tail deletion does not repeat the recovery scan");
    }
}

void RootOnlyRecoveryDefersCacheUntilAnEditNeedsIt() {
    namespace fixed = modules::litchcraft_frame_store_layout;
    for (const bool missing : {false, true}) {
        MemoryFile file;
        store::FrameStore seeded(file.Ops());
        auto manifest = Manifest(4U);
        manifest.predecessorDeltas = true;
        store::TransactionToken token{};
        Check(seeded.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
              "root-only fixture starts a real committed document");
        for (uint16_t index = 0U; index < 4U; ++index) {
            Check(seeded.WriteFrame(token, index, Frame(index + 1U)) == store::Status::Ok,
                  "root-only fixture writes exact frames");
        }
        Check(seeded.Commit(token) == store::Status::Ok, "root-only fixture commits");
        if (missing) file.bytes[fixed::Layout::SummaryOffset(0U, 0U) + 100U] ^= 1U;
        file.durable = file.bytes;
        store::FrameStore recreated(file.Ops());
        file.frameLoads = 0U;
        const size_t events = file.events;
        const size_t reads = file.readCalls;
        file.maximumRead = 0U;
        Check(recreated.Recover(store::FrameStore::RecoveryPurpose::RootOnly) ==
                  store::Status::Ok && recreated.Generation() == 1U &&
                  file.frameLoads == 0U && file.events == events &&
                  file.readCalls == reads + 2U &&
                  file.maximumRead == 2U * fixed::ROOT_BYTES,
              "playback reconstruction reads each adjacent root pair once without payloads or cache writes");
        Check(recreated.EnsureWireSummaries(false) ==
                  (missing ? store::Status::Incomplete : store::Status::Ok) &&
                  file.frameLoads == 0U,
              "startup loads bound summaries without scanning or repairing missing artwork evidence");
        unsigned maintenanceFrames = 0U;
        const auto interrupted = recreated.EnsureWireSummaries(true, [](void* p) {
            return ++*static_cast<unsigned*>(p) < 2U;
        }, &maintenanceFrames);
        Check(interrupted == (missing ? store::Status::Incomplete : store::Status::Ok) &&
                  file.frameLoads < 4U,
              "foreground demand during optional repair stops before a full scan or cache publication");
        file.frameLoads = 0U;
        Check(recreated.BeginEdit(1U, 2U, token) == store::Status::Ok &&
                  file.frameLoads == (missing ? 4U : 0U),
              "edit loads bound summaries or rebuilds missing evidence exactly once");
        file.frameLoads = 0U;
        Check(recreated.DeleteFrame(token, 3U) == store::Status::Ok &&
                  recreated.Commit(token) == store::Status::Ok && file.frameLoads == 0U,
              "reconstructed worker tail edit has no artwork payload reads");
        store::FrameStore reboot(file.Ops());
        store::Frame actual{};
        Check(reboot.Recover() == store::Status::Ok &&
                  reboot.CurrentManifest().frameCount == 3U &&
                  reboot.ReadFrame(2U, actual) == store::Status::Ok && Same(actual, Frame(3U)),
              "root-only recovery and missing-cache repair preserve durable exact content");
    }
}

void AliasesAndMultipleStructuralEditsKeepCanonicalAdjacency() {
    std::vector<store::Frame> source;
    for (uint16_t index = 0U; index < 255U; ++index) source.push_back(Frame(index + 1U));
    MemoryFile baseline;
    store::FrameStore seeded(baseline.Ops());
    auto manifest = Manifest(255U);
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    Check(seeded.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "alias fixture begins");
    for (uint16_t index = 0U; index < source.size(); ++index) {
        Check(seeded.WriteFrame(token, index, source[index]) == store::Status::Ok,
              "alias fixture writes unique frames");
    }
    Check(seeded.Commit(token) == store::Status::Ok, "alias fixture commits");
    for (uint16_t index = 0U; index < source.size(); ++index) {
        auto file = baseline;
        store::FrameStore frames(file.Ops());
        Check(frames.Recover() == store::Status::Ok, "alias fixture recovers");
        const size_t before = file.frameLoads;
        const size_t writes = file.bodyBytesWritten;
        Check(frames.BeginEdit(1U, 2U, token) == store::Status::Ok &&
                  frames.CopyFrameAfter(token, index) == store::Status::Ok &&
                  frames.Commit(token) == store::Status::Ok &&
                  file.frameLoads - before <= 3U && file.bodyBytesWritten == writes,
              "copy at every position reads only the two adjacent record relationships");
        auto expected = source;
        expected.insert(expected.begin() + index + 1U, source[index]);
        auto body = Body(expected, true, true);
        Check(frames.CurrentBodyBytes() == body.size() &&
                  frames.CurrentBodyCrc32() == fxe::Crc32(body.data(), body.size()),
              "physical aliases have correct logical predecessor checksums");
    }
    store::FrameStore frames(baseline.Ops());
    Check(frames.Recover() == store::Status::Ok &&
              frames.BeginEdit(1U, 2U, token) == store::Status::Ok &&
              frames.DeleteFrame(token, 10U) == store::Status::Ok &&
              frames.DeleteFrame(token, 100U) == store::Status::Ok &&
              frames.CopyFrameAfter(token, 31U) == store::Status::Ok &&
              frames.CopyFrameAfter(token, 63U) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "one transaction keeps all dirty edges through multiple shifts");
    auto expected = source;
    expected.erase(expected.begin() + 10U);
    expected.erase(expected.begin() + 100U);
    expected.insert(expected.begin() + 32U, expected[31U]);
    expected.insert(expected.begin() + 64U, expected[63U]);
    auto body = Body(expected, true, true);
    Check(frames.CurrentBodyBytes() == body.size() &&
              frames.CurrentBodyCrc32() == fxe::Crc32(body.data(), body.size()),
          "multiple structural dirty edges match a full independent re-encode");
    const auto replacement = Frame(999U);
    Check(frames.BeginEdit(2U, 3U, token) == store::Status::Ok &&
              frames.ReplaceFrame(token, 32U, replacement) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "editing an aliased copy creates independent staged content");
    expected[32U] = replacement;
    body = Body(expected, true, true);
    store::Frame shown{};
    Check(frames.CurrentBodyBytes() == body.size() &&
              frames.CurrentBodyCrc32() == fxe::Crc32(body.data(), body.size()) &&
              frames.ReadFrame(31U, shown) == store::Status::Ok && Same(shown, expected[31U]),
          "alias edit changes its own delta and successor without painting the source");
    const size_t before = baseline.frameLoads;
    Check(frames.UpdateModeActive(false, 3U) == store::Status::Ok &&
              baseline.frameLoads == before,
          "mode-only roots rebind cached evidence without reading artwork");
    baseline.Crash();
    store::FrameStore reboot(baseline.Ops());
    Check(reboot.Recover() == store::Status::Ok &&
              reboot.BeginEdit(reboot.Generation(), 4U, token) == store::Status::Ok &&
              reboot.DeleteFrame(token, 254U) == store::Status::Ok &&
              reboot.Commit(token) == store::Status::Ok &&
              baseline.frameLoads - before <= 1U,
          "mode rebind and cold stage fold retain a bounded next tail deletion");
}

void OptionalCacheReadFailureCannotBlockModeOrFold() {
    MemoryFile file;
    store::FrameStore frames(file.Ops());
    auto manifest = Manifest(3U);
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    Check(frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "optional-cache fault fixture begins");
    for (uint16_t index = 0U; index < 3U; ++index) {
        Check(frames.WriteFrame(token, index, Frame(index + 1U)) == store::Status::Ok,
              "optional-cache fixture writes source");
    }
    Check(frames.Commit(token) == store::Status::Ok &&
              frames.BeginEdit(1U, 2U, token) == store::Status::Ok &&
              frames.ReplaceFrame(token, 1U, Frame(9U)) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "optional-cache fault starts with a durable staged edit");
    file.failSummaryRead = true;
    Check(frames.UpdateModeActive(false, 2U) == store::Status::Ok && frames.Generation() == 3U,
          "a derived-tail read fault cannot block explicit mode Exit");
    Check(frames.BeginEdit(3U, 3U, token) == store::Status::Ok &&
              frames.DeleteFrame(token, 2U) == store::Status::Ok &&
              frames.PrepareCommit(token) == store::Status::Ok,
          "the same optional fault cannot block folding valid staged artwork");
    Check(frames.PublishPreparedCommit(token) == store::Status::IoFailure,
          "mandatory new cache readback failure cannot falsely complete an edit");
    file.failSummaryRead = false;
    Check(frames.PublishPreparedCommit(token) == store::Status::Ok,
          "the prepared edit retries unchanged after cache channel recovery");
    file.Crash();
    store::FrameStore reboot(file.Ops());
    store::Frame shown{};
    Check(reboot.Recover() == store::Status::Ok && reboot.Generation() == 4U &&
              reboot.CurrentManifest().frameCount == 2U &&
              reboot.ReadFrame(1U, shown) == store::Status::Ok && Same(shown, Frame(9U)),
          "mode, fold and retry preserve exact durable content");
}

void InterruptedCachedFoldKeepsThePreviouslyCommittedEdit() {
    MemoryFile baseline;
    store::FrameStore frames(baseline.Ops());
    auto manifest = Manifest(3U);
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    Check(frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "cached-fold fixture begins");
    for (uint16_t index = 0U; index < 3U; ++index) {
        Check(frames.WriteFrame(token, index, Frame(index + 1U)) == store::Status::Ok,
              "cached-fold fixture writes source");
    }
    Check(frames.Commit(token) == store::Status::Ok &&
              frames.BeginEdit(1U, 2U, token) == store::Status::Ok &&
              frames.ReplaceFrame(token, 1U, Frame(9U)) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "cached-fold fixture commits a staged frame and bound cache");
    for (const size_t cut : {0U, 500U, 1097U, 1098U, 1130U, 4206U, 4400U, 4591U, 4975U}) {
        auto file = baseline;
        store::FrameStore candidate(file.Ops());
        Check(candidate.Recover() == store::Status::Ok, "cached-fold recovers before cut");
        file.faultAfterEvent = file.events + cut;
        Check(candidate.BeginEdit(2U, 3U, token) == store::Status::IoFailure,
              "cut during slot/cache/root/stage retirement rejects new edit admission");
        file.Crash();
        file.faultAfterEvent = std::numeric_limits<size_t>::max();
        store::FrameStore reboot(file.Ops());
        store::Frame shown{};
        Check(reboot.Recover() == store::Status::Ok && reboot.Generation() == 2U &&
                  reboot.ReadFrame(1U, shown) == store::Status::Ok && Same(shown, Frame(9U)),
              "every fold cut retains the already committed staged frame");
        Check(reboot.BeginEdit(2U, 4U, token) == store::Status::Ok &&
                  reboot.DeleteFrame(token, 2U) == store::Status::Ok &&
                  reboot.Commit(token) == store::Status::Ok,
              "recovered staged or folded authority can complete the next edit");
    }
}

} // namespace

int main() {
    ReplacementPreparationFollowsDurableRetirementAndRetriesSafely();
    FrameFileClassifiesPrimitiveLatencyAndLeaseWait();
    FrameFileSyncProbeIdentifiesRetainedTailAndPhase();
    RepeatedOneFrameEditsBoundRenderAdmissionCost();
    DurableSlotReadbackRejectsEveryCorruptByte();
    CachedStructuralChecksumsStayBoundedAtEveryPosition();
    CacheLossAndPublicationCutsPreserveAuthority();
    RootOnlyRecoveryDefersCacheUntilAnEditNeedsIt();
    AliasesAndMultipleStructuralEditsKeepCanonicalAdjacency();
    OptionalCacheReadFailureCannotBlockModeOrFold();
    InterruptedCachedFoldKeepsThePreviouslyCommittedEdit();
    IncomingWirePowerLossNeverRecoversAnUnpublishedBank();
    IncomingWireBatchesDurabilityAndVerifiesPersistedIdentity();
    IncomingWireReadbackFailureCannotPublishOrPoisonOldArtwork();
    OwnedRecoveryBlankKeepsCanonicalIdentityAndTransactionGuards();
    RgbWireCommitsOnlyLegacyIdentityAndRecovers();
    RgbWireRejectsMalformedAndInterruptedReplacement();
    ClearHidesBothResidentsAndInvalidatesReaders();
    ClearExplicitlyRecoversCorruptRootsAndHonorsGenerationFence();
    ClearFailureKeepsAuthorityAndCanRetry();
    ClearCannotStealActiveOrPreparedTransaction();
    RetirementIsDurableAtomicAndGenerationOrdered();
    GeneratedRepeated256StaysCompressed();
    PredecessorDeltaCorpusStreamsAndRecoversLosslessly();
    PredecessorRootOnlyCopyAndDeleteKeepPhysicalChain();
    WarmCacheCannotPoisonFrameZeroReplacement();
    AbortedTransferCannotPoisonCommittedDecodeCache();
    CommittedPagingPreservesIncomingPreviewAndPredecessor();
    CorruptOtherSlotCannotPoisonIndependentFrame();
    ExactWireBodyStreamsAcrossArbitraryChunks();
    TrailingWireBytesCannotIndexPastTheBlockSummaries();
    CorruptTransferCannotReplaceAuthority();
    UnfinishedReplacementDoesNotMutateDurableAuthority();
    TwoResidentsSwitchWithoutWritesAndReplaceInactiveSafely();
    MiddleEditsRemainAtomicAndCompact();
    Sparse256FramePaintDoesNotReplayTheWholeBody();
    Dense256FramePaintWritesOnlyOneStage();
    DeletingFirstFrameUpdatesOnlyTheRootMap();
    CapacityAndBoundedReads();
    CommitTimingObserverReceivesStorePhases();
    ProductionReadMetricCountsOnlyReturnedFileBytes();
    ProductionFileMappingHasNoInactiveBankGap();
    if (failures == 0) {
        std::printf(
            "[LITCHCRAFT_FRAME_STORE] body=%zu file=%zu remainder=%zu page=%zu passed\n",
            store::BODY_BANK_BYTES, store::ARTWORK_FILE_CAPACITY,
            store::FILESYSTEM_SETTINGS_RECOVERY_REMAINDER_BYTES,
            store::FrameStore::PageCacheBytes());
    }
    return failures == 0 ? 0 : 1;
}
