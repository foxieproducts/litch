#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "modules/connection_session_codec.hpp"

namespace modules {

enum class BleOtaChunkQueueFailure : uint8_t {
    None = 0,
    Empty,
    TooLarge,
    Full,
};

class BleOtaChunkQueue {
  public:
    enum class Admission : uint8_t { Accepted, DeferredCapacity, Invalid };
    // Shared artwork/OTA byte arena. Object grants are bounded by both this
    // arena and the callback FIFO, including maximum reassembled records.
    static constexpr uint8_t CAPACITY = 64;
    static constexpr uint16_t MAX_CHUNK_BYTES = 512;
    // Preserve the established byte budget. These 244-byte units size the arena,
    // not the negotiated packet limit; each record supports up to 512 bytes.
    static constexpr size_t RESERVED_OBJECT_BYTES = 244U;
    // Trade byte-arena slack for one token per descriptor and one returned
    // token/count pair. This keeps the existing worker/transport RAM budget.
    static constexpr size_t CREDIT_STORAGE_BYTES =
        (size_t(CAPACITY) + 2U) * sizeof(uint32_t);
    static constexpr size_t BYTE_CAPACITY =
        size_t(CAPACITY) * RESERVED_OBJECT_BYTES + MAX_CHUNK_BYTES - CREDIT_STORAGE_BYTES;
    // Payload credit may use this amount regardless of the ring's current
    // head/tail positions. The final MAX_CHUNK_BYTES are wrap padding, not
    // sender-visible receive credit.
    static constexpr size_t GUARANTEED_PAYLOAD_BYTES =
        BYTE_CAPACITY - MAX_CHUNK_BYTES;

    bool Begin();
    uint32_t AdvanceEpoch();
    bool AdvanceEpochIfCurrent(uint32_t expectedEpoch);
    uint32_t CurrentEpoch() const;
    bool Enqueue(const uint8_t* data, size_t len, uint32_t ingressOrdinal);
    // A leased Object remains owned upstream while capacity is unavailable.
    // Deferral must not latch the legacy raw-ingress fatal Full signal.
    Admission EnqueueReserved(const uint8_t* data, size_t len,
                              uint32_t ingressOrdinal, uint32_t objectCreditToken = 0U);
    // Sole cooperative consumer returns these through ConnectionSession after
    // releasing this queue's lock. A batch contains at most CAPACITY credits.
    bool TakeReturnedObjectCredits(uint32_t& token, uint32_t& count);
    bool TryPeekOrdinal(uint32_t& outIngressOrdinal) const;
    bool TryPeekFirstLitchcraftBarrierOrdinal(
        uint32_t& outIngressOrdinal) const;
    bool TryPeekLitchcraftFrameStore() const;
    bool TryAcquireReadSlot(const uint8_t*& outData,
                            size_t& outLen,
                            uint32_t& outIngressOrdinal,
                            uint32_t& outSessionEpoch);
    bool TryAcquireReadSlot(const uint8_t*& outData,
                            size_t& outLen,
                            uint32_t& outIngressOrdinal) {
        uint32_t ignoredSessionEpoch = 0U;
        return TryAcquireReadSlot(
            outData, outLen, outIngressOrdinal, ignoredSessionEpoch);
    }
    void ReleaseReadSlot();
    // Relinquish this read lease while retaining the exact FIFO head and credit.
    void DeferReadSlot();
    bool TryConsumeFailure(uint32_t sessionEpoch,
                           BleOtaChunkQueueFailure& outFailure);
    uint8_t HighWaterMark() const;
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
    struct LiveIngressDiagnostics {
        uint16_t bytes;
        uint8_t count;
        bool readClaimed;
        uint32_t headOrdinal = 0U;
        uint8_t headOperation = 0U;
    };
    LiveIngressDiagnostics GetLiveIngressDiagnostics() const;
#endif

  private:
    struct Slot {
        uint32_t ingressOrdinal = 0U;
        uint32_t sessionEpoch = 0U;
        uint32_t objectCreditToken = 0U;
        uint16_t offset = 0U;
        uint16_t len = 0;
    };
    static_assert(sizeof(Slot) == 4U * sizeof(uint32_t),
                  "slot credit accounting must match the byte-arena trade");

    void RecordFailure(BleOtaChunkQueueFailure failure);
    Admission EnqueueInternal(const uint8_t* data, size_t len,
                              uint32_t ingressOrdinal, bool recordFailure,
                              uint32_t objectCreditToken);
    void RecordReturnedObjectCredit(uint32_t token);

    Slot m_slots[CAPACITY] = {};
    std::array<uint8_t, BYTE_CAPACITY> m_bytes{};
    mutable std::atomic_flag m_lock = ATOMIC_FLAG_INIT;
    uint8_t m_head = 0;
    uint8_t m_tail = 0;
    uint8_t m_count = 0;
    uint8_t m_highWaterMark = 0;
    uint8_t m_readIndex = 0;
    bool m_readClaimed = false;
    uint16_t m_byteHead = 0U;
    uint16_t m_byteTail = 0U;
    uint16_t m_byteUsed = 0U;
    uint32_t m_currentEpoch = 1U;
    uint32_t m_failureEpoch = 0U;
    uint32_t m_returnedObjectToken = 0U, m_returnedObjectCount = 0U;
    BleOtaChunkQueueFailure m_failure = BleOtaChunkQueueFailure::None;
};

static_assert(sizeof(BleOtaChunkQueue) <= 18U * 1024U,
              "BLE bulk ingress must not consume the NimBLE heap reserve");

} // namespace modules
