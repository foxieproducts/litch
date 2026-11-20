#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "modules/litchcraft_frame_store.hpp"
#include "modules/litchcraft_store_history.hpp"

namespace modules::litchcraft_frame_stream {

// Render owns presentation; the storage worker owns Service(). A frame is
// decoded completely into the non-active slot before one atomic publication.
// A short publication lock covers payload copies so the worker can recycle the
// old slot without a C++ data race or exposing a partial frame.
class FrameStream final {
  public:
    static constexpr uint32_t EDIT_SAVE_QUIET_MS = 5000U;
    static constexpr uint8_t CACHE_SLOTS = 3U;
    enum class GuardedEditResult : uint8_t {
        Conflict,
        Unchanged,
        Changed,
    };
    enum class CanvasAction : uint8_t {
        Fill = 0U,
        Erase = 1U,
        Clockwise = 2U,
        HalfTurn = 3U,
        Counterclockwise = 4U,
        LightFill = 5U,
        DarkFill = 6U,
        LightErase = 7U,
        DarkErase = 8U,
    };
    struct CanvasEdit final {
        uint32_t generation = 0U;
        uint32_t revision = 0U;
        uint16_t frame = 0U;
        uint8_t mask = 15U;
        Rgb color{};
        CanvasAction action = CanvasAction::Fill;
    };
    enum class FrameEditAction : uint8_t { AddCopy = 0U, Delete = 1U, Duration = 3U };
    struct FrameEdit final {
        uint32_t generation = 0U;
        uint32_t revision = 0U;
        uint16_t source = 0U;
        uint16_t durationMs = 0U;
        FrameEditAction action = FrameEditAction::AddCopy;
    };
    enum class StructuralResult : uint8_t { None, Pending, Changed, Conflict, IoFailure };
    struct LiveAuthority final {
        uint32_t generation = 0U;
        uint32_t revision = 0U;
        bool dirty = false;
    };
    enum class ServiceResult : uint8_t {
        Idle = 0U,
        DeferredForControl,
        Loading,
        WaitingForPresentation,
        LoadedRequested,
        PrefetchedNext,
        Corrupt,
        IoFailure,
    };

    // The storage owner resolves this boot-only fact after publishing the
    // initial manifest, or after a terminal absent/unavailable recovery result.
    void BeginInitialRecovery() { m_initialRecoveryPending.store(true, std::memory_order_release); }
    void CompleteInitialRecovery() { m_initialRecoveryPending.store(false, std::memory_order_release); }
    bool InitialRecoveryPending() const { return m_initialRecoveryPending.load(std::memory_order_acquire); }

    bool Configure(const litchcraft_frame_store::Manifest& manifest,
                   uint32_t generation, uint16_t firstFrame = 0U);
    // Publish a root-only mode change under the live edit publication lock.
    litchcraft_frame_store::Status SaveModeRoot(
        litchcraft_frame_store::FrameStore& store, bool active);
    void Clear();
    // Cooperative document adoption revokes only published authority. Worker
    // cursors/cache remain worker-owned; old roots cannot republish this boot.
    enum class RetirementScope : uint8_t { CurrentDocument, EmptyOnly };
    bool RetireDocument(RetirementScope scope = RetirementScope::CurrentDocument);
    void BindRetirementGeneration(uint32_t generation);
    bool AdvanceDurableRetirementGeneration(uint32_t generation);
    bool NeedsDurableRetirement() const {
        return m_retirementState.load(std::memory_order_acquire) ==
                RetirementState::AwaitingRoot ||
            RetiredGeneration() != 0U;
    }
    uint32_t RetiredGeneration() const {
        return m_retiredGeneration.load(std::memory_order_acquire);
    }
    bool StageFirstFrame(const litchcraft_frame_store::Manifest& manifest,
                         uint32_t generation,
                         const litchcraft_frame_store::Frame& firstFrame) {
        return TryStageFirstFrame(manifest, generation, firstFrame).kind ==
            litchcraft_frame_store::PresentationAdmission::Kind::Accepted;
    }
    litchcraft_frame_store::PresentationAdmission TryStageFirstFrame(
        const litchcraft_frame_store::Manifest& manifest, uint32_t generation,
        const litchcraft_frame_store::Frame& firstFrame) {
        return AdoptDocument(manifest, generation, 0U, &firstFrame);
    }
    // Reserve the old live authority before root I/O. Paint accepted before
    // reservation conflicts; mutations during publication cannot be lost.
    bool ReserveReplacement(uint32_t expectedCurrentGeneration,
                            const litchcraft_frame_store::Manifest& manifest,
                            uint32_t generation) {
        return TryReserveReplacement(expectedCurrentGeneration, manifest, generation).kind ==
            litchcraft_frame_store::PresentationAdmission::Kind::Accepted;
    }
    litchcraft_frame_store::PresentationAdmission TryReserveReplacement(
        uint32_t expectedCurrentGeneration,
        const litchcraft_frame_store::Manifest& manifest, uint32_t generation);
    // Called only after the new root is durable. The reservation keeps this
    // first-frame publication valid across a root-write retry.
    bool PublishReservedReplacement(
        uint32_t expectedCurrentGeneration,
        const litchcraft_frame_store::Manifest& manifest,
        uint32_t generation,
        const litchcraft_frame_store::Frame& firstFrame);
    bool ReplacementPending() const;
    bool Request(uint16_t frameIndex);
    struct PresentationToken final {
        uint32_t documentEpoch = 0U;
        uint32_t contextEpoch = 0U;
        uint32_t frameIdentity = 0U;
        uint32_t loadSequence = 0U;
        uint32_t editSequence = 0U;
        uint32_t lease = 0U;
        uint16_t frameIndex = 0U;
        uint8_t slot = CACHE_SLOTS;
    };
    // The independent output owner enables this mode before accepting input.
    // Prepared pixels never become Displayed until their exact Show completes.
    void SetConfirmedPresentation(bool enabled);
    // Caller names requested or last-confirmed fallback in token.frameIndex.
    bool PreparePresentation(litchcraft_frame_store::Frame& out,
        uint32_t contextEpoch, PresentationToken& token);
    bool ConfirmPresented(const PresentationToken& token);
    bool CancelPresentation(const PresentationToken& token);
    void RetirePresentationContext();
    bool HasPresentationCandidate() const {
        return m_presentationSlot.load(std::memory_order_acquire) < CACHE_SLOTS;
    }
    bool PresentationReady() const;
    // Main publishes semantic visual freshness before ending its mutation
    // lease. Routine captures/feedback echoes keep this stamp unchanged.
    bool PublishPresentationContext(uint32_t presentationEpoch);
    bool IsPresentationContextCurrent(uint32_t presentationEpoch) const;
    bool RequestPresentation(const PresentationToken& token, uint16_t frameIndex);
    // Control admission also permits Enter/Exit before the first frame is ready.
    uint32_t TryBeginPresentationControl();
    uint32_t TryBeginPresentationMutation();
    bool EndPresentationMutation(uint32_t lease);
    bool TryPresentRequested();
    bool CopyDisplayed(litchcraft_frame_store::Frame& out,
                       uint16_t& outIndex) const;
    bool CopyDisplayedSquare(
        uint8_t square,
        std::array<litchcraft_frame_store::Emitter, 4U>& out,
        uint16_t& outIndex, uint16_t& outDurationMs) const;
    bool RestoreDisplayedSquare(
        uint32_t expectedDocumentEpoch, uint16_t frameIndex, uint8_t square,
        const std::array<litchcraft_frame_store::Emitter, 4U>& emitters);
    bool PaintDisplayed(uint16_t frameIndex, uint8_t square, uint8_t mask,
                        Rgb color, bool clearEffects);
    GuardedEditResult PaintDisplayedIfRevision(
        uint32_t expectedRevision, uint16_t frameIndex, uint8_t square,
        uint8_t mask, Rgb color, bool clearEffects);
    // On Changed or Unchanged, capture the packet's resulting revision while
    // holding the publication lock; a later physical edit cannot enter it.
    GuardedEditResult PaintDisplayedBatchIfRevision(
        uint32_t expectedRevision, uint16_t frameIndex,
        const uint8_t* squares, size_t squareCount, uint8_t mask,
        Rgb color, bool clearEffects, uint32_t* resultingRevision = nullptr);
    // Canvas edits journal and publish one complete operation under the same
    // lock, reusing the existing selected-frame preimage without frame scratch.
    GuardedEditResult EditDisplayedCanvasIfRevision(
        const CanvasEdit& edit, uint32_t* resultingRevision = nullptr);
    GuardedEditResult EditDisplayedDurationIfRevision(const FrameEdit& edit,
        uint32_t* resultingRevision = nullptr);
    // Known one-frame New canvas publishes live authority without storage I/O.
    // The existing worker publishes its root before saving later brush edits.
    GuardedEditResult NewBlankDocument(uint32_t expectedGeneration,
        uint32_t expectedRevision, const std::array<Rgb, 8U>& favorites,
        uint32_t* resultingRevision = nullptr);
    bool HasPendingBlankDocument() const {
        return m_blankDocumentPending.load(std::memory_order_acquire);
    }
    StructuralResult QueueStructuralEdit(const FrameEdit& edit);
    StructuralResult QueueStructuralHistory(uint32_t expectedRevision, bool redo);
    StructuralResult TakeStructuralResult(uint16_t& selected, uint32_t& revision);
    void CancelStructuralEdit();
    bool HasPendingStructuralEdit() const;
    bool HasPendingStructuralSave() const {
        return m_persistCount.load(std::memory_order_acquire) != 0U;
    }
    bool HistoryIsStructural(bool redo) const;
    bool SteppedNeedsRepair(uint32_t expectedRevision) const;
    GuardedEditResult ClearDisplayedIfRevision(uint32_t expectedRevision,
                                              uint16_t frameIndex);
    bool ApplyDisplayedEffect(uint16_t frameIndex, uint8_t square,
                              uint8_t mask, uint8_t effect,
                              uint16_t durationMs);
    GuardedEditResult ApplyDisplayedEffectIfRevision(
        uint32_t expectedRevision, uint16_t frameIndex, uint8_t square,
        uint8_t mask, uint8_t effect, uint16_t durationMs);
    GuardedEditResult ApplyDisplayedEffectBatchIfRevision(
        uint32_t expectedRevision, uint16_t frameIndex,
        const uint8_t* squares, size_t squareCount, uint8_t mask,
        uint8_t effect, uint16_t durationMs,
        uint32_t* resultingRevision = nullptr);
    bool BeginHistoryStroke(uint16_t frameIndex);
    void EndHistoryStroke();
    GuardedEditResult StepHistoryIfRevision(uint32_t expectedRevision,
                                           uint16_t frameIndex, bool redo);
    uint16_t HistoryTargetFrame(bool redo) const;
    size_t SteppedEmitterCount(uint32_t expectedRevision) const;
    bool CopySteppedEmitter(uint32_t expectedRevision, size_t ordinal,
                            uint8_t& index,
                            litchcraft_frame_store::Emitter& emitter) const;
    uint8_t UndoCount() const;
    uint8_t RedoCount() const;
    enum class ServiceMode : uint8_t { PagingAndEdits, PagingOnly };
    enum class PrefetchPolicy : uint8_t { None, Next, TwoAhead };
    ServiceResult Service(litchcraft_frame_store::FrameStore& store,
                          bool controlPending,
                          ServiceMode mode = ServiceMode::PagingAndEdits,
                          PrefetchPolicy prefetch = PrefetchPolicy::Next);
    bool RequestedFrameCorrupt() const;
    bool RequestedFrameFailed() const;
    bool HasUnpersistedEdit() const {
        if (HasPendingBlankDocument()) return true;
        if (m_persistCount.load(std::memory_order_acquire) != 0U) return true;
        for (uint8_t slot = 0U; slot < m_frames.size(); ++slot) {
            if (SlotDirty(slot)) return true;
        }
        return false;
    }
    bool EditSaveReady(uint32_t nowMs) const;
    void RequestImmediateSave() {
        if (HasUnpersistedEdit()) {
            m_saveImmediately.store(true, std::memory_order_release);
        }
    }
    uint32_t LiveRevision() const {
        LockPublication();
        const uint32_t revision =
            m_liveRevision.load(std::memory_order_relaxed);
        UnlockPublication();
        return revision;
    }
    LiveAuthority ReadLiveAuthority() const;
    enum class LiveFrameResult : uint8_t { Cached, Durable, Conflict };
    bool CaptureLiveSnapshot(uint16_t selector,
        litchcraft_frame_store::LiveSnapshot& snapshot) const;
    bool MatchesLiveSnapshot(
        const litchcraft_frame_store::LiveSnapshot& snapshot) const;
    LiveFrameResult CopyLiveFrame(
        const litchcraft_frame_store::LiveSnapshot& snapshot,
        litchcraft_frame_store::Frame& out) const;
    LiveFrameResult CopyDirtyFrame(
        const litchcraft_frame_store::LiveSnapshot& snapshot, uint8_t slot,
        litchcraft_frame_store::Frame& out, uint16_t& index) const;
    uint16_t LiveDurableIndex(const litchcraft_frame_store::LiveSnapshot& snapshot,
                             uint16_t index) const;

    uint16_t RequestedIndex() const {
        return m_requested.load(std::memory_order_acquire);
    }
    uint16_t DisplayedIndex() const;
    uint32_t DisplayedFingerprint() const;
    uint32_t PresentationCount() const;
    uint16_t FrameCount() const {
        return Generation() == 0U ? 0U : m_frameCount.load(std::memory_order_acquire);
    }
    uint32_t Generation() const {
        if (m_retirementState.load(std::memory_order_acquire) ==
            RetirementState::AwaitingRoot) return 0U;
        const uint32_t generation = m_authorityGeneration.load(std::memory_order_acquire);
        const uint32_t retired = RetiredGeneration();
        return retired != 0U && static_cast<int32_t>(generation - retired) <= 0
            ? 0U : generation;
    }
    uint8_t Topology() const {
        return m_topology.load(std::memory_order_acquire);
    }
    uint32_t SparkleSeed() const {
        return m_sparkleSeed.load(std::memory_order_acquire);
    }
    bool Active() const {
        return m_documentActive.load(std::memory_order_acquire);
    }
    bool PlaybackOnEntry() const {
        return m_playbackOnEntry.load(std::memory_order_acquire);
    }
    uint32_t DocumentEpoch() const {
        return m_documentEpoch.load(std::memory_order_acquire);
    }
    bool AcceptsGeneration(uint32_t generation) const;

  private:
    enum class RetirementState : uint8_t { None, AwaitingRoot, Bound };
    static constexpr uint8_t NO_SLOT = CACHE_SLOTS;
    static constexpr uint16_t NO_FRAME = 0xffffU;

    bool PublishLoaded(uint8_t slot, uint16_t frameIndex, uint32_t generation,
                       const litchcraft_frame_store::Frame& frame, uint32_t identity);
    ServiceResult ServiceBlankDocument(litchcraft_frame_store::FrameStore& store);
    ServiceResult ServiceStructural(litchcraft_frame_store::FrameStore& store);
    ServiceResult ServiceStructuralSave(litchcraft_frame_store::FrameStore& store);
    bool ApplyStructuralLocked();
    void ResetFrameIdentities(uint16_t count);
    uint16_t FindLiveIdentity(uint32_t identity) const;
    uint16_t DurableIndexLocked(uint32_t identity) const;
    uint8_t CachedIdentity(uint32_t identity) const;
    uint8_t FreeStructuralSlot(uint8_t protectedSlot = NO_SLOT) const;
    bool StructuralPayloadPinned(uint32_t identity) const;
    void ReindexCachedFrames();
    ServiceResult ServiceEdits(litchcraft_frame_store::FrameStore& store,
                               uint32_t generation);
    ServiceResult ServicePage(litchcraft_frame_store::FrameStore& store,
                              uint32_t generation,
                              PrefetchPolicy prefetch = PrefetchPolicy::Next);
    litchcraft_frame_store::PresentationAdmission AdoptDocument(const litchcraft_frame_store::Manifest& manifest,
                       uint32_t generation, uint16_t firstFrameIndex,
                       const litchcraft_frame_store::Frame* preparedFirstFrame,
                       std::optional<uint32_t> expectedCurrentGeneration =
                           std::nullopt);
    GuardedEditResult PaintDisplayedImpl(
        std::optional<uint32_t> expectedRevision, uint16_t frameIndex,
        const uint8_t* squares, size_t squareCount, uint8_t mask,
        Rgb color, bool clearEffects, uint32_t* resultingRevision = nullptr);
    GuardedEditResult ApplyDisplayedEffectImpl(
        std::optional<uint32_t> expectedRevision, uint16_t frameIndex,
        const uint8_t* squares, size_t squareCount, uint8_t mask,
        uint8_t effect, uint16_t durationMs,
        uint32_t* resultingRevision = nullptr);
    uint16_t NextIndex(uint16_t frameIndex) const;
    bool SlotDirty(uint8_t slot) const;
    void AdvanceLiveRevision(litchcraft_frame_store::MutationScope scope =
        litchcraft_frame_store::MutationScope::Unavailable,
        uint16_t index = 0U, uint16_t aux = 0U);
    bool MatchesLiveSnapshotLocked(
        const litchcraft_frame_store::LiveSnapshot& snapshot) const;
    void RecordEdit(uint8_t slot);
    void LockPublication() const;
    void UnlockPublication() const;
    bool PresentationReadyLocked() const;
    bool MatchesPresentationLocked(const PresentationToken& token) const;
    void RetirePresentationLocked();

    std::array<litchcraft_frame_store::Frame, CACHE_SLOTS> m_frames{};
    std::atomic<uint8_t> m_presentationSlot{NO_SLOT};
    bool m_confirmedPresentation = false; // Protected by publication lock.
    uint16_t m_presentationOriginalRequest = NO_FRAME;
    uint32_t m_presentationLease = 0U;
    uint32_t m_presentationContext = 0U;
    uint32_t m_publishedPresentationContext = 0U;
    uint32_t m_confirmedDocumentEpoch = 0U;
    uint32_t m_confirmedPresentationCount = 0U;
    uint32_t m_presentationMutationLease = 0U;
    uint32_t m_nextPresentationMutationLease = 0U;
    std::array<std::atomic<uint32_t>, CACHE_SLOTS> m_sequences{};
    std::array<std::atomic<uint16_t>, CACHE_SLOTS> m_indices{
        std::atomic<uint16_t>{NO_FRAME}, std::atomic<uint16_t>{NO_FRAME},
        std::atomic<uint16_t>{NO_FRAME}};
    std::array<std::atomic<uint32_t>, CACHE_SLOTS> m_generations{};
    std::array<uint32_t, litchcraft_frame_store::FRAME_COUNT> m_liveIdentities{};
    std::array<uint32_t, litchcraft_frame_store::FRAME_COUNT> m_durableIdentities{};
    std::array<uint32_t, CACHE_SLOTS> m_cacheIdentities{};
    uint32_t m_nextFrameIdentity = 1U;
    uint16_t m_durableCount = 0U;
    std::atomic<uint16_t> m_frameCount{0U};
    std::atomic<uint16_t> m_requested{NO_FRAME};
    std::atomic<uint16_t> m_failedIndex{NO_FRAME};
    std::atomic<uint32_t> m_failedGeneration{0U};
    std::atomic<uint8_t> m_failedStatus{
        static_cast<uint8_t>(litchcraft_frame_store::Status::Ok)};
    std::atomic<uint32_t> m_authorityGeneration{0U};
    std::atomic<uint32_t> m_documentBaseGeneration{0U};
    std::atomic<uint32_t> m_documentEpoch{0U};
    std::atomic<uint32_t> m_liveRevision{0U};
    bool m_liveRevisionExhausted = false;
    std::atomic<bool> m_initialRecoveryPending{false};
    uint32_t m_scopeBaseRevision = 0U;
    uint16_t m_scopeIndex = 0U;
    uint16_t m_scopeAux = 0U;
    litchcraft_frame_store::MutationScope m_scope =
        litchcraft_frame_store::MutationScope::Unavailable;
    std::atomic<uint32_t> m_editSequence{0U};
    std::array<std::atomic<uint32_t>, CACHE_SLOTS> m_slotEditSequence{};
    std::array<std::atomic<uint32_t>, CACHE_SLOTS> m_slotPersistedSequence{};
    std::atomic<uint32_t> m_lastEditAtMs{0U};
    std::atomic<bool> m_saveImmediately{false};
    std::atomic<bool> m_blankDocumentPending{false};
    uint32_t m_blankInitialSequence = 0U; // Protected by publication lock.
    std::array<Rgb, 8U> m_blankFavorites{};
    std::atomic<uint32_t> m_retiredGeneration{0U};
    std::atomic<RetirementState> m_retirementState{RetirementState::None};
    std::atomic<uint32_t> m_sparkleSeed{1U};
    std::atomic<uint8_t> m_topology{3U};
    std::atomic<bool> m_documentActive{false};
    std::atomic<bool> m_playbackOnEntry{false};
    std::atomic<uint8_t> m_activeSlot{NO_SLOT};
    std::atomic<uint32_t> m_presentationCount{0U};
    mutable std::atomic_flag m_publicationLock = ATOMIC_FLAG_INIT;
    bool m_replacementReserved = false; // Protected by m_publicationLock.
    uint32_t m_reservedPreviousGeneration = 0U;
    uint32_t m_reservedGeneration = 0U;
    // Only unaccepted cold/cache-pressure preparation occupies the domain
    // result slot. Accepted live edits persist independently below.
    struct StructuralEdit final {
        uint32_t revision = 0U;
        uint32_t documentEpoch = 0U;
        uint16_t source = 0U;
        uint16_t selected = 0U;
        uint16_t requested = 0U;
        uint8_t action = 0U;
        StructuralResult result = StructuralResult::None;
        bool history = false;
        bool redo = false;
        bool running = false;
        bool cancelled = false;
    };
    StructuralEdit m_structuralEdit{};
    // These are accepted live edits awaiting the existing storage worker,
    // never another transport queue. Identity survives logical index shifts.
    static constexpr uint8_t PERSIST_CAPACITY = 16U;
    struct StructuralSave final {
        uint32_t identity = 0U;
        uint32_t sourceIdentity = 0U;
        uint32_t documentEpoch = 0U;
        uint32_t previousGeneration = 0U;
        uint32_t committedGeneration = 0U;
        uint32_t previousCrc = 0U;
        uint32_t sequence = 0U;
        uint8_t action = 0U;
        uint8_t resident = 0U;
        uint8_t slot = NO_SLOT;
        uint8_t flags = 0U;
        static constexpr uint8_t PAYLOAD = 1U;
        static constexpr uint8_t UNCERTAIN = 2U;
        bool HasPayload() const { return (flags & PAYLOAD) != 0U; }
        bool IsUncertain() const { return (flags & UNCERTAIN) != 0U; }
        void SetPayload(bool value) {
            flags = value ? flags | PAYLOAD : flags & static_cast<uint8_t>(~PAYLOAD);
        }
        void SetUncertain(bool value) {
            flags = value ? flags | UNCERTAIN : flags & static_cast<uint8_t>(~UNCERTAIN);
        }
    };
    static_assert(sizeof(StructuralSave) == 32U,
                  "sixteen accepted layout saves keep fixed 32-byte metadata records");
    std::array<StructuralSave, PERSIST_CAPACITY> m_structuralSaves{};
    std::atomic<uint8_t> m_persistCount{0U};
    uint8_t m_persistHead = 0U;
    litchcraft_store_history::StoreHistory m_history{};
    uint16_t m_historyFrame = NO_FRAME;
    litchcraft_frame_store::FrameStore::ReadCursor m_readCursor{};
    uint16_t m_loadingIndex = NO_FRAME;
    uint32_t m_loadingGeneration = 0U;
    uint32_t m_loadingIdentity = 0U;
};

static_assert(sizeof(FrameStream) < 13000U,
              "three-frame COW cache, identity maps and bounded history must fit RAM");

} // namespace modules::litchcraft_frame_stream
