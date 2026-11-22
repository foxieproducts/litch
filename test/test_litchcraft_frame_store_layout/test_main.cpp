#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "firmware/litchcraft_frame_file.hpp"
#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "modules/litchcraft_frame_store_layout.hpp"
#include "modules/litchcraft_fixed_slot_wire_view.hpp"
#include "modules/litchcraft_wire_codec.hpp"

namespace layout = modules::litchcraft_frame_store_layout;
namespace store = modules::litchcraft_frame_store;

namespace {

int failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) return;
    ++failures;
    std::printf("[FAIL] %s\n", message);
}

struct MemoryFile final {
    std::vector<uint8_t> bytes =
        std::vector<uint8_t>(layout::ARTWORK_FILE_CAPACITY, 0xffU);
    std::vector<uint8_t> durable = bytes;
    size_t writes = 0U;
    size_t reads = 0U;
    size_t events = 0U;
    size_t faultAfterEvent = std::numeric_limits<size_t>::max();
    size_t corruptReadAt = std::numeric_limits<size_t>::max();
    bool failRead = false;

    static bool Read(void* const context, const size_t offset,
                     uint8_t* const out, const size_t size) {
        auto& self = *static_cast<MemoryFile*>(context);
        if (self.failRead || out == nullptr || offset > self.bytes.size() ||
            size > self.bytes.size() - offset) return false;
        ++self.reads;
        std::memcpy(out, self.bytes.data() + offset, size);
        if (self.corruptReadAt >= offset && self.corruptReadAt - offset < size) {
            out[self.corruptReadAt - offset] ^= 1U;
        }
        return true;
    }

    static bool Write(void* const context, const size_t offset,
                      const uint8_t* const source, const size_t size) {
        auto& self = *static_cast<MemoryFile*>(context);
        if (source == nullptr || offset > self.bytes.size() ||
            size > self.bytes.size() - offset) return false;
        for (size_t index = 0U; index < size; ++index) {
            if (self.events >= self.faultAfterEvent) return false;
            self.bytes[offset + index] = source[index];
            ++self.events;
            ++self.writes;
        }
        return true;
    }

    static bool Sync(void* const context) {
        auto& self = *static_cast<MemoryFile*>(context);
        if (self.events >= self.faultAfterEvent) return false;
        ++self.events;
        self.durable = self.bytes;
        return true;
    }

    store::FrameStore::Ops Ops() { return {this, &Read, &Write, &Sync}; }
    void Crash() { bytes = durable; }
};

store::Frame Frame(const uint8_t marker) {
    store::Frame frame{};
    frame.durationMs = static_cast<uint16_t>(100U + marker);
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        auto& emitter = frame.emitters[index];
        emitter.color = {static_cast<uint8_t>(index + marker),
                         static_cast<uint8_t>(index * 3U + marker),
                         static_cast<uint8_t>(index * 5U + marker)};
        emitter.effect = static_cast<uint8_t>(index % 9U);
        emitter.effectDurationMs = emitter.effect == 0U ? 0U :
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

layout::Root InitialRoot() {
    layout::Root root{};
    root.resident = 0U;
    root.generation = 1U;
    root.serial = 1U;
    root.manifest.frameCount = 2U;
    root.manifest.topology = 3U;
    root.manifest.sparkleSeed = 17U;
    root.manifest.predecessorDeltas = true;
    root.bodyBytes = 32U;
    root.bodyCrc32 = 0x12345678U;
    root.slots[0] = 0U;
    root.slots[1] = 1U;
    return root;
}

layout::Root WithIdentity(layout::Root root, const uint32_t bytes,
                          const uint32_t crc) {
    root.bodyBytes = bytes;
    root.bodyCrc32 = crc;
    root.blockBytes.fill(0U);
    root.blockCrc32.fill(0U);
    root.blockBytes[0U] = bytes;
    root.blockCrc32[0U] = crc;
    return root;
}

void Seed(MemoryFile& file) {
    std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
    const layout::Layout storage(file.Ops(), scratch.data(), scratch.size());
    const auto root = InitialRoot();
    Check(storage.WriteSlot(0U, 0U, Frame(1U)) == layout::Status::Ok &&
              storage.WriteSlot(0U, 1U, Frame(2U)) == layout::Status::Ok &&
              storage.PublishRoot(root, 0U) == layout::Status::Ok,
          "two independent source frames and their root become durable");
}

void CompleteRootReadbackRejectsEveryCorruptByte() {
    MemoryFile file;
    std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
    const layout::Layout storage(file.Ops(), scratch.data(), scratch.size());
    const auto root = InitialRoot();
    for (size_t byte = 0U; byte < layout::ROOT_BYTES; ++byte) {
        file.corruptReadAt = byte;
        Check(storage.PublishRoot(root, 0U) == layout::Status::IoFailure,
              "single-read root verification rejects corruption anywhere in its complete span");
    }
    file.corruptReadAt = std::numeric_limits<size_t>::max();
    Check(storage.PublishRoot(root, 0U) == layout::Status::Ok,
          "root publication recovers after the controlled readback fault clears");
}

void EditsFoldWithoutCopyingTheBank() {
    MemoryFile file;
    Seed(file);
    std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
    const layout::Layout storage(file.Ops(), scratch.data(), scratch.size());
    layout::Root root{};
    uint8_t copy = 2U;
    Check(storage.LatestRoot(0U, root, copy) == layout::Status::Ok &&
              copy == 0U && root.generation == 1U,
          "first committed root recovers from two root copies");
    const size_t before = file.writes;
    layout::Root staged{};
    Check(storage.CommitFrameEdit(root, copy, 1U, Frame(9U), 2U,
                                  WithIdentity(root, 33U, 0x23456789U),
                                  staged) ==
              layout::Status::Ok &&
              file.writes - before == layout::FRAME_SLOT_BYTES +
                                       layout::ROOT_BYTES,
          "ordinary edit writes one stage and one root, never a bank");
    store::Frame shown{};
    Check(storage.LatestRoot(0U, root, copy) == layout::Status::Ok &&
              copy == 1U && root.stageIndex == 1U &&
              storage.ReadFrame(root, 1U, shown) == layout::Status::Ok &&
              Same(shown, Frame(9U)),
          "committed staged frame becomes recoverable authority");
    Check(storage.StageFrame(Frame(10U)) == layout::Status::StageInUse,
          "another edit cannot overwrite a referenced stage");
    layout::Root folded{};
    store::Frame frameScratch{};
    const size_t beforeFold = file.writes;
    Check(storage.FoldStage(root, copy, frameScratch, folded) ==
              layout::Status::Ok &&
              file.writes - beforeFold == layout::FRAME_SLOT_BYTES +
                                           2U * layout::ROOT_BYTES,
          "fold writes one fixed slot and bounded roots");
    file.Crash();
    Check(storage.LatestRoot(0U, root, copy) == layout::Status::Ok &&
              copy == 0U && root.generation == 2U &&
              root.stageIndex == layout::NO_STAGE &&
              storage.ReadFrame(root, 0U, shown) == layout::Status::Ok &&
              Same(shown, Frame(1U)) &&
              storage.ReadFrame(root, 1U, shown) == layout::Status::Ok &&
              Same(shown, Frame(9U)),
          "folded root retains changed and untouched frames across reboot");
    Check(storage.StageFrame(Frame(10U)) == layout::Status::Ok,
          "stage becomes reusable only after its old root is invalidated");
}

void PowerCutsKeepOldOrNewCompleteFrame() {
    MemoryFile seed;
    Seed(seed);
    for (const size_t cut : std::array<size_t, 8U>{
             0U, 200U, layout::FRAME_SLOT_BYTES - 1U,
             layout::FRAME_SLOT_BYTES,
             layout::FRAME_SLOT_BYTES + 1U,
             layout::FRAME_SLOT_BYTES + 100U,
             layout::FRAME_SLOT_BYTES + layout::ROOT_BYTES,
             layout::FRAME_SLOT_BYTES + layout::ROOT_BYTES + 1U}) {
        MemoryFile file = seed;
        file.faultAfterEvent = file.events + cut;
        std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
        const layout::Layout storage(file.Ops(), scratch.data(), scratch.size());
        layout::Root staged{};
        (void)storage.CommitFrameEdit(InitialRoot(), 0U, 1U, Frame(9U),
                                      2U, WithIdentity(InitialRoot(), 33U,
                                                       0x23456789U), staged);
        file.Crash();
        layout::Root recovered{};
        uint8_t copy = 2U;
        store::Frame actual{};
        const bool complete =
            storage.LatestRoot(0U, recovered, copy) == layout::Status::Ok &&
            storage.ReadFrame(recovered, 1U, actual) == layout::Status::Ok &&
            ((recovered.generation == 1U && Same(actual, Frame(2U))) ||
             (recovered.generation == 2U && Same(actual, Frame(9U))));
        Check(complete,
              "cut during stage or root publication restores old or new complete frame");
    }
}

void PowerCutsDuringFoldKeepTheStagedEdit() {
    MemoryFile seed;
    Seed(seed);
    {
        std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
        const layout::Layout storage(seed.Ops(), scratch.data(), scratch.size());
        layout::Root staged{};
        Check(storage.CommitFrameEdit(InitialRoot(), 0U, 1U, Frame(9U),
                                      2U, WithIdentity(InitialRoot(), 33U,
                                                       0x23456789U), staged) ==
                  layout::Status::Ok,
              "fold power-cut fixture begins with a durable staged edit");
    }
    for (const size_t cut : std::array<size_t, 9U>{
             0U, 500U, layout::FRAME_SLOT_BYTES,
             layout::FRAME_SLOT_BYTES + 1U,
             layout::FRAME_SLOT_BYTES + layout::ROOT_BYTES,
             layout::FRAME_SLOT_BYTES + layout::ROOT_BYTES + 1U,
             layout::FRAME_SLOT_BYTES + layout::ROOT_BYTES + 100U,
             layout::FRAME_SLOT_BYTES + 2U * layout::ROOT_BYTES,
             layout::FRAME_SLOT_BYTES + 2U * layout::ROOT_BYTES + 2U}) {
        MemoryFile file = seed;
        file.faultAfterEvent = file.events + cut;
        std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
        const layout::Layout storage(file.Ops(), scratch.data(), scratch.size());
        layout::Root staged{};
        uint8_t stagedCopy = 2U;
        Check(storage.LatestRoot(0U, staged, stagedCopy) ==
                  layout::Status::Ok && stagedCopy == 1U,
              "fold trial reads the committed stage root");
        layout::Root folded{};
        store::Frame frameScratch{};
        (void)storage.FoldStage(staged, stagedCopy, frameScratch, folded);
        file.Crash();
        layout::Root recovered{};
        uint8_t copy = 2U;
        store::Frame actual{};
        Check(storage.LatestRoot(0U, recovered, copy) ==
                  layout::Status::Ok &&
                  recovered.generation == 2U &&
                  (recovered.stageIndex == 1U ||
                   recovered.stageIndex == layout::NO_STAGE) &&
                  storage.ReadFrame(recovered, 1U, actual) ==
                      layout::Status::Ok && Same(actual, Frame(9U)),
              "cut during fold preserves the committed edited pixels");
        if (cut == layout::FRAME_SLOT_BYTES +
                       layout::ROOT_BYTES + 2U) {
            file.faultAfterEvent = std::numeric_limits<size_t>::max();
            Check(storage.StageFrame(Frame(10U)) == layout::Status::Ok &&
                      storage.LatestRoot(0U, recovered, copy) ==
                          layout::Status::Ok &&
                      recovered.stageIndex == layout::NO_STAGE &&
                      storage.ReadFrame(recovered, 1U, actual) ==
                          layout::Status::Ok && Same(actual, Frame(9U)),
                  "recovery retires a stale stage root before reusing stage bytes");
        }
    }
}

void CorruptSlotNeverPublishesPartialPixels() {
    MemoryFile file;
    Seed(file);
    std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
    const layout::Layout storage(file.Ops(), scratch.data(), scratch.size());
    const auto root = InitialRoot();
    file.bytes[layout::Layout::SlotOffset(0U, 1U) + 17U] ^= 1U;
    store::Frame output = Frame(44U);
    const store::Frame sentinel = output;
    Check(storage.ReadFrame(root, 1U, output) == layout::Status::Corrupt &&
              Same(output, sentinel),
          "record CRC rejects damaged slot without publishing partial pixels");
}

void Dense256FrameEditKeepsWritesBounded() {
    MemoryFile file;
    std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
    const layout::Layout storage(file.Ops(), scratch.data(), scratch.size());
    auto root = InitialRoot();
    root.manifest.frameCount = store::FRAME_COUNT;
    root.bodyBytes = 280000U;
    for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
        root.slots[index] = static_cast<uint8_t>(index);
        Check(storage.WriteSlot(0U, static_cast<uint8_t>(index),
                                 Frame(static_cast<uint8_t>(index))) ==
                  layout::Status::Ok,
              "maximum artwork seeds every physical frame slot");
    }
    Check(storage.PublishRoot(root, 0U) == layout::Status::Ok,
          "maximum artwork publishes one complete root");
    const size_t beforeEdit = file.writes;
    layout::Root staged{};
    Check(storage.CommitFrameEdit(root, 0U, 255U, Frame(42U), 2U,
                                  WithIdentity(root, 280000U, 0x87654321U),
                                  staged) ==
              layout::Status::Ok &&
              file.writes - beforeEdit == layout::FRAME_SLOT_BYTES +
                                          layout::ROOT_BYTES,
          "dense 256-frame edit writes only one stage and one root");
    const size_t beforeFold = file.writes;
    layout::Root folded{};
    store::Frame frameScratch{};
    Check(storage.FoldStage(staged, 1U, frameScratch, folded) ==
              layout::Status::Ok &&
              file.writes - beforeFold == layout::FRAME_SLOT_BYTES +
                                          2U * layout::ROOT_BYTES,
          "dense 256-frame stage folds without rewriting the bank");
    file.Crash();
    uint8_t copy = 2U;
    store::Frame actual{};
    Check(storage.LatestRoot(0U, root, copy) == layout::Status::Ok &&
              root.manifest.frameCount == store::FRAME_COUNT &&
              storage.ReadFrame(root, 0U, actual) == layout::Status::Ok &&
              Same(actual, Frame(0U)) &&
              storage.ReadFrame(root, 255U, actual) == layout::Status::Ok &&
              Same(actual, Frame(42U)),
          "maximum artwork retains first and edited last frame after reboot");
}

void ProductionFileAdapterMapsAllFixedSlotRegions() {
    using File = firmware::LitchcraftFrameFile;
    File::PhysicalSpan span{};
    Check(File::ResolvePhysicalSpan(layout::Layout::StageOffset(),
                                      layout::FRAME_SLOT_BYTES, span) &&
              span.region == File::Region::Root0 &&
              span.offset == 2U * layout::ROOT_BYTES,
          "stage maps inside root0's physical file");
    Check(File::ResolvePhysicalSpan(layout::Layout::RootOffset(1U, 1U),
                                      layout::ROOT_BYTES, span) &&
              span.region == File::Region::Root1 &&
              span.offset == layout::ROOT_BYTES,
          "second resident's alternate root maps inside root1");
    File::PhysicalSpan lastPart{};
    Check(File::ResolvePhysicalSpan(layout::Layout::SlotOffset(1U, 240U),
                                      layout::FRAME_SLOT_BYTES, lastPart) && lastPart.offset == 0U &&
              File::ResolvePhysicalSpan(layout::Layout::SlotOffset(1U, 255U),
                                      layout::FRAME_SLOT_BYTES, span) &&
              span.region == lastPart.region &&
              span.offset == File::BODY_FILE_BYTES -
                                 layout::FRAME_SLOT_BYTES &&
              !File::ResolvePhysicalSpan(
                  layout::Layout::SlotOffset(1U, 255U),
                  layout::FRAME_SLOT_BYTES + 1U, span),
          "last fixed slot ends exactly at body1 capacity");

    namespace fs = std::filesystem;
    fs::create_directories("build");
    const std::array<std::string, 4U> paths{{
        "build/litch_layout_test.r0", "build/litch_layout_test.r1",
        "build/litch_layout_test.b0", "build/litch_layout_test.b1"}};
    { File cleanup(paths[0].c_str(), paths[1].c_str(), paths[2].c_str(), paths[3].c_str());
      Check(cleanup.Remove(), "fixture removes every checked artwork part"); }
    {
        File file(paths[0].c_str(), paths[1].c_str(),
                  paths[2].c_str(), paths[3].c_str());
        std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
        const layout::Layout storage(file.StoreOps(), scratch.data(),
                                      scratch.size());
        auto first = InitialRoot();
        first.manifest.frameCount = 1U;
        auto second = first;
        second.resident = 1U;
        second.slots[0] = 255U;
        Check(storage.WriteSlot(0U, 0U, Frame(1U)) == layout::Status::Ok &&
                  storage.WriteSlot(1U, 255U, Frame(2U)) ==
                      layout::Status::Ok &&
                  storage.PublishRoot(first, 0U) == layout::Status::Ok &&
                  storage.PublishRoot(second, 0U) == layout::Status::Ok,
              "production bounded-file adapter seeds both resident banks");
        layout::Root staged{};
        Check(storage.CommitFrameEdit(second, 0U, 0U, Frame(9U), 2U,
                                      WithIdentity(second, 33U, 0x23456789U),
                                      staged) ==
                  layout::Status::Ok,
              "production adapter persists a staged edit in the other resident");
        file.CloseCachedHandles();
    }
    {
        File reboot(paths[0].c_str(), paths[1].c_str(),
                    paths[2].c_str(), paths[3].c_str());
        std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
        const layout::Layout storage(reboot.StoreOps(), scratch.data(),
                                      scratch.size());
        layout::Root first{};
        layout::Root second{};
        uint8_t firstCopy = 2U;
        uint8_t secondCopy = 2U;
        store::Frame actual{};
        Check(storage.LatestRoot(0U, first, firstCopy) ==
                  layout::Status::Ok && firstCopy == 0U &&
                  storage.ReadFrame(first, 0U, actual) == layout::Status::Ok &&
                  Same(actual, Frame(1U)) &&
                  storage.LatestRoot(1U, second, secondCopy) ==
                      layout::Status::Ok && secondCopy == 1U &&
                  storage.ReadFrame(second, 0U, actual) == layout::Status::Ok &&
                  Same(actual, Frame(9U)),
              "both residents and staged edit recover through real file adapter");
        layout::Root folded{};
        store::Frame frameScratch{};
        Check(storage.FoldStage(second, secondCopy, frameScratch, folded) ==
                  layout::Status::Ok,
              "production adapter folds stage into one physical slot");
        reboot.CloseCachedHandles();
    }
    Check(fs::file_size(paths[0]) == layout::ROOT0_FILE_BYTES &&
              fs::file_size(paths[1]) == layout::ROOT1_FILE_BYTES &&
              fs::file_size(paths[2] + ".00") == layout::FRAME_SLOT_BYTES &&
              fs::file_size(paths[3] + ".15") == File::BODY_FILE_BYTES,
          "four physical files retain exact fixed-slot size bounds");
    { File cleanup(paths[0].c_str(), paths[1].c_str(), paths[2].c_str(), paths[3].c_str());
      Check(cleanup.Remove(), "fixture removes every checked artwork part"); }
}

void FixedSlotsReconstructOneCompressedWireBody() {
    MemoryFile file;
    std::array<uint8_t, layout::FRAME_SLOT_BYTES> slotPage{};
    const layout::Layout storage(file.Ops(), slotPage.data(), slotPage.size());
    auto root = InitialRoot();
    root.manifest.frameCount = store::FRAME_COUNT;
    std::vector<uint8_t> expected;
    store::Frame previous{};
    for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
        const store::Frame frame = Frame(static_cast<uint8_t>(index));
        root.slots[index] = static_cast<uint8_t>(index);
        Check(storage.WriteSlot(0U, static_cast<uint8_t>(index), frame) ==
                  layout::Status::Ok,
              "256 independent slots seed the compressed wire view");
        std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
        size_t length = 0U;
        const store::Frame* base = index % store::KEYFRAME_INTERVAL == 0U
            ? nullptr : &previous;
        Check(modules::litchcraft_wire_codec::FrameCodec::Encode(
                  base, frame, encoded.data(), encoded.size(), length),
              "reference schema-4 body encodes each frame");
        const size_t start = expected.size();
        expected.resize(start + store::WIRE_RECORD_HEADER_BYTES + length);
        fxe::WriteU16LE(static_cast<uint16_t>(length),
                        expected.data() + start);
        fxe::WriteU32LE(fxe::Crc32(encoded.data(), length),
                        expected.data() + start + 2U);
        std::memcpy(expected.data() + start + store::WIRE_RECORD_HEADER_BYTES,
                    encoded.data(), length);
        previous = frame;
    }
    std::array<uint8_t, store::WIRE_RECORD_HEADER_BYTES +
                            store::WIRE_FRAME_MAX_BYTES> record{};
    store::Frame oldFrame{};
    store::Frame currentFrame{};
    modules::litchcraft_fixed_slot_wire_view::WireView view(
        storage, root, record.data(), record.size(), oldFrame, currentFrame);
    uint32_t bodyBytes = 0U;
    uint32_t bodyCrc = 0U;
    Check(view.Measure(bodyBytes, bodyCrc) == layout::Status::Ok &&
              bodyBytes == expected.size() &&
              bodyCrc == fxe::Crc32(expected.data(), expected.size()),
          "fixed slots derive exact schema-4 body length and CRC");
    layout::Root measured{};
    Check(view.MeasureBlocks(measured) == layout::Status::Ok &&
              measured.bodyBytes == bodyBytes &&
              measured.bodyCrc32 == bodyCrc &&
              storage.PublishRoot(measured, 0U) == layout::Status::Ok,
          "eight compressed-block summaries publish with the fixed-slot root");
    layout::Root reopened{};
    uint8_t rootCopy = 2U;
    Check(storage.LatestRoot(0U, reopened, rootCopy) == layout::Status::Ok &&
              rootCopy == 0U &&
              reopened.blockBytes == measured.blockBytes &&
              reopened.blockCrc32 == measured.blockCrc32,
          "root recovery retains exact block identity summaries");
    file.reads = 0U;
    std::vector<uint8_t> actual;
    uint32_t offset = 0U;
    modules::litchcraft_fixed_slot_wire_view::WireView::Cursor cursor{};
    while (offset < bodyBytes) {
        modules::litchcraft_fixed_slot_wire_view::WireView resumed(
            storage, measured, record.data(), record.size(), oldFrame,
            currentFrame);
        if (offset != 0U) {
            Check(resumed.RestoreCursor(cursor),
                  "reverse read resumes its bounded cursor across worker calls");
        }
        std::array<uint8_t, 438U> chunk{};
        size_t size = 0U;
        const auto status = resumed.Read(offset, chunk.data(), chunk.size(), size);
        if (status != layout::Status::Ok || size == 0U) {
            Check(false, "sequential reverse chunks read through fixed slots");
            break;
        }
        cursor = resumed.CaptureCursor();
        actual.insert(actual.end(), chunk.begin(), chunk.begin() + size);
        offset += static_cast<uint32_t>(size);
    }
    Check(actual == expected && file.reads == store::FRAME_COUNT,
          "256-frame reverse read is byte-exact and visits each slot once");
    std::array<uint8_t, 67U> repeat{};
    size_t repeated = 0U;
    Check(view.Read(23U, repeat.data(), repeat.size(), repeated) ==
              layout::Status::Ok && repeated == repeat.size() &&
              std::equal(repeat.begin(), repeat.end(), expected.begin() + 23U),
          "retry from an older cursor reproduces the same bytes");

    const store::Frame replacement = Frame(201U);
    modules::litchcraft_fixed_slot_wire_view::WireView edited(
        storage, measured, record.data(), record.size(), oldFrame, currentFrame,
        7U, &replacement);
    const size_t before = file.writes;
    const size_t readBefore = file.reads;
    layout::Root changed{};
    const auto changedStatus = edited.MeasureChangedBlock(7U, changed);
    Check(changedStatus == layout::Status::Ok &&
              file.reads - readBefore == store::KEYFRAME_INTERVAL - 1U &&
              file.writes == before && changed.bodyCrc32 !=
                  fxe::Crc32(expected.data(), expected.size()),
          "one edit re-encodes exactly its 32-frame block without a bank write");
    layout::Root fullScan{};
    Check(edited.MeasureBlocks(fullScan) == layout::Status::Ok &&
              changed.bodyBytes == fullScan.bodyBytes &&
              changed.bodyCrc32 == fullScan.bodyCrc32 &&
              changed.blockBytes == fullScan.blockBytes &&
              changed.blockCrc32 == fullScan.blockCrc32,
          "changed-block identity equals a fresh full 256-frame encode");
    auto damaged = measured;
    damaged.bodyCrc32 ^= 1U;
    modules::litchcraft_fixed_slot_wire_view::WireView damagedView(
        storage, damaged, record.data(), record.size(), oldFrame, currentFrame,
        7U, &replacement);
    const size_t beforeDamaged = file.reads;
    Check(damagedView.MeasureChangedBlock(7U, changed) ==
              layout::Status::Corrupt && file.reads == beforeDamaged,
          "inconsistent root summaries cannot publish an incremental edit");
    layout::Root committed{};
    const size_t beforeCommit = file.writes;
    Check(storage.CommitFrameEdit(measured, 0U, 7U, replacement, 2U,
                                  fullScan, committed) == layout::Status::Ok &&
              file.writes - beforeCommit == layout::FRAME_SLOT_BYTES +
                                              layout::ROOT_BYTES &&
              committed.stageIndex == 7U &&
              committed.bodyBytes == fullScan.bodyBytes &&
              committed.bodyCrc32 == fullScan.bodyCrc32 &&
              committed.blockBytes == fullScan.blockBytes &&
              committed.blockCrc32 == fullScan.blockCrc32,
          "one physical staged edit publishes its canonical wire summaries");
    file.Crash();
    layout::Root recoveredEdit{};
    Check(storage.LatestRoot(0U, recoveredEdit, rootCopy) ==
              layout::Status::Ok && rootCopy == 1U &&
              recoveredEdit.blockBytes == fullScan.blockBytes &&
              recoveredEdit.blockCrc32 == fullScan.blockCrc32,
          "published edit and all block summaries recover after reopen");
    modules::litchcraft_fixed_slot_wire_view::WireView committedView(
        storage, recoveredEdit, record.data(), record.size(), oldFrame,
        currentFrame);
    uint32_t committedBytes = 0U;
    uint32_t committedCrc = 0U;
    Check(committedView.Measure(committedBytes, committedCrc) ==
              layout::Status::Ok &&
              committedBytes == recoveredEdit.bodyBytes &&
              committedCrc == recoveredEdit.bodyCrc32,
          "staged frame reads back under the exact committed wire identity");

    root.manifest.frameCount = 3U;
    root.manifest.predecessorDeltas = false;
    expected.clear();
    const store::Frame base = Frame(0U);
    for (uint16_t index = 0U; index < root.manifest.frameCount; ++index) {
        const store::Frame frame = Frame(static_cast<uint8_t>(index));
        std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
        size_t length = 0U;
        Check(modules::litchcraft_wire_codec::FrameCodec::Encode(
                  index == 0U ? nullptr : &base, frame, encoded.data(),
                  encoded.size(), length),
              "non-predecessor reference keeps frame zero as base");
        const size_t start = expected.size();
        expected.resize(start + store::WIRE_RECORD_HEADER_BYTES + length);
        fxe::WriteU16LE(static_cast<uint16_t>(length),
                        expected.data() + start);
        fxe::WriteU32LE(fxe::Crc32(encoded.data(), length),
                        expected.data() + start + 2U);
        std::memcpy(expected.data() + start + store::WIRE_RECORD_HEADER_BYTES,
                    encoded.data(), length);
    }
    modules::litchcraft_fixed_slot_wire_view::WireView baseView(
        storage, root, record.data(), record.size(), oldFrame, currentFrame);
    Check(baseView.Measure(bodyBytes, bodyCrc) == layout::Status::Ok &&
              bodyBytes == expected.size() &&
              bodyCrc == fxe::Crc32(expected.data(), expected.size()),
          "non-predecessor fixed slots retain exact base-frame wire identity");
    layout::Root nonPredecessorIdentity{};
    Check(baseView.MeasureBlocks(nonPredecessorIdentity) ==
              layout::Status::Ok,
          "non-predecessor root records a wire-block summary");
    std::vector<uint8_t> baseRead(bodyBytes);
    size_t baseSize = 0U;
    Check(baseView.Read(0U, baseRead.data(), baseRead.size(), baseSize) ==
              layout::Status::Ok && baseSize == bodyBytes &&
              baseRead == expected,
          "non-predecessor readback matches canonical schema-4 body");
    modules::litchcraft_fixed_slot_wire_view::WireView changedBase(
        storage, nonPredecessorIdentity, record.data(), record.size(),
        oldFrame, currentFrame, 0U, &replacement);
    layout::Root changedBaseIdentity{};
    layout::Root fullBaseIdentity{};
    Check(changedBase.MeasureChangedBlock(0U, changedBaseIdentity) ==
              layout::Status::Ok &&
              changedBase.MeasureBlocks(fullBaseIdentity) ==
                  layout::Status::Ok &&
              changedBaseIdentity.bodyBytes == fullBaseIdentity.bodyBytes &&
              changedBaseIdentity.bodyCrc32 == fullBaseIdentity.bodyCrc32,
          "editing non-predecessor base recomputes dependent wire identity");
}

void DerivedPagesUseExistingRootFilesAndPreserveOldPrefixes() {
    namespace fs = std::filesystem;
    using File = firmware::LitchcraftFrameFile;
    fs::create_directories("build");
    const std::array<std::string, 4U> paths{{
        "build/litch_summary_test.r0", "build/litch_summary_test.r1",
        "build/litch_summary_test.b0", "build/litch_summary_test.b1"}};
    { File cleanup(paths[0].c_str(), paths[1].c_str(), paths[2].c_str(), paths[3].c_str());
      Check(cleanup.Remove(), "fixture removes every checked artwork part"); }
    store::WireSummaries summaries{};
    for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
        summaries[index] = {13U, 13U, uint32_t(index + 1U), uint32_t(index + 2U)};
    }
    auto root = InitialRoot();
    {
        File file(paths[0].c_str(), paths[1].c_str(), paths[2].c_str(), paths[3].c_str());
        std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
        layout::Layout storage(file.StoreOps(), scratch.data(), scratch.size());
        Check(storage.WriteSlot(0U, 0U, Frame(1U)) == layout::Status::Ok &&
                  storage.WriteSlot(0U, 1U, Frame(2U)) == layout::Status::Ok &&
                  storage.PublishRoot(root, 0U, &summaries) == layout::Status::Ok,
              "real root file stores a durable CRC-bound derived tail");
        root.serial = 2U;
        root.manifest.active = true;
        Check(storage.PublishRoot(root, 1U) == layout::Status::Ok,
              "root-only mode publication retags the existing derived page");
        root.resident = 1U;
        Check(storage.PublishRoot(root, 0U, &summaries) == layout::Status::Ok,
              "second resident uses its existing second root file tail");
        file.CloseCachedHandles();
    }
    File::PhysicalSpan span{};
    Check(File::ResolvePhysicalSpan(layout::Layout::SummaryOffset(0U, 0U),
              store::WIRE_SUMMARY_PAGE_BYTES, span) &&
              span.region == File::Region::Summary0 && span.offset == layout::ROOT0_FILE_BYTES &&
              !File::ResolvePhysicalSpan(layout::FRAME_FILES_CAPACITY - 1U, 2U, span),
          "derived regions cannot overlap or cross the unchanged body bank end");
    {
        File file(paths[0].c_str(), paths[1].c_str(), paths[2].c_str(), paths[3].c_str());
        std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
        layout::Layout storage(file.StoreOps(), scratch.data(), scratch.size());
        layout::Root recovered{};
        uint8_t copy = 2U;
        store::WireSummaries cached{};
        store::Frame actual{};
        Check(storage.LatestRoot(0U, recovered, copy) == layout::Status::Ok && copy == 1U &&
                  storage.ReadWireSummaries(recovered, copy, cached) == layout::Status::Ok &&
                  cached[255U].deltaCrc32 == summaries[255U].deltaCrc32 &&
                  storage.ReadFrame(recovered, 1U, actual) == layout::Status::Ok &&
                  Same(actual, Frame(2U)),
              "old root/frame prefix and exact serial-bound cache recover together");
        auto wrong = recovered;
        ++wrong.serial;
        Check(storage.ReadWireSummaries(wrong, copy, cached) == layout::Status::RevisionConflict,
              "same generation with another serial cannot reuse stale cache");
        file.CloseCachedHandles();
    }
    Check(fs::file_size(paths[0]) == layout::ROOT0_FILE_BYTES + layout::SUMMARY_RESIDENT_BYTES &&
              fs::file_size(paths[1]) == layout::ROOT1_FILE_BYTES + store::WIRE_SUMMARY_PAGE_BYTES &&
              fs::file_size(paths[2] + ".00") == 2U * layout::FRAME_SLOT_BYTES && !fs::exists(paths[3] + ".00"),
          "summary tails add no new file or inactive bank padding");
    { File cleanup(paths[0].c_str(), paths[1].c_str(), paths[2].c_str(), paths[3].c_str());
      Check(cleanup.Remove(), "fixture removes every checked artwork part"); }
}

void ProductionBodiesBoundCopyWorkAndPreserveArbitraryFrames() {
    namespace fs = std::filesystem;
    const std::array<std::string, 4U> paths{
        "build/bounded_artwork.r0", "build/bounded_artwork.r1",
        "build/bounded_artwork.b0", "build/bounded_artwork.b1"};
    fs::create_directories("build");
    firmware::LitchcraftFrameFile file(paths[0].c_str(), paths[1].c_str(), paths[2].c_str(), paths[3].c_str());
    Check(file.Remove(), "bounded body fixture starts with only its own artwork removed");
    store::FrameStore frames(file.StoreOps());
    Check(frames.Recover() == store::Status::NoArtwork, "bounded body fixture recovers empty");
    store::FrameStore::ResidentDescriptor residents[2]{};
    std::array<std::vector<store::Frame>, 2U> expected;
    for (uint8_t bank = 0U; bank < 2U; ++bank) {
        store::Manifest manifest{}; manifest.frameCount = 256U; manifest.active = true;
        store::TransactionToken token{};
        Check(frames.Begin(manifest, frames.Generation(), 0x534547U, token) == store::Status::Ok,
              "bounded body begins a full 256-frame resident through production Store");
        for (uint16_t frame = 0U; frame < 256U; ++frame) {
            expected[bank].push_back(Frame(static_cast<uint8_t>(frame + bank * 37U)));
            Check(frames.WriteFrame(token, frame, expected[bank].back()) == store::Status::Ok,
                  "bounded body writes each independently addressed full frame");
        }
        Check(frames.Commit(token) == store::Status::Ok && frames.CurrentResident(residents[bank]),
              "bounded body commits both dense residents with checked publication");
    }
    constexpr size_t fileLimit = 16U * layout::FRAME_SLOT_BYTES;
    file.ResetAccessTiming();
    for (const uint16_t index : {0U, 15U, 16U, 255U, 127U, 1U, 254U, 17U}) {
        store::TransactionToken token{};
        auto replacement = expected[1U][index];
        replacement.emitters[9U].color = {240U, static_cast<uint8_t>(index), 71U};
        replacement.durationMs = static_cast<uint16_t>(700U + index);
        Check(frames.BeginEdit(frames.Generation(), 0x45444954U, token) == store::Status::Ok &&
                  frames.ReplaceFrame(token, index, replacement) == store::Status::Ok &&
                  frames.Commit(token) == store::Status::Ok,
              "arbitrary early boundary and last-frame replacements retain existing transaction verification");
        expected[1U][index] = replacement;
    }
    Check(frames.CurrentResident(residents[1U]), "bounded body captures latest edited identity");
    file.CloseCachedHandles();
    uintmax_t maximumBodyBytes = 0U;
    for (uint8_t bank = 0U; bank < 2U; ++bank) {
        if (fs::exists(paths[2U + bank])) maximumBodyBytes = std::max(maximumBodyBytes, fs::file_size(paths[2U + bank]));
        for (uint8_t part = 0U; part < 16U; ++part) {
            char suffix[8]{}; std::snprintf(suffix, sizeof(suffix), ".%02u", unsigned(part));
            const auto path = paths[2U + bank] + suffix;
            if (fs::exists(path)) maximumBodyBytes = std::max(maximumBodyBytes, fs::file_size(path));
            Check(fs::exists(path) && fs::file_size(path) <= fileLimit,
                  "each physical body file is bounded to sixteen slots");
        }
    }
    Check(maximumBodyBytes <= fileLimit, "an early-frame fold MUST NOT rewrite a complete 256-frame physical bank");
    store::FrameStore reopened(file.StoreOps());
    Check(reopened.Recover() == store::Status::Ok, "bounded body fresh Store reopens after all random edits");
    for (uint8_t bank = 0U; bank < 2U; ++bank) {
        Check(reopened.SelectResident(residents[bank].rootSlot, residents[bank].generation, residents[bank].bodyCrc32) == store::Status::Ok,
              "bounded body selects either exact resident without rewriting its frames");
        for (uint16_t index = 0U; index < 256U; ++index) {
            store::Frame actual{};
            Check(reopened.ReadFrame(index, actual) == store::Status::Ok && Same(actual, expected[bank][index]),
                  "all 512 frames retain exact RGB effects effect durations and dwell across file boundaries and arbitrary replacement order");
        }
    }
    std::printf("[BOUNDED_ARTWORK_FILES] resident_frames=256 residents=2 max_body_file=%zu maximum_physical_body=%zu reader_writer=unchanged exact512=1 failures=%d\n",
                fileLimit, static_cast<size_t>(maximumBodyBytes), failures);
    const std::array<uint8_t, 4U> boundaryBytes{91U, 7U, 203U, 54U};
    std::array<uint8_t, 4U> observed{};
    const size_t boundary = layout::Layout::BodyOffset(0U) + fileLimit - 2U;
    const auto ops = file.StoreOps();
    Check(ops.read(ops.context, boundary, observed.data(), observed.size()) &&
              ops.write(ops.context, boundary, boundaryBytes.data(), boundaryBytes.size()) &&
              ops.sync(ops.context) &&
              ops.read(ops.context, boundary, observed.data(), observed.size()) && observed == boundaryBytes,
          "raw logical access crossing a part boundary splits exactly and invalidates both stale reader positions");
    Check(file.Remove(), "bounded body fixture removes all its artwork parts through the checked lifecycle");
}

void RecognizedPrelaunchLC5IsDistinctFromCorruption() {
    MemoryFile file;
    std::array<uint8_t, layout::FRAME_SLOT_BYTES> scratch{};
    layout::Layout storage(file.Ops(), scratch.data(), scratch.size());
    const auto root = InitialRoot();
    Check(storage.PublishRoot(root, 0U) == layout::Status::Ok,
          "development format fixture publishes a current CRC-valid root");
    const auto at = layout::Layout::RootOffset(0U, 0U);
    Check(file.bytes[at + 2U] == '6' && file.bytes[at + 4U] == 6U,
          "segmented development format is explicitly versioned");
    file.bytes[at + 2U] = '5'; file.bytes[at + 4U] = 5U;
    fxe::WriteU32LE(fxe::Crc32(file.bytes.data() + at, layout::ROOT_BYTES - 4U),
                    file.bytes.data() + at + layout::ROOT_BYTES - 4U);
    const auto before = file.bytes;
    const auto writes = file.writes;
    layout::Root rejected{};
    Check(storage.ReadRoot(0U, 0U, rejected) == layout::Status::PrelaunchLC5 &&
              file.bytes == before && file.writes == writes,
          "only the recognized CRC-valid prelaunch LC5 format gets the one-time reset classification");
    store::FrameStore classified(file.Ops());
    Check(classified.Recover() == store::Status::PrelaunchLC5 &&
              file.bytes == before && file.writes == writes,
          "Store classification itself never removes or rewrites artwork");
    const auto recover = [&] {
        store::FrameStore reopened(file.Ops());
        return reopened.Recover(store::FrameStore::RecoveryPurpose::RootOnly);
    };
    const auto crc = [&] {
        fxe::WriteU32LE(fxe::Crc32(file.bytes.data() + at, layout::ROOT_BYTES - 4U),
                        file.bytes.data() + at + layout::ROOT_BYTES - 4U);
    };
    file.bytes[at + 12U] ^= 1U;
    Check(recover() == store::Status::Corrupt && file.writes == writes,
          "damaged LC5 CRC never authorizes automatic deletion");
    file.bytes = before; file.bytes[at + 2U] = '7'; file.bytes[at + 4U] = 7U; crc();
    Check(recover() == store::Status::Corrupt && file.writes == writes,
          "unknown future artwork format never authorizes automatic deletion");
    file.bytes = before; file.bytes[at + 2U] = '6'; file.bytes[at + 4U] = 6U; crc();
    file.bytes[at + 12U] ^= 1U;
    Check(recover() == store::Status::Corrupt && file.writes == writes,
          "current LC6 corruption never becomes the prelaunch reset exception");
    file.bytes = before;
    Check(storage.PublishRoot(root, 1U) == layout::Status::Ok &&
              recover() == store::Status::Ok,
          "a valid current LC6 copy wins over an old LC5 copy");
    file.bytes = before;
    auto currentOther = root; currentOther.resident = 1U;
    Check(storage.PublishRoot(currentOther, 0U) == layout::Status::Ok &&
              recover() == store::Status::Ok,
          "a valid current LC6 resident wins over an old LC5 resident");
    file.bytes = before;
    file.failRead = true;
    const auto beforeIo = file.bytes;
    const auto writesBeforeIo = file.writes;
    Check(recover() == store::Status::IoFailure && file.bytes == beforeIo &&
              file.writes == writesBeforeIo,
          "an unreadable root preserves all bytes and never authorizes reset");
    file.failRead = false;
    store::FrameStore replacement(file.Ops());
    store::Manifest manifest{}; manifest.frameCount = 1U; manifest.active = true;
    store::TransactionToken token{};
    Check(replacement.Recover() == store::Status::PrelaunchLC5 &&
              replacement.Begin(manifest, 0U, 0x4e4557U, token) == store::Status::Ok &&
              replacement.WriteFrame(token, 0U, Frame(17U)) == store::Status::Ok &&
              replacement.Commit(token) == store::Status::Ok,
          "an explicit new artwork can replace the obsolete development root through existing checked publication");
    store::FrameStore reopened(file.Ops()); store::Frame actual{};
    Check(reopened.Recover() == store::Status::Ok && reopened.ReadFrame(0U, actual) == store::Status::Ok && Same(actual, Frame(17U)),
          "the explicit replacement reopens as the new format without a legacy reader or migration path");
}

} // namespace

int main() {
    if (std::getenv("LITCH_BOUNDED_BODY_ONLY") != nullptr) {
        ProductionBodiesBoundCopyWorkAndPreserveArbitraryFrames();
        return failures == 0 ? 0 : 1;
    }
    ProductionBodiesBoundCopyWorkAndPreserveArbitraryFrames();
    RecognizedPrelaunchLC5IsDistinctFromCorruption();
    CompleteRootReadbackRejectsEveryCorruptByte();
    EditsFoldWithoutCopyingTheBank();
    PowerCutsKeepOldOrNewCompleteFrame();
    PowerCutsDuringFoldKeepTheStagedEdit();
    CorruptSlotNeverPublishesPartialPixels();
    Dense256FrameEditKeepsWritesBounded();
    ProductionFileAdapterMapsAllFixedSlotRegions();
    FixedSlotsReconstructOneCompressedWireBody();
    DerivedPagesUseExistingRootFilesAndPreserveOldPrefixes();
    std::printf("[LITCHCRAFT_FRAME_STORE_LAYOUT] failures=%d capacity=%zu\n",
                failures, layout::ARTWORK_FILE_CAPACITY);
    return failures == 0 ? 0 : 1;
}
