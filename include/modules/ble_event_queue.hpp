#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#endif

#include "modules/connection_session_codec.hpp"
#include "modules/connection_session.hpp"
#include "modules/board_owner_peer.hpp"

namespace modules {

enum class BleEventType : uint8_t {
    None = 0,
    SettingWrite,
    Litchcraft,
    LitchcraftSubscription,
    OtaControl,
    ConsoleInput,
    ConnectionChange,
    ConnectionParameters,
    LinkObservation,
    ConnectionSession,
    OwnerSecurity,
    ConnectionLitchcraft,
};

struct BleOwnerSecurityEvent {
    enum class Kind : uint8_t { Identity, SecurityCompletion };
    Kind kind = Kind::SecurityCompletion;
    uint16_t connHandle = 0xffffU;
    uint32_t connectionEpoch = 0U;
    uint32_t admissionToken = 0U;
    uint32_t sessionToken = 0U;
    int32_t nativeStatus = -1;
    uint8_t peerAddress[6U]{};
    uint8_t peerAddressType = 0U;
    uint8_t peerCount = 0U;
    BoardOwnerPeerSet savedPeers{};
    bool inventoryAvailable = false;
    bool peerIdentityAvailable = false;
    bool peerIdentityResolved = false;
    bool encrypted = false;
    bool bonded = false;
    bool peerMatches = false;
};

struct BleSettingWriteEvent {
    char key[32] = {};
    char value[64] = {};
    bool hasValue = false;
    uint32_t connectionEpoch = 0U;
};

struct BleOtaControlEvent {
    char input[192] = {};
    uint8_t len = 0;
    uint32_t sessionEpoch = 0U;
    uint32_t connectionEpoch = 0U;
    uint32_t objectCreditToken = 0U;
};

// char0106 carries canonical v2 chunks at negotiated ATT payload 244. Keep
// this distinct from text controls, sharing the existing bounded queue union.
struct BleLitchcraftEvent {
    uint8_t input[244] = {};
    uint8_t len = 0;
    uint32_t connectionEpoch = 0U;
};

// A session token fences this derived event and owns its raw FIFO reservation.
// It replaces the epoch field without reducing the legacy 244-byte payload.
struct BleConnectionLitchcraftEvent {
    uint8_t input[244] = {};
    uint8_t len = 0U;
    uint32_t objectCreditToken = 0U;
};

struct BleConsoleInputEvent {
    char input[192] = {};
    uint8_t len = 0;
    uint32_t connectionEpoch = 0U;
};

struct BleConnectionSessionEvent {
    uint8_t input[ConnectionSession::OBJECT_PACKET_BYTES] = {};
    uint16_t len = 0U;
    uint32_t connectionEpoch = 0U;
};

struct BleConnectionChangeEvent {
    bool connected = false;
    uint16_t connHandle = 0;
    uint16_t mtu = 0;
    int32_t reason = 0;
    uint8_t connectionCount = 0;
    uint32_t connectionEpoch = 0U;
    uint32_t callbackAtMs = 0U;
    char peerAddress[24] = {};
};

struct BleDataLengthObservation {
    uint16_t txOctets = 0U, txTimeUs = 0U, rxOctets = 0U, rxTimeUs = 0U;
};

struct BleLinkObservationEvent {
    uint16_t handle = 0xffffU, mtu = 0U;
    uint8_t txPhy = 0U, rxPhy = 0U;
    uint32_t connectionEpoch = 0U;
    BleDataLengthObservation dataLength{};
    bool dataLengthObserved = false;
    bool phyUpdateCompleted = false;
};

struct BleConnectionParametersEvent {
    uint16_t handle = 0xffffU;
    uint16_t intervalUnits = 0U;
    uint16_t latency = 0U;
    uint16_t supervisionTimeoutUnits = 0U;
    uint32_t connectionEpoch = 0U;
    bool initial = false;
};

struct BleEvent {
    BleEventType type = BleEventType::None;
    uint32_t ingressOrdinal = 0U;

    union Payload {
        BleSettingWriteEvent setting;
        BleOtaControlEvent otaControl;
        BleLitchcraftEvent litchcraft;
        BleConnectionLitchcraftEvent connectionLitchcraft;
        BleConsoleInputEvent consoleInput;
        BleConnectionSessionEvent connectionSession;
        BleConnectionChangeEvent connection;
        BleConnectionParametersEvent connectionParameters;
        BleLinkObservationEvent linkObservation;
        BleOwnerSecurityEvent ownerSecurity;

        Payload();
    } payload;

    BleEvent();
};

static_assert(sizeof(BleEvent::Payload) == 520U && sizeof(BleEvent) == 528U,
              "Session ingress lengths must retain the complete 512-byte packet");

bool MakeLitchcraftIngressEvent(const uint8_t* bytes, size_t size,
                               uint32_t connectionEpoch, BleEvent& event);
bool MakeConnectionSessionIngressEvent(const uint8_t* bytes, size_t size,
                                       uint32_t connectionEpoch,
                                       BleEvent& event);

class BleEventQueue {
  public:
    // A native Object batch may contain the complete negotiated window. Keep
    // that burst intact at the callback boundary without allocating slots
    // that the negotiated bulk path cannot use.
    static constexpr uint8_t CAPACITY = ConnectionSession::OBJECT_CAPACITY;
    static constexpr uint8_t PRIORITY_CAPACITY = 4;
    static_assert(size_t(CAPACITY) * sizeof(BleEvent) <= 54U * 260U,
                  "Larger Object packets must preserve the callback FIFO RAM budget");

    bool Begin();
    bool Enqueue(const BleEvent& event);
    // Replace a just-consumed envelope with its completed domain control at
    // the same ordinary FIFO position. Existing priority classification stays.
    bool EnqueueNext(const BleEvent& event);
    static bool UsesOrdinaryCapacity(const BleEvent& event);
    bool TryPeek(BleEvent& outEvent) const;
    bool TryPeekOrdinary(BleEvent& outEvent) const;
    bool TryDequeueOrdinaryIfOrdinal(uint32_t expectedOrdinal, BleEvent& outEvent);
    bool TryDequeue(BleEvent& outEvent);
    bool TryDequeueIfOrdinal(uint32_t expectedOrdinal, BleEvent& outEvent);
    // Session repair/control must remain serviceable while a reply-producing
    // event waits for retained output capacity in the existing priority lane.
    bool TryDequeueConnectionSessionControl(
        BleEvent& outEvent, uint16_t allowedCodes = UINT16_MAX);
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
    struct LiveIngressDiagnostics {
        uint8_t ordinaryCount = 0U, priorityCount = 0U;
        BleEventType headType = BleEventType::None;
        uint8_t headKind = 0xffU;
        uint32_t headSequence = 0U;
    };
    // Ordinary FIFO head, separate from the reserved priority-lane count.
    // Call on its sole consumer; concurrent producers may append afterward.
    LiveIngressDiagnostics GetLiveIngressDiagnostics() const;
#endif

  private:
    struct PriorityEvent final {
        BleEventType type = BleEventType::None;
        uint32_t ingressOrdinal = 0U;
        BleSettingWriteEvent setting{};
        BleOwnerSecurityEvent ownerSecurity{};
        std::array<uint8_t, 20U> bytes{};
        uint8_t byteLength = 0U;
        uint32_t sessionEpoch = 0U;
        uint32_t connectionEpoch = 0U;
    };

    uint32_t NextOrdinal();
    bool EnqueuePriority(const BleEvent& event);
    bool TryPeekPriority(BleEvent& outEvent) const;
    bool TryDequeuePriority(BleEvent& outEvent);
    void CopyPriorityEvent(const PriorityEvent& queued,
                           BleEvent& outEvent) const;
    std::atomic<uint32_t> m_nextOrdinal{1U};
    mutable std::atomic_flag m_priorityLock = ATOMIC_FLAG_INIT;
    std::array<PriorityEvent, PRIORITY_CAPACITY> m_priorityEvents{};
    uint8_t m_priorityHead = 0U;
    uint8_t m_priorityTail = 0U;
    uint8_t m_priorityCount = 0U;
#if defined(ARDUINO)
    StaticQueue_t m_queueControl{};
    alignas(BleEvent) std::array<uint8_t, CAPACITY * sizeof(BleEvent)>
        m_queueStorage{};
    void* m_queue = nullptr;
#else
    BleEvent m_events[CAPACITY] = {};
    uint8_t m_head = 0U;
    uint8_t m_tail = 0U;
    uint8_t m_count = 0U;
#endif
};

// Connection availability is a latest-state projection, not bulk ingress.
// A one-item overwrite queue guarantees that queue pressure cannot discard the
// only callback that activates the current session or restores advertising.
class BleConnectionProjection {
  public:
    bool Begin();
    bool Publish(const BleConnectionChangeEvent& event);
    bool TryPeek(BleConnectionChangeEvent& outEvent) const;
    bool TryTake(BleConnectionChangeEvent& outEvent);

  private:
#if defined(ARDUINO)
    StaticQueue_t m_queueControl{};
    alignas(BleConnectionChangeEvent)
        std::array<uint8_t, sizeof(BleConnectionChangeEvent)> m_queueStorage{};
    void* m_queue = nullptr;
#else
    BleConnectionChangeEvent m_event{};
    bool m_pending = false;
#endif
};

} // namespace modules
