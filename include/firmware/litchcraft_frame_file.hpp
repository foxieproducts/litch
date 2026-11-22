#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#if defined(LITCH_NATIVE)
#include <atomic>
#endif

#include "modules/litchcraft_frame_store.hpp"
#include "modules/litchcraft_frame_slot_codec.hpp"
#include "foxie_embedded/fixed_string.hpp"

namespace firmware {

// LittleFS random-access adapter. Every bounded read/write call acquires the
// existing RMT/flash lease; callers remain responsible for worker ownership.
// Keep each fixed-slot access inside one physical LittleFS file.
class LitchcraftFrameFile final {
  public:
    static constexpr size_t FRAMES_PER_BODY_FILE = 16U;
    static constexpr size_t BODY_FILES_PER_BANK = modules::litchcraft_frame_store::FRAME_COUNT / FRAMES_PER_BODY_FILE;
    static constexpr size_t BODY_FILE_BYTES = FRAMES_PER_BODY_FILE *
        (modules::litchcraft_frame_store::WIRE_RECORD_HEADER_BYTES +
         modules::litchcraft_frame_slot_codec::PACKED_FRAME_BYTES);
    static constexpr size_t ACCESS_BYTES =
        modules::litchcraft_frame_store::STORE_INGRESS_BATCH_BYTES;
#if defined(LITCH_NATIVE)
    static constexpr const char* DEFAULT_ROOT0_PATH =
        "build/litchcraft_frames.r0";
    static constexpr const char* DEFAULT_ROOT1_PATH =
        "build/litchcraft_frames.r1";
    static constexpr const char* DEFAULT_BODY0_PATH =
        "build/litchcraft_frames.b0";
    static constexpr const char* DEFAULT_BODY1_PATH =
        "build/litchcraft_frames.b1";
#else
    static constexpr const char* DEFAULT_ROOT0_PATH = "/litchcraft_frames.r0";
    static constexpr const char* DEFAULT_ROOT1_PATH = "/litchcraft_frames.r1";
    static constexpr const char* DEFAULT_BODY0_PATH = "/litchcraft_frames.b0";
    static constexpr const char* DEFAULT_BODY1_PATH = "/litchcraft_frames.b1";
#endif
    enum class Region : uint8_t { Root0 = 0U, Root1, Body0, Body1, Summary0, Summary1, BodyPartBase };
    struct PhysicalSpan final {
        Region region = Region::Root0;
        size_t offset = 0U;
    };
    enum class AccessOperation : uint8_t { None, Read, Write, Sync };
    struct AccessOps final {
        void* context = nullptr;
        void (*begin)(void*) = nullptr;
        void (*end)(void*, AccessOperation, size_t, uint32_t) = nullptr;
    };
    struct SyncPhaseTiming final {
        uint32_t totalMs = 0U;
        uint32_t maximumMs = 0U;
        uint32_t calls = 0U;
    };
    struct SyncTiming final {
        // Begin, Prepare and Publish, matching the existing edit timing record.
        std::array<SyncPhaseTiming, 3U> phases{};
        uint32_t maximumMs = 0U;
        uint32_t maximumFileBytes = 0U;
        uint32_t maximumCursor = 0U;
        Region maximumRegion = Region::Root0;
        Region maximumFileRegion = Region::Root0;
        uint8_t maximumPhase = UINT8_MAX;
    };
    struct AccessTiming final {
        uint32_t maximumMs = 0U;
        uint32_t maximumLeaseWaitMs = 0U;
        uint32_t maximumBytes = 0U;
        uint32_t maximumOffset = 0U;
        uint32_t accesses = 0U;
        AccessOperation maximumOperation = AccessOperation::None;
        SyncTiming sync{};
    };
    // Storage-worker diagnostics. Reset immediately before the measured slice;
    // elapsed access includes lease acquisition and physical filesystem work.
    void ResetAccessTiming() {
        m_accessTiming = {};
        m_editTimingPhase = 0U;
    }
    void SetEditTimingPhase(
        modules::litchcraft_frame_store::FrameStore::TimingPhase phase);
    const AccessTiming& GetAccessTiming() const { return m_accessTiming; }

    struct BenchmarkResult final {
        bool passed = false;
        uint32_t bytes = 0U;
        uint32_t writeMs = 0U;
        uint32_t readMs = 0U;
        uint32_t maximumAccessMs = 0U;
        uint32_t accesses = 0U;
    };

    explicit LitchcraftFrameFile(
        const char* root0Path = DEFAULT_ROOT0_PATH,
        const char* root1Path = DEFAULT_ROOT1_PATH,
        const char* body0Path = DEFAULT_BODY0_PATH,
        const char* body1Path = DEFAULT_BODY1_PATH)
        : m_paths{root0Path, root1Path, body0Path, body1Path} {}
    ~LitchcraftFrameFile();

    LitchcraftFrameFile(const LitchcraftFrameFile&) = delete;
    LitchcraftFrameFile& operator=(const LitchcraftFrameFile&) = delete;

    bool SetAccessOps(AccessOps ops);
    modules::litchcraft_frame_store::FrameStore::Ops StoreOps();
    bool Remove();
    // Close retained reads before a worker storage union is destroyed.
    // May wait for the next safe flash-access window when a handle is open.
    void CloseCachedHandles();
    bool Benchmark(uint32_t bytes, BenchmarkResult& out, uint8_t* page, size_t pageCapacity);
#if defined(LITCH_NATIVE)
    uint32_t BodyReadOpensForTest() const { return m_bodyReadOpens; }
    uint32_t ReadOpensForTest() const { return m_readOpens; }
    void BlockRootWritesForTest(bool blocked) {
        m_blockRootWritesForTest.store(blocked, std::memory_order_release);
    }
#endif
    static bool ResolvePhysicalSpan(size_t logicalOffset, size_t size,
                                    PhysicalSpan& out);

  private:
    static bool Read(void* context, size_t offset, uint8_t* out, size_t size);
    static bool Write(void* context, size_t offset, const uint8_t* bytes,
                      size_t size);
    static bool Sync(void* context);
    static bool PrepareReplacementBank(void* context, uint8_t resident);
    bool ReadAt(size_t offset, uint8_t* out, size_t size);
    bool WriteAt(size_t offset, const uint8_t* bytes, size_t size);
    static size_t ContiguousBytes(size_t offset, size_t remaining);
    static Region BodyRegion(uint8_t resident, uint8_t part);
    static bool IsBody(Region region);
    static uint8_t BodyResident(Region region);
    static uint8_t BodyPart(Region region);
    bool BuildPath(Region region, fxe::FixedString<105U>& out, bool mounted = false) const;
    bool SyncWrites();
    bool PrepareBank(uint8_t resident);
    bool PrepareBodyPart(uint8_t resident, uint8_t part);
    void CloseRead();
    bool CloseWrite();
    void BeginAccess();
    void EndAccess(AccessOperation operation = AccessOperation::None,
                   size_t actualBytes = 0U, uint32_t elapsedMs = 0U);
    void RecordAccess(AccessOperation operation, size_t offset, size_t size,
                      uint32_t elapsedMs);
    void RecordSync(Region region, size_t fileBytes, size_t cursor,
                    uint32_t elapsedMs);

    static Region FileRegion(Region region) {
        if (region == Region::Summary0) return Region::Root0;
        if (region == Region::Summary1) return Region::Root1;
        return region;
    }

    const char* m_paths[4U]{};
    AccessOps m_accessOps{};
    AccessTiming m_accessTiming{};
    uint8_t m_editTimingPhase = 0U;
#if defined(ARDUINO) || defined(LITCH_NATIVE)
    std::FILE* m_readFile = nullptr;
    Region m_readRegion = Region::Body0;
    size_t m_readSize = 0U;
#endif
#if defined(LITCH_NATIVE)
    std::atomic<bool> m_blockRootWritesForTest{false};
    uint32_t m_bodyReadOpens = 0U;
    uint32_t m_readOpens = 0U;
#endif
    // Replacement marks untouched parts; written parts must never reset again.
    std::array<uint16_t, 2U> m_pendingBodyPreparation{};
#if defined(ARDUINO) || defined(LITCH_NATIVE)
    int m_writeFd = -1;
    // A failed barrier cannot turn into success just because close consumed fd.
    int m_writeError = 0;
    Region m_writeRegion = Region::Root0;
    bool m_writeRegionValid = false;
    size_t m_writeCursor = 0U;
    size_t m_writeSize = 0U;
#endif
};

} // namespace firmware
