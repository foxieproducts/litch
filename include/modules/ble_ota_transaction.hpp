#pragma once

#include <cstddef>
#include <cstdint>

namespace modules {

#if defined(LITCH_ESP32S3) || defined(LITCH_NATIVE)
inline constexpr uint32_t BLE_OTA_SLOT_BYTES = 0x330000U;
#else
inline constexpr uint32_t BLE_OTA_SLOT_BYTES = 0x140000U;
#endif
inline constexpr size_t BLE_OTA_MAX_CHUNK_BYTES = 512U;
inline constexpr uint32_t BLE_OTA_INACTIVITY_TIMEOUT_MS = 30000U;
inline constexpr uint8_t BLE_OTA_MAX_BURST_COUNT = 48U;

enum class BleOtaRejectedBeginReason : uint8_t {
    None = 0,
    OversizedControl,
    EventQueueFull,
};

struct BleOtaRejectedBeginSignal {
    uint32_t cleanupEpoch = 0U;
    BleOtaRejectedBeginReason reason = BleOtaRejectedBeginReason::None;
};

enum class BleOtaBeginCandidateResolution : uint8_t {
    Unrelated = 0,
    Accepted,
    RejectAndInvalidate,
};

struct BleOtaControlIngressClassification {
    bool beginCandidate = false;
    bool acceptForQueue = false;
    BleOtaRejectedBeginReason rejectReason =
        BleOtaRejectedBeginReason::None;
};

bool IsBleOtaBeginControlCandidate(const char* data, size_t len);
BleOtaControlIngressClassification ClassifyBleOtaControlIngress(
    const char* data,
    size_t len,
    size_t maxControlBytes);
BleOtaBeginCandidateResolution ResolveBleOtaBeginCandidate(
    bool callbackClassifiedBegin,
    bool parsedBegin,
    bool beginAccepted);
BleOtaRejectedBeginSignal ResolveBleOtaBeginIngressRejectSignal(
    bool callbackClassifiedBegin,
    bool enqueued,
    uint32_t sessionEpoch,
    BleOtaRejectedBeginReason rejectionReason);
const char* BleOtaRejectedBeginErrorDetail(
    BleOtaRejectedBeginReason reason);
uint8_t ClampBleOtaBurstCount(uint32_t requestedBurstCount);
uint32_t ClampBleOtaWindowBytes(uint32_t requestedWindowBytes,
                                uint16_t negotiatedChunkBytes);
size_t FormatBleOtaErrorPayload(const char* detail,
                                size_t maxPayloadBytes,
                                char* out,
                                size_t outCapacity);

enum class BleOtaTransactionResult : uint8_t {
    Accepted = 0,
    NotActive,
    AlreadyActive,
    AlreadyComplete,
    InvalidSize,
    ImageTooLarge,
    InvalidNegotiatedChunk,
    EmptyChunk,
    ChunkTooLarge,
    ChunkOverNegotiated,
    ChunkOvershoot,
    StaleSession,
    MissingEndSize,
    EndSizeMismatch,
    Incomplete,
    MissingCrc,
    MalformedCrc,
    ChecksumMismatch,
    BeginFailed,
    WriteFailed,
    FlushFailed,
    EndFailed,
    TimedOut,
};

struct BleOtaTransactionOps {
    void* context = nullptr;
    bool (*begin)(void* context, uint32_t size) = nullptr;
    bool (*write)(void* context, const uint8_t* data, size_t len) = nullptr;
    bool (*flush)(void* context) = nullptr;
    bool (*end)(void* context, bool evenIfRemaining) = nullptr;
    void (*abort)(void* context) = nullptr;
};

struct BleOtaBeginRequest {
    uint32_t declaredSize = 0U;
    uint16_t negotiatedChunkBytes = 0U;
    uint32_t sessionEpoch = 0U;
    uint32_t nowMs = 0U;
    bool strictProfile = false;
};

struct BleOtaEndRequest {
    bool hasSize = false;
    uint32_t declaredSize = 0U;
    const char* crc32Hex = nullptr;
};

/**
 * \brief Allocation-free policy and backend seam for one BLE OTA transaction.
 *
 * Rejected chunks never call the backend and never mutate receive count, CRC,
 * or activity time. Abort is idempotent and is the only backend cleanup path.
 */
class BleOtaTransaction {
  public:
    BleOtaTransactionResult Begin(const BleOtaBeginRequest& request,
                                  const BleOtaTransactionOps& ops);
    BleOtaTransactionResult WriteChunk(uint32_t sessionEpoch,
                                       const uint8_t* data,
                                       size_t len,
                                       uint32_t nowMs,
                                       const BleOtaTransactionOps& ops);
    BleOtaTransactionResult ValidateEnd(const BleOtaEndRequest& request) const;
    BleOtaTransactionResult End(const BleOtaEndRequest& request,
                                const BleOtaTransactionOps& ops);
    BleOtaTransactionResult CheckTimeout(uint32_t nowMs,
                                         const BleOtaTransactionOps& ops);
    void Abort(const BleOtaTransactionOps& ops);

    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] bool IsComplete() const;
    [[nodiscard]] bool IsStrictProfile() const;
    [[nodiscard]] uint32_t ExpectedSize() const;
    [[nodiscard]] uint32_t ReceivedSize() const;
    [[nodiscard]] uint32_t SessionEpoch() const;
    [[nodiscard]] uint32_t ComputedCrc32() const;
    [[nodiscard]] uint32_t LastActivityMs() const;

  private:
    enum class State : uint8_t {
        Idle = 0,
        Active,
        Complete,
    };

    void Clear(State nextState);

    State m_state = State::Idle;
    bool m_backendActive = false;
    bool m_strictProfile = false;
    uint16_t m_negotiatedChunkBytes = 0U;
    uint32_t m_expectedSize = 0U;
    uint32_t m_receivedSize = 0U;
    uint32_t m_sessionEpoch = 0U;
    uint32_t m_lastActivityMs = 0U;
    uint32_t m_crc32 = 0U;
};

} // namespace modules
