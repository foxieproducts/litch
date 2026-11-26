#include "modules/ble_service.hpp"

#include <algorithm>

#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#endif
#if defined(ARDUINO)
#include "firmware/board_owner_persistence_runtime.hpp"
#endif
#include "modules/ble_service_internal.hpp"
#include "logger.hpp"

namespace modules {
namespace {
bool UnlistedPeer(const BleOwnerSecurityEvent& event) {
    return event.inventoryAvailable && event.peerCount >= 1U && event.peerCount <= MAX_SAVED_PHONES && event.peerIdentityAvailable &&
        !event.peerMatches;
}
}

bool BleService::ConfigureOwnerPairing(BoardOwnerPairingRuntime& runtime,
    firmware::BoardOwnerPersistenceRuntime& persistence,
    BoardIdentityPersistence& identity) {
#if defined(LITCH_BLE)
    if (m_initialized) return false;
    m_ownerRuntime = &runtime;
    m_ownerPersistence = &persistence;
    m_ownerIdentity = &identity;
    if (OwnerPairingEnabled()) RevokeOwnerAdmission();
    return true;
#else
    (void)runtime; (void)persistence; (void)identity;
    return false;
#endif
}

uint64_t BleService::ConsoleInputGeneration() const {
#if defined(LITCH_BLE)
    if (!OwnerProductAccess()) return 0U;
    const auto connection = OwnerConnection();
    if (connection.epoch == 0U || connection.sessionToken == 0U) return 0U;
    return (static_cast<uint64_t>(connection.epoch) << 32U) | connection.sessionToken;
#else
    return 0U;
#endif
}

bool BleService::RequestOwnerSettingsClear() {
#if defined(LITCH_BLE)
    if (!OwnerPairingEnabled() || !OwnerProductAccess() || m_ownerRuntime == nullptr ||
        !m_ownerRuntime->RequestOwnerClear(OwnerConnection())) return false;
    // The reboot owner will continue native/receipt progress. Retire product
    // admission now so queued work cannot slip through before its next poll.
    RevokeOwnerAdmission();
    return true;
#else
    return false;
#endif
}

#if defined(LITCH_BLE)
bool BleService::OwnerPairingEnabled() const {
    return m_ownerPreviewEnabled && !m_recoveryOnly && !m_factoryBootOnly;
}

void BleService::RevokeOwnerAdmission() {
    m_ownerAdmissionToken.store(0U, std::memory_order_release);
    m_ownerProductEpoch.store(0U, std::memory_order_release);
    m_bleSettings.SetProductAccess(!OwnerPairingEnabled());
}

bool BleService::OwnerProductAccess() const {
    if (!OwnerPairingEnabled()) return true;
    const auto epoch = m_ownerNativeEpoch.load(std::memory_order_acquire);
    return epoch != 0U && m_ownerProductEpoch.load(std::memory_order_acquire) == epoch &&
        m_ownerRejectedEpoch.load(std::memory_order_acquire) != epoch &&
        m_bleSettings.OwnsConnectionEpoch(epoch) &&
        m_ownerNativeHandle.load(std::memory_order_acquire) != ble_internal::INVALID_CONN_HANDLE;
}

bool BleService::OwnerCallbackAllowed(uint16_t connHandle, bool bootstrap) const {
    return !OwnerPairingEnabled() ||
        (!m_ownerQuiescing.load(std::memory_order_acquire) &&
         m_ownerNativeEpoch.load(std::memory_order_acquire) != 0U &&
         m_ownerNativeHandle.load(std::memory_order_acquire) == connHandle &&
         (bootstrap || OwnerProductAccess()));
}

bool BleService::OwnerAcceptsNewConnection(const uint16_t connHandle) const {
    return !OwnerPairingEnabled() || ((!m_initialized || m_enabled) &&
        !m_ownerQuiescing.load(std::memory_order_acquire) &&
        (m_ownerNativeHandle.load(std::memory_order_acquire) == ble_internal::INVALID_CONN_HANDLE ||
         (connHandle != ble_internal::INVALID_CONN_HANDLE &&
          m_ownerNativeHandle.load(std::memory_order_acquire) == connHandle)));
}

bool BleService::NativePairingAllowed(uint16_t connHandle) const {
    if (!OwnerPairingEnabled()) return true;
    const uint32_t token = NativePairingLeaseToken(connHandle);
    return token != 0U &&
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
        OwnerNativePairingCandidate(connHandle) &&
#endif
        m_ownerAdmissionToken.load(std::memory_order_acquire) == token;
}

uint32_t BleService::NativePairingLeaseToken(uint16_t connHandle) const {
    if (!OwnerPairingEnabled()) return 0U;
    const uint32_t token = m_ownerAdmissionToken.load(std::memory_order_acquire);
    return token != 0U && !m_ownerQuiescing.load(std::memory_order_acquire) &&
        m_ownerAdmissionHandle.load(std::memory_order_acquire) == connHandle &&
        m_ownerNativeHandle.load(std::memory_order_acquire) == connHandle &&
        m_ownerAdmissionEpoch.load(std::memory_order_acquire) ==
            m_ownerNativeEpoch.load(std::memory_order_acquire) &&
        m_ownerAdmissionEpoch.load(std::memory_order_acquire) !=
            m_ownerRejectedEpoch.load(std::memory_order_acquire) &&
        m_ownerAdmissionSession.load(std::memory_order_acquire) != 0U &&
        m_ownerAdmissionToken.load(std::memory_order_acquire) == token ? token : 0U;
}

uint32_t BleService::NativeAddRpaToken(uint16_t connHandle) const {
    const uint32_t token = NativePairingLeaseToken(connHandle);
    // The Add journal is durable before RequestSecurity publishes this bit.
    // Unlike fresh-pair admission, this remains valid after key persistence
    // makes the new peer appear in inventory. Retirement still revokes it.
    return token != 0U && m_ownerAdmissionAdding.load(std::memory_order_relaxed) &&
        m_ownerAdmissionToken.load(std::memory_order_acquire) == token ? token : 0U;
}

BoardOwnerPairingRuntime::Connection BleService::OwnerConnection() const {
    if (!m_connectionSession.IsActivated()) return {};
    const uint32_t epoch = m_ownerNativeEpoch.load(std::memory_order_acquire);
    if (!m_bleSettings.OwnsConnectionEpoch(epoch)) return {};
    return {m_ownerNativeHandle.load(std::memory_order_acquire), epoch,
            m_connectionSession.ConnectionToken()};
}

void BleService::OnOwnerSecurity(const BleOwnerSecurityEvent& event) {
    if (!OwnerPairingEnabled() || event.connectionEpoch == 0U ||
        event.connHandle != m_ownerNativeHandle.load(std::memory_order_acquire) ||
        event.connectionEpoch != m_ownerNativeEpoch.load(std::memory_order_acquire)) return;
    m_ownerProductEpoch.store(0U, std::memory_order_release);
    m_bleSettings.SetProductAccess(false);
    if (UnlistedPeer(event)) {
        m_ownerForeignEpoch.store(event.connectionEpoch, std::memory_order_release);
        if (event.peerCount == MAX_SAVED_PHONES)
            m_ownerRejectedEpoch.store(event.connectionEpoch, std::memory_order_release);
    }
    if (event.kind == BleOwnerSecurityEvent::Kind::SecurityCompletion &&
        (event.nativeStatus != 0 || !event.encrypted || !event.bonded ||
         !event.inventoryAvailable || event.peerCount < 1U || event.peerCount > MAX_SAVED_PHONES || !event.peerMatches)) {
        m_ownerRejectedEpoch.store(event.connectionEpoch, std::memory_order_release);
        m_ownerNativeFault.store(true, std::memory_order_release);
    }
    BleEvent queued;
    queued.type = BleEventType::OwnerSecurity;
    queued.payload.ownerSecurity = event;
    if (!m_eventQueue.Enqueue(queued)) {
        RevokeOwnerAdmission();
        m_ownerRejectedEpoch.store(event.connectionEpoch, std::memory_order_release);
        m_ownerNativeFault.store(true, std::memory_order_release);
    }
}

void BleService::HandleOwnerSecurity(const BleOwnerSecurityEvent& event) {
    if (event.kind == BleOwnerSecurityEvent::Kind::Identity) {
        if (OwnerPairingEnabled() && event.connectionEpoch != 0U &&
            event.connectionEpoch == m_ownerNativeEpoch.load(std::memory_order_acquire) &&
            event.connHandle == m_ownerNativeHandle.load(std::memory_order_acquire) &&
            event.connectionEpoch != m_ownerRejectedEpoch.load(std::memory_order_acquire))
            m_ownerPeerIdentity = event;
        return;
    }
    if (event.kind == BleOwnerSecurityEvent::Kind::SecurityCompletion && OwnerPairingEnabled()) {
        // The native callback only enqueues facts. Format diagnostics here on
        // the cooperative loop, including a failure already latched at ingress.
        const bool current = event.connectionEpoch != 0U &&
            event.connectionEpoch == m_ownerNativeEpoch.load(std::memory_order_acquire) &&
            event.connHandle == m_ownerNativeHandle.load(std::memory_order_acquire);
        if (current && event.nativeStatus != 0) {
            logger::Warn("ble: phone security failed (status=%ld); pairing is incomplete",
                         static_cast<long>(event.nativeStatus));
        }
        logger::Trace("ble: owner security rc=%ld encrypted=%u bonded=%u inventory=%u peers=%u match=%u current=%u lease=%u",
            static_cast<long>(event.nativeStatus), unsigned(event.encrypted), unsigned(event.bonded),
            unsigned(event.inventoryAvailable), unsigned(event.peerCount), unsigned(event.peerMatches),
            unsigned(current), unsigned(current && NativePairingAllowed(event.connHandle)));
    }
    if (event.kind != BleOwnerSecurityEvent::Kind::SecurityCompletion ||
        !OwnerPairingEnabled() || m_ownerRuntime == nullptr ||
        event.connHandle != m_ownerNativeHandle.load(std::memory_order_acquire) ||
        event.connectionEpoch == 0U ||
        event.connectionEpoch != m_ownerNativeEpoch.load(std::memory_order_acquire) ||
        event.connectionEpoch == m_ownerRejectedEpoch.load(std::memory_order_acquire) ||
        !m_bleSettings.OwnsConnectionEpoch(event.connectionEpoch)) return;
    m_ownerSecurity = event;
    auto& custody = m_ownerRuntime->Custody();
    const auto effect = custody.PendingEffect();
    const auto connection = OwnerConnection();
    if (effect.type == BoardOwnerPairingController::EffectType::RequestSecurity &&
        event.admissionToken == effect.token && event.sessionToken == connection.sessionToken &&
        m_ownerRuntime->ClaimLeaseMatches(connection, effect)) {
        RevokeOwnerAdmission();
        BoardOwnerPairingController::ConnectionSecurity security{
            {event.inventoryAvailable, event.peerCount, event.savedPeers}, event.nativeStatus == 0,
            event.encrypted, event.bonded, event.peerMatches};
        (void)custody.AcknowledgeSecurity(effect.token, effect.session, effect.connection,
                                         effect.epoch, effect.physicalEnd, security);
    }
}

void BleService::PublishOwnerPairingState() {
    if (!OwnerPairingEnabled() || m_ownerRuntime == nullptr ||
        !m_connectionSession.IsActivated()) return;
    std::array<uint8_t, owner_pairing_session_wire::STATUS_BYTES> body{};
    const auto connection = OwnerConnection();
    auto status = m_ownerRuntime->Status(connection);
    const auto effect = m_ownerRuntime->Custody().PendingEffect();
    if (status.state == owner_pairing_session_wire::State::Securing &&
        m_ownerRuntime->PhoneInitiatesSecurity() &&
        m_ownerRuntime->ClaimLeaseMatches(connection, effect) &&
        m_ownerAdmissionToken.load(std::memory_order_acquire) == effect.token &&
        NativePairingAllowed(connection.handle)) {
        status.flags |= owner_pairing_session_wire::NativeBondAllowed;
    }
    if (!OwnerProductAccess()) {
        status.flags &= ~owner_pairing_session_wire::ConnectionOwnerReady;
        if (status.state == owner_pairing_session_wire::State::OwnerReady)
            status.state = owner_pairing_session_wire::State::OwnedOther;
    }
    const auto nativeEpoch = m_ownerNativeEpoch.load(std::memory_order_acquire);
    if (nativeEpoch == 0U || m_ownerForeignEpoch.load(std::memory_order_acquire) != nativeEpoch)
        status.flags &= ~owner_pairing_session_wire::CanAddPhone;
    if (nativeEpoch != 0U && status.state == owner_pairing_session_wire::State::OwnedOther &&
        m_ownerForeignEpoch.load(std::memory_order_acquire) ==
            nativeEpoch)
        status.reason = owner_pairing_session_wire::Reason::WrongConnection;
    if (!owner_pairing_session_wire::EncodeStatus(status, body.data(), body.size())) return;
    if (!m_ownerStateValid || m_ownerStateBytes != body) {
        if (++m_ownerStateRevision == 0U) ++m_ownerStateRevision;
        m_ownerStateBytes = body;
        m_ownerStateValid = true;
        logger::Trace("ble: owner state=%u reason=%u flags=%u phase=%u operation=%u",
            unsigned(status.state), unsigned(status.reason), unsigned(status.flags),
            unsigned(m_ownerRuntime->Custody().GetPhase()),
            unsigned(m_ownerRuntime->Custody().GetOperation()));
    }
    BoardIdentity identity{};
    if (m_ownerIdentity != nullptr) (void)m_ownerIdentity->GetDurableIdentity(identity);
    (void)m_connectionSession.PublishState(owner_pairing_session_wire::STATUS_CODE,
        m_ownerStateRevision, body.data(), body.size());
    (void)m_connectionSession.PublishState(owner_pairing_session_wire::IDENTITY_LOW_CODE,
        m_ownerStateRevision, identity.bytes.data(), 8U);
    (void)m_connectionSession.PublishState(owner_pairing_session_wire::IDENTITY_HIGH_CODE,
        m_ownerStateRevision, identity.bytes.data() + 8U, 8U);
}
#endif

void BleService::UpdateOwnerPairing() {
#if defined(LITCH_BLE)
    if (!OwnerPairingEnabled()) return;
    if (m_ownerRuntime == nullptr || m_ownerIdentity == nullptr) {
        RevokeOwnerAdmission();
        m_connectionSession.SetProductAccess(false);
        return;
    }
    if (!m_ownerBooted && m_initialized) (void)InitOwnerPairingNative();
    const auto connection = OwnerConnection();
    const auto nativeEpoch = m_ownerNativeEpoch.load(std::memory_order_acquire);
    const bool firstNativeObservation = connection.epoch != 0U &&
        (m_ownerObservedConnection.epoch != connection.epoch ||
         m_ownerObservedConnection.handle != connection.handle);
    if (m_ownerObservedConnection.epoch != 0U &&
        (m_ownerObservedConnection.epoch != nativeEpoch ||
         m_ownerObservedConnection.handle != m_ownerNativeHandle.load(std::memory_order_acquire))) {
        m_ownerRuntime->Disconnected(m_ownerObservedConnection);
        if (m_ownerSecurity.connectionEpoch == m_ownerObservedConnection.epoch)
            m_ownerSecurity = {};
        m_ownerObservedConnection = {};
    } else if (m_ownerObservedConnection.sessionToken != 0U &&
               connection.sessionToken != m_ownerObservedConnection.sessionToken) {
        m_ownerRuntime->Retire(m_ownerObservedConnection);
        m_ownerObservedConnection = {};
        RevokeOwnerAdmission();
    }
    if (connection.epoch != 0U) {
        // Native lookup can be unavailable during the connect callback. The
        // first activated observation gets one cooperative reread; subsequent
        // updates and app State queries never reload the saved-phone store.
        if (firstNativeObservation &&
            m_ownerRejectedEpoch.load(std::memory_order_acquire) != connection.epoch &&
            (m_ownerPeerIdentity.connectionEpoch != connection.epoch ||
             m_ownerPeerIdentity.connHandle != connection.handle ||
             !m_ownerPeerIdentity.inventoryAvailable ||
             !m_ownerPeerIdentity.peerIdentityAvailable))
            ObserveOwnerNativePeer(connection.handle);
        m_ownerObservedConnection = connection;
    }
#if defined(ARDUINO)
    if (m_ownerPersistence != nullptr)
        m_ownerRuntime->UpdateDurableState(m_ownerPersistence->DurableOwnerRecord(),
                                           m_ownerIdentity->IsDurable());
#endif
    m_ownerRuntime->Poll();
    auto& custody = m_ownerRuntime->Custody();
    const bool foreignOwner = nativeEpoch != 0U &&
        m_ownerForeignEpoch.load(std::memory_order_acquire) == nativeEpoch &&
        custody.GetOwnership() == BoardOwnerPairingController::Ownership::Owned &&
        custody.GetOperation() == BoardOwnerPairingController::Operation::None;
    if (!foreignOwner && connection.epoch != 0U &&
        m_ownerRejectedEpoch.load(std::memory_order_acquire) != connection.epoch &&
        m_ownerPeerIdentity.connectionEpoch == connection.epoch &&
        m_ownerPeerIdentity.connHandle == connection.handle &&
        m_ownerPeerIdentity.inventoryAvailable && m_ownerPeerIdentity.peerCount >= 1U &&
        m_ownerPeerIdentity.peerCount <= MAX_SAVED_PHONES &&
        m_ownerPeerIdentity.peerIdentityAvailable && m_ownerPeerIdentity.peerMatches &&
        custody.GetOwnership() == BoardOwnerPairingController::Ownership::Owned &&
        m_ownerQuietSecurityEpoch != connection.epoch &&
        (m_ownerSecurity.connectionEpoch != connection.epoch || !m_ownerSecurity.encrypted) &&
        custody.GetOperation() == BoardOwnerPairingController::Operation::None) {
        m_ownerQuietSecurityEpoch = connection.epoch;
        if (!StartOwnerNativeSecurity(connection.handle)) {
            m_ownerRejectedEpoch.store(connection.epoch, std::memory_order_release);
            m_ownerNativeFault.store(true, std::memory_order_release);
        }
    }
    if (m_ownerNativeFault.exchange(false, std::memory_order_acq_rel) && !foreignOwner) {
        logger::Warn("ble: owner native fault phase=%u operation=%u lease=%u",
            unsigned(custody.GetPhase()), unsigned(custody.GetOperation()),
            unsigned(NativePairingAllowed(connection.handle)));
        RevokeOwnerAdmission();
        m_ownerRuntime->Retire(connection);
        const uint16_t handle = m_ownerNativeHandle.load(std::memory_order_acquire);
        if (m_server != nullptr && handle != ble_internal::INVALID_CONN_HANDLE &&
            m_ownerRejectedEpoch.load(std::memory_order_acquire) ==
                m_ownerNativeEpoch.load(std::memory_order_acquire)) m_server->disconnect(handle);
    }
    if (m_ownerRuntime->NeedsDisconnect() && m_server != nullptr) {
        RevokeOwnerAdmission();
        m_server->disconnect(m_ownerRuntime->DisconnectTarget().handle);
    }
    const auto effect = custody.PendingEffect();
    using Effect = BoardOwnerPairingController::EffectType;
    if (effect.type == Effect::RequestSecurity && effect.token != m_ownerStartedSecurityToken) {
        if (m_ownerRuntime->ClaimLeaseMatches(connection, effect)) {
            m_ownerAdmissionHandle.store(connection.handle, std::memory_order_relaxed);
            m_ownerAdmissionEpoch.store(connection.epoch, std::memory_order_relaxed);
            m_ownerAdmissionSession.store(connection.sessionToken, std::memory_order_relaxed);
            m_ownerAdmissionAdding.store(custody.IsAdding() && custody.AddBaseline().Valid(),
                                        std::memory_order_relaxed);
            m_ownerAdmissionToken.store(effect.token, std::memory_order_release);
            m_ownerStartedSecurityToken = effect.token;
            if (!m_ownerRuntime->PhoneInitiatesSecurity()) {
                int nativeResult = 0;
                const bool started = StartOwnerNativeSecurity(connection.handle, &nativeResult);
                logger::Trace("ble: owner claim security start rc=%d", nativeResult);
                if (!started) {
                    logger::Warn("ble: phone security could not start (status=%d)", nativeResult);
                    BleOwnerSecurityEvent failed{};
                    failed.connHandle = connection.handle; failed.connectionEpoch = connection.epoch;
                    failed.admissionToken = effect.token; failed.sessionToken = connection.sessionToken;
                    failed.nativeStatus = nativeResult;
                    HandleOwnerSecurity(failed);
                }
            }
        }
    } else if (effect.type == Effect::QuiesceAndRejectWork || effect.type == Effect::DeleteAllBonds ||
               effect.type == Effect::RollbackAddedPeers) {
        RevokeOwnerAdmission();
        m_ownerQuiescing.store(true, std::memory_order_release);
        m_consoleInput.Clear(); m_consoleOutputLen = 0U;
        ClearPendingConnectionControlAction();
        // Once submitted, only the worker owns GAP/controller work. Repeating
        // a main-loop stop while its privacy commands run would race the reset.
#if defined(ARDUINO)
        if (m_ownerClearToken != 0U) {
            if ((effect.type == Effect::DeleteAllBonds || effect.type == Effect::RollbackAddedPeers) &&
                m_ownerClearToken == effect.token &&
                m_ownerPersistence != nullptr) {
                using Outcome = BoardOwnerPersistence::Outcome;
                const bool rollback = effect.type == Effect::RollbackAddedPeers;
                const auto result = rollback ? m_ownerPersistence->GetNativeRollbackOutcome()
                                             : m_ownerPersistence->GetNativeClearOutcome();
                if ((result == Outcome::Saved || result == Outcome::Failed) &&
                    (rollback ? m_ownerPersistence->ReleaseNativeRollback()
                              : m_ownerPersistence->ReleaseNativeClear())) {
                    m_ownerClearToken = 0U;
                    (void)custody.AcknowledgeBondDelete(effect.token, result == Outcome::Saved);
                }
            }
        } else
#endif
        if (!QuiesceOwnerNativeGap()) {
            if (effect.type == Effect::QuiesceAndRejectWork)
                (void)custody.AcknowledgeQuiesce(effect.token, false);
            else (void)custody.AcknowledgeBondDelete(effect.token, false);
        }
        // Keep accepted domain work with its owner. Never delete keys while an
        // update or native link can still write peer state.
        else if (!IsOtaInProgress() && m_server != nullptr) {
            DisconnectOwnerNativeLinks();
            if (m_ownerNativeHandle.load(std::memory_order_acquire) == ble_internal::INVALID_CONN_HANDLE &&
                OwnerNativeLinksQuiescent() && OwnerNativeGapQuiescent()) {
                if (effect.type == Effect::QuiesceAndRejectWork) {
                    (void)custody.AcknowledgeQuiesce(effect.token, true);
                }
#if defined(ARDUINO)
                else if (m_ownerPersistence != nullptr) {
                    const bool admitted = effect.type == Effect::RollbackAddedPeers
                        ? m_ownerPersistence->RequestNativeRollback({this, &RollbackOwnerNativeStore})
                        : m_ownerPersistence->RequestNativeClear({this, &ClearOwnerNativeStore});
                    if (admitted)
                        m_ownerClearToken = effect.token;
                }
#endif
            }
        }
    } else if (effect.type == Effect::PairSucceeded || effect.type == Effect::UnpairSucceeded) {
        (void)custody.AcknowledgeSuccess(effect.token);
    } else if (effect.type == Effect::Disconnect && m_server != nullptr) {
        RevokeOwnerAdmission();
        if (m_ownerNativeHandle.load(std::memory_order_acquire) == effect.connection)
            m_server->disconnect(effect.connection);
        else (void)custody.AcknowledgeDisconnect(effect.token, true);
    }
    if (m_ownerRejectedEpoch.load(std::memory_order_acquire) != connection.epoch &&
        m_ownerSecurity.connectionEpoch == connection.epoch && connection.epoch != 0U &&
        m_ownerSecurity.connHandle == connection.handle && m_ownerSecurity.nativeStatus == 0) {
        BoardOwnerPairingController::ConnectionSecurity security{
            {m_ownerSecurity.inventoryAvailable, m_ownerSecurity.peerCount, m_ownerSecurity.savedPeers}, true,
            m_ownerSecurity.encrypted, m_ownerSecurity.bonded, m_ownerSecurity.peerMatches};
        if (security.IsSavedOwner()) (void)m_ownerRuntime->RegisterOwnerConnection(connection, security);
    }
    const bool allowed = m_ownerRuntime->OwnerReady(connection) &&
        m_ownerRejectedEpoch.load(std::memory_order_acquire) != connection.epoch &&
        m_ownerSecurity.connectionEpoch == connection.epoch && m_ownerSecurity.nativeStatus == 0 &&
        m_ownerSecurity.connHandle == connection.handle && m_ownerSecurity.inventoryAvailable &&
        m_ownerSecurity.peerCount >= 1U && m_ownerSecurity.peerCount <= MAX_SAVED_PHONES &&
        m_ownerSecurity.encrypted && m_ownerSecurity.bonded &&
        m_ownerSecurity.peerMatches;
    m_ownerProductEpoch.store(allowed ? connection.epoch : 0U, std::memory_order_release);
    m_bleSettings.SetProductAccess(allowed, allowed ? connection.handle : 0xffffU);
    m_connectionSession.SetProductAccess(allowed);
    if (custody.Pairable() || (custody.GetOwnership() == BoardOwnerPairingController::Ownership::Owned &&
        custody.GetOperation() == BoardOwnerPairingController::Operation::None &&
        custody.GetPhase() == BoardOwnerPairingController::Phase::Idle))
        m_ownerQuiescing.store(false, std::memory_order_release);
    PublishOwnerPairingState();
#endif
}

} // namespace modules
