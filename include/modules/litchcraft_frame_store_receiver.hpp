#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/litchcraft_frame_store.hpp"

namespace modules::litchcraft_frame_store {

inline constexpr uint8_t STORE_BEGIN = 18U;
inline constexpr uint8_t STORE_CHUNK = 19U;
inline constexpr uint8_t STORE_CHECKPOINT = 20U;
inline constexpr uint8_t STORE_COMMIT = 21U;
inline constexpr uint8_t STORE_CANCEL = 22U;
// Compact runtime control travels on the ordinary Litchcraft characteristic.
// It never enters the Store body FIFO or changes the compressed document root.
inline constexpr uint8_t STORE_RUNTIME = 23U;
inline constexpr uint8_t STORE_SNAPSHOT = 24U;
inline constexpr uint8_t STORE_RESIDENTS = 25U;
inline constexpr uint8_t STORE_SELECT = 26U;
inline constexpr uint8_t STORE_STATE = 0x8dU;
inline constexpr uint8_t STORE_CHECKPOINT_RECEIPT = 0x8fU;
inline constexpr uint8_t STORE_SNAPSHOT_CHUNK = 0x90U;
inline constexpr uint8_t STORE_RESIDENTS_STATE = 0x92U;
inline constexpr uint8_t RESIDENT_LISTING_PROBE = 7U;
inline constexpr size_t STORE_RESPONSE_MAX_BYTES = 498U;

enum class WireStatus : uint8_t {
    Ok = 0U,
    Busy = 1U,
    StaleSession = 2U,
    Malformed = 3U,
    Unsupported = 4U,
    NoHistory = 5U,
    Incomplete = 6U,
    StaleRequest = 7U,
    BoardNotEmpty = 8U,
    RevisionConflict = 9U,
    ResourceLimit = 10U,
    IntegrityFailure = 11U,
};

struct StoreProjection final {
    uint32_t session = 0U;
    uint16_t selectedFrame = 0U;
    uint8_t topology = 3U;
    bool active = false;
    bool playing = false;
};

struct StoreIngressResult final {
    bool handled = false;
    bool responseReady = false;
    bool playbackReady = false;
    std::array<uint8_t, STORE_RESPONSE_MAX_BYTES> response{};
    uint16_t responseSize = 0U;

    void Reset() {
        handled = false;
        responseReady = false;
        playbackReady = false;
        response.fill(0U);
        responseSize = 0U;
    }
};

struct StoreReceipt final {
    StoreIngressResult response{};
    uint32_t identity = 0U;
    uint32_t session = 0U;
    uint32_t ownerEpoch = 0U;
    uint16_t transfer = 0U;
    uint8_t operation = 0U;
};

// Validates the additive schema-4 wire contract and applies it directly to the
// compressed two-root store. The caller owns packet scheduling and executes
// this receiver only on the storage worker; no packet queue or frame copy lives
// here.
class StoreReceiver final {
  public:
    // Four KiB shares ingress and derived-checksum phases within the complete
    // artwork RAM budget. Flash still writes one fixed slot per frame; radio
    // admission credit and Checkpoint durability are independent of this batch.
    static constexpr size_t WRITE_BATCH_BYTES = STORE_INGRESS_BATCH_BYTES;
    using CommitReservation = PresentationAdmission (*)(void*, uint32_t, uint32_t,
                                              const Manifest&, uint32_t,
                                              const Frame&);
    using CommitPublication = bool (*)(void*, uint32_t, uint32_t,
                                        const Manifest&, uint32_t,
                                        const Frame&);

    explicit StoreReceiver(FrameStore& store) : m_store(store) {}

    Status Recover(StoreProjection projection, FrameStore::RecoveryPurpose purpose =
        FrameStore::RecoveryPurpose::EditReady);
    // Create one blank frame only after a real Litchcraft session enters.
    Status RecoverOrCreateBlank(StoreProjection projection,
        FrameStore::RecoveryPurpose purpose = FrameStore::RecoveryPurpose::EditReady);
    void Unavailable(uint16_t request, StoreIngressResult& result) const;
    void SetProjection(StoreProjection projection);
    void EndOwner(uint32_t ownerEpoch);
    void BindLiveStream(litchcraft_frame_stream::FrameStream& stream) {
        m_liveStream = &stream;
    }
    void SetActivityStartedCallback(void (*callback)(void*), void* context) {
        m_activityStarted = callback;
        m_activityContext = context;
    }
    bool ReadActive() const { return m_readRequest != 0U; }
    bool SavedReadActive() const { return ReadActive() && !m_liveCaptured; }
    void SetSnapshotPacketMaximum(uint16_t packetMaximum);
    // One Snapshot request streams a generation-fenced suffix through the
    // existing reverse Object owner. Its worker may prepare only as many
    // responses as that owner's bounded credit window can retain.
    void ContinueRead(uint32_t ownerEpoch, StoreIngressResult& result);
    void SetCommitCallbacks(CommitReservation reservation,
                            CommitPublication publication, void* context) {
        m_commitReservation = reservation;
        m_commitPublication = publication;
        m_commitContext = context;
    }
    bool CommitPending() const { return m_commitAccepted; }
    uint32_t PendingOwnerEpoch() const { return m_ownerEpoch; }
    // Encode directly into caller-owned storage. The worker supplies an
    // unpublished response slot, so no packet-sized return value remains on
    // its stack during filesystem I/O. Input and output must not alias.
    void RetryPendingCommit(StoreIngressResult& result);
    // A non-null budget makes Chunk admission RAM-only. The worker drains
    // BodyPending before claiming another FIFO packet; Cancel may preempt it.
    void Handle(const uint8_t* bytes, size_t size, uint32_t ownerEpoch,
                StoreIngressResult& result, uint16_t* frameBudget = nullptr,
                bool cancelDeferredSelection = false);
    // Worker continuation uses the same budget as packet dispatch. Pending
    // bytes remain in the existing ingress union until this prefix is staged.
    bool BodyPending() const { return m_pendingBodyBytes != 0U && m_transferFailure == WireStatus::Ok; }
    void ContinueBody(uint16_t& frameBudget);
    // Consumed parser prefix, including an unfinished record; not a durable receipt.
    uint32_t StagedBodyCursor() const { return m_bodyCursor - static_cast<uint32_t>(m_pendingBodyBytes); }
    void RejectResidentPresentation(StoreProjection previous, uint16_t request,
                                    WireStatus status, StoreIngressResult& result);

    bool TransferActive() const { return m_transfer != 0U; }
    uint32_t BodyCursor() const { return m_bodyCursor; }
    uint16_t Transfer() const { return m_transfer; }
    bool IncomingPreviewReady() const {
        return m_store.IncomingPreviewReady();
    }
    const Manifest& IncomingManifest() const {
        return m_store.IncomingManifest();
    }
    uint32_t IncomingGeneration() const {
        return m_store.IncomingGeneration();
    }
    const Frame& IncomingFirstFrame() const {
        return m_store.IncomingFirstFrame();
    }
    // Valid until the receiver is mutated or its worker storage is released.
    const StoreReceipt& Receipt() const { return m_receipt; }
    void RestoreReceipt(const StoreReceipt& receipt) { m_receipt = receipt; }

  private:
    void State(uint16_t request, WireStatus status,
               StoreIngressResult& result) const;
    void InspectState(uint16_t request, WireStatus status,
                      StoreIngressResult& result) const;
    void CheckpointState(uint16_t request, uint32_t cursor, WireStatus status,
                         StoreIngressResult& result) const;
    void SnapshotChunk(uint16_t request, uint32_t expectedGeneration,
                       uint32_t cursor, uint16_t frameIndex,
                       StoreIngressResult& result) const;
    void Residents(uint16_t request, WireStatus status,
                   StoreIngressResult& result) const;
    WireStatus MapStatus(Status status) const;
    void RememberTerminal(uint8_t operation, uint32_t session,
                          uint16_t transfer, uint32_t ownerEpoch,
                          uint32_t identity,
                          const StoreIngressResult& response);
    Status FlushPendingBody(uint16_t& frameBudget);
    Status BufferBody(const uint8_t* bytes, size_t size);
    void Retire(bool abort);
    void RetireRead();
    void HandleLiveSnapshot(const uint8_t* bytes, uint32_t ownerEpoch,
                            StoreIngressResult& result);
    void LiveSnapshotChunk(uint16_t request, uint32_t cursor,
                           StoreIngressResult& result);
    void AdvanceRead(const StoreIngressResult& result, size_t headerBytes);

    void NotifyActivityStarted() {
        if (m_activityStarted != nullptr) m_activityStarted(m_activityContext);
    }
    void (*m_activityStarted)(void*) = nullptr;
    void* m_activityContext = nullptr;
    FrameStore& m_store;
    StoreProjection m_projection{};
    Manifest m_incomingManifest{};
    TransactionToken m_token{};
    uint32_t m_ownerEpoch = 0U;
    uint32_t m_bodyCursor = 0U;
    uint32_t m_bodyBytes = 0U;
    size_t m_pendingBodyBytes = 0U;
    uint32_t m_beginIdentity = 0U;
    uint32_t m_transferSession = 0U;
    uint16_t m_transfer = 0U;
    WireStatus m_transferFailure = WireStatus::Ok;
    CommitReservation m_commitReservation = nullptr;
    CommitPublication m_commitPublication = nullptr;
    void* m_commitContext = nullptr;
    bool m_commitAccepted = false;
    bool m_commitRootPublished = false;
    uint32_t m_commitExpectedGeneration = 0U;
    uint32_t m_commitIdentity = 0U;
    StoreReceipt m_receipt{};
    litchcraft_frame_stream::FrameStream* m_liveStream = nullptr;
    LiveRead m_liveRead{};
    bool m_liveCaptured = false;
    uint32_t m_readGeneration = 0U;
    uint32_t m_readSession = 0U;
    uint32_t m_readOwnerEpoch = 0U;
    uint32_t m_readCursor = 0U;
    uint16_t m_readRequest = 0U;
    uint16_t m_readFrameIndex = FRAME_COUNT;
    uint16_t m_snapshotResponseMaximum = STORE_RESPONSE_MAX_BYTES;
};

} // namespace modules::litchcraft_frame_store
