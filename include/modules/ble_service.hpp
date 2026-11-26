#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ArduinoJson.h>

#include "foxie_embedded/fixed_string.hpp"
#include "foxie_embedded/setting_descriptor.hpp"
#include "modules/ble_console_input_buffer.hpp"
#include "modules/ble_identify_session.hpp"
#include "modules/board_identity_persistence.hpp"
#include "modules/board_owner_pairing_feature.hpp"
#include "modules/board_owner_pairing_runtime.hpp"
#include "modules/connection_session.hpp"
#include "modules/connection_object_benchmark.hpp"
#include "modules/ble_device_capabilities.hpp"
#include "modules/ble_event_queue.hpp"
#include "modules/ble_runtime_health_protocol.hpp"
#include "modules/firmware_digest_provider.hpp"
#include "modules/factory_radio_protocol.hpp"
#include "modules/factory_radio_role_gate.hpp"
#include "modules/ota_platform_adapter.hpp"
#include "modules/ota_led_presentation.hpp"
#include "modules/physical_input_state_wire_codec.hpp"
#if defined(LITCH_BLE_OTA)
#include "modules/ble_ota_chunk_queue.hpp"
#include "modules/ble_ota_transaction.hpp"
#include "modules/litchcraft_frame_store_receiver.hpp"
#endif
#include "modules/ble_settings.hpp"
#include "modules/button_state_wire_codec.hpp"
#include "modules/settings_store.hpp"
#include "modules/support_snapshot.hpp"

#if defined(LITCH_BLE) && defined(LITCH_HW_VARIANT_S3)
#include <nimble/nimble/include/nimble/nimble_npl.h>
#endif

#if defined(LITCH_BLE)
class NimBLEAdvertising;
class NimBLEClient;
class NimBLECharacteristic;
class NimBLECharacteristicCallbacks;
class NimBLEServer;
class NimBLEService;
#endif

namespace runtime {
class BootGuard;
}
namespace firmware { class BoardOwnerPersistenceRuntime; struct SettingsFactoryTaskStorage; }

namespace modules {

class ControlMutationService;
class GamePersistenceCoordinator;

struct BleLinkTestResult {
    uint16_t packetSize = 0;
    uint16_t packetCount = 0;
    uint32_t elapsedMs = 0;
    uint32_t bytesAttempted = 0;
    uint32_t bytesSent = 0;
    uint16_t okCount = 0;
    uint16_t failCount = 0;
};

enum class FactoryBleDutResult : uint8_t { Pending, NoPeer, Verified };

struct FactoryBleProof {
    uint32_t sessionId = 0U;
    uint32_t dutNonce = 0U;
    uint32_t verifierNonce = 0U;
};

class BleService {
  public:
    using SupportCaptureCallback = size_t (*)(void*, bool, uint8_t, uint32_t, uint8_t*, size_t);
    void SetSupportCapture(SupportCaptureCallback capture, void* context) {
        m_supportCapture = capture; m_supportCaptureContext = context;
    }
    FirmwareDigestState CachedRunningFirmwareDigest(uint8_t* digest, size_t capacity) const;
    using BeforeBlockingDiagnosticCallback = void (*)(void* context);
    using SettingsMutationCallback = BleSettingsMutationCallback;
    using RuntimeHealthRecheckCallback = bool (*)(void* context);
    using ConnectionGameHistoryIngressCallback = bool (*)(
        void*, const uint8_t*, size_t, uint32_t, bool);
    using ConnectionGameHistoryPeekCallback = bool (*)(
        void*, uint8_t*, size_t, size_t&, uint32_t&, uint32_t);
    using ConnectionGameHistoryAcceptCallback = void (*)(
        void*, uint32_t, uint32_t);
#if defined(LITCH_BLE_OTA)
    using LitchcraftFrameIngressCallback = void (*)(
        void*, BleOtaChunkQueue&,
        litchcraft_frame_store::StoreProjection, uint32_t, uint16_t);
    using LitchcraftFrameResponsePeekCallback = bool (*)(
        void*, uint8_t*, size_t, size_t&);
    using LitchcraftFrameResponseAcceptCallback = void (*)(void*);
    using LitchcraftFrameReceiptReplayCallback = bool (*)(
        void*, uint16_t, uint32_t);
    using LitchcraftFramePriorityCallback = void (*)(
        void*, const uint8_t*, size_t, uint32_t);
    using OtaStoreSaveRequestCallback = void (*)(void*);
    using OtaStoreSavePendingCallback = bool (*)(void*);
    using OtaStoreBeginBusyCallback = bool (*)(void*);
#endif

    explicit BleService(hardware::RmtFlashArbiter* flashArbiter = nullptr)
#if defined(LITCH_BLE)
        : m_nativeStoreFlashArbiter(flashArbiter),
          m_firmwareDigestProvider(flashArbiter)
#endif
    {
#if !defined(LITCH_BLE)
        (void)flashArbiter;
#endif
    }
    BleService(const BleService&) = delete;
    BleService& operator=(const BleService&) = delete;
    BleService(BleService&&) = delete;
    BleService& operator=(BleService&&) = delete;

    void Begin(const SettingsStore& settings);
    bool ConfigureOwnerPairing(BoardOwnerPairingRuntime& runtime,
        firmware::BoardOwnerPersistenceRuntime& persistence,
        BoardIdentityPersistence& identity);
    void UpdateOwnerPairing();
    // Rechecks the active owner/session at execution, not just Console ingress.
    bool RequestOwnerSettingsClear();
    uint64_t ConsoleInputGeneration() const;
#if defined(LITCH_BLE)
    bool BeginNativeStoreFlashAccess();
    void EndNativeStoreFlashAccess();
#endif
    // Recovery starts only the existing updater transport, without loading or
    // exposing domain settings. Leaving recovery requires an ordinary restart.
    void BeginRecovery();
    void SetLitchcraftCoordinator(GamePersistenceCoordinator* game) {
#if defined(LITCH_BLE)
        if (m_recoveryOnly) return;
#endif
        m_litchcraftGame = game;
    }
    void SetLitchcraftRuntimeEligible(bool eligible) { m_litchcraftEligible=eligible; }
    void SetConnectionSessionBootNonce(uint64_t bootNonce) {
        m_connectionSessionBootNonce = bootNonce;
    }
    [[nodiscard]] uint64_t ConnectionSessionBootNonce() const {
        return m_connectionSessionBootNonce;
    }
    bool ReadOtaAdmissionEvidence(OtaAdmissionEvidence& evidence) const {
#if defined(LITCH_BLE_OTA)
        return m_otaPlatformAdapter != nullptr &&
               m_otaPlatformAdapter->ReadAdmissionEvidence(evidence);
#else
        (void)evidence;
        return false;
#endif
    }
    void SetConnectionGameHistoryIngressCallback(
        ConnectionGameHistoryIngressCallback callback, void* context) {
        m_connectionGameHistoryIngressCallback = callback;
        m_connectionGameHistoryIngressContext = context;
    }
    void SetConnectionGameHistoryOutputCallbacks(
        ConnectionGameHistoryPeekCallback peek,
        ConnectionGameHistoryAcceptCallback accept, void* context) {
        m_connectionGameHistoryPeekCallback = peek;
        m_connectionGameHistoryAcceptCallback = accept;
        m_connectionGameHistoryOutputContext = context;
    }
#if defined(LITCH_BLE_OTA)
    void SetLitchcraftFrameIngressCallbacks(
        LitchcraftFrameIngressCallback ingress,
        LitchcraftFramePriorityCallback priority,
        LitchcraftFrameResponsePeekCallback peek,
        LitchcraftFrameResponseAcceptCallback accept,
        LitchcraftFrameReceiptReplayCallback replay,
        void* context) {
        m_litchcraftFrameIngressCallback = ingress;
        m_litchcraftFramePriorityCallback = priority;
        m_litchcraftFrameResponsePeekCallback = peek;
        m_litchcraftFrameResponseAcceptCallback = accept;
        m_litchcraftFrameReceiptReplayCallback = replay;
        m_litchcraftFrameIngressContext = context;
    }
    void SetOtaStorePersistenceCallbacks(
        OtaStoreSaveRequestCallback requestSave,
        OtaStoreSavePendingCallback savePending, void* context,
        OtaStoreBeginBusyCallback beginBusy = nullptr) {
        m_otaStoreSaveRequest = requestSave;
        m_otaStoreSavePending = savePending;
        m_otaStoreBeginBusy = beginBusy;
        m_otaStorePersistenceContext = context;
    }
#endif

    void SetFactoryDutTaskStorage(firmware::SettingsFactoryTaskStorage& storage) {
#if defined(LITCH_BLE) && defined(LITCH_HW_VARIANT_S3)
        m_factoryDutTaskStorage = &storage;
#else
        (void)storage;
#endif
    }
    void SetFactoryBootOnly(bool enabled, bool savedVerifierRequested = false) {
#if defined(LITCH_BLE)
        m_factoryBootOnly = enabled;
        m_factorySavedVerifierRequested = enabled && savedVerifierRequested;
#else
        (void)enabled;
        (void)savedVerifierRequested;
#endif
    }
    void SetHardwareRevision(
        ble_device_capabilities::HardwareRevision revision);
    void SetRuntimeHealthProvider(
        const runtime::RuntimeHealthReducer* runtimeHealth) {
        m_runtimeHealth = runtimeHealth;
    }
    void SetRuntimeHealthRecheckCallback(
        RuntimeHealthRecheckCallback callback,
        void* context) {
        m_runtimeHealthRecheckCallback = callback;
        m_runtimeHealthRecheckContext = context;
    }
    void SetOtaPlatformAdapter(OtaPlatformAdapter* adapter) {
        m_otaPlatformAdapter = adapter;
    }
    // Non-owning; bind before Begin and retain across stack restarts.
    void SetBootGuard(runtime::BootGuard* bootGuard) {
        m_bootGuard = bootGuard;
    }
    // Called only by the runtime health owner after durable VALID readback.
    void PublishOtaRuntimeAccepted() { m_otaRuntimeAccepted = true; }
    void ReserveBootFirmwareDigest();
    void ReleaseBootFirmwareDigest();
    void PublishVerifiedRunningFirmwareDigest(const uint8_t* digest);
    [[nodiscard]] bool IsOtaRuntimeAccepted() const {
        return m_otaRuntimeAccepted;
    }
    FirmwareDigestState ReadRunningFirmwareDigest(
        uint8_t* outDigest, size_t outCapacity);
    void DrainEventQueue();
    void Update(const SettingsStore& settings);
    void StartAdvertising();
    void SetDiagnosticRadioOverride(bool active, bool enabled) {
        m_diagnosticRadioOverride = active; m_diagnosticRadioEnabled = enabled;
    }
    bool RefreshDiagnosticPhysicalInputState();
    bool IsConnectionSessionActive() const;
    struct LinkObservation final {
        uint16_t mtu = 0U, intervalUnits = 0U, latency = 0U, timeoutUnits = 0U;
        uint8_t txPhy = 0U, rxPhy = 0U;
        bool phyKnown = false;
        BleDataLengthObservation dataLength{};
        bool dataLengthKnown = false;
    };
    LinkObservation GetLinkObservation() const {
#if defined(LITCH_BLE)
        auto value = m_linkObservation;
        value.mtu = GetMtu(); // SDK's cached negotiated peer MTU, not an HCI call.
        return value;
#else
        return {};
#endif
    }
    uint32_t SessionNotifyCount() const { return m_sessionNotifyCount; }
    uint32_t SessionIngressCount() const { return m_sessionIngressCount.load(); }
    struct SessionDiagnostics {
        uint32_t received = 0U;
        uint32_t notifyAttempts = 0U;
        uint32_t notifyFailures = 0U;
        uint32_t lastFailureMs = 0U;
        uint32_t stateStreamToken = 0U;
        uint32_t consoleOutputOverflows = 0U;
        uint64_t sentStateCodes = 0U;
        uint16_t receivedControls = 0U;
        uint8_t lastIngressResult = 0U;
        uint8_t lastFailureCode = 0U;
        uint8_t lastFailureKind = 0U;
        bool activated = false;
        bool subscribed = false;
    };
    // Read only on the cooperative loop, which also owns dispatch and notify.
    SessionDiagnostics GetSessionDiagnostics() const {
        auto result = m_sessionDiagnostics;
        result.consoleOutputOverflows = m_consoleOutputOverflows.load(std::memory_order_relaxed);
#if defined(LITCH_BLE)
        result.activated = m_connectionSession.IsActivated();
        result.subscribed = m_connectionSessionSubscribed.load(std::memory_order_acquire);
        result.stateStreamToken = m_connectionSession.StateStreamToken();
#endif
        return result;
    }
#if defined(LITCH_BLE) && defined(LITCH_BLE_OTA) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
    struct LiveIngressDiagnostics {
        ConnectionSession::LiveObjectDiagnostics session;
        BleEventQueue::LiveIngressDiagnostics raw;
        BleOtaChunkQueue::LiveIngressDiagnostics store;
        uint32_t nativeEpoch = 0U, nativeSequence = 0U, progressSubmitted = 0U;
        uint8_t nativeEnqueued = 0U;
        bool nativeAvailable = false;
    };
    // Console and session dispatch share the cooperative-loop owner.
    LiveIngressDiagnostics GetLiveIngressDiagnostics() const {
        LiveIngressDiagnostics result{m_connectionSession.GetLiveObjectDiagnostics(),
                m_eventQueue.GetLiveIngressDiagnostics(),
                m_otaChunkQueue.GetLiveIngressDiagnostics()};
        // Single native callback writer. Odd/changing revisions are explicitly
        // unavailable; all tuple fields are atomic, including during a race.
        const uint32_t revision = m_liveObjectRxRevision.load();
        result.nativeEpoch = m_bleSettings.CurrentConnectionEpoch();
        if ((revision & 1U) == 0U) {
            const uint32_t epoch = m_liveObjectRxEpoch.load();
            const uint32_t sequence = m_liveObjectRxSequence.load();
            const uint32_t enqueued = m_liveObjectRxEnqueued.load();
            result.nativeAvailable = revision != 0U && revision == m_liveObjectRxRevision.load() &&
                epoch == result.nativeEpoch && epoch == m_bleSettings.CurrentConnectionEpoch();
            if (result.nativeAvailable) {
                result.nativeSequence = sequence;
                result.nativeEnqueued = static_cast<uint8_t>(enqueued);
            }
        }
        if (m_liveObjectProgressToken == m_connectionSession.ObjectStreamToken())
            result.progressSubmitted = m_liveObjectProgressSubmitted;
        return result;
    }
    // Reuses the existing frame-ingress context; bind after those callbacks.
    void SetLiveIngressDiagnosticCallback(void (*callback)(void*)) {
        m_liveIngressDiagnosticCallback = callback;
    }
#endif
    // True only after native callbacks/controller and owned resources stop.
    bool Stop();

    [[nodiscard]] bool IsConnected() const;
    [[nodiscard]] bool IsEnabled() const;
    [[nodiscard]] bool IsAdvertising() const;
    [[nodiscard]] uint16_t GetMtu() const;
    [[nodiscard]] uint8_t GetConnectionCount() const;
    [[nodiscard]] bool IsOtaInProgress() const;
    [[nodiscard]] uint8_t GetOtaProgressPercent() const;
    [[nodiscard]] OtaLedPresentation GetOtaLedPresentation() const;
    [[nodiscard]] bool IsBulkTransferActive() const;
    [[nodiscard]] const char* GetDeviceName() const;
    [[nodiscard]] const char* GetPeerAddress() const;
    [[nodiscard]] bool IsConsoleStreamSubscribed() const;
    bool ConsumeConsoleStreamSnapshotRequest();
    // Start a Home publication only on the live subscribed stream. A request
    // stays pending until that stream can own its replay.
    bool TryBeginHomeStatePublication(bool& outForce, uint32_t* outReceipt = nullptr);
    [[nodiscard]] bool TryReadConsoleInputChar(char& outChar);
    void WriteConsoleOutput(const char* data, size_t len);
    void SetBleLinkTestInProgress(bool enabled);
    void SetBeforeBlockingDiagnosticCallback(
        BeforeBlockingDiagnosticCallback callback,
        void* context);
    void SetSettingsMutationCallback(
        SettingsMutationCallback callback,
        void* context);
    void SetControlMutationService(ControlMutationService* service) {
#if defined(LITCH_BLE)
        m_controlMutationService = service;
        m_bleSettings.SetControlMutationService(service);
#else
        (void)service;
#endif
    }
    // Shared admission for the wire command and developer console batches.
    [[nodiscard]] static bool AreNotifyTestArgumentsValid(
        uint16_t packetSize, uint16_t packetCount, uint16_t delayMs);
    // False includes an interrupted batch; partial counts remain available.
    bool RunConsoleNotifyTest(uint16_t packetSize,
                              uint16_t packetCount,
                              uint16_t interPacketDelayMs,
                              BleLinkTestResult& outResult,
                              uint32_t run = 0U);
    bool ConsumeIdentifyStart(uint8_t& outNumber,
                              uint8_t& outRed,
                              uint8_t& outGreen,
                              uint8_t& outBlue,
                              bool& outPadTwoDigits,
                              uint32_t& outDurationMs);
    bool ConsumeIdentifyStop();
    FactoryBleDutResult PollFactoryBleDut(uint32_t sessionId,
        uint32_t dutNonce, bool lowerAddressOnly, FactoryBleProof& outProof);
    // Nonblocking cancellation. Stack teardown must wait for true quiescence.
    bool QuiesceFactoryBleDut();
    bool IsFactoryBleDutRunning() const;
    bool HasFactoryBleCandidate() const;
    uint64_t FactoryRadioIdentity() const;
    bool EnableTemporaryFactoryVerifierEndpoint();
    bool ConsumeFactoryBleVerifierProof(FactoryBleProof& outProof);
    bool EnableFactoryVerifierEndpoint(const SettingsStore& settings);
    void SetFactoryVerifierSupported(bool supported);
    void SetFactoryVerifierConfiguredEnabled(bool enabled);
    void SetFactoryVerifierRuntimeAllowed(bool allowed);
    [[nodiscard]] bool IsFactoryVerifierEndpointEnabled() const;
    [[nodiscard]] bool IsFactoryVerifierConfiguredEnabled() const;

#if defined(LITCH_BLE)
    void OnLitchcraftWrite(const uint8_t* data, size_t len);
    void OnLitchcraftSubscription(uint16_t value);
    void OnConnectionSessionWrite(const uint8_t* data, size_t len);
    void OnConnectionSessionSubscription(uint16_t value);
    bool PublishConnectionState(uint8_t code, uint32_t revision,
                                const uint8_t* data, size_t len);
    void PublishConnectionPhysicalInputState(uint64_t occupancy);
    void PublishConnectionButtonState(const ButtonEvent& event,
                                      uint32_t observedAtMs);
    void ResetConnectionButtonState();
    void OnCommandCharacteristicWrite(const char* data, size_t len);
    void OnOtaControlCharacteristicWrite(const char* data, size_t len);
    void OnFactoryRadioCharacteristicWrite(const uint8_t* data, size_t len);
#if defined(LITCH_BLE_OTA)
    bool OnOtaDataCharacteristicWrite(const uint8_t* data, size_t len);
#endif
    void OnConnected(uint16_t connHandle, const char* peerAddress, uint16_t mtu, uint8_t connectionCount);
    void OnDisconnected(int reason = 0);
    void OnDisconnected(uint16_t connHandle, int reason);
    bool OwnerCallbackAllowed(uint16_t connHandle, bool bootstrap = false) const;
    bool NativePairingAllowed(uint16_t connHandle) const;
    struct OwnerStoreDiagnostics {
        bool available = false;
        uint8_t savedPeers = 0U, rpas = 0U, orphanRpas = 0U;
    };
    // Fresh read-only counts; unavailable never implies empty custody.
    [[nodiscard]] OwnerStoreDiagnostics ReadOwnerStoreDiagnostics() const;
    bool OwnerAcceptsNewConnection(uint16_t connHandle = 0xffffU) const;
    void OnOwnerSecurity(const BleOwnerSecurityEvent& event);
    void OnNativeEncryptionChange(uint16_t connHandle, int32_t nativeStatus);
    void OnLinkObservation(uint16_t handle, uint16_t mtu, uint8_t txPhy, uint8_t rxPhy,
                           bool phyUpdateCompleted = false);
    void OnNativeDataLengthChange(uint16_t handle, const BleDataLengthObservation& value);
    void OnConnectionParametersChanged(uint16_t intervalUnits,
                                       uint16_t latency,
                                       uint16_t supervisionTimeoutUnits,
                                       bool initial, uint16_t handle = 0xffffU);
    void OnConsoleStreamSubscriptionChanged(uint16_t subValue);
    void OnOtaControlSubscriptionChanged(uint16_t subValue);
#endif

  private:
#if defined(LITCH_NATIVE)
    friend struct BleServiceConnectionEpochTestPeer;
    friend class VirtualLitchBleAdapter;
#endif
    static constexpr const char* DEFAULT_DEVICE_NAME = "Foxie Litch";
    static constexpr const char* RECOVERY_DEVICE_NAME = "Litch Recovery";
    void InvokeBeforeBlockingWorkCallback();
    void InvokeSettingsMutationCallback(const char* key);

#if defined(LITCH_BLE)
    void InitStack(const SettingsStore* settings);
    bool StopNativeStack();
    bool ConfigureAdvertising();
    bool OwnerPairingEnabled() const;
    bool OwnerProductAccess() const;
    void RevokeOwnerAdmission();
    bool InitOwnerPairingNative();
    void ObserveOwnerNativePeer(uint16_t connHandle);
    bool StartOwnerNativeSecurity(uint16_t connHandle, int* nativeResult = nullptr);
    bool OwnerNativePairingCandidate(uint16_t connHandle) const;
    bool ReadIdentifyNativePeer(ble_identify::PeerIdentity& peer) const;
    bool OwnerNativeLinksQuiescent() const;
    bool OwnerNativeGapQuiescent() const;
    bool QuiesceOwnerNativeGap();
    void DisconnectOwnerNativeLinks();
    void HandleOwnerSecurity(const BleOwnerSecurityEvent& event);
    void PublishOwnerPairingState();
    BoardOwnerPairingRuntime::Connection OwnerConnection() const;
    static bool ClearOwnerNativeStore(void* context);
    bool ReadOwnerAddBaseline(BoardOwnerAddBaseline& baseline) const;
    static bool RollbackOwnerNativeStore(void* context, const BoardOwnerAddBaseline& baseline);
    void BeginQueues();
    void UpdateLitchcraftNotifications();
    void UpdateConnectionSessionNotifications();
    void UpdateFastConnectionPreference();
    void DrainConnectionSessionCommands(uint32_t connectionEpoch);
    bool RetryPendingConnectionObject();
#if defined(LITCH_BLE_OTA)
    bool ReturnBulkObjectCredits();
#endif
    enum class ObjectAdmission : uint8_t { Accepted, DeferredCapacity, Invalid, RetainedCredit };
    void DispatchConnectionObject(const ConnectionSession::ObjectCommand& object,
                                  uint32_t connectionEpoch);
    ObjectAdmission HandleConnectionSupportObject(const ConnectionSession::ObjectCommand& object, uint32_t connectionEpoch);
    void UpdateConnectionSupportOutput();
    ObjectAdmission HandleConnectionGameHistoryObject(
        const ConnectionSession::ObjectCommand& object,
        uint32_t connectionEpoch);
#if defined(LITCH_BLE_OTA)
    ObjectAdmission HandleConnectionOtaDataObject(
        const ConnectionSession::ObjectCommand& object,
        uint32_t ingressOrdinal);
#endif
    ObjectAdmission HandleConnectionOtaControlObject(
        const ConnectionSession::ObjectCommand& object,
        uint32_t ingressOrdinal, uint32_t connectionEpoch);
    ObjectAdmission HandleConnectionLitchcraftObject(
        const ConnectionSession::ObjectCommand& object,
        uint32_t ingressOrdinal, uint32_t connectionEpoch);
    enum class OtaStatusDelivery : uint8_t { Reliable, Progress, Reboot };
    bool CanRetainConnectionOtaStatus() const;
#if defined(LITCH_BLE_OTA)
    bool CanRetainConnectionOtaTerminal() const;
#endif
    bool QueueConnectionOtaStatus(
        const uint8_t* data, size_t len,
        OtaStatusDelivery delivery = OtaStatusDelivery::Reliable);
    void ResetConnectionOtaStatusOutput();
    void UpdateConnectionOtaStatusOutput();
#if defined(LITCH_BLE_OTA)
    bool UpdateConnectionFrameStoreOutput();
#endif
    void UpdateConnectionLitchcraftOutput();
    void UpdateConnectionGameHistoryOutput();
    bool CompleteConnectionSessionCommand(
        uint32_t streamToken, uint32_t sequence, uint8_t opcode,
        const uint8_t* body, size_t bodySize,
        fxe::OrderedReceiveWindow<
            ConnectionSession::ORDERED_CAPACITY,
            ConnectionSession::INTERACTIVE_PACKET_BYTES>::Outcome outcome,
        uint32_t revision);
    void ClearPendingConnectionControlAction();
    bool ConfigureFactoryRadioEndpoint(const SettingsStore& settings);
    bool CreateFactoryRadioEndpoint();
    void ReleaseFactoryRadioEndpoint();
    bool ShutdownStack();
    bool StopAdvertising();
    void ReconcileConnectionProjection();
    void HandleEvent(const BleEvent& event);
    bool TryBeginLitchcraftEvent(const BleEvent& event);
    void HandleConsoleInputEvent(const BleConsoleInputEvent& event);
    void HandleConsoleInputPacket(const char* data, size_t len);
    void HandleOtaControlPacket(const char* data,
                                size_t len,
                                uint32_t sessionEpoch,
                                uint32_t connectionEpoch, bool sharedIngress = false);
    bool HandleRuntimeHealthRequest(const char* data,
                                    size_t len,
                                    uint32_t connectionEpoch);
    void NotifyRuntimeHealth(uint32_t connectionEpoch);
    void NotifyRuntimeHealthRecheckResult(bool succeeded,
                                          uint32_t connectionEpoch);
#if defined(LITCH_BLE_OTA)
    void HandleOtaChunkPacket(const uint8_t* data,
                              size_t len,
                              uint32_t sessionEpoch);
#endif
    bool NotifyOtaJson(const char* jsonPayload);
    bool NotifyOtaStatus(const char* status, const char* detail = nullptr);
    void NotifyDeviceInfo();
    void NotifyBluetoothIdentitySuffix();
    void RequestFirmwareDigestNotification();
    void UpdateFirmwareDigestNotification();
#if defined(LITCH_BLE_OTA)
    void NotifyOtaProbe(const char* mode, uint32_t requestedSize);
    void NotifyOtaBeginAccepted();
    void ResetOtaTransferState(bool stopIdentify);
    void StartOtaIdentify(uint8_t number, uint32_t durationMs);
    void StopOtaIdentify();
    uint16_t GetOtaMaxChunkBytes() const;
    uint16_t GetOtaRecommendedChunkBytes() const;
    uint8_t GetOtaRecommendedBurstCount() const;
    uint32_t GetOtaRecommendedWindowBytes() const;
    bool DrainOneOtaChunk();
    void WakeLitchcraftFrameIngress();
    bool DeferOtaEndForPersistence(const char* data, size_t len,
                                  uint32_t sessionEpoch, uint32_t connectionEpoch);
    void PollOtaEndPersistence();
    void UpdateOtaInactivityTimeout();
    bool HandleRejectedOtaBeginCandidateIngressFailure();
    bool HandleOtaQueueFailure();
    bool AppendOtaWriteStage(const uint8_t* data, size_t len);
    bool FlushOtaWriteStageBuffer();
    BleOtaTransactionOps GetOtaTransactionOps();
    static bool BeginOtaBackend(void* context, uint32_t size);
    static bool WriteOtaBackend(void* context,
                                const uint8_t* data,
                                size_t len);
    static bool FlushOtaBackend(void* context);
    static bool EndOtaBackend(void* context, bool evenIfRemaining);
    static void AbortOtaBackend(void* context);
    void RejectOtaBeginCandidate(uint32_t sessionEpoch);
    void AbortOtaTransfer(bool stopIdentify,
                          bool invalidateQueuedData,
                          uint32_t ownedSessionEpoch = 0U);
    void LogOtaWriteMetrics(const char* status) const;
#endif
    void EnqueueConsoleInputBytes(const char* data, size_t len);
    void FlushConsoleOutputBuffer();
    bool TryDequeueConsoleInputChar(char& outChar);
    bool IsJsonEnvelope(const char* data, size_t len) const;
    bool ReadEnabledSetting(const SettingsStore& settings) const;
    fxe::FixedString<32> ReadDeviceNameSetting(const SettingsStore& settings) const;

    BleEventQueue m_eventQueue;
    BleConnectionProjection m_connectionProjection;
#if defined(LITCH_BLE_OTA)
    BleOtaChunkQueue m_otaChunkQueue;
    BleOtaTransaction m_otaTransaction;
    // The OTA transaction retains image authority while this original end
    // request waits for a durable settings receipt.
    BleOtaControlEvent m_pendingOtaEnd{};
    uint32_t m_pendingOtaEndAtMs = 0U;
    uint32_t m_nextOtaPersistencePrepareAtMs = 0U;
    bool m_otaEndPersistenceReady = false;
    bool m_otaEndFlushRequested = false;
    OtaStoreSaveRequestCallback m_otaStoreSaveRequest = nullptr;
    OtaStoreSavePendingCallback m_otaStoreSavePending = nullptr;
    OtaStoreBeginBusyCallback m_otaStoreBeginBusy = nullptr;
    void* m_otaStorePersistenceContext = nullptr;
    std::atomic_flag m_otaIngressLock = ATOMIC_FLAG_INIT;
    std::atomic<bool> m_otaDisconnectPending = false;
    uint32_t m_otaRejectedBeginCandidateEpoch = 0U;
    BleOtaRejectedBeginReason m_otaRejectedBeginCandidateReason =
        BleOtaRejectedBeginReason::None;
    uint32_t m_nextOtaIngressOrdinal = 1U;
#endif
    // Current shared diagnostics also serve BLE profiles without an updater.
    std::array<char, 192U> m_connectionOtaControlInput{};
    uint8_t m_connectionOtaControlExpected = 0U;
    uint8_t m_connectionOtaControlWritten = 0U;
    std::array<uint8_t, 224U> m_connectionOtaStatusPending{};
    uint8_t m_connectionOtaStatusPendingSize = 0U;
    uint8_t m_connectionOtaStatusPendingOffset = 0U;
    uint32_t m_connectionOtaStatusRevision = 0U;
#if defined(LITCH_BLE_OTA)
    bool m_connectionOtaStatusReplaceable = false;
    bool m_connectionOtaProgressDirty = false;
    bool m_connectionOtaStatusCompletesReboot = false;
    // One terminal continuation preserves a safety abort without allocating a
    // second JSON buffer or blocking the abort behind receiver credits.
    const char* m_connectionOtaDeferredTerminalStatus = nullptr;
    fxe::FixedString<32> m_connectionOtaDeferredTerminalDetail;
    uint32_t m_connectionOtaRebootSequence = 0U;
    std::array<uint8_t, 512U> m_connectionLitchcraftInput{};
#else
    // Static Craft's control packets fit the minimum ATT payload. No bulk or
    // inactive animation/program bytes are retained on these profiles.
    std::array<uint8_t, 20U> m_connectionLitchcraftInput{};
#endif
    uint16_t m_connectionLitchcraftExpected = 0U;
    uint16_t m_connectionLitchcraftWritten = 0U;
    uint16_t m_connectionLitchcraftOutputOffset = 0U;
    uint32_t m_connectionLitchcraftOutputRevision = 0U;
#if defined(LITCH_BLE_OTA)
    uint16_t m_connectionFrameStoreOutputOffset = 0U;
    uint32_t m_connectionFrameStoreOutputCrc = 0U;
    LitchcraftFrameIngressCallback m_litchcraftFrameIngressCallback = nullptr;
    LitchcraftFramePriorityCallback m_litchcraftFramePriorityCallback = nullptr;
    LitchcraftFrameResponsePeekCallback
        m_litchcraftFrameResponsePeekCallback = nullptr;
    LitchcraftFrameResponseAcceptCallback
        m_litchcraftFrameResponseAcceptCallback = nullptr;
    LitchcraftFrameReceiptReplayCallback
        m_litchcraftFrameReceiptReplayCallback = nullptr;
    void* m_litchcraftFrameIngressContext = nullptr;
#endif
    SettingsStore* m_settings = nullptr;
    ControlMutationService* m_controlMutationService = nullptr;
    NimBLEServer* m_server = nullptr;
    NimBLEService* m_baseService = nullptr;
    NimBLEService* m_settingsService = nullptr;
    NimBLEService* m_factoryRadioService = nullptr;
    NimBLEAdvertising* m_advertising = nullptr;
    NimBLECharacteristic* m_litchcraftCharacteristic = nullptr;
    std::atomic<bool> m_litchcraftSubscribed=false;
    std::atomic<uint16_t> m_litchcraftSubscriptionValue{0U};
    std::atomic<uint32_t> m_litchcraftSubscriptionEpoch{0U};
    std::atomic<uint32_t> m_litchcraftSubscriptionRevision{0U};
    std::atomic<bool> m_litchcraftIngressOverflow=false;
    NimBLECharacteristic* m_connectionSessionIngressCharacteristic = nullptr;
    NimBLECharacteristic* m_connectionSessionEgressCharacteristic = nullptr;
    std::atomic<bool> m_connectionSessionSubscribed = false;
    std::atomic<uint16_t> m_connectionSessionSubscriptionValue{0U};
    std::atomic<uint32_t> m_connectionSessionSubscriptionEpoch{0U};
    std::atomic<uint32_t> m_connectionSessionSubscriptionRevision{0U};
    NimBLECharacteristic* m_commandCharacteristic = nullptr;
    NimBLECharacteristic* m_consoleStreamCharacteristic = nullptr;
    NimBLECharacteristic* m_otaControlCharacteristic = nullptr;
    NimBLECharacteristic* m_factoryRadioCharacteristic = nullptr;
    NimBLECharacteristicCallbacks* m_factoryRadioCallbacks = nullptr;
#if defined(LITCH_HW_VARIANT_S3)
    alignas(std::max_align_t) unsigned char m_factoryRadioCallbackStorage[2U * sizeof(void*)]{};
#endif
#if defined(LITCH_BLE_OTA)
    NimBLECharacteristic* m_otaDataCharacteristic = nullptr;
#endif
    BleSettings m_bleSettings;
    BoardOwnerPairingRuntime* m_ownerRuntime = nullptr;
    firmware::BoardOwnerPersistenceRuntime* m_ownerPersistence = nullptr;
    BoardIdentityPersistence* m_ownerIdentity = nullptr;
    BoardOwnerPairingRuntime::Connection m_ownerObservedConnection{};
    BleOwnerSecurityEvent m_ownerSecurity{};
    BleOwnerSecurityEvent m_ownerPeerIdentity{};
    std::atomic<uint16_t> m_ownerNativeHandle{0xffffU};
    std::atomic<uint32_t> m_ownerNativeEpoch{0U};
    std::atomic<uint32_t> m_ownerAdmissionToken{0U};
    std::atomic<uint32_t> m_ownerAdmissionEpoch{0U};
    std::atomic<uint32_t> m_ownerAdmissionSession{0U};
    std::atomic<uint16_t> m_ownerAdmissionHandle{0xffffU};
    std::atomic<uint32_t> m_ownerProductEpoch{0U};
    std::atomic<uint32_t> m_ownerRejectedEpoch{0U};
    std::atomic<uint32_t> m_ownerForeignEpoch{0U};
    std::atomic<bool> m_ownerNativeFault{false};
    std::atomic<bool> m_ownerQuiescing{false};
    uint32_t m_ownerStartedSecurityToken = 0U;
    uint32_t m_ownerQuietSecurityEpoch = 0U;
    uint32_t m_ownerClearToken = 0U;
    uint32_t m_ownerStateRevision = 0U;
    std::array<uint8_t, 8U> m_ownerStateBytes{};
    bool m_ownerStateValid = false;
    bool m_ownerBooted = false;
    bool m_ownerPreviewEnabled = OWNER_PAIRING_ENABLED;
    bool m_factoryBootOnly = false;
    bool m_factorySavedVerifierRequested = false;
    bool m_initialized = false;
    bool m_recoveryOnly = false;

    BleConsoleInputBuffer m_consoleInput;

    static constexpr size_t CONSOLE_OUTPUT_BUFFER_CAPACITY = 4096;
    char m_consoleOutputBuffer[CONSOLE_OUTPUT_BUFFER_CAPACITY] = {};
    size_t m_consoleOutputLen = 0;
    uint32_t m_lastConsoleNotifyMs = 0;

    bool m_otaInProgress = false;
    bool m_otaSimulationMode = false;
    bool m_otaPendingReboot = false;
    uint32_t m_otaRebootArmedAtMs = 0U;
    bool m_otaRebootGraceArmed = false;
    uint32_t m_otaRebootDisconnectAtMs = 0U;
    bool m_otaRebootDisconnectIssued = false;
    uint32_t m_otaExpectedSize = 0;
    uint32_t m_otaBytesReceived = 0;
    uint32_t m_lastOtaStatusMs = 0;
    uint32_t m_otaStartedAtMs = 0;
    uint8_t m_otaProgressPercent = 0;
    uint32_t m_otaCrc32 = 0;
#if defined(LITCH_BLE_OTA)
    bool m_otaStrictProfileArmed = false;
    uint16_t m_otaNegotiatedChunkBytes = 0;
    uint8_t m_otaNegotiatedBurstCount = 0;
    uint32_t m_otaNegotiatedWindowBytes = 0;
    static constexpr size_t OTA_WRITE_STAGE_CAPACITY = 4096;
    uint8_t m_otaWriteStageBuffer[OTA_WRITE_STAGE_CAPACITY] = {};
    size_t m_otaWriteStageLen = 0;
    uint32_t m_otaWriteCallCount = 0;
    uint32_t m_otaWriteBytesFlushed = 0;
    uint32_t m_otaWriteTotalMs = 0;
    uint32_t m_otaWriteMaxMs = 0;
    uint32_t m_otaWriteSlowCount = 0;
    uint32_t m_otaChunkEnqueueCount = 0;
    uint32_t m_otaChunkEnqueueDrops = 0;
    uint32_t m_otaChunkDrainCycles = 0;
    uint32_t m_otaChunkDrainCount = 0;
    uint32_t m_otaChunkDrainMaxPerCycle = 0;
#endif
    uint32_t m_otaNotifyCount = 0;
    uint32_t m_otaNotifyTotalMs = 0;
    uint32_t m_otaNotifyMaxMs = 0;
    hardware::RmtFlashArbiter* m_nativeStoreFlashArbiter = nullptr;
    bool m_bootDigestReserved = false;
    FirmwareDigestProvider m_firmwareDigestProvider;
    FirmwareDigestState m_lastDigestNotifyState = FirmwareDigestState::Unavailable;
    uint8_t m_digestNotifyFragment = 0U;
    bool m_digestNotifyRequested = false;

#if defined(LITCH_BLE) && defined(LITCH_HW_VARIANT_S3)
    static void FactoryBleDutTask(void* context);
    static void FactoryBleDutCleanup(ble_npl_event* event);
    void ExecuteFactoryBleDut();
    bool ReleaseFactoryBleDutResources();
    enum class FactoryDutJobState : uint8_t {
        Idle, Running, Complete, CleanupSubmit, CleanupQueued,
    };
    enum class FactoryDutCleanupResult : uint8_t { Pending, Retry, Released };
    firmware::SettingsFactoryTaskStorage* m_factoryDutTaskStorage = nullptr;
    void* m_factoryDutTask = nullptr;
    std::atomic<FactoryDutJobState> m_factoryDutJobState{FactoryDutJobState::Idle};
    ble_npl_event m_factoryDutCleanupEvent{}; // One fixed SDK host event, no owned queue.
    std::atomic<FactoryDutCleanupResult> m_factoryDutCleanupResult{FactoryDutCleanupResult::Pending};
    std::atomic_flag m_factoryDutClientAccess = ATOMIC_FLAG_INIT;
    std::atomic<bool> m_factoryDutCancel{false};
    FactoryBleProof m_factoryDutProof{};
    NimBLEClient* m_factoryDutClient = nullptr;
    std::atomic<bool> m_factoryDutCandidate{false};
    uint32_t m_factoryDutStartedMs = 0U;
    FactoryRadioPeerAttempts m_factoryPeerAttempts;
    bool m_factoryDutLowerOnly = false;
    bool m_factoryDutVerified = false;
#endif
    std::atomic<uint32_t> m_factoryVerifierGeneration{1U};
    std::atomic<uint32_t> m_factoryProofGeneration{0U};
    uint32_t m_factoryVerifierCallbackGeneration = 0U;
    uint32_t m_factoryVerifierLastFrameMs = 0U;
    FactoryRadioVerifierHandshake m_factoryVerifierHandshake;
    std::atomic<bool> m_factoryVerifierConfiguredEnabled = false;
    std::atomic<bool> m_factoryVerifierRuntimeAllowed = false;
    std::atomic<bool> m_factoryVerifierProofPending = false;
    std::atomic<uint32_t> m_factoryProofSessionId = 0U;
    std::atomic<uint32_t> m_factoryProofDutNonce = 0U;
    std::atomic<uint32_t> m_factoryProofVerifierNonce = 0U;
    bool m_factoryVerifierEndpointEnabled = false;
    // Fail closed until boot binds a concrete supported hardware profile.
    bool m_factoryVerifierSupported = false;

    ble_identify::RateLimit m_identifyRateLimit;
    bool m_identifyStartPending = false;
    bool m_identifyStopPending = false;
    uint8_t m_identifyNumber = 1;
    uint8_t m_identifyRed = 255;
    uint8_t m_identifyGreen = 255;
    uint8_t m_identifyBlue = 255;
    bool m_identifyPadTwoDigits = false;
    uint32_t m_identifyDurationMs = 3000;

#if defined(LITCH_BLE_OTA)
    uint8_t m_otaLastDisplayedPercent = 255;
    uint8_t m_otaLastLoggedStep = 255;
#endif
    bool m_bleLinkTestInProgress = false;
    uint16_t m_connectionHandle = 0;
    uint32_t m_fastConnectionEpoch = 0U;
    uint8_t m_fastConnectionRequestCount = 0U;
    bool m_fastConnectionUpdatePending = false;
    bool m_fastConnectionBootstrapHandled = false;
#if defined(LITCH_HW_VARIANT_S3)
    uint32_t m_preferredPhyEpoch = 0U; // Last physical epoch considered; SDK errors never retry.
    bool m_preferredPhyPending = false;
#endif
    uint32_t m_connectedAtMs = 0;
    uint32_t m_lastClientActivityMs = 0;
    bool m_staleDisconnectIssued = false;
    std::atomic<bool> m_consoleStreamSubscribed = false;
    std::atomic<uint16_t> m_consoleStreamSubscriptionValue{0U};
    std::atomic<uint32_t> m_consoleStreamSubscriptionEpoch{0U};
    std::atomic<uint32_t> m_consoleStreamSubscriptionRevision{0U};
    std::atomic<bool> m_consoleStreamSnapshotRequested = false;
    std::atomic<uint32_t> m_consoleSnapshotReceipt = 0;
    std::atomic<bool> m_otaControlSubscribed = false;
    std::atomic<uint16_t> m_otaControlSubscriptionValue{0U};
    std::atomic<uint32_t> m_otaControlSubscriptionEpoch{0U};
    std::atomic<uint32_t> m_otaControlSubscriptionRevision{0U};
#if defined(LITCH_NATIVE)
    void (*m_subscriptionActivationTestHook)(void*) = nullptr;
    void* m_subscriptionActivationTestContext = nullptr;
    void (*m_beforeIngressDispatchTestHook)(void*) = nullptr;
    void* m_beforeIngressDispatchTestContext = nullptr;
#endif
#endif

    GamePersistenceCoordinator* m_litchcraftGame=nullptr;
    ConnectionSession m_connectionSession;
    LinkObservation m_linkObservation{};
#if defined(LITCH_BLE_OTA)
    ConnectionObjectBenchmark m_objectBenchmark;
    ConnectionObjectBenchmark::Lifetime BenchmarkLifetime() const;
    bool BenchmarkAvailable() const;
    bool IsCurrentSharedBenchmarkCancel(const BleEvent& event) const;
    bool HandleObjectBenchmarkControl(const char* cmd, JsonObjectConst request, bool sharedIngress);
    bool BeginForwardBenchmark(JsonObjectConst request, bool sharedIngress);
    void UpdateObjectBenchmarkOutput(uint8_t* scratch, size_t capacity);
    void NotifyObjectBenchmarkDone();
#endif
    std::atomic<bool> m_connectionSessionActivated = false;
    // Callback publication; native disconnect is dispatched by the service loop.
    std::atomic<uint32_t> m_connectionRenewalEpoch{0U};
    std::atomic<uint16_t> m_connectionRenewalHandle{0xffffU};
    uint32_t m_connectionRenewalIssuedEpoch = 0U;
    void PollConnectionRenewal();
    uint64_t m_connectionSessionBootNonce = 0U;
    uint32_t m_connectionSessionDomainDigest = 0U;
    struct PendingConnectionControlAction final {
        [[nodiscard]] bool IsActive() const { return opcode != 0U; }
        uint32_t streamToken = 0U;
        uint32_t sequence = 0U;
        uint16_t requestId = 0U;
        uint8_t opcode = 0U;
        // Control25 and Store Frame Edit10 both retain exactly five semantic
        // bytes for asynchronous completion at the same Ordered position.
        std::array<uint8_t, 5U> body{};
    };
    PendingConnectionControlAction m_pendingConnectionControlAction{};
    ConnectionGameHistoryIngressCallback
        m_connectionGameHistoryIngressCallback = nullptr;
    void* m_connectionGameHistoryIngressContext = nullptr;
    ConnectionGameHistoryPeekCallback m_connectionGameHistoryPeekCallback =
        nullptr;
    ConnectionGameHistoryAcceptCallback
        m_connectionGameHistoryAcceptCallback = nullptr;
    void* m_connectionGameHistoryOutputContext = nullptr;
    SupportCaptureCallback m_supportCapture = nullptr;
    void* m_supportCaptureContext = nullptr;
    std::array<uint8_t, support_snapshot::CAPACITY> m_connectionSupportOutput{};
    uint16_t m_connectionSupportSize = 0U;
    uint16_t m_connectionSupportOffset = 0U;
    uint32_t m_connectionSupportEpoch = 0U;
    uint32_t m_connectionSupportRevision = 0U;
    uint8_t m_connectionSupportCode = 0U;
    std::array<uint8_t, 20U> m_connectionGameHistoryInput{};
    uint8_t m_connectionGameHistoryExpected = 0U;
    uint8_t m_connectionGameHistoryWritten = 0U;
    uint32_t m_connectionGameHistoryOutputToken = 0U;
    uint16_t m_connectionGameHistoryOutputSize = 0U;
    uint16_t m_connectionGameHistoryOutputOffset = 0U;
    std::array<uint8_t, 10U> m_connectionSessionLitchcraftState{};
    uint32_t m_connectionSessionLitchcraftStateRevision = 0U;
    uint32_t m_connectionSessionLitchcraftStatePublishedAtMs = 0U;
    uint32_t m_connectionSessionLitchcraftSourceRevision = 0U;
    bool m_connectionSessionLitchcraftStateValid = false;
    std::array<uint8_t, 10U> m_connectionSessionStoreAuthority{};
    uint32_t m_connectionSessionStoreAuthorityRevision = 0U;
    bool m_connectionSessionStoreAuthorityValid = false;
    uint64_t m_connectionPhysicalOccupancy = 0ULL;
    uint16_t m_connectionButtonBits = 0U;
    uint32_t m_connectionPhysicalInputRevision = 0U;
    bool m_connectionPhysicalOccupancyValid = false;
    bool m_litchcraftEligible=false;
    const runtime::RuntimeHealthReducer* m_runtimeHealth = nullptr;
    RuntimeHealthRecheckCallback m_runtimeHealthRecheckCallback = nullptr;
    void* m_runtimeHealthRecheckContext = nullptr;
    OtaPlatformAdapter* m_otaPlatformAdapter = nullptr;
    runtime::BootGuard* m_bootGuard = nullptr;
    bool m_otaRuntimeAccepted = false;
    BeforeBlockingDiagnosticCallback m_beforeBlockingDiagnosticCallback = nullptr;
    void* m_beforeBlockingDiagnosticContext = nullptr;
    SettingsMutationCallback m_settingsMutationCallback = nullptr;
    void* m_settingsMutationContext = nullptr;
    bool m_diagnosticRadioOverride = false;
    bool m_diagnosticRadioEnabled = false;
    uint32_t m_sessionNotifyCount = 0U;
    SessionDiagnostics m_sessionDiagnostics{};
    std::atomic<uint32_t> m_sessionIngressCount{0U};
    bool m_enabled = false;
    ble_device_capabilities::HardwareRevision m_hardwareRevision =
        ble_device_capabilities::HardwareRevision::Unknown;
    std::atomic<bool> m_connected = false;
    std::atomic<uint16_t> m_mtu = 0;
    std::atomic<uint8_t> m_connectionCount = 0;
    std::atomic<uint32_t> m_droppedEvents = 0;
#if defined(LITCH_BLE) && defined(LITCH_BLE_OTA) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
    void (*m_liveIngressDiagnosticCallback)(void*) = nullptr;
    bool m_liveIngressDiagnosticLogged = false;
    std::atomic<uint32_t> m_liveObjectRxRevision{0U}, m_liveObjectRxEpoch{0U};
    std::atomic<uint32_t> m_liveObjectRxSequence{0U}, m_liveObjectRxEnqueued{0U};
    uint32_t m_liveObjectProgressSubmitted = 0U, m_liveObjectProgressToken = 0U;
    uint32_t m_liveStallProgressMs = 0U, m_liveStallLastDrainMs = 0U;
    uint32_t m_liveStallLastSampleMs = 0U;
    uint32_t m_liveStallObjectToken = 0U;
    uint8_t m_liveStallSnapshots = 0U;
    bool m_liveStallArmed = false;
    void PollLiveIngressStallDiagnostics();
#endif
    std::atomic<uint32_t> m_consoleOutputOverflows = 0;
    fxe::FixedString<32> m_deviceName = fxe::FixedString<32>(DEFAULT_DEVICE_NAME);
    fxe::FixedString<24> m_peerAddress = fxe::FixedString<24>("");
};

} // namespace modules
