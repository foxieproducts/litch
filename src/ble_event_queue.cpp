#include "modules/ble_event_queue.hpp"

#include <algorithm>

#if defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#endif

namespace modules {

namespace {

class PriorityLockGuard final {
  public:
    explicit PriorityLockGuard(std::atomic_flag& lock) : m_lock(lock) {
        while (m_lock.test_and_set(std::memory_order_acquire)) {}
    }
    ~PriorityLockGuard() { m_lock.clear(std::memory_order_release); }

  private:
    std::atomic_flag& m_lock;
};

bool IsPriorityIngress(const BleEvent& event) {
    if (event.type == BleEventType::OwnerSecurity) return true;
    // Setup writes and control actions are user interactions, even though their
    // wire envelope is JSON. They must not sit behind document/OTA ingress.
    if (event.type == BleEventType::SettingWrite) return true;
    if (event.type == BleEventType::ConnectionSession) {
        const auto& input = event.payload.connectionSession.input;
        const size_t size = event.payload.connectionSession.len;
        // Object records share the ordinary bounded FIFO with other bulk
        // ingress. Negotiation, recovery, ordered actions and state control
        // retain the reserved priority lane.
        return size == 0U || (input[0U] & 0x0fU) != 4U;
    }
    if (event.type == BleEventType::OtaControl) {
        // Capability admission is a read-only connection bootstrap. Keep it
        // available even when slower diagnostics/update controls fill the
        // ordinary FIFO; mutating OTA controls retain strict data ordering.
        constexpr char info[] = "{\"cmd\":\"info\"}";
        const auto& input = event.payload.otaControl.input;
        const size_t size = event.payload.otaControl.len;
        return size == sizeof(info) - 1U &&
            std::equal(input, input + size, info);
    }
    if (event.type != BleEventType::Litchcraft) return false;
    const auto& input = event.payload.litchcraft.input;
    const size_t size = event.payload.litchcraft.len;
    if (size > 20U) return false;
    if (size >= 2U && input[0] == 0x4cU) {
        // Interactive legacy control. Snapshot/replacement/palette traffic
        // stays on the ordinary lane.
        return (input[1] >= 1U && input[1] <= 7U) || input[1] == 12U ||
            input[1] == 14U;
    }
    if (size < 3U || input[0] != 0x4dU || input[1] != 2U) return false;
    // V2 timeline, paint/effect/frame/history/canvas edits and transfer cancel
    // are bounded human controls. Snapshot/replace/stream data remains bulk.
    return (input[2] >= 1U && input[2] <= 7U) || input[2] == 12U ||
        input[2] == 13U || input[2] == 22U;
}

} // namespace

bool MakeConnectionSessionIngressEvent(const uint8_t* const bytes,
                                       const size_t size,
                                       const uint32_t connectionEpoch,
                                       BleEvent& event) {
    if (bytes == nullptr || size == 0U ||
        size > sizeof(BleConnectionSessionEvent::input)) {
        return false;
    }
    event.type = BleEventType::ConnectionSession;
    event.payload.connectionSession = BleConnectionSessionEvent{};
    std::copy_n(bytes, size, event.payload.connectionSession.input);
    event.payload.connectionSession.len = static_cast<uint16_t>(size);
    event.payload.connectionSession.connectionEpoch = connectionEpoch;
    return true;
}

bool MakeLitchcraftIngressEvent(const uint8_t* const bytes, const size_t size,
                               const uint32_t connectionEpoch, BleEvent& event) {
    if (bytes == nullptr || size == 0U || size > sizeof(BleLitchcraftEvent::input) ||
        (bytes[0] != 0x4dU && size > 20U)) return false;
    event.type = BleEventType::Litchcraft;
    event.payload.litchcraft = BleLitchcraftEvent{};
    for (size_t i = 0U; i < size; ++i) event.payload.litchcraft.input[i] = bytes[i];
    event.payload.litchcraft.len = static_cast<uint8_t>(size);
    event.payload.litchcraft.connectionEpoch = connectionEpoch;
    return true;
}

BleEvent::Payload::Payload() {
    connection = BleConnectionChangeEvent{};
}

BleEvent::BleEvent()
    : payload() {
}

bool BleEventQueue::Begin() {
#if defined(ARDUINO)
    if (m_queue != nullptr) {
        return true;
    }

    const QueueHandle_t queue = xQueueCreateStatic(
        CAPACITY, sizeof(BleEvent), m_queueStorage.data(), &m_queueControl);
    m_queue = queue;
    return queue != nullptr;
#else
    return true;
#endif
}

bool BleEventQueue::Enqueue(const BleEvent& event) {
    BleEvent queued = event;
    if (queued.ingressOrdinal == 0U) queued.ingressOrdinal = NextOrdinal();
    const bool priority = IsPriorityIngress(queued);
    if (priority) return EnqueuePriority(queued);
#if defined(ARDUINO)
    if (m_queue == nullptr) {
        return false;
    }

    const BaseType_t ok =
        xQueueSend(static_cast<QueueHandle_t>(m_queue), &queued, 0);
    return ok == pdTRUE;
#else
    if (m_count >= CAPACITY) {
        return false;
    }
    m_events[m_tail] = queued;
    m_tail = static_cast<uint8_t>((m_tail + 1U) % CAPACITY);
    ++m_count;
    return true;
#endif
}

bool BleEventQueue::EnqueueNext(const BleEvent& event) {
    BleEvent queued = event;
    if (queued.ingressOrdinal == 0U) queued.ingressOrdinal = NextOrdinal();
    if (IsPriorityIngress(queued)) return EnqueuePriority(queued);
#if defined(ARDUINO)
    if (m_queue == nullptr) return false;
    return xQueueSendToFront(static_cast<QueueHandle_t>(m_queue), &queued, 0) == pdTRUE;
#else
    if (m_count >= CAPACITY) return false;
    m_head = static_cast<uint8_t>((m_head + CAPACITY - 1U) % CAPACITY);
    m_events[m_head] = queued;
    ++m_count;
    return true;
#endif
}

bool BleEventQueue::UsesOrdinaryCapacity(const BleEvent& event) {
    return !IsPriorityIngress(event);
}

bool BleEventQueue::TryPeek(BleEvent& outEvent) const {
    if (TryPeekPriority(outEvent)) return true;
    return TryPeekOrdinary(outEvent);
}

#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
BleEventQueue::LiveIngressDiagnostics BleEventQueue::GetLiveIngressDiagnostics() const {
    LiveIngressDiagnostics result;
    {
        PriorityLockGuard guard(m_priorityLock);
        result.priorityCount = m_priorityCount;
    }
#if defined(ARDUINO)
    if (m_queue != nullptr) {
        result.ordinaryCount = static_cast<uint8_t>(
            uxQueueMessagesWaiting(static_cast<QueueHandle_t>(m_queue)));
    }
#else
    result.ordinaryCount = m_count;
#endif
    BleEvent head;
    if (TryPeekOrdinary(head)) {
        result.headType = head.type;
        if (head.type == BleEventType::ConnectionSession &&
            head.payload.connectionSession.len >= connection_session_codec::HEADER_BYTES) {
            const auto* bytes = head.payload.connectionSession.input;
            result.headKind = bytes[0U] & 0x0fU;
            result.headSequence = uint32_t(bytes[6U]) | (uint32_t(bytes[7U]) << 8U) |
                (uint32_t(bytes[8U]) << 16U) | (uint32_t(bytes[9U]) << 24U);
        }
    }
    return result;
}
#endif

bool BleEventQueue::TryPeekOrdinary(BleEvent& outEvent) const {
#if defined(ARDUINO)
    return m_queue != nullptr &&
        xQueuePeek(static_cast<QueueHandle_t>(m_queue), &outEvent, 0) == pdTRUE;
#else
    if (m_count == 0U) {
        return false;
    }
    outEvent = m_events[m_head];
    return true;
#endif
}

bool BleEventQueue::TryDequeue(BleEvent& outEvent) {
    if (TryDequeuePriority(outEvent)) return true;
#if defined(ARDUINO)
    if (m_queue == nullptr) {
        return false;
    }

    const BaseType_t ok = xQueueReceive(static_cast<QueueHandle_t>(m_queue), &outEvent, 0);
    return ok == pdTRUE;
#else
    if (m_count == 0U) {
        return false;
    }
    outEvent = m_events[m_head];
    m_head = static_cast<uint8_t>((m_head + 1U) % CAPACITY);
    --m_count;
    return true;
#endif
}

bool BleEventQueue::TryDequeueIfOrdinal(const uint32_t expectedOrdinal,
                                        BleEvent& outEvent) {
    PriorityLockGuard guard(m_priorityLock);
    if (m_priorityCount != 0U) {
        const auto& queued = m_priorityEvents[m_priorityHead];
        if (queued.ingressOrdinal != expectedOrdinal) return false;
        CopyPriorityEvent(queued, outEvent);
        m_priorityHead = static_cast<uint8_t>(
            (m_priorityHead + 1U) % PRIORITY_CAPACITY);
        --m_priorityCount;
        return true;
    }
    return TryDequeueOrdinaryIfOrdinal(expectedOrdinal, outEvent);
}

bool BleEventQueue::TryDequeueOrdinaryIfOrdinal(
    const uint32_t expectedOrdinal, BleEvent& outEvent) {
#if defined(ARDUINO)
    if (m_queue == nullptr) return false;
    BleEvent queued;
    if (xQueuePeek(static_cast<QueueHandle_t>(m_queue), &queued, 0) != pdTRUE ||
        queued.ingressOrdinal != expectedOrdinal) {
        return false;
    }
    return xQueueReceive(static_cast<QueueHandle_t>(m_queue), &outEvent, 0) ==
        pdTRUE;
#else
    if (m_count == 0U ||
        m_events[m_head].ingressOrdinal != expectedOrdinal) {
        return false;
    }
    outEvent = m_events[m_head];
    m_head = static_cast<uint8_t>((m_head + 1U) % CAPACITY);
    --m_count;
    return true;
#endif
}

bool BleEventQueue::TryDequeueConnectionSessionControl(
    BleEvent& outEvent, const uint16_t allowedCodes) {
    PriorityLockGuard guard(m_priorityLock);
    for (uint8_t offset = 0U; offset < m_priorityCount; ++offset) {
        const uint8_t index = static_cast<uint8_t>(
            (m_priorityHead + offset) % PRIORITY_CAPACITY);
        const auto& queued = m_priorityEvents[index];
        if (queued.type != BleEventType::ConnectionSession ||
            queued.byteLength == 0U || (queued.bytes[0U] & 0x0fU) != 0U) {
            continue;
        }
        if (allowedCodes != UINT16_MAX &&
            (queued.byteLength < 2U || queued.bytes[1U] >= 16U ||
             (allowedCodes & static_cast<uint16_t>(1U << queued.bytes[1U])) == 0U)) {
            continue;
        }
        CopyPriorityEvent(queued, outEvent);
        for (uint8_t next = offset + 1U; next < m_priorityCount; ++next) {
            m_priorityEvents[(m_priorityHead + next - 1U) % PRIORITY_CAPACITY] =
                m_priorityEvents[(m_priorityHead + next) % PRIORITY_CAPACITY];
        }
        m_priorityTail = static_cast<uint8_t>(
            (m_priorityTail + PRIORITY_CAPACITY - 1U) % PRIORITY_CAPACITY);
        --m_priorityCount;
        return true;
    }
    return false;
}

bool BleEventQueue::EnqueuePriority(const BleEvent& event) {
    PriorityLockGuard guard(m_priorityLock);
    if (m_priorityCount >= PRIORITY_CAPACITY) return false;
    auto& queued = m_priorityEvents[m_priorityTail];
    queued = {};
    queued.type = event.type;
    queued.ingressOrdinal = event.ingressOrdinal;
    if (event.type == BleEventType::OwnerSecurity) {
        queued.ownerSecurity = event.payload.ownerSecurity;
    } else if (event.type == BleEventType::SettingWrite) {
        queued.setting = event.payload.setting;
    } else if (event.type == BleEventType::Litchcraft) {
        queued.byteLength = event.payload.litchcraft.len;
        if (queued.byteLength > queued.bytes.size()) return false;
        std::copy_n(event.payload.litchcraft.input,
                    queued.byteLength, queued.bytes.begin());
        queued.connectionEpoch =
            event.payload.litchcraft.connectionEpoch;
    } else if (event.type == BleEventType::ConnectionSession) {
        // Classification precedes decoding; malformed non-Object frames may
        // exceed the bounded Control/Ordered priority storage.
        if (event.payload.connectionSession.len > queued.bytes.size()) return false;
        queued.byteLength = static_cast<uint8_t>(event.payload.connectionSession.len);
        std::copy_n(event.payload.connectionSession.input,
                    queued.byteLength, queued.bytes.begin());
        queued.connectionEpoch =
            event.payload.connectionSession.connectionEpoch;
    } else {
        queued.byteLength = event.payload.otaControl.len;
        if (queued.byteLength > queued.bytes.size()) return false;
        std::copy_n(event.payload.otaControl.input,
                    queued.byteLength, queued.bytes.begin());
        queued.sessionEpoch = event.payload.otaControl.sessionEpoch;
        queued.connectionEpoch = event.payload.otaControl.connectionEpoch;
    }
    m_priorityTail =
        static_cast<uint8_t>((m_priorityTail + 1U) % PRIORITY_CAPACITY);
    ++m_priorityCount;
    return true;
}

bool BleEventQueue::TryPeekPriority(BleEvent& outEvent) const {
    PriorityLockGuard guard(m_priorityLock);
    if (m_priorityCount == 0U) return false;
    const auto& queued = m_priorityEvents[m_priorityHead];
    CopyPriorityEvent(queued, outEvent);
    return true;
}

bool BleEventQueue::TryDequeuePriority(BleEvent& outEvent) {
    PriorityLockGuard guard(m_priorityLock);
    if (m_priorityCount == 0U) return false;
    const auto& queued = m_priorityEvents[m_priorityHead];
    CopyPriorityEvent(queued, outEvent);
    m_priorityHead = static_cast<uint8_t>(
        (m_priorityHead + 1U) % PRIORITY_CAPACITY);
    --m_priorityCount;
    return true;
}

void BleEventQueue::CopyPriorityEvent(const PriorityEvent& queued,
                                      BleEvent& outEvent) const {
    outEvent = {};
    outEvent.type = queued.type;
    outEvent.ingressOrdinal = queued.ingressOrdinal;
    if (queued.type == BleEventType::OwnerSecurity) {
        outEvent.payload.ownerSecurity = queued.ownerSecurity;
    } else if (queued.type == BleEventType::SettingWrite) {
        outEvent.payload.setting = queued.setting;
    } else if (queued.type == BleEventType::Litchcraft) {
        outEvent.payload.litchcraft.len = queued.byteLength;
        std::copy_n(queued.bytes.begin(), queued.byteLength,
                    outEvent.payload.litchcraft.input);
        outEvent.payload.litchcraft.connectionEpoch = queued.connectionEpoch;
    } else if (queued.type == BleEventType::ConnectionSession) {
        outEvent.payload.connectionSession.len = queued.byteLength;
        std::copy_n(queued.bytes.begin(), queued.byteLength,
                    outEvent.payload.connectionSession.input);
        outEvent.payload.connectionSession.connectionEpoch =
            queued.connectionEpoch;
    } else {
        outEvent.payload.otaControl = BleOtaControlEvent{};
        outEvent.payload.otaControl.len = queued.byteLength;
        std::copy_n(queued.bytes.begin(), queued.byteLength,
                    outEvent.payload.otaControl.input);
        outEvent.payload.otaControl.input[queued.byteLength] = '\0';
        outEvent.payload.otaControl.sessionEpoch = queued.sessionEpoch;
        outEvent.payload.otaControl.connectionEpoch = queued.connectionEpoch;
    }
}

uint32_t BleEventQueue::NextOrdinal() {
    uint32_t ordinal = m_nextOrdinal.fetch_add(1U, std::memory_order_relaxed);
    if (ordinal == 0U) {
        ordinal = m_nextOrdinal.fetch_add(1U, std::memory_order_relaxed);
    }
    return ordinal;
}

bool BleConnectionProjection::Begin() {
#if defined(ARDUINO)
    if (m_queue != nullptr) {
        return true;
    }
    m_queue = xQueueCreateStatic(1U, sizeof(BleConnectionChangeEvent),
                               m_queueStorage.data(), &m_queueControl);
    return m_queue != nullptr;
#else
    return true;
#endif
}

bool BleConnectionProjection::Publish(const BleConnectionChangeEvent& event) {
#if defined(ARDUINO)
    if (m_queue == nullptr) {
        return false;
    }
    return xQueueOverwrite(static_cast<QueueHandle_t>(m_queue), &event) == pdTRUE;
#else
    m_event = event;
    m_pending = true;
    return true;
#endif
}

bool BleConnectionProjection::TryPeek(BleConnectionChangeEvent& outEvent) const {
#if defined(ARDUINO)
    return m_queue != nullptr &&
        xQueuePeek(static_cast<QueueHandle_t>(m_queue), &outEvent, 0) == pdTRUE;
#else
    if (!m_pending) {
        return false;
    }
    outEvent = m_event;
    return true;
#endif
}

bool BleConnectionProjection::TryTake(BleConnectionChangeEvent& outEvent) {
#if defined(ARDUINO)
    if (m_queue == nullptr) {
        return false;
    }
    return xQueueReceive(static_cast<QueueHandle_t>(m_queue), &outEvent, 0) == pdTRUE;
#else
    if (!m_pending) {
        return false;
    }
    outEvent = m_event;
    m_pending = false;
    return true;
#endif
}

} // namespace modules
