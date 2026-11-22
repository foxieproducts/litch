#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/litchcraft_animation.hpp"
#include "modules/rgb.hpp"

namespace modules::litchcraft_frame_stream { class FrameStream; }

namespace modules::litchcraft_frame_store {

inline constexpr uint8_t SCHEMA = 4U;
inline constexpr uint16_t FRAME_COUNT = 256U;
inline constexpr uint16_t KEYFRAME_INTERVAL = 32U;
inline constexpr uint16_t EMITTER_COUNT = 256U;
inline constexpr size_t WIRE_FRAME_MAX_BYTES = 1123U;
inline constexpr size_t WIRE_RECORD_HEADER_BYTES = 6U;
inline constexpr size_t WIRE_BODY_MAX_BYTES =
    FRAME_COUNT * (WIRE_RECORD_HEADER_BYTES + WIRE_FRAME_MAX_BYTES);
inline constexpr size_t STAGE_SLOT_BYTES = 1097U;
inline constexpr size_t STORE_INGRESS_BATCH_BYTES = 4096U;
inline constexpr size_t BODY_BANK_BYTES = FRAME_COUNT * STAGE_SLOT_BYTES;
inline constexpr size_t ROOT_BYTES = 384U;
inline constexpr size_t WIRE_SUMMARY_BYTES = 12U;
inline constexpr size_t WIRE_SUMMARY_HEADER_BYTES = 32U;
inline constexpr size_t WIRE_SUMMARY_PAGE_BYTES =
    WIRE_SUMMARY_HEADER_BYTES + FRAME_COUNT * WIRE_SUMMARY_BYTES + 4U;
inline constexpr size_t FRAME_FILES_CAPACITY =
    4U * ROOT_BYTES + STAGE_SLOT_BYTES + 2U * BODY_BANK_BYTES;
inline constexpr size_t ARTWORK_FILE_CAPACITY =
    FRAME_FILES_CAPACITY + 4U * WIRE_SUMMARY_PAGE_BYTES;
inline constexpr size_t LITTLEFS_PARTITION_BYTES = 1638400U;
inline constexpr size_t GAME_LOG_RESERVE_BYTES = 729326U;
inline constexpr size_t FILESYSTEM_SETTINGS_RECOVERY_REMAINDER_BYTES =
    LITTLEFS_PARTITION_BYTES - GAME_LOG_RESERVE_BYTES - ARTWORK_FILE_CAPACITY;

struct Emitter final {
    Rgb color{};
    uint8_t effect = 0U;
    litchcraft_animation::EffectDuration effectDurationMs{};
};

struct Frame final {
    uint16_t durationMs = 1000U;
    std::array<Emitter, EMITTER_COUNT> emitters{};
};

static_assert(sizeof(Emitter) == 5U, "stored emitter retains exact RGB/effect/duration in five bytes");
static_assert(sizeof(Frame) == 1282U, "frame keeps uint16 dwell and all 256 compact emitters");

struct Manifest final {
    uint16_t frameCount = 1U;
    uint8_t topology = 3U;
    bool active = false;
    bool playbackOnEntry = false;
    bool predecessorDeltas = false;
    uint32_t sparkleSeed = 1U;
    std::array<Rgb, 8U> favorites{};
};

// Both complete canonical records include their length/CRC header. Logical
// adjacency owns the delta; aliases of one physical slot can differ here.
struct WireSummary final {
    uint16_t absoluteBytes = 0U;
    uint16_t deltaBytes = 0U;
    uint32_t absoluteCrc32 = 0U;
    uint32_t deltaCrc32 = 0U;
};
using WireSummaries = std::array<WireSummary, FRAME_COUNT>;
using WireSummaryEdits = std::array<bool, FRAME_COUNT>;
static_assert(sizeof(WireSummary) == WIRE_SUMMARY_BYTES);

// One resident's durable fixed-slot authority. The storage layout owns its
// serialization; the Store and the wire view share this value directly.
struct FixedRoot final {
    Manifest manifest{};
    std::array<uint8_t, FRAME_COUNT> slots{};
    uint32_t generation = 0U;
    uint32_t serial = 0U;
    uint32_t bodyBytes = 0U;
    uint32_t bodyCrc32 = 0U;
    std::array<uint32_t, FRAME_COUNT / KEYFRAME_INTERVAL> blockBytes{};
    std::array<uint32_t, FRAME_COUNT / KEYFRAME_INTERVAL> blockCrc32{};
    uint16_t stageIndex = 0xffffU;
    uint8_t resident = 0U;
    bool retired = false;
};

enum class Status : uint8_t {
    Ok = 0U,
    NotConfigured,
    NoArtwork,
    InvalidArgument,
    RevisionConflict,
    Incomplete,
    Corrupt,
    IoFailure,
    PrelaunchLC5, // Only the recognized, unreleased monolithic development format.
};

struct PresentationAdmission final {
    enum class Kind : uint8_t { Accepted, PendingPresentation, Rejected };
    Kind kind = Kind::Rejected;
    bool dirty = false;
};

// Only the incoming transaction uses RGB wire records. Durable roots and
// reverse reads always identify the canonical legacy re-encoding.
enum class WireBodyMode : uint8_t { Legacy, Rgb };

enum class MutationScope : uint8_t { Unavailable = 0U, Frame, Insert, Delete };

// One publication-lock capture. A zero base revision means no covered mutation.
struct LiveSnapshot final {
    uint32_t generation = 0U;
    uint32_t revision = 0U;
    uint32_t documentEpoch = 0U;
    uint32_t baseRevision = 0U;
    uint16_t frameCount = 0U;
    uint16_t frameIndex = 0U;
    uint16_t scopeIndex = 0U;
    uint16_t scopeAux = 0U;
    MutationScope scope = MutationScope::Unavailable;
};

struct LiveRead final {
    LiveSnapshot snapshot{};
    uint32_t pageEpoch = 0U;
    uint32_t bodyBytes = 0U;
    uint32_t bodyCrc32 = 0U;
    bool layout = false;
};

struct TransactionToken final {
    uint32_t generation = 0U;
    uint32_t nonce = 0U;
    uint32_t ownerEpoch = 0U;
};

class FrameStore final {
  public:
    enum class TimingPhase : uint8_t {
        BodyWrite = 0U,
        BodyPrepare,
        RootPublish,
        EditBegin,
    };
    using TimingObserver = void (*)(void*, TimingPhase, uint32_t);

    struct ResidentDescriptor final {
        Manifest manifest{};
        uint32_t generation = 0U;
        uint32_t bodyBytes = 0U;
        uint32_t bodyCrc32 = 0U;
        uint8_t rootSlot = 0U;
    };

    struct ReadCursor final {
        uint32_t generation = 0U;
        uint32_t pageEpoch = 0U;
        uint16_t targetIndex = 0U;
        uint16_t encodedRead = 0U;
        bool active = false;
    };

    struct Ops final {
        void* context = nullptr;
        bool (*read)(void*, size_t, uint8_t*, size_t) = nullptr;
        bool (*write)(void*, size_t, const uint8_t*, size_t) = nullptr;
        bool (*sync)(void*) = nullptr;
        // Optional for flat storage. Called once before a whole replacement's
        // first bytes, after both inactive roots are durably invalidated. It
        // must preserve every other resident and acknowledge durable reset.
        bool (*prepareReplacementBank)(void*, uint8_t resident) = nullptr;
    };

    explicit FrameStore(Ops ops) : m_ops(ops) {}

    // Diagnostics only. The observer is invoked on the caller's storage
    // thread after a bounded body write, body durability sync, or root
    // publication, or after Edit Begin (including summary readiness/fold).
    // It never changes admission or persistence behavior.
    void SetTimingObserver(TimingObserver observer, void* context) {
        m_timingObserver = observer;
        m_timingContext = context;
    }

    // An ordinary edit may page the committed document after each completed
    // file operation. The callback is worker-only, may only page committed
    // artwork, and borrows idle wire/decode scratch during fold/publication.
    using EditPagingCallback = void (*)(void*, FrameStore&);
    void SetEditPagingCallback(EditPagingCallback callback, void* context) {
        m_editPagingCallback = callback;
        m_editPagingContext = context;
    }

    // Storage-worker-only ingress scratch. The receiver drains this complete
    // batch before PrepareCommit switches its lifetime to derived summaries.
    // A current cache is reloaded from its bound page after aborted ingress.
    using IngressBody = std::array<uint8_t, STORE_INGRESS_BATCH_BYTES>;
    IngressBody& IngressBodyBuffer();

    enum class RecoveryPurpose : uint8_t { EditReady, RootOnly };
    // RootOnly restores authority without reading derived edit summaries.
    // A boot worker publishes the saved mode before preparing those summaries.
    Status Recover(RecoveryPurpose purpose = RecoveryPurpose::EditReady);
    // Storage-worker-only optional cache preparation. Failure does not change
    // recovered root authority; an actual edit still requires its own readiness.
    // With rebuild disabled, only read the existing root-bound cache. Missing
    // evidence stays Incomplete; LIVE reads never repair it implicitly.
    Status EnsureWireSummaries(bool allowRebuild = true,
        bool (*continueWork)(void*) = nullptr, void* context = nullptr);
    // Explicit local clear publishes a durable retired root above both
    // residents and the live stream's retirement fence. Unlike boot recovery,
    // it may replace corrupt roots; artwork bodies remain untouched.
    Status Clear(uint32_t minimumGeneration);
    // After the replacement schema-3 file is durable, retire an older streamed
    // root without rewriting artwork or constructing another store working set.
    // Zero generation resolves an as-yet unpublished root. Optionally returns
    // the retired bound so the stream can finish that boot-time handoff.
    static Status Retire(const Ops& ops, uint32_t retiredGeneration,
                         uint32_t* resolvedGeneration = nullptr);
    Status Begin(const Manifest& manifest, uint32_t expectedGeneration,
                 uint32_t ownerEpoch, TransactionToken& outToken);
    Status BeginWire(const Manifest& manifest, uint32_t expectedGeneration,
                     uint32_t ownerEpoch, uint32_t bodyBytes,
                     uint32_t bodyCrc32, TransactionToken& outToken,
                     WireBodyMode mode = WireBodyMode::Legacy);
    Status BeginEdit(uint32_t expectedGeneration, uint32_t ownerEpoch,
                     TransactionToken& outToken);
    // Publish a mode-only root without writing any artwork frame slots.
    Status UpdateModeActive(bool active, uint32_t expectedGeneration);
    Status WriteBodyChunk(const TransactionToken& token, uint32_t bodyOffset,
                          const uint8_t* bytes, size_t size);
    // Consume only a prefix, stopping after the shared frame budget is spent.
    // CRC/cursor advance exactly once for consumed bytes; caller retains suffix.
    Status WriteBodyChunk(const TransactionToken& token, uint32_t bodyOffset,
                          const uint8_t* bytes, size_t size, size_t& consumed,
                          uint16_t& frameBudget);
    Status Checkpoint(const TransactionToken& token, uint32_t bodyOffset);
    Status WriteFrame(const TransactionToken& token, uint16_t index,
                      const Frame& frame);
    // A fresh one-frame Generated transaction uses owned scratch for recovery
    // so its blank Frame does not occupy the bounded storage worker stack.
    Status WriteBlankFirstFrame(const TransactionToken& token);
    Status ReplaceFrame(const TransactionToken& token, uint16_t index,
                        const Frame& frame);
    Status InsertFrame(const TransactionToken& token, uint16_t index,
                       const Frame& frame);
    Status AppendFrame(const TransactionToken& token, const Frame& frame);
    Status CopyFrameAfter(const TransactionToken& token, uint16_t index);
    Status DeleteFrame(const TransactionToken& token, uint16_t index);
    // A validated wire body can become live presentation authority before its
    // root is published. Publication may be retried with the same token after
    // an uncertain flash failure; neither phase rewrites the accepted body.
    Status PrepareCommit(const TransactionToken& token);
    Status PublishPreparedCommit(const TransactionToken& token);
    Status Commit(const TransactionToken& token);
    Status Abort(const TransactionToken& token);
    Status BeginReadFrame(uint16_t index, ReadCursor& cursor);
    // On Ok, DecodedFrame() holds the completed read in existing Store scratch.
    Status ContinueReadFrame(ReadCursor& cursor, size_t maximumBytes);
    void CancelReadFrame(ReadCursor& cursor);
    Status ReadFrame(uint16_t index, Frame& out);
    Status ReadWireBody(uint32_t expectedGeneration, uint32_t bodyOffset,
                        uint8_t* out, size_t capacity,
                        size_t& outSize);
    // One independently encoded keyframe record from the committed fixed slot.
    // Reuses the worker-owned wire/decode scratch and invalidates its body
    // cursor; its length and CRC cover only this record.
    Status ReadWireFrame(uint32_t expectedGeneration, uint16_t frameIndex,
                         uint32_t cursor, uint8_t* out, size_t capacity,
                         size_t& outSize, uint32_t& totalBytes,
                         uint32_t& bodyCrc32);
    // Worker-only LIVE reads borrow the same decode, wire and summary scratch.
    // Cursor zero captures authority; later chunks never recapture or seek.
    Status CaptureLiveRead(litchcraft_frame_stream::FrameStream& stream,
                          uint16_t selector, LiveRead& read);
    Status ReadLiveChunk(const LiveRead& read, uint32_t cursor,
                         uint8_t* out, size_t capacity, size_t& outSize) const;

    uint8_t ResidentCount() const;
    bool Resident(uint8_t rootSlot, ResidentDescriptor& out) const;
    bool CurrentResident(ResidentDescriptor& out) const;
    Status SelectResident(uint8_t rootSlot, uint32_t expectedGeneration,
                          uint32_t expectedBodyCrc32);
    // Undo an immediately preceding in-memory selection when presentation
    // rejects it. This never reads or writes flash.
    bool UndoResidentSelection(const ResidentDescriptor& previous);
    // Worker-owned decode scratch. Consume immediately after a successful
    // ContinueReadFrame or SelectResident; any other Store operation may
    // invalidate it. Presentation copies it into its own protected cache.
    const Frame& DecodedFrame() const { return m_decodeCachedFrame; }

    // Frame zero becomes available after its own CRC-validated record is
    // parsed. It remains stable until the transaction is committed or aborted;
    // presentation waits for complete-body validation at Commit admission.
    bool IncomingPreviewReady() const;
    const Manifest& IncomingManifest() const { return m_incomingRoot.manifest; }
    uint32_t IncomingGeneration() const { return m_incomingRoot.generation; }
    const Frame& IncomingFirstFrame() const { return m_editFrame; }

    bool HasArtwork() const { return m_hasCurrent; }
    uint32_t Generation() const { return m_currentRoot.generation; }
    const Manifest& CurrentManifest() const { return m_currentRoot.manifest; }
    uint32_t CurrentBodyBytes() const { return m_currentRoot.bodyBytes; }
    uint32_t CurrentBodyCrc32() const { return m_currentRoot.bodyCrc32; }
    static constexpr size_t PageCacheBytes() {
        return WIRE_RECORD_HEADER_BYTES + WIRE_FRAME_MAX_BYTES;
    }
    static constexpr size_t RootOffset(const uint8_t slot) {
        return slot == 0U ? 0U : 2U * ROOT_BYTES + STAGE_SLOT_BYTES;
    }
    static constexpr size_t BodyOffset(const uint8_t slot) {
        return 4U * ROOT_BYTES + STAGE_SLOT_BYTES +
            size_t(slot) * BODY_BANK_BYTES;
    }

  private:
    enum class TransactionKind : uint8_t { None, Generated, Wire, Edit };

    bool IsConfigured() const;
    Status StartTransaction(const Manifest& manifest,
                            uint32_t expectedGeneration, uint32_t ownerEpoch,
                            TransactionKind kind, uint32_t bodyBytes,
                            uint32_t bodyCrc, TransactionToken& outToken);
    Status ParseWireBytes(const uint8_t* bytes, size_t size);
    Status ParseWireBytes(const uint8_t* bytes, size_t size, size_t& consumed,
                          uint16_t& frameBudget);
    Status FinishRecord();
    Status PrepareIncomingBodyWrite();
    Status PrepareEdit();
    WireSummaries& SummaryWorkspace();
    bool LoadWireSummaries();
    Status LoadBoundWireSummaries();
    Status MeasureWireSummaries(const FixedRoot& root, WireSummaries& out,
        bool (*continueWork)(void*) = nullptr, void* context = nullptr);
    Status VerifyIncomingWireBody();
    bool Owns(const TransactionToken& token) const;
    static bool SameToken(const TransactionToken& left,
                          const TransactionToken& right);
    static bool Newer(uint32_t left, uint32_t right);
    void SwapResidentAuthority();
    void ResetParser();
    void ResetWireCursor();
    Status EncodeLiveFrame();
    void ReportTiming(TimingPhase phase, uint32_t elapsedMs) const;
    Ops EditStorageOps();
    void ServiceEditPaging();

    Ops m_ops{};
    EditPagingCallback m_editPagingCallback = nullptr;
    void* m_editPagingContext = nullptr;
    bool m_editPaging = false;
    TimingObserver m_timingObserver = nullptr;
    void* m_timingContext = nullptr;
    std::array<uint8_t, WIRE_RECORD_HEADER_BYTES + WIRE_FRAME_MAX_BYTES>
        m_wireRecord{};
    std::array<uint8_t, WIRE_FRAME_MAX_BYTES> m_slotScratch{};
    std::array<uint8_t, WIRE_RECORD_HEADER_BYTES> m_recordHeader{};
    Frame m_incomingPreviousFrame{};
    Frame m_decodeCachedFrame{};
    Frame m_editFrame{};
    FixedRoot m_currentRoot{};
    FixedRoot m_alternateRoot{};
    FixedRoot m_incomingRoot{};
    // Ingress and checksum preparation are sequential storage-worker phases.
    // Their typed lifetimes share the fixed four-KiB batch allocation.
    union Workspace {
        IngressBody ingress;
        WireSummaries summaries;
        Workspace() : summaries{} {}
    };
    Workspace m_workspace{};
    bool m_ingressWorkspace = false;
    WireSummaryEdits m_summaryEdits{};
    bool m_wireSummariesReady = false;
    uint32_t m_nextNonce = 0U;
    uint32_t m_pageEpoch = 0U;
    uint32_t m_expectedBodyBytes = 0U;
    uint32_t m_expectedBodyCrc = 0U;
    uint32_t m_bodyCursor = 0U;
    uint32_t m_bodyCrcState = 0xffffffffU;
    uint32_t m_frameCrcState = 0xffffffffU;
    uint32_t m_blockCrcState = 0xffffffffU;
    uint32_t m_wireGeneration = 0U;
    uint32_t m_wireOffset = 0U;
    size_t m_wireRecordSize = 0U;
    size_t m_wireRecordRead = 0U;
    uint16_t m_recordHeaderRead = 0U;
    uint16_t m_recordLength = 0U;
    uint16_t m_recordPayloadRead = 0U;
    uint16_t m_parsedFrames = 0U;
    uint16_t m_wireNextFrame = 0U;
    uint16_t m_wireFrameIndex = FRAME_COUNT;
    uint16_t m_editIndex = FRAME_COUNT;
    uint8_t m_currentCopy = 0U;
    uint8_t m_alternateCopy = 0U;
    uint8_t m_incomingCopy = 0U;
    bool m_hasCurrent = false;
    bool m_hasAlternate = false;
    bool m_transactionActive = false;
    bool m_commitPrepared = false;
    bool m_incomingRootNeedsInvalidation = false;
    bool m_incomingRootTouched = false;
    bool m_editHasFrame = false;
    bool m_editStructural = false;
    TransactionKind m_transactionKind = TransactionKind::None;
    WireBodyMode m_wireBodyMode = WireBodyMode::Legacy;
    TransactionToken m_activeToken{};
    TransactionToken m_lastAbortedToken{};
};

static_assert(FRAME_FILES_CAPACITY == 564297U,
              "existing root, stage and frame offsets remain unchanged");
static_assert(ARTWORK_FILE_CAPACITY == 576729U,
              "four derived summary pages have an exact artwork budget");
static_assert(ARTWORK_FILE_CAPACITY + GAME_LOG_RESERVE_BYTES +
                  FILESYSTEM_SETTINGS_RECOVERY_REMAINDER_BYTES ==
              LITTLEFS_PARTITION_BYTES,
              "schema-4 artwork capacity must preserve the fixed LittleFS budget");
static_assert(FILESYSTEM_SETTINGS_RECOVERY_REMAINDER_BYTES > 320U * 1024U,
              "settings, LittleFS metadata and recovery retain over 320 KiB");

} // namespace modules::litchcraft_frame_store
