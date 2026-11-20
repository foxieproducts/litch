#include "trace/support_action_history.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/connection_session.hpp"
#include "modules/connection_session_codec.hpp"
#include "modules/litchcraft_frame_store.hpp"
#include "modules/litchcraft_frame_store_layout.hpp"
#include "modules/litchcraft_frame_store_receiver.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "modules/litchcraft_session.hpp"
#include "modules/litchcraft_wire_codec.hpp"
#include "virtual_litch_session.hpp"

namespace store = modules::litchcraft_frame_store;
namespace stream = modules::litchcraft_frame_stream;
namespace wire = modules::litchcraft_wire_codec;

namespace {
int failures = 0;
void Check(const bool value, const char* const message) {
    if (value) return;
    ++failures;
    std::printf("FAIL: %s\n", message);
}

struct Medium final {
    std::vector<uint8_t> bytes =
        std::vector<uint8_t>(store::ARTWORK_FILE_CAPACITY, 0xffU);
    size_t reads = 0U;
    size_t writes = 0U;
    size_t bytesWritten = 0U;
    size_t summaryBytesWritten = 0U;
    bool failRootSyncOnce = false;
    bool publishedRootWritten = false;
    size_t syncs = 0U;
    size_t failWrite = 0U;
    size_t failSync = 0U;
    bool failReads = false;
    stream::FrameStream* paintOnWrite = nullptr;
    bool paintDuringWriteAccepted = false;
    stream::FrameStream* paintOnRead = nullptr;
    bool paintDuringReadAccepted = false;
    stream::FrameStream* renderProbe = nullptr;
    stream::FrameStream* retireOnSync = nullptr;
    void (*onceOnRead)(void*) = nullptr;
    size_t onceOnReadOffset = size_t(-1);
    void (*onceOnWrite)(void*) = nullptr;
    void (*onceOnSync)(void*) = nullptr;
    void* onceOnSyncContext = nullptr;
    std::mutex renderProbeMutex;
    std::condition_variable renderProbeChanged;
    std::thread renderProbeThread;
    bool renderProbeStarted = false;
    bool renderProbeDone = false;
    bool renderBlockedByRead = false;

    ~Medium() {
        if (renderProbeThread.joinable()) renderProbeThread.join();
    }

    static bool Read(void* context, const size_t offset, uint8_t* out,
                     const size_t size) {
        auto& self = *static_cast<Medium*>(context);
        if (offset + size > self.bytes.size() || self.failReads) return false;
        ++self.reads;
        if (self.onceOnRead != nullptr &&
            (self.onceOnReadOffset == size_t(-1) || self.onceOnReadOffset == offset)) {
            const auto callback = self.onceOnRead;
            self.onceOnRead = nullptr;
            callback(self.onceOnSyncContext);
        }
        if (self.paintOnRead != nullptr) {
            auto* const pager = self.paintOnRead;
            self.paintOnRead = nullptr;
            self.paintDuringReadAccepted =
                pager->Request(1U) && pager->TryPresentRequested() &&
                pager->PaintDisplayed(1U, 2U, 1U, {83U, 84U, 85U}, true) &&
                pager->Request(0U) && pager->TryPresentRequested();
        }
        if (self.renderProbe != nullptr && !self.renderProbeStarted) {
            self.renderProbeStarted = true;
            self.renderProbeThread = std::thread([&self] {
                std::array<store::Emitter, 4U> square{};
                uint16_t index = 0U;
                uint16_t duration = 0U;
                (void)self.renderProbe->CopyDisplayedSquare(
                    0U, square, index, duration);
                std::lock_guard<std::mutex> lock(self.renderProbeMutex);
                self.renderProbeDone = true;
                self.renderProbeChanged.notify_all();
            });
            std::unique_lock<std::mutex> lock(self.renderProbeMutex);
            self.renderBlockedByRead = !self.renderProbeChanged.wait_for(
                lock, std::chrono::milliseconds(100),
                [&self] { return self.renderProbeDone; });
        }
        std::memcpy(out, self.bytes.data() + offset, size);
        return true;
    }
    static bool Write(void* context, const size_t offset, const uint8_t* bytes,
                      const size_t size) {
        auto& self = *static_cast<Medium*>(context);
        if (offset + size > self.bytes.size()) return false;
        ++self.writes;
        if (self.onceOnWrite != nullptr) {
            const auto callback = self.onceOnWrite;
            self.onceOnWrite = nullptr;
            callback(self.onceOnSyncContext);
        }
        if (self.failWrite == self.writes) return false;
        if (offset < store::FRAME_FILES_CAPACITY) self.bytesWritten += size;
        else self.summaryBytesWritten += size;
        namespace fixed = modules::litchcraft_frame_store_layout;
        self.publishedRootWritten = size == fixed::ROOT_BYTES &&
            (offset == fixed::Layout::RootOffset(0U, 0U) ||
             offset == fixed::Layout::RootOffset(0U, 1U) ||
             offset == fixed::Layout::RootOffset(1U, 0U) ||
             offset == fixed::Layout::RootOffset(1U, 1U)) &&
            bytes[0U] != 0xffU;
        if (self.paintOnWrite != nullptr) {
            auto* const pager = self.paintOnWrite;
            self.paintOnWrite = nullptr;
            self.paintDuringWriteAccepted = pager->PaintDisplayed(
                1U, 2U, 1U, {73U, 84U, 95U}, true);
        }
        std::memcpy(self.bytes.data() + offset, bytes, size);
        return true;
    }
    static bool Sync(void* context) {
        auto& self = *static_cast<Medium*>(context);
        ++self.syncs;
        if (self.onceOnSync != nullptr) {
            const auto callback = self.onceOnSync;
            self.onceOnSync = nullptr;
            callback(self.onceOnSyncContext);
        }
        if (self.failRootSyncOnce && self.publishedRootWritten) {
            self.failRootSyncOnce = false;
            return false;
        }
        self.publishedRootWritten = false;
        if (self.failSync == self.syncs) return false;
        if (self.retireOnSync != nullptr) {
            auto* const pager = self.retireOnSync;
            self.retireOnSync = nullptr;
            (void)pager->RetireDocument();
        }
        return true;
    }
    store::FrameStore::Ops Ops() { return {this, &Read, &Write, &Sync}; }
};

store::Frame MakeFrame(const uint8_t marker) {
    store::Frame frame{};
    frame.durationMs = static_cast<uint16_t>(100U + marker);
    frame.emitters[0].color = {marker, static_cast<uint8_t>(marker + 1U),
                               static_cast<uint8_t>(marker + 2U)};
    return frame;
}

store::Frame MakeDenseFrame(const uint16_t marker) {
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

void Seed(Medium& medium, store::FrameStore& frames,
          const uint16_t durationMs = 0U) {
    Check(frames.Recover() == store::Status::NoArtwork, "blank store recovers");
    store::Manifest manifest{};
    manifest.frameCount = 4U;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    store::TransactionToken token{};
    Check(frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "four-frame transaction begins");
    for (uint16_t index = 0U; index < 4U; ++index) {
        auto frame = MakeFrame(static_cast<uint8_t>(10U + index));
        if (durationMs != 0U) frame.durationMs = durationMs;
        Check(frames.WriteFrame(token, index, frame) == store::Status::Ok,
              "frame writes");
    }
    Check(frames.Commit(token) == store::Status::Ok, "four frames commit");
    medium.reads = 0U;
}

std::vector<store::Frame> SeedPredecessor256(
    Medium& medium, store::FrameStore& frames) {
    std::vector<store::Frame> source;
    source.reserve(store::FRAME_COUNT);
    source.push_back(MakeFrame(20U));
    for (uint16_t index = 1U; index < store::FRAME_COUNT; ++index) {
        source.push_back(source.back());
        source.back().durationMs = static_cast<uint16_t>(50U + index);
        source.back().emitters[index - 1U].color.r =
            static_cast<uint8_t>(index);
    }
    store::Manifest manifest{};
    manifest.frameCount = store::FRAME_COUNT;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(manifest, 0U, 21U, token) == store::Status::Ok,
          "predecessor stream fixture begins");
    for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
        Check(frames.WriteFrame(token, index, source[index]) ==
                  store::Status::Ok,
              "predecessor stream fixture writes");
    }
    Check(frames.Commit(token) == store::Status::Ok,
          "predecessor stream fixture commits");
    medium.reads = 0U;
    return source;
}

void SeedDense256(Medium& medium, store::FrameStore& frames) {
    store::Manifest manifest{};
    manifest.frameCount = store::FRAME_COUNT;
    manifest.active = true;
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(manifest, 0U, 21U, token) == store::Status::Ok,
          "dense seek fixture begins 256-frame Store document");
    for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
        Check(frames.WriteFrame(token, index, MakeDenseFrame(index)) ==
                  store::Status::Ok,
              "dense seek fixture writes each frame");
    }
    Check(frames.Commit(token) == store::Status::Ok,
          "dense seek fixture commits complete document");
    medium.reads = 0U;
}

stream::FrameStream::ServiceResult LoadOne(stream::FrameStream& pager,
                                           store::FrameStore& frames) {
    for (uint8_t attempt = 0U; attempt < 16U; ++attempt) {
        const auto result = pager.Service(frames, false);
        if (result != stream::FrameStream::ServiceResult::Loading) return result;
    }
    Check(false, "bounded frame load finishes");
    return stream::FrameStream::ServiceResult::IoFailure;
}

size_t RecordPayloadOffset(const Medium& medium, const uint16_t frameIndex) {
    size_t cursor = store::FrameStore::BodyOffset(0U);
    for (uint16_t index = 0U; index < frameIndex; ++index) {
        cursor += store::WIRE_RECORD_HEADER_BYTES +
                  fxe::ReadU16LE(medium.bytes.data() + cursor);
    }
    return cursor + store::WIRE_RECORD_HEADER_BYTES;
}

void TestPrefetchAndPriority() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()),
          "pager configures");
    Check(pager.Service(frames, true) ==
              stream::FrameStream::ServiceResult::DeferredForControl &&
              medium.reads == 0U,
          "control priority starts no flash read");
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::LoadedRequested,
          "requested frame loads in one bounded record access");
    Check(pager.TryPresentRequested(), "complete frame publishes atomically");
    store::Frame displayed{};
    uint16_t displayedIndex = 99U;
    Check(pager.CopyDisplayed(displayed, displayedIndex) && displayedIndex == 0U &&
              displayed.emitters[0].color.r == 10U,
          "render copies stable first frame");

    const size_t beforePrefetch = medium.reads;
    Check(LoadOne(pager, frames) ==
              stream::FrameStream::ServiceResult::PrefetchedNext &&
              medium.reads > beforePrefetch,
          "worker prefetches only the following frame");
    const size_t afterPrefetch = medium.reads;
    Check(pager.Request(1U) && pager.TryPresentRequested() &&
              medium.reads == afterPrefetch,
          "playback boundary swaps prefetched frame without flash I/O");
    Check(pager.CopyDisplayed(displayed, displayedIndex) && displayedIndex == 1U &&
              displayed.emitters[0].color.r == 11U,
          "prefetched frame becomes stable display");
}

void TestDenseSequentialPlaybackUsesOneFlashLeasePerFrame() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    store::Manifest manifest{};
    manifest.frameCount = 2U;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(manifest, 0U, 7U, token) == store::Status::Ok &&
              frames.WriteFrame(token, 0U, MakeDenseFrame(17U)) ==
                  store::Status::Ok &&
              frames.WriteFrame(token, 1U, MakeDenseFrame(91U)) ==
                  store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "dense playback fixture commits");
    const size_t body = store::FrameStore::BodyOffset(0U);
    const uint16_t firstBytes = fxe::ReadU16LE(medium.bytes.data() + body);
    const size_t second = body + store::WIRE_RECORD_HEADER_BYTES + firstBytes;
    const uint16_t secondBytes = fxe::ReadU16LE(medium.bytes.data() + second);
    Check(firstBytes > 512U && secondBytes > 512U,
          "dense playback fixture exceeds the old page budget");

    medium.reads = 0U;
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              pager.Service(frames, false) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              medium.reads == 1U && pager.TryPresentRequested(),
          "dense requested frame loads under one flash lease");
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::PrefetchedNext &&
              medium.reads == 2U,
          "dense sequential successor loads under one additional flash lease");
}

void TestSeedHeaderIoNeverHoldsPublicationLock() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.StageFirstFrame(frames.CurrentManifest(), frames.Generation(),
                                MakeFrame(10U)),
          "render-lock probe stages a displayed frame");
    medium.renderProbe = &pager;
    Check(pager.Request(1U) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "render-lock probe reads the independent requested slot");
    if (medium.renderProbeThread.joinable()) medium.renderProbeThread.join();
    Check(medium.renderProbeStarted && medium.renderProbeDone &&
              !medium.renderBlockedByRead,
          "flash header reads never hold the publication lock needed by render");
}

void TestPredecessorPlaybackResumesFromNearestPublishedFrame() {
    Medium medium;
    store::FrameStore initial(medium.Ops());
    const auto source = SeedPredecessor256(medium, initial);
    store::Frame independent{};
    Check(initial.ReadFrame(192U, independent) == store::Status::Ok &&
              Same(independent, source[192U]),
          "generated frame 192 decodes from its independent slot");
    stream::FrameStream pager;
    Check(pager.StageFirstFrame(initial.CurrentManifest(),
                                initial.Generation(), source[0]) &&
              pager.Request(200U),
          "distant predecessor request starts from staged frame zero");

    store::FrameStore firstRelease(medium.Ops());
    Check(firstRelease.Recover(store::FrameStore::RecoveryPurpose::RootOnly) ==
              store::Status::Ok,
          "released union reconstructs its first playback store");
    medium.reads = 0U;
    Check(LoadOne(pager, firstRelease) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "multi-slice predecessor request loads exactly");
    store::Frame displayed{};
    uint16_t displayedIndex = 0U;
    const bool copied = pager.CopyDisplayed(displayed, displayedIndex);
    if (!copied || displayedIndex != 200U ||
        !Same(displayed, source[200U]) || medium.reads > 32U) {
        std::printf("predecessor frame=%u copied=%u exact=%u reads=%zu\n",
                    displayedIndex, copied ? 1U : 0U,
                    Same(displayed, source[200U]) ? 1U : 0U, medium.reads);
    }
    Check(copied && displayedIndex == 200U &&
              Same(displayed, source[200U]) && medium.reads <= 32U,
          "distant request decodes from its nearby verified raw anchor");

    store::FrameStore secondRelease(medium.Ops());
    Check(secondRelease.Recover(store::FrameStore::RecoveryPurpose::RootOnly) ==
              store::Status::Ok &&
              pager.Request(201U),
          "released union reconstructs its next playback store");
    medium.reads = 0U;
    Check(LoadOne(pager, secondRelease) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              pager.CopyDisplayed(displayed, displayedIndex) &&
              displayedIndex == 201U && Same(displayed, source[201U]) &&
              medium.reads <= 4U,
          "fresh store resumes from frame 200 instead of replaying the 201-frame chain");
}

void TestDirtyFrameCannotSeedCommittedPredecessorDecode() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    const auto source = SeedPredecessor256(medium, frames);
    stream::FrameStream pager;
    Check(pager.StageFirstFrame(frames.CurrentManifest(), frames.Generation(),
                                source[0U]) &&
              pager.PaintDisplayed(0U, 1U, 1U, {217U, 5U, 6U}, true) &&
              pager.Request(1U),
          "dirty predecessor fixture paints frame zero before selecting one");
    const size_t writesBeforeSeek = medium.writes;
    Check(LoadOne(pager, frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "dirty predecessor fixture presents frame one before Store save");
    store::Frame displayed{};
    uint16_t index = store::FRAME_COUNT;
    Check(pager.CopyDisplayed(displayed, index) && index == 1U &&
              Same(displayed, source[1U]) &&
              medium.writes == writesBeforeSeek,
          "decoding a committed delta does not inherit unsaved paint from frame zero");
    Check(pager.Request(0U) && pager.TryPresentRequested() &&
              pager.CopyDisplayed(displayed, index) && index == 0U &&
              displayed.emitters[4U].color.r == 217U,
          "returning to frame zero preserves the unsaved physical paint");
}

void TestJumpAndCorruptionRetainCurrent() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "first frame is ready");
    store::Frame displayed{};
    uint16_t displayedIndex = 99U;

    Check(pager.Request(3U) && !pager.TryPresentRequested() &&
              pager.CopyDisplayed(displayed, displayedIndex) &&
              displayedIndex == 0U,
          "arbitrary jump retains current until complete");
    Check(LoadOne(pager, frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "arbitrary requested frame loads and publishes");

    medium.bytes[RecordPayloadOffset(medium, 2U) + 2U] ^= 0x80U;
    Check(pager.Request(2U), "corrupt target request is admitted");
    const auto corruptResult = LoadOne(pager, frames);
    Check(corruptResult == stream::FrameStream::ServiceResult::Corrupt,
          "corrupt target returns a corruption result");
    Check(pager.RequestedFrameCorrupt(),
          "corrupt target requests yellow status");
    Check(!pager.TryPresentRequested() &&
              pager.CopyDisplayed(displayed, displayedIndex) &&
              displayedIndex == 3U,
          "corrupt target retains current frame");
    const size_t afterCorrupt = medium.reads;
    for (uint8_t attempt = 0U; attempt < 10U; ++attempt) {
        Check(pager.Service(frames, false) ==
                  stream::FrameStream::ServiceResult::Idle,
              "known corrupt page is not reread");
    }
    Check(medium.reads == afterCorrupt,
          "corrupt dwell creates no repeated flash traffic");
    Check(pager.Request(1U) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "a valid request recovers after a corrupt frame");
}

void TestGenerationInvalidatesSameIndexCache() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "generation test loads frame zero");

    store::TransactionToken token{};
    Check(frames.BeginEdit(1U, 2U, token) == store::Status::Ok &&
              frames.ReplaceFrame(token, 0U, MakeFrame(42U)) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "same logical index is replaced in a new generation");
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              pager.Service(frames, false) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "new authority invalidates the old same-index cache");
    store::Frame displayed{};
    uint16_t displayedIndex = 99U;
    Check(pager.CopyDisplayed(displayed, displayedIndex) && displayedIndex == 0U &&
              displayed.emitters[0].color.r == 42U,
          "same-index commit cannot leave stale artwork displayed indefinitely");
}

void TestValidatedIncomingFirstFramePresentsWithoutFlashRead() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    auto incoming = frames.CurrentManifest();
    incoming.frameCount = store::FRAME_COUNT;
    const store::Frame first = MakeFrame(77U);
    const size_t readsBefore = medium.reads;
    Check(pager.StageFirstFrame(incoming, frames.Generation() + 1U, first) &&
              pager.TryPresentRequested() && medium.reads == readsBefore,
          "validated incoming frame zero presents atomically without a flash read");
    store::Frame displayed{};
    uint16_t displayedIndex = 99U;
    Check(pager.Generation() == frames.Generation() + 1U &&
              pager.FrameCount() == store::FRAME_COUNT &&
              pager.CopyDisplayed(displayed, displayedIndex) &&
              displayedIndex == 0U && displayed.emitters[0].color.r == 77U,
          "provisional generation exposes the complete first frame and 256-frame topology");

    const store::Frame alternate = MakeFrame(42U);
    std::atomic<bool> reading{true};
    std::atomic<uint32_t> reads{0U};
    std::atomic<uint32_t> missing{0U};
    std::atomic<uint32_t> partial{0U};
    std::thread render([&] {
        std::array<store::Emitter, 4U> square{};
        uint16_t index = 0U;
        uint16_t duration = 0U;
        while (reading.load(std::memory_order_acquire)) {
            if (!pager.CopyDisplayedSquare(0U, square, index, duration)) {
                missing.fetch_add(1U, std::memory_order_relaxed);
            } else if (index != 0U ||
                       (square[0].color.r != 77U &&
                        square[0].color.r != 42U)) {
                partial.fetch_add(1U, std::memory_order_relaxed);
            }
            reads.fetch_add(1U, std::memory_order_relaxed);
        }
    });
    for (uint32_t step = 0U; step < 2048U; ++step) {
        Check(pager.StageFirstFrame(
                  incoming, frames.Generation() + 2U + step,
                  (step & 1U) == 0U ? alternate : first),
              "resident switch stages a complete first frame");
        if ((step & 31U) == 0U) std::this_thread::yield();
    }
    reading.store(false, std::memory_order_release);
    render.join();
    std::printf("[CONCURRENT_SWITCH] reads=%u missing=%u partial=%u\n",
                reads.load(), missing.load(), partial.load());
    Check(reads.load() > 0U && missing.load() == 0U &&
              partial.load() == 0U,
          "concurrent render never observes a blank or partial resident switch");
    pager.Clear();
    Check(pager.Generation() == 0U && pager.FrameCount() == 0U &&
              !pager.CopyDisplayed(displayed, displayedIndex),
          "a rejected provisional generation clears every visible cache slot");
}

struct NoopEditFixture final {
    Medium medium;
    store::FrameStore frames{medium.Ops()};
    stream::FrameStream pager;
    store::Frame original = MakeFrame(10U);

    NoopEditFixture() {
        original.durationMs = 250U;
        original.emitters[0U].effect = 1U;
        original.emitters[0U].effectDurationMs = 1600U;
        original.emitters[255U].color = {91U, 73U, 27U};
        original.emitters[255U].effect = 2U;
        original.emitters[255U].effectDurationMs = 900U;
        store::Manifest manifest{};
        manifest.frameCount = 4U;
        manifest.active = true;
        manifest.playbackOnEntry = true;
        manifest.predecessorDeltas = true;
        manifest.favorites = modules::LITCHCRAFT_DEFAULT_QUICK_PALETTE;
        store::TransactionToken token{};
        Check(frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
              "no-op regression begins the real frame-store document");
        for (uint16_t index = 0U; index < 4U; ++index) {
            Check(frames.WriteFrame(token, index, index == 0U ? original :
                  MakeFrame(static_cast<uint8_t>(10U + index))) == store::Status::Ok,
                  "no-op regression writes complete durable frames");
        }
        Check(frames.Commit(token) == store::Status::Ok &&
                  pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested &&
                  pager.TryPresentRequested(), "no-op regression presents durable frame zero");
    }

    void EraseUndo() {
        const uint32_t revision = pager.LiveRevision();
        Check(pager.ClearDisplayedIfRevision(revision, 0U) ==
                  stream::FrameStream::GuardedEditResult::Changed &&
                  pager.StepHistoryIfRevision(pager.LiveRevision(), 0U, false) ==
                  stream::FrameStream::GuardedEditResult::Changed &&
                  pager.LiveRevision() == revision + 2U && pager.RedoCount() >= 1U &&
                  pager.HasUnpersistedEdit(), "exact Erase/Undo has two live actions and retained Redo");
    }

    void Drain() {
        for (unsigned attempt = 0U; attempt < 12U && pager.HasUnpersistedEdit(); ++attempt) {
            (void)pager.Service(frames, false);
        }
        Check(!pager.HasUnpersistedEdit(), "bounded worker retry settles retained edits");
    }

    store::FixedRoot Root() {
        namespace fixed = modules::litchcraft_frame_store_layout;
        std::array<uint8_t, fixed::FRAME_SLOT_BYTES> scratch{};
        fixed::Layout layout(medium.Ops(), scratch.data(), scratch.size());
        store::FixedRoot root{};
        uint8_t copy = 0U;
        Check(layout.LatestRoot(0U, root, copy) == fixed::Status::Ok,
              "test reads the authoritative root through its existing layout owner");
        return root;
    }
};

void TestNetUnchangedEditsRetainDurableRoot() {
    for (unsigned scenario = 0U; scenario < 4U; ++scenario) {
        NoopEditFixture f;
        if (scenario == 1U) {
            Check(f.pager.PaintDisplayed(0U, 7U, 1U, {77U, 88U, 99U}, false),
                  "staged no-op case first makes a real edit");
            f.Drain();
            uint16_t index = 99U;
            Check(f.pager.CopyDisplayed(f.original, index) && index == 0U &&
                      f.Root().stageIndex == 0U,
                  "genuine durable edit leaves an actual stage before Erase/Undo");
        }
        if (scenario >= 2U) {
            if (scenario == 3U) {
                std::fill(f.medium.bytes.begin() + store::FRAME_FILES_CAPACITY,
                          f.medium.bytes.end(), 0xffU);
            }
            Check(f.frames.Recover(store::FrameStore::RecoveryPurpose::RootOnly) ==
                      store::Status::Ok, "no-op compares recovered root without derived readiness");
        }
        f.EraseUndo();
        const uint32_t generation = f.frames.Generation();
        const uint32_t revision = f.pager.LiveRevision();
        const uint32_t epoch = f.pager.DocumentEpoch();
        const size_t writes = f.medium.writes, syncs = f.medium.syncs;
        const auto bytes = f.medium.bytes;
        f.pager.RequestImmediateSave();
        f.Drain();
        Check(f.frames.Generation() == generation && f.pager.Generation() == generation &&
                  f.pager.LiveRevision() == revision && f.pager.DocumentEpoch() == epoch &&
                  f.pager.RedoCount() >= 1U && f.medium.writes == writes &&
                  f.medium.syncs == syncs && f.medium.bytes == bytes,
              "net-unchanged inverse acknowledges live sequence without any root/body/cache Write or Sync");
        store::FrameStore recovered(f.medium.Ops());
        store::Frame restored{};
        const auto recoveredStatus = recovered.Recover(store::FrameStore::RecoveryPurpose::RootOnly);
        const auto readStatus = recovered.ReadFrame(0U, restored);
        Check(recoveredStatus == store::Status::Ok && recovered.Generation() == generation &&
                  readStatus == store::Status::Ok && Same(restored, f.original),
              "no-op recovery retains every RGB/effect/effect-duration/dwell field");
        std::printf("[EDIT_NOOP] baseline=%u gen=%u->%u live=%u epoch=%u writes=%zu syncs=%zu redo=%u recovered_gen=%u semantic_exact=%u\n",
                    scenario, generation, f.frames.Generation(), revision, epoch,
                    f.medium.writes - writes, f.medium.syncs - syncs,
                    unsigned(f.pager.RedoCount()), recovered.Generation(), Same(restored, f.original));
    }
}

void TestUnsavedSemanticDifferencesStillPersist() {
    for (unsigned field = 0U; field < 4U; ++field) {
        NoopEditFixture f;
        if (field == 0U) Check(f.pager.PaintDisplayed(0U, 5U, 1U, {77U, 88U, 99U}, false),
                              "pre-Erase paint differs from durable baseline");
        if (field == 1U) Check(f.pager.ApplyDisplayedEffect(0U, 0U, 1U, 2U, 1600U),
                              "effect-only difference is a real edit");
        if (field == 2U) Check(f.pager.ApplyDisplayedEffect(0U, 0U, 1U, 1U, 1700U),
                              "effect-duration-only difference is a real edit");
        if (field == 3U) Check(f.pager.EditDisplayedDurationIfRevision(
            {f.pager.Generation(), f.pager.LiveRevision(), 0U, 350U,
             stream::FrameStream::FrameEditAction::Duration}) ==
                stream::FrameStream::GuardedEditResult::Changed,
                              "frame-dwell-only difference is a real edit");
        store::Frame expected{};
        uint16_t index = 99U;
        Check(f.pager.CopyDisplayed(expected, index) && !Same(expected, f.original),
              "unsaved baseline retains its independent semantic difference");
        f.EraseUndo();
        const uint32_t generation = f.frames.Generation();
        const size_t writes = f.medium.writes;
        f.Drain();
        store::FrameStore recovered(f.medium.Ops());
        store::Frame saved{};
        Check(f.medium.writes > writes && f.frames.Generation() == generation + 1U &&
                  recovered.Recover() == store::Status::Ok &&
                  recovered.ReadFrame(0U, saved) == store::Status::Ok && Same(saved, expected),
              "Undo to an unsaved pre-Erase frame persists the exact semantic difference");
        std::printf("[EDIT_REAL] field=%u gen=%u->%u recovered=exact\n",
                    field, generation, f.frames.Generation());
    }
    NoopEditFixture f;
    Check(f.pager.ClearDisplayedIfRevision(f.pager.LiveRevision(), 0U) ==
              stream::FrameStream::GuardedEditResult::Changed, "erase precedes a durable boundary");
    f.Drain();
    const uint32_t generation = f.frames.Generation();
    Check(f.pager.StepHistoryIfRevision(f.pager.LiveRevision(), 0U, false) ==
              stream::FrameStream::GuardedEditResult::Changed, "Undo follows already saved Erase");
    f.Drain();
    store::Frame saved{};
    Check(f.frames.Generation() == generation + 1U &&
              f.frames.ReadFrame(0U, saved) == store::Status::Ok && Same(saved, f.original),
          "Undo after durable Erase must publish the restored baseline");
}

void TestNoopPreservesOtherDirtySlotsAndPageDemand() {
    NoopEditFixture f;
    f.EraseUndo();
    Check(f.pager.Request(1U) && LoadOne(f.pager, f.frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested && f.pager.TryPresentRequested() &&
              f.pager.PaintDisplayed(1U, 2U, 1U, {83U, 84U, 85U}, false),
          "another displayed slot accepts a real edit before equality inspection");
    const uint32_t generation = f.frames.Generation();
    const size_t writes = f.medium.writes, syncs = f.medium.syncs;
    Check(f.pager.Service(f.frames, false) == stream::FrameStream::ServiceResult::Loading &&
              f.pager.HasUnpersistedEdit() && f.medium.writes == writes && f.medium.syncs == syncs &&
              f.frames.Generation() == generation, "equal inactive slot does not clear second dirty slot");
    f.Drain();
    store::Frame saved{};
    Check(f.frames.Generation() == generation + 1U &&
              f.frames.ReadFrame(1U, saved) == store::Status::Ok && saved.emitters[8U].color.r == 83U,
          "second dirty slot persists after the no-op acknowledgement");

    NoopEditFixture page;
    store::FrameStore::ReadCursor outstanding{};
    Check(page.frames.BeginReadFrame(1U, outstanding) == store::Status::Ok &&
              page.frames.ContinueReadFrame(outstanding, 1U) == store::Status::Incomplete &&
              outstanding.active,
          "existing Store paging owner has an incomplete cursor before comparison");
    page.EraseUndo();
    page.pager.RequestImmediateSave();
    page.Drain();
    Check(page.frames.ContinueReadFrame(outstanding, store::STAGE_SLOT_BYTES) ==
              store::Status::RevisionConflict && !outstanding.active &&
              page.pager.Request(1U) && LoadOne(page.pager, page.frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested && page.pager.TryPresentRequested() &&
              page.pager.DisplayedIndex() == 1U,
          "comparison invalidates old Store cursor and the pager reissues demand without IoFailure");
    std::puts("[EDIT_NOOP] multiple_dirty=retained pending_page=reissued immediate_flush=settled");
}

void TestNoopReadMutationAndRecoveryBoundaries() {
    struct Mutation final {
        stream::FrameStream* pager = nullptr;
        unsigned action = 0U;
        bool accepted = false;
        static void Apply(void* context) {
            auto& self = *static_cast<Mutation*>(context);
            if (self.action == 0U) self.accepted = self.pager->PaintDisplayed(
                0U, 5U, 1U, {77U, 88U, 99U}, false);
            if (self.action == 1U) {
                self.accepted = self.pager->NewBlankDocument(
                    self.pager->Generation(), self.pager->LiveRevision(),
                    modules::LITCHCRAFT_DEFAULT_QUICK_PALETTE) ==
                    stream::FrameStream::GuardedEditResult::Changed;
                self.accepted = self.accepted && self.pager->PaintDisplayed(
                    0U, 5U, 1U, {77U, 88U, 99U}, false);
            }
            if (self.action == 2U) self.accepted = self.pager->RetireDocument();
            if (self.action == 3U) self.accepted = self.pager->QueueStructuralEdit(
                {self.pager->Generation(), self.pager->LiveRevision(), 0U, 0U,
                 stream::FrameStream::FrameEditAction::AddCopy}) ==
                    stream::FrameStream::StructuralResult::Pending;
        }
    };
    for (unsigned action = 0U; action < 4U; ++action) {
        NoopEditFixture f;
        f.EraseUndo();
        const uint32_t epoch = f.pager.DocumentEpoch();
        Mutation mutation{&f.pager, action};
        f.medium.onceOnRead = &Mutation::Apply;
        f.medium.onceOnSyncContext = &mutation;
        const size_t writes = f.medium.writes;
        Check(f.pager.Service(f.frames, false) == stream::FrameStream::ServiceResult::Loading &&
                  mutation.accepted && f.medium.writes == writes,
              "comparison yields to actual production mutation without acknowledging stale identity");
        if (action == 2U) {
            Check(f.pager.Generation() == 0U && f.pager.RetiredGeneration() == f.frames.Generation(),
                  "no-op read cannot resurrect retired document");
            std::puts("[EDIT_NOOP] read_interleave=retire authority=revoked root=retained");
            continue;
        }
        if (action == 1U) Check(f.pager.HasPendingBlankDocument() &&
                    f.pager.DocumentEpoch() != epoch && f.pager.HasUnpersistedEdit(),
                    "New's initial pending baseline cannot be marked clean by old equality");
        if (action == 3U) {
            uint16_t selected = 99U;
            uint32_t revision = 0U;
            Check(f.pager.HasUnpersistedEdit() && f.pager.FrameCount() == 5U &&
                      f.frames.CurrentManifest().frameCount == 4U &&
                      f.pager.TakeStructuralResult(selected, revision) ==
                          stream::FrameStream::StructuralResult::Changed,
                  "structural RAM application during comparison retains its separate dirty save");
        }
        f.Drain();
        if (action == 3U) Check(f.frames.CurrentManifest().frameCount == 5U,
                               "no-op acknowledgement cannot discard accepted layout persistence");
        store::Frame saved{};
        Check(f.frames.ReadFrame(0U, saved) == store::Status::Ok &&
                  ((action == 3U && Same(saved, f.original)) || saved.emitters[20U].color.r == 77U),
              "retry saves only current document's accepted intent");
        std::printf("[EDIT_NOOP] read_interleave=%u epoch=%u->%u accepted=1 recovered=current\n",
                    action, epoch, f.pager.DocumentEpoch());
    }
    for (const bool corrupt : {false, true}) {
        NoopEditFixture f;
        f.EraseUndo();
        namespace fixed = modules::litchcraft_frame_store_layout;
        const size_t offset = fixed::Layout::SlotOffset(0U, 0U) + 100U;
        const uint8_t byte = f.medium.bytes[offset];
        if (corrupt) f.medium.bytes[offset] ^= 1U;
        else f.medium.failReads = true;
        const size_t writes = f.medium.writes, syncs = f.medium.syncs;
        Check(f.pager.Service(f.frames, false) == stream::FrameStream::ServiceResult::IoFailure &&
                  f.pager.HasUnpersistedEdit() && f.medium.writes == writes && f.medium.syncs == syncs,
              "failed or corrupt comparison never certifies dirty frame clean");
        f.medium.failReads = false;
        f.medium.bytes[offset] = byte;
        f.Drain();
        Check(f.medium.writes == writes && f.medium.syncs == syncs,
              "repaired read retries exact no-op without a fabricated save");
        std::printf("[EDIT_NOOP] read_fault=%s retry_dirty=%u writes=%zu syncs=%zu\n",
                    corrupt ? "crc" : "io", f.pager.HasUnpersistedEdit(),
                    f.medium.writes - writes, f.medium.syncs - syncs);
    }
}

void TestResidentSwitchWaitsForAcknowledgedBoardEdit() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    const auto manifest = frames.CurrentManifest();
    Check(pager.StageFirstFrame(manifest, frames.Generation(), MakeFrame(10U)) &&
              pager.PaintDisplayed(0U, 0U, 1U, {201U, 4U, 5U}, true),
          "board paint changes the displayed Store frame before persistence");
    const auto dirtyAdmission = pager.TryStageFirstFrame(
        manifest, frames.Generation() + 1U, MakeFrame(77U));
    Check(dirtyAdmission.kind == store::PresentationAdmission::Kind::Rejected &&
              dirtyAdmission.dirty,
          "resident switch rejects an unpersisted board edit instead of deferring it as Show");
    store::Frame displayed{};
    uint16_t index = 99U;
    Check(pager.CopyDisplayed(displayed, index) && index == 0U &&
              displayed.emitters[0].color.r == 201U,
          "rejected switch keeps the acknowledged board edit visible");
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Idle,
          "normal storage worker saves the pending board edit");
    store::FrameStore recovered(medium.Ops());
    store::Frame saved{};
    Check(recovered.Recover() == store::Status::Ok &&
              recovered.ReadFrame(0U, saved) == store::Status::Ok &&
              saved.emitters[0].color.r == 201U &&
              pager.StageFirstFrame(
                  manifest, frames.Generation() + 1U, MakeFrame(77U)),
          "board edit is durable before the next resident may present");
}

void TestPlaybackKeepsDirtyPaintedFrameWhileSeeking() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::PrefetchedNext,
          "two-frame playback fixture has the next frame already prefetched");
    Check(pager.PaintDisplayed(0U, 0U, 1U, {219U, 7U, 5U}, true) &&
              pager.Request(1U) && pager.TryPresentRequested() &&
              pager.DisplayedIndex() == 1U && pager.HasUnpersistedEdit(),
          "prefetched frame presents while acknowledged paint stays resident");
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Idle &&
              !pager.HasUnpersistedEdit() &&
              pager.DisplayedIndex() == 1U,
          "background save durably retains paint after visible frame seek");
    store::FrameStore rebooted(medium.Ops());
    store::Frame painted{};
    Check(rebooted.Recover() == store::Status::Ok &&
              rebooted.ReadFrame(0U, painted) == store::Status::Ok &&
              painted.emitters[0].color.r == 219U,
          "reboot retains the frame painted before an animation boundary");
}

void TestDirtyFrameSelectionPresentsBeforeStoreRewrite() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    SeedDense256(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "dirty seek fixture presents frame zero");
    const size_t writesBeforeSeek = medium.writes;
    Check(pager.PaintDisplayed(0U, 0U, 1U, {219U, 7U, 5U}, true) &&
              pager.Request(1U),
          "physical paint is visible before a frame tap");
    const auto page = LoadOne(pager, frames);
    Check(page == stream::FrameStream::ServiceResult::LoadedRequested &&
              medium.writes == writesBeforeSeek &&
              pager.TryPresentRequested() && pager.DisplayedIndex() == 1U,
          "requested frame presents before the dirty Store body is rewritten");
    store::Frame displayed{};
    uint16_t index = store::FRAME_COUNT;
    Check(pager.CopyDisplayed(displayed, index) && index == 1U &&
              Same(displayed, MakeDenseFrame(1U)),
          "board shows the requested frame while the old paint awaits storage");
    bool scannedWithoutSave = true;
    for (uint16_t next = 2U; next < 7U; ++next) {
        const auto result = pager.Request(next) ? LoadOne(pager, frames)
                                                : stream::FrameStream::ServiceResult::IoFailure;
        scannedWithoutSave = scannedWithoutSave &&
            result == stream::FrameStream::ServiceResult::LoadedRequested &&
            pager.TryPresentRequested() && pager.DisplayedIndex() == next &&
            medium.writes == writesBeforeSeek;
    }
    Check(scannedWithoutSave,
          "rapid frame browsing after paint never waits for a full Store save");
    const size_t readsBeforeJump = medium.reads;
    Check(pager.Request(255U) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              pager.DisplayedIndex() == 255U &&
              medium.reads <= readsBeforeJump + 3U &&
              medium.writes == writesBeforeSeek,
          "distant independent frame seeks with one bounded page read and no save");
    Check(pager.Request(1U) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "scanning can return to the first newly selected frame");
    Check(pager.BeginHistoryStroke(1U) &&
              pager.PaintDisplayed(1U, 1U, 1U, {77U, 88U, 99U}, true),
          "the next brush paints the selected frame before the prior save");
    pager.EndHistoryStroke();
    Check(pager.StepHistoryIfRevision(pager.LiveRevision(), 1U, false) ==
              stream::FrameStream::GuardedEditResult::Changed &&
              pager.CopyDisplayed(displayed, index) &&
              Same(displayed, MakeDenseFrame(1U)),
          "Undo immediately restores the selected frame during deferred save");
    Check(pager.Request(0U) && pager.TryPresentRequested() &&
              pager.CopyDisplayed(displayed, index) && index == 0U &&
              displayed.emitters[0].color.r == 219U,
          "returning to the edited frame preserves acknowledged paint");
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Loading &&
              pager.Service(frames, false) ==
                  stream::FrameStream::ServiceResult::Idle &&
              !pager.HasUnpersistedEdit(),
          "both edited frames save without discarding the next brush or Undo");
    store::FrameStore rebooted(medium.Ops());
    store::Frame recovered{};
    Check(rebooted.Recover() == store::Status::Ok &&
              rebooted.ReadFrame(0U, recovered) == store::Status::Ok &&
              recovered.emitters[0].color.r == 219U &&
              rebooted.ReadFrame(1U, recovered) == store::Status::Ok &&
              Same(recovered, MakeDenseFrame(1U)),
          "reboot restores paint and Undo after deferred dense Store rewrite");
}

void TestNextBrushDuringDeferredSaveKeepsBothFrames() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              pager.PaintDisplayed(0U, 0U, 1U, {219U, 7U, 5U}, true) &&
              pager.Request(1U) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "deferred-save fixture seeks to the next frame before rewriting");
    medium.paintOnWrite = &pager;
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Loading &&
              medium.paintDuringWriteAccepted &&
              pager.DisplayedIndex() == 1U,
          "second frame accepts physical paint during the first frame save");
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Idle &&
              !pager.HasUnpersistedEdit(),
          "second frame saves after the first without losing its edit");
    store::FrameStore rebooted(medium.Ops());
    store::Frame restored{};
    Check(rebooted.Recover() == store::Status::Ok &&
              rebooted.ReadFrame(0U, restored) == store::Status::Ok &&
              restored.emitters[0].color.r == 219U &&
              rebooted.ReadFrame(1U, restored) == store::Status::Ok &&
              restored.emitters[8U].color.r == 73U,
          "reboot retains both edits accepted across the slow save boundary");
}

void TestFullDirtyCacheFlushesWithoutLosingAcknowledgedPaint() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "three-dirty fixture presents the first saved frame");
    const size_t writesBeforeScan = medium.writes;
    bool painted = pager.PaintDisplayed(0U, 0U, 1U, {71U, 2U, 3U}, true);
    for (uint16_t index = 1U; index <= 2U; ++index) {
        painted = painted && pager.Request(index) &&
            LoadOne(pager, frames) ==
                stream::FrameStream::ServiceResult::LoadedRequested &&
            pager.TryPresentRequested() &&
            pager.PaintDisplayed(index, static_cast<uint8_t>(index), 1U,
                                   {static_cast<uint8_t>(71U + index), 2U, 3U},
                                   true);
    }
    Check(painted && medium.writes == writesBeforeScan,
          "three different frames accept paint without foreground Store writes");
    Check(pager.Request(3U) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() && pager.DisplayedIndex() == 3U,
          "full dirty cache saves one frame and admits the next selection");
    pager.RequestImmediateSave();
    for (uint8_t attempt = 0U; attempt < 3U && pager.HasUnpersistedEdit();
         ++attempt) {
        (void)pager.Service(frames, false);
    }
    Check(!pager.HasUnpersistedEdit(),
          "remaining dirty frames flush at the lifecycle boundary");
    store::FrameStore rebooted(medium.Ops());
    bool retained = rebooted.Recover() == store::Status::Ok;
    store::Frame frame{};
    for (uint16_t index = 0U; index <= 2U; ++index) {
        retained = retained &&
            rebooted.ReadFrame(index, frame) == store::Status::Ok &&
            frame.emitters[static_cast<size_t>(index) * 4U].color.r ==
                71U + index;
    }
    Check(retained, "reboot restores all three acknowledged frame paints");
}

void TestFullDirty256CacheSelectsWithoutWholeBodyReplay() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    const auto source = SeedPredecessor256(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "256-frame saturation fixture presents its first saved frame");
    bool painted = pager.PaintDisplayed(0U, 0U, 1U, {71U, 2U, 3U}, true);
    for (uint16_t index = 1U; index <= 2U; ++index) {
        painted = painted && pager.Request(index) &&
            LoadOne(pager, frames) ==
                stream::FrameStream::ServiceResult::LoadedRequested &&
            pager.TryPresentRequested() &&
            pager.PaintDisplayed(index, static_cast<uint8_t>(index), 1U,
                                   {static_cast<uint8_t>(71U + index), 2U, 3U},
                                   true);
    }
    medium.reads = 0U;
    const bool selected = painted && pager.Request(3U) &&
        LoadOne(pager, frames) ==
            stream::FrameStream::ServiceResult::LoadedRequested &&
        pager.TryPresentRequested() && pager.DisplayedIndex() == 3U;
    if (medium.reads >= 120U) {
        std::printf("256-frame dirty selection storage reads=%zu\n",
                    medium.reads);
    }
    Check(selected && medium.reads < 120U,
          "fourth frame presents after bounded save and page service");
    store::Frame displayed{};
    uint16_t displayedIndex = store::FRAME_COUNT;
    Check(pager.CopyDisplayed(displayed, displayedIndex) &&
              displayedIndex == 3U && Same(displayed, source[3U]),
          "fourth selection presents exact committed frame pixels");
}

void TestDenseSinglePaintWritesOnlyOneFrame() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    SeedDense256(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "dense edit presents the first saved frame");
    medium.bytesWritten = 0U;
    medium.summaryBytesWritten = 0U;
    Check(pager.PaintDisplayed(0U, 0U, 1U, {71U, 2U, 3U}, true) &&
              pager.Service(frames, false) ==
                  stream::FrameStream::ServiceResult::Idle,
          "dense edit is accepted and saved through FrameStream service");
    constexpr size_t MAX_SINGLE_EDIT_BYTES =
        2U * modules::litchcraft_frame_store_layout::FRAME_SLOT_BYTES +
        4U * modules::litchcraft_frame_store_layout::ROOT_BYTES;
    if (medium.bytesWritten > MAX_SINGLE_EDIT_BYTES) {
        std::printf("dense single-paint bytes written=%zu limit=%zu\n",
                    medium.bytesWritten, MAX_SINGLE_EDIT_BYTES);
    }
    Check(medium.bytesWritten <= MAX_SINGLE_EDIT_BYTES,
          "one dense frame edit never writes the artwork bank");
    Check(medium.summaryBytesWritten == store::WIRE_SUMMARY_PAGE_BYTES,
          "one dense frame edit writes one bounded derived summary page");
    store::FrameStore recovered(medium.Ops());
    store::Frame first{};
    store::Frame last{};
    Check(recovered.Recover() == store::Status::Ok &&
              recovered.ReadFrame(0U, first) == store::Status::Ok &&
              recovered.ReadFrame(255U, last) == store::Status::Ok &&
              first.emitters[0U].color.r == 71U &&
              Same(last, MakeDenseFrame(255U)),
          "bounded edit reboots with changed and untouched dense frames");
}

void TestPageReadCannotOverwritePaintedInactiveSlot() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::PrefetchedNext &&
              pager.Request(2U) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              pager.Request(0U) && pager.TryPresentRequested(),
          "interleaved read fixture has A displayed and all three slots resident");
    medium.paintOnRead = &pager;
    Check(pager.Request(3U), "fourth frame starts a Store page read");
    const auto interruptedPage = LoadOne(pager, frames);
    store::Frame displayed{};
    uint16_t index = store::FRAME_COUNT;
    Check(interruptedPage == stream::FrameStream::ServiceResult::Idle &&
              medium.paintDuringReadAccepted && pager.HasUnpersistedEdit() &&
              pager.Request(1U) && pager.TryPresentRequested() &&
              pager.CopyDisplayed(displayed, index) && index == 1U &&
              displayed.emitters[8U].color.r == 83U,
          "completed read cannot overwrite B painted during decode");
    pager.RequestImmediateSave();
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Idle &&
              !pager.HasUnpersistedEdit(),
          "interleaved B paint commits after the refused page publication");
    store::FrameStore rebooted(medium.Ops());
    store::Frame restored{};
    Check(rebooted.Recover() == store::Status::Ok &&
              rebooted.ReadFrame(1U, restored) == store::Status::Ok &&
              restored.emitters[8U].color.r == 83U,
          "reboot retains paint accepted during a concurrent page read");
    Check(pager.Request(3U) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() && pager.DisplayedIndex() == 3U,
          "refused page publication retries after the painted frame is saved");
}

void TestImmediateSaveBypassesPausedPresentation() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::PrefetchedNext &&
              pager.PaintDisplayed(0U, 0U, 1U, {193U, 4U, 5U}, true) &&
              pager.Request(1U),
          "sleep barrier starts with dirty A and pending resident B");
    pager.RequestImmediateSave();
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Idle &&
              !pager.HasUnpersistedEdit() && pager.DisplayedIndex() == 0U,
          "immediate durability saves without waiting for a render tick");
    store::FrameStore rebooted(medium.Ops());
    store::Frame saved{};
    Check(rebooted.Recover() == store::Status::Ok &&
              rebooted.ReadFrame(0U, saved) == store::Status::Ok &&
              saved.emitters[0U].color.r == 193U,
          "reboot retains the edit after the paused-presentation flush");
}

void TestEditInvalidatesAnInterruptedPageRead() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    store::FrameStore::ReadCursor cursor{};
    Check(frames.BeginReadFrame(1U, cursor) == store::Status::Ok &&
              frames.ContinueReadFrame(cursor, 512U) ==
                  store::Status::Incomplete,
          "page read can pause after one bounded chunk");
    store::TransactionToken token{};
    Check(frames.BeginEdit(1U, 4U, token) == store::Status::Ok &&
              frames.ReplaceFrame(token, 0U, MakeFrame(31U)) == store::Status::Ok &&
              frames.ContinueReadFrame(cursor, 512U) ==
                  store::Status::RevisionConflict,
          "priority edit cannot leave a mixed interrupted read page");
    Check(frames.Abort(token) == store::Status::Ok,
          "interrupted-read fixture aborts cleanly");
}

void StartStorePlayback(modules::LitchcraftSession& session,
                        const uint32_t generation) {
    std::array<uint8_t, 6U> play{1U, 0U};
    fxe::WriteU32LE(generation, play.data() + 2U);
    Check(!session.IsAnimationPlaying() &&
              session.ApplyConnectionOrdered(2U, play.data(), play.size()) ==
                  modules::LitchcraftSession::Status::Ok,
          "saved Store artwork starts paused until explicit Play");
}

void TestSessionPresentsAndAdvancesFlashBackedFrames() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()),
          "playback pager configures from committed manifest");
    Check(LoadOne(pager, frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::PrefetchedNext,
          "worker loads first frame and successor before presentation begins");

    uint32_t storageWakes = 0U;
    modules::LitchcraftSession session;
    modules::LitchcraftSession::Canvas fallback{};
    session.Configure(3U, 41U);
    session.RestoreArtwork(fallback, false, 0U);
    session.AttachFrameStream(
        &pager,
        [](void* context, bool, uint32_t, bool) {
            ++*static_cast<uint32_t*>(context);
        },
        &storageWakes);
    Check(session.IsActive(),
          "schema-4 active authority makes boot-restored playback visible");

    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    session.SetIndependentPresentation(true);
    modules::litchcraft_presentation::Inputs inputs{};
    modules::litchcraft_presentation::Renderer renderer;
    const modules::litchcraft_presentation::StreamPort presentationPort{
        &pager,
        [](void* context, bool, uint32_t, bool, bool) {
            ++*static_cast<uint32_t*>(context);
        }, &storageWakes, true};
    session.CapturePresentation(inputs);
    auto candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(candidate.ready && !pager.PresentationReady() &&
              pager.DisplayedIndex() == UINT16_MAX,
          "prepared first frame is not physical display evidence");
    Check(renderer.NotePresented(candidate, presentationPort) &&
              session.ApplyPresentationFeedback(candidate),
          "confirmed first Show admits exact artwork feedback");
    Check(board[0].left.r == 10U && session.SelectedFrame() == 0U,
          "production session presents committed frame zero");
    StartStorePlayback(session, frames.Generation());
    fxe::ManualMillis::SetNowMs(109U);
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(renderer.NotePresented(candidate, presentationPort) &&
              session.ApplyPresentationFeedback(candidate),
          "unchanged publication preserves the accepted playback origin");
    Check(board[0].left.r == 10U && session.SelectedFrame() == 0U,
          "frame zero remains visible for its complete duration");
    fxe::ManualMillis::SetNowMs(110U);
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(board[0].left.r == 11U && pager.DisplayedIndex() == 0U &&
              session.SelectedFrame() == 0U &&
              pager.TryBeginPresentationMutation() == 0U,
          "prepared successor pins confirmed A and gates mutation before Show");
    std::printf("[CONFIRMED_STORE_PRESENTATION] t=110 prepared_frame=%u confirmed_frame=%u session_frame=%u next=confirm_or_cancel actual_output=controlled\n",
        candidate.token.frameIndex, pager.DisplayedIndex(), session.SelectedFrame());
    const auto heldCommitAdmission = pager.TryReserveReplacement(
        frames.Generation(), frames.CurrentManifest(), frames.Generation() + 1U);
    const auto heldSelectAdmission = pager.TryStageFirstFrame(
        frames.CurrentManifest(), frames.Generation() + 1U, MakeFrame(77U));
    const auto staleHeldAdmission = pager.TryReserveReplacement(
        frames.Generation() + 1U, frames.CurrentManifest(), frames.Generation() + 2U);
    Check(heldCommitAdmission.kind == store::PresentationAdmission::Kind::PendingPresentation &&
              !heldCommitAdmission.dirty &&
              heldSelectAdmission.kind == store::PresentationAdmission::Kind::PendingPresentation &&
              staleHeldAdmission.kind == store::PresentationAdmission::Kind::Rejected &&
              !pager.Configure(frames.CurrentManifest(), frames.Generation()),
          "only valid root replacement waits for Show; stale authority remains rejected");
    const auto denied = candidate;
    const uint32_t preparedEpoch = inputs.presentationEpoch;
    session.CapturePresentation(inputs);
    Check(inputs.presentationEpoch == preparedEpoch &&
              pager.IsPresentationContextCurrent(preparedEpoch),
          "unchanged main capture cannot invalidate an in-flight Show");
    renderer.Cancel(candidate, presentationPort);
    Check(pager.DisplayedIndex() == 0U,
          "denied Show retains the physically confirmed frame");
    const uint32_t mutation = pager.TryBeginPresentationMutation();
    auto blocked = denied.token;
    Check(mutation != 0U &&
              !pager.PreparePresentation(inputs.frame, inputs.presentationEpoch, blocked) &&
              !pager.EndPresentationMutation(mutation + 1U) &&
              pager.EndPresentationMutation(mutation),
          "bounded main admission excludes preparation and stale release");
    fxe::ManualMillis::SetNowMs(115U);
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(!pager.ConfirmPresented(denied.token) &&
              !pager.CancelPresentation(denied.token) &&
              pager.HasPresentationCandidate(),
          "stale output completion cannot promote or release a successor lease");
    Check(renderer.NotePresented(candidate, presentationPort) &&
              session.ApplyPresentationFeedback(candidate),
          "retry confirms B without restarting the authored schedule");
    std::printf("[CONFIRMED_STORE_PRESENTATION] t=115 denied=retained retry=confirmed frame=%u stale_completion=rejected next=authored_boundary_221 actual_output=controlled\n",
        session.SelectedFrame());
    Check(board[0].left.r == 11U && session.SelectedFrame() == 1U &&
              storageWakes != 0U,
          "prefetched frame one swaps at the exact playback boundary");
    Check(LoadOne(pager, frames) ==
              stream::FrameStream::ServiceResult::PrefetchedNext,
          "boundary wake prefetches frame two without a timer delay");
    fxe::ManualMillis::SetNowMs(221U);
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(renderer.NotePresented(candidate, presentationPort) &&
              session.ApplyPresentationFeedback(candidate),
          "confirmed playback continues after an output denial");
    Check(board[0].left.r == 12U && session.SelectedFrame() == 2U,
          "production playback continues through the flash-backed chain");

    fxe::ManualMillis::SetNowMs(300U);
    std::array<uint8_t, 6U> rewind{2U, 0U};
    fxe::WriteU32LE(frames.Generation(), rewind.data() + 2U);
    Check(session.ApplyConnectionOrdered(2U, rewind.data(), rewind.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "cached rewind starts the next playback run at confirmed A");
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(renderer.NotePresented(candidate, presentationPort) &&
              session.ApplyPresentationFeedback(candidate),
          "rewind confirms A before a multi-frame catch-up");
    constexpr uint32_t editEpoch = 62U;
    std::array<uint8_t, 8U> guard{};
    fxe::WriteU32LE(frames.Generation(), guard.data());
    fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
    const std::array<uint8_t, 9U> mark{0U, 77U, 0U, 3U, 15U, 21U, 22U, 23U, 63U};
    Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), editEpoch) ==
              modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(3U, mark.data(), mark.size(), editEpoch) ==
              modules::LitchcraftSession::Status::Ok && session.UndoCount() == 1U,
          "actual Ordered paint records one frame-A history entry");
    pager.RequestImmediateSave();
    (void)pager.Service(frames, false);
    Check(!pager.HasUnpersistedEdit(),
          "existing storage owner saves the history mark before playback resumes");
    StartStorePlayback(session, frames.Generation());
    fxe::ManualMillis::SetNowMs(550U);
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(candidate.playback.selectedFrame == 2U &&
              candidate.playback.frameStartedAtMs == 521U &&
              pager.DisplayedIndex() == 0U,
          "catch-up stages C after both authored predecessor dwells");
    renderer.Cancel(candidate, presentationPort);
    fxe::ManualMillis::SetNowMs(560U);
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    std::printf("[CONFIRMED_STORE_CATCHUP] denied_t=550 retry_t=560 candidate=%u start=%u awaiting=%u actual=%u\n",
        candidate.playback.selectedFrame, candidate.playback.frameStartedAtMs,
        candidate.playback.awaitingPlaybackFrame, pager.DisplayedIndex());
    Check(candidate.playback.selectedFrame == 2U &&
              candidate.playback.frameStartedAtMs == 521U &&
              !candidate.playback.awaitingPlaybackFrame &&
              renderer.NotePresented(candidate, presentationPort) &&
              session.ApplyPresentationFeedback(candidate) &&
              session.AnimationFrameElapsedMs() == 39U,
          "denied multi-frame catch-up retries C at its original authored phase");

    const auto staleInputs = inputs;
    const uint32_t control = pager.TryBeginPresentationControl();
    std::array<uint8_t, 6U> select{2U, 3U};
    fxe::WriteU32LE(frames.Generation(), select.data() + 2U);
    Check(control != 0U && session.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "bounded control admission accepts a cold explicit seek");
    session.CapturePresentation(inputs);
    Check(pager.EndPresentationMutation(control) &&
              !pager.IsPresentationContextCurrent(staleInputs.presentationEpoch),
          "new intent fences task input captured before the main mutation");
    auto staleWorking = staleInputs;
    candidate = renderer.Compose(staleWorking, presentationPort, board, status);
    Check(!candidate.ready && !pager.HasPresentationCandidate(),
          "stale private input cannot prepare or request another frame");
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(candidate.ready && !candidate.intentApplied &&
              renderer.NotePresented(candidate, presentationPort) &&
              !session.ApplyPresentationFeedback(candidate) &&
              session.SelectedFrame() == 3U && pager.DisplayedIndex() == 2U &&
              pager.RequestedIndex() == 3U && board[0].left.r == 12U,
          "uncached seek retains confirmed C without acknowledging the desired frame");
    const std::array<uint8_t, 8U> namedPaint{3U, 1U, 0U, 0U, 15U, 1U, 2U, 3U};
    Check(session.OrderedWaitsForPresentedFrame(3U, namedPaint.data(), namedPaint.size()),
          "active named paint retains its pending presentation target");
    session.SetMutationsPaused(true);
    Check(!session.OrderedWaitsForPresentedFrame(3U, namedPaint.data(), namedPaint.size()),
          "paused named paint reaches its ordinary refusal instead of waiting for pixels");
    session.SetMutationsPaused(false);
    session.ObserveOccupancy(UINT64_C(1) << 7U);
    Check(session.SelectedFrame() == 3U && pager.DisplayedIndex() == 2U &&
              session.UndoCount() == 1U,
          "physical input preserves an explicit cold seek without painting prior pixels");
    select[1U] = 2U;
    Check(session.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "returning to the confirmed frame resolves the pending seek");
    session.ObserveOccupancy(0U);
    session.ObserveButton({modules::UserButton::P1X, fxe::Button::Event::Press, modules::UiOwner::P1});
    session.ObserveButton({modules::UserButton::P1X, fxe::Button::Event::Release, modules::UiOwner::P1});
    Check(session.SelectedFrame() == 0U && pager.DisplayedIndex() == 2U &&
              !session.CanMutatePresentedFrame() && session.UndoCount() == 1U,
          "physical X retains history until its frame is actually confirmed");
    fxe::ManualMillis::AdvanceMs(5001U);
    session.Update();
    Check(session.SelectedFrame() == 2U && pager.RequestedIndex() == 2U &&
              session.UndoCount() == 1U && session.RedoCount() == 0U,
          "control-safe five-second timeout restores confirmed C without spending history");
    std::printf("[CONFIRMED_STORE_MAINTENANCE] pending_history=expired frame=2 request=2 undo=1 next=paint_or_retry\n");
    select[1U] = 3U;
    Check(session.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "exit fixture retains a cold named target");
    session.Exit();
    Check(!session.IsActive(), "streamed playback exit retires mode authority");
    Check(!session.OrderedWaitsForPresentedFrame(3U, namedPaint.data(), namedPaint.size()),
          "inactive named paint cannot block a later Enter behind a cold target");
    renderer.Reset(presentationPort);
    session.SetIndependentPresentation(false);
    std::array<uint8_t, 8U> enter{
        modules::LitchcraftSession::MAGIC,
        static_cast<uint8_t>(modules::LitchcraftSession::Op::Enter),
        0U, 0U, 0U, 0U, 91U, 0U};
    session.HandlePacket(enter.data(), enter.size(), true, 0U);
    session.Compose(board, status);
    Check(board[0].left.r == 10U && session.SelectedFrame() == 0U,
          "re-entry presents cached frame zero instead of stale frame two");
    const size_t readsBeforePrefetch = medium.reads;
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Idle &&
              medium.reads == readsBeforePrefetch,
          "re-entry needs no flash read while frame zero and one remain cached");
    session.SetIndependentPresentation(true);
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(renderer.NotePresented(candidate, presentationPort) && session.ApplyPresentationFeedback(candidate),
          "bulk journey starts from an actual confirmed frame");
    StartStorePlayback(session, frames.Generation());
    fxe::ManualMillis::AdvanceMs(25U);
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(renderer.NotePresented(candidate, presentationPort) && session.ApplyPresentationFeedback(candidate),
          "bulk journey establishes a partial authored dwell");
    const auto beforeTransfer = candidate;
    const auto frozenElapsed = session.AnimationElapsedMs();
    const auto frozenFrameElapsed = session.AnimationFrameElapsedMs();
    const auto frozenEffect = candidate.playback.effectElapsedMs;
    session.SetBulkTransferActive(true);
    Check(!session.ApplyPresentationFeedback(beforeTransfer),
          "pre-transfer renderer feedback cannot restore an older running clock");
    fxe::ManualMillis::AdvanceMs(5000U);
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(renderer.NotePresented(candidate, presentationPort) && session.ApplyPresentationFeedback(candidate) &&
              session.IsAnimationPlaying() && session.SelectedFrame() == 0U &&
              session.AnimationElapsedMs() == frozenElapsed &&
              session.AnimationFrameElapsedMs() == frozenFrameElapsed &&
              candidate.playback.effectElapsedMs == frozenEffect && board[0].left.r == 10U,
          "transfer freezes artwork frame and effect phase while preserving explicit Play intent");
    const auto suspendedFeedback = candidate;
    session.SetBulkTransferActive(false);
    Check(!session.ApplyPresentationFeedback(suspendedFeedback) &&
              !session.ApplyPresentationFeedback(beforeTransfer),
          "neither suspended nor pre-transfer feedback can cross a complete hold generation");
    fxe::ManualMillis::AdvanceMs(1U);
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(renderer.NotePresented(candidate, presentationPort) && session.ApplyPresentationFeedback(candidate) &&
              session.IsAnimationPlaying() && session.AnimationElapsedMs() == frozenElapsed + 1U,
          "transfer exit resumes remaining phase without catching up paused wall time");
    session.SetBulkTransferActive(true);
    auto pause = std::array<uint8_t, 6U>{2U, 1U};
    fxe::WriteU32LE(frames.Generation(), pause.data() + 2U);
    Check(session.ApplyConnectionOrdered(2U, pause.data(), pause.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "explicit frame selection remains available during suspension");
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(renderer.NotePresented(candidate, presentationPort) && session.ApplyPresentationFeedback(candidate) &&
              !session.IsAnimationPlaying() && session.SelectedFrame() == 1U,
          "paused selection presents its requested frame during a transfer");
    session.SetBulkTransferActive(false);
    fxe::ManualMillis::AdvanceMs(5000U);
    session.CapturePresentation(inputs);
    candidate = renderer.Compose(inputs, presentationPort, board, status);
    Check(renderer.NotePresented(candidate, presentationPort) && session.ApplyPresentationFeedback(candidate) &&
              !session.IsAnimationPlaying() && session.SelectedFrame() == 1U,
          "ending a transfer never resumes an explicitly paused or selected artwork");
    session.SetBulkTransferActive(true);
    session.Exit();
    session.SetBulkTransferActive(false);
    Check(!session.IsActive() && !session.IsAnimationPlaying(),
          "Exit remains authoritative when an outstanding transfer retires");
    renderer.Reset(presentationPort);
}

void TestConnectionTimelineControlsCommittedStoreWithoutReupload() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "ordered Store runtime fixture loads committed frame zero");
    modules::LitchcraftSession session;
    session.Configure(3U, 46U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    const uint32_t generation = frames.Generation();
    const auto command = [generation](const uint8_t action,
                                      const uint8_t frame) {
        std::array<uint8_t, 6U> body{action, frame};
        fxe::WriteU32LE(generation, body.data() + 2U);
        return body;
    };
    auto select = command(2U, 3U);
    Check(session.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
              modules::LitchcraftSession::Status::Ok &&
              session.SelectedFrame() == 3U &&
              !session.IsAnimationPlaying(),
          "ordered Timeline selects a committed Store frame");
    Check(LoadOne(pager, frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested,
          "selected Store frame is loaded through the production pager");
    session.Update();
    auto play = command(1U, 3U);
    Check(session.ApplyConnectionOrdered(2U, play.data(), play.size()) ==
              modules::LitchcraftSession::Status::Ok &&
              session.IsAnimationPlaying(),
          "ordered Timeline resumes committed Store playback");
    fxe::ManualMillis::SetNowMs(25U);
    session.Update();
    Check(session.SelectedFrame() == 3U &&
              session.AnimationFrameElapsedMs() == 25U,
          "nonzero Store Play publishes its actual frame-local time anchor");
    const auto playingLive = pager.LiveRevision();
    const auto playingWrites = medium.writes;
    const std::array<uint8_t, 5U> playingTool{0U, 15U, 0x11U, 0x22U, 0x33U};
    Check(session.ApplyConnectionOrdered(5U, playingTool.data(),
                                        playingTool.size()) ==
              modules::LitchcraftSession::Status::Ok &&
              session.IsAnimationPlaying() && session.SelectedFrame() == 3U &&
              session.AnimationFrameElapsedMs() == 25U &&
              session.ConnectionRevision() == playingLive &&
              pager.LiveRevision() == playingLive && medium.writes == playingWrites,
          "Store Tool selects without pausing or changing playing artwork");
    auto pause = command(0U, 3U);
    Check(session.ApplyConnectionOrdered(2U, pause.data(), pause.size()) ==
              modules::LitchcraftSession::Status::Ok &&
              !session.IsAnimationPlaying() &&
              session.ConnectionRevision() == playingLive,
          "ordered Timeline pauses without replacing the committed root");
    fxe::ManualMillis::SetNowMs(75U);
    session.Update();
    Check(session.AnimationFrameElapsedMs() == 25U,
          "paused Store State retains the accepted frame-local time anchor");
    auto stale = command(1U, 3U);
    stale[2U] ^= 1U;
    Check(session.ApplyConnectionOrdered(2U, stale.data(), stale.size()) ==
              modules::LitchcraftSession::Status::RevisionConflict &&
              !session.IsAnimationPlaying(),
          "stale Store generation cannot change runtime authority");
    const auto writesBeforeTool = medium.writes;
    session.ObserveOccupancy(UINT64_C(1));
    const auto liveBeforeTool = pager.LiveRevision();
    const std::array<uint8_t, 5U> tool{0U, 5U, 0x33U, 0x99U, 0x66U};
    Check(session.ApplyConnectionOrdered(5U, tool.data(), tool.size()) ==
              modules::LitchcraftSession::Status::Ok &&
              !session.IsErasing() && session.Target() == 5U &&
              session.Color().r == 0x33U && session.Color().g == 0x99U &&
              session.Color().b == 0x66U,
          "ordered Store Tool selects the physical brush color and mask");
    Check(pager.LiveRevision() == liveBeforeTool &&
              session.ConnectionRevision() == liveBeforeTool &&
              medium.writes == writesBeforeTool &&
              session.SelectedFrame() == 3U,
          "Store Tool neither repaints a resting brush nor writes artwork");
    const std::array<uint8_t, 5U> malformedTool{0U, 0U, 1U, 2U, 3U};
    Check(session.ApplyConnectionOrdered(5U, malformedTool.data(),
                                        malformedTool.size()) ==
              modules::LitchcraftSession::Status::Malformed &&
              session.ApplyConnectionOrdered(5U, nullptr, 5U) ==
                  modules::LitchcraftSession::Status::Malformed &&
              session.Target() == 5U && session.Color().r == 0x33U,
          "malformed Store Tool preserves the selected brush");
}

uint32_t OpenStoreOrderedSession(modules::ConnectionSession& connection,
                                 const uint32_t epoch) {
    namespace sessionWire = modules::connection_session_codec;
    const auto ingress = [&](const sessionWire::PacketView& packet) {
        std::array<uint8_t, 20U> bytes{};
        size_t size = 0U;
        Check(sessionWire::Codec::Encode(packet, {bytes.data(), bytes.size()}, size) ==
                  sessionWire::Error::None &&
                  connection.Ingress(epoch, bytes.data(), size) ==
                      modules::ConnectionSession::Result::Ok,
              "Store checkpoint fixture admits its minimum-MTU session packet");
        return bytes;
    };
    const auto take = [&](std::array<uint8_t, 20U>& bytes) {
        size_t size = 0U;
        sessionWire::PacketView packet{};
        Check(connection.PeekOutput(epoch, bytes.data(), bytes.size(), size) &&
                  sessionWire::Codec::Decode({bytes.data(), size}, packet, 20U) ==
                      sessionWire::Error::None,
              "Store checkpoint fixture reads authentic session output");
        connection.AcceptOutput(epoch);
        return packet;
    };
    Check(connection.Begin(epoch, UINT64_C(0x8899aabbccddeeff), 20U,
                           modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC),
          "Store checkpoint fixture begins the production connection owner");
    std::array<uint8_t, 10U> helloBody{};
    fxe::WriteU64LE(UINT64_C(0x1122334455667788), helloBody.data());
    fxe::WriteU16LE(20U, helloBody.data() + 8U);
    const auto hello = ingress(
        {{sessionWire::Kind::Control, uint8_t(sessionWire::Control::Hello), 0U, 0U},
         {helloBody.data(), helloBody.size()}});
    std::array<uint8_t, 20U> welcome{};
    std::array<uint8_t, 20U> identity{};
    const auto welcomed = take(welcome);
    (void)take(identity);
    std::array<uint8_t, 60U> transcript{};
    std::copy(hello.begin(), hello.end(), transcript.begin());
    std::copy(welcome.begin(), welcome.end(), transcript.begin() + 20U);
    std::copy(identity.begin(), identity.end(), transcript.begin() + 40U);
    std::array<uint8_t, 4U> readyBody{};
    fxe::WriteU32LE(fxe::Crc32(transcript.data(), transcript.size()), readyBody.data());
    (void)ingress(
        {{sessionWire::Kind::Control, uint8_t(sessionWire::Control::Ready),
          welcomed.header.token, 0U}, {readyBody.data(), readyBody.size()}});
    std::array<uint8_t, 20U> output{};
    (void)take(output); // Activated.
    (void)take(output); // Reverse State Open; this proof does not use State.
    std::array<uint8_t, 8U> openBody{};
    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC,
                    openBody.data());
    openBody[3U] = modules::ConnectionSession::ORDERED_CAPACITY;
    fxe::WriteU16LE(10U, openBody.data() + 4U);
    (void)ingress(
        {{sessionWire::Kind::Control, uint8_t(sessionWire::Control::Open),
          welcomed.header.token, 1U}, {openBody.data(), openBody.size()}});
    const auto opened = take(output);
    if (opened.body.Size() != 10U) return 0U;
    const auto token = fxe::ReadU32LE(opened.body.Data());
    Check(opened.header.code == uint8_t(sessionWire::Control::Opened) && token != 0U,
          "Store checkpoint fixture opens the authentic Ordered stream");
    (void)take(output); // Initial grant.
    return token;
}

void TestStoreToolPreservesCoalescedLiveEditCheckpoint() {
    namespace sessionWire = modules::connection_session_codec;
    using Window = fxe::OrderedReceiveWindow<
        modules::ConnectionSession::ORDERED_CAPACITY,
        modules::ConnectionSession::INTERACTIVE_PACKET_BYTES>;
    for (const uint8_t opcode : {3U, 4U, 9U}) {
        Medium medium;
        store::FrameStore frames(medium.Ops());
        Seed(medium, frames);
        stream::FrameStream pager;
        Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested &&
                  pager.TryPresentRequested() &&
                  pager.PaintDisplayed(0U, 5U, 15U, {61U, 62U, 63U}, false) &&
                  pager.PaintDisplayed(0U, 6U, 15U, {71U, 72U, 73U}, false),
              "coalesced checkpoint fixture creates two live edits");
        const auto savedRevision = pager.LiveRevision();
        pager.RequestImmediateSave();
        Check(pager.Service(frames, false) == stream::FrameStream::ServiceResult::Idle &&
                  !pager.HasUnpersistedEdit() && pager.LiveRevision() == savedRevision &&
                  pager.Generation() == frames.Generation() &&
                  frames.Generation() != savedRevision,
              "same-R save changes saved generation without changing live authority");
        modules::LitchcraftSession session;
        session.Configure(3U, 89U);
        session.RestoreArtwork({}, false, 0U);
        session.AttachFrameStream(&pager);
        session.Update();
        modules::ConnectionSession connection;
        constexpr uint32_t epoch = 97U;
        const auto token = OpenStoreOrderedSession(connection, epoch);
        if (token == 0U) continue;
        uint32_t sequence = 0U;
        const auto dispatch = [&](const uint8_t action, const uint8_t* body,
                                  const size_t bodySize) {
            std::array<uint8_t, 20U> bytes{};
            size_t size = 0U;
            Check(sessionWire::Codec::Encode(
                      {{sessionWire::Kind::Ordered, action, token, ++sequence},
                       {body, bodySize}}, {bytes.data(), bytes.size()}, size) ==
                      sessionWire::Error::None &&
                      connection.Ingress(epoch, bytes.data(), size) ==
                          modules::ConnectionSession::Result::Ok,
                  "Guard/edit/Tool enters the real bounded Ordered window");
            modules::ConnectionSession::OrderedCommand command{};
            Check(connection.BeginNext(command) == modules::ConnectionSession::Result::Ok &&
                      command.opcode == action && command.sequence == sequence &&
                      session.ApplyConnectionOrdered(command.opcode, command.body.Data(),
                          command.body.Size(), epoch) == modules::LitchcraftSession::Status::Ok,
                  "the production domain owner accepts Guard/edit/Tool exactly once");
            // BLE event-queue dispatch is bypassed. These are the existing
            // domain revisions supplied by that adapter to the session owner.
            const auto revision = action == 5U ? session.ConnectionRevision()
                                               : session.StoreGuardRevision();
            Check(connection.Complete(token, sequence, Window::Outcome{}, revision, 0U) ==
                      modules::ConnectionSession::Result::Ok,
                  "production ConnectionSession retains the terminal checkpoint");
        };
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(frames.Generation(), guard.data());
        fxe::WriteU32LE(savedRevision, guard.data() + 4U);
        dispatch(8U, guard.data(), guard.size());
        const std::array<uint8_t, 9U> paint{0U, 41U, 0U, 3U, 15U, 91U, 92U, 93U, 7U};
        const std::array<uint8_t, 9U> effect{0U, 1U, 0xf4U, 0x01U, 15U, 41U, 0U, 3U, 7U};
        const std::array<uint8_t, 6U> canvas{0U, 15U, 81U, 82U, 83U, 0U};
        if (opcode == 3U) dispatch(opcode, paint.data(), paint.size());
        else if (opcode == 4U) dispatch(opcode, effect.data(), effect.size());
        else dispatch(opcode, canvas.data(), canvas.size());
        const auto editRevision = pager.LiveRevision();
        const auto writes = medium.writes;
        const std::array<uint8_t, 5U> tool{0U, 5U, 21U, 22U, 23U};
        dispatch(5U, tool.data(), tool.size());
        dispatch(5U, tool.data(), tool.size());
        Check(editRevision == savedRevision + 1U && pager.LiveRevision() == editRevision &&
                  medium.writes == writes,
              "changed and repeated no-op Tool preserve the live edit and storage writes");
        std::array<uint8_t, 20U> checkpointBytes{};
        size_t checkpointSize = 0U;
        sessionWire::PacketView checkpoint{};
        Check(connection.PeekOutput(epoch, checkpointBytes.data(), checkpointBytes.size(),
                                    checkpointSize) &&
                  sessionWire::Codec::Decode({checkpointBytes.data(), checkpointSize},
                      checkpoint, 20U) == sessionWire::Error::None,
              "delayed transport reads the real coalesced checkpoint");
        const auto terminalRevision = fxe::ReadU32LE(checkpoint.body.Data());
        std::printf("[STORE_TOOL_CHECKPOINT] edit=%u savedG=%u savedR=%u liveR=%u through=%u receiptR=%u\n",
                    opcode, frames.Generation(), savedRevision, editRevision,
                    checkpoint.header.sequence, terminalRevision);
        Check(checkpoint.header.code == uint8_t(sessionWire::Control::Checkpoint) &&
                  checkpoint.header.sequence == sequence &&
                  fxe::ReadU16LE(checkpoint.body.Data() + 8U) == 0U &&
                  terminalRevision == editRevision,
              "coalesced Guard/edit/Tool Applied receipt preserves the live edit revision");
        connection.AcceptOutput(epoch);
        fxe::WriteU32LE(terminalRevision, guard.data() + 4U);
        Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok,
              "coalesced Tool receipt remains a valid Guard for the next app edit");
    }
}

void TestConnectionStoreToolDuringReservedReplacement() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "reserved Tool fixture presents committed Store artwork");
    modules::LitchcraftSession session;
    session.Configure(3U, 46U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    const auto generation = frames.Generation();
    Check(pager.ReserveReplacement(generation, frames.CurrentManifest(),
                                   generation + 1U) && pager.ReplacementPending(),
          "production owner reserves Store replacement before root publication");
    const auto live = pager.LiveRevision();
    const auto writes = medium.writes;
    const std::array<uint8_t, 5U> tool{1U, 5U, 0x33U, 0x99U, 0x66U};
    const std::array<uint8_t, 5U> malformed{0U, 0U, 1U, 2U, 3U};
    Check(session.ApplyConnectionOrdered(5U, tool.data(), tool.size()) ==
              modules::LitchcraftSession::Status::Ok && session.IsErasing() &&
              session.Target() == 5U && session.Color().g == 0x99U &&
              session.ApplyConnectionOrdered(5U, malformed.data(),
                                             malformed.size()) ==
                  modules::LitchcraftSession::Status::Malformed,
          "reserved replacement admits selection and rejects malformed Tool");
    Check(pager.ReplacementPending() && pager.Generation() == generation &&
              session.SelectedFrame() == 0U && pager.LiveRevision() == live &&
              medium.writes == writes,
          "Tool keeps replacement reservation and committed artwork unchanged");
}

void TestOrderedStorePaintIsImmediateAndPersistsOffRenderPath() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "Store paint fixture presents committed frame zero");
    modules::LitchcraftSession session;
    trace::SupportActionHistory actions;
    session.SetSupportActions(&actions);
    session.Configure(3U, 47U);
    session.RestoreArtwork({}, false, 0U);
    uint32_t storageWakes = 0U;
    session.AttachFrameStream(
        &pager,
        [](void* context, bool, uint32_t, bool) {
            ++*static_cast<uint32_t*>(context);
        },
        &storageWakes);
    session.Update();
    store::Frame beforePaint{};
    uint16_t beforeFrame = 0U;
    Check(pager.CopyDisplayed(beforePaint, beforeFrame) && beforeFrame == 0U,
          "Store Undo fixture records the displayed preimage");

    const uint32_t originalGeneration = frames.Generation();
    const std::array<uint8_t, 9U> paint{
        0U, 1U, 0U, 3U, 15U, 0x2eU, 0xffU, 0x00U, 7U};
    constexpr uint32_t connectionEpoch = 11U;
    Check(session.ApplyConnectionOrdered(
              3U, paint.data(), paint.size(), connectionEpoch) ==
              modules::LitchcraftSession::Status::RevisionConflict,
          "Store paint without a current-session guard cannot change the board");
    Check(session.ApplyConnectionOrdered(
              6U, nullptr, 0U, connectionEpoch) ==
              modules::LitchcraftSession::Status::RevisionConflict,
          "Store Undo without a current-session guard cannot pop history");
    std::array<uint8_t, 8U> guard{};
    fxe::WriteU32LE(originalGeneration, guard.data());
    fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
    Check(session.ApplyConnectionOrdered(
              8U, guard.data(), guard.size(), connectionEpoch) ==
              modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(
                  3U, paint.data(), paint.size(), connectionEpoch) ==
              modules::LitchcraftSession::Status::Ok &&
              storageWakes != 0U &&
              session.StoreGuardRevision() == pager.LiveRevision(),
          "ordered Store paint is accepted and wakes only the storage worker");
    Check(actions.Size() == 1U && actions.At(0U).kind == trace::SupportActionKind::Paint &&
              actions.At(0U).outcome == trace::SupportActionOutcome::Applied &&
              actions.At(0U).count == 1U,
          "one guarded modern Store paint gesture records one applied action; rejected guard adds none");
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    session.Compose(board, status);
    Check(board[7].left.r == 0x2eU && board[7].left.g == 0xffU &&
              board[7].left.b == 0x00U,
          "ordered Store paint reaches the production render on the next compose");
    modules::LitchcraftSession::PhysicalStoreEdit physicalEdit{};
    Check(!session.CopyPhysicalStoreEdit(physicalEdit),
          "app-originated Store paint does not echo as a physical edit");

    Check(session.UndoCount() == 1U && session.RedoCount() == 0U &&
              session.ApplyConnectionOrdered(
                  6U, nullptr, 0U, connectionEpoch) ==
                  modules::LitchcraftSession::Status::Ok &&
              session.UndoCount() == 0U && session.RedoCount() == 1U,
          "guarded Store Undo reverses one complete paint stroke");
    store::Frame afterUndo{};
    uint16_t undoFrame = 0U;
    Check(pager.CopyDisplayed(afterUndo, undoFrame) && undoFrame == 0U &&
              afterUndo.emitters[28U].color.r ==
                  beforePaint.emitters[28U].color.r &&
              afterUndo.emitters[28U].color.g ==
                  beforePaint.emitters[28U].color.g,
          "Store Undo reaches the displayed frame without object transfer");
    std::array<uint8_t, 234U> historyBatch{};
    size_t historyBatchSize = 0U;
    Check(session.PeekPhysicalStoreBatch(
              historyBatch.data(), historyBatch.size(), historyBatchSize) &&
              historyBatch[14U] != 0U && historyBatch[19U] == 0U &&
              fxe::ReadU64LE(historyBatch.data() + 24U) ==
                  (UINT64_C(1) << 7U) &&
              fxe::ReadU32LE(historyBatch.data() + historyBatchSize - 4U) ==
                  pager.LiveRevision(),
          "ordered Store Undo publishes the real inverse on the reverse stream");
    session.AcceptPhysicalStoreBatch();
    Check(session.ApplyConnectionOrdered(7U, nullptr, 0U, connectionEpoch) ==
              modules::LitchcraftSession::Status::Ok &&
              session.UndoCount() == 1U && session.RedoCount() == 0U,
          "guarded Store Redo replays the same bounded inverse patch");
    Check(session.PeekPhysicalStoreBatch(
              historyBatch.data(), historyBatch.size(), historyBatchSize) &&
              historyBatch[14U] == 1U && historyBatch[21U] == 0x2eU &&
              historyBatch[22U] == 0xffU && historyBatch[23U] == 0x00U &&
              fxe::ReadU64LE(historyBatch.data() + 24U) ==
                  (UINT64_C(1) << 7U) &&
              fxe::ReadU32LE(historyBatch.data() + 32U) == pager.LiveRevision(),
          "ordered Store Redo publishes the real forward patch on the reverse stream");
    session.AcceptPhysicalStoreBatch();
    Check(session.CopyPhysicalStoreEdit(physicalEdit),
          "ordered history advances the retained edit watermark");
    const uint32_t historyEditRevision = physicalEdit.revision;
    Check(session.ApplyConnectionOrdered(7U, nullptr, 0U, connectionEpoch) ==
              modules::LitchcraftSession::Status::NoHistory &&
              session.ApplyConnectionOrdered(6U, nullptr, 0U,
                                             connectionEpoch + 1U) ==
                  modules::LitchcraftSession::Status::RevisionConflict,
          "exhausted Redo and a different BLE epoch cannot mutate Store");
    session.Compose(board, status);
    Check(board[7].left.r == 0x2eU && board[7].left.g == 0xffU,
          "Redo restores the painted physical render");

    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Idle &&
              frames.Generation() == originalGeneration + 1U,
          "storage worker commits the coalesced displayed frame asynchronously");
    store::Frame durable{};
    Check(frames.ReadFrame(0U, durable) == store::Status::Ok &&
              durable.emitters[28U].color.r == 0x2eU &&
              durable.emitters[28U].color.g == 0xffU,
          "paint survives a Store read after the render-path acceptance");

    std::array<uint8_t, 6U> pause{0U, 0U};
    fxe::WriteU32LE(originalGeneration, pause.data() + 2U);
    Check(session.ApplyConnectionOrdered(2U, pause.data(), pause.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "same-document runtime commands accept the pre-persistence generation");
    session.Compose(board, status);
    Check(board[7].left.r == 0x2eU && session.SelectedFrame() == 0U,
          "persistence generation advance does not reset the displayed frame");

    const uint64_t physicalFootprint =
        (UINT64_C(1) << 8U) | (UINT64_C(1) << 9U);
    session.ObserveOccupancy(physicalFootprint);
    session.Compose(board, status);
    Check(board[8].left.r == session.Color().r &&
              board[8].left.g == session.Color().g &&
              board[9].left.r == session.Color().r &&
              board[9].left.g == session.Color().g,
          "one physical sample paints multiple squares on the displayed Store frame");
    const std::array<uint8_t, 9U> stalePaint{
        0U, 2U, 0U, 3U, 15U, 0xabU, 0xcdU, 0xefU, 8U};
    const auto staleResult = session.ApplyConnectionOrdered(
        3U, stalePaint.data(), stalePaint.size(), connectionEpoch);
    session.Compose(board, status);
    Check(staleResult == modules::LitchcraftSession::Status::RevisionConflict &&
              board[8].left.r == session.Color().r,
          "an unsaved physical stroke invalidates the armed app guard");
    Check(session.CopyPhysicalStoreEdit(physicalEdit) &&
              physicalEdit.revision == historyEditRevision + 2U &&
              physicalEdit.generation == frames.Generation() &&
              physicalEdit.frame == 0U && physicalEdit.square == 9U &&
              physicalEdit.mask == session.Target() &&
              physicalEdit.color.r == session.Color().r &&
              physicalEdit.color.g == session.Color().g &&
              physicalEdit.color.b == session.Color().b &&
              !physicalEdit.clearEffects,
          "physical Store paint publishes a retained edit for each square");
    std::array<uint8_t, 234U> physicalBatch{};
    size_t physicalBatchSize = 0U;
    Check(session.PeekPhysicalStoreBatch(
              physicalBatch.data(), physicalBatch.size(),
              physicalBatchSize) &&
              physicalBatchSize == 57U &&
              physicalBatch[0U] == 0x4dU && physicalBatch[1U] == 2U &&
              physicalBatch[2U] ==
                  modules::LitchcraftSession::PHYSICAL_STORE_BATCH_EVENT &&
              fxe::ReadU32LE(physicalBatch.data() + 7U) == historyEditRevision + 2U &&
              physicalBatch[13U] == 0U && physicalBatch[14U] == 2U &&
              fxe::ReadU32LE(physicalBatch.data() + 15U) ==
                  frames.Generation() &&
              physicalBatch[19U] == 0U &&
              fxe::ReadU64LE(physicalBatch.data() + 24U) ==
                  (UINT64_C(1) << 8U) &&
              fxe::ReadU64LE(physicalBatch.data() + 45U) ==
                  (UINT64_C(1) << 9U) &&
              fxe::ReadU32LE(physicalBatch.data() + 53U) ==
                  pager.LiveRevision(),
          "one reliable batch carries a distinct edit for each touched square");
    session.AcceptPhysicalStoreBatch();
    Check(!session.PeekPhysicalStoreBatch(
              physicalBatch.data(), physicalBatch.size(),
              physicalBatchSize),
          "accepted physical batch leaves no duplicate pending record");
    for (uint8_t square = 10U; square <= 26U; ++square) {
        session.ObserveOccupancy(UINT64_C(1) << square);
    }
    Check(session.PeekPhysicalStoreBatch(
              physicalBatch.data(), physicalBatch.size(),
              physicalBatchSize) &&
              physicalBatchSize == 225U &&
              physicalBatch[13U] == 1U && physicalBatch[14U] == 10U &&
              fxe::ReadU32LE(physicalBatch.data() + 7U) == historyEditRevision + 13U &&
              fxe::ReadU64LE(physicalBatch.data() + 24U) ==
                  (UINT64_C(1) << 11U),
          "bounded batch reports overflow and retains the newest complete footprints");
    session.AcceptPhysicalStoreBatch();
    Check(session.PeekPhysicalStoreBatch(
              physicalBatch.data(), physicalBatch.size(),
              physicalBatchSize) &&
              physicalBatchSize == 141U &&
              physicalBatch[13U] == 0U && physicalBatch[14U] == 6U &&
              fxe::ReadU32LE(physicalBatch.data() + 7U) == historyEditRevision + 19U,
          "overflow marker clears only after its immutable repair batch is accepted");
    session.AcceptPhysicalStoreBatch();
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Idle,
          "physical Store paint is committed by the same storage-worker path");
}

void TestOrderedStoreCanvasIsOneAtomicHistoryMutation() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    const store::Frame original = MakeDenseFrame(13U);
    store::TransactionToken token{};
    Check(frames.BeginEdit(frames.Generation(), 90U, token) == store::Status::Ok &&
              frames.ReplaceFrame(token, 0U, original) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "Canvas fixture commits RGB, effects and independent frame duration");
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "Canvas fixture presents its exact Store frame");
    modules::LitchcraftSession session;
    session.Configure(3U, 81U);
    session.RestoreArtwork({}, false, 0U);
    uint32_t wakes = 0U;
    session.AttachFrameStream(&pager,
        [](void* context, bool, uint32_t, bool) {
            ++*static_cast<uint32_t*>(context);
        }, &wakes);
    session.Update();
    constexpr uint32_t epoch = 29U;
    std::array<uint8_t, 8U> guard{};
    const auto arm = [&]() {
        fxe::WriteU32LE(frames.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        return session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch);
    };
    const std::array<uint8_t, 6U> fill{0U, 5U, 91U, 92U, 93U, 0U};
    const auto beforeRevision = pager.LiveRevision();
    const auto writes = medium.writes;
    Check(arm() == modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(9U, fill.data(), fill.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok &&
              pager.LiveRevision() == beforeRevision + 1U &&
              session.StoreGuardRevision() == beforeRevision + 1U &&
              session.UndoCount() == 1U && session.RedoCount() == 0U &&
              medium.writes == writes && wakes != 0U,
          "one Fill command changes one live revision and one Undo without flash I/O");
    store::Frame filled{};
    uint16_t index = 0U;
    Check(pager.CopyDisplayed(filled, index) && index == 0U,
          "Canvas Fill publishes the complete selected frame");
    bool fillCorrect = filled.durationMs == original.durationMs;
    for (size_t i = 0U; i < filled.emitters.size(); ++i) {
        const auto& actual = filled.emitters[i];
        const auto& prior = original.emitters[i];
        const bool targeted = (5U & (1U << (i % 4U))) != 0U;
        fillCorrect = fillCorrect &&
            actual.color.r == (targeted ? 91U : prior.color.r) &&
            actual.color.g == (targeted ? 92U : prior.color.g) &&
            actual.color.b == (targeted ? 93U : prior.color.b) &&
            actual.effect == prior.effect &&
            actual.effectDurationMs == prior.effectDurationMs;
    }
    Check(fillCorrect && !session.IsErasing() && session.Target() == 5U &&
              session.Color().r == 91U,
          "Fill respects the mask, preserves effects/durations and selects Paint");
    modules::LitchcraftSession::PhysicalStoreEdit edit{};
    std::array<uint8_t, 234U> batch{};
    size_t batchSize = 0U;
    Check(!session.CopyPhysicalStoreEdit(edit) &&
              !session.PeekPhysicalStoreBatch(batch.data(), batch.size(), batchSize),
          "known phone Canvas Fill creates no reverse per-square events");
    StartStorePlayback(session, frames.Generation());
    Check(session.ApplyConnectionOrdered(9U, fill.data(), fill.size(), epoch) ==
              modules::LitchcraftSession::Status::Ok &&
              pager.LiveRevision() == beforeRevision + 1U &&
              session.UndoCount() == 1U && !session.IsAnimationPlaying(),
          "no-op Fill pauses selected playback without changing revision/history");
    Check(session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
              modules::LitchcraftSession::Status::Ok &&
              session.UndoCount() == 0U && session.RedoCount() == 1U,
          "one Undo restores the entire masked Canvas action");
    store::Frame undone{};
    Check(pager.CopyDisplayed(undone, index) && Same(undone, original),
          "Canvas Undo restores exact RGB/effect/frame and emitter durations");
    Check(session.ApplyConnectionOrdered(7U, nullptr, 0U, epoch) ==
              modules::LitchcraftSession::Status::Ok,
          "one Redo reapplies the Canvas action");
    store::Frame redone{};
    Check(pager.CopyDisplayed(redone, index) && Same(redone, filled),
          "Canvas Redo preserves the exact masked result");
    const std::array<uint8_t, 5U> selectedErase{1U, 5U, 91U, 92U, 93U};
    Check(session.ApplyConnectionOrdered(5U, selectedErase.data(), selectedErase.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "Canvas Erase fixture retains a narrower brush and last Paint color");
    const auto eraseRevision = pager.LiveRevision();
    const std::array<uint8_t, 6U> erase{0U, 15U, 0U, 0U, 0U, 1U};
    Check(session.ApplyConnectionOrdered(9U, erase.data(), erase.size(), epoch) ==
              modules::LitchcraftSession::Status::Ok &&
              pager.LiveRevision() == eraseRevision + 1U &&
              session.UndoCount() == 2U,
          "whole-frame Erase is one revision and one additional Undo");
    store::Frame erased{};
    Check(pager.CopyDisplayed(erased, index), "Erase publishes a complete frame");
    bool erasedCorrect = erased.durationMs == original.durationMs;
    for (const auto& emitter : erased.emitters) {
        erasedCorrect = erasedCorrect && emitter.color.r == 0U &&
            emitter.color.g == 0U && emitter.color.b == 0U &&
            emitter.effect == 0U && emitter.effectDurationMs == 0U;
    }
    Check(erasedCorrect && session.IsErasing() && session.Target() == 5U &&
              session.Color().r == 91U && session.Color().g == 92U &&
              session.Color().b == 93U &&
              session.ApplyConnectionOrdered(9U, erase.data(), erase.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok &&
              pager.LiveRevision() == eraseRevision + 1U &&
              session.UndoCount() == 2U,
          "Erase clears RGB/effects/durations, preserves dwell/brush/last color, and no-op has no Undo");
    Check(session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
              modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(9U, fill.data(), fill.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok &&
              session.UndoCount() == 1U && session.RedoCount() == 1U &&
              session.ApplyConnectionOrdered(7U, nullptr, 0U, epoch) ==
                  modules::LitchcraftSession::Status::Ok,
          "a no-op Canvas action after Undo preserves the existing Redo branch");
    fxe::ManualMillis::SetNowMs(5100U);
    const auto liveBeforeSave = pager.LiveRevision();
    Check(pager.Service(frames, false) == stream::FrameStream::ServiceResult::Idle &&
              !pager.HasUnpersistedEdit() && pager.LiveRevision() == liveBeforeSave,
          "Canvas saves through the existing worker after its quiet period");
    store::FrameStore rebooted(medium.Ops());
    store::Frame durable{};
    Check(rebooted.Recover() == store::Status::Ok &&
              rebooted.ReadFrame(0U, durable) == store::Status::Ok && Same(durable, erased),
          "Canvas Erase remains exact after durable recovery");
}

void TestCanvasClosesRestingPhysicalStrokeOnlyAfterGuardAdmission() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    constexpr uint32_t epoch = 41U;
    for (const uint8_t action : {0U, 1U}) {
        fxe::ManualMillis::SetNowMs(100U);
        Medium medium;
        store::FrameStore frames(medium.Ops());
        Seed(medium, frames);
        stream::FrameStream pager;
        Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
              "resting physical Canvas fixture presents the committed frame");
        modules::LitchcraftSession session;
        session.Configure(3U, 85U);
        session.RestoreArtwork({}, false, 0U);
        session.AttachFrameStream(&pager);
        session.Update();
        store::Frame pristine{};
        uint16_t index = 0U;
        Check(pager.CopyDisplayed(pristine, index),
              "physical Canvas fixture captures the pre-brush artwork");
        session.ObserveOccupancy(UINT64_C(1) << 7U);
        store::Frame physicallyPainted{};
        Check(pager.CopyDisplayed(physicallyPainted, index) &&
                  physicallyPainted.emitters[28U].color.r == session.Color().r &&
                  session.Occupancy() == (UINT64_C(1) << 7U) &&
                  session.UndoCount() == 0U,
              "a resting Hall piece paints immediately while its stroke remains open");
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(frames.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        const uint32_t revision = pager.LiveRevision();
        const std::array<uint8_t, 6U> canvas{
            0U, 15U, static_cast<uint8_t>(action == 0U ? 91U : 0U),
            static_cast<uint8_t>(action == 0U ? 92U : 0U),
            static_cast<uint8_t>(action == 0U ? 93U : 0U), action};
        Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok &&
                  session.ApplyConnectionOrdered(9U, canvas.data(), canvas.size(), epoch) ==
                      modules::LitchcraftSession::Status::Ok &&
                  pager.LiveRevision() == revision + 1U && session.UndoCount() == 2U &&
                  session.Occupancy() == (UINT64_C(1) << 7U),
              "fresh guarded Fill/Erase finishes the resting stroke and records its own Undo");
        Check(session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
                  modules::LitchcraftSession::Status::Ok,
              "first Undo reverses only the Canvas action with the piece still resting");
        store::Frame undone{};
        Check(pager.CopyDisplayed(undone, index) && Same(undone, physicallyPainted) &&
                  session.UndoCount() == 1U && session.RedoCount() == 1U &&
                  session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
                      modules::LitchcraftSession::Status::Ok &&
                  pager.CopyDisplayed(undone, index) && Same(undone, pristine),
              "second Undo independently restores the prior physical stroke preimage");
    }
    fxe::ManualMillis::SetNowMs(100U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
          "stale resting Canvas fixture loads its frame");
    modules::LitchcraftSession session;
    session.Configure(3U, 86U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    store::Frame pristine{};
    uint16_t index = 0U;
    Check(pager.CopyDisplayed(pristine, index), "stale fixture captures pre-brush artwork");
    std::array<uint8_t, 8U> guard{};
    fxe::WriteU32LE(frames.Generation(), guard.data());
    fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
    Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) ==
              modules::LitchcraftSession::Status::Ok, "stale fixture arms before Hall paint");
    session.ObserveOccupancy(UINT64_C(1) << 7U);
    const auto fingerprint = pager.DisplayedFingerprint();
    const auto revision = pager.LiveRevision();
    const std::array<uint8_t, 6U> erase{0U, 15U, 0U, 0U, 0U, 1U};
    Check(session.ApplyConnectionOrdered(9U, erase.data(), erase.size(), epoch) ==
              modules::LitchcraftSession::Status::RevisionConflict &&
              pager.DisplayedFingerprint() == fingerprint &&
              pager.LiveRevision() == revision && session.UndoCount() == 0U,
          "stale Canvas cannot mutate or prematurely close a resting physical journal");
    session.ObserveOccupancy(0U);
    fxe::ManualMillis::SetNowMs(450U);
    session.Update();
    fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
    Check(session.UndoCount() == 1U &&
              session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
                  modules::LitchcraftSession::Status::Ok,
          "normal physical lift still completes the preserved stroke after stale Canvas rejection");
    store::Frame restored{};
    Check(pager.CopyDisplayed(restored, index) && Same(restored, pristine),
          "rejected Canvas leaves the physical stroke's complete preimage undoable");
}

void TestStoreToolsNativeCausalAdmission() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
          "Store tools fixture presents the captured source");
    modules::LitchcraftSession session;
    session.Configure(3U, 87U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    constexpr uint32_t epoch = 93U;
    const auto arm = [&]() {
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(frames.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        return session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch);
    };
    const std::array<uint8_t, 6U> rotate{0U, 15U, 0U, 0U, 0U, 2U};
    Check(arm() == modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(9U, rotate.data(), rotate.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok,
          "human Store Rotate is admitted as one atomic Canvas action");
    const std::array<uint8_t, 5U> duration{3U, 0U, 0U, 0xdcU, 5U};
    Check(arm() == modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(10U, duration.data(), duration.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok,
          "human Store duration commits through typed Ordered owner");
    const std::array<uint8_t, 5U> copy{0U, 0U, 1U, 0U, 0U};
    Check(arm() == modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(10U, copy.data(), copy.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok && pager.HasUnpersistedEdit(),
          "human Store New frame acknowledges live application before worker publication");
}

void TestStoreToolsRotationDurationAndStructuralHistory() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(10U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    store::TransactionToken seed{};
    store::Frame seeded = MakeDenseFrame(41U);
    seeded.emitters[0U].effect = 1U;
    seeded.emitters[0U].effectDurationMs = 1600U;
    Check(frames.BeginEdit(frames.Generation(), 95U, seed) == store::Status::Ok &&
              frames.ReplaceFrame(seed, 0U, seeded) == store::Status::Ok &&
              frames.Commit(seed) == store::Status::Ok, "dense tool source commits");
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
          "tools frame is presented");
    modules::LitchcraftSession session;
    session.Configure(3U, 88U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    constexpr uint32_t epoch = 94U;
    const auto arm = [&]() {
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(pager.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        return session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch);
    };
    const auto drain = [&](const uint8_t op, const uint8_t* body, const size_t size) {
        auto status = session.ApplyConnectionOrdered(op, body, size, epoch);
        for (uint8_t attempts = 0U; status == modules::LitchcraftSession::Status::Busy &&
             attempts < 12U; ++attempts) {
            (void)pager.Service(frames, false);
            session.Update();
            status = session.ApplyConnectionOrdered(op, body, size, epoch);
        }
        return status;
    };
    store::Frame original{};
    uint16_t index = 0U;
    Check(pager.CopyDisplayed(original, index), "capture full dense frame");
    const auto writes = medium.writes;
    const auto revision = pager.LiveRevision();
    const std::array<uint8_t, 6U> rotate{0U, 15U, 0U, 0U, 0U, 2U};
    store::Frame expected = original;
    for (size_t square = 0U; square < 64U; ++square) {
        const size_t to = (square % 8U) * 8U + 7U - square / 8U;
        for (size_t region = 0U; region < 4U; ++region) {
            expected.emitters[to * 4U + (region + 1U) % 4U] = original.emitters[square * 4U + region];
        }
    }
    store::Frame actual{};
    Check(arm() == modules::LitchcraftSession::Status::Ok &&
              drain(9U, rotate.data(), rotate.size()) == modules::LitchcraftSession::Status::Ok &&
              pager.CopyDisplayed(actual, index) && Same(actual, expected) &&
              pager.LiveRevision() == revision + 1U && session.UndoCount() == 1U &&
              medium.writes == writes,
          "CW rotation moves all256 RGB/effect/duration regions with one RAM revision/inverse");
    Check(drain(6U, nullptr, 0U) == modules::LitchcraftSession::Status::Ok &&
              pager.CopyDisplayed(actual, index) && Same(actual, original) &&
              drain(7U, nullptr, 0U) == modules::LitchcraftSession::Status::Ok &&
              pager.CopyDisplayed(actual, index) && Same(actual, expected),
          "rotation Undo/Redo restores exact full emitter metadata");
    const std::array<uint8_t, 5U> duration{3U, 0U, 0U, 0xdcU, 5U};
    const auto beforeDuration = pager.LiveRevision();
    Check(arm() == modules::LitchcraftSession::Status::Ok &&
              drain(10U, duration.data(), duration.size()) == modules::LitchcraftSession::Status::Ok &&
              pager.CopyDisplayed(actual, index) && actual.durationMs == 1500U &&
              pager.LiveRevision() == beforeDuration + 1U && session.UndoCount() == 2U,
          "duration commits one RAM revision and typed2byte inverse");
    expected.durationMs = 1500U;
    Check(Same(actual, expected) &&
              drain(10U, duration.data(), duration.size()) == modules::LitchcraftSession::Status::Ok &&
              pager.LiveRevision() == beforeDuration + 1U && session.UndoCount() == 2U,
          "duration no-op retains RGB/effects and history/revision");
    fxe::ManualMillis::AdvanceMs(300U);
    modules::BoardLedFrame effectBefore{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> effectStatus{};
    session.Compose(effectBefore, effectStatus);
    const std::array<uint8_t, 5U> copy{0U, 0U, 1U, 0U, 0U};
    const auto beforeCopy = pager.LiveRevision();
    const auto documentEpoch = pager.DocumentEpoch();
    Check(arm() == modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(10U, copy.data(), copy.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok &&
              pager.FrameCount() == 5U && frames.CurrentManifest().frameCount == 4U &&
              pager.LiveRevision() == beforeCopy + 1U && session.UndoCount() == 3U &&
              session.SelectedFrame() == 1U && pager.DisplayedIndex() == 1U &&
              pager.CopyDisplayed(actual, index) && Same(actual, expected) &&
              pager.HasUnpersistedEdit(),
          "copy publishes exact dirty RAM source, selection and inverse before flash");
    for (uint8_t attempt = 0U; attempt < 16U && pager.HasUnpersistedEdit(); ++attempt) {
        (void)pager.Service(frames, false);
    }
    Check(!pager.HasUnpersistedEdit() && frames.CurrentManifest().frameCount == 5U &&
              pager.LiveRevision() == beforeCopy + 1U && pager.DocumentEpoch() == documentEpoch &&
              pager.CopyDisplayed(actual, index) && Same(actual, expected),
          "copy persistence preserves the accepted RAM identity, content and revision");
    modules::BoardLedFrame effectAfter{};
    session.Compose(effectAfter, effectStatus);
    Check(effectBefore[7U].right.r == effectAfter[7U].right.r &&
              effectBefore[7U].right.g == effectAfter[7U].right.g &&
              effectBefore[7U].right.b == effectAfter[7U].right.b &&
              session.StoreLiveAuthority().generation == frames.Generation(),
          "structural publication keeps continuous Breathe clock and publishes saved generation");
    const std::array<uint8_t, 5U> remove{1U, 1U, 0U, 0U, 0U};
    Check(arm() == modules::LitchcraftSession::Status::Ok &&
              drain(10U, remove.data(), remove.size()) == modules::LitchcraftSession::Status::Ok &&
              pager.FrameCount() == 4U && session.SelectedFrame() == 0U &&
              session.UndoCount() == 4U && pager.CopyDisplayed(actual, index) && Same(actual, expected),
          "Delete publishes preceding survivor and complete removed inverse");
    Check(drain(6U, nullptr, 0U) == modules::LitchcraftSession::Status::Ok &&
              pager.FrameCount() == 5U && session.SelectedFrame() == 1U &&
              pager.CopyDisplayed(actual, index) && Same(actual, expected) &&
              drain(6U, nullptr, 0U) == modules::LitchcraftSession::Status::Ok &&
              pager.FrameCount() == 4U && session.SelectedFrame() == 0U &&
              drain(6U, nullptr, 0U) == modules::LitchcraftSession::Status::Ok &&
              pager.CopyDisplayed(actual, index) && actual.durationMs == original.durationMs &&
              drain(6U, nullptr, 0U) == modules::LitchcraftSession::Status::Ok &&
              pager.CopyDisplayed(actual, index) && Same(actual, original),
          "chronological structural Undo restores layout before duration and old indexed rotation");
    for (uint8_t step = 0U; step < 4U; ++step) {
        Check(drain(7U, nullptr, 0U) == modules::LitchcraftSession::Status::Ok,
              "same arena Redo reapplies chronological operation");
    }
    Check(pager.FrameCount() == 4U && pager.CopyDisplayed(actual, index) && Same(actual, expected) &&
              session.UndoCount() == 4U && session.RedoCount() == 0U,
          "all structural/duration/rotation Redo completes exact final layout/content");
    std::printf("[STORE_TOOLS_RESOURCES] frame_stream=%zu session=%zu history=%zu arena=%zu\n",
        sizeof(stream::FrameStream), sizeof(modules::LitchcraftSession),
        sizeof(modules::litchcraft_store_history::StoreHistory),
        modules::litchcraft_store_history::StoreHistory::ARENA_BYTES);
}

void TestStoreStructuralCancelDurableBoundary() {
    for (const bool afterCommit : {false, true}) {
        fxe::ManualMillis::SetNowMs(100U);
        Medium medium;
        store::FrameStore frames(medium.Ops());
        Seed(medium, frames);
        stream::FrameStream pager;
        Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
              "cancel fixture presents frame0");
        modules::LitchcraftSession session;
        session.Configure(3U, 89U);
        session.RestoreArtwork({}, false, 0U);
        session.AttachFrameStream(&pager);
        session.Update();
        struct Boundary final { modules::LitchcraftSession* session; bool after; bool fired = false; } boundary{&session, afterCommit};
        frames.SetTimingObserver([](void* context, store::FrameStore::TimingPhase phase, uint32_t) {
            auto& boundary = *static_cast<Boundary*>(context);
            if (boundary.fired || phase != (boundary.after ? store::FrameStore::TimingPhase::RootPublish
                                                          : store::FrameStore::TimingPhase::BodyPrepare)) return;
            boundary.fired = true;
            boundary.session->Exit();
            if (boundary.after) {
                const std::array<uint8_t, 8U> enter{0x4cU, 1U, 0U, 0U, 0U, 0U, 1U, 0U};
                boundary.session->HandlePacket(enter.data(), enter.size(), true, 0U);
            }
        }, &boundary);
        constexpr uint32_t epoch = 95U;
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(frames.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        const std::array<uint8_t, 5U> copy{0U, 0U, 1U, 0U, 0U};
        const auto generation = frames.Generation();
        Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) == modules::LitchcraftSession::Status::Ok &&
                  session.ApplyConnectionOrdered(10U, copy.data(), copy.size(), epoch) == modules::LitchcraftSession::Status::Ok,
              "copy is live before either storage departure boundary");
        (void)pager.Service(frames, false);
        session.Update();
        Check(boundary.fired && !pager.ReplacementPending() && !session.IsStoreHistoryPending(),
              "mode departure retires caller and settles internal reservation");
        if (!afterCommit) {
            Check(frames.Generation() != generation && pager.FrameCount() == 5U &&
                      session.UndoCount() == 1U && !session.IsActive(),
                  "Exit cannot roll back an acknowledged RAM copy while its save completes");
        } else {
            Check(frames.Generation() != generation && frames.CurrentManifest().frameCount == 5U &&
                      pager.FrameCount() == 5U && session.UndoCount() == 1U && session.IsActive(),
                  "postcommit new mode retains real accepted root and chronological inverse");
            Check(session.SelectedFrame() == 0U && pager.RequestedIndex() == 0U,
                  "postcommit old job cannot retarget new Enter frame0");
            for (uint8_t i = 0U; i < 4U && pager.DisplayedIndex() != 0U; ++i) {
                (void)pager.Service(frames, false); session.Update();
            }
            store::Frame actual{}; uint16_t index = 0U;
            Check(pager.CopyDisplayed(actual, index) && index == 0U && Same(actual, MakeFrame(10U)) &&
                      session.SelectedFrame() == 0U,
                  "new mode renders its requested frame0 from coherent committed layout");
        }
    }
}

void TestStoreStructuralRefusalAndFaultRecovery() {
    for (uint8_t failure = 0U; failure < 4U; ++failure) {
        Medium medium;
        store::FrameStore frames(medium.Ops());
        Seed(medium, frames);
        stream::FrameStream pager;
        Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
              "fault fixture presents exact source");
        modules::LitchcraftSession session;
        session.Configure(3U, 90U);
        session.RestoreArtwork({}, false, 0U);
        session.AttachFrameStream(&pager); session.Update();
        constexpr uint32_t epoch = 96U;
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(frames.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        const std::array<uint8_t, 5U> copy{0U, 0U, 1U, 0U, 0U};
        const auto generation = frames.Generation();
        const auto revision = pager.LiveRevision();
        Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) == modules::LitchcraftSession::Status::Ok &&
                  session.ApplyConnectionOrdered(10U, copy.data(), copy.size(), epoch) == modules::LitchcraftSession::Status::Ok &&
                  pager.FrameCount() == 5U && frames.Generation() == generation,
              "actual guarded Add is acknowledged in RAM before storage fault injection");
        if (failure == 0U) medium.failWrite = medium.writes + 1U;
        else if (failure == 1U) medium.failSync = medium.syncs + 1U;
        else medium.failRootSyncOnce = true;
        if (failure == 3U) frames.SetTimingObserver([](void* context, store::FrameStore::TimingPhase phase, uint32_t) {
            if (phase == store::FrameStore::TimingPhase::RootPublish) static_cast<Medium*>(context)->failReads = true;
        }, &medium);
        (void)pager.Service(frames, false);
        Check(pager.FrameCount() == 5U && pager.LiveRevision() == revision + 1U &&
                  session.UndoCount() == 1U && session.SelectedFrame() == 1U &&
                  !pager.ReplacementPending() && pager.HasUnpersistedEdit(),
              "write/sync/recovery failure preserves accepted RAM identity and inverse for retry");
        if (failure == 3U) {
            Check(!frames.HasArtwork(), "failed root reconciliation has no readable saved descriptor");
            store::StoreReceiver receiver(frames);
            receiver.BindLiveStream(pager);
            receiver.SetProjection({session.SessionId(), session.SelectedFrame(), pager.Topology(), true, false});
            std::array<uint8_t, 10U> inspect{0x4dU, 2U, 1U};
            fxe::WriteU32LE(session.SessionId(), inspect.data() + 3U);
            fxe::WriteU16LE(25U, inspect.data() + 7U);
            inspect[9U] = store::RESIDENT_LISTING_PROBE;
            const auto reads = medium.reads;
            const auto writes = medium.writes;
            store::StoreIngressResult pending{};
            receiver.Handle(inspect.data(), inspect.size(), epoch, pending);
            Check(pending.responseReady && pending.response[13U] == static_cast<uint8_t>(store::WireStatus::Busy) &&
                      pager.FrameCount() == 5U && pager.LiveRevision() == revision + 1U &&
                      medium.reads == reads && medium.writes == writes,
                  "reconnect inspection retries uncertain save without advertising absent artwork or losing live RAM");
        }
        medium.failReads = false;
        frames.SetTimingObserver(nullptr, nullptr);
        for (uint8_t attempt = 0U; attempt < 16U && pager.HasUnpersistedEdit(); ++attempt) {
            (void)pager.Service(frames, false);
        }
        Check(!pager.HasUnpersistedEdit() && frames.Generation() != generation &&
                  frames.CurrentManifest().frameCount == 5U && pager.FrameCount() == 5U &&
                  pager.LiveRevision() == revision + 1U && session.UndoCount() == 1U &&
                  session.SelectedFrame() == 1U,
              "uncertain publication reconciles exactly once without repeating the live edit");
        std::printf("[STORE_TOOLS_FAULT] phase=%u saved_g=%u live_r=%u count=5 undo=1 result=RAM_retained_then_saved\n",
            failure, frames.Generation(), pager.LiveRevision());
    }
    for (const bool foreignPaint : {true, false}) {
        Medium medium;
        store::FrameStore frames(medium.Ops()); Seed(medium, frames);
        stream::FrameStream pager;
        Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
              "cold structural refusal fixture loads");
        modules::LitchcraftSession session;
        session.Configure(3U, 91U); session.RestoreArtwork({}, false, 0U);
        session.AttachFrameStream(&pager); session.Update();
        constexpr uint32_t epoch = 97U;
        std::array<uint8_t, 6U> select{2U, 3U};
        fxe::WriteU32LE(pager.Generation(), select.data() + 2U);
        Check(session.ApplyConnectionOrdered(2U, select.data(), select.size()) == modules::LitchcraftSession::Status::Ok,
              "cold Delete source seeks through actual Timeline");
        for (uint8_t attempt = 0U; attempt < 8U && pager.DisplayedIndex() != 3U; ++attempt) {
            (void)LoadOne(pager, frames); session.Update();
        }
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(pager.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        const std::array<uint8_t, 5U> remove{1U, 3U, 0U, 0U, 0U};
        Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) == modules::LitchcraftSession::Status::Ok &&
                  session.ApplyConnectionOrdered(10U, remove.data(), remove.size(), epoch) == modules::LitchcraftSession::Status::Busy,
              "cold survivor preparation precedes Delete RAM application");
        if (foreignPaint) Check(pager.PaintDisplayed(3U, 7U, 1U, {12U, 34U, 56U}, false),
                               "foreign mark wins before cold preparation finishes");
        else Check(pager.Request(1U), "newer frame request wins before cold preparation finishes");
        (void)pager.Service(frames, false);
        Check(session.ApplyConnectionOrdered(10U, remove.data(), remove.size(), epoch) == modules::LitchcraftSession::Status::RevisionConflict &&
                  pager.FrameCount() == 4U && frames.CurrentManifest().frameCount == 4U &&
                  (foreignPaint || pager.RequestedIndex() == 1U),
              "foreign revision or selection cancels unaccepted cold Delete without layout mutation");
    }
}

void TestStoreToolsNoopBoundsAndLastFrameHistory() {
    Medium medium;
    store::FrameStore frames(medium.Ops()); Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested, "last frame fixture loads");
    modules::LitchcraftSession session;
    session.Configure(3U, 92U); session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager); session.Update();
    constexpr uint32_t epoch = 98U;
    const auto arm = [&]() {
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(pager.Generation(), guard.data()); fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        return session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch);
    };
    const auto drain = [&](uint8_t op, const uint8_t* body, size_t size) {
        auto status = session.ApplyConnectionOrdered(op, body, size, epoch);
        for (uint8_t i = 0U; i < 12U && status == modules::LitchcraftSession::Status::Busy; ++i) {
            (void)pager.Service(frames, false); session.Update();
            status = session.ApplyConnectionOrdered(op, body, size, epoch);
        }
        return status;
    };
    std::array<uint8_t, 6U> select{2U, 3U}; fxe::WriteU32LE(pager.Generation(), select.data() + 2U);
    Check(session.ApplyConnectionOrdered(2U, select.data(), select.size()) == modules::LitchcraftSession::Status::Ok,
          "last-frame source selected through actual Timeline");
    for (uint8_t i = 0U; i < 8U && pager.DisplayedIndex() != 3U; ++i) { (void)LoadOne(pager, frames); session.Update(); }
    const std::array<uint8_t, 5U> duration{3U, 3U, 0U, 0x20U, 3U};
    Check(arm() == modules::LitchcraftSession::Status::Ok && drain(10U, duration.data(), duration.size()) == modules::LitchcraftSession::Status::Ok,
          "last indexed duration records before structural change");
    const std::array<uint8_t, 5U> remove{1U, 3U, 0U, 0U, 0U};
    Check(drain(10U, remove.data(), remove.size()) == modules::LitchcraftSession::Status::Ok && pager.FrameCount() == 3U &&
              session.SelectedFrame() == 2U && drain(6U, nullptr, 0U) == modules::LitchcraftSession::Status::Ok &&
              pager.FrameCount() == 4U && session.SelectedFrame() == 3U &&
              drain(6U, nullptr, 0U) == modules::LitchcraftSession::Status::Ok,
          "deleted last frame restores before older indexed inverse without requesting absent index");
    store::Frame actual{}; uint16_t index = 0U;
    Check(pager.CopyDisplayed(actual, index) && index == 3U && Same(actual, MakeFrame(13U)),
          "older indexed duration restores exact last-frame RGB/effects/duration");
    const auto revision = pager.LiveRevision(); const auto undo = session.UndoCount(); const auto redo = session.RedoCount();
    const std::array<std::array<uint8_t, 5U>, 7U> bad{{
        {2U, 3U, 0U, 0U, 0U}, {0U, 3U, 3U, 0U, 0U}, {0U, 3U, 4U, 1U, 0U},
        {1U, 3U, 1U, 0U, 0U}, {1U, 3U, 0U, 1U, 0U}, {3U, 3U, 0U, 49U, 0U}, {3U, 3U, 0U, 0x89U, 0x13U}
    }};
    bool refused = true;
    for (const auto& body : bad) refused = refused &&
        session.ApplyConnectionOrdered(10U, body.data(), body.size(), epoch) == modules::LitchcraftSession::Status::Malformed;
    Check(refused && pager.LiveRevision() == revision && session.UndoCount() == undo && session.RedoCount() == redo,
          "strict5byte opcode10 validation preserves existing history and live content");

    modules::litchcraft_store_history::StoreHistory history;
    store::Frame dense = MakeDenseFrame(11U);
    for (uint8_t i = 0U; i < 4U; ++i) {
        history.Begin(i, dense);
        history.CommitStructural(modules::litchcraft_store_history::StoreHistory::Kind::Delete, i);
    }
    Check(history.UndoCount() == 2U, "4KiB arena evicts whole dense removed-frame operations");
    store::Frame restored{};
    Check(history.ReadRemovedFrame(false, restored) && Same(restored, dense) &&
              history.CommitStructuralStep(false) && history.ReadRemovedFrame(false, restored) && Same(restored, dense) &&
              history.CommitStructuralStep(false) && history.UndoCount() == 0U && history.RedoCount() == 2U,
          "newest complete dense structural inverse survives bounded eviction");
}

void TestVirtualLitchStoreToolsJourney() {
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium; store::FrameStore frames(medium.Ops()); Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested, "tools journey first frame loads");
    virtual_litch_config config{};
    config.brightness_scale_256 = 256U; config.max_led_current_ma = 12000U; config.strong_power_source = 1U;
    runtime::VirtualLitchSession board(config); board.ObserveBootHallMask(0U);
    auto& art = board.Litchcraft(); art.RestoreArtwork({}, true, 0U, nullptr, true); art.AttachFrameStream(&pager);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && art.IsActive(), "tools journey enters saved Store on empty Hall board");
    constexpr uint32_t epoch = 99U;
    const auto arm = [&]() {
        std::array<uint8_t, 8U> guard{}; fxe::WriteU32LE(pager.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        return art.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch);
    };
    const auto checkpoint = [&](const char* stage, const char* result, const char* next) {
        const auto led = board.SemanticFrame().board[7U][1U];
        std::printf("[STORE_TOOLS_JOURNEY] stage=%s t=%u occupancy=%016llx mode=%u result=%s frame=%u count=%u saved_g=%u live_r=%u led=%u,%u,%u display=\"%.*s\" next=%s expiry=none\n",
            stage, board.State().now_ms, static_cast<unsigned long long>(art.Occupancy()), art.IsActive() ? 1U : 0U,
            result, art.SelectedFrame(), art.FrameCount(), art.StoreLiveAuthority().generation,
            art.StoreLiveAuthority().revision, led.r, led.g, led.b,
            static_cast<int>(board.DisplayText().size()), board.DisplayText().data(), next);
    };
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK, "tools journey pumps initial rendered frame before rotation");
    const auto unrotated = board.SemanticFrame().board[0U][0U];
    const std::array<uint8_t, 6U> rotate{0U, 15U, 0U, 0U, 0U, 2U};
    Check(arm() == modules::LitchcraftSession::Status::Ok && art.ApplyConnectionOrdered(9U, rotate.data(), rotate.size(), epoch) == modules::LitchcraftSession::Status::Ok &&
              board.Advance(20U) == VIRTUAL_LITCH_OK,
          "mounted-equivalent Rotate produces quarter-turn semantic emitter LEDs");
    const auto rotated = board.SemanticFrame().board[7U][1U];
    modules::BoardSquareLeds projected{};
    projected.top = {unrotated.r, unrotated.g, unrotated.b};
    const auto expectedPiece = modules::RoundedPieceAverage(projected);
    Check(rotated.r == expectedPiece.r && rotated.g == expectedPiece.g && rotated.b == expectedPiece.b && rotated.r != 0U,
          "CW moves left to h1 top, using existing top/bottom legacy semantic projection");
    checkpoint("rotate", "accepted", "new_frame");
    const std::array<uint8_t, 5U> copy{0U, 0U, 1U, 0U, 0U};
    Check(arm() == modules::LitchcraftSession::Status::Ok && art.ApplyConnectionOrdered(10U, copy.data(), copy.size(), epoch) == modules::LitchcraftSession::Status::Ok && pager.HasUnpersistedEdit(),
          "new frame is applied and acknowledged before worker root publication");
    checkpoint("copy_pending", "pending", "wait_root");
    for (uint8_t attempt = 0U; attempt < 16U && pager.HasUnpersistedEdit(); ++attempt) {
        (void)pager.Service(frames, false);
    }
    Check(!pager.HasUnpersistedEdit(), "copy save converges within bounded worker turns");
    (void)pager.Service(frames, false);
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK && art.SelectedFrame() == 1U && art.FrameCount() == 5U,
          "root terminal selects copied frame and retained semantic LEDs");
    checkpoint("copy", "accepted", "delete");
    const std::array<uint8_t, 5U> remove{1U, 1U, 0U, 0U, 0U};
    Check(arm() == modules::LitchcraftSession::Status::Ok && art.ApplyConnectionOrdered(10U, remove.data(), remove.size(), epoch) == modules::LitchcraftSession::Status::Ok,
          "Delete publishes its exact preceding survivor before durable publication");
    (void)pager.Service(frames, false);
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK && art.SelectedFrame() == 0U && art.FrameCount() == 4U,
          "Delete terminal leaves same preceding frame on app/board");
    checkpoint("delete", "accepted", "undo");
    Check(art.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) == modules::LitchcraftSession::Status::Ok, "Undo restores its removed RAM frame before worker root publication");
    (void)pager.Service(frames, false);
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK && art.SelectedFrame() == 1U,
          "Undo restores deleted frame and announces real next action");
    checkpoint("undo", "accepted", "paint_or_redo");
    const std::array<uint8_t, 5U> invalid{2U, 1U, 0U, 0U, 0U};
    Check(art.ApplyConnectionOrdered(10U, invalid.data(), invalid.size(), epoch) == modules::LitchcraftSession::Status::Malformed && art.FrameCount() == 5U,
          "unsupported Move refuses without changing visible frame or retained root");
    checkpoint("invalid_move", "rejected", "paint_or_redo");
    art.Exit(); Check(board.Advance(20U) == VIRTUAL_LITCH_OK && !art.IsActive(), "journey exits and retires Guard");
    checkpoint("exit", "accepted", "setup");
}

void TestOrderedStoreCanvasRejectsStaleAndMalformedBeforeMutation() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
          "Canvas guard fixture presents one committed frame");
    modules::LitchcraftSession session;
    session.Configure(3U, 82U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    constexpr uint32_t epoch = 31U;
    const std::array<uint8_t, 6U> fill{0U, 15U, 71U, 72U, 73U, 0U};
    std::array<uint8_t, 8U> guard{};
    const auto arm = [&]() {
        fxe::WriteU32LE(frames.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        return session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch);
    };
    const auto pristine = pager.DisplayedFingerprint();
    const auto writes = medium.writes;
    Check(session.ApplyConnectionOrdered(9U, fill.data(), fill.size(), epoch) ==
              modules::LitchcraftSession::Status::RevisionConflict &&
              arm() == modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(9U, fill.data(), fill.size(), epoch + 1U) ==
                  modules::LitchcraftSession::Status::RevisionConflict,
          "Canvas requires Guard from this exact connection epoch");
    const std::array<std::array<uint8_t, 6U>, 7U> malformed{{
        {0U, 0U, 71U, 72U, 73U, 0U},
        {0U, 16U, 71U, 72U, 73U, 0U},
        {0U, 1U, 0U, 0U, 0U, 2U},
        {0U, 15U, 0U, 0U, 0U, 9U},
        {0U, 5U, 0U, 0U, 0U, 1U},
        {0U, 15U, 1U, 0U, 0U, 1U},
        {4U, 15U, 71U, 72U, 73U, 0U},
    }};
    bool invalidRejected = session.ApplyConnectionOrdered(9U, nullptr, 6U, epoch) ==
        modules::LitchcraftSession::Status::Malformed &&
        session.ApplyConnectionOrdered(9U, fill.data(), 5U, epoch) ==
            modules::LitchcraftSession::Status::Malformed;
    for (const auto& body : malformed) {
        invalidRejected = invalidRejected &&
            session.ApplyConnectionOrdered(9U, body.data(), body.size(), epoch) ==
                modules::LitchcraftSession::Status::Malformed;
    }
    Check(invalidRejected && pager.DisplayedFingerprint() == pristine &&
              session.UndoCount() == 0U && medium.writes == writes,
          "invalid Canvas enum/mask/frame/Erase/length leaves content, history and flash intact");
    const std::array<uint8_t, 6U> wrongFrame{1U, 15U, 71U, 72U, 73U, 0U};
    Check(session.ApplyConnectionOrdered(9U, wrongFrame.data(), wrongFrame.size(), epoch) ==
              modules::LitchcraftSession::Status::RevisionConflict &&
              pager.DisplayedFingerprint() == pristine && session.UndoCount() == 0U,
          "Canvas cannot retarget a different selected frame");
    session.Exit();
    const std::array<uint8_t, 8U> enter{0x4cU, 1U, 0U, 0U, 0U, 0U, 1U, 0U};
    session.HandlePacket(enter.data(), enter.size(), true, 0U);
    session.Update();
    Check(session.IsActive() &&
              session.ApplyConnectionOrdered(9U, fill.data(), fill.size(), epoch) ==
                  modules::LitchcraftSession::Status::RevisionConflict &&
              pager.DisplayedFingerprint() == pristine && session.UndoCount() == 0U,
          "Exit and re-entry retire old Canvas Guard before async mode-root persistence");
    const std::array<uint8_t, 9U> retiredPaint{0U, 51U, 0U, 3U, 15U, 71U, 72U, 73U, 3U};
    Check(session.ApplyConnectionOrdered(3U, retiredPaint.data(), retiredPaint.size(), epoch) ==
              modules::LitchcraftSession::Status::RevisionConflict &&
              pager.DisplayedFingerprint() == pristine && session.UndoCount() == 0U,
          "the same mode-session fence retires an ordinary queued Paint Guard");
    const std::array<uint8_t, 6U> emptyCheckerErase{0U, 8U, 0U, 0U, 0U, 7U};
    Check(arm() == modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(9U, emptyCheckerErase.data(),
                                             emptyCheckerErase.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok &&
              pager.DisplayedFingerprint() == pristine && session.UndoCount() == 0U,
          "a fresh Guard after re-entry admits a valid Canvas no-op");
    Check(arm() == modules::LitchcraftSession::Status::Ok &&
              pager.PaintDisplayed(0U, 4U, 1U, {41U, 42U, 43U}, false),
          "physical edit races the queued Canvas after Guard");
    const auto physical = pager.DisplayedFingerprint();
    const auto physicalRevision = pager.LiveRevision();
    Check(session.ApplyConnectionOrdered(9U, fill.data(), fill.size(), epoch) ==
              modules::LitchcraftSession::Status::RevisionConflict &&
              pager.DisplayedFingerprint() == physical &&
              pager.LiveRevision() == physicalRevision && session.UndoCount() == 0U,
          "stale Canvas rejects before any partial frame or history mutation");
    pager.RequestImmediateSave();
    Check(pager.Service(frames, false) == stream::FrameStream::ServiceResult::Idle &&
              !pager.HasUnpersistedEdit(),
          "replacement fixture durably drains the competing physical edit");
    Check(arm() == modules::LitchcraftSession::Status::Ok &&
              pager.ReserveReplacement(frames.Generation(), frames.CurrentManifest(),
                                       frames.Generation() + 1U),
          "replacement reserves the live owner after Canvas Guard");
    Check(session.ApplyConnectionOrdered(9U, fill.data(), fill.size(), epoch) ==
              modules::LitchcraftSession::Status::Busy &&
              pager.DisplayedFingerprint() == physical &&
              pager.LiveRevision() == physicalRevision && session.UndoCount() == 0U,
          "replacement fence rejects Canvas without partial mutation");
}

void TestStoreCanvasMinimumAttCheckerAndGenerationFence() {
    namespace codec = modules::connection_session_codec;
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
          "checker Canvas fixture loads the committed frame");
    modules::LitchcraftSession session;
    session.Configure(3U, 83U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    constexpr uint32_t epoch = 37U;
    std::array<uint8_t, 8U> guard{};
    fxe::WriteU32LE(frames.Generation(), guard.data());
    fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
    Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) ==
              modules::LitchcraftSession::Status::Ok,
          "checker Canvas arms the Store guard");
    const std::array<uint8_t, 6U> body{0U, 2U, 61U, 62U, 63U, 5U};
    std::array<uint8_t, 20U> packet{};
    size_t packetSize = 0U;
    Check(codec::Codec::Encode(
              {{codec::Kind::Ordered, 9U, 7U, 1U}, {body.data(), body.size()}},
              {packet.data(), packet.size()}, packetSize) == codec::Error::None &&
              packetSize == 16U,
          "six-byte Canvas body fits one minimum 20-byte ATT packet");
    const std::array<uint8_t, 16U> expected{
        0x43U, 9U, 7U, 0U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 2U, 61U, 62U, 63U, 5U};
    Check(std::equal(expected.begin(), expected.end(), packet.begin()),
          "Canvas bytes match the Dart adapter's minimum-ATT command fixture");
    codec::PacketView decoded{};
    Check(codec::Codec::Decode({packet.data(), packetSize}, decoded, 20U) ==
              codec::Error::None,
          "minimum ATT Canvas packet decodes without fragmentation");
    const auto revision = pager.LiveRevision();
    const auto tool = session.Color();
    Check(session.ApplyConnectionOrdered(decoded.header.code, decoded.body.Data(),
                                         decoded.body.Size(), epoch) ==
              modules::LitchcraftSession::Status::Ok &&
              pager.LiveRevision() == revision + 1U && session.UndoCount() == 1U,
          "one checker Fill is one atomic Canvas operation");
    store::Frame actual{};
    uint16_t frameIndex = 0U;
    Check(pager.CopyDisplayed(actual, frameIndex), "checker result is displayed");
    bool checkerCorrect = true;
    const auto prior = MakeFrame(10U);
    for (size_t index = 0U; index < actual.emitters.size(); ++index) {
        const size_t square = index / 4U;
        const bool targeted = (square / 8U + square % 8U) % 2U == 0U &&
            index % 4U == 1U;
        checkerCorrect = checkerCorrect && actual.emitters[index].color.r ==
            (targeted ? 61U : prior.emitters[index].color.r);
    }
    Check(checkerCorrect && session.Color().r == tool.r && session.Target() == 15U,
          "checker honors parity/mask and leaves the physical tool unchanged");
    std::array<uint8_t, 6U> select{2U, 1U};
    fxe::WriteU32LE(frames.Generation(), select.data() + 2U);
    Check(session.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "Timeline selection owns its requested frame before presentation");
    using Stream = stream::FrameStream;
    const auto fingerprint = pager.DisplayedFingerprint();
    Check(pager.EditDisplayedCanvasIfRevision(
              {frames.Generation(), pager.LiveRevision(), 0U, 15U,
               {81U, 82U, 83U}, Stream::CanvasAction::Fill}) ==
              Stream::GuardedEditResult::Conflict &&
              pager.DisplayedFingerprint() == fingerprint,
          "publication lock fences pending selection before Canvas history/content change");
    select[1U] = 0U;
    Check(session.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "Canvas fixture returns to the displayed frame");
    session.Update();
    Check(pager.EditDisplayedCanvasIfRevision(
              {frames.Generation() + 1U, pager.LiveRevision(), 0U, 15U,
               {81U, 82U, 83U}, Stream::CanvasAction::Fill}) ==
              Stream::GuardedEditResult::Conflict &&
              pager.DisplayedFingerprint() == fingerprint,
          "generation fence runs under publication lock before Canvas changes");
}

void TestColdPlaybackHallStrokePinsCanvasAndPreservesExplicitSeekFences() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    constexpr uint32_t epoch = 43U;
    for (const uint8_t action : {0U, 1U}) {
        fxe::ManualMillis::SetNowMs(0U);
        Medium medium;
        store::FrameStore frames(medium.Ops());
        Seed(medium, frames);
        stream::FrameStream pager;
        Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
              "cold Hall Canvas fixture has a displayed frame without prefetched successor");
        modules::LitchcraftSession session;
        session.Configure(3U, 87U);
        session.RestoreArtwork({}, false, 0U);
        session.AttachFrameStream(&pager);
        session.Update();
        StartStorePlayback(session, frames.Generation());
        fxe::ManualMillis::SetNowMs(110U);
        session.Update();
        Check(pager.RequestedIndex() == 1U && pager.DisplayedIndex() == 0U &&
                  session.SelectedFrame() == 0U && session.IsAnimationPlaying(),
              "playback reaches a cold successor while the prior frame remains visible");
        session.ObserveOccupancy(UINT64_C(1) << 7U);
        Check(!session.IsAnimationPlaying() && session.SelectedFrame() == 0U &&
                  pager.DisplayedIndex() == 0U && pager.RequestedIndex() == 0U,
              "Hall brush pause pins its displayed frame and cancels only pending playback");
        store::Frame painted{};
        uint16_t index = 0U;
        Check(pager.CopyDisplayed(painted, index) &&
                  painted.emitters[28U].color.r == session.Color().r,
              "cold playback Hall input paints the latched visible frame");
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(frames.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        const auto revision = pager.LiveRevision();
        const std::array<uint8_t, 6U> canvas{
            0U, 15U, static_cast<uint8_t>(action == 0U ? 91U : 0U),
            static_cast<uint8_t>(action == 0U ? 92U : 0U),
            static_cast<uint8_t>(action == 0U ? 93U : 0U), action};
        Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok &&
                  session.ApplyConnectionOrdered(9U, canvas.data(), canvas.size(), epoch) ==
                      modules::LitchcraftSession::Status::Ok &&
                  pager.LiveRevision() == revision + 1U && session.UndoCount() == 2U,
              "fresh guarded Fill/Erase succeeds on the paused cold-playback Hall frame");
        modules::BoardLedFrame board{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
        session.Compose(board, status);
        Check(board[7U].left.r == (action == 0U ? 91U : 0U) &&
                  session.SelectedFrame() == 0U && pager.RequestedIndex() == 0U,
              "accepted Canvas renders the actual selected frame on the next composition");
        Check(session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
                  modules::LitchcraftSession::Status::Ok,
              "one Undo reverses the cold-playback Canvas action");
        store::Frame restored{};
        Check(pager.CopyDisplayed(restored, index) && Same(restored, painted) &&
                  session.UndoCount() == 1U && session.RedoCount() == 1U,
              "cold-playback Canvas Undo preserves the preceding physical stroke exactly");
        session.ObserveOccupancy(0U);
        session.Update();
        Check(session.AnimationFrameElapsedMs() == 109U &&
                  session.SelectedFrame() == 0U && pager.DisplayedIndex() == 0U,
              "released Hall stroke retains the expired displayed frame's frozen phase");
        fxe::ManualMillis::SetNowMs(150U);
        std::array<uint8_t, 6U> play{1U, 0U};
        fxe::WriteU32LE(frames.Generation(), play.data() + 2U);
        Check(session.ApplyConnectionOrdered(2U, play.data(), play.size()) ==
                  modules::LitchcraftSession::Status::Ok,
              "released Hall stroke resumes its unchanged painted frame");
        session.Compose(board, status);
        std::printf("[LATE_FRAME_HALL_RESUME] canvas=%u at=150 selected=%u displayed=%u requested=%u elapsed=%u painted_red=%u\n",
                    action, session.SelectedFrame(), pager.DisplayedIndex(),
                    pager.RequestedIndex(), session.AnimationFrameElapsedMs(),
                    board[7U].left.r);
        Check(session.IsAnimationPlaying() && session.SelectedFrame() == 0U &&
                  pager.DisplayedIndex() == 0U && pager.RequestedIndex() == 1U &&
                  session.AnimationFrameElapsedMs() == 109U &&
                  board[7U].left.r == painted.emitters[28U].color.r,
              "Hall cancellation consumes no new dwell and preserves paint while the successor loads");
        fxe::ManualMillis::SetNowMs(175U);
        Check(LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
              "resumed Hall stroke loads the exact next authored frame");
        session.Compose(board, status);
        Check(session.SelectedFrame() == 1U && pager.DisplayedIndex() == 1U &&
                  board[0].left.r == 11U &&
                  session.AnimationFrameElapsedMs() == 25U,
              "Hall-resumed frame uses the resumed boundary rather than its late completion time");
        fxe::ManualMillis::SetNowMs(261U);
        session.Compose(board, status);
        Check(session.SelectedFrame() == 1U && pager.RequestedIndex() == 2U &&
                  session.AnimationFrameElapsedMs() == 110U,
              "Hall-resumed playback reaches the next authored boundary exactly");
    }
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
          "explicit seek fence fixture has a cold selected target");
    modules::LitchcraftSession session;
    session.Configure(3U, 88U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    std::array<uint8_t, 6U> select{2U, 1U};
    fxe::WriteU32LE(frames.Generation(), select.data() + 2U);
    Check(session.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "explicit Timeline seek selects the cold frame before Hall input");
    const auto fingerprint = pager.DisplayedFingerprint();
    session.ObserveOccupancy(UINT64_C(1) << 7U);
    Check(session.SelectedFrame() == 1U && pager.RequestedIndex() == 1U &&
              pager.DisplayedIndex() == 0U && pager.DisplayedFingerprint() == fingerprint &&
              session.UndoCount() == 0U,
          "Hall input preserves explicit cold seek and cannot paint the prior frame");
    Check(pager.ReserveReplacement(frames.Generation(), frames.CurrentManifest(),
                                   frames.Generation() + 1U),
          "replacement reserves the clean explicit-seek fixture");
    session.ObserveOccupancy(UINT64_C(1) << 8U);
    Check(pager.ReplacementPending() && session.SelectedFrame() == 1U &&
              pager.RequestedIndex() == 1U && pager.DisplayedFingerprint() == fingerprint &&
              session.UndoCount() == 0U,
          "replacement reservation rejects physical stroke admission before retargeting");
}

void TestShakeErasePinsDisplayedFrameAcrossPendingPlaybackRead() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    for (const bool pausedBeforeShake : {false, true}) {
        fxe::ManualMillis::SetNowMs(0U);
        Medium medium;
        store::FrameStore frames(medium.Ops());
        Seed(medium, frames);
        stream::FrameStream pager;
        Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
              "physical erase fixture deliberately has no prefetched successor");
        modules::LitchcraftSession session;
        session.Configure(3U, 84U);
        session.RestoreArtwork({}, false, 0U);
        session.AttachFrameStream(&pager);
        session.Update();
        session.ConfigureShakeErase(true);
        StartStorePlayback(session, frames.Generation());
        fxe::ManualMillis::SetNowMs(110U);
        session.Update();
        Check(pager.RequestedIndex() == 1U && pager.DisplayedIndex() == 0U &&
                  session.SelectedFrame() == 0U && session.IsAnimationPlaying(),
              "physical erase arrives while the displayed frame awaits its successor");
        std::array<uint8_t, 6U> runtime{0U, 0U};
        fxe::WriteU32LE(frames.Generation(), runtime.data() + 2U);
        if (pausedBeforeShake) {
            Check(session.ApplyConnectionOrdered(2U, runtime.data(), runtime.size()) ==
                      modules::LitchcraftSession::Status::Ok &&
                      !session.IsAnimationPlaying() &&
                      session.AnimationFrameElapsedMs() == 109U,
                  "physical erase starts with a genuinely paused pending successor");
        }
        const auto revision = pager.LiveRevision();
        session.ObserveMotionSample(0, 0, 1000);
        for (uint8_t sample = 0U; sample < 27U; ++sample) {
            fxe::ManualMillis::SetNowMs(110U + sample * 40U);
            const int32_t direction = sample % 2U == 0U ? 700 : -700;
            session.ObserveMotionSample(direction, -direction / 2, 1000);
        }
        store::Frame cleared{};
        uint16_t index = 0U;
        Check(session.LastShakeEraseToken() != 0U && session.UndoCount() == 1U &&
                  pager.LiveRevision() == revision + 1U &&
                  pager.RequestedIndex() == 0U && !session.IsAnimationPlaying() &&
                  pager.CopyDisplayed(cleared, index) && index == 0U &&
                  cleared.emitters[0U].color.r == 0U,
              "physical erase cancels pending playback, clears the displayed frame, and keeps one Undo");
        if (!pausedBeforeShake) continue;
        Check(session.AnimationFrameElapsedMs() == 109U,
              "Shake pin preserves the already-paused displayed elapsed value");
        fxe::ManualMillis::SetNowMs(1750U);
        runtime[0U] = 1U;
        Check(session.ApplyConnectionOrdered(2U, runtime.data(), runtime.size()) ==
                  modules::LitchcraftSession::Status::Ok,
              "physical erase resumes through the ordinary ordered Play owner");
        modules::BoardLedFrame board{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
        session.Compose(board, status);
        Check(session.SelectedFrame() == 0U && pager.RequestedIndex() == 1U &&
                  board[0].left.r == 0U && session.AnimationFrameElapsedMs() == 109U,
              "Shake-resumed playback preserves the cleared frame while its successor loads");
        fxe::ManualMillis::SetNowMs(1775U);
        Check(LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
              "Shake-resumed playback loads its exact authored successor");
        session.Compose(board, status);
        std::printf("[LATE_FRAME_SHAKE_RESUME] at=1775 selected=%u displayed=%u requested=%u elapsed=%u red=%u\n",
                    session.SelectedFrame(), pager.DisplayedIndex(),
                    pager.RequestedIndex(), session.AnimationFrameElapsedMs(),
                    board[0].left.r);
        store::Frame resumed{};
        Check(session.SelectedFrame() == 1U && pager.DisplayedIndex() == 1U &&
                  pager.RequestedIndex() == 1U &&
                  pager.CopyDisplayed(resumed, index) && index == 1U &&
                  resumed.emitters[0U].color.r == 11U &&
                  session.AnimationFrameElapsedMs() == 25U,
              "Shake-resumed successor inherits the resumed boundary rather than the motion or load time");
        Check(board[0].left.r == 0U,
              "resumed playback preserves the in-progress one-second physical erase cue");
        fxe::ManualMillis::SetNowMs(1860U);
        session.Compose(board, status);
        Check(pager.RequestedIndex() == 1U &&
                  session.AnimationFrameElapsedMs() == 110U,
              "Shake-resumed successor keeps its full remaining dwell before the boundary");
        fxe::ManualMillis::SetNowMs(1861U);
        session.Compose(board, status);
        Check(session.SelectedFrame() == 1U && pager.RequestedIndex() == 2U &&
                  session.AnimationFrameElapsedMs() == 110U,
              "Shake-resumed successor requests the next frame at its exact authored boundary");
    }
}

void TestStoreTwoSquareStrokeKeepsExactGuardAndOneUndo() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "two-square stroke fixture loads the Store frame");
    modules::LitchcraftSession session;
    session.Configure(3U, 47U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    store::Frame before{};
    uint16_t displayedIndex = 0U;
    Check(pager.CopyDisplayed(before, displayedIndex) && displayedIndex == 0U,
          "two-square stroke fixture captures the preimage");

    constexpr uint32_t connectionEpoch = 19U;
    const uint32_t baseRevision = pager.LiveRevision();
    std::array<uint8_t, 8U> guard{};
    fxe::WriteU32LE(frames.Generation(), guard.data());
    fxe::WriteU32LE(baseRevision, guard.data() + 4U);
    const std::array<uint8_t, 10U> paint{
        0U, 42U, 0U, 1U, 15U, 0x31U, 0x32U, 0x33U, 12U, 13U};
    const std::array<uint8_t, 10U> effect{
        0U, 1U, 0xf4U, 0x01U, 15U, 42U, 0U, 2U, 12U, 13U};
    Check(session.ApplyConnectionOrdered(
              8U, guard.data(), guard.size(), connectionEpoch) ==
              modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(
                  3U, paint.data(), paint.size(), connectionEpoch) ==
                  modules::LitchcraftSession::Status::Ok &&
              pager.LiveRevision() == baseRevision + 2U &&
              session.StoreGuardRevision() == baseRevision + 2U,
          "two-square Paint packet returns its exact terminal Guard revision");
    Check(session.ApplyConnectionOrdered(
              4U, effect.data(), effect.size(), connectionEpoch) ==
              modules::LitchcraftSession::Status::Ok &&
              pager.LiveRevision() == baseRevision + 4U &&
              session.StoreGuardRevision() == baseRevision + 4U &&
              session.UndoCount() == 1U,
          "two-square Effect packet advances twice within one stroke Undo boundary");

    const std::array<uint8_t, 10U> invalidPaint{
        0U, 43U, 0U, 3U, 15U, 1U, 2U, 3U, 14U, 64U};
    Check(session.ApplyConnectionOrdered(
              3U, invalidPaint.data(), invalidPaint.size(), connectionEpoch) ==
              modules::LitchcraftSession::Status::Malformed &&
              pager.LiveRevision() == baseRevision + 4U,
          "invalid second square rejects the packet before its first square changes");
    Check(pager.PaintDisplayed(0U, 14U, 15U, {0x71U, 0x72U, 0x73U}, false),
          "concurrent physical paint advances authority after app acceptance");
    const std::array<uint8_t, 9U> stalePaint{
        0U, 43U, 0U, 3U, 15U, 1U, 2U, 3U, 15U};
    Check(session.StoreGuardRevision() == baseRevision + 4U &&
              pager.LiveRevision() == baseRevision + 5U &&
              session.ApplyConnectionOrdered(
                  3U, stalePaint.data(), stalePaint.size(), connectionEpoch) ==
                  modules::LitchcraftSession::Status::RevisionConflict &&
              session.StoreGuardRevision() == 0U,
          "physical edit after packet cannot be mistaken for its Guard result");
    store::Frame rejected{};
    Check(pager.CopyDisplayed(rejected, displayedIndex) &&
              rejected.emitters[60U].color.r == before.emitters[60U].color.r,
          "stale packet leaves its target square unchanged");

    fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
    Check(session.ApplyConnectionOrdered(
              8U, guard.data(), guard.size(), connectionEpoch) ==
              modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(
                  6U, nullptr, 0U, connectionEpoch) ==
                  modules::LitchcraftSession::Status::Ok &&
              session.UndoCount() == 0U && session.RedoCount() == 1U,
          "one guarded Undo reverses both packets in the completed stroke");
    store::Frame undone{};
    Check(pager.CopyDisplayed(undone, displayedIndex) &&
              undone.emitters[48U].color.r == before.emitters[48U].color.r &&
              undone.emitters[52U].color.r == before.emitters[52U].color.r &&
              undone.emitters[48U].effect == before.emitters[48U].effect &&
              undone.emitters[52U].effect == before.emitters[52U].effect &&
              undone.emitters[56U].color.r == 0x71U,
          "Undo restores both app squares without discarding later physical paint");
}

void TestLiveRevisionRejectsUnpublishedPhysicalEdit() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "revision guard fixture presents the committed frame");
    const uint32_t commonRevision = pager.LiveRevision();
    const auto commonAuthority = pager.ReadLiveAuthority();
    Check(commonAuthority.generation == frames.Generation() &&
              commonAuthority.revision == commonRevision,
          "live authority samples generation and revision together");
    Check(commonRevision != 0U &&
              pager.PaintDisplayed(0U, 7U, 15U, {0x76U, 0x54U, 0x32U}, false),
          "physical paint becomes visible before Store publication");
    Check(pager.HasUnpersistedEdit() &&
              pager.LiveRevision() != commonRevision &&
              pager.ReadLiveAuthority().dirty,
          "visible physical paint advances live authority before save");
    Check(pager.PaintDisplayedIfRevision(
              commonRevision, 0U, 7U, 15U, {0xabU, 0xcdU, 0xefU}, false) ==
              stream::FrameStream::GuardedEditResult::Conflict,
          "stale app paint is rejected against the live frame");
    store::Frame displayed{};
    uint16_t displayedIndex = store::FRAME_COUNT;
    Check(pager.CopyDisplayed(displayed, displayedIndex) &&
              displayed.emitters[28U].color.r == 0x76U,
          "rejected app paint cannot overwrite the physical color");
    const uint32_t physicalRevision = pager.LiveRevision();
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Idle &&
              pager.LiveRevision() == physicalRevision &&
              !pager.ReadLiveAuthority().dirty,
          "durable publication does not invent a second live edit");
    Check(pager.PaintDisplayedIfRevision(
              commonRevision, 0U, 7U, 15U, {0xabU, 0xcdU, 0xefU}, false) ==
              stream::FrameStream::GuardedEditResult::Conflict,
          "old app base remains stale after the physical edit is saved");
    const uint8_t validSquares[] = {7U, 8U};
    const uint8_t invalidSquares[] = {7U, 64U};
    Check(pager.PaintDisplayedBatchIfRevision(
              physicalRevision, 0U, invalidSquares, 2U, 15U,
              {0xabU, 0xcdU, 0xefU}, false) ==
              stream::FrameStream::GuardedEditResult::Conflict &&
              pager.LiveRevision() == physicalRevision,
          "invalid second square rejects the whole packet before mutation");
    Check(pager.CopyDisplayed(displayed, displayedIndex) &&
              displayed.emitters[28U].color.r == 0x76U,
          "invalid second square leaves the first square unchanged");
    uint32_t paintResultRevision = 0U;
    Check(pager.PaintDisplayedBatchIfRevision(
              physicalRevision, 0U, validSquares, 2U, 15U,
              {0xabU, 0xcdU, 0xefU}, false, &paintResultRevision) ==
              stream::FrameStream::GuardedEditResult::Changed &&
              pager.LiveRevision() == physicalRevision + 2U &&
              paintResultRevision == physicalRevision + 2U,
          "two changed paint squares publish two live edits atomically");
    Check(pager.CopyDisplayed(displayed, displayedIndex) &&
              displayed.emitters[28U].color.r == 0xabU &&
              displayed.emitters[32U].color.r == 0xabU,
          "both squares become visible together");
    const uint32_t afterPaint = pager.LiveRevision();
    uint32_t unchangedResultRevision = 0U;
    Check(pager.PaintDisplayedBatchIfRevision(
              afterPaint, 0U, validSquares, 2U, 15U,
              {0xabU, 0xcdU, 0xefU}, false,
              &unchangedResultRevision) ==
              stream::FrameStream::GuardedEditResult::Unchanged &&
              pager.LiveRevision() == afterPaint &&
              unchangedResultRevision == afterPaint,
          "identical guarded paint does not advance authority");
    uint32_t effectResultRevision = 0U;
    Check(pager.ApplyDisplayedEffectBatchIfRevision(
              afterPaint, 0U, validSquares, 2U, 15U, 1U, 500U,
              &effectResultRevision) ==
              stream::FrameStream::GuardedEditResult::Changed &&
              pager.LiveRevision() == afterPaint + 2U &&
              effectResultRevision == afterPaint + 2U,
          "two changed effect squares publish two live edits atomically");
    const uint32_t afterEffect = pager.LiveRevision();
    unchangedResultRevision = 0U;
    Check(pager.ApplyDisplayedEffectBatchIfRevision(
              afterEffect, 0U, validSquares, 2U, 15U, 1U, 500U,
              &unchangedResultRevision) ==
              stream::FrameStream::GuardedEditResult::Unchanged &&
              pager.LiveRevision() == afterEffect &&
              unchangedResultRevision == afterEffect,
          "identical guarded effect does not advance authority");
    const uint8_t repeatedSquare[] = {9U, 9U};
    Check(pager.PaintDisplayedBatchIfRevision(
              afterEffect, 0U, repeatedSquare, 2U, 15U,
              {0x12U, 0x34U, 0x56U}, false) ==
              stream::FrameStream::GuardedEditResult::Changed &&
              pager.LiveRevision() == afterEffect + 1U,
          "duplicate changed square advances paint authority only once");
    const uint32_t afterRepeatedPaint = pager.LiveRevision();
    Check(pager.PaintDisplayedBatchIfRevision(
              afterRepeatedPaint, 0U, repeatedSquare, 2U, 15U,
              {0x12U, 0x34U, 0x56U}, false) ==
              stream::FrameStream::GuardedEditResult::Unchanged &&
              pager.LiveRevision() == afterRepeatedPaint,
          "duplicate no-op paint leaves authority unchanged");
    Check(pager.ApplyDisplayedEffectBatchIfRevision(
              afterRepeatedPaint, 0U, repeatedSquare, 2U, 15U, 2U, 700U) ==
              stream::FrameStream::GuardedEditResult::Changed &&
              pager.LiveRevision() == afterRepeatedPaint + 1U,
          "duplicate changed square advances effect authority only once");
    const uint32_t afterRepeatedEffect = pager.LiveRevision();
    Check(pager.ApplyDisplayedEffectBatchIfRevision(
              afterRepeatedEffect, 0U, repeatedSquare, 2U, 15U, 2U, 700U) ==
              stream::FrameStream::GuardedEditResult::Unchanged &&
              pager.LiveRevision() == afterRepeatedEffect,
          "duplicate no-op effect leaves authority unchanged");
    const uint8_t mixedSquares[] = {9U, 10U};
    Check(pager.PaintDisplayedBatchIfRevision(
              afterRepeatedEffect, 0U, mixedSquares, 2U, 15U,
              {0x12U, 0x34U, 0x56U}, false) ==
              stream::FrameStream::GuardedEditResult::Changed &&
              pager.LiveRevision() == afterRepeatedEffect + 1U,
          "one unchanged and one changed paint square advance only once");
    const uint32_t afterMixedPaint = pager.LiveRevision();
    Check(pager.ApplyDisplayedEffectBatchIfRevision(
              afterMixedPaint, 0U, mixedSquares, 2U, 15U, 2U, 700U) ==
              stream::FrameStream::GuardedEditResult::Changed &&
              pager.LiveRevision() == afterMixedPaint + 1U,
          "one unchanged and one changed effect square advance only once");
    const uint32_t afterMixedEffect = pager.LiveRevision();
    Check(pager.PaintDisplayedIfRevision(
              afterMixedEffect, 1U, 7U, 15U, {1U, 2U, 3U}, false) ==
              stream::FrameStream::GuardedEditResult::Conflict &&
              pager.LiveRevision() == afterMixedEffect,
          "unavailable display frame rejects rather than reports no-op");
    const uint32_t beforeClear = pager.LiveRevision();
    pager.Clear();
    Check(pager.ReadLiveAuthority().generation == 0U &&
              pager.LiveRevision() != beforeClear,
          "mode exit retires the live edit token");
}

void TestLiveRevisionRolloverFailsClosed() {
    stream::FrameStream pager;
    store::Manifest manifest{};
    manifest.frameCount = 1U;
    manifest.active = true;
    const store::Frame frame = MakeFrame(1U);
    Check(pager.StageFirstFrame(manifest, UINT32_MAX - 1U, frame) &&
              pager.LiveRevision() == UINT32_MAX - 1U,
          "rollover fixture starts at the last two live revisions");
    Check(pager.PaintDisplayed(0U, 0U, 1U, {2U, 3U, 4U}, false) &&
              pager.LiveRevision() == UINT32_MAX &&
              pager.PaintDisplayed(0U, 0U, 1U, {5U, 6U, 7U}, false) &&
              pager.LiveRevision() == 0U,
          "exhausted revision becomes invalid instead of reusing an old token");
    Check(pager.PaintDisplayedIfRevision(
              UINT32_MAX - 1U, 0U, 0U, 1U, {8U, 9U, 10U}, false) ==
              stream::FrameStream::GuardedEditResult::Conflict,
          "stale wire base cannot pass after revision exhaustion");
    pager.Clear();
    Check(pager.StageFirstFrame(manifest, UINT32_MAX - 1U, frame) &&
              pager.LiveRevision() == 0U,
          "a same-boot reload cannot reissue an exhausted token");
}

void TestInitialClearDoesNotExhaustLiveRevision() {
    stream::FrameStream pager;
    pager.Clear();
    pager.Clear();
    store::Manifest manifest{};
    manifest.frameCount = 1U;
    manifest.active = true;
    const store::Frame frame = MakeFrame(1U);
    Check(pager.StageFirstFrame(manifest, 7U, frame) &&
              pager.ReadLiveAuthority().generation == 7U &&
              pager.LiveRevision() == 7U,
          "empty-Store startup clears cannot invalidate the first artwork token");
}

void TestRestoredStoreFirstBrushAndHeldColorPick() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    store::Manifest manifest{};
    manifest.active = true;
    store::Frame frame{};
    constexpr uint8_t sourceSquare = 4U;
    constexpr uint8_t destinationSquare = 5U;
    const modules::Rgb sampled{23U, 67U, 101U};
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        frame.emitters[sourceSquare * 4U + slot].color = sampled;
    }
    stream::FrameStream pager;
    Check(pager.StageFirstFrame(manifest, 9U, frame),
          "boot fixture presents the saved Store frame");
    pager.RequestImmediateSave();
    modules::LitchcraftSession session;
    session.Configure(3U, 52U);
    session.RestoreArtwork({}, true, 0U, nullptr, true);
    session.AttachFrameStream(&pager);
    session.Update();

    // Boot sampled an empty board. Its first Hall change is a new brush,
    // never an occupancy baseline that silently consumes the gesture.
    session.ObserveOccupancy(UINT64_C(1) << sourceSquare);
    std::array<store::Emitter, 4U> shown{};
    uint16_t index = store::FRAME_COUNT;
    uint16_t duration = 0U;
    Check(pager.CopyDisplayedSquare(sourceSquare, shown, index, duration) &&
              shown[0].color.r == 255U && shown[0].color.g == 116U,
          "first post-restore C8 placement paints immediately");
    Check(!pager.EditSaveReady(fxe::ManualMillis::NowMs()),
          "a clean lifecycle request cannot make the next brush save immediately");

    fxe::ManualMillis::AdvanceMs(
        modules::LitchcraftSession::PICK_ARM_MS +
        modules::LitchcraftSession::PICK_PROGRESS_MS);
    session.Update();
    const auto selected = session.Color();
    Check(selected.r == sampled.r && selected.g == sampled.g &&
              selected.b == sampled.b,
          "long hold samples the pre-touch Store color");
    Check(pager.CopyDisplayedSquare(sourceSquare, shown, index, duration) &&
              shown[0].color.r == sampled.r &&
              shown[0].color.g == sampled.g &&
              shown[0].color.b == sampled.b,
          "completed pick restores the touched Store square");
    std::array<uint8_t, 128U> physicalBatch{};
    size_t batchSize = 0U;
    Check(session.PeekPhysicalStoreBatch(
              physicalBatch.data(), physicalBatch.size(), batchSize) &&
              physicalBatch[13U] == 0U && physicalBatch[14U] == 2U,
          "physical reverse stream carries brush and color-pick restoration");
    session.ObserveOccupancy(0U);
    session.ObserveOccupancy(UINT64_C(1) << destinationSquare);
    Check(pager.CopyDisplayedSquare(destinationSquare, shown, index, duration) &&
              shown[0].color.r == sampled.r &&
              shown[0].color.g == sampled.g &&
              shown[0].color.b == sampled.b,
          "next physical placement paints with the sampled color immediately");
    const uint32_t lastEditAtMs = fxe::ManualMillis::NowMs();
    Check(!pager.EditSaveReady(lastEditAtMs +
                                   stream::FrameStream::EDIT_SAVE_QUIET_MS - 1U) &&
              pager.EditSaveReady(lastEditAtMs +
                                  stream::FrameStream::EDIT_SAVE_QUIET_MS),
          "physical edits coalesce until five quiet seconds without hiding RAM paint");
    pager.RequestImmediateSave();
    Check(pager.EditSaveReady(lastEditAtMs),
          "explicit lifecycle flush bypasses the trailing save delay");
}

void TestStoreReplacementCancelsHeldColorPick() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    store::Manifest manifest{};
    manifest.active = true;
    store::Frame original{};
    constexpr uint8_t square = 4U;
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        original.emitters[square * 4U + slot].color = {255U, 116U, 48U};
    }
    stream::FrameStream pager;
    Check(pager.StageFirstFrame(manifest, 9U, original),
          "replacement fixture presents a no-op orange source square");
    modules::LitchcraftSession session;
    session.Configure(3U, 52U);
    session.RestoreArtwork({}, true, 0U, nullptr, true);
    session.AttachFrameStream(&pager);
    session.Update();
    session.BeginConnection();
    session.ObserveOccupancy(UINT64_C(1) << square);
    Check(!pager.HasUnpersistedEdit(),
          "matching brush arms color pick without a pending Store write");
    modules::LitchcraftSession::Packet packet{};
    bool sawPicking = false;
    while (session.PeekPacket(packet)) {
        if (packet.bytes[1] ==
                static_cast<uint8_t>(modules::LitchcraftSession::Event::ColorPick)) {
            sawPicking = packet.bytes[12U] == 1U;
        }
        session.AcceptPacket();
    }
    Check(sawPicking, "connected app observes the pending Store color pick");
    store::Frame replacement{};
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        replacement.emitters[square * 4U + slot].color = {2U, 33U, 77U};
    }
    Check(pager.StageFirstFrame(manifest, 10U, replacement),
          "new root replaces the held source before its pick completes");
    fxe::ManualMillis::AdvanceMs(
        modules::LitchcraftSession::PICK_ARM_MS +
        modules::LitchcraftSession::PICK_PROGRESS_MS);
    session.Update();
    bool sawCancelled = false;
    while (session.PeekPacket(packet)) {
        if (packet.bytes[1] ==
                static_cast<uint8_t>(modules::LitchcraftSession::Event::ColorPick)) {
            sawCancelled = packet.bytes[12U] == 0U;
        }
        session.AcceptPacket();
    }
    Check(sawCancelled,
          "replacement publishes idle pick status to the connected app");
    std::array<store::Emitter, 4U> shown{};
    uint16_t index = store::FRAME_COUNT;
    uint16_t duration = 0U;
    Check(pager.CopyDisplayedSquare(square, shown, index, duration) &&
              shown[0].color.r == 2U && shown[0].color.g == 33U &&
              shown[0].color.b == 77U && !pager.HasUnpersistedEdit(),
          "retired hold cannot overwrite or save the replacement root");
}

void TestRetirementDuringPersistenceCannotResurrect() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              pager.PaintDisplayed(0U, 7U, 15U, {3U, 4U, 5U}, false),
          "retirement race fixture has an unsaved displayed edit");
    medium.retireOnSync = &pager;
    (void)pager.Service(frames, false);
    Check(medium.retireOnSync == nullptr && frames.Generation() != 0U &&
              pager.ReadLiveAuthority().generation == 0U &&
              pager.FrameCount() == 0U,
          "flash publication cannot resurrect authority retired during commit");
}

void TestClearAdvancesDurableRetirementFence() {
    store::Manifest manifest{};
    stream::FrameStream pager;
    Check(pager.Configure(manifest, 1U) && pager.RetireDocument(),
          "clear fence starts from retired live generation one");
    Check(!pager.Configure(manifest, 0x80000001U),
          "old live fence refuses an ambiguous distant blank generation");
    Check(pager.AdvanceDurableRetirementGeneration(0x80000000U) &&
              !pager.Configure(manifest, 0x80000000U) &&
              pager.Configure(manifest, 0x80000001U),
          "verified durable clear marker fences old roots and admits its adjacent blank");
    Check(!pager.AdvanceDurableRetirementGeneration(1U),
          "clear cannot move durable retirement fence backwards");
}

void TestPlayingCanvasPinsAcceptedSourceAtClockBoundary() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames, 50U);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "playing Canvas boundary loads its source frame");
    modules::LitchcraftSession session;
    session.Configure(3U, 42U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    session.Compose(board, status);
    Check(LoadOne(pager, frames) == stream::FrameStream::ServiceResult::PrefetchedNext,
          "playing Canvas boundary has a distinct warm successor");
    StartStorePlayback(session, frames.Generation());
    constexpr uint32_t epoch = 37U;
    std::array<uint8_t, 8U> guard{};
    fxe::WriteU32LE(frames.Generation(), guard.data());
    fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
    Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) ==
              modules::LitchcraftSession::Status::Ok,
          "playing Canvas guards its selected source before the boundary");
    const auto revision = pager.LiveRevision();
    const auto fingerprint = pager.DisplayedFingerprint();
    const size_t writes = medium.writes;
    const size_t reads = medium.reads;
    const std::array<uint8_t, 6U> rotate{0U, 15U, 0U, 0U, 0U, 2U};
    fxe::ManualMillis::SetNowMs(49U);
    const auto accepted = [&]() {
        // Admission samples 49 ms; edit completion and any later sample see
        // 50 ms. The accepted guarded source must own the resulting pause.
        fxe::ScopedMillisProviderOverride boundaryClock(+[]() -> uint32_t {
            const auto sampled = fxe::ManualMillis::NowMs();
            fxe::ManualMillis::SetNowMs(50U);
            return sampled;
        });
        return session.ApplyConnectionOrdered(
            9U, rotate.data(), rotate.size(), epoch);
    }();
    Check(accepted == modules::LitchcraftSession::Status::Ok &&
              pager.LiveRevision() == revision + 1U &&
              pager.ReadLiveAuthority().dirty && session.UndoCount() == 1U &&
              session.RedoCount() == 0U && medium.writes == writes &&
              medium.reads == reads,
          "boundary Canvas accepts one dirty Rotate without storage I/O");
    std::printf("[PLAYING_CANVAS_BOUNDARY] selected=%u displayed=%u requested=%u elapsed=%u playing=%u revision=%u undo=%u\n",
                session.SelectedFrame(), pager.DisplayedIndex(), pager.RequestedIndex(),
                session.AnimationFrameElapsedMs(), session.IsAnimationPlaying(),
                pager.LiveRevision(), session.UndoCount());
    Check(!session.IsAnimationPlaying() && session.SelectedFrame() == 0U &&
              pager.DisplayedIndex() == 0U && pager.RequestedIndex() == 0U &&
              pager.DisplayedFingerprint() != fingerprint,
          "accepted playing Canvas pauses its modified source instead of resampling the warm successor");
    session.Compose(board, status);
    Check(session.SelectedFrame() == 0U && pager.DisplayedIndex() == 0U &&
              pager.RequestedIndex() == 0U && session.AnimationFrameElapsedMs() == 49U,
          "subsequent Compose retains the accepted Canvas pause on its source");
}

void TestPausedStoreEditPreservesPlaybackPhase() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    enum class Edit { Rotate, Undo, Redo, PhysicalUndo };
    for (const auto edit : {Edit::Rotate, Edit::Undo, Edit::Redo, Edit::PhysicalUndo}) {
        for (const uint16_t phase : {0U, 25U}) {
            for (const uint32_t waitBeforeEdit : {0U, 1000U}) {
                for (const uint32_t waitBeforePlay : {0U, 150U}) {
                    fxe::ManualMillis::SetNowMs(0U);
                    Medium medium;
                    store::FrameStore frames(medium.Ops());
                    Seed(medium, frames, 50U);
                    stream::FrameStream pager;
                    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                              LoadOne(pager, frames) ==
                                  stream::FrameStream::ServiceResult::LoadedRequested,
                          "paused edit journey loads its original complete frame");
                    modules::LitchcraftSession session;
                    session.Configure(3U, 42U);
                    session.RestoreArtwork({}, false, 0U);
                    session.AttachFrameStream(&pager);
                    modules::BoardLedFrame board{};
                    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
                    session.Compose(board, status);
                    constexpr uint32_t epoch = 37U;
                    const auto arm = [&]() {
                        std::array<uint8_t, 8U> guard{};
                        fxe::WriteU32LE(frames.Generation(), guard.data());
                        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
                        return session.ApplyConnectionOrdered(
                            8U, guard.data(), guard.size(), epoch);
                    };
                    const std::array<uint8_t, 6U> rotate{0U, 15U, 0U, 0U, 0U, 2U};
                    if (edit != Edit::Rotate) {
                        Check(arm() == modules::LitchcraftSession::Status::Ok &&
                                  session.ApplyConnectionOrdered(
                                      9U, rotate.data(), rotate.size(), epoch) ==
                                      modules::LitchcraftSession::Status::Ok,
                              "Undo/Redo journey starts from one accepted dirty Rotate");
                        if (edit == Edit::Redo) {
                            Check(arm() == modules::LitchcraftSession::Status::Ok &&
                                      session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
                                          modules::LitchcraftSession::Status::Ok,
                                  "Redo journey has an accepted Undo to replay");
                        }
                    }
                    std::array<uint8_t, 6U> runtime{1U, 0U};
                    fxe::WriteU32LE(frames.Generation(), runtime.data() + 2U);
                    if (phase != 0U) {
                        StartStorePlayback(session, frames.Generation());
                        fxe::ManualMillis::SetNowMs(phase);
                        session.Compose(board, status);
                        runtime[0U] = 0U;
                        Check(session.ApplyConnectionOrdered(
                                  2U, runtime.data(), runtime.size()) ==
                                  modules::LitchcraftSession::Status::Ok,
                              "edit journey pauses the same frame at a nonzero phase");
                    }
                    const uint16_t frozenPhase = session.AnimationFrameElapsedMs();
                    Check(frozenPhase == phase && session.SelectedFrame() == 0U,
                          "before edit the paused phase is authoritative");
                    const size_t writesBeforeEdit = medium.writes;
                    const uint32_t revisionBeforeEdit = pager.LiveRevision();
                    const uint32_t editedAt = phase + waitBeforeEdit;
                    fxe::ManualMillis::SetNowMs(editedAt);
                    session.Compose(board, status);
                    Check(session.AnimationFrameElapsedMs() == frozenPhase &&
                              arm() == modules::LitchcraftSession::Status::Ok,
                          "paused wall time preserves the immutable pre-edit phase");
                    const bool undo = edit == Edit::Undo || edit == Edit::PhysicalUndo;
                    auto accepted = modules::LitchcraftSession::Status::NoHistory;
                    if (edit == Edit::PhysicalUndo) {
                        session.ObserveButton({modules::UserButton::P1X,
                                               fxe::Button::Event::Press,
                                               modules::UiOwner::P1});
                        session.ObserveButton({modules::UserButton::P1X,
                                               fxe::Button::Event::Release,
                                               modules::UiOwner::P1});
                        if (session.UndoCount() == 0U) {
                            accepted = modules::LitchcraftSession::Status::Ok;
                        }
                    } else {
                        accepted = edit == Edit::Rotate
                            ? session.ApplyConnectionOrdered(
                                  9U, rotate.data(), rotate.size(), epoch)
                            : session.ApplyConnectionOrdered(
                                  undo ? 6U : 7U, nullptr, 0U, epoch);
                    }
                    Check(accepted == modules::LitchcraftSession::Status::Ok &&
                              pager.LiveRevision() == revisionBeforeEdit + 1U &&
                              pager.ReadLiveAuthority().dirty &&
                              medium.writes == writesBeforeEdit &&
                              session.UndoCount() == (undo ? 0U : 1U) &&
                              session.RedoCount() == (undo ? 1U : 0U),
                          "edit accepts exactly one dirty history step without flash writes");
                    session.Compose(board, status);
                    std::printf("[PAUSED_EDIT_CLOCK] edit=%u phase=%u wait_edit=%u wait_play=%u after_edit=%u selected=%u requested=%u\n",
                                static_cast<unsigned>(edit), phase, waitBeforeEdit,
                                waitBeforePlay, session.AnimationFrameElapsedMs(),
                                session.SelectedFrame(), pager.RequestedIndex());
                    Check(!session.IsAnimationPlaying() &&
                              session.SelectedFrame() == 0U &&
                              session.AnimationFrameElapsedMs() == frozenPhase,
                          "accepted Rotate/Undo/Redo preserves the exact paused phase");
                    const uint32_t playedAt = editedAt + waitBeforePlay;
                    fxe::ManualMillis::SetNowMs(playedAt);
                    runtime[0U] = 0U;
                    Check(session.ApplyConnectionOrdered(
                              2U, runtime.data(), runtime.size()) ==
                              modules::LitchcraftSession::Status::Ok &&
                              session.AnimationFrameElapsedMs() == frozenPhase,
                          "newer duplicate Pause retains the edit's original phase");
                    runtime[0U] = 1U;
                    Check(session.ApplyConnectionOrdered(
                              2U, runtime.data(), runtime.size()) ==
                              modules::LitchcraftSession::Status::Ok,
                          "immediate or waited Play follows the accepted edit");
                    session.Compose(board, status);
                    Check(session.SelectedFrame() == 0U &&
                              pager.RequestedIndex() == 0U &&
                              session.AnimationFrameElapsedMs() == frozenPhase,
                          "Play starts at the frozen phase without consuming paused wall time");
                    std::printf("[PAUSED_EDIT_PLAY] edit=%u at=%u selected=%u requested=%u elapsed=%u\n",
                                static_cast<unsigned>(edit), playedAt,
                                session.SelectedFrame(), pager.RequestedIndex(),
                                session.AnimationFrameElapsedMs());
                    const uint32_t boundary = playedAt + 50U - frozenPhase;
                    fxe::ManualMillis::SetNowMs(boundary - 1U);
                    session.Compose(board, status);
                    Check(session.SelectedFrame() == 0U &&
                              pager.RequestedIndex() == 0U &&
                              session.AnimationFrameElapsedMs() == 49U,
                          "edit-to-Play reaches the final millisecond of its authored dwell");
                    fxe::ManualMillis::SetNowMs(boundary);
                    session.Compose(board, status);
                    Check(session.SelectedFrame() == 0U && pager.RequestedIndex() == 1U &&
                              LoadOne(pager, frames) ==
                                  stream::FrameStream::ServiceResult::LoadedRequested,
                          "edit-to-Play requests only the exact first successor boundary");
                    session.Compose(board, status);
                    Check(session.SelectedFrame() == 1U && pager.RequestedIndex() == 1U &&
                              session.AnimationFrameElapsedMs() == 0U && board[0].left.r == 11U,
                          "first boundary presents the complete successor at phase zero");
                    std::printf("[PAUSED_EDIT_BOUNDARY] edit=%u at=%u selected=%u requested=%u elapsed=%u red=%u\n",
                                static_cast<unsigned>(edit), boundary,
                                session.SelectedFrame(), pager.RequestedIndex(),
                                session.AnimationFrameElapsedMs(), board[0].left.r);
                    fxe::ManualMillis::SetNowMs(boundary + 10U);
                    runtime[0U] = 0U;
                    runtime[1U] = 1U;
                    Check(session.ApplyConnectionOrdered(
                              2U, runtime.data(), runtime.size()) ==
                              modules::LitchcraftSession::Status::Ok &&
                              session.AnimationFrameElapsedMs() == 10U,
                          "a newer Pause owns the newly displayed successor phase");
                    fxe::ManualMillis::SetNowMs(boundary + 510U);
                    runtime[0U] = 2U;
                    runtime[1U] = 2U;
                    Check(session.ApplyConnectionOrdered(
                              2U, runtime.data(), runtime.size()) ==
                              modules::LitchcraftSession::Status::Ok &&
                              LoadOne(pager, frames) ==
                                  stream::FrameStream::ServiceResult::LoadedRequested,
                          "a newer seek selects its own complete frame after the edit");
                    session.Compose(board, status);
                    runtime[0U] = 1U;
                    Check(session.ApplyConnectionOrdered(
                              2U, runtime.data(), runtime.size()) ==
                              modules::LitchcraftSession::Status::Ok,
                          "Play resumes the newer explicit seek");
                    session.Compose(board, status);
                    Check(session.SelectedFrame() == 2U && pager.RequestedIndex() == 2U &&
                              session.AnimationFrameElapsedMs() == 0U,
                          "old paused edit phase cannot replace the newer seek anchor");
                }
            }
        }
    }
}

void RunColdReadPlaybackCadence(const uint16_t heldFrame,
                                const uint8_t initialWarmFrames,
                                const uint32_t throughMs) {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    store::Manifest manifest{};
    manifest.frameCount = 32U;
    manifest.active = true;
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "cold-part playback seeds the actual Store");
    for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
        auto frame = MakeFrame(static_cast<uint8_t>(index));
        frame.durationMs = 50U;
        Check(frames.WriteFrame(token, index, frame) == store::Status::Ok,
              "cold-part playback writes distinct complete frames");
    }
    Check(frames.Commit(token) == store::Status::Ok,
          "cold-part playback commits complete authority");
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "cold playback presents its initial complete stored frame");
    if (initialWarmFrames == 2U) {
        Check(LoadOne(pager, frames) == stream::FrameStream::ServiceResult::PrefetchedNext,
              "cold-part playback warms only the production successor");
    }
    modules::LitchcraftSession session;
    session.Configure(3U, 142U);
    session.RestoreArtwork({}, false, 0U);
    struct Cadence {
        modules::LitchcraftSession& session;
        stream::FrameStream& pager;
        modules::BoardLedFrame board{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
        uint32_t nextRenderMs = 20U;
        uint32_t misses = 0U;
        uint32_t firstMissMs = 0U;
        uint32_t readStartedMs = 0U;
        uint32_t readCompletedMs = 0U;
        uint16_t firstExpected = 0U;
        uint16_t firstDisplayed = 0U;
        uint16_t heldFrame = 0U;
        uint32_t retainedSamples = 0U;
        bool held = false;
        bool playing = false;
        void Render(const uint32_t nowMs) {
            fxe::ManualMillis::SetNowMs(nowMs);
            session.Compose(board, status);
            const uint16_t expected = static_cast<uint16_t>((nowMs / 50U) % 32U);
            const uint16_t displayed = pager.DisplayedIndex();
            store::Frame complete{};
            uint16_t completeIndex = store::FRAME_COUNT;
            auto authored = MakeFrame(static_cast<uint8_t>(displayed));
            authored.durationMs = 50U;
            Check(pager.CopyDisplayed(complete, completeIndex) &&
                      completeIndex == displayed && Same(complete, authored) &&
                      session.SelectedFrame() == displayed,
                  "cold read exposes only one complete authored frame");
            Check(board[0U].left.r == authored.emitters[0U].color.r &&
                      board[0U].left.g == authored.emitters[0U].color.g &&
                      board[0U].left.b == authored.emitters[0U].color.b,
                  "cold read composition retains the complete displayed pixels");
            if (displayed == expected) {
                Check(session.AnimationFrameElapsedMs() == nowMs % 50U,
                      "ready frames retain their original authored phase");
            }
            if (held && nowMs >= uint32_t(heldFrame) * 50U &&
                nowMs <= readCompletedMs) {
                ++retainedSamples;
                Check(displayed + 1U == heldFrame &&
                          pager.RequestedIndex() == heldFrame &&
                          session.AnimationFrameElapsedMs() == 49U,
                      "late cold successor retains its predecessor and one pending boundary");
            }
            if (displayed != expected && misses++ == 0U) {
                firstMissMs = nowMs;
                firstExpected = expected;
                firstDisplayed = displayed;
            }
        }
        static void HoldRead(void* context) {
            auto& self = *static_cast<Cadence*>(context);
            self.held = true;
            self.readStartedMs = fxe::Millis();
            self.readCompletedMs = self.readStartedMs + 70U;
            while (self.nextRenderMs <= self.readCompletedMs) {
                const uint32_t sample = self.nextRenderMs;
                self.nextRenderMs += 20U;
                self.Render(sample);
            }
            fxe::ManualMillis::SetNowMs(self.readCompletedMs);
        }
    } cadence{session, pager};
    cadence.heldFrame = heldFrame;
    session.AttachFrameStream(&pager,
        [](void* context, const bool playing, uint32_t, bool) {
            static_cast<Cadence*>(context)->playing = playing;
        }, &cadence);
    cadence.Render(0U);
    StartStorePlayback(session, frames.Generation());
    // First successors test Play with only frame zero warm; frame 16 tests a
    // later physical part. Only this read costs 70 ms, not every frame. These
    // are controlled worker delays, not measurements of LittleFS or LED Show.
    medium.onceOnReadOffset = modules::litchcraft_frame_store_layout::Layout::SlotOffset(0U, heldFrame);
    medium.onceOnRead = &Cadence::HoldRead;
    medium.onceOnSyncContext = &cadence;
    while (cadence.nextRenderMs <= throughMs) {
        const uint32_t sample = cadence.nextRenderMs;
        cadence.nextRenderMs += 20U;
        cadence.Render(sample);
        // Two ordinary production paging opportunities per render sample;
        // unlike a settle loop, neither waits for a page before time advances.
        const auto prefetch = cadence.playing ? stream::FrameStream::PrefetchPolicy::TwoAhead
                                             : stream::FrameStream::PrefetchPolicy::Next;
        (void)pager.Service(frames, false, stream::FrameStream::ServiceMode::PagingOnly, prefetch);
        (void)pager.Service(frames, false, stream::FrameStream::ServiceMode::PagingOnly, prefetch);
    }
    std::printf("[COLD_PART_PLAYBACK] render_ms=20 hold_frame=%u warm_frames=%u hold_ms=70 "
                "read_begin=%u read_end=%u misses=%u first_ms=%u expected=%u displayed=%u "
                "retained_samples=%u final_ms=%u final_frame=%u requested=%u phase=%u\n",
                heldFrame, initialWarmFrames, cadence.readStartedMs, cadence.readCompletedMs, cadence.misses,
                cadence.firstMissMs, cadence.firstExpected, cadence.firstDisplayed,
                cadence.retainedSamples, fxe::Millis(), pager.DisplayedIndex(),
                pager.RequestedIndex(), session.AnimationFrameElapsedMs());
    Check(cadence.held && cadence.readCompletedMs - cadence.readStartedMs == 70U,
          "cold-part playback exercises exactly one bounded read hold");
    const uint32_t expectedMisses = heldFrame == 1U ? 2U : 0U;
    Check(cadence.misses == expectedMisses &&
              cadence.retainedSamples == expectedMisses,
          "only a cold first successor delayed beyond its dwell retains the previous frame");
    Check(pager.DisplayedIndex() == (throughMs / 50U) % 32U &&
              pager.RequestedIndex() == pager.DisplayedIndex() &&
              session.AnimationFrameElapsedMs() == throughMs % 50U &&
              session.IsAnimationPlaying(),
          "cold startup catches up to the authored schedule without a second Play");
}

void TestOneColdPartReadKeepsAuthoredPlaybackCadence() {
    RunColdReadPlaybackCadence(16U, 2U, 1100U);
}

void TestColdSuccessorPlaybackRetainsFramesAndCatchesUp() {
    RunColdReadPlaybackCadence(1U, 1U, 1000U);
    RunColdReadPlaybackCadence(2U, 1U, 1000U);
}

void TestPrefetchPolicyPreservesPausedReverseAndSmallWraps() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames, 50U);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::PrefetchedNext &&
              pager.Request(1U) && pager.TryPresentRequested() &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::PrefetchedNext,
          "paused paging prepares the displayed frame and next successor");
    const size_t pausedReads = medium.reads;
    Check(pager.Service(frames, false) == stream::FrameStream::ServiceResult::Idle &&
              pager.Request(0U) && pager.TryPresentRequested() && medium.reads == pausedReads,
          "default paused paging retains its warm reverse seek without far-prefetch I/O");
    Check(pager.Request(3U), "transfer-held pager still admits an explicit cold seek");
    const size_t beforeRequested = medium.reads;
    Check(pager.Service(frames, false, stream::FrameStream::ServiceMode::PagingOnly,
              stream::FrameStream::PrefetchPolicy::None) ==
                  stream::FrameStream::ServiceResult::LoadedRequested && pager.TryPresentRequested() &&
              medium.reads == beforeRequested + 1U,
          "no-lookahead still loads the essential requested control frame");
    Check(pager.Service(frames, false, stream::FrameStream::ServiceMode::PagingOnly,
              stream::FrameStream::PrefetchPolicy::None) == stream::FrameStream::ServiceResult::Idle &&
              medium.reads == beforeRequested + 1U,
          "no-lookahead skips only cosmetic background reads after the requested frame is ready");
    for (const uint16_t count : {1U, 2U}) {
        Medium smallMedium;
        store::FrameStore smallFrames(smallMedium.Ops());
        store::Manifest manifest{};
        manifest.frameCount = count;
        manifest.active = true;
        store::TransactionToken token{};
        Check(smallFrames.Recover() == store::Status::NoArtwork &&
                  smallFrames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
              "small-wrap prefetch starts actual stored authority");
        for (uint16_t index = 0U; index < count; ++index) {
            Check(smallFrames.WriteFrame(token, index, MakeFrame(static_cast<uint8_t>(index))) ==
                      store::Status::Ok, "small-wrap prefetch stores each distinct frame");
        }
        Check(smallFrames.Commit(token) == store::Status::Ok, "small-wrap prefetch commits");
        smallMedium.reads = 0U;
        stream::FrameStream smallPager;
        Check(smallPager.Configure(smallFrames.CurrentManifest(), smallFrames.Generation()),
              "small-wrap prefetch configures the production pager");
        for (uint8_t call = 0U; call < 4U; ++call) {
            (void)smallPager.Service(smallFrames, false,
                stream::FrameStream::ServiceMode::PagingOnly,
                stream::FrameStream::PrefetchPolicy::TwoAhead);
            Check(smallPager.TryPresentRequested(), "small-wrap requested frame remains available");
        }
        Check(smallMedium.reads == count && smallPager.Request(count - 1U) &&
                  smallPager.TryPresentRequested(),
              "two-ahead wrapping loads every unique frame exactly once");
    }
}

void TestSeekDuringFarReadPreservesNewDemand() {
    for (const uint16_t seek : {0U, 5U}) {
        Medium medium;
        store::FrameStore frames(medium.Ops());
        store::Manifest manifest{};
        manifest.frameCount = 6U;
        manifest.active = true;
        store::TransactionToken token{};
        Check(frames.Recover() == store::Status::NoArtwork &&
                  frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
              "seek-during-prefetch starts actual Store authority");
        for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
            Check(frames.WriteFrame(token, index, MakeFrame(static_cast<uint8_t>(index))) ==
                      store::Status::Ok, "seek-during-prefetch stores distinct frames");
        }
        Check(frames.Commit(token) == store::Status::Ok, "seek-during-prefetch commits");
        stream::FrameStream pager;
        Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested &&
                  pager.TryPresentRequested(), "seek-during-prefetch presents initial frame");
        const auto page = [&] {
            return pager.Service(frames, false, stream::FrameStream::ServiceMode::PagingOnly,
                                 stream::FrameStream::PrefetchPolicy::TwoAhead);
        };
        Check(page() == stream::FrameStream::ServiceResult::PrefetchedNext &&
                  page() == stream::FrameStream::ServiceResult::PrefetchedNext &&
                  pager.Request(1U) && pager.TryPresentRequested(),
              "seek-during-prefetch warms the existing three cache slots");
        struct Seek { stream::FrameStream& pager; uint16_t frame; } request{pager, seek};
        medium.onceOnRead = [](void* context) {
            auto& request = *static_cast<Seek*>(context);
            Check(request.pager.Request(request.frame), "seek is admitted during far-page I/O");
        };
        medium.onceOnSyncContext = &request;
        Check(page() == stream::FrameStream::ServiceResult::Idle &&
                  medium.onceOnRead == nullptr && pager.RequestedIndex() == seek &&
                  pager.DisplayedIndex() == 1U,
              "far-page publication rechecks the current requested frame and successor");
        const size_t reads = medium.reads;
        Check(pager.Request(0U) && pager.TryPresentRequested() && medium.reads == reads,
              "far read preserves victim zero as a newly requested frame or immediate successor");
    }
}

void TestLateFrameCatchesUpToAuthoredSchedule() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "late-frame journey loads only frame zero");
    modules::LitchcraftSession session;
    modules::LitchcraftSession::Canvas fallback{};
    session.Configure(3U, 42U);
    session.RestoreArtwork(fallback, false, 0U);
    session.AttachFrameStream(&pager);
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    session.Compose(board, status);
    Check(session.SelectedFrame() == 0U &&
              session.AnimationFrameElapsedMs() == 0U,
          "initial paused Store presentation starts with a fresh frame-local anchor");
    StartStorePlayback(session, frames.Generation());
    fxe::ManualMillis::SetNowMs(150U);
    session.Compose(board, status);
    Check(board[0].left.r == 10U,
          "late prefetch holds the last complete frame");
    Check(session.SelectedFrame() == 0U &&
              session.AnimationFrameElapsedMs() == 109U,
          "late held frame zero reports its expired displayed dwell at 150 ms");
    Check(LoadOne(pager, frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested,
          "late requested frame finishes after its nominal boundary");
    fxe::ManualMillis::SetNowMs(200U);
    session.Compose(board, status);
    Check(board[0].left.r == 11U,
          "late frame publishes when complete");
    fxe::ManualMillis::SetNowMs(221U);
    session.Compose(board, status);
    Check(board[0].left.r == 11U && pager.RequestedIndex() == 2U,
          "late frame catches up at its authored boundary instead of adding permanent drift");

    Check(session.AnimationFrameElapsedMs() == 110U,
          "late held frame one reports its own expired dwell");
    std::array<uint8_t, 6U> runtime{0U, 1U};
    fxe::WriteU32LE(frames.Generation(), runtime.data() + 2U);
    Check(session.ApplyConnectionOrdered(2U, runtime.data(), runtime.size()) ==
              modules::LitchcraftSession::Status::Ok &&
              !session.IsAnimationPlaying() &&
              session.AnimationFrameElapsedMs() == 110U,
          "paused pending playback retains the displayed frame's elapsed anchor");
    fxe::ManualMillis::SetNowMs(250U);
    session.Compose(board, status);
    Check(session.SelectedFrame() == 1U && pager.RequestedIndex() == 1U &&
              session.AnimationFrameElapsedMs() == 110U,
          "named Pause pins its displayed frame and freezes elapsed authority");
    Check(LoadOne(pager, frames) ==
              stream::FrameStream::ServiceResult::PrefetchedNext,
          "canceled successor remains eligible for production prefetch while paused");
    session.Compose(board, status);
    session.Compose(board, status);
    std::printf("[LATE_FRAME_PAUSED_COMPLETE] at=250 selected=%u displayed=%u requested=%u elapsed=%u red=%u\n",
                session.SelectedFrame(), pager.DisplayedIndex(),
                pager.RequestedIndex(), session.AnimationFrameElapsedMs(),
                board[0].left.r);
    Check(!session.IsAnimationPlaying() && session.SelectedFrame() == 1U &&
              pager.DisplayedIndex() == 1U && pager.RequestedIndex() == 1U &&
              board[0].left.r == 11U &&
              session.AnimationFrameElapsedMs() == 110U,
          "late successor completion cannot change the paused displayed frame or elapsed authority");
    const size_t readsAfterPausedLoad = medium.reads;
    fxe::ManualMillis::SetNowMs(275U);
    runtime[0U] = 1U;
    Check(session.ApplyConnectionOrdered(2U, runtime.data(), runtime.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "paused displayed frame resumes despite its already-loaded successor");
    session.Compose(board, status);
    Check(session.IsAnimationPlaying() && session.SelectedFrame() == 2U &&
              pager.DisplayedIndex() == 2U && board[0].left.r == 12U &&
              session.AnimationFrameElapsedMs() == 0U &&
              medium.reads == readsAfterPausedLoad,
          "resume presents the retained successor at the pause-shifted authored boundary without rereading");
    fxe::ManualMillis::SetNowMs(300U);
    session.Compose(board, status);
    Check(session.SelectedFrame() == 2U &&
              session.AnimationFrameElapsedMs() == 25U,
          "resumed successor advances from its authored anchor rather than storage completion");
    fxe::ManualMillis::SetNowMs(387U);
    session.Compose(board, status);
    Check(session.SelectedFrame() == 2U && pager.RequestedIndex() == 3U &&
              board[0].left.r == 12U &&
              session.AnimationFrameElapsedMs() == 111U,
          "resumed successor reaches its exact next authored boundary");
    runtime[0U] = 0U;
    runtime[1U] = 2U;
    Check(session.ApplyConnectionOrdered(2U, runtime.data(), runtime.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "resumed successor can pause with its own cold next page");
    runtime[0U] = 2U;
    runtime[1U] = 1U;
    Check(session.ApplyConnectionOrdered(2U, runtime.data(), runtime.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "explicit seek cancels the pending successor");
    session.Compose(board, status);
    Check(session.SelectedFrame() == 1U && pager.RequestedIndex() == 1U &&
              session.AnimationFrameElapsedMs() == 0U,
          "explicit seek resets the displayed anchor and pending playback fence");
    fxe::ManualMillis::SetNowMs(412U);
    runtime[0U] = 1U;
    Check(session.ApplyConnectionOrdered(2U, runtime.data(), runtime.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "explicitly sought frame resumes through ordered playback");
    fxe::ManualMillis::SetNowMs(437U);
    session.Compose(board, status);
    Check(session.SelectedFrame() == 1U &&
              session.AnimationFrameElapsedMs() == 25U,
          "resumed explicit seek uses its new anchor without a stale pending clamp");

    const std::array<std::array<uint32_t, 4U>, 2U> heldSamples{{
        {{150U, 260U, 370U, 370U}},
        {{370U, 370U, 370U, 370U}},
    }};
    for (size_t scenario = 0U; scenario < heldSamples.size(); ++scenario) {
        fxe::ManualMillis::SetNowMs(0U);
        Medium delayedMedium;
        store::FrameStore delayedFrames(delayedMedium.Ops());
        Seed(delayedMedium, delayedFrames);
        stream::FrameStream delayedPager;
        Check(delayedPager.Configure(delayedFrames.CurrentManifest(),
                                     delayedFrames.Generation()) &&
                  LoadOne(delayedPager, delayedFrames) ==
                      stream::FrameStream::ServiceResult::LoadedRequested,
              "multi-dwell journey loads only frame zero");
        modules::LitchcraftSession delayedSession;
        delayedSession.Configure(3U, 42U);
        delayedSession.RestoreArtwork(fallback, false, 0U);
        delayedSession.AttachFrameStream(&delayedPager);
        delayedSession.Compose(board, status);
        StartStorePlayback(delayedSession, delayedFrames.Generation());

        // Keep the healthy requested page pending across several dwells. Both
        // real render ticks and repeated sampling at one instant must consume
        // the displayed frame's authored boundary exactly once.
        for (const uint32_t sample : heldSamples[scenario]) {
            fxe::ManualMillis::SetNowMs(sample);
            delayedSession.Compose(board, status);
            Check(board[0].left.r == 10U &&
                      delayedPager.RequestedIndex() == 1U,
                  "multi-dwell hold retains the last complete frame and exact successor");
            std::printf("[LATE_FRAME_HOLD] scenario=%zu at=%u selected=%u requested=%u elapsed=%u red=%u\n",
                        scenario, sample, delayedSession.SelectedFrame(),
                        delayedPager.RequestedIndex(),
                        delayedSession.AnimationFrameElapsedMs(),
                        board[0].left.r);
            Check(delayedSession.AnimationFrameElapsedMs() == 109U,
                  "repeated pending samples preserve the authored late-frame anchor");
        }

        // Release one production worker page per sampling call, with time
        // still at 370 ms. Durations 110/111/112/113 place that instant 37 ms
        // into frame three; storage completion cannot restart the schedule.
        constexpr std::array<uint16_t, 3U> expectedReleaseElapsed{
            110U, 111U, 37U};
        for (uint8_t release = 0U; release < 3U; ++release) {
            (void)LoadOne(delayedPager, delayedFrames);
            delayedSession.Compose(board, status);
            std::printf("[LATE_FRAME_RELEASE] scenario=%zu release=%u at=370 selected=%u requested=%u elapsed=%u red=%u\n",
                        scenario, release, delayedSession.SelectedFrame(),
                        delayedPager.RequestedIndex(),
                        delayedSession.AnimationFrameElapsedMs(),
                        board[0].left.r);
            Check(delayedSession.SelectedFrame() == release + 1U &&
                      delayedPager.DisplayedIndex() == release + 1U &&
                      board[0].left.r == 11U + release &&
                      delayedSession.AnimationFrameElapsedMs() ==
                          expectedReleaseElapsed[release],
                  "each released frame reports its own displayed elapsed authority");
        }
        Check(delayedSession.SelectedFrame() == 3U &&
                  delayedPager.DisplayedIndex() == 3U &&
                  board[0].left.r == 13U &&
                  delayedSession.AnimationFrameElapsedMs() == 37U,
              "multi-dwell decode catches up to the original authored frame and phase");
    }

    for (const uint32_t initial : {0U, UINT32_MAX - 40U, UINT32_MAX - 74U}) {
        fxe::ManualMillis::SetNowMs(initial);
        Medium clockMedium;
        store::FrameStore clockFrames(clockMedium.Ops());
        Seed(clockMedium, clockFrames);
        stream::FrameStream clockPager;
        Check(clockPager.Configure(clockFrames.CurrentManifest(),
                                   clockFrames.Generation()) &&
                  LoadOne(clockPager, clockFrames) ==
                      stream::FrameStream::ServiceResult::LoadedRequested,
              "zero/wrap pause journey loads only its initial Store frame");
        modules::LitchcraftSession clockSession;
        clockSession.Configure(3U, 42U);
        clockSession.RestoreArtwork({}, false, 0U);
        clockSession.AttachFrameStream(&clockPager);
        clockSession.Compose(board, status);
        std::array<uint8_t, 6U> clockRuntime{1U, 0U};
        fxe::WriteU32LE(clockFrames.Generation(), clockRuntime.data() + 2U);
        fxe::ManualMillis::SetNowMs(initial + 50U);
        Check(clockSession.ApplyConnectionOrdered(
                  2U, clockRuntime.data(), clockRuntime.size()) ==
                  modules::LitchcraftSession::Status::Ok,
              "zero/wrap paused frame resumes through ordered Play");
        clockSession.Compose(board, status);
        Check(clockSession.SelectedFrame() == 0U &&
                  clockSession.AnimationFrameElapsedMs() == 0U,
              "a valid initial pause at zero or across wrap preserves elapsed zero");
        fxe::ManualMillis::SetNowMs(initial + 75U);
        clockSession.Compose(board, status);
        Check(clockSession.AnimationFrameElapsedMs() == 25U &&
                  clockSession.ApplyConnectionOrdered(
                      2U, clockRuntime.data(), clockRuntime.size()) ==
                      modules::LitchcraftSession::Status::Ok &&
                  clockSession.AnimationFrameElapsedMs() == 25U,
              "duplicate Play preserves the existing fractional authored anchor");
        clockRuntime[0U] = 0U;
        Check(clockSession.ApplyConnectionOrdered(
                  2U, clockRuntime.data(), clockRuntime.size()) ==
                  modules::LitchcraftSession::Status::Ok,
              "zero/wrap fractional frame pauses through its ordinary owner");
        fxe::ManualMillis::SetNowMs(initial + 100U);
        Check(clockSession.ApplyConnectionOrdered(
                  2U, clockRuntime.data(), clockRuntime.size()) ==
                  modules::LitchcraftSession::Status::Ok &&
                  !clockSession.IsAnimationPlaying() &&
                  clockSession.SelectedFrame() == 0U &&
                  clockSession.AnimationFrameElapsedMs() == 25U,
              "duplicate Pause preserves its fractional displayed phase after wall time");
        fxe::ManualMillis::SetNowMs(initial + 115U);
        clockSession.Compose(board, status);
        Check(clockSession.AnimationFrameElapsedMs() == 25U,
              "zero/wrap paused elapsed stays frozen during wall time");
        clockRuntime[0U] = 1U;
        Check(clockSession.ApplyConnectionOrdered(
                  2U, clockRuntime.data(), clockRuntime.size()) ==
                  modules::LitchcraftSession::Status::Ok,
              "zero/wrap fractional pause resumes through ordered Play");
        clockSession.Compose(board, status);
        Check(clockSession.AnimationFrameElapsedMs() == 25U,
              "a valid fractional pause at millis zero resumes its exact phase");
        fxe::ManualMillis::SetNowMs(initial + 199U);
        clockSession.Compose(board, status);
        Check(clockSession.SelectedFrame() == 0U &&
                  clockPager.RequestedIndex() == 0U &&
                  clockSession.AnimationFrameElapsedMs() == 109U,
              "zero/wrap resumed frame reaches the last millisecond without consuming the next boundary");
        fxe::ManualMillis::SetNowMs(initial + 200U);
        clockSession.Compose(board, status);
        Check(clockSession.SelectedFrame() == 0U &&
                  clockPager.RequestedIndex() == 1U &&
                  clockSession.AnimationFrameElapsedMs() == 109U &&
                  LoadOne(clockPager, clockFrames) ==
                      stream::FrameStream::ServiceResult::LoadedRequested,
              "zero/wrap resumed frame requests its exact successor at the authored boundary");
        clockSession.Compose(board, status);
        Check(clockSession.SelectedFrame() == 1U &&
                  clockSession.AnimationFrameElapsedMs() == 0U,
              "zero/wrap successor presents at elapsed zero without extra dwell");
    }
}

void TestSessionSignalsCorruptPlaybackFrame() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::PrefetchedNext,
          "corrupt journey primes its first two frames");
    medium.bytes[RecordPayloadOffset(medium, 2U) + 2U] ^= 0x80U;

    modules::LitchcraftSession session;
    modules::LitchcraftSession::Canvas fallback{};
    session.Configure(3U, 43U);
    session.RestoreArtwork(fallback, true, 0U);
    session.AttachFrameStream(&pager);
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    session.Compose(board, status);
    StartStorePlayback(session, frames.Generation());
    fxe::ManualMillis::SetNowMs(110U);
    session.Compose(board, status);
    Check(LoadOne(pager, frames) ==
              stream::FrameStream::ServiceResult::Corrupt,
          "prefetch detects the corrupted third frame");
    fxe::ManualMillis::SetNowMs(221U);
    session.Compose(board, status);
    Check(board[0].left.r == 11U &&
              std::all_of(status.begin(), status.end(), [](const modules::Rgb led) {
                  return led.r == 48U && led.g == 42U && led.b == 0U;
              }),
          "corrupt frame retains the last complete image and shows yellow status");
    fxe::ManualMillis::SetNowMs(332U);
    session.Compose(board, status);
    Check(LoadOne(pager, frames) ==
              stream::FrameStream::ServiceResult::LoadedRequested,
          "corrupt dwell skips to the next independently decodable frame");
    fxe::ManualMillis::SetNowMs(400U);
    session.Compose(board, status);
    Check(board[0].left.r == 13U,
          "first valid frame after corruption publishes");
    fxe::ManualMillis::SetNowMs(512U);
    session.Compose(board, status);
    Check(board[0].left.r == 13U,
          "recovered frame receives its complete duration from publication");
}

void TestInactiveGenerationRunsTheExitLifecycle() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "authority transition loads its active generation");

    modules::LitchcraftSession session;
    modules::LitchcraftSession::Canvas fallback{};
    session.Configure(3U, 44U);
    session.RestoreArtwork(fallback, false, 0U);
    session.AttachFrameStream(&pager);
    Check(session.UpdatePresentationAccess() == modules::LitchcraftSession::PresentationAccess::Control,
          "initial streamed mode adoption requires presentation control while stored session is inactive");
    session.Update();
    Check(session.IsActive(),
          "active schema-4 generation enters presentation authority");
    const uint32_t beforeRevision = session.Revision();

    auto inactive = frames.CurrentManifest();
    inactive.active = false;
    inactive.playbackOnEntry = false;
    Check(pager.Configure(inactive, frames.Generation() + 1U),
          "new inactive schema-4 authority configures");
    Check(!session.IsActive() &&
              session.UpdatePresentationAccess() == modules::LitchcraftSession::PresentationAccess::Control,
          "streamed inactive authority cannot bypass admission before stored active mode finishes Exit");
    session.Update();
    Check(session.UpdatePresentationAccess() == modules::LitchcraftSession::PresentationAccess::Control,
          "pending coordinator Exit handoff retains presentation control");
    Check(!session.IsActive() && session.ConsumeExited() &&
              session.Revision() == beforeRevision + 1U,
          "inactive schema-4 authority runs Exit cleanup and publishes the coordinator barrier");
    Check(session.UpdatePresentationAccess() == modules::LitchcraftSession::PresentationAccess::None,
          "settled inactive Store maintenance needs no presentation lease");
    Check(pager.Configure(inactive, frames.Generation() + 1U) &&
              session.UpdatePresentationAccess() == modules::LitchcraftSession::PresentationAccess::Control,
          "reopening the same Store generation requires document-epoch adoption control");
    session.Update();
    Check(session.UpdatePresentationAccess() == modules::LitchcraftSession::PresentationAccess::None,
          "same-generation document adoption returns to lease-free inactive maintenance");
    Check(session.BeginBootArtworkOnce() && !session.IsActive() &&
              session.UpdatePresentationAccess() == modules::LitchcraftSession::PresentationAccess::Control,
          "boot-only artwork keeps presentation ownership while Craft is inactive");
    session.CancelBootArtwork();
    Check(session.UpdatePresentationAccess() == modules::LitchcraftSession::PresentationAccess::None,
          "retired boot artwork releases idle presentation ownership");
}

void TestExitWinsOverPreviewRollbackToActiveRoot() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation() + 1U),
          "Exit rollback journey starts with a provisional generation");
    modules::LitchcraftSession session;
    session.Configure(3U, 45U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    Check(session.IsActive(), "boot can adopt streamed mode authority");
    session.Exit();
    (void)session.ConsumeExited();
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()),
          "retired transfer restores its previously committed active root");
    Check(!session.IsActive(), "root rollback cannot undo the accepted Exit before Update");
    session.Update();
    Check(!session.IsActive(), "root rollback cannot undo the accepted Exit during Update");
    const std::array<uint8_t, 8U> enter{0x4cU,
        static_cast<uint8_t>(modules::LitchcraftSession::Op::Enter),
        0U, 0U, 0U, 0U, 91U, 0U};
    session.HandlePacket(enter.data(), enter.size(), true, 0U);
    Check(session.IsActive(), "deliberate Enter restores the retained artwork normally");
}

void TestOpenWireUploadKeepsCommittedPlaybackDecodeIndependent() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "wire interleave fixture presents the committed frame");

    std::array<uint8_t, store::WIRE_RECORD_HEADER_BYTES +
                            store::WIRE_FRAME_MAX_BYTES> firstRecord{};
    size_t firstRecordRead = 0U;
    Check(frames.ReadWireBody(frames.Generation(), 0U,
                               firstRecord.data(), firstRecord.size(),
                               firstRecordRead) == store::Status::Ok,
          "wire interleave fixture reads the current schema-4 first record");
    const size_t recordBytes = store::WIRE_RECORD_HEADER_BYTES +
        fxe::ReadU16LE(firstRecord.data());
    std::vector<uint8_t> replacement(
        firstRecord.begin(), firstRecord.begin() + recordBytes);
    store::Manifest manifest = frames.CurrentManifest();
    manifest.frameCount = 1U;
    store::TransactionToken token{};
    Check(frames.BeginWire(manifest, frames.Generation(), 9U,
                           static_cast<uint32_t>(replacement.size()),
                           fxe::Crc32(replacement.data(), replacement.size()),
                           token) == store::Status::Ok &&
              frames.WriteBodyChunk(token, 0U, replacement.data(), 10U) ==
                  store::Status::Ok,
          "wire interleave leaves a partial incoming record in the inactive bank");
    Check(pager.Request(2U) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "committed frame pages while a replacement record is incomplete");
    store::Frame displayed{};
    uint16_t index = store::FRAME_COUNT;
    Check(pager.CopyDisplayed(displayed, index) && index == 2U &&
              Same(displayed, MakeFrame(12U)),
          "open upload never substitutes its partial frame for committed playback");
    Check(frames.WriteBodyChunk(token, 10U,
                                replacement.data() + 10U,
                                replacement.size() - 10U) ==
                  store::Status::Ok &&
              frames.IncomingPreviewReady() &&
              Same(frames.IncomingFirstFrame(), MakeFrame(10U)),
          "incoming frame zero survives interleaved playback byte-exact");
    Check(frames.Abort(token) == store::Status::Ok &&
              pager.Request(3U) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              pager.CopyDisplayed(displayed, index) && index == 3U &&
              Same(displayed, MakeFrame(13U)),
          "Cancel retains the committed bank and decode cache");
}
void TestStoreHistoryRetainsSparseStrokesWithoutWholeFrames() {
    modules::litchcraft_store_history::StoreHistory history;
    store::Frame frame{};
    for (uint8_t stroke = 0U; stroke < 50U; ++stroke) {
        history.Begin(0U, frame);
        frame.emitters[stroke].color = {static_cast<uint8_t>(stroke + 1U),
                                        2U, 3U};
        frame.emitters[stroke].effect = 1U;
        frame.emitters[stroke].effectDurationMs = 100U;
        history.End(0U, frame);
    }
    Check(history.UndoCount() == 50U && history.RedoCount() == 0U,
          "4 KiB Store journal retains 50 sparse strokes with exact boundaries");
    for (uint8_t remaining = 50U; remaining > 0U; --remaining) {
        Check(history.Step(false, 0U, frame),
              "each sparse Store operation remains independently undoable");
    }
    Check(history.UndoCount() == 0U && history.RedoCount() == 50U &&
              frame.emitters[0U].color.r == 0U &&
              frame.emitters[49U].effect == 0U,
          "Store Undo restores exact color, effect and duration preimages");
    for (uint8_t remaining = 50U; remaining > 0U; --remaining) {
        Check(history.Step(true, 0U, frame),
              "the same compact Store records replay Redo");
    }
    Check(history.UndoCount() == 50U && history.RedoCount() == 0U &&
              frame.emitters[49U].color.r == 50U &&
              frame.emitters[49U].effectDurationMs == 100U,
          "Store Redo restores the terminal frame without an object transfer");
    Check(history.Step(false, 1U, frame) == false &&
              history.UndoCount() == 50U,
          "a different displayed frame cannot consume Store history");
    Check(history.Step(false, 0U, frame),
          "one Undo creates a redo branch");
    history.Begin(0U, frame);
    frame.emitters[63U].color = {9U, 8U, 7U};
    history.End(0U, frame);
    Check(history.RedoCount() == 0U && history.UndoCount() == 50U,
          "a new accepted stroke discards only the abandoned redo branch");
}

void TestUnknownStoreHistoryGroupsCompleteFootprints() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    enum class Shape : uint8_t { Uniform, MasksAndColors, Heterogeneous, Effect, Duration, PacketSplit };
    struct ExpectedRecord final {
        uint8_t mask;
        modules::Rgb color;
        uint64_t footprint;
        bool clearEffects;
        uint32_t revision;
    };
    constexpr uint64_t uniformFootprint = UINT64_C(0xffffff) << 8U;
    constexpr uint64_t laterFootprint = UINT64_C(0xffffff) << 32U;
    constexpr modules::Rgb paintColor{71U, 72U, 73U};
    for (const auto shape : {Shape::Uniform, Shape::MasksAndColors,
                            Shape::Heterogeneous, Shape::Effect, Shape::Duration, Shape::PacketSplit}) {
        fxe::ManualMillis::SetNowMs(100U);
        Medium medium;
        store::FrameStore frames(medium.Ops());
        Seed(medium, frames);
        stream::FrameStream pager;
        Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
              "unknown history fixture loads the real committed Store frame");
        modules::LitchcraftSession session;
        session.Configure(3U, 90U);
        session.RestoreArtwork({}, false, 0U);
        session.AttachFrameStream(&pager);
        session.Update();
        store::Frame before{};
        uint16_t displayed = UINT16_MAX;
        Check(pager.CopyDisplayed(before, displayed) && displayed == 0U,
              "unknown history captures its true preimage before the app stroke");
        const auto capturedGeneration = frames.Generation();
        const auto writesBefore = medium.writes;
        constexpr uint32_t epoch = 98U;
        const auto arm = [&]() {
            std::array<uint8_t, 8U> guard{};
            fxe::WriteU32LE(capturedGeneration, guard.data());
            fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
            Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) ==
                      modules::LitchcraftSession::Status::Ok,
                  "unknown history uses the authentic current-generation live Guard");
        };
        const auto paint = [&](const uint8_t square, const uint8_t mask,
                               const modules::Rgb color, const uint8_t flags,
                               const uint16_t stroke) {
            const std::array<uint8_t, 9U> body{
                0U, static_cast<uint8_t>(stroke), static_cast<uint8_t>(stroke >> 8U),
                flags, mask, color.r, color.g, color.b, square};
            Check(session.ApplyConnectionOrdered(3U, body.data(), body.size(), epoch) ==
                      modules::LitchcraftSession::Status::Ok,
                  "the reported Store stroke enters production guarded Paint history");
        };
        const auto drain = [&]() {
            std::array<uint8_t, 234U> bytes{};
            size_t size = 0U;
            while (session.PeekPhysicalStoreBatch(bytes.data(), bytes.size(), size)) {
                session.AcceptPhysicalStoreBatch();
            }
        };
        const auto batch = [&](const char* action, const bool overflow,
                               const std::vector<ExpectedRecord>& expected) {
            std::array<uint8_t, 234U> bytes{};
            size_t size = 0U;
            const bool pending = session.PeekPhysicalStoreBatch(bytes.data(), bytes.size(), size);
            Check(pending && size >= 36U,
                  "unknown history publishes an authentic reverse Store Object batch");
            if (!pending || size < 36U) return;
            const uint8_t count = bytes[14U];
            std::printf("[STORE_HISTORY_FOOTPRINT] shape=%u action=%s t=%u savedG=%u liveR=%u records=%u overflow=%u bytes=%zu\n",
                        static_cast<unsigned>(shape), action, fxe::Millis(),
                        frames.Generation(), pager.LiveRevision(), count, bytes[13U], size);
            Check(bytes[0U] == 0x4dU && bytes[1U] == 2U &&
                      bytes[2U] == modules::LitchcraftSession::PHYSICAL_STORE_BATCH_EVENT &&
                      fxe::ReadU32LE(bytes.data() + 3U) == session.SessionId() &&
                      bytes[13U] == (overflow ? 1U : 0U) && count != 0U && count <= 10U &&
                      size == 15U + 21U * count,
                  "reverse history carries the exact session and truthful bounded repair status");
            if (!expected.empty()) {
                Check(count == expected.size(),
                      "representable history preserves exact consecutive complete mask/color footprints");
                for (const auto& record : expected) {
                    bool found = false;
                    for (uint8_t i = 0U; i < count && 15U + 21U * (i + 1U) <= size; ++i) {
                        const auto* payload = bytes.data() + 15U + 21U * i;
                        found = found ||
                            (fxe::ReadU32LE(payload) == capturedGeneration && payload[4U] == 0U &&
                             payload[5U] == (record.mask | (record.clearEffects ? 0x10U : 0U)) &&
                             payload[6U] == record.color.r && payload[7U] == record.color.g &&
                             payload[8U] == record.color.b &&
                             fxe::ReadU64LE(payload + 9U) == record.footprint &&
                             fxe::ReadU32LE(payload + 17U) == record.revision);
                    }
                    Check(found,
                          "grouped history keeps exact captured G/R/frame/mask/RGB/clear and footprint");
                }
            } else {
                for (uint8_t i = 0U; i < count && 15U + 21U * (i + 1U) <= size; ++i) {
                    const auto* payload = bytes.data() + 15U + 21U * i;
                    Check(fxe::ReadU32LE(payload) == capturedGeneration && payload[4U] == 0U &&
                              fxe::ReadU32LE(payload + 17U) == pager.LiveRevision(),
                          "unrepresentable history repair still names the exact resulting authority");
                }
            }
        };
        arm();
        if (shape == Shape::Uniform || shape == Shape::Heterogeneous ||
            shape == Shape::PacketSplit) {
            const uint8_t squares = shape == Shape::Uniform ? 24U :
                (shape == Shape::PacketSplit ? 11U : 17U);
            for (uint8_t i = 0U; i < squares; ++i) {
                const modules::Rgb color = shape == Shape::Uniform ? paintColor
                    : modules::Rgb{static_cast<uint8_t>(i + 1U),
                                   static_cast<uint8_t>(i + 31U),
                                   static_cast<uint8_t>(i + 61U)};
                paint(static_cast<uint8_t>(8U + i), 15U, color,
                      static_cast<uint8_t>((i == 0U ? 1U : 0U) |
                                           (i + 1U == squares ? 2U : 0U)), 51U);
            }
        } else if (shape == Shape::MasksAndColors) {
            constexpr modules::Rgb secondColor{81U, 82U, 83U};
            for (uint8_t i = 0U; i < 8U; ++i) {
                const uint8_t square = i < 6U ? static_cast<uint8_t>(8U + i) : 14U;
                const uint8_t mask = (i == 1U || i == 3U || i == 7U) ? 2U : 1U;
                paint(square, mask, i >= 4U && i != 6U ? secondColor : paintColor,
                      static_cast<uint8_t>((i == 0U ? 1U : 0U) | (i == 7U ? 2U : 0U)), 51U);
            }
        } else if (shape == Shape::Effect) {
            const std::array<uint8_t, 9U> effect{0U, 1U, 0xf4U, 0x01U, 15U, 51U, 0U, 3U, 8U};
            Check(session.ApplyConnectionOrdered(4U, effect.data(), effect.size(), epoch) ==
                      modules::LitchcraftSession::Status::Ok,
                  "effect fallback is seeded through the same guarded production stroke");
        } else {
            const std::array<uint8_t, 5U> duration{3U, 0U, 0U, 200U, 0U};
            Check(session.ApplyConnectionOrdered(10U, duration.data(), duration.size(), epoch) ==
                      modules::LitchcraftSession::Status::Ok,
                  "duration fallback is seeded through production Store Frame Edit");
        }
        store::Frame afterEdit{};
        Check(session.UndoCount() == 1U && pager.CopyDisplayed(afterEdit, displayed) &&
                  !Same(before, afterEdit) && medium.writes == writesBefore,
              "the actual operation has one reversible boundary without saving or transfer");
        fxe::ManualMillis::SetNowMs(200U);
        arm();
        const auto beforeUndoR = pager.LiveRevision();
        Check(session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
                  modules::LitchcraftSession::Status::Ok &&
                  session.StoreGuardRevision() == beforeUndoR + 1U &&
                  pager.LiveRevision() == beforeUndoR + 1U &&
                  session.UndoCount() == 0U && session.RedoCount() == 1U,
              "unknown Guard/Undo completes immediately with one terminal live revision");
        store::Frame restored{};
        Check(pager.CopyDisplayed(restored, displayed) && displayed == 0U && Same(before, restored),
              "unknown Undo restores the full RAM preimage including effects and duration");
        const auto undoR = pager.LiveRevision();
        if (shape == Shape::Uniform) {
            batch("undo", false, {{15U, {}, uniformFootprint, true, undoR}});
        } else if (shape == Shape::MasksAndColors) {
            batch("undo", false, {
                {1U, {}, UINT64_C(1) << 8U, true, undoR},
                {2U, {}, UINT64_C(1) << 9U, true, undoR},
                {1U, {}, UINT64_C(1) << 10U, true, undoR},
                {2U, {}, UINT64_C(1) << 11U, true, undoR},
                {1U, {}, (UINT64_C(1) << 12U) | (UINT64_C(1) << 13U), true, undoR},
                {3U, {}, UINT64_C(1) << 14U, true, undoR}});
        } else if (shape == Shape::Heterogeneous || shape == Shape::PacketSplit) {
            const uint64_t footprint = shape == Shape::PacketSplit ?
                UINT64_C(0x7ff) << 8U : UINT64_C(0x1ffff) << 8U;
            batch("undo", false, {{15U, {}, footprint, true, undoR}});
        } else if (shape == Shape::Effect) {
            batch("undo", false, {{15U, {}, UINT64_C(1) << 8U, true, undoR}});
        } else batch("undo_duration_repair", true, {});
        drain();
        fxe::ManualMillis::SetNowMs(300U);
        arm();
        Check(session.ApplyConnectionOrdered(7U, nullptr, 0U, epoch) ==
                  modules::LitchcraftSession::Status::Ok &&
                  pager.LiveRevision() == undoR + 1U &&
                  session.StoreGuardRevision() == undoR + 1U &&
                  session.UndoCount() == 1U && session.RedoCount() == 0U &&
                  pager.CopyDisplayed(restored, displayed) && Same(afterEdit, restored),
              "unknown Guard/Redo restores exact terminal RAM artwork immediately");
        const auto redoR = pager.LiveRevision();
        if (shape == Shape::Uniform) {
            batch("redo", false, {{15U, paintColor, uniformFootprint, true, redoR}});
        } else if (shape == Shape::MasksAndColors) {
            batch("redo", false, {
                {1U, paintColor, UINT64_C(1) << 8U, true, redoR},
                {2U, paintColor, UINT64_C(1) << 9U, true, redoR},
                {1U, paintColor, UINT64_C(1) << 10U, true, redoR},
                {2U, paintColor, UINT64_C(1) << 11U, true, redoR},
                {1U, {81U, 82U, 83U}, (UINT64_C(1) << 12U) | (UINT64_C(1) << 13U), true, redoR},
                {1U, paintColor, UINT64_C(1) << 14U, true, redoR},
                {2U, {81U, 82U, 83U}, UINT64_C(1) << 14U, true, redoR}});
        } else if (shape == Shape::PacketSplit) {
            std::vector<ExpectedRecord> prefix;
            for (uint8_t i = 0U; i < 10U; ++i) {
                prefix.push_back({15U, {static_cast<uint8_t>(i + 1U),
                    static_cast<uint8_t>(i + 31U), static_cast<uint8_t>(i + 61U)},
                    UINT64_C(1) << (8U + i), true, redoR});
            }
            batch("redo_11_group_prefix_requires_repair", true, prefix);
            session.AcceptPhysicalStoreBatch();
            batch("redo_11_group_tail", false, {{15U, {11U, 41U, 71U},
                UINT64_C(1) << 18U, true, redoR}});
        } else if (shape == Shape::Heterogeneous) batch("redo_17_color_repair", true, {});
        else if (shape == Shape::Effect) {
            batch("redo_effect_repair", true, {{15U, {}, UINT64_C(1) << 8U, false, redoR}});
        } else batch("redo_duration_repair", true, {});
        if (shape == Shape::Effect) {
            // Keep the existing repair-required effect record unaccepted. A
            // later representable history result must not erase its repair bit.
            fxe::ManualMillis::SetNowMs(400U);
            arm();
            for (uint8_t i = 0U; i < 24U; ++i) {
                paint(static_cast<uint8_t>(32U + i), 15U, paintColor,
                      static_cast<uint8_t>((i == 0U ? 1U : 0U) | (i == 23U ? 2U : 0U)), 52U);
            }
            fxe::ManualMillis::SetNowMs(500U);
            arm();
            Check(session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
                      modules::LitchcraftSession::Status::Ok &&
                      pager.CopyDisplayed(restored, displayed) && Same(afterEdit, restored),
                  "a later uniform inverse leaves the restored non-off effect untouched");
            batch("prior_effect_repair_preserved", true, {
                {15U, {}, UINT64_C(1) << 8U, false, redoR},
                {15U, {}, laterFootprint, true, pager.LiveRevision()}});
        }
        drain();
        if (shape == Shape::PacketSplit) {
            // Nine complete inverses at distinct live revisions fit in one
            // Object. The next two-record inverse must not authorize a prefix.
            fxe::ManualMillis::SetNowMs(400U);
            arm();
            paint(63U, 15U, paintColor, 3U, 52U);
            std::vector<ExpectedRecord> queued;
            for (uint8_t i = 0U; i < 9U; ++i) {
                fxe::ManualMillis::SetNowMs(410U + i);
                arm();
                Check(session.ApplyConnectionOrdered(i % 2U == 0U ? 6U : 7U,
                          nullptr, 0U, epoch) == modules::LitchcraftSession::Status::Ok,
                      "queued reverse records arise from real distinct-R history steps");
                queued.push_back({15U, i % 2U == 0U ? modules::Rgb{} : paintColor,
                    UINT64_C(1) << 63U, true, pager.LiveRevision()});
            }
            batch("nine_complete_prior_inverses", false, queued);
            fxe::ManualMillis::SetNowMs(450U);
            arm();
            paint(32U, 1U, paintColor, 1U, 53U);
            paint(33U, 2U, paintColor, 2U, 53U);
            fxe::ManualMillis::SetNowMs(500U);
            arm();
            Check(session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
                      modules::LitchcraftSession::Status::Ok &&
                      pager.CopyDisplayed(restored, displayed) && Same(afterEdit, restored),
                  "two-mask inverse restores exact RAM after nine pending complete inverses");
            queued.push_back({1U, {}, UINT64_C(1) << 32U, true, pager.LiveRevision()});
            batch("nine_prior_two_same_R_prefix_requires_repair", true, queued);
            session.AcceptPhysicalStoreBatch();
            batch("two_same_R_tail", false, {{2U, {}, UINT64_C(1) << 33U,
                true, pager.LiveRevision()}});
            drain();

            // A full ten-record Object ending at a whole inverse boundary is
            // complete even when the next inverse remains queued.
            fxe::ManualMillis::SetNowMs(600U);
            arm();
            paint(63U, 15U, paintColor, 3U, 54U);
            queued.clear();
            for (uint8_t i = 0U; i < 11U; ++i) {
                fxe::ManualMillis::SetNowMs(610U + i);
                arm();
                Check(session.ApplyConnectionOrdered(i % 2U == 0U ? 6U : 7U,
                          nullptr, 0U, epoch) == modules::LitchcraftSession::Status::Ok,
                      "whole-boundary fixture queues eleven real one-record inverses");
                if (i < 10U) {
                    queued.push_back({15U, i % 2U == 0U ? modules::Rgb{} : paintColor,
                        UINT64_C(1) << 63U, true, pager.LiveRevision()});
                }
            }
            batch("ten_complete_inverse_boundaries", false, queued);
            session.AcceptPhysicalStoreBatch();
            batch("next_complete_inverse_boundary", false, {{15U, {},
                UINT64_C(1) << 63U, true, pager.LiveRevision()}});
            Check(pager.CopyDisplayed(restored, displayed) && Same(afterEdit, restored),
                  "complete Object boundaries retain the exact final RAM artwork");
            drain();
        }
        const auto lastEditAt = fxe::Millis();
        const auto terminalR = pager.LiveRevision();
        fxe::ManualMillis::SetNowMs(lastEditAt + stream::FrameStream::EDIT_SAVE_QUIET_MS - 1U);
        // The production storage worker gates Service on EditSaveReady; calling
        // Service directly here would deliberately bypass its quiet period.
        store::Frame durable{};
        Check(!pager.EditSaveReady(fxe::Millis()) &&
                  frames.Generation() == capturedGeneration && pager.Generation() == capturedGeneration &&
                  pager.LiveRevision() == terminalR && medium.writes == writesBefore &&
                  pager.HasUnpersistedEdit() && frames.ReadFrame(0U, durable) == store::Status::Ok &&
                  Same(durable, before),
              "reverse repair and grouped records do not force persistence before the quiet period");
        std::printf("[STORE_HISTORY_DURABILITY] shape=%u phase=before_quiet t=%u savedG=%u liveR=%u writes=%zu dirty=%u\n",
                    static_cast<unsigned>(shape), fxe::Millis(), frames.Generation(),
                    pager.LiveRevision(), medium.writes - writesBefore, pager.HasUnpersistedEdit());
        fxe::ManualMillis::AdvanceMs(1U);
        Check(pager.EditSaveReady(fxe::Millis()),
              "history becomes eligible for the actual worker at exactly five quiet seconds");
        (void)pager.Service(frames, false);
        Check(frames.Generation() == capturedGeneration + 1U &&
                  pager.Generation() == capturedGeneration + 1U &&
                  pager.LiveRevision() == terminalR && !pager.HasUnpersistedEdit() &&
                  medium.writes > writesBefore && frames.ReadFrame(0U, durable) == store::Status::Ok &&
                  Same(durable, afterEdit),
              "the worker's production Store service persists exact history without changing live R");
        std::printf("[STORE_HISTORY_DURABILITY] shape=%u phase=after_quiet t=%u savedG=%u liveR=%u writes=%zu dirty=%u\n",
                    static_cast<unsigned>(shape), fxe::Millis(), frames.Generation(),
                    pager.LiveRevision(), medium.writes - writesBefore, pager.HasUnpersistedEdit());
    }
}

void TestPhysicalStoreUndoPublishesReverseEdit() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "physical Undo journey loads the committed Store frame");
    modules::LitchcraftSession session;
    session.Configure(3U, 47U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    store::Frame before{};
    uint16_t beforeIndex = store::FRAME_COUNT;
    Check(pager.CopyDisplayed(before, beforeIndex) && beforeIndex == 0U,
          "physical Undo journey captures the displayed frame");
    const uint64_t occupied = UINT64_C(1) << 7U;
    session.ObserveOccupancy(occupied);
    session.ObserveOccupancy(0U);
    fxe::ManualMillis::SetNowMs(400U);
    session.Update();
    Check(session.UndoCount() == 1U,
          "physical paint completes one Store Undo boundary after lift");
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    session.Compose(board, status);
    Check(board[7U].left.r == session.Color().r,
          "physical paint is visible before Undo");
    std::array<uint8_t, 6U> selectFrameOne{2U, 1U};
    fxe::WriteU32LE(frames.Generation(), selectFrameOne.data() + 2U);
    Check(session.ApplyConnectionOrdered(
              2U, selectFrameOne.data(), selectFrameOne.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "selected frame moves away from the edited frame");
    for (uint8_t attempt = 0U; attempt < 8U &&
            pager.DisplayedIndex() != 1U; ++attempt) {
        (void)LoadOne(pager, frames);
        session.Update();
    }
    Check(pager.DisplayedIndex() == 1U,
          "Store worker presents a different frame before physical Undo");
    session.ObserveButton({modules::UserButton::P1X,
                           fxe::Button::Event::Press,
                           modules::UiOwner::P1});
    session.ObserveButton({modules::UserButton::P1X,
                           fxe::Button::Event::Release,
                           modules::UiOwner::P1});
    session.Update();
    session.Compose(board, status);
    Check(session.UndoCount() == 0U && session.RedoCount() == 1U &&
              session.SelectedFrame() == 0U &&
              board[7U].left.r == before.emitters[28U].color.r,
          "physical X returns to its edit frame and reverses Store history");
    std::array<uint8_t, store::STORE_RESPONSE_MAX_BYTES> batch{};
    size_t size = 0U;
    Check(session.PeekPhysicalStoreBatch(batch.data(), batch.size(), size) &&
              batch[14U] >= 2U &&
              fxe::ReadU32LE(batch.data() + 7U) >= 2U,
          "physical X publishes a reverse Store edit for the phone");
    session.ConfigureShakeErase(true);
    session.ObserveMotionSample(0, 0, 1000);
    for (uint8_t sample = 0U; sample < 27U; ++sample) {
        fxe::ManualMillis::SetNowMs(440U + sample * 40U);
        const int32_t direction = sample % 2U == 0U ? 700 : -700;
        session.ObserveMotionSample(direction, -direction / 2, 1000);
    }
    store::Frame erased{};
    uint16_t erasedIndex = store::FRAME_COUNT;
    Check(session.LastShakeEraseToken() != 0U &&
              pager.CopyDisplayed(erased, erasedIndex) &&
              erasedIndex == 0U &&
              erased.emitters[28U].color.r == 0U &&
              erased.emitters[28U].effect == 0U &&
              session.UndoCount() == 1U && session.RedoCount() == 0U,
          "deliberate V3 shake clears Store artwork as one undoable action");
    pager.RequestImmediateSave();
    Check(pager.Service(frames, false) ==
              stream::FrameStream::ServiceResult::Idle &&
              !pager.HasUnpersistedEdit(),
          "cold Undo fixture persists the edited target before evicting it");
    selectFrameOne[1U] = 2U;
    fxe::WriteU32LE(frames.Generation(), selectFrameOne.data() + 2U);
    Check(session.ApplyConnectionOrdered(
              2U, selectFrameOne.data(), selectFrameOne.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "deferred Undo exit journey selects a different frame");
    for (uint8_t attempt = 0U; attempt < 8U &&
            pager.DisplayedIndex() != 2U; ++attempt) {
        (void)LoadOne(pager, frames);
        session.Update();
    }
    Check(pager.DisplayedIndex() == 2U,
          "first different frame presents before cold-page fixture");
    selectFrameOne[1U] = 3U;
    fxe::WriteU32LE(frames.Generation(), selectFrameOne.data() + 2U);
    Check(session.ApplyConnectionOrdered(
              2U, selectFrameOne.data(), selectFrameOne.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "second different frame evicts the Undo target from RAM");
    for (uint8_t attempt = 0U; attempt < 8U &&
            pager.DisplayedIndex() != 3U; ++attempt) {
        (void)LoadOne(pager, frames);
        session.Update();
    }
    Check(pager.DisplayedIndex() == 3U,
          "cold-page fixture presents frame three before physical X");
    session.ObserveButton({modules::UserButton::P1X,
                           fxe::Button::Event::Press,
                           modules::UiOwner::P1});
    session.ObserveButton({modules::UserButton::P1X,
                           fxe::Button::Event::Release,
                           modules::UiOwner::P1});
    fxe::ManualMillis::SetNowMs(7000U);
    session.Update();
    Check(session.UndoCount() == 1U && session.SelectedFrame() == 3U,
          "stalled cold frame read expires X without consuming Store history");
    session.ObserveButton({modules::UserButton::P1X,
                           fxe::Button::Event::Press,
                           modules::UiOwner::P1});
    session.ObserveButton({modules::UserButton::P1X,
                           fxe::Button::Event::Release,
                           modules::UiOwner::P1});
    session.SetMutationsPaused(true);
    session.SetMutationsPaused(false);
    session.Update();
    Check(session.UndoCount() == 1U,
          "pause and resume cancel a deferred physical Undo");
    fxe::WriteU32LE(frames.Generation(), selectFrameOne.data() + 2U);
    Check(session.ApplyConnectionOrdered(
              2U, selectFrameOne.data(), selectFrameOne.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "exit journey reselects another Store frame");
    for (uint8_t attempt = 0U; attempt < 8U &&
            pager.DisplayedIndex() != 3U; ++attempt) {
        (void)LoadOne(pager, frames);
        session.Update();
    }
    session.ObserveButton({modules::UserButton::P1X,
                           fxe::Button::Event::Press,
                           modules::UiOwner::P1});
    session.ObserveButton({modules::UserButton::P1X,
                           fxe::Button::Event::Release,
                           modules::UiOwner::P1});
    session.Exit();
    session.Update();
    Check(!session.IsActive() && session.UndoCount() == 1U,
          "Exit cancels a deferred physical Undo before it can change Store");
}

void TestOrderedStoreHistoryPresentationAndCancellation() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
          "ordered cold history begins at a presented Store frame");
    modules::LitchcraftSession session;
    session.Configure(3U, 47U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    session.ObserveOccupancy(UINT64_C(1) << 7U);
    session.ObserveOccupancy(0U);
    fxe::ManualMillis::SetNowMs(400U);
    session.Update();
    pager.RequestImmediateSave();
    (void)pager.Service(frames, false);
    const auto evictTarget = [&]() {
        for (uint8_t frame : {1U, 2U, 3U}) {
            std::array<uint8_t, 6U> select{2U, frame};
            fxe::WriteU32LE(frames.Generation(), select.data() + 2U);
            Check(session.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
                      modules::LitchcraftSession::Status::Ok,
                  "cold ordered history selects another frame");
            for (uint8_t attempt = 0U; attempt < 8U && pager.DisplayedIndex() != frame; ++attempt) {
                (void)LoadOne(pager, frames);
                session.Update();
            }
        }
    };
    constexpr uint32_t epoch = 91U;
    const auto beginUndo = [&]() {
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(frames.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) ==
                  modules::LitchcraftSession::Status::Ok &&
                  session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
                  modules::LitchcraftSession::Status::Busy &&
                  session.IsStoreHistoryPending() && session.UndoCount() == 1U,
              "cold ordered Undo waits for its journal frame without consuming history");
    };
    evictTarget();
    std::array<uint8_t, 6U> runtime{1U, 3U};
    fxe::WriteU32LE(frames.Generation(), runtime.data() + 2U);
    Check(session.ApplyConnectionOrdered(2U, runtime.data(), runtime.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "cold history lifecycle starts real playback on frame three");
    fxe::ManualMillis::AdvanceMs(113U);
    session.Update();
    std::printf("[LATE_FRAME_COLD_HISTORY_PENDING] at=%u selected=%u displayed=%u requested=%u elapsed=%u\n",
                fxe::Millis(), session.SelectedFrame(), pager.DisplayedIndex(),
                pager.RequestedIndex(), session.AnimationFrameElapsedMs());
    Check(session.SelectedFrame() == 3U && pager.DisplayedIndex() == 3U &&
              pager.RequestedIndex() == 0U &&
              session.AnimationFrameElapsedMs() == 112U,
          "cold history lifecycle reaches an expired displayed frame and cold successor");
    runtime[0U] = 0U;
    Check(session.ApplyConnectionOrdered(2U, runtime.data(), runtime.size()) ==
              modules::LitchcraftSession::Status::Ok &&
              !session.IsAnimationPlaying(),
          "cold history lifecycle pauses the pending playback successor");
    beginUndo();
    session.Update();
    Check(session.UndoCount() == 1U,
          "physical Update cannot execute the pending Ordered history intent");
    for (uint8_t attempt = 0U; attempt < 8U && pager.DisplayedIndex() != 0U; ++attempt) {
        (void)LoadOne(pager, frames);
        session.Update();
    }
    std::printf("[LATE_FRAME_COLD_HISTORY] at=%u selected=%u displayed=%u requested=%u elapsed=%u\n",
                fxe::Millis(), session.SelectedFrame(), pager.DisplayedIndex(),
                pager.RequestedIndex(), session.AnimationFrameElapsedMs());
    Check(session.SelectedFrame() == 0U && pager.DisplayedIndex() == 0U &&
              session.AnimationFrameElapsedMs() == 0U,
          "cold history selection retires pending playback and adopts a fresh paused anchor");
    Check(session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
              modules::LitchcraftSession::Status::Ok &&
              !session.IsStoreHistoryPending() && session.RedoCount() == 1U,
          "presented journal frame completes the same Ordered Undo once");
    runtime[0U] = 1U;
    runtime[1U] = 0U;
    Check(session.ApplyConnectionOrdered(2U, runtime.data(), runtime.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "history-selected frame resumes through the normal ordered owner");
    fxe::ManualMillis::AdvanceMs(25U);
    session.Update();
    Check(session.SelectedFrame() == 0U &&
              session.AnimationFrameElapsedMs() == 25U,
          "resumed history selection uses its fresh frame-local anchor");
    runtime[0U] = 0U;
    Check(session.ApplyConnectionOrdered(2U, runtime.data(), runtime.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "history-selected frame pauses before ordinary Redo");
    Check(session.ApplyConnectionOrdered(7U, nullptr, 0U, epoch) ==
              modules::LitchcraftSession::Status::Ok,
          "ordered Redo restores the cancellation fixture");
    pager.RequestImmediateSave();
    (void)pager.Service(frames, false);
    evictTarget();
    beginUndo();
    session.CancelPendingStoreHistory();
    session.Update();
    Check(!session.IsStoreHistoryPending() && session.UndoCount() == 1U &&
              session.SelectedFrame() == pager.DisplayedIndex() &&
              pager.RequestedIndex() == pager.DisplayedIndex(),
          "disconnect or eligibility cancellation preserves the board journal");
    evictTarget();
    beginUndo();
    session.Exit();
    Check(session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
              modules::LitchcraftSession::Status::Busy &&
              !session.IsStoreHistoryPending() && session.UndoCount() == 1U,
          "Exit cancels ordered Undo before the journal can change");
    const std::array<uint8_t, 8U> enter{
        0x4cU, static_cast<uint8_t>(modules::LitchcraftSession::Op::Enter),
        0U, 0U, 0U, 0U, 1U, 0U};
    session.HandlePacket(enter.data(), enter.size(), true, 0U);
    session.Update();
    evictTarget();
    beginUndo();
    fxe::ManualMillis::AdvanceMs(5001U);
    Check(session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) ==
              modules::LitchcraftSession::Status::ResourceLimit &&
              !session.IsStoreHistoryPending() && session.UndoCount() == 1U &&
              session.SelectedFrame() == pager.DisplayedIndex() &&
              pager.RequestedIndex() == pager.DisplayedIndex(),
          "stalled ordered frame read rejects after the existing five-second bound");
    session.ObserveOccupancy(UINT64_C(1) << 8U);
    store::Frame recovered{};
    uint16_t recoveredFrame = UINT16_MAX;
    Check(pager.CopyDisplayed(recovered, recoveredFrame) &&
              recoveredFrame == 3U &&
              recovered.emitters[32U].color.r == session.Color().r &&
              recovered.emitters[32U].color.g == session.Color().g,
          "a failed cold Undo keeps the displayed frame physically paintable");
    session.ObserveOccupancy(0U);
    fxe::ManualMillis::AdvanceMs(400U);
    session.Update();
    fxe::ManualMillis::AdvanceMs(6000U);
    Check(pager.Service(frames, false) == stream::FrameStream::ServiceResult::Idle &&
              !pager.HasUnpersistedEdit() &&
              frames.ReadFrame(3U, recovered) == store::Status::Ok &&
              recovered.emitters[32U].color.r == session.Color().r &&
              recovered.emitters[32U].color.g == session.Color().g,
          "normal persistence saves the retained-frame mark after failed cold Undo");
}

void TestVirtualLitchStorePhysicalUndoJourney() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested,
          "VirtualLitch Store Undo journey loads its authoritative frame");
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession virtualBoard(config);
    virtualBoard.ObserveBootHallMask(0U);
    Check(virtualBoard.Advance(3000U) == VIRTUAL_LITCH_OK,
          "VirtualLitch Store Undo journey starts in empty Setup");
    auto& art = virtualBoard.Litchcraft();
    art.AttachFrameStream(&pager);
    std::array<uint8_t, 8U> enter{};
    enter[0U] = art.MAGIC;
    enter[1U] = static_cast<uint8_t>(modules::LitchcraftSession::Op::Enter);
    virtualBoard.SendLitchcraftPacket(enter.data(), enter.size());
    Check(virtualBoard.Advance(1U) == VIRTUAL_LITCH_OK && art.IsActive(),
          "VirtualLitch Store Undo enters through the production mode gate");
    store::Frame before{};
    uint16_t index = store::FRAME_COUNT;
    Check(pager.CopyDisplayed(before, index) && index == 0U,
          "VirtualLitch Store Undo begins at the presented root");
    Check(virtualBoard.ObserveHallMask(UINT64_C(1) << 7U) ==
              VIRTUAL_LITCH_OK &&
              virtualBoard.Advance(20U) == VIRTUAL_LITCH_OK &&
              virtualBoard.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              virtualBoard.Advance(400U) == VIRTUAL_LITCH_OK,
          "VirtualLitch Hall placement and lift finish one paint stroke");
    Check(art.UndoCount() == 1U &&
              pager.CopyDisplayed(before, index) &&
              before.emitters[28U].color.r == art.Color().r,
          "VirtualLitch authoritative Store frame contains the physical mark");
    const auto paintedLed = virtualBoard.SemanticFrame().board[7U][0U];
    std::printf("[LITCHCRAFT_STORE_UNDO_JOURNEY] stage=paint t=%u "
                "occupancy=%016llx undo=%u led=%u,%u,%u next=lift_then_X\n",
                virtualBoard.State().now_ms,
                static_cast<unsigned long long>(art.Occupancy()),
                art.UndoCount(), paintedLed.r, paintedLed.g, paintedLed.b);
    Check(virtualBoard.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
              runtime::VirtualLitchPhysicalButton::X, true) ==
              VIRTUAL_LITCH_OK &&
              virtualBoard.Advance(20U) == VIRTUAL_LITCH_OK &&
              virtualBoard.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
              runtime::VirtualLitchPhysicalButton::X, false) ==
              VIRTUAL_LITCH_OK &&
              virtualBoard.Advance(20U) == VIRTUAL_LITCH_OK,
          "VirtualLitch X press and release reach the production board route");
    Check(art.UndoCount() == 0U && art.RedoCount() == 1U &&
              pager.CopyDisplayed(before, index) &&
              before.emitters[28U].color.r != art.Color().r,
          "VirtualLitch X restores the board's prior Store pixels");
    const auto semantic = virtualBoard.SemanticFrame().board[7U][0U];
    Check(paintedLed.r != semantic.r || paintedLed.g != semantic.g ||
              paintedLed.b != semantic.b,
          "VirtualLitch timed LED changes when physical X restores the frame");
    std::printf("[LITCHCRAFT_STORE_UNDO_JOURNEY] t=%u mode=%u occupancy=%016llx "
                "undo=%u redo=%u frame=%u led=%u,%u,%u display=\"%.*s\" "
                "next=paint_or_redo\n",
                virtualBoard.State().now_ms, art.IsActive() ? 1U : 0U,
                static_cast<unsigned long long>(art.Occupancy()),
                art.UndoCount(), art.RedoCount(), art.SelectedFrame(),
                semantic.r, semantic.g, semantic.b,
                static_cast<int>(virtualBoard.DisplayText().size()),
                virtualBoard.DisplayText().data());
    Check(virtualBoard.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
              runtime::VirtualLitchPhysicalButton::X, true) ==
              VIRTUAL_LITCH_OK &&
              virtualBoard.Advance(20U) == VIRTUAL_LITCH_OK &&
              virtualBoard.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
              runtime::VirtualLitchPhysicalButton::X, false) ==
              VIRTUAL_LITCH_OK &&
              virtualBoard.Advance(20U) == VIRTUAL_LITCH_OK &&
              art.UndoCount() == 0U,
          "VirtualLitch exhausted X is rejected without changing Store");
    Check(virtualBoard.ObserveHallMask(UINT64_C(1) << 8U) ==
              VIRTUAL_LITCH_OK &&
              virtualBoard.Advance(20U) == VIRTUAL_LITCH_OK &&
              virtualBoard.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              virtualBoard.Advance(400U) == VIRTUAL_LITCH_OK &&
              art.UndoCount() == 1U,
          "VirtualLitch recovers from exhausted Undo with the next stroke");
    std::printf("[LITCHCRAFT_STORE_UNDO_JOURNEY] stage=recovery t=%u "
                "occupancy=%016llx undo=%u next=X_available\n",
                virtualBoard.State().now_ms,
                static_cast<unsigned long long>(art.Occupancy()),
                art.UndoCount());
}

void TestVirtualLitchDirtyFrameSeekJourney() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) ==
                  stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(),
          "dirty seek journey presents the saved first frame");
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
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && art.IsActive(),
          "dirty seek journey restores saved Litchcraft on empty board");
    art.RestoreArtwork({}, true, 0U, nullptr, true);
    art.Update();
    Check(board.ObserveHallMask(UINT64_C(1) << 7U) == VIRTUAL_LITCH_OK &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              board.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              board.Advance(400U) == VIRTUAL_LITCH_OK &&
              pager.HasUnpersistedEdit(),
          "physical placement paints before any Store save");
    const auto paintedLed = board.SemanticFrame().board[7U][0U];
    const size_t writesBeforeSeek = medium.writes;
    std::array<uint8_t, 6U> select{2U, 1U};
    fxe::WriteU32LE(frames.Generation(), select.data() + 2U);
    const auto seekStatus = art.ApplyConnectionOrdered(
        2U, select.data(), select.size());
    const auto pageStatus = LoadOne(pager, frames);
    const auto renderStatus = board.Advance(20U);
    Check(seekStatus == modules::LitchcraftSession::Status::Ok &&
              art.SelectedFrame() == 1U &&
              pageStatus == stream::FrameStream::ServiceResult::LoadedRequested &&
              renderStatus == VIRTUAL_LITCH_OK &&
              pager.DisplayedIndex() == 1U &&
              medium.writes == writesBeforeSeek,
          "ordered frame tap renders the requested frame before full-body save");
    const auto soughtLed = board.SemanticFrame().board[7U][0U];
    Check(soughtLed.r != paintedLed.r || soughtLed.g != paintedLed.g ||
              soughtLed.b != paintedLed.b,
          "semantic LEDs show the new frame after one board render turn");
    std::printf("[LITCHCRAFT_DIRTY_SEEK_JOURNEY] stage=seek t=%u "
                "occupancy=%016llx mode=%u result=accepted frame=%u "
                "led=%u,%u,%u display=\"%.*s\" "
                "next=return_to_paint expiry=none\n",
                board.State().now_ms,
                static_cast<unsigned long long>(art.Occupancy()),
                art.IsActive() ? 1U : 0U, pager.DisplayedIndex(),
                soughtLed.r, soughtLed.g, soughtLed.b,
                static_cast<int>(board.DisplayText().size()),
                board.DisplayText().data());
    select[2U] ^= 1U;
    Check(art.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
              modules::LitchcraftSession::Status::RevisionConflict &&
              art.SelectedFrame() == 1U,
          "stale frame tap is rejected without changing the displayed frame");
    std::printf("[LITCHCRAFT_DIRTY_SEEK_JOURNEY] stage=stale t=%u "
                "occupancy=%016llx mode=%u result=rejected frame=%u "
                "display=\"%.*s\" next=retry_with_current_generation "
                "expiry=none\n",
                board.State().now_ms,
                static_cast<unsigned long long>(art.Occupancy()),
                art.IsActive() ? 1U : 0U, pager.DisplayedIndex(),
                static_cast<int>(board.DisplayText().size()),
                board.DisplayText().data());
    select[2U] ^= 1U;
    auto priorFrameLed = board.SemanticFrame().board[0U][0U];
    for (uint8_t next = 2U; next <= 3U; ++next) {
        select[1U] = next;
        const auto accepted = art.ApplyConnectionOrdered(
            2U, select.data(), select.size());
        const auto page = LoadOne(pager, frames);
        const auto rendered = board.Advance(20U);
        const auto nextFrameLed = board.SemanticFrame().board[0U][0U];
        Check(accepted == modules::LitchcraftSession::Status::Ok &&
                  page == stream::FrameStream::ServiceResult::LoadedRequested &&
                  rendered == VIRTUAL_LITCH_OK &&
                  pager.DisplayedIndex() == next &&
                  medium.writes == writesBeforeSeek &&
                  (nextFrameLed.r != priorFrameLed.r ||
                   nextFrameLed.g != priorFrameLed.g ||
                   nextFrameLed.b != priorFrameLed.b),
              "rapid ordered frame tap changes LEDs before any full-body save");
        std::printf("[LITCHCRAFT_DIRTY_SEEK_JOURNEY] stage=scan t=%u "
                    "occupancy=%016llx mode=%u result=accepted frame=%u "
                    "led=%u,%u,%u next=select_or_return expiry=none\n",
                    board.State().now_ms,
                    static_cast<unsigned long long>(art.Occupancy()),
                    art.IsActive() ? 1U : 0U, pager.DisplayedIndex(),
                    nextFrameLed.r, nextFrameLed.g, nextFrameLed.b);
        priorFrameLed = nextFrameLed;
    }
    select[1U] = 0U;
    fxe::WriteU32LE(frames.Generation(), select.data() + 2U);
    Check(art.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
              modules::LitchcraftSession::Status::Ok &&
              board.Advance(20U) == VIRTUAL_LITCH_OK &&
              pager.DisplayedIndex() == 0U,
          "valid frame tap recovers the still-resident painted frame");
    const auto returnedLed = board.SemanticFrame().board[7U][0U];
    Check(returnedLed.r == paintedLed.r &&
              returnedLed.g == paintedLed.g &&
              returnedLed.b == paintedLed.b,
          "returning to the edited frame restores its visible paint");
    std::printf("[LITCHCRAFT_DIRTY_SEEK_JOURNEY] stage=return t=%u "
                "occupancy=%016llx mode=%u result=accepted frame=%u "
                "led=%u,%u,%u display=\"%.*s\" next=save expiry=none\n",
                board.State().now_ms,
                static_cast<unsigned long long>(art.Occupancy()),
                art.IsActive() ? 1U : 0U, pager.DisplayedIndex(),
                returnedLed.r, returnedLed.g, returnedLed.b,
                static_cast<int>(board.DisplayText().size()),
                board.DisplayText().data());
}
void TestNewCanvasAtStoragePublicationBoundaries() {
    using Stream = stream::FrameStream;
    struct Interleave final {
        Stream* pager = nullptr;
        bool newAccepted = false;
        bool paintAccepted = false;
        uint32_t newEpoch = 0U;
        static void New(void* context) {
            auto& self = *static_cast<Interleave*>(context);
            self.newAccepted = self.pager->NewBlankDocument(
                self.pager->Generation(), self.pager->LiveRevision(),
                modules::LITCHCRAFT_DEFAULT_QUICK_PALETTE) == Stream::GuardedEditResult::Changed;
            self.newEpoch = self.pager->DocumentEpoch();
        }
        static void Paint(void* context) {
            auto& self = *static_cast<Interleave*>(context);
            self.paintAccepted = self.pager->PaintDisplayedIfRevision(
                self.pager->LiveRevision(), 0U, 63U, 15U, {94U, 35U, 17U}, true) ==
                Stream::GuardedEditResult::Changed;
        }
        static void NewAndPaint(void* context) { New(context); Paint(context); }
    };
    // These callbacks execute the actual render-side mutation while production
    // paging/save is held at its storage callback, before it can publish.
    for (uint8_t scenario = 0U; scenario < 5U; ++scenario) {
        Medium medium;
        store::FrameStore frames(medium.Ops());
        Seed(medium, frames);
        Stream pager;
        Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == Stream::ServiceResult::LoadedRequested &&
                  pager.TryPresentRequested(), "New interleave fixture presents old frame");
        Interleave interleave{&pager};
        medium.onceOnSyncContext = &interleave;
        const uint32_t oldEpoch = pager.DocumentEpoch();
        if (scenario == 0U) {
            Check(pager.Request(1U), "old page begins before New");
            medium.onceOnRead = &Interleave::New;
        } else if (scenario == 1U) {
            Check(pager.PaintDisplayed(0U, 0U, 15U, {83U, 34U, 12U}, true),
                  "old edit begins before New");
            medium.onceOnSync = &Interleave::New;
        } else {
            Interleave::New(&interleave);
            if (scenario == 2U) medium.onceOnSync = &Interleave::Paint;
            if (scenario == 3U) medium.onceOnSync = &Interleave::NewAndPaint;
            if (scenario == 4U) medium.failRootSyncOnce = true;
        }
        (void)pager.Service(frames, false);
        store::Frame shown{};
        uint16_t index = 99U;
        Check(interleave.newAccepted && pager.DocumentEpoch() != oldEpoch &&
                  pager.CopyDisplayed(shown, index) && index == 0U &&
                  pager.FrameCount() == 1U && shown.emitters[0U].color.r == 0U,
              "old page/save completion cannot publish over the new one-frame canvas");
        if (scenario == 2U || scenario == 3U) {
            Check(interleave.paintAccepted && pager.HasUnpersistedEdit() &&
                      shown.emitters[252U].color.r == 94U,
                  "paint during blank commit retains its own dirty sequence");
        }
        if (scenario == 3U) {
            Check(pager.HasPendingBlankDocument() &&
                      pager.DocumentEpoch() == interleave.newEpoch,
                  "first New commit cannot settle a newer New epoch");
        }
        for (uint8_t attempt = 0U; attempt < 12U && pager.HasUnpersistedEdit(); ++attempt) {
            (void)pager.Service(frames, false);
        }
        Check(!pager.HasUnpersistedEdit() && pager.Generation() == frames.Generation() &&
                  frames.CurrentManifest().frameCount == 1U &&
                  pager.CopyDisplayed(shown, index) && shown.emitters[0U].color.r == 0U &&
                  ((scenario != 2U && scenario != 3U) || shown.emitters[252U].color.r == 94U),
              "New publication reconciles old work and uncertain root sync without losing new paint");
        store::Frame durable{};
        Check(frames.ReadFrame(0U, durable) == store::Status::Ok && Same(durable, shown),
              "settled New has exact durable/live content parity");
    }
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    Stream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == Stream::ServiceResult::LoadedRequested &&
              pager.NewBlankDocument(pager.Generation(), pager.LiveRevision(),
                  modules::LITCHCRAFT_DEFAULT_QUICK_PALETTE) == Stream::GuardedEditResult::Changed,
          "New retirement fixture accepts blank");
    medium.retireOnSync = &pager;
    (void)pager.Service(frames, false);
    Check(pager.Generation() == 0U && !pager.HasPendingBlankDocument() &&
              pager.RetiredGeneration() == frames.Generation() && !pager.TryPresentRequested(),
          "durable blank completion cannot resurrect explicitly retired document");
}

void TestStructuralEditQueuedBehindInstantNew() {
    using Stream = stream::FrameStream;
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    Stream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == Stream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested() &&
              pager.NewBlankDocument(pager.Generation(), pager.LiveRevision(),
                  modules::LITCHCRAFT_DEFAULT_QUICK_PALETTE) == Stream::GuardedEditResult::Changed,
          "structural New fixture accepts its live blank before housekeeping");
    const auto epoch = pager.DocumentEpoch();
    uint16_t selected = 0U;
    uint32_t revision = 0U;
    Check(pager.QueueStructuralEdit({pager.Generation(), pager.LiveRevision(),
              0U, 0U, Stream::FrameEditAction::AddCopy}) == Stream::StructuralResult::Pending &&
              pager.TakeStructuralResult(selected, revision) == Stream::StructuralResult::Changed &&
              selected == 1U && pager.FrameCount() == 2U && pager.HasPendingBlankDocument(),
          "Add following instant New is live before either baseline or layout save");
    (void)pager.Service(frames, false);
    Check(!pager.HasPendingBlankDocument() && pager.HasUnpersistedEdit() &&
              pager.FrameCount() == 2U && pager.DocumentEpoch() == epoch,
          "blank baseline completion retains newer Add and its dirty identity");
    Check(pager.QueueStructuralEdit({pager.Generation(), pager.LiveRevision(),
              1U, 0U, Stream::FrameEditAction::Delete}) == Stream::StructuralResult::Pending &&
              pager.TakeStructuralResult(selected, revision) == Stream::StructuralResult::Changed &&
              selected == 0U && pager.FrameCount() == 1U,
          "Delete after unsaved Add binds the inserted RAM identity");
    for (uint8_t attempt = 0U; attempt < 16U && pager.HasUnpersistedEdit(); ++attempt) {
        (void)pager.Service(frames, false);
    }
    Check(!pager.HasUnpersistedEdit() && pager.FrameCount() == 1U &&
              pager.DocumentEpoch() == epoch && frames.CurrentManifest().frameCount == 1U,
          "New Add Delete converges without replacing the live artwork lifetime");
}

void TestNewCanvasPhysicalStrokeAndDeparture() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
          "New departure fixture loads old artwork");
    modules::LitchcraftSession session;
    session.Configure(3U, 87U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    session.ObserveOccupancy(UINT64_C(1) << 7U);
    std::array<uint8_t, 8U> intent{};
    fxe::WriteU32LE(pager.Generation(), intent.data());
    fxe::WriteU32LE(pager.LiveRevision(), intent.data() + 4U);
    const auto prior = pager.DisplayedFingerprint();
    Check(session.ApplyConnectionOrdered(11U, intent.data(), intent.size(), 202U) ==
              modules::LitchcraftSession::Status::Busy && pager.DisplayedFingerprint() == prior,
          "held physical brush refuses New without ending or clearing its current stroke");
    session.ObserveOccupancy(0U);
    fxe::ManualMillis::SetNowMs(600U);
    session.Update();
    fxe::WriteU32LE(pager.LiveRevision(), intent.data() + 4U);
    Check(session.ApplyConnectionOrdered(11U, intent.data(), intent.size(), 202U) ==
              modules::LitchcraftSession::Status::Ok && pager.HasPendingBlankDocument(),
          "released physical brush can accept New through the same Ordered owner");
    session.EndConnection();
    session.Exit();
    Check(!session.IsActive() && pager.HasPendingBlankDocument(),
          "disconnect and immediate mode exit keep the acknowledged New pending for independent persistence");
    for (uint8_t attempt = 0U; attempt < 12U && pager.HasUnpersistedEdit(); ++attempt) {
        (void)pager.Service(frames, false);
    }
    Check(!session.IsActive() && !pager.HasUnpersistedEdit() &&
              frames.CurrentManifest().frameCount == 1U &&
              pager.SaveModeRoot(frames, false) == store::Status::Ok &&
              !frames.CurrentManifest().active,
          "saving New and the existing exit root cannot reactivate the departed mode");
}

void TestVirtualLitchInstantNewCanvasJourney() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested &&
              pager.TryPresentRequested(), "New journey presents prior saved artwork");
    virtual_litch_config config{};
    config.brightness_scale_256 = 256U;
    config.max_led_current_ma = 12000U;
    config.strong_power_source = 1U;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    auto& art = board.Litchcraft();
    art.RestoreArtwork({}, true, 0U, nullptr, true);
    art.AttachFrameStream(&pager);
    Check(board.Advance(20U) == VIRTUAL_LITCH_OK && art.IsActive(),
          "New journey starts in active Litchcraft with no physical pieces");
    std::array<uint8_t, 6U> play{1U, 0U};
    fxe::WriteU32LE(pager.Generation(), play.data() + 2U);
    Check(art.ApplyConnectionOrdered(2U, play.data(), play.size(), 201U) ==
              modules::LitchcraftSession::Status::Ok && art.IsAnimationPlaying(),
          "New journey begins with animation playing");
    const uint32_t savedGeneration = pager.Generation();
    const uint32_t priorEpoch = pager.DocumentEpoch();
    const uint32_t priorRevision = pager.LiveRevision();
    const size_t priorReads = medium.reads;
    const size_t priorWrites = medium.writes;
    std::array<uint8_t, 8U> intent{};
    fxe::WriteU32LE(savedGeneration, intent.data());
    fxe::WriteU32LE(priorRevision, intent.data() + 4U);
    constexpr uint32_t epoch = 201U;
    const uint32_t oldFingerprint = pager.DisplayedFingerprint();
    std::array<uint8_t, 8U> invalid = intent;
    fxe::WriteU32LE(savedGeneration + 5U, invalid.data());
    Check(art.ApplyConnectionOrdered(11U, invalid.data(), invalid.size(), epoch) ==
              modules::LitchcraftSession::Status::RevisionConflict &&
              art.ApplyConnectionOrdered(11U, intent.data(), intent.size() - 1U, epoch) ==
              modules::LitchcraftSession::Status::Malformed &&
              pager.DisplayedFingerprint() == oldFingerprint &&
              pager.DocumentEpoch() == priorEpoch && art.IsAnimationPlaying(),
          "bad New cannot clear or pause valid prior artwork");
    const auto accepted = art.ApplyConnectionOrdered(11U, intent.data(), intent.size(), epoch);
    Check(accepted == modules::LitchcraftSession::Status::Ok &&
              board.Advance(20U) == VIRTUAL_LITCH_OK && art.SelectedFrame() == 0U &&
              art.FrameCount() == 1U && !art.IsAnimationPlaying(),
          "accepted New presents one paused blank frame before storage service");
    store::Frame displayed{};
    uint16_t index = 99U;
    Check(pager.CopyDisplayed(displayed, index) && index == 0U &&
              std::all_of(displayed.emitters.begin(), displayed.emitters.end(),
                  [](const store::Emitter& emitter) {
                      return emitter.color.r == 0U && emitter.color.g == 0U &&
                          emitter.color.b == 0U && emitter.effect == 0U &&
                          emitter.effectDurationMs == 0U;
                  }) && displayed.durationMs == 1000U &&
              pager.Generation() == savedGeneration && pager.LiveRevision() == priorRevision + 1U &&
              pager.DocumentEpoch() != priorEpoch && pager.HasUnpersistedEdit() &&
              medium.reads == priorReads && medium.writes == priorWrites,
          "New owns clean blank content and a dirty live revision without flash I/O");
    Check(board.SemanticFrame().board[0U][0U].r == 0U &&
              board.SemanticFrame().board[0U][0U].g == 0U &&
              board.SemanticFrame().board[0U][0U].b == 0U,
          "next virtual render clears actual semantic canvas while housekeeping is withheld");
    const auto checkpoint = [&](const char* stage, const char* result,
                                const char* next, const char* persistence) {
        (void)board.Advance(20U);
        const auto led = board.SemanticFrame().board[63U][0U];
        std::printf("[INSTANT_NEW_CANVAS_JOURNEY] stage=%s t=%u occupancy=%016llx "
                    "mode=%s frame=%u count=%u playing=%u saved_g=%u live_r=%u "
                    "result=%s led63=%u,%u,%u display=\"%.*s\" next=%s "
                    "persistence=%s expiry=none\n",
            stage, board.State().now_ms, static_cast<unsigned long long>(art.Occupancy()),
            art.IsActive() ? "Litchcraft" : "departed", art.SelectedFrame(), art.FrameCount(),
            art.IsAnimationPlaying() ? 1U : 0U, pager.Generation(), pager.LiveRevision(),
            result, led.r, led.g, led.b, static_cast<int>(board.DisplayText().size()),
            board.DisplayText().data(), next, persistence);
    };
    checkpoint("accepted", "accepted", "paint", "pending");
    if (accepted != modules::LitchcraftSession::Status::Ok) return;
    store::StoreReceiver receiver(frames);
    receiver.BindLiveStream(pager);
    receiver.SetProjection({art.SessionId(), art.SelectedFrame(), pager.Topology(), true, false});
    std::array<uint8_t, 10U> inspect{0x4dU, 2U, 1U};
    fxe::WriteU32LE(art.SessionId(), inspect.data() + 3U);
    fxe::WriteU16LE(25U, inspect.data() + 7U);
    inspect[9U] = store::RESIDENT_LISTING_PROBE;
    store::StoreIngressResult pending{};
    receiver.Handle(inspect.data(), inspect.size(), epoch, pending);
    Check(pending.responseReady &&
              pending.response[13U] == static_cast<uint8_t>(store::WireStatus::Busy) &&
              medium.reads == priorReads && medium.writes == priorWrites,
          "pending New is Busy, preserving the healthy channel without exposing old artwork");
    const auto revision = pager.LiveRevision();
    const std::array<uint8_t, 9U> paint{0U, 1U, 0U, 1U, 15U, 99U, 17U, 44U, 63U};
    Check(art.ApplyConnectionOrdered(3U, paint.data(), paint.size(), epoch) ==
              modules::LitchcraftSession::Status::Ok && pager.LiveRevision() == revision + 1U,
          "paint accepted before blank root publication belongs to the new frame");
    std::array<uint8_t, 23U> liveRead{0x4dU, 2U, store::STORE_SNAPSHOT};
    fxe::WriteU32LE(art.SessionId(), liveRead.data() + 3U);
    fxe::WriteU16LE(26U, liveRead.data() + 7U);
    fxe::WriteU32LE(pager.Generation(), liveRead.data() + 9U);
    fxe::WriteU16LE(0xfffeU, liveRead.data() + 17U);
    fxe::WriteU32LE(pager.LiveRevision(), liveRead.data() + 19U);
    store::StoreIngressResult pendingRead{};
    receiver.Handle(liveRead.data(), liveRead.size(), epoch, pendingRead);
    Check(pendingRead.responseReady &&
              pendingRead.response[13U] == static_cast<uint8_t>(store::WireStatus::Busy),
          "post-New live repair is Busy while the blank root is pending, not a channel failure");
    const std::array<uint8_t, 8U> end{0U, 1U, 0U, 2U, 15U, 0U, 0U, 0U};
    Check(art.ApplyConnectionOrdered(3U, end.data(), end.size(), epoch) ==
              modules::LitchcraftSession::Status::Ok && pager.UndoCount() == 1U,
          "finger-up closes the accepted post-New stroke through the same healthy Ordered owner");
    checkpoint("read_deferred", "busy", "finish_stroke_then_retry_read", "pending");
    medium.failWrite = medium.writes + 1U;
    (void)pager.Service(frames, false);
    Check(pager.CopyDisplayed(displayed, index) && displayed.emitters[252U].color.r == 99U &&
              pager.HasUnpersistedEdit() && frames.Generation() == savedGeneration,
          "failed housekeeping preserves accepted blank and its subsequent paint with prior root intact");
    checkpoint("save_failed", "paint_retained", "retry_save", "pending");
    medium.failWrite = 0U;
    for (uint8_t attempt = 0U; attempt < 12U && pager.HasUnpersistedEdit(); ++attempt) {
        (void)pager.Service(frames, false);
        art.Update();
    }
    Check(!pager.HasUnpersistedEdit() && frames.CurrentManifest().frameCount == 1U &&
              pager.CopyDisplayed(displayed, index) && displayed.emitters[252U].color.r == 99U &&
              pager.LiveRevision() == revision + 1U,
          "housekeeping retry and ordinary save retain exact painted new document without another live mutation");
    receiver.SetProjection({art.SessionId(), art.SelectedFrame(), pager.Topology(),
                            art.IsActive(), art.IsAnimationPlaying()});
    fxe::WriteU32LE(art.SessionId(), liveRead.data() + 3U);
    fxe::WriteU16LE(27U, liveRead.data() + 7U);
    fxe::WriteU32LE(pager.Generation(), liveRead.data() + 9U);
    fxe::WriteU32LE(pager.LiveRevision(), liveRead.data() + 19U);
    receiver.Handle(liveRead.data(), liveRead.size(), epoch, pendingRead);
    Check(pendingRead.responseReady &&
              pendingRead.response[13U] == static_cast<uint8_t>(store::WireStatus::Ok),
          "the deferred read recovers after New and its small drawing are saved");
    checkpoint("read_recovered", "accepted", "paint_or_exit", "saved");
    const auto beforeBad = pager.LiveRevision();
    Check(art.ApplyConnectionOrdered(11U, intent.data(), intent.size(), epoch) ==
              modules::LitchcraftSession::Status::RevisionConflict &&
              pager.CopyDisplayed(displayed, index) && displayed.emitters[252U].color.r == 99U &&
              pager.LiveRevision() == beforeBad,
          "stale New intent cannot erase acknowledged paint or reset its document epoch");
    checkpoint("stale_new", "rejected", "paint_or_exit", "saved");
    art.EndConnection();
    art.Exit();
    checkpoint("departed", "accepted", "other_mode", "saved");
}

void TestSameGenerationResidentReopenRetainsAcceptedFrame() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
          "resident reopen fixture presents the original document");
    modules::LitchcraftSession session;
    session.Configure(3U, 120U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    const auto savedGeneration = pager.Generation();
    const auto originalEpoch = pager.DocumentEpoch();
    store::Frame first{};
    Check(frames.ReadFrame(0U, first) == store::Status::Ok &&
              pager.StageFirstFrame(frames.CurrentManifest(), savedGeneration, first) &&
              pager.Generation() == savedGeneration && pager.DocumentEpoch() != originalEpoch,
          "same resident reopen advances artwork lifetime without a saved-generation change");
    session.Update();
    constexpr uint32_t connection = 121U;
    std::array<uint8_t, 8U> guard{};
    fxe::WriteU32LE(pager.Generation(), guard.data());
    fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
    const std::array<uint8_t, 5U> copy{0U, 0U, 1U, 0U, 0U};
    Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), connection) ==
              modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(10U, copy.data(), copy.size(), connection) ==
              modules::LitchcraftSession::Status::Ok && session.SelectedFrame() == 1U,
          "same-resident AddCopy acknowledges and selects its inserted frame");
    const auto acceptedEpoch = pager.DocumentEpoch();
    const auto acceptedRevision = pager.LiveRevision();
    for (uint8_t attempt = 0U; attempt < 8U && pager.HasUnpersistedEdit(); ++attempt) {
        (void)pager.Service(frames, false);
    }
    session.Update();
    Check(!pager.HasUnpersistedEdit() && pager.Generation() != savedGeneration &&
              pager.DocumentEpoch() == acceptedEpoch && pager.LiveRevision() == acceptedRevision &&
              session.SelectedFrame() == 1U && pager.RequestedIndex() == 1U &&
              pager.DisplayedIndex() == 1U,
          "Add save never discovers an old resident epoch and resets the accepted frame to zero");
    fxe::WriteU32LE(pager.Generation(), guard.data());
    fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
    const std::array<uint8_t, 5U> duration{3U, 1U, 0U, 137U, 0U};
    Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), connection) ==
              modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(10U, duration.data(), duration.size(), connection) ==
              modules::LitchcraftSession::Status::Ok && pager.LiveRevision() == acceptedRevision + 1U,
          "Duration137 immediately after accepted Add save applies exactly once without reselection");
    store::Frame shown{};
    uint16_t selected = 99U;
    Check(pager.CopyDisplayed(shown, selected) && selected == 1U && shown.durationMs == 137U,
          "the copied frame carries the exact legal millisecond duration");
    std::printf("[RESIDENT_EPOCH_FRAME_EDIT] G=%lu->%lu R=%lu->%lu selected=%u duration=%u\n",
                static_cast<unsigned long>(savedGeneration), static_cast<unsigned long>(pager.Generation()),
                static_cast<unsigned long>(acceptedRevision), static_cast<unsigned long>(pager.LiveRevision()),
                selected, shown.durationMs);
}

void TestStructuralLiveAcknowledgementPrecedesStorage() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Seed(medium, frames);
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
              LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
          "RAM structural fixture presents its source");
    modules::LitchcraftSession session;
    session.Configure(3U, 120U);
    session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager);
    session.Update();
    constexpr uint32_t epoch = 121U;
    std::array<uint8_t, 6U> select{2U, 1U};
    fxe::WriteU32LE(pager.Generation(), select.data() + 2U);
    Check(session.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "RAM structural fixture seeks the middle frame through Timeline");
    for (uint8_t i = 0U; i < 8U && pager.DisplayedIndex() != 1U; ++i) {
        (void)LoadOne(pager, frames);
        session.Update();
    }
    std::array<uint8_t, 8U> guard{};
    fxe::WriteU32LE(pager.Generation(), guard.data());
    fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
    Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) ==
              modules::LitchcraftSession::Status::Ok,
          "RAM structural edit arms its actual shared-session guard");
    const auto savedGeneration = frames.Generation();
    const auto liveRevision = pager.LiveRevision();
    const auto writes = medium.writes;
    const auto reads = medium.reads;
    const std::array<uint8_t, 5U> copy{0U, 1U, 2U, 0U, 0U};
    const auto admissionStarted = std::chrono::steady_clock::now();
    auto result = session.ApplyConnectionOrdered(10U, copy.data(), copy.size(), epoch);
    if (result == modules::LitchcraftSession::Status::Busy) {
        result = session.ApplyConnectionOrdered(10U, copy.data(), copy.size(), epoch);
    }
    const auto admissionUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - admissionStarted).count();
    const bool applied = result == modules::LitchcraftSession::Status::Ok &&
        pager.FrameCount() == 5U && pager.DisplayedIndex() == 2U &&
        pager.LiveRevision() == liveRevision + 1U && session.SelectedFrame() == 2U;
    Check(applied,
          "middle Add Applied and exact live selection precede any storage worker service");
    Check(medium.writes == writes && medium.reads == reads &&
              frames.Generation() == savedGeneration,
          "RAM Add acknowledgement performs no flash read/write or durable publication");
    if (!applied) return;
    Check(pager.PaintDisplayedIfRevision(pager.LiveRevision(), 2U, 27U, 15U,
                                        {238U, 119U, 68U}, false) ==
              stream::FrameStream::GuardedEditResult::Changed,
          "drawing immediately after acknowledged Add edits its inserted RAM identity");
    const auto epochBeforeSave = pager.DocumentEpoch();
    const auto revisionBeforeSave = pager.LiveRevision();
    store::LiveRead live{};
    live.snapshot.generation = pager.Generation();
    live.snapshot.revision = pager.LiveRevision();
    Check(frames.CaptureLiveRead(pager, 0xfffeU, live) == store::Status::Ok &&
              live.snapshot.frameCount == 5U,
          "unsaved Add is readable as the exact live layout, never the old durable root");
    for (uint8_t i = 0U; i < 8U && pager.HasUnpersistedEdit(); ++i) {
        (void)pager.Service(frames, false);
    }
    store::Frame saved{};
    Check(!pager.HasUnpersistedEdit() && frames.CurrentManifest().frameCount == 5U &&
              frames.ReadFrame(2U, saved) == store::Status::Ok &&
              saved.emitters[108U].color.r == 238U && pager.DisplayedIndex() == 2U &&
              pager.LiveRevision() == revisionBeforeSave && pager.DocumentEpoch() == epochBeforeSave,
          "save retains inserted identity, following drawing, live revision and artwork lifetime");
    const auto inserted = saved;
    uint16_t selectIndex = 0U;
    const auto beforeDeleteWrites = medium.writes;
    fxe::WriteU32LE(pager.Generation(), guard.data());
    fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
    const std::array<uint8_t, 5U> remove{1U, 2U, 0U, 0U, 0U};
    Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) == modules::LitchcraftSession::Status::Ok &&
              session.ApplyConnectionOrdered(10U, remove.data(), remove.size(), epoch) == modules::LitchcraftSession::Status::Ok &&
              pager.FrameCount() == 4U && session.SelectedFrame() == 1U && medium.writes == beforeDeleteWrites,
          "Delete selects its warm survivor and acknowledges RAM before flash");
    Check(session.ApplyConnectionOrdered(6U, nullptr, 0U, epoch) == modules::LitchcraftSession::Status::Ok &&
              pager.FrameCount() == 5U && session.SelectedFrame() == 2U && medium.writes == beforeDeleteWrites &&
              pager.CopyDisplayed(saved, selectIndex) && Same(saved, inserted),
          "Undo restores exact removed RGB/effect/duration before either queued layout publishes");
    struct DuringSave final { stream::FrameStream* pager; bool accepted = false; } during{&pager};
    medium.onceOnWrite = [](void* context) {
        auto& during = *static_cast<DuringSave*>(context);
        during.accepted = during.pager->PaintDisplayedIfRevision(
            during.pager->LiveRevision(), 2U, 28U, 15U, {17U, 204U, 51U}, false) ==
            stream::FrameStream::GuardedEditResult::Changed;
    };
    medium.onceOnSyncContext = &during;
    const auto beforePublicationRevision = pager.LiveRevision();
    for (uint8_t attempt = 0U; attempt < 16U && pager.HasUnpersistedEdit(); ++attempt) {
        (void)pager.Service(frames, false);
    }
    Check(during.accepted && !pager.HasUnpersistedEdit() && pager.DisplayedIndex() == 2U &&
              pager.DocumentEpoch() == epochBeforeSave && pager.LiveRevision() == beforePublicationRevision + 1U &&
              frames.ReadFrame(2U, saved) == store::Status::Ok && saved.emitters[108U].color.r == 238U &&
              saved.emitters[112U].color.g == 204U,
          "drawing during Delete/Undo publication survives exact identity rebasing without a new live revision");
    std::printf("[RAM_STRUCTURAL_ACK] middle_Add=Applied selected=2 storage_ops=0 draw=retained saved_g=%u live_r=%u admission_us=%lld\n",
                savedGeneration, pager.LiveRevision(), static_cast<long long>(admissionUs));
    std::printf("[RAM_STRUCTURAL_SIZE] frame_stream=%zu frame_store=%zu cache_slots=%u pending_capacity=16\n",
                sizeof(pager), sizeof(frames), stream::FrameStream::CACHE_SLOTS);
}

void TestStructuralLiveBackpressureRetainsCopies() {
    for (const bool paint : {true, false}) {
        Medium medium;
        store::FrameStore frames(medium.Ops()); Seed(medium, frames);
        stream::FrameStream pager;
        Check(pager.Configure(frames.CurrentManifest(), frames.Generation()) &&
                  LoadOne(pager, frames) == stream::FrameStream::ServiceResult::LoadedRequested,
              "bounded structural window starts with a real displayed frame");
        modules::LitchcraftSession session;
        session.Configure(3U, 122U); session.RestoreArtwork({}, false, 0U);
        session.AttachFrameStream(&pager); session.Update();
        constexpr uint32_t epoch = 123U;
        const uint8_t acceptedBeforePressure = 3U;
        const auto epochBefore = pager.DocumentEpoch();
        const auto writesBefore = medium.writes;
        const auto append = [&]() {
            std::array<uint8_t, 8U> guard{};
            fxe::WriteU32LE(pager.Generation(), guard.data());
            fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
            Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) == modules::LitchcraftSession::Status::Ok,
                  "each real Add uses its current guarded live base");
            const auto source = static_cast<uint8_t>(session.SelectedFrame());
            const std::array<uint8_t, 5U> copy{0U, source, static_cast<uint8_t>(source + 1U), 0U, 0U};
            return session.ApplyConnectionOrdered(10U, copy.data(), copy.size(), epoch);
        };
        for (uint8_t edit = 1U; edit <= acceptedBeforePressure; ++edit) {
            Check(append() == modules::LitchcraftSession::Status::Ok,
                  "available bounded RAM accepts Add before worker service");
            if (paint) Check(pager.PaintDisplayedIfRevision(pager.LiveRevision(), edit, 7U, 15U,
                                  {edit, 87U, 123U}, false) == stream::FrameStream::GuardedEditResult::Changed,
                             "each copied frame keeps a distinct dirty COW payload");
        }
        Check(medium.writes == writesBefore && frames.CurrentManifest().frameCount == 4U,
              "all actions inside the RAM bound acknowledge without flash writes");
        const auto liveBefore = pager.FrameCount();
        Check(append() == modules::LitchcraftSession::Status::Busy && pager.FrameCount() == liveBefore,
              "cache/metadata pressure retains the next ordered action without partial mutation");
        const auto source = static_cast<uint8_t>(session.SelectedFrame());
        const std::array<uint8_t, 5U> copy{0U, source, static_cast<uint8_t>(source + 1U), 0U, 0U};
        auto status = modules::LitchcraftSession::Status::Busy;
        for (uint8_t attempt = 0U; attempt < 8U && status == modules::LitchcraftSession::Status::Busy; ++attempt) {
            (void)pager.Service(frames, false);
            status = session.ApplyConnectionOrdered(10U, copy.data(), copy.size(), epoch);
        }
        Check(status == modules::LitchcraftSession::Status::Ok && pager.FrameCount() == liveBefore + 1U &&
                  frames.CurrentManifest().frameCount < pager.FrameCount() && pager.DocumentEpoch() == epochBefore,
              "bounded pressure drains a predecessor, then acknowledges the next live edit before its own save");
        if (paint) Check(pager.PaintDisplayedIfRevision(pager.LiveRevision(), acceptedBeforePressure + 1U,
                              7U, 15U, {static_cast<uint8_t>(acceptedBeforePressure + 1U), 87U, 123U}, false) ==
                              stream::FrameStream::GuardedEditResult::Changed,
                         "first post-pressure drawing keeps its new frame target");
        for (uint8_t attempt = 0U; attempt < 64U && pager.HasUnpersistedEdit(); ++attempt) {
            (void)pager.Service(frames, false);
        }
        Check(!pager.HasUnpersistedEdit() && frames.CurrentManifest().frameCount == pager.FrameCount(),
              "bounded queued persistence eventually converges without replacing the live layout");
        for (uint8_t frame = 1U; frame <= acceptedBeforePressure + 1U; ++frame) {
            store::Frame saved{};
            Check(frames.ReadFrame(frame, saved) == store::Status::Ok &&
                      (!paint || saved.emitters[28U].color.r == frame),
                  "every copied frame retains its own latest payload across cache reuse and publication");
        }
        std::printf("[RAM_STRUCTURAL_PRESSURE] kind=%s accepted_without_IO=%u final_frames=%u epoch=unchanged result=exact_convergence\n",
                    paint ? "dirty_copy_cache" : "clean_copy_cache", acceptedBeforePressure, pager.FrameCount());
    }
}

void TestStructuralDeleteMetadataWindow() {
    Medium medium;
    store::FrameStore frames(medium.Ops());
    store::Manifest manifest{};
    manifest.frameCount = 21U;
    manifest.active = true;
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "metadata pressure seeds twenty-one actual stored frames");
    for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
        Check(frames.WriteFrame(token, index, MakeDenseFrame(index)) == store::Status::Ok,
              "metadata fixture writes every complete frame");
    }
    Check(frames.Commit(token) == store::Status::Ok, "metadata pressure corpus commits");
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), frames.Generation()), "metadata pressure configures stream");
    modules::LitchcraftSession session;
    session.Configure(3U, 122U); session.RestoreArtwork({}, false, 0U);
    session.AttachFrameStream(&pager); session.Update();
    constexpr uint32_t epoch = 123U;
    const auto page = [&](const uint16_t index) {
        Check(pager.Request(index), "delete pressure requests survivor through existing pager");
        for (uint8_t attempt = 0U; attempt < 8U && !pager.TryPresentRequested(); ++attempt) {
            (void)pager.Service(frames, false, stream::FrameStream::ServiceMode::PagingOnly);
        }
        Check(pager.TryPresentRequested() && pager.DisplayedIndex() == index,
              "delete pressure prepares survivor without saving accepted layouts");
    };
    const auto removeTail = [&]() {
        const auto last = static_cast<uint8_t>(pager.FrameCount() - 1U);
        std::array<uint8_t, 6U> seek{2U, last};
        fxe::WriteU32LE(pager.Generation(), seek.data() + 2U);
        Check(session.ApplyConnectionOrdered(2U, seek.data(), seek.size(), epoch) == modules::LitchcraftSession::Status::Ok,
              "metadata pressure selects the actual tail through ordered runtime control");
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(pager.Generation(), guard.data());
        fxe::WriteU32LE(pager.LiveRevision(), guard.data() + 4U);
        Check(session.ApplyConnectionOrdered(8U, guard.data(), guard.size(), epoch) == modules::LitchcraftSession::Status::Ok,
              "delete pressure guards current live revision");
        const uint8_t payload[] = {1U, last, 0U, 0U, 0U};
        return session.ApplyConnectionOrdered(10U, payload, sizeof(payload), epoch);
    };
    const auto writesBefore = medium.writes;
    const auto generationBefore = frames.Generation();
    for (uint8_t index = 0U; index < 16U; ++index) {
        page(pager.FrameCount() - 2U);
        page(pager.FrameCount() - 1U);
        Check(removeTail() == modules::LitchcraftSession::Status::Ok,
              "all sixteen metadata-only deletes fit the existing bounded storage window");
    }
    Check(medium.writes == writesBefore && frames.Generation() == generationBefore && pager.FrameCount() == 5U,
          "sixteen accepted deletes change RAM only, with no root or body write");
    page(3U);
    page(4U);
    const auto revisionBefore = pager.LiveRevision();
    Check(removeTail() == modules::LitchcraftSession::Status::Busy && pager.FrameCount() == 5U &&
              pager.LiveRevision() == revisionBefore,
          "seventeenth delete waits at existing ordered head without partial count or revision change");
    const uint8_t payload[] = {1U, 4U, 0U, 0U, 0U};
    auto status = modules::LitchcraftSession::Status::Busy;
    for (uint8_t attempt = 0U; attempt < 32U && status == modules::LitchcraftSession::Status::Busy; ++attempt) {
        (void)pager.Service(frames, false);
        status = session.ApplyConnectionOrdered(10U, payload, sizeof(payload), epoch);
    }
    Check(status == modules::LitchcraftSession::Status::Ok && pager.FrameCount() == 4U,
          "seventeenth metadata operation drains once capacity becomes available");
    for (uint8_t attempt = 0U; attempt < 64U && pager.HasUnpersistedEdit(); ++attempt) (void)pager.Service(frames, false);
    store::FrameStore reopened(medium.Ops());
    Check(!pager.HasUnpersistedEdit() && reopened.Recover() == store::Status::Ok &&
              reopened.CurrentManifest().frameCount == 4U, "all seventeen deletes persist and reopen");
    for (uint16_t index = 0U; index < 4U; ++index) {
        store::Frame saved{};
        Check(reopened.ReadFrame(index, saved) == store::Status::Ok && Same(saved, MakeDenseFrame(index)),
              "metadata pressure retains exact RGB effects and duration of every surviving original");
    }
    std::puts("[RAM_STRUCTURAL_PRESSURE] kind=delete_metadata accepted_without_writes=16 total=17 reopened_frames=4 exact=all_fields");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (std::getenv("LITCH_COLD_PART_PLAYBACK_ONLY") != nullptr) {
        TestOneColdPartReadKeepsAuthoredPlaybackCadence();
        TestColdSuccessorPlaybackRetainsFramesAndCatchesUp();
        TestPrefetchPolicyPreservesPausedReverseAndSmallWraps();
        TestSeekDuringFarReadPreservesNewDemand();
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_RAM_STRUCTURAL_ONLY") != nullptr) {
        TestSameGenerationResidentReopenRetainsAcceptedFrame();
        TestStructuralLiveAcknowledgementPrecedesStorage();
        TestStructuralLiveBackpressureRetainsCopies();
        TestStructuralDeleteMetadataWindow();
        return failures == 0 ? 0 : 1;
    }
    TestNetUnchangedEditsRetainDurableRoot();
    TestUnsavedSemanticDifferencesStillPersist();
    TestNoopPreservesOtherDirtySlotsAndPageDemand();
    TestNoopReadMutationAndRecoveryBoundaries();
    if (std::getenv("LITCH_NET_UNCHANGED_EDIT_ONLY") != nullptr) {
        std::printf("[NET_UNCHANGED_EDIT_REGRESSION] %s\n", failures == 0 ? "ok" : "failed");
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_INSTANT_NEW_CANVAS_ONLY") != nullptr) {
        TestVirtualLitchInstantNewCanvasJourney();
        TestNewCanvasAtStoragePublicationBoundaries();
        TestNewCanvasPhysicalStrokeAndDeparture();
        TestStructuralEditQueuedBehindInstantNew();
        std::printf("[INSTANT_NEW_CANVAS_REGRESSION] %s\n", failures == 0 ? "ok" : "failed");
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_PLAYING_CANVAS_BOUNDARY_ONLY") != nullptr) {
        TestPlayingCanvasPinsAcceptedSourceAtClockBoundary();
        std::printf("[PLAYING_CANVAS_BOUNDARY_REGRESSION] %s\n", failures == 0 ? "ok" : "failed");
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_PAUSED_EDIT_CLOCK_ONLY") != nullptr) {
        TestPlayingCanvasPinsAcceptedSourceAtClockBoundary();
        TestPausedStoreEditPreservesPlaybackPhase();
        std::printf("[PAUSED_EDIT_CLOCK_REGRESSION] %s\n", failures == 0 ? "ok" : "failed");
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_HISTORY_FOOTPRINT_REGRESSION_ONLY") != nullptr) {
        TestUnknownStoreHistoryGroupsCompleteFootprints();
        std::printf("[STORE_HISTORY_FOOTPRINT_REGRESSION] %s\n", failures == 0 ? "ok" : "failed");
        return failures == 0 ? 0 : 1;
    }
    TestPrefetchAndPriority();
    TestDenseSequentialPlaybackUsesOneFlashLeasePerFrame();
    TestSeedHeaderIoNeverHoldsPublicationLock();
    TestPredecessorPlaybackResumesFromNearestPublishedFrame();
    TestDirtyFrameCannotSeedCommittedPredecessorDecode();
    TestJumpAndCorruptionRetainCurrent();
    TestGenerationInvalidatesSameIndexCache();
    TestValidatedIncomingFirstFramePresentsWithoutFlashRead();
    TestResidentSwitchWaitsForAcknowledgedBoardEdit();
    TestPlaybackKeepsDirtyPaintedFrameWhileSeeking();
    TestDirtyFrameSelectionPresentsBeforeStoreRewrite();
    TestNextBrushDuringDeferredSaveKeepsBothFrames();
    TestFullDirtyCacheFlushesWithoutLosingAcknowledgedPaint();
    TestFullDirty256CacheSelectsWithoutWholeBodyReplay();
    TestDenseSinglePaintWritesOnlyOneFrame();
    TestPageReadCannotOverwritePaintedInactiveSlot();
    TestImmediateSaveBypassesPausedPresentation();
    TestEditInvalidatesAnInterruptedPageRead();
    TestSessionPresentsAndAdvancesFlashBackedFrames();
    TestConnectionTimelineControlsCommittedStoreWithoutReupload();
    TestStoreToolPreservesCoalescedLiveEditCheckpoint();
    TestConnectionStoreToolDuringReservedReplacement();
    TestOrderedStorePaintIsImmediateAndPersistsOffRenderPath();
    TestOrderedStoreCanvasIsOneAtomicHistoryMutation();
    TestCanvasClosesRestingPhysicalStrokeOnlyAfterGuardAdmission();
    TestSameGenerationResidentReopenRetainsAcceptedFrame();
    TestStructuralLiveAcknowledgementPrecedesStorage();
    TestStructuralLiveBackpressureRetainsCopies();
    TestStructuralDeleteMetadataWindow();
    TestStoreToolsNativeCausalAdmission();
    TestStoreToolsRotationDurationAndStructuralHistory();
    TestStoreStructuralCancelDurableBoundary();
    TestStoreStructuralRefusalAndFaultRecovery();
    TestStoreToolsNoopBoundsAndLastFrameHistory();
    TestVirtualLitchStoreToolsJourney();
    TestVirtualLitchInstantNewCanvasJourney();
    TestNewCanvasAtStoragePublicationBoundaries();
    TestNewCanvasPhysicalStrokeAndDeparture();
    TestStructuralEditQueuedBehindInstantNew();
    TestOrderedStoreCanvasRejectsStaleAndMalformedBeforeMutation();
    TestStoreCanvasMinimumAttCheckerAndGenerationFence();
    TestColdPlaybackHallStrokePinsCanvasAndPreservesExplicitSeekFences();
    TestShakeErasePinsDisplayedFrameAcrossPendingPlaybackRead();
    TestStoreTwoSquareStrokeKeepsExactGuardAndOneUndo();
    TestLiveRevisionRejectsUnpublishedPhysicalEdit();
    TestLiveRevisionRolloverFailsClosed();
    TestInitialClearDoesNotExhaustLiveRevision();
    TestRestoredStoreFirstBrushAndHeldColorPick();
    TestStoreReplacementCancelsHeldColorPick();
    TestRetirementDuringPersistenceCannotResurrect();
    TestClearAdvancesDurableRetirementFence();
    TestPlayingCanvasPinsAcceptedSourceAtClockBoundary();
    TestPausedStoreEditPreservesPlaybackPhase();
    TestLateFrameCatchesUpToAuthoredSchedule();
    TestOneColdPartReadKeepsAuthoredPlaybackCadence();
    TestColdSuccessorPlaybackRetainsFramesAndCatchesUp();
    TestPrefetchPolicyPreservesPausedReverseAndSmallWraps();
    TestSeekDuringFarReadPreservesNewDemand();
    TestSessionSignalsCorruptPlaybackFrame();
    TestInactiveGenerationRunsTheExitLifecycle();
    TestExitWinsOverPreviewRollbackToActiveRoot();
    TestOpenWireUploadKeepsCommittedPlaybackDecodeIndependent();
    TestStoreHistoryRetainsSparseStrokesWithoutWholeFrames();
    TestUnknownStoreHistoryGroupsCompleteFootprints();
    TestPhysicalStoreUndoPublishesReverseEdit();
    TestOrderedStoreHistoryPresentationAndCancellation();
    TestVirtualLitchStorePhysicalUndoJourney();
    TestVirtualLitchDirtyFrameSeekJourney();
    std::printf("[LITCHCRAFT_FRAME_STREAM] %s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
