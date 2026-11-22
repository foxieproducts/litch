#include "modules/ble_ota_transaction.hpp"

#include "modules/ble_ota_chunk_queue.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>

namespace modules {
namespace {

constexpr uint32_t kCrc32Initial = 0xFFFFFFFFU;
constexpr uint32_t kCrc32FinalXor = 0xFFFFFFFFU;
constexpr size_t kErrorJsonOverheadBytes = 8U;
constexpr char kFallbackCompactError[] = "error";

struct BleOtaCompactErrorAlias {
    std::string_view canonical;
    std::string_view compact;
};

constexpr BleOtaCompactErrorAlias kCompactErrorAliases[] = {
    {"invalid_begin", "inv_begin"},
    {"invalid_test_args", "test_args"},
    {"notify_test_failed", "notify_fail"},
    {"reboot_pending", "reboot_wait"},
    {"image_too_large", "image_large"},
    {"not_in_progress", "idle_ota"},
    {"size_overshoot", "overshoot"},
    {"size_mismatch", "wrong_size"},
    {"checksum_mismatch", "crc_mismatch"},
    {"invalid_chunk", "bad_chunk"},
    {"chunk_too_large", "large_chunk"},
};

static_assert(sizeof("inv_begin") - 1U <= 12U);
static_assert(sizeof("test_args") - 1U <= 12U);
static_assert(sizeof("notify_fail") - 1U <= 12U);
static_assert(sizeof("reboot_wait") - 1U <= 12U);
static_assert(sizeof("image_large") - 1U <= 12U);
static_assert(sizeof("idle_ota") - 1U <= 12U);
static_assert(sizeof("overshoot") - 1U <= 12U);
static_assert(sizeof("wrong_size") - 1U <= 12U);
static_assert(sizeof("crc_mismatch") - 1U <= 12U);
static_assert(sizeof("bad_chunk") - 1U <= 12U);
static_assert(sizeof("large_chunk") - 1U <= 12U);

uint32_t UpdateCrc32(uint32_t currentCrc,
                     const uint8_t* const data,
                     const size_t len) {
    uint32_t crc = currentCrc;
    for (size_t byteIndex = 0U; byteIndex < len; ++byteIndex) {
        crc ^= static_cast<uint32_t>(data[byteIndex]);
        for (uint8_t bitIndex = 0U; bitIndex < 8U; ++bitIndex) {
            const bool lsbSet = (crc & 1U) != 0U;
            crc >>= 1U;
            if (lsbSet) {
                crc ^= 0xEDB88320U;
            }
        }
    }
    return crc;
}

bool TryParseExactCrc32(const char* const rawHex, uint32_t& outValue) {
    if (rawHex == nullptr || std::strlen(rawHex) != 8U) {
        return false;
    }

    uint32_t parsed = 0U;
    for (size_t index = 0U; index < 8U; ++index) {
        const char digit = rawHex[index];
        uint8_t nibble = 0U;
        if (digit >= '0' && digit <= '9') {
            nibble = static_cast<uint8_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            nibble = static_cast<uint8_t>(digit - 'a' + 10);
        } else if (digit >= 'A' && digit <= 'F') {
            nibble = static_cast<uint8_t>(digit - 'A' + 10);
        } else {
            return false;
        }
        parsed = static_cast<uint32_t>((parsed << 4U) | nibble);
    }
    outValue = parsed;
    return true;
}

std::string_view CompactBleOtaErrorAlias(
    const std::string_view canonical) {
    for (const BleOtaCompactErrorAlias& alias : kCompactErrorAliases) {
        if (alias.canonical == canonical) {
            return alias.compact;
        }
    }

    return std::string_view(kFallbackCompactError,
                            sizeof(kFallbackCompactError) - 1U);
}

} // namespace

bool IsBleOtaBeginControlCandidate(const char* const data,
                                   const size_t len) {
    if (data == nullptr || len == 0U) {
        return false;
    }

    const std::string_view packet(data, len);
    const size_t commandKey = packet.find("\"cmd\"");
    if (commandKey == std::string_view::npos) {
        return false;
    }
    size_t cursor = commandKey + 5U;
    while (cursor < packet.size() &&
           (packet[cursor] == ' ' || packet[cursor] == '\t' ||
            packet[cursor] == '\r' || packet[cursor] == '\n')) {
        ++cursor;
    }
    if (cursor >= packet.size() || packet[cursor] != ':') {
        return false;
    }
    ++cursor;
    while (cursor < packet.size() &&
           (packet[cursor] == ' ' || packet[cursor] == '\t' ||
            packet[cursor] == '\r' || packet[cursor] == '\n')) {
        ++cursor;
    }

    const std::string_view value = packet.substr(cursor);
    return value.compare(0U, 11U, "\"ota_begin\"") == 0 ||
           value.compare(0U, 15U, "\"ota_sim_begin\"") == 0;
}

BleOtaControlIngressClassification ClassifyBleOtaControlIngress(
    const char* const data,
    const size_t len,
    const size_t maxControlBytes) {
    const bool beginCandidate =
        IsBleOtaBeginControlCandidate(data, len);
    return BleOtaControlIngressClassification{
        beginCandidate,
        len <= maxControlBytes,
        (len <= maxControlBytes)
            ? BleOtaRejectedBeginReason::None
            : BleOtaRejectedBeginReason::OversizedControl,
    };
}

BleOtaBeginCandidateResolution ResolveBleOtaBeginCandidate(
    const bool callbackClassifiedBegin,
    const bool parsedBegin,
    const bool beginAccepted) {
    if (!callbackClassifiedBegin && !parsedBegin) {
        return BleOtaBeginCandidateResolution::Unrelated;
    }
    if (callbackClassifiedBegin && parsedBegin && beginAccepted) {
        return BleOtaBeginCandidateResolution::Accepted;
    }
    return BleOtaBeginCandidateResolution::RejectAndInvalidate;
}

BleOtaRejectedBeginSignal ResolveBleOtaBeginIngressRejectSignal(
    const bool callbackClassifiedBegin,
    const bool enqueued,
    const uint32_t sessionEpoch,
    const BleOtaRejectedBeginReason rejectionReason) {
    if (!callbackClassifiedBegin || enqueued || sessionEpoch == 0U) {
        return BleOtaRejectedBeginSignal{};
    }

    return BleOtaRejectedBeginSignal{
        sessionEpoch,
        rejectionReason,
    };
}

const char* BleOtaRejectedBeginErrorDetail(
    const BleOtaRejectedBeginReason reason) {
    switch (reason) {
        case BleOtaRejectedBeginReason::OversizedControl:
            return "invalid_begin";
        case BleOtaRejectedBeginReason::EventQueueFull:
            return "queue_full";
        case BleOtaRejectedBeginReason::None:
            break;
    }

    return "invalid_begin";
}

uint8_t ClampBleOtaBurstCount(const uint32_t requestedBurstCount) {
    if (requestedBurstCount == 0U) {
        return 1U;
    }
    if (requestedBurstCount > BLE_OTA_MAX_BURST_COUNT) {
        return BLE_OTA_MAX_BURST_COUNT;
    }
    return static_cast<uint8_t>(requestedBurstCount);
}

uint32_t ClampBleOtaWindowBytes(const uint32_t requestedWindowBytes,
                                const uint16_t negotiatedChunkBytes) {
    if (negotiatedChunkBytes == 0U ||
        negotiatedChunkBytes > BLE_OTA_MAX_CHUNK_BYTES) {
        return 0U;
    }
    const uint32_t burstCapacityBytes =
        static_cast<uint32_t>(negotiatedChunkBytes) *
        static_cast<uint32_t>(BLE_OTA_MAX_BURST_COUNT);
    const uint32_t capacityBytes =
        (burstCapacityBytes < BleOtaChunkQueue::GUARANTEED_PAYLOAD_BYTES)
            ? burstCapacityBytes
            : static_cast<uint32_t>(
                  BleOtaChunkQueue::GUARANTEED_PAYLOAD_BYTES);
    const uint32_t minimumWindowBytes =
        (capacityBytes < 1024U) ? capacityBytes : 1024U;
    if (requestedWindowBytes < minimumWindowBytes) {
        return minimumWindowBytes;
    }
    return (requestedWindowBytes > capacityBytes)
               ? capacityBytes
               : requestedWindowBytes;
}

size_t FormatBleOtaErrorPayload(const char* const detail,
                                const size_t maxPayloadBytes,
                                char* const out,
                                const size_t outCapacity) {
    if (out == nullptr || outCapacity == 0U || maxPayloadBytes == 0U) {
        return 0U;
    }

    const std::string_view canonical =
        (detail != nullptr) ? std::string_view(detail) : std::string_view();
    std::string_view encoded = canonical;
    const size_t maxTokenBytes =
        (maxPayloadBytes > kErrorJsonOverheadBytes)
            ? (maxPayloadBytes - kErrorJsonOverheadBytes)
            : 0U;
    if (encoded.size() > maxTokenBytes) {
        encoded = CompactBleOtaErrorAlias(canonical);
    }
    if (encoded.size() > maxTokenBytes) {
        encoded = std::string_view(kFallbackCompactError,
                                   sizeof(kFallbackCompactError) - 1U);
    }

    const int written = std::snprintf(out,
                                      outCapacity,
                                      "{\"e\":\"%.*s\"}",
                                      static_cast<int>(encoded.size()),
                                      encoded.data());
    if (written <= 0 ||
        static_cast<size_t>(written) >= outCapacity) {
        out[0] = '\0';
        return 0U;
    }

    const size_t payloadLen = static_cast<size_t>(written);
    if (payloadLen > maxPayloadBytes) {
        out[0] = '\0';
        return 0U;
    }
    return payloadLen;
}

BleOtaTransactionResult BleOtaTransaction::Begin(
    const BleOtaBeginRequest& request,
    const BleOtaTransactionOps& ops) {
    if (m_state == State::Active) {
        return BleOtaTransactionResult::AlreadyActive;
    }
    if (request.declaredSize == 0U) {
        return BleOtaTransactionResult::InvalidSize;
    }
    if (request.declaredSize > BLE_OTA_SLOT_BYTES) {
        return BleOtaTransactionResult::ImageTooLarge;
    }
    if (request.negotiatedChunkBytes == 0U ||
        request.negotiatedChunkBytes > BLE_OTA_MAX_CHUNK_BYTES) {
        return BleOtaTransactionResult::InvalidNegotiatedChunk;
    }

    Clear(State::Idle);
    m_backendActive = true;
    if (ops.begin == nullptr ||
        !ops.begin(ops.context, request.declaredSize)) {
        Abort(ops);
        return BleOtaTransactionResult::BeginFailed;
    }

    m_state = State::Active;
    m_strictProfile = request.strictProfile;
    m_negotiatedChunkBytes = request.negotiatedChunkBytes;
    m_expectedSize = request.declaredSize;
    m_sessionEpoch = request.sessionEpoch;
    m_lastActivityMs = request.nowMs;
    m_crc32 = kCrc32Initial;
    return BleOtaTransactionResult::Accepted;
}

BleOtaTransactionResult BleOtaTransaction::WriteChunk(
    const uint32_t sessionEpoch,
    const uint8_t* const data,
    const size_t len,
    const uint32_t nowMs,
    const BleOtaTransactionOps& ops) {
    if (m_state != State::Active) {
        return BleOtaTransactionResult::NotActive;
    }
    if (sessionEpoch != m_sessionEpoch) {
        return BleOtaTransactionResult::StaleSession;
    }
    if (data == nullptr || len == 0U) {
        return BleOtaTransactionResult::EmptyChunk;
    }
    if (len > BLE_OTA_MAX_CHUNK_BYTES) {
        return BleOtaTransactionResult::ChunkTooLarge;
    }
    if (len > m_negotiatedChunkBytes) {
        return BleOtaTransactionResult::ChunkOverNegotiated;
    }
    if (len > static_cast<size_t>(m_expectedSize - m_receivedSize)) {
        return BleOtaTransactionResult::ChunkOvershoot;
    }
    if (ops.write == nullptr || !ops.write(ops.context, data, len)) {
        Abort(ops);
        return BleOtaTransactionResult::WriteFailed;
    }

    m_crc32 = UpdateCrc32(m_crc32, data, len);
    m_receivedSize += static_cast<uint32_t>(len);
    m_lastActivityMs = nowMs;
    return BleOtaTransactionResult::Accepted;
}

BleOtaTransactionResult BleOtaTransaction::ValidateEnd(
    const BleOtaEndRequest& request) const {
    if (m_state == State::Complete) {
        return BleOtaTransactionResult::AlreadyComplete;
    }
    if (m_state != State::Active) {
        return BleOtaTransactionResult::NotActive;
    }
    if (m_strictProfile && !request.hasSize) {
        return BleOtaTransactionResult::MissingEndSize;
    }
    if (request.hasSize && request.declaredSize != m_expectedSize) {
        return BleOtaTransactionResult::EndSizeMismatch;
    }
    if (m_receivedSize != m_expectedSize) {
        return BleOtaTransactionResult::Incomplete;
    }
    if (m_strictProfile &&
        (request.crc32Hex == nullptr || request.crc32Hex[0] == '\0')) {
        return BleOtaTransactionResult::MissingCrc;
    }
    if (request.crc32Hex != nullptr && request.crc32Hex[0] != '\0') {
        uint32_t expectedCrc32 = 0U;
        if (!TryParseExactCrc32(request.crc32Hex, expectedCrc32)) {
            return BleOtaTransactionResult::MalformedCrc;
        }
        if (ComputedCrc32() != expectedCrc32) {
            return BleOtaTransactionResult::ChecksumMismatch;
        }
    }
    return BleOtaTransactionResult::Accepted;
}

BleOtaTransactionResult BleOtaTransaction::End(
    const BleOtaEndRequest& request,
    const BleOtaTransactionOps& ops) {
    const auto validation = ValidateEnd(request);
    if (validation != BleOtaTransactionResult::Accepted) return validation;
    if (ops.flush == nullptr || !ops.flush(ops.context)) {
        Abort(ops);
        return BleOtaTransactionResult::FlushFailed;
    }
    if (ops.end == nullptr || !ops.end(ops.context, false)) {
        Abort(ops);
        return BleOtaTransactionResult::EndFailed;
    }

    m_backendActive = false;
    m_state = State::Complete;
    return BleOtaTransactionResult::Accepted;
}

BleOtaTransactionResult BleOtaTransaction::CheckTimeout(
    const uint32_t nowMs,
    const BleOtaTransactionOps& ops) {
    if (m_state != State::Active) {
        return BleOtaTransactionResult::NotActive;
    }
    if (static_cast<uint32_t>(nowMs - m_lastActivityMs) <
        BLE_OTA_INACTIVITY_TIMEOUT_MS) {
        return BleOtaTransactionResult::Accepted;
    }

    Abort(ops);
    return BleOtaTransactionResult::TimedOut;
}

void BleOtaTransaction::Abort(const BleOtaTransactionOps& ops) {
    if (m_backendActive && ops.abort != nullptr) {
        ops.abort(ops.context);
    }
    Clear(State::Idle);
}

bool BleOtaTransaction::IsActive() const {
    return m_state == State::Active;
}

bool BleOtaTransaction::IsComplete() const {
    return m_state == State::Complete;
}

bool BleOtaTransaction::IsStrictProfile() const {
    return m_strictProfile;
}

uint32_t BleOtaTransaction::ExpectedSize() const {
    return m_expectedSize;
}

uint32_t BleOtaTransaction::ReceivedSize() const {
    return m_receivedSize;
}

uint32_t BleOtaTransaction::SessionEpoch() const {
    return m_sessionEpoch;
}

uint32_t BleOtaTransaction::ComputedCrc32() const {
    return m_crc32 ^ kCrc32FinalXor;
}

uint32_t BleOtaTransaction::LastActivityMs() const {
    return m_lastActivityMs;
}

void BleOtaTransaction::Clear(const State nextState) {
    m_state = nextState;
    m_backendActive = false;
    m_strictProfile = false;
    m_negotiatedChunkBytes = 0U;
    m_expectedSize = 0U;
    m_receivedSize = 0U;
    m_sessionEpoch = 0U;
    m_lastActivityMs = 0U;
    m_crc32 = 0U;
}

} // namespace modules
