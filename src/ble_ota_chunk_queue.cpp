#include "modules/ble_ota_chunk_queue.hpp"

#include <cstring>

#include "modules/litchcraft_frame_store_receiver.hpp"

namespace modules {

namespace {

class SpinLockGuard {
  public:
    explicit SpinLockGuard(std::atomic_flag& lock)
        : m_lock(lock) {
        while (m_lock.test_and_set(std::memory_order_acquire)) {
        }
    }

    ~SpinLockGuard() {
        m_lock.clear(std::memory_order_release);
    }

  private:
    std::atomic_flag& m_lock;
};

} // namespace

bool BleOtaChunkQueue::Begin() {
    SpinLockGuard guard(m_lock);
    m_head = 0U;
    m_tail = 0U;
    m_count = 0U;
    m_highWaterMark = 0U;
    m_readIndex = 0U;
    m_readClaimed = false;
    m_byteHead = 0U;
    m_byteTail = 0U;
    m_byteUsed = 0U;
    m_currentEpoch = 1U;
    m_failureEpoch = 0U;
    m_failure = BleOtaChunkQueueFailure::None;
    m_returnedObjectToken = m_returnedObjectCount = 0U;
    return true;
}

uint32_t BleOtaChunkQueue::AdvanceEpoch() {
    SpinLockGuard guard(m_lock);
    ++m_currentEpoch;
    if (m_currentEpoch == 0U) {
        m_currentEpoch = 1U;
    }
    m_failureEpoch = 0U;
    m_failure = BleOtaChunkQueueFailure::None;
    return m_currentEpoch;
}

bool BleOtaChunkQueue::AdvanceEpochIfCurrent(
    const uint32_t expectedEpoch) {
    SpinLockGuard guard(m_lock);
    if (m_currentEpoch != expectedEpoch) {
        return false;
    }
    ++m_currentEpoch;
    if (m_currentEpoch == 0U) {
        m_currentEpoch = 1U;
    }
    m_failureEpoch = 0U;
    m_failure = BleOtaChunkQueueFailure::None;
    return true;
}

uint32_t BleOtaChunkQueue::CurrentEpoch() const {
    SpinLockGuard guard(m_lock);
    return m_currentEpoch;
}

bool BleOtaChunkQueue::Enqueue(const uint8_t* data, const size_t len,
                               const uint32_t ingressOrdinal) {
    return EnqueueInternal(data, len, ingressOrdinal, true, 0U) == Admission::Accepted;
}

BleOtaChunkQueue::Admission BleOtaChunkQueue::EnqueueReserved(
    const uint8_t* data, const size_t len, const uint32_t ingressOrdinal,
    const uint32_t objectCreditToken) {
    return EnqueueInternal(data, len, ingressOrdinal, false, objectCreditToken);
}

BleOtaChunkQueue::Admission BleOtaChunkQueue::EnqueueInternal(
    const uint8_t* data, const size_t len, const uint32_t ingressOrdinal,
    const bool recordFailure, const uint32_t objectCreditToken) {
    SpinLockGuard guard(m_lock);
    if (data == nullptr || len == 0U) {
        if (recordFailure) RecordFailure(BleOtaChunkQueueFailure::Empty);
        return Admission::Invalid;
    }
    if (len > MAX_CHUNK_BYTES) {
        if (recordFailure) RecordFailure(BleOtaChunkQueueFailure::TooLarge);
        return Admission::Invalid;
    }

    if (m_count >= CAPACITY) {
        if (recordFailure) RecordFailure(BleOtaChunkQueueFailure::Full);
        return Admission::DeferredCapacity;
    }

    if (m_count == 0U) {
        m_byteHead = 0U;
        m_byteTail = 0U;
        m_byteUsed = 0U;
    }

    const size_t byteHead = m_byteHead;
    const size_t byteTail = m_byteTail;
    const size_t byteUsed = m_byteUsed;
    const size_t byteFree = BYTE_CAPACITY - byteUsed;
    size_t dataOffset = byteTail;
    size_t reservation = len;

    if (byteTail < byteHead) {
        if (len > byteHead - byteTail) {
            if (recordFailure) RecordFailure(BleOtaChunkQueueFailure::Full);
            return Admission::DeferredCapacity;
        }
    } else if (len > BYTE_CAPACITY - byteTail) {
        if (len > byteHead) {
            if (recordFailure) RecordFailure(BleOtaChunkQueueFailure::Full);
            return Admission::DeferredCapacity;
        }
        reservation += BYTE_CAPACITY - byteTail;
        dataOffset = 0U;
    }
    if (reservation > byteFree) {
        if (recordFailure) RecordFailure(BleOtaChunkQueueFailure::Full);
        return Admission::DeferredCapacity;
    }

    Slot& slot = m_slots[m_tail];
    slot.ingressOrdinal = ingressOrdinal;
    slot.sessionEpoch = m_currentEpoch;
    slot.objectCreditToken = objectCreditToken;
    slot.offset = static_cast<uint16_t>(dataOffset);
    slot.len = static_cast<uint16_t>(len);
    std::memcpy(m_bytes.data() + dataOffset, data, len);
    m_byteTail = static_cast<uint16_t>(
        (byteTail + reservation) % BYTE_CAPACITY);
    m_byteUsed = static_cast<uint16_t>(byteUsed + reservation);
    m_tail = static_cast<uint8_t>((m_tail + 1U) % CAPACITY);
    ++m_count;
    if (m_count > m_highWaterMark) {
        m_highWaterMark = m_count;
    }
    return Admission::Accepted;
}

bool BleOtaChunkQueue::TryPeekOrdinal(uint32_t& outIngressOrdinal) const {
    SpinLockGuard guard(m_lock);
    if (m_count == 0U) {
        return false;
    }

    outIngressOrdinal = m_slots[m_head].ingressOrdinal;
    return true;
}

bool BleOtaChunkQueue::TryPeekFirstLitchcraftBarrierOrdinal(
    uint32_t& outIngressOrdinal) const {
    SpinLockGuard guard(m_lock);
    for (uint8_t offset = 0U; offset < m_count; ++offset) {
        const Slot& slot = m_slots[(m_head + offset) % CAPACITY];
        if (slot.sessionEpoch == m_currentEpoch && slot.len >= 3U &&
            m_bytes[slot.offset] == 0x4dU &&
            m_bytes[slot.offset + 1U] == 2U &&
            (m_bytes[slot.offset + 2U] == 18U ||
             m_bytes[slot.offset + 2U] == 16U ||
             m_bytes[slot.offset + 2U] == 17U ||
             (m_bytes[slot.offset + 2U] >= 20U &&
              m_bytes[slot.offset + 2U] <= 21U))) {
            outIngressOrdinal = slot.ingressOrdinal;
            return true;
        }
    }
    return false;
}

bool BleOtaChunkQueue::TryPeekLitchcraftFrameStore() const {
    SpinLockGuard guard(m_lock);
    if (m_count == 0U) return false;
    const Slot& slot = m_slots[m_head];
    if (slot.len < 3U || m_bytes[slot.offset] != 0x4dU ||
        m_bytes[slot.offset + 1U] != 2U) {
        return false;
    }
    const uint8_t operation = m_bytes[slot.offset + 2U];
    return (operation >= 18U && operation <= 22U) ||
        (operation >= litchcraft_frame_store::STORE_SNAPSHOT &&
         operation <= litchcraft_frame_store::STORE_SELECT) ||
        (operation == 1U && slot.len == 10U &&
         (m_bytes[slot.offset + 9U] == 4U ||
          m_bytes[slot.offset + 9U] == 5U ||
          m_bytes[slot.offset + 9U] == 6U ||
          m_bytes[slot.offset + 9U] == 7U));
}

bool BleOtaChunkQueue::TryAcquireReadSlot(const uint8_t*& outData,
                                          size_t& outLen,
                                          uint32_t& outIngressOrdinal,
                                          uint32_t& outSessionEpoch) {
    SpinLockGuard guard(m_lock);
    if (m_readClaimed || m_count == 0U) {
        return false;
    }

    m_readClaimed = true;
    m_readIndex = m_head;
    const Slot& slot = m_slots[m_readIndex];
    outData = m_bytes.data() + slot.offset;
    outLen = slot.len;
    outIngressOrdinal = slot.ingressOrdinal;
    outSessionEpoch = slot.sessionEpoch;
    return true;
}

void BleOtaChunkQueue::DeferReadSlot() {
    SpinLockGuard guard(m_lock);
    m_readClaimed = false;
}

void BleOtaChunkQueue::ReleaseReadSlot() {
    SpinLockGuard guard(m_lock);
    if (!m_readClaimed) {
        return;
    }

    const Slot& slot = m_slots[m_readIndex];
    RecordReturnedObjectCredit(slot.objectCreditToken);
    const size_t skipped =
        (size_t(slot.offset) + BYTE_CAPACITY - m_byteHead) % BYTE_CAPACITY;
    const size_t released = skipped + slot.len;
    m_byteHead = static_cast<uint16_t>(
        (size_t(slot.offset) + slot.len) % BYTE_CAPACITY);
    m_byteUsed = static_cast<uint16_t>(m_byteUsed - released);
    m_head = static_cast<uint8_t>((m_head + 1U) % CAPACITY);
    --m_count;
    m_readClaimed = false;
    if (m_count == 0U) {
        m_byteHead = 0U;
        m_byteTail = 0U;
        m_byteUsed = 0U;
    }
}

void BleOtaChunkQueue::RecordReturnedObjectCredit(const uint32_t token) {
    // Caller owns m_lock. ConnectionSession tokens increase across Begin/End
    // and stop before their high bit, so an old stream cannot replace a newer
    // stream's unconsumed return batch. Queue retirement does not revoke credit.
    if (token == 0U || token < m_returnedObjectToken) return;
    if (token > m_returnedObjectToken) {
        m_returnedObjectToken = token;
        m_returnedObjectCount = 0U;
    }
    if (m_returnedObjectCount < CAPACITY) ++m_returnedObjectCount;
}

bool BleOtaChunkQueue::TakeReturnedObjectCredits(uint32_t& token, uint32_t& count) {
    SpinLockGuard guard(m_lock);
    token = m_returnedObjectToken;
    count = m_returnedObjectCount;
    m_returnedObjectCount = 0U;
    // Retain the newest-token floor even after consumption.
    return count != 0U;
}

bool BleOtaChunkQueue::TryConsumeFailure(
    const uint32_t sessionEpoch,
    BleOtaChunkQueueFailure& outFailure) {
    SpinLockGuard guard(m_lock);
    if (m_failure == BleOtaChunkQueueFailure::None ||
        m_failureEpoch != sessionEpoch) {
        return false;
    }

    outFailure = m_failure;
    m_failureEpoch = 0U;
    m_failure = BleOtaChunkQueueFailure::None;
    return true;
}

uint8_t BleOtaChunkQueue::HighWaterMark() const {
    SpinLockGuard guard(m_lock);
    return m_highWaterMark;
}

#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
BleOtaChunkQueue::LiveIngressDiagnostics BleOtaChunkQueue::GetLiveIngressDiagnostics() const {
    SpinLockGuard guard(m_lock);
    LiveIngressDiagnostics result{m_byteUsed, m_count, m_readClaimed};
    if (m_count != 0U) {
        const auto& head = m_slots[m_head];
        result.headOrdinal = head.ingressOrdinal;
        if (head.len >= 3U && m_bytes[head.offset] == 0x4dU &&
            m_bytes[head.offset + 1U] == 2U) {
            result.headOperation = m_bytes[head.offset + 2U];
        }
    }
    return result;
}
#endif

void BleOtaChunkQueue::RecordFailure(
    const BleOtaChunkQueueFailure failure) {
    if (m_failure == BleOtaChunkQueueFailure::None) {
        m_failureEpoch = m_currentEpoch;
        m_failure = failure;
    }
}

} // namespace modules
