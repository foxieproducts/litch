#include "modules/ble_service.hpp"
#include "modules/game_persistence_coordinator.hpp"

#include <algorithm>
#include <cstring>
#include "modules/ble_json_text.hpp"

#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/ble_service_internal.hpp"
#if defined(LITCH_BLE_OTA)
#include "modules/ble_ota_ingress_order.hpp"
#endif

namespace modules {

#if defined(LITCH_BLE) && defined(LITCH_BLE_OTA)
namespace {

bool IsPriorityLitchcraftControl(const BleEvent& event) {
    if (event.type == BleEventType::ConnectionSession) {
        const auto& input = event.payload.connectionSession.input;
        const size_t size = event.payload.connectionSession.len;
        return size == 19U && (input[0U] & 0x0fU) == 3U &&
            input[1U] == 22U && input[10U] == 0x4dU &&
            input[11U] == 2U && input[12U] == 22U;
    }
    if (event.type != BleEventType::Litchcraft) return false;
    const auto& input = event.payload.litchcraft.input;
    const size_t size = event.payload.litchcraft.len;
    if (size >= 2U && input[0] == LitchcraftSession::MAGIC) {
        return (input[1] >= 1U && input[1] <= 7U) || input[1] == 12U ||
            input[1] == 14U;
    }
    return size >= 3U && input[0] == 0x4dU && input[1] == 2U &&
        ((input[2] >= 1U && input[2] <= 7U) || input[2] == 12U ||
         input[2] == 13U);
}

} // namespace
#endif

bool BleService::TryBeginHomeStatePublication(bool& outForce, uint32_t* outReceipt) {
    if (outReceipt != nullptr) *outReceipt = 0U;
    outForce = false;
#if defined(LITCH_BLE)
    if (m_recoveryOnly || !OwnerProductAccess()) return false;
#endif
    // Leave the request pending while a prior record drains. Repeated changes
    // coalesce into the latest authoritative snapshot instead of queueing old
    // bulk records faster than a minimum-MTU link can consume them.
    if (!IsConsoleStreamSubscribed()) {
        return false;
    }
#if defined(LITCH_BLE)
    if (m_consoleOutputLen != 0U ||
        !m_bleSettings.OwnsConnectionEpoch(m_bleSettings.CurrentConnectionEpoch())) {
        return false;
    }
#endif
    outForce = ConsumeConsoleStreamSnapshotRequest();
#if defined(LITCH_BLE)
    if (outForce) {
        const auto receipt = m_consoleSnapshotReceipt.exchange(0U, std::memory_order_acq_rel);
        if (outReceipt != nullptr) *outReceipt = receipt;
    }
#endif
    return true;
}

#if defined(LITCH_BLE)
bool BleService::TryBeginLitchcraftEvent(const BleEvent& event) {
    if (m_litchcraftGame == nullptr) return true;
    if (event.type == BleEventType::Litchcraft && m_bleSettings.OwnsConnectionEpoch(
            event.payload.litchcraft.connectionEpoch))
        return m_litchcraftGame->TryBeginLitchcraftPacket(
            event.payload.litchcraft.input, event.payload.litchcraft.len);
    if (event.type == BleEventType::ConnectionLitchcraft && m_connectionSession.IsActivated() &&
        event.payload.connectionLitchcraft.objectCreditToken == m_connectionSession.ObjectStreamToken() &&
        m_bleSettings.OwnsConnectionEpoch(m_connectionSession.Generation()))
        return m_litchcraftGame->TryBeginLitchcraftPacket(
            event.payload.connectionLitchcraft.input, event.payload.connectionLitchcraft.len);
    return true;
}

void BleService::ReconcileConnectionProjection() {
#if defined(LITCH_BLE_OTA)
    if (m_otaDisconnectPending.exchange(false, std::memory_order_acq_rel) &&
        m_otaTransaction.IsActive()) {
        logger::Warn("ble: OTA disconnect callback invalidated active session");
        AbortOtaTransfer(true, false);
    }
#endif
    BleConnectionChangeEvent projectedConnection;
    if (m_connectionProjection.TryTake(projectedConnection)) {
        BleEvent connectionEvent;
        connectionEvent.type = BleEventType::ConnectionChange;
        connectionEvent.payload.connection = projectedConnection;
        HandleEvent(connectionEvent);
    }
}
#endif

void BleService::DrainEventQueue() {
#if defined(LITCH_BLE)
#if defined(LITCH_BLE_OTA) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
    PollLiveIngressStallDiagnostics();
#endif
    ReconcileConnectionProjection();
    bool connectionSessionIngressHandled = false;
#if defined(LITCH_BLE_OTA)
    connectionSessionIngressHandled = ReturnBulkObjectCredits();
#endif
    connectionSessionIngressHandled =
        RetryPendingConnectionObject() || connectionSessionIngressHandled;
    namespace wire = connection_session_codec;
    constexpr uint16_t repairCodes =
        (1U << uint8_t(wire::Control::Query)) |
        (1U << uint8_t(wire::Control::Confirm)) |
        (1U << uint8_t(wire::Control::Progress)) |
        (1U << uint8_t(wire::Control::Heartbeat));
#if defined(LITCH_BLE_OTA)
    uint32_t drainedChunks = 0U;
    const uint32_t startedAt = fxe::Millis();
    // Finish each dispatched operation before yielding. A single SDK write can
    // exceed the soft time budget; never keep draining a refilled queue after it.
    for (uint32_t items = 0U;
         items < ble_internal::OTA_INGRESS_DRAIN_MAX_ITEMS &&
         (items == 0U || fxe::Millis() - startedAt < ble_internal::OTA_INGRESS_DRAIN_SLICE_MS);
         ++items) {
        ReconcileConnectionProjection();
#if defined(LITCH_NATIVE)
        if (m_beforeIngressDispatchTestHook != nullptr) {
            const auto hook = m_beforeIngressDispatchTestHook;
            void* const context = m_beforeIngressDispatchTestContext;
            m_beforeIngressDispatchTestHook = nullptr;
            m_beforeIngressDispatchTestContext = nullptr;
            hook(context);
        }
#endif
        if (HandleRejectedOtaBeginCandidateIngressFailure()) {
            continue;
        }
        if (HandleOtaQueueFailure()) {
            continue;
        }
        {
            BleEvent retired;
            if (m_eventQueue.TryPeekOrdinary(retired)) {
                uint32_t eventEpoch = 0U;
                switch (retired.type) {
                case BleEventType::ConnectionSession: eventEpoch = retired.payload.connectionSession.connectionEpoch; break;
                case BleEventType::OtaControl: eventEpoch = retired.payload.otaControl.connectionEpoch; break;
                case BleEventType::Litchcraft: eventEpoch = retired.payload.litchcraft.connectionEpoch; break;
                case BleEventType::ConsoleInput: eventEpoch = retired.payload.consoleInput.connectionEpoch; break;
                default: break;
                }
                const bool retiredDerived = retired.type == BleEventType::ConnectionLitchcraft &&
                    (retired.payload.connectionLitchcraft.objectCreditToken != m_connectionSession.ObjectStreamToken() ||
                     m_connectionSession.Generation() != m_bleSettings.CurrentConnectionEpoch());
                if ((retiredDerived || (eventEpoch != 0U && eventEpoch != m_bleSettings.CurrentConnectionEpoch())) &&
                    m_eventQueue.TryDequeueOrdinaryIfOrdinal(retired.ingressOrdinal, retired)) {
                    HandleEvent(retired);
                    continue;
                }
            }
        }
        ConnectionSession::ObjectCommand pendingObject;
        connectionSessionIngressHandled = RetryPendingConnectionObject() || connectionSessionIngressHandled;
        const bool pendingCapacity = m_connectionSession.PeekPendingObject(pendingObject);
        if (pendingCapacity) {
            BleEvent repair;
            if (m_eventQueue.TryDequeueConnectionSessionControl(repair, repairCodes)) {
                ReconcileConnectionProjection();
                HandleEvent(repair);
                connectionSessionIngressHandled = true;
                continue;
            }
        }
        const bool pendingLitchcraftBarrier = pendingCapacity &&
            pendingObject.opcode == ConnectionSession::LITCHCRAFT_OBJECT_OPCODE &&
            m_connectionLitchcraftInput[0U] == 0x4dU && m_connectionLitchcraftInput[1U] == 2U &&
            (m_connectionLitchcraftInput[2U] == 18U ||
             m_connectionLitchcraftInput[2U] == 20U || m_connectionLitchcraftInput[2U] == 21U ||
             m_connectionLitchcraftInput[2U] == 16U || m_connectionLitchcraftInput[2U] == 17U);
        BleEvent event;
        const bool hasOrdinary = m_eventQueue.TryPeekOrdinary(event);
        const uint32_t ordinaryOrdinal = hasOrdinary ? event.ingressOrdinal : 0U;
        const bool ordinaryOtaControl = hasOrdinary &&
            event.type == BleEventType::OtaControl;
        bool hasEvent = m_eventQueue.TryPeek(event);
        const bool selectOrdinary = ordinaryOtaControl && hasEvent &&
            event.type == BleEventType::ConnectionSession &&
            event.payload.connectionSession.len >= 2U &&
            (event.payload.connectionSession.input[0U] & 0x0fU) == 0U &&
            (event.payload.connectionSession.input[1U] >= 16U ||
             (repairCodes & (1U << event.payload.connectionSession.input[1U])) == 0U) &&
            IsBleOtaOrdinalEarlier(ordinaryOrdinal, event.ingressOrdinal);
        if (selectOrdinary) hasEvent = m_eventQueue.TryPeekOrdinary(event);
        // Keep the exact granted suffix in the existing raw FIFO while its
        // predecessor waits for domain capacity. Priority control still runs.
        if (pendingCapacity && hasEvent && event.type == BleEventType::ConnectionSession &&
            event.payload.connectionSession.len != 0U &&
            (event.payload.connectionSession.input[0U] & 0x0fU) == 4U) hasEvent = false;
        if (!CanRetainConnectionOtaStatus()) {
            BleEvent control;
            const uint16_t allowedCodes =
                selectOrdinary || m_connectionSession.HasPendingObjectAdmission()
                    ? repairCodes : UINT16_MAX;
            if (m_eventQueue.TryDequeueConnectionSessionControl(
                    control, allowedCodes)) {
                ReconcileConnectionProjection();
                connectionSessionIngressHandled = true;
                HandleEvent(control);
                continue;
            }
        }
        uint32_t chunkOrdinal = 0U;
        const bool hasChunk = m_otaChunkQueue.TryPeekOrdinal(chunkOrdinal);
        if (!hasEvent && !hasChunk) {
            break;
        }

        uint32_t barrierOrdinal = 0U;
        const bool compactBlockedByBarrier = hasEvent &&
            IsPriorityLitchcraftControl(event) &&
            ((m_otaChunkQueue.TryPeekFirstLitchcraftBarrierOrdinal(barrierOrdinal) &&
              IsBleOtaOrdinalEarlier(barrierOrdinal, event.ingressOrdinal)) ||
             (pendingLitchcraftBarrier && IsBleOtaOrdinalEarlier(pendingObject.ingressOrdinal, event.ingressOrdinal)));
        if (hasEvent && event.type != BleEventType::OtaControl &&
            !compactBlockedByBarrier) {
            if (!TryBeginLitchcraftEvent(event)) break;
            // The exact raw event stays queued when admission is denied.
            // End also covers a raced dequeue or stale-event rejection.
            GamePersistenceCoordinator::LitchcraftPresentationGuard admission(m_litchcraftGame);
            if (m_eventQueue.TryDequeueIfOrdinal(
                    event.ingressOrdinal, event)) {
                ReconcileConnectionProjection();
                connectionSessionIngressHandled =
                    connectionSessionIngressHandled ||
                    event.type == BleEventType::ConnectionSession;
                HandleEvent(event);
            }
            continue;
        }

        const BleOtaIngressSource source = SelectNextBleOtaIngress({
            hasEvent,
            hasEvent ? event.ingressOrdinal : 0U,
            hasChunk,
            chunkOrdinal,
        });
        if (source == BleOtaIngressSource::Chunk) {
            if (!CanRetainConnectionOtaStatus()) break;
            ReconcileConnectionProjection();
            if (DrainOneOtaChunk()) {
                ++drainedChunks;
                continue;
            }
            break;
        }
        if (source == BleOtaIngressSource::Control &&
            m_connectionSession.HasPendingObjectAdmission()) {
            // The existing queue already owns this control. Hand its admission
            // Progress to NimBLE before domain work can block the main loop.
            UpdateConnectionSessionNotifications();
            if (m_connectionSession.HasPendingObjectAdmission()) {
                BleEvent control;
                // Close/Open must keep their FIFO position: closing the stream
                // here would discard the Progress that still fences this work.
                if (m_eventQueue.TryDequeueConnectionSessionControl(
                        control, repairCodes)) {
                    ReconcileConnectionProjection();
                    connectionSessionIngressHandled = true;
                    HandleEvent(control);
                    continue;
                }
                break;
            }
        }
        if (source == BleOtaIngressSource::Control &&
            (CanRetainConnectionOtaStatus() || IsCurrentSharedBenchmarkCancel(event))) {
            if (!TryBeginLitchcraftEvent(event)) break;
            GamePersistenceCoordinator::LitchcraftPresentationGuard admission(m_litchcraftGame);
            if (!(selectOrdinary
                 ? m_eventQueue.TryDequeueOrdinaryIfOrdinal(event.ingressOrdinal, event)
                 : m_eventQueue.TryDequeueIfOrdinal(event.ingressOrdinal, event))) break;
            ReconcileConnectionProjection();
            const bool wasActive = m_otaTransaction.IsActive();
            const uint32_t previousEpoch = m_otaTransaction.SessionEpoch();
            HandleEvent(event);
            if (m_otaTransaction.IsActive() &&
                (!wasActive || m_otaTransaction.SessionEpoch() != previousEpoch)) {
                // Let initial progress latch before queued data triggers erase.
                // Preserve all accepted chunks and their original ordinal order.
                break;
            }
            continue;
        }
        break;
    }

    if (drainedChunks > 0U) {
        ++m_otaChunkDrainCycles;
        if (drainedChunks > m_otaChunkDrainMaxPerCycle) {
            m_otaChunkDrainMaxPerCycle = drainedChunks;
        }
    }
    PollOtaEndPersistence();
    // Released slots belong to this pass's Progress. Waiting for the next
    // drain leaves a chunks-only pass unable to replenish the phone's window.
    connectionSessionIngressHandled =
        ReturnBulkObjectCredits() || connectionSessionIngressHandled;
#else
    for (;;) {
        ReconcileConnectionProjection();
        connectionSessionIngressHandled = RetryPendingConnectionObject() || connectionSessionIngressHandled;
        BleEvent event;
        if (!m_eventQueue.TryPeek(event)) break;
        if (!CanRetainConnectionOtaStatus() ||
            m_connectionSession.HasPendingObject()) {
            BleEvent repair;
            if (m_eventQueue.TryDequeueConnectionSessionControl(repair, repairCodes)) {
                ReconcileConnectionProjection();
                HandleEvent(repair);
                connectionSessionIngressHandled = true;
                continue;
            }
        }
        if (event.type == BleEventType::OtaControl) {
            if (m_connectionSession.HasPendingObjectAdmission()) {
                UpdateConnectionSessionNotifications();
            }
            // Keep the existing queue as the owner until a complete reply can
            // be retained. Progress/Confirm above can release a full window.
            if (!CanRetainConnectionOtaStatus() ||
                m_connectionSession.HasPendingObjectAdmission()) break;
        }
        if (m_connectionSession.HasPendingObject() && event.type == BleEventType::ConnectionSession &&
            event.payload.connectionSession.len != 0U &&
            (event.payload.connectionSession.input[0U] & 0x0fU) == 4U) break;
        if (!TryBeginLitchcraftEvent(event)) break;
        GamePersistenceCoordinator::LitchcraftPresentationGuard admission(m_litchcraftGame);
        if (!m_eventQueue.TryDequeueIfOrdinal(event.ingressOrdinal, event)) break;
        connectionSessionIngressHandled =
            connectionSessionIngressHandled ||
            event.type == BleEventType::ConnectionSession;
        HandleEvent(event);
    }
#endif
    // A session packet commonly arrives just after the 20 ms BLE cadence.
    // Service its bounded response burst in this same cooperative pass so the
    // phone can use the next connection event instead of waiting for another
    // application cadence. Unrelated idle loops retain the normal cadence.
    if (connectionSessionIngressHandled) {
        UpdateConnectionSessionNotifications();
    }
#endif

    const uint32_t dropped = m_droppedEvents.exchange(0, std::memory_order_acq_rel);
    if (dropped > 0) {
#if defined(LITCH_BLE) && defined(LITCH_BLE_OTA) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
        if (!m_liveIngressDiagnosticLogged && m_connectionSession.IsActivated()) {
            m_liveIngressDiagnosticLogged = true;
            const auto live = GetLiveIngressDiagnostics();
            // Capture both owners before the first row can wait on log output.
            if (m_liveIngressDiagnosticCallback != nullptr) {
                m_liveIngressDiagnosticCallback(m_litchcraftFrameIngressContext);
            } else {
                logger::Warn("store_ingress_live: available=0");
            }
            // reverse=confirmed/grant; raw=ordinary/priority/type/kind/sequence;
            // store=descriptors/reserved_bytes/read_claimed. No cross-owner transaction.
            logger::Warn("ble_ingress_live: epoch=%lu accepted=%lu grant=%lu pending=%lu reverse=%lu/%lu raw=%u/%u/%u/%u/%lu store=%u/%u/%u",
                static_cast<unsigned long>(live.session.generation),
                static_cast<unsigned long>(live.session.accepted),
                static_cast<unsigned long>(live.session.grant),
                static_cast<unsigned long>(live.session.pending),
                static_cast<unsigned long>(live.session.reverseConfirmed),
                static_cast<unsigned long>(live.session.reverseGrant),
                unsigned(live.raw.ordinaryCount), unsigned(live.raw.priorityCount),
                unsigned(live.raw.headType), unsigned(live.raw.headKind),
                static_cast<unsigned long>(live.raw.headSequence),
                unsigned(live.store.count), unsigned(live.store.bytes),
                unsigned(live.store.readClaimed));
        }
#endif
        logger::Warn("ble: dropped %lu ingress events", static_cast<unsigned long>(dropped));
    }
}

#if defined(LITCH_BLE) && defined(LITCH_BLE_OTA) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
void BleService::PollLiveIngressStallDiagnostics() {
    const uint32_t nowMs = fxe::Millis();
    const uint32_t loopGapMs = m_liveStallLastDrainMs == 0U ? 0U : nowMs - m_liveStallLastDrainMs;
    m_liveStallLastDrainMs = nowMs;
    const uint32_t quietMs = nowMs - m_liveStallProgressMs;
    if (!m_connectionSession.IsActivated() ||
        m_liveStallObjectToken != m_connectionSession.ObjectStreamToken()) {
        m_liveStallArmed = false;
        return;
    }
    if (!m_liveStallArmed || m_liveStallSnapshots >= 2U ||
        (quietMs < 750U && loopGapMs < 750U) ||
        (m_liveStallSnapshots != 0U && nowMs - m_liveStallLastSampleMs < 750U)) return;
    ++m_liveStallSnapshots;
    m_liveStallLastSampleMs = nowMs;
    // Before Reconcile: a late loop must retain the retired logical frontier.
    // A large gap is detected on resumption, never claimed as a mid-gap sample.
    const auto live = GetLiveIngressDiagnostics();
    if (m_liveIngressDiagnosticCallback != nullptr)
        m_liveIngressDiagnosticCallback(m_litchcraftFrameIngressContext);
    else logger::Warn("store_ingress_live: available=0");
    // e=logical/native epoch; obj=accepted/grant/pending;
    // rx=current-native-epoch available/sequence/enqueued;
    // raw=ordinary/priority/type/kind/sequence; fifo=count/bytes/claimed/ordinal/op.
    logger::Warn("ble_stall: e=%lu/%lu age=%lu gap=%lu n=%u obj=%lu/%lu/%lu rev=%lu/%lu rx=%u/%lu/%u tx=%lu raw=%u/%u/%u/%u/%lu fifo=%u/%u/%u/%lu/%u",
        static_cast<unsigned long>(live.session.generation),
        static_cast<unsigned long>(live.nativeEpoch),
        static_cast<unsigned long>(quietMs), static_cast<unsigned long>(loopGapMs), unsigned(m_liveStallSnapshots),
        static_cast<unsigned long>(live.session.accepted), static_cast<unsigned long>(live.session.grant),
        static_cast<unsigned long>(live.session.pending), static_cast<unsigned long>(live.session.reverseConfirmed),
        static_cast<unsigned long>(live.session.reverseGrant), unsigned(live.nativeAvailable),
        static_cast<unsigned long>(live.nativeSequence),
        unsigned(live.nativeEnqueued), static_cast<unsigned long>(live.progressSubmitted),
        unsigned(live.raw.ordinaryCount), unsigned(live.raw.priorityCount), unsigned(live.raw.headType),
        unsigned(live.raw.headKind), static_cast<unsigned long>(live.raw.headSequence), unsigned(live.store.count),
        unsigned(live.store.bytes), unsigned(live.store.readClaimed),
        static_cast<unsigned long>(live.store.headOrdinal), unsigned(live.store.headOperation));
}
#endif

#if defined(LITCH_BLE)
#if defined(LITCH_BLE_OTA)
bool BleService::ReturnBulkObjectCredits() {
    uint32_t token = 0U, count = 0U;
    if (!m_otaChunkQueue.TakeReturnedObjectCredits(token, count)) return false;
    if (!m_bleSettings.OwnsConnectionEpoch(m_connectionSession.Generation()) ||
        token != m_connectionSession.ObjectStreamToken()) return false;
    for (uint32_t returned = 0U; returned < count; ++returned)
        m_connectionSession.ReturnObjectCredit(token);
    return m_connectionSession.HasPendingObjectAdmission();
}
#endif
bool BleService::RetryPendingConnectionObject() {
    if (!m_bleSettings.OwnsConnectionEpoch(m_connectionSession.Generation())) return false;
    ConnectionSession::ObjectCommand object;
    if (!m_connectionSession.PeekPendingObject(object)) return false;
    DispatchConnectionObject(object, m_connectionSession.Generation());
    return true;
}
#endif

bool BleService::TryReadConsoleInputChar(char& outChar) {
#if defined(LITCH_BLE)
    return TryDequeueConsoleInputChar(outChar);
#else
    (void)outChar;
    return false;
#endif
}

bool BleService::IsConsoleStreamSubscribed() const {
#if defined(LITCH_BLE)
    return OwnerProductAccess() && IsConnected() &&
           m_consoleStreamSubscribed.load(std::memory_order_acquire);
#else
    return false;
#endif
}

bool BleService::ConsumeConsoleStreamSnapshotRequest() {
#if defined(LITCH_BLE)
    return m_consoleStreamSnapshotRequested.exchange(
        false, std::memory_order_acq_rel);
#else
    return false;
#endif
}

void BleService::WriteConsoleOutput(const char* data, const size_t len) {
#if defined(LITCH_BLE)
    if (m_bleLinkTestInProgress || data == nullptr || len == 0 || !IsConnected() ||
        !IsConsoleStreamSubscribed() ||
        m_consoleStreamCharacteristic == nullptr) {
        return;
    }

    for (size_t i = 0; i < len; ++i) {
        if (m_consoleOutputLen >= CONSOLE_OUTPUT_BUFFER_CAPACITY) {
            // Report through the support snapshot. Logging here feeds the full queue.
            m_consoleOutputOverflows.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        m_consoleOutputBuffer[m_consoleOutputLen++] = data[i];
    }

    FlushConsoleOutputBuffer();
#else
    (void)data;
    (void)len;
#endif
}

#if defined(LITCH_BLE)
void BleService::HandleEvent(const BleEvent& event) {
    if (m_bleSettings.OwnsConnectionEpoch(m_connectionSession.Generation()) &&
        event.type == BleEventType::OtaControl) {
        m_connectionSession.ReturnObjectCredit(event.payload.otaControl.objectCreditToken);
    } else if (m_bleSettings.OwnsConnectionEpoch(m_connectionSession.Generation()) &&
               event.type == BleEventType::ConnectionLitchcraft && BleEventQueue::UsesOrdinaryCapacity(event)) {
        m_connectionSession.ReturnObjectCredit(event.payload.connectionLitchcraft.objectCreditToken);
    }
    if (!OwnerProductAccess() && (event.type == BleEventType::ConsoleInput ||
        event.type == BleEventType::SettingWrite || event.type == BleEventType::Litchcraft ||
        event.type == BleEventType::ConnectionLitchcraft)) return;
    if (m_recoveryOnly &&
        (event.type == BleEventType::ConsoleInput ||
         event.type == BleEventType::SettingWrite ||
         event.type == BleEventType::Litchcraft ||
         event.type == BleEventType::ConnectionLitchcraft ||
         event.type == BleEventType::LitchcraftSubscription)) return;
    switch (event.type) {
        case BleEventType::OwnerSecurity:
            HandleOwnerSecurity(event.payload.ownerSecurity);
            break;
        case BleEventType::ConnectionChange: {
            // A stop/restart or a rapid replacement may leave older lifecycle
            // events queued. Only the newest callback-side epoch may alter the
            // active connection or response ledger.
            if (!m_bleSettings.ActivateConnectionEpoch(
                    event.payload.connection.connectionEpoch)) {
                break;
            }
            m_connected.store(event.payload.connection.connected, std::memory_order_release);
            m_linkObservation = {}; // Physical link facts survive shared-session renegotiation.
            // Completion delivery belongs to one BLE connection. Accepted
            // semantic actions remain service-owned, but a disconnect or a
            // replacement connection relinquishes every prior response ID.
            const uint32_t connectionEpoch = event.payload.connection.connectionEpoch;
            bool projectedLitchcraftSubscription = false;
            bool projectedConnectionSessionSubscription = false;
            bool projectedOtaSubscription = false;
            bool projectedConsoleSubscription = false;
            if (event.payload.connection.connected) {
                for (;;) {
                    const uint32_t litchcraftRevision =
                        m_litchcraftSubscriptionRevision.load(std::memory_order_acquire);
                    const uint32_t otaRevision =
                        m_otaControlSubscriptionRevision.load(std::memory_order_acquire);
                    const uint32_t consoleRevision =
                        m_consoleStreamSubscriptionRevision.load(std::memory_order_acquire);
                    const uint32_t connectionSessionRevision =
                        m_connectionSessionSubscriptionRevision.load(
                            std::memory_order_acquire);
                    projectedLitchcraftSubscription =
                        m_litchcraftSubscriptionEpoch.load(std::memory_order_acquire) == connectionEpoch &&
                        (m_litchcraftSubscriptionValue.load(std::memory_order_acquire) & 1U) != 0U;
                    projectedOtaSubscription =
                        m_otaControlSubscriptionEpoch.load(std::memory_order_acquire) == connectionEpoch &&
                        (m_otaControlSubscriptionValue.load(std::memory_order_acquire) & 0x03U) != 0U;
                    projectedConsoleSubscription =
                        m_consoleStreamSubscriptionEpoch.load(std::memory_order_acquire) == connectionEpoch &&
                        (m_consoleStreamSubscriptionValue.load(std::memory_order_acquire) & 0x03U) != 0U;
                    projectedConnectionSessionSubscription =
                        m_connectionSessionSubscriptionEpoch.load(
                            std::memory_order_acquire) == connectionEpoch &&
                        (m_connectionSessionSubscriptionValue.load(
                             std::memory_order_acquire) & 0x03U) != 0U;
#if defined(LITCH_NATIVE)
                    if (m_subscriptionActivationTestHook != nullptr) {
                        const auto hook = m_subscriptionActivationTestHook;
                        void* const context = m_subscriptionActivationTestContext;
                        m_subscriptionActivationTestHook = nullptr;
                        m_subscriptionActivationTestContext = nullptr;
                        hook(context);
                    }
#endif
                    m_litchcraftSubscribed.store(
                        projectedLitchcraftSubscription, std::memory_order_release);
                    m_otaControlSubscribed.store(
                        projectedOtaSubscription, std::memory_order_release);
                    m_consoleStreamSubscribed.store(
                        projectedConsoleSubscription, std::memory_order_release);
                    m_connectionSessionSubscribed.store(
                        projectedConnectionSessionSubscription,
                        std::memory_order_release);
                    if (m_litchcraftSubscriptionRevision.load(std::memory_order_acquire) == litchcraftRevision &&
                        m_otaControlSubscriptionRevision.load(std::memory_order_acquire) == otaRevision &&
                        m_consoleStreamSubscriptionRevision.load(std::memory_order_acquire) == consoleRevision &&
                        m_connectionSessionSubscriptionRevision.load(
                            std::memory_order_acquire) ==
                            connectionSessionRevision) {
                        break;
                    }
                }
            } else {
                m_litchcraftSubscribed.store(false, std::memory_order_release);
                m_otaControlSubscribed.store(false, std::memory_order_release);
                m_consoleStreamSubscribed.store(false, std::memory_order_release);
                m_connectionSessionSubscribed.store(
                    false, std::memory_order_release);
            }
            if (event.payload.connection.connected) {
                m_sessionDiagnostics = {};
#if defined(LITCH_BLE_OTA) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
                m_liveIngressDiagnosticLogged = false;
                m_liveStallArmed = false;
                m_liveStallSnapshots = 0U;
                m_liveStallLastDrainMs = 0U;
                m_liveObjectProgressSubmitted = m_liveObjectProgressToken = 0U;
#endif
                ClearPendingConnectionControlAction();
                m_connectionSessionActivated.store(
                    false, std::memory_order_release);
                m_connectionSessionDomainDigest = 0U;
                m_connectionSessionLitchcraftStateRevision = 0U;
                m_connectionSessionLitchcraftStateValid = false;
                m_connectionSessionStoreAuthorityRevision = 0U;
                m_connectionSessionStoreAuthorityValid = false;
                ResetConnectionOtaStatusOutput();
                m_connectionLitchcraftExpected = 0U;
                m_connectionLitchcraftWritten = 0U;
                m_connectionLitchcraftOutputOffset = 0U;
                m_connectionLitchcraftOutputRevision = 0U;
#if defined(LITCH_BLE_OTA)
                m_connectionFrameStoreOutputOffset = 0U;
                m_connectionFrameStoreOutputCrc = 0U;
#endif
                if (!m_connectionSession.Begin(
                        connectionEpoch, m_connectionSessionBootNonce,
                        // The connection callback commonly carries ATT's
                        // pre-exchange MTU (23). Hello arrives only after the
                        // app calibrates the live link and advertises its real
                        // write ceiling, so retain the firmware ceiling here
                        // and let the transcript select the smaller peer value.
                        ConnectionSession::REVERSE_OBJECT_PACKET_BYTES,
                        ConnectionSession::LITCHCRAFT_ORDERED_TOPIC)) {
                    logger::Warn("ble: connection session unavailable");
                }
            } else {
                ClearPendingConnectionControlAction();
                m_connectionSession.End(m_connectionSession.Generation());
                m_connectionSessionActivated.store(
                    false, std::memory_order_release);
                ResetConnectionOtaStatusOutput();
                m_connectionLitchcraftExpected = 0U;
                m_connectionLitchcraftWritten = 0U;
                m_connectionLitchcraftOutputOffset = 0U;
                m_connectionLitchcraftOutputRevision = 0U;
#if defined(LITCH_BLE_OTA)
                m_connectionFrameStoreOutputOffset = 0U;
                m_connectionFrameStoreOutputCrc = 0U;
#endif
            }
            if (m_litchcraftGame) {
                if (event.payload.connection.connected) m_litchcraftGame->Litchcraft().BeginConnection();
                else m_litchcraftGame->Litchcraft().EndConnection();
#if defined(LITCH_BLE_OTA)
                WakeLitchcraftFrameIngress();
#endif
            }
            if (event.payload.connection.connected) {
                m_connectionHandle = event.payload.connection.connHandle;
                m_mtu.store(event.payload.connection.mtu, std::memory_order_release);
                m_connectionCount.store(event.payload.connection.connectionCount, std::memory_order_release);
                m_connectedAtMs = fxe::Millis();
                m_lastClientActivityMs = m_connectedAtMs;
                m_staleDisconnectIssued = false;
                if (projectedConsoleSubscription) {
                    m_consoleStreamSnapshotRequested.store(true, std::memory_order_release);
                }
                (void)m_peerAddress.Assign(event.payload.connection.peerAddress);
                m_consoleInput.Clear();
                m_consoleOutputLen = 0;
                m_lastConsoleNotifyMs = 0;
#if defined(LITCH_BLE_OTA)
                m_otaStrictProfileArmed = false;
#endif
                logger::Info("ble: connected");
            } else {
                const uint32_t callbackToLoopMs = static_cast<uint32_t>(
                    fxe::Millis() - event.payload.connection.callbackAtMs);
                const uint32_t connectedElapsedMs =
                    (m_connectedAtMs == 0U)
                        ? 0U
                        : static_cast<uint32_t>(fxe::Millis() -
                                                m_connectedAtMs);
                FlushConsoleOutputBuffer();
                m_connectionCount.store(0, std::memory_order_release);
                m_connectionHandle = ble_internal::INVALID_CONN_HANDLE;
                m_connectedAtMs = 0;
                m_lastClientActivityMs = 0;
                m_staleDisconnectIssued = false;
                m_otaControlSubscribed.store(false, std::memory_order_release);
#if defined(LITCH_BLE_OTA)
                if (m_otaTransaction.IsActive()) {
                    logger::Warn("ble: OTA interrupted by disconnect; aborting rx=%lu total=%lu sim=%u",
                                 static_cast<unsigned long>(m_otaBytesReceived),
                                 static_cast<unsigned long>(m_otaExpectedSize),
                                 m_otaSimulationMode ? 1U : 0U);
                    AbortOtaTransfer(true, false);
                }
                m_otaStrictProfileArmed = false;
#endif
                (void)m_peerAddress.Assign("");
                m_consoleInput.Clear();
                logger::Trace(
                    "ble: disconnect detail reason=%ld connected_ms=%lu callback_to_loop_ms=%lu",
                    static_cast<long>(event.payload.connection.reason),
                    static_cast<unsigned long>(connectedElapsedMs),
                    static_cast<unsigned long>(callbackToLoopMs));
                logger::Info("ble: disconnected");
                if (m_enabled) {
                    StartAdvertising();
                }
            }
            break;
        }
        case BleEventType::LitchcraftSubscription:
            if (IsConnected() && m_bleSettings.OwnsConnectionEpoch(
                    event.payload.otaControl.connectionEpoch)) {
                m_litchcraftSubscribed.store(event.payload.otaControl.input[0]!=0U);
            }
            break;
        case BleEventType::Litchcraft:
            if (m_litchcraftGame && m_bleSettings.OwnsConnectionEpoch(
                    event.payload.litchcraft.connectionEpoch)) {
                m_lastClientActivityMs=fxe::Millis();
                const auto* input = event.payload.litchcraft.input;
                const size_t size = event.payload.litchcraft.len;
                m_litchcraftGame->HandleLitchcraftPacket(
                    input, size,
                    m_litchcraftEligible && !IsOtaInProgress());
#if defined(LITCH_BLE_OTA)
                WakeLitchcraftFrameIngress();
#endif
            }
            break;
        case BleEventType::ConnectionLitchcraft:
            if (m_litchcraftGame && m_connectionSession.IsActivated() &&
                event.payload.connectionLitchcraft.objectCreditToken == m_connectionSession.ObjectStreamToken() &&
                m_bleSettings.OwnsConnectionEpoch(m_connectionSession.Generation())) {
                m_lastClientActivityMs = fxe::Millis();
                m_litchcraftGame->HandleLitchcraftPacket(event.payload.connectionLitchcraft.input,
                    event.payload.connectionLitchcraft.len, m_litchcraftEligible && !IsOtaInProgress());
#if defined(LITCH_BLE_OTA)
                WakeLitchcraftFrameIngress();
#endif
            }
            break;
        case BleEventType::ConnectionSession:
            if (m_bleSettings.OwnsConnectionEpoch(
                    event.payload.connectionSession.connectionEpoch)) {
                bool objectStreamBusy = m_connectionGameHistoryExpected != 0U;
                objectStreamBusy = objectStreamBusy ||
                    m_connectionOtaControlExpected != 0U ||
                    m_connectionLitchcraftExpected != 0U;
                m_connectionSession.SetObjectStreamBusy(objectStreamBusy);
                ConnectionSession::ObjectCommand object;
                const auto result = m_connectionSession.Ingress(
                    event.payload.connectionSession.connectionEpoch,
                    event.payload.connectionSession.input,
                    event.payload.connectionSession.len, &object, event.ingressOrdinal);
                ++m_sessionDiagnostics.received;
                m_sessionDiagnostics.lastIngressResult = static_cast<uint8_t>(result);
                connection_session_codec::PacketView diagnosticPacket;
                if (connection_session_codec::Codec::Decode(
                        {event.payload.connectionSession.input,
                         event.payload.connectionSession.len}, diagnosticPacket,
                        ConnectionSession::REVERSE_OBJECT_PACKET_BYTES) ==
                        connection_session_codec::Error::None &&
                    diagnosticPacket.header.kind == connection_session_codec::Kind::Control &&
                    diagnosticPacket.header.code < 16U) {
                    m_sessionDiagnostics.receivedControls |=
                        static_cast<uint16_t>(1U << diagnosticPacket.header.code);
                }
                if (result == ConnectionSession::Result::Ok) {
                    m_lastClientActivityMs = fxe::Millis();
                }
                m_connectionSessionActivated.store(
                    m_connectionSession.IsActivated(),
                    std::memory_order_release);
                if (!m_recoveryOnly && !OwnerProductAccess() && object.streamToken != 0U &&
                    object.opcode != ConnectionSession::PUBLIC_SUPPORT_OBJECT_OPCODE) {
                    m_connectionSession.RejectObject(object.streamToken, object.sequence);
                    break;
                }
                if (m_recoveryOnly && object.streamToken != 0U &&
                    object.opcode != ConnectionSession::OTA_CONTROL_OBJECT_OPCODE &&
                    object.opcode != ConnectionSession::OTA_DATA_OBJECT_OPCODE &&
                    object.opcode != ConnectionSession::PUBLIC_SUPPORT_OBJECT_OPCODE) {
                    m_connectionSession.RejectObject(object.streamToken, object.sequence);
                    break;
                }
#if defined(LITCH_BLE_OTA)
                if (!m_recoveryOnly && result == ConnectionSession::Result::Ok) {
                    const uint16_t receiptRequest =
                        m_connectionSession.TakeRequestedObjectReceipt();
                    if (receiptRequest != 0U &&
                        m_litchcraftFrameReceiptReplayCallback != nullptr) {
                        (void)m_litchcraftFrameReceiptReplayCallback(
                            m_litchcraftFrameIngressContext, receiptRequest,
                            event.payload.connectionSession.connectionEpoch);
                    }
                }
#endif
                if (result == ConnectionSession::Result::Ok && object.streamToken != 0U) {
                    DispatchConnectionObject(object, event.payload.connectionSession.connectionEpoch);
                }
            }
            break;
        case BleEventType::ConsoleInput:
            HandleConsoleInputEvent(event.payload.consoleInput);
            break;
        case BleEventType::SettingWrite:
            if (!m_bleSettings.OwnsConnectionEpoch(
                    event.payload.setting.connectionEpoch)) {
                break;
            }
            m_lastClientActivityMs = fxe::Millis();
            if (m_settings != nullptr) {
                m_bleSettings.HandleSettingWriteEvent(
                    *m_settings,
                    event.payload.setting,
                    m_settingsMutationCallback,
                    m_settingsMutationContext);
            }
            break;
        case BleEventType::OtaControl:
            if (!m_bleSettings.OwnsConnectionEpoch(
                    event.payload.otaControl.connectionEpoch)) {
                break;
            }
            m_lastClientActivityMs = fxe::Millis();
            HandleOtaControlPacket(event.payload.otaControl.input,
                                   event.payload.otaControl.len,
                                   event.payload.otaControl.sessionEpoch,
                                   event.payload.otaControl.connectionEpoch,
                                   event.payload.otaControl.objectCreditToken != 0U);
            break;
        case BleEventType::LinkObservation: {
            const auto& value = event.payload.linkObservation;
            if (!m_bleSettings.OwnsConnectionEpoch(value.connectionEpoch) ||
                !IsConnected() || value.handle != m_connectionHandle) break;
#if defined(LITCH_HW_VARIANT_S3)
            if (value.phyUpdateCompleted) m_preferredPhyPending = false;
#endif
            if (value.mtu >= 23U) {
                m_mtu.store(value.mtu, std::memory_order_release);
                m_linkObservation.mtu = value.mtu;
            }
            if ((value.txPhy == 1U || value.txPhy == 2U || value.txPhy == 3U) &&
                (value.rxPhy == 1U || value.rxPhy == 2U || value.rxPhy == 3U)) {
                m_linkObservation.txPhy = value.txPhy;
                m_linkObservation.rxPhy = value.rxPhy;
                m_linkObservation.phyKnown = true;
            }
            if (value.dataLengthObserved) {
                m_linkObservation.dataLength = value.dataLength;
                m_linkObservation.dataLengthKnown = true;
            }
            break;
        }
        case BleEventType::ConnectionParameters: {
            const BleConnectionParametersEvent& params =
                event.payload.connectionParameters;
            if (!m_bleSettings.OwnsConnectionEpoch(params.connectionEpoch) ||
                !m_connected.load(std::memory_order_acquire)) {
                break;
            }
            if (params.handle == m_connectionHandle) {
                m_linkObservation.intervalUnits = params.intervalUnits;
                m_linkObservation.latency = params.latency;
                m_linkObservation.timeoutUnits = params.supervisionTimeoutUnits;
                m_fastConnectionUpdatePending = true;
            }
            logger::Trace(
                "ble: conn params %s interval_units=%u interval_us=%lu latency=%u timeout_units=%u timeout_ms=%lu",
                params.initial ? "initial" : "negotiated",
                static_cast<unsigned>(params.intervalUnits),
                static_cast<unsigned long>(params.intervalUnits) * 1250UL,
                static_cast<unsigned>(params.latency),
                static_cast<unsigned>(params.supervisionTimeoutUnits),
                static_cast<unsigned long>(params.supervisionTimeoutUnits) *
                    10UL);
            break;
        }
        case BleEventType::None:
        default:
            break;
    }
}
#endif

} // namespace modules
