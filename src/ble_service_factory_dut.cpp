#include "modules/ble_service.hpp"

#if defined(LITCH_BLE) && defined(LITCH_HW_VARIANT_S3)
#if defined(ARDUINO)
#include <Arduino.h>
#endif
#include <NimBLEDevice.h>
#include <nimble/porting/nimble/include/nimble/nimble_port.h>

#include "firmware/settings_factory_task_storage.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/ble_service_internal.hpp"
#include "modules/factory_radio_role_gate.hpp"
#include "runtime/progress_watchdog.hpp"

namespace modules {
namespace {

// SDK client deletion and foreground cancellation must exclude each other's
// check/use window. Try once: the input owner and host task never spin or wait.
class FactoryClientAccess final {
  public:
    explicit FactoryClientAccess(std::atomic_flag& flag)
        : m_flag(flag), m_held(!flag.test_and_set(std::memory_order_acquire)) {}
    ~FactoryClientAccess() { if (m_held) m_flag.clear(std::memory_order_release); }
    explicit operator bool() const { return m_held; }
  private:
    std::atomic_flag& m_flag;
    bool m_held;
};

uint64_t AddressIdentity(const NimBLEAddress& address) {
    // NimBLE's native address bytes are little-endian. Never depend on text
    // formatting, rotating advertisement identifiers, or discovery order.
    const auto* bytes = address.getBase()->val;
    uint64_t identity = 0U;
    for (uint8_t index = 0U; index < 6U; ++index) {
        identity |= static_cast<uint64_t>(bytes[index]) << (8U * index);
    }
    return identity;
}

class FactoryPeerScan final : public NimBLEScanCallbacks {
  public:
    FactoryPeerScan(const FactoryRadioPeerAttempts& attempts, uint64_t self, bool lowerOnly)
        : m_attempts(attempts), m_self(self), m_lowerOnly(lowerOnly) {}

    void onResult(const NimBLEAdvertisedDevice* advertised) override {
        if (!advertised->isConnectable() || !advertised->isAdvertisingService(m_service)) return;
        const auto address = advertised->getAddress();
        const uint64_t identity = AddressIdentity(address);
        if (identity >= m_identity || !FactoryRadioRoleGate::PeerEligible(m_self, identity, m_lowerOnly) ||
            !m_attempts.CanTry(identity, fxe::Millis())) return;
        m_address = address;
        m_identity = identity;
    }

    bool Found() const { return m_identity != UINT64_MAX; }
    const NimBLEAddress& Address() const { return m_address; }
    uint64_t Identity() const { return m_identity; }

  private:
    const FactoryRadioPeerAttempts& m_attempts;
    const uint64_t m_self;
    const bool m_lowerOnly;
    const NimBLEUUID m_service{ble_internal::FACTORY_RADIO_SERVICE_UUID};
    NimBLEAddress m_address{};
    uint64_t m_identity = UINT64_MAX;
};

bool Exchange(NimBLERemoteCharacteristic& characteristic,
              const FactoryRadioFrame& outbound, FactoryRadioFrame& inbound) {
    FactoryRadioWireFrame wire{};
    if (!EncodeFactoryRadioFrame(outbound, wire) ||
        !characteristic.writeValue(wire.data(), wire.size(), true)) {
        return false;
    }
    const NimBLEAttValue value = characteristic.readValue();
    return DecodeFactoryRadioFrame(value.data(), value.size(), inbound);
}

bool Prove(NimBLEClient& client, const NimBLEAddress& peer,
           FactoryBleProof& proof, const std::atomic<bool>& cancelled) {
    // A write-with-response has an opcode and handle in the default 23-byte ATT MTU.
    static_assert(FACTORY_RADIO_WIRE_FRAME_SIZE <= 20U);
    client.setConnectTimeout(900U);
    if (cancelled.load(std::memory_order_acquire) || !client.connect(peer, true, false, false)) {
        return false;
    }
    auto* service = client.getService(ble_internal::FACTORY_RADIO_SERVICE_UUID);
    auto* characteristic = service == nullptr ? nullptr :
        service->getCharacteristic(ble_internal::FACTORY_RADIO_CHARACTERISTIC_UUID);
    FactoryRadioDutHandshake handshake;
    FactoryRadioFrame hello, challenge, response, terminal;
    if (characteristic == nullptr || cancelled.load(std::memory_order_acquire) ||
        !handshake.Start(FactoryRadioChannel::Ble, proof.sessionId, proof.dutNonce, hello) ||
        !Exchange(*characteristic, hello, challenge) ||
        !handshake.Handle(challenge, response) ||
        cancelled.load(std::memory_order_acquire) ||
        !Exchange(*characteristic, response, terminal) ||
        !handshake.Handle(terminal, response) || !handshake.IsVerified()) {
        return false;
    }
    proof.verifierNonce = handshake.GetPeerNonce();
    return !cancelled.load(std::memory_order_acquire);
}

} // namespace

uint64_t BleService::FactoryRadioIdentity() const {
    return AddressIdentity(NimBLEDevice::getAddress());
}

void BleService::ExecuteFactoryBleDut() {
    m_factoryDutVerified = false;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan == nullptr || scan->isScanning()) {
        return;
    }
    FactoryPeerScan peer(m_factoryPeerAttempts, FactoryRadioIdentity(), m_factoryDutLowerOnly);
    // The factory UUID is in the primary advertisement. With passive callbacks
    // and no retained results, NimBLE erases each report after onResult.
    scan->setActiveScan(false);
    scan->setMaxResults(0U);
    scan->setScanCallbacks(&peer);
    runtime::NoteProgress(1U, runtime::ProgressStage::FactoryBleScan);
    // Only this worker owns the scanner. The host completes its callbacks
    // before releasing getResults; detach before proceeding to GATT.
    (void)scan->getResults(150U, false);
    scan->setScanCallbacks(nullptr);

    m_factoryDutCandidate.store(peer.Found(), std::memory_order_release);
    if (peer.Found() && !IsConnected() &&
        !m_factoryVerifierProofPending.load(std::memory_order_acquire) &&
        !m_factoryDutCancel.load(std::memory_order_acquire)) {
        m_factoryVerifierRuntimeAllowed.store(false, std::memory_order_release);
        runtime::NoteProgress(1U, runtime::ProgressStage::FactoryBleGatt);
        m_factoryDutVerified = Prove(*m_factoryDutClient, peer.Address(), m_factoryDutProof,
                                    m_factoryDutCancel);
        (void)m_factoryDutClient->disconnect();
        if (!m_factoryDutVerified) {
            m_factoryPeerAttempts.Failed(peer.Identity(), fxe::Millis());
            m_factoryDutCandidate.store(false, std::memory_order_release);
        }
    }
    runtime::NoteProgress(1U, runtime::ProgressStage::Idle);
}

void BleService::FactoryBleDutTask(void* context) {
    auto& self = *static_cast<BleService*>(context);
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (self.m_factoryDutJobState.load(std::memory_order_acquire) ==
            FactoryDutJobState::CleanupSubmit) {
            // The SDK queue put may wait when full. Only this existing worker
            // submits; the cooperative input/render owner remains nonblocking.
            ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &self.m_factoryDutCleanupEvent);
            self.m_factoryDutJobState.store(FactoryDutJobState::CleanupQueued,
                                            std::memory_order_release);
            continue;
        }
        self.ExecuteFactoryBleDut();
        // Release publishes both result fields; after this store, the worker
        // touches no BLE stack/record data until the next explicit request.
        self.m_factoryDutJobState.store(FactoryDutJobState::Complete, std::memory_order_release);
    }
}

FactoryBleDutResult BleService::PollFactoryBleDut(
    uint32_t sessionId, uint32_t dutNonce, bool lowerAddressOnly,
    FactoryBleProof& outProof) {
    const auto state = m_factoryDutJobState.load(std::memory_order_acquire);
    if (state == FactoryDutJobState::Running) {
        constexpr uint32_t OPERATION_TIMEOUT_MS = 5000U;
        if (static_cast<uint32_t>(fxe::Millis() - m_factoryDutStartedMs) >=
            OPERATION_TIMEOUT_MS) {
            FactoryClientAccess access(m_factoryDutClientAccess);
            if (!access || m_factoryDutJobState.load(std::memory_order_acquire) != FactoryDutJobState::Running ||
                m_factoryDutProof.sessionId != sessionId || m_factoryDutProof.dutNonce != dutNonce)
                return FactoryBleDutResult::Pending;
            m_factoryDutCancel.store(true, std::memory_order_release);
            // SDK disconnect/cancel wakes pending GATT waits. The client stays
            // allocated until the worker has relinquished it.
            (void)m_factoryDutClient->cancelConnect();
            (void)m_factoryDutClient->disconnect();
        }
        return FactoryBleDutResult::Pending;
    }
    if (state != FactoryDutJobState::Idle) {
        if (!ReleaseFactoryBleDutResources()) return FactoryBleDutResult::Pending;
        const bool accepted = m_factoryDutVerified &&
            !m_factoryDutCancel.load(std::memory_order_acquire) &&
            !m_factoryVerifierProofPending.load(std::memory_order_acquire) &&
            m_factoryDutProof.sessionId == sessionId &&
            m_factoryDutProof.dutNonce == dutNonce;
        if (accepted) {
            outProof = m_factoryDutProof;
        }
        m_factoryDutJobState.store(FactoryDutJobState::Idle, std::memory_order_release);
        return accepted ? FactoryBleDutResult::Verified : FactoryBleDutResult::NoPeer;
    }
    if (m_factoryDutTaskStorage == nullptr || !m_initialized || !m_enabled || IsConnected() ||
        m_factoryVerifierProofPending.load(std::memory_order_acquire)) {
        m_factoryDutCandidate.store(false, std::memory_order_release);
        return FactoryBleDutResult::NoPeer;
    }
    FactoryClientAccess access(m_factoryDutClientAccess);
    if (!access) return FactoryBleDutResult::Pending;
    if (m_factoryDutClient == nullptr) {
        m_factoryDutClient = NimBLEDevice::createClient();
        if (m_factoryDutClient == nullptr) {
            return FactoryBleDutResult::NoPeer;
        }
        m_factoryDutClient->setSelfDelete(false, false);
    }
    if (m_factoryDutClient->isConnected()) {
        return FactoryBleDutResult::NoPeer;
    }
    if (m_factoryDutTask == nullptr) {
        if (m_factoryDutTaskStorage->TryClaim()) {
            m_factoryDutTask = xTaskCreateStaticPinnedToCore(
                &FactoryBleDutTask, "factory_ble", firmware::SettingsFactoryTaskStorage::FACTORY_STACK_BYTES,
                this, 1U, m_factoryDutTaskStorage->stack, &m_factoryDutTaskStorage->control, xPortGetCoreID());
            if (m_factoryDutTask == nullptr) m_factoryDutTaskStorage->Release();
        }
        if (m_factoryDutTask == nullptr) {
            // A never-used client has no SDK callbacks or operation owner.
            if (NimBLEDevice::deleteClient(m_factoryDutClient)) m_factoryDutClient = nullptr;
            return FactoryBleDutResult::NoPeer;
        }
    }
    m_factoryDutStartedMs = fxe::Millis();
    m_factoryDutCandidate.store(false, std::memory_order_release);
    m_factoryDutProof = FactoryBleProof{sessionId, dutNonce, 0U};
    m_factoryDutLowerOnly = lowerAddressOnly;
    m_factoryDutCancel.store(false, std::memory_order_release);
    m_factoryDutJobState.store(FactoryDutJobState::Running, std::memory_order_release);
    xTaskNotifyGive(static_cast<TaskHandle_t>(m_factoryDutTask));
    return FactoryBleDutResult::Pending;
}

bool BleService::HasFactoryBleCandidate() const {
    return m_factoryDutCandidate.load(std::memory_order_acquire);
}

bool BleService::IsFactoryBleDutRunning() const {
    const auto state = m_factoryDutJobState.load(std::memory_order_acquire);
    return state == FactoryDutJobState::Running || state == FactoryDutJobState::CleanupSubmit ||
           state == FactoryDutJobState::CleanupQueued;
}

void BleService::FactoryBleDutCleanup(ble_npl_event* event) {
    auto& self = *static_cast<BleService*>(ble_npl_event_get_arg(event));
    FactoryClientAccess access(self.m_factoryDutClientAccess);
    if (!access) {
        self.m_factoryDutCleanupResult.store(FactoryDutCleanupResult::Retry, std::memory_order_release);
        return;
    }
    // Run on the existing SDK host task, serialized after its callback tail.
    // isConnected() alone cannot establish that safety from another task.
    if (self.m_factoryDutClient != nullptr) {
        if (self.m_factoryDutClient->isConnected()) {
            (void)self.m_factoryDutClient->disconnect();
            self.m_factoryDutCleanupResult.store(FactoryDutCleanupResult::Retry,
                                                 std::memory_order_release);
            return;
        }
        runtime::NoteProgress(1U, runtime::ProgressStage::FactoryBleClientRelease);
        const bool released = NimBLEDevice::deleteClient(self.m_factoryDutClient);
        runtime::NoteProgress(1U, runtime::ProgressStage::Idle);
        if (!released) {
            self.m_factoryDutCleanupResult.store(FactoryDutCleanupResult::Retry,
                                                 std::memory_order_release);
            return;
        }
        self.m_factoryDutClient = nullptr;
    }
    self.m_factoryDutCleanupResult.store(FactoryDutCleanupResult::Released,
                                         std::memory_order_release);
}

bool BleService::ReleaseFactoryBleDutResources() {
    FactoryClientAccess access(m_factoryDutClientAccess);
    if (!access) return false;
    const auto state = m_factoryDutJobState.load(std::memory_order_acquire);
    if (state == FactoryDutJobState::Running || state == FactoryDutJobState::CleanupSubmit) return false;
    if (m_factoryDutTask == nullptr) {
        // Task creation failed before this client could be used by the SDK.
        if (m_factoryDutClient != nullptr) {
            if (!NimBLEDevice::deleteClient(m_factoryDutClient)) return false;
            m_factoryDutClient = nullptr;
        }
        return true;
    }
    const auto task = static_cast<TaskHandle_t>(m_factoryDutTask);
    // Only the lifecycle caller can notify this idle worker. A same-core,
    // blocked task is freed synchronously by the pinned IDF4.4.7 SDK; foreign
    // callers retain ownership until the ordinary loop resumes on its core.
    if (eTaskGetState(task) != eBlocked || xTaskGetAffinity(task) != xPortGetCoreID()) return false;
    if (state == FactoryDutJobState::CleanupQueued) {
        const auto result = m_factoryDutCleanupResult.load(std::memory_order_acquire);
        if (result == FactoryDutCleanupResult::Pending) return false;
        if (result == FactoryDutCleanupResult::Retry) {
            m_factoryDutJobState.store(FactoryDutJobState::Complete, std::memory_order_release);
            return false;
        }
        runtime::NoteProgress(1U, runtime::ProgressStage::FactoryBleTaskRelease);
        vTaskDelete(task);
        m_factoryDutTaskStorage->Release();
        runtime::NoteProgress(1U, runtime::ProgressStage::Idle);
        m_factoryDutTask = nullptr;
        m_factoryDutCandidate.store(false, std::memory_order_release);
        return true;
    }
    m_factoryDutCleanupResult.store(FactoryDutCleanupResult::Pending, std::memory_order_release);
    ble_npl_event_init(&m_factoryDutCleanupEvent, &FactoryBleDutCleanup, this);
    m_factoryDutJobState.store(FactoryDutJobState::CleanupSubmit, std::memory_order_release);
    xTaskNotifyGive(task);
    return false;
}

bool BleService::QuiesceFactoryBleDut() {
    m_factoryDutCancel.store(true, std::memory_order_release);
    {
        FactoryClientAccess access(m_factoryDutClientAccess);
        if (!access) return false;
        if (m_factoryDutJobState.load(std::memory_order_acquire) == FactoryDutJobState::Running) {
            if (m_factoryDutClient != nullptr) {
                (void)m_factoryDutClient->cancelConnect();
                (void)m_factoryDutClient->disconnect();
            }
            return false;
        }
    }
    if (!ReleaseFactoryBleDutResources()) return false;
    m_factoryDutCandidate.store(false, std::memory_order_release);
    m_factoryDutJobState.store(FactoryDutJobState::Idle, std::memory_order_release);
    return true;
}

} // namespace modules
#else
namespace modules {
FactoryBleDutResult BleService::PollFactoryBleDut(uint32_t, uint32_t, bool,
                                                FactoryBleProof&) {
    return FactoryBleDutResult::NoPeer;
}
bool BleService::IsFactoryBleDutRunning() const { return false; }
bool BleService::HasFactoryBleCandidate() const { return false; }
bool BleService::QuiesceFactoryBleDut() { return true; }
uint64_t BleService::FactoryRadioIdentity() const { return 0U; }
} // namespace modules
#endif
