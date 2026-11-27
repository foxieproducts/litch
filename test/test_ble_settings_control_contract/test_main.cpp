#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"

#include "ble_service_test_config.hpp"

#include <NimBLEDevice.h>
#include <nimble/nimble/host/include/host/ble_store.h>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"
#include "firmware/led_runtime_admission.hpp"
#include "firmware/firmware_version.hpp"
#include "firmware/sleep_controller.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/ota_attempt_proof.hpp"
#include "modules/ble_event_queue.hpp"
#include "modules/ble_ota_ingress_order.hpp"
#include "modules/ble_service.hpp"
#include "modules/ble_service_internal.hpp"
#include "modules/litchcraft_frame_store.hpp"
#include "modules/litchcraft_frame_store_receiver.hpp"
#include "modules/litchcraft_wire_codec.hpp"
#include "firmware/litchcraft_frame_file.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "modules/ble_settings.hpp"
#include "modules/ble_settings_async.hpp"
#include "modules/ble_settings_enumerate.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/connection_session_codec.hpp"
#include "modules/settings_registry.hpp"
#include "modules/user_text.hpp"
#include "modules/settings_state_wire_codec.hpp"
#include "runtime/boot_guard.hpp"
#include "modules/support_snapshot.hpp"

namespace ota_runtime_test {
struct RestartRequested {};
extern unsigned shutdowns;
extern unsigned restarts;
void SetBootResetReason(uint32_t reason);
}

int TestArduinoBleConnectionProjectionPeek();
int TestEarlyUpdaterRuntime(modules::SettingsStore&, modules::BleService&,
                            hardware::RmtFlashArbiter&);
int TestRuntimeBootHealthRecovery(modules::SettingsStore&, modules::BleService&,
                                  hardware::RmtFlashArbiter&, NimBLEAdvertising&);

namespace modules {

struct RuntimeHealthRecheckProbe {
    uint32_t calls = 0U;
    bool result = false;
};

bool RunRuntimeHealthRecheckProbe(void* context) {
    auto* const probe = static_cast<RuntimeHealthRecheckProbe*>(context);
    if (probe == nullptr) {
        return false;
    }
    ++probe->calls;
    return probe->result;
}

struct BleServiceConnectionEpochTestPeer {
    static void ConfigureOwnerPreview(BleService& service, BoardOwnerPairingRuntime& runtime,
                                     BoardIdentityPersistence& identity) {
        service.m_ownerPreviewEnabled = true;
        service.m_ownerRuntime = &runtime;
        service.m_ownerIdentity = &identity;
        service.m_ownerBooted = true;
        service.m_server = NimBLEDevice::getServer();
        (void)service.InitOwnerPairingNative();
    }
    static BoardOwnerPairingRuntime::Connection OwnerConnection(const BleService& service) {
        return service.OwnerConnection();
    }
    static bool ProductAccess(const BleService& service) { return service.OwnerProductAccess(); }
    static void RequireOwnerProof(BleService& service, bool required) {
        service.m_ownerPreviewEnabled = required;
    }
    static uint8_t OwnerWireFlags(const BleService& service) { return service.m_ownerStateBytes[3U]; }
    static bool OwnerWireReady(const BleService& service) {
        return service.m_ownerStateValid && (service.m_ownerStateBytes[3U] &
            owner_pairing_session_wire::ConnectionOwnerReady) != 0U;
    }
    static owner_pairing_session_wire::Reason OwnerWireReason(const BleService& service) {
        return static_cast<owner_pairing_session_wire::Reason>(service.m_ownerStateBytes[2U]);
    }
    static owner_pairing_session_wire::State OwnerWireState(const BleService& service) {
        return static_cast<owner_pairing_session_wire::State>(service.m_ownerStateBytes[1U]);
    }
    static bool FillOwnerPriority(BleService& service) {
        BleEvent event; event.type = BleEventType::SettingWrite;
        for (unsigned i = 0U; i < BleEventQueue::PRIORITY_CAPACITY; ++i)
            if (!service.m_eventQueue.Enqueue(event)) return false;
        return true;
    }
    static bool OwnerSettingsCharacteristics(BleService& service, NimBLEService& settings) {
        return service.m_bleSettings.CreateCharacteristics(settings);
    }
    static bool OwnerNativeQuiescent(const BleService& service) { return service.OwnerNativeLinksQuiescent(); }
    static bool NativePairingCandidate(const BleService& service, uint16_t handle) {
        return service.OwnerNativePairingCandidate(handle);
    }
    static bool Configure(BleService& service,
                          SettingsStore& settings,
                          ControlMutationService& controlMutation) {
        const bool queueReady = service.m_eventQueue.Begin() &&
                                service.m_connectionProjection.Begin() &&
                                service.m_otaChunkQueue.Begin();
        service.m_settings = &settings;
        service.m_bleSettings.Configure(
            &settings, &service.m_eventQueue, &service.m_droppedEvents);
        service.SetControlMutationService(&controlMutation);
        service.m_lastClientActivityMs = 41U;
        return queueReady;
    }

    static void ConfigureOtaNotifications(BleService& service,
                                          NimBLECharacteristic& characteristic) {
        service.m_otaControlCharacteristic = &characteristic;
    }
    static void ConfigureConnectionSessionNotifications(
        BleService& service, NimBLECharacteristic& characteristic) {
        service.m_connectionSessionEgressCharacteristic = &characteristic;
    }
    static bool InjectPrivateSupport(BleService& service) {
        const uint8_t privateBytes[] = {1U, 2U, 3U};
        return service.m_connectionSession.PublishReverseObject(0x44U, 99U, privateBytes, sizeof(privateBytes));
    }
    static bool RecoveryHasNoDomainBindings(const BleService& service) {
        return service.m_recoveryOnly && service.m_settings == nullptr &&
            service.m_litchcraftGame == nullptr && service.m_settingsService == nullptr &&
            !service.m_consoleStreamSubscribed.load() &&
            !service.m_litchcraftSubscribed.load();
    }
    static void MarkInitialized(BleService& service) { service.m_initialized = true; }
    static bool OwnerWasBooted(const BleService& service) { return service.m_ownerBooted; }
    static void MarkConnected(BleService& service, const bool connected) {
        service.m_connected.store(connected, std::memory_order_release);
    }
    static void SetDeviceName(BleService& service, const char* name) {
        service.m_deviceName = fxe::FixedString<32>(name);
    }
    static bool PendingReboot(const BleService& service) { return service.m_otaPendingReboot; }
    static void PollOtaInactivity(BleService& service) { service.UpdateOtaInactivityTimeout(); }
    static uint32_t RebootArmedAtMs(const BleService& service) {
        return service.m_otaRebootArmedAtMs;
    }
    static bool RebootGraceArmed(const BleService& service) {
        return service.m_otaRebootGraceArmed;
    }
    static uint32_t LastPublishedReverseSequence(const BleService& service) {
        return service.m_connectionSession.LastPublishedReverseObjectSequence();
    }
    static void AttachRuntimeHealth(BleService& service,
                                    runtime::RuntimeHealthReducer& health,
                                    RuntimeHealthRecheckProbe& probe) {
        service.m_runtimeHealth = &health;
        service.SetRuntimeHealthRecheckCallback(&RunRuntimeHealthRecheckProbe,
                                                &probe);
    }
    static void NotifyProgress(BleService& service) {
        service.NotifyOtaStatus("progress");
    }
    static void NotifyTimeoutError(BleService& service) {
        service.NotifyOtaStatus("error", "timeout");
    }

    static bool DrainConsoleEvent(BleService& service) {
        BleEvent event;
        if (!service.m_eventQueue.TryDequeue(event) ||
            event.type != BleEventType::ConsoleInput) {
            return false;
        }
        service.HandleConsoleInputEvent(event.payload.consoleInput);
        return true;
    }

    static bool DiagnosticEnabled(const BleService& service) { return service.m_enabled; }
    static void SubscribeDiagnosticState(BleService& service) {
        service.m_connectionSessionSubscribed.store(true);
    }
    static uint32_t PhysicalInputRevision(const BleService& service) {
        return service.m_connectionPhysicalInputRevision;
    }
    static void UpdateSharedStates(BleService& service) {
        service.UpdateConnectionSessionNotifications();
    }
    static uint32_t LitchcraftPublicationRevision(const BleService& service) {
        return service.m_connectionSessionLitchcraftStateRevision;
    }
    static bool LitchcraftPublicationValid(const BleService& service) {
        return service.m_connectionSessionLitchcraftStateValid;
    }
    static const std::array<uint8_t, 10U>& LitchcraftPublicationBody(
        const BleService& service) {
        return service.m_connectionSessionLitchcraftState;
    }
    static void SetPublicationRevision(BleService& service, uint32_t revision) {
        service.m_connectionSessionLitchcraftStateRevision = revision;
    }
    static void PrepareProducerGeneration(BleService& service) {
        while (service.m_bleSettings.CurrentConnectionEpoch() < 7U) {
            (void)service.m_bleSettings.AdvanceConnectionEpoch();
        }
        (void)service.m_bleSettings.ActivateConnectionEpoch(7U);
    }
    static void FillStatePublicationCapacity(BleService& service) {
        (void)service.m_connectionSession.Begin(
            service.m_bleSettings.CurrentConnectionEpoch(), 77U, 20U,
            ConnectionSession::LITCHCRAFT_ORDERED_TOPIC);
        const uint8_t value = 1U;
        for (uint8_t code = 100U; code < 100U + ConnectionSession::STATE_CAPACITY; ++code) {
            (void)service.m_connectionSession.PublishState(code, 1U, &value, 1U);
        }
    }
    static bool TakeLitchcraftState(BleService& service,
                                   std::array<uint8_t, 10U>& body, uint32_t& revision) {
        namespace wire = connection_session_codec;
        std::array<uint8_t, 20U> bytes{};
        const uint32_t epoch = service.m_bleSettings.CurrentConnectionEpoch();
        for (uint8_t attempt = 0U; attempt < 32U; ++attempt) {
            size_t size = 0U;
            if (!service.m_connectionSession.PeekOutput(epoch, bytes.data(), bytes.size(), size)) return false;
            wire::PacketView packet;
            if (wire::Codec::Decode({bytes.data(), size}, packet, 20U) != wire::Error::None) return false;
            const bool selected = packet.header.kind == wire::Kind::State &&
                packet.header.code == ConnectionSession::LITCHCRAFT_STATE_CODE &&
                packet.body.Size() == body.size();
            if (selected) {
                std::copy_n(packet.body.Data(), body.size(), body.begin());
                revision = packet.header.sequence;
            }
            service.m_connectionSession.AcceptOutput(epoch);
            if (selected) return true;
        }
        return false;
    }
    static uint32_t ProducerEpoch(const BleService& service) {
        return service.m_bleSettings.CurrentConnectionEpoch();
    }

    static unsigned DrainQueuedEvents(BleService& service) {
        unsigned count = 0U;
        BleEvent event;
        while (service.m_eventQueue.TryDequeue(event)) ++count;
        return count;
    }

    static void ResetStackGeneration(BleService& service) {
        service.m_bleSettings.Reset();
    }

    static void ShutdownLifetime(BleService& service) { service.ShutdownStack(); }
    static bool PortableSessionActive(const BleService& service) {
        return service.m_connectionSession.IsActivated();
    }
    static uint32_t ConnectionToken(const BleService& service) {
        return service.m_connectionSession.ConnectionToken();
    }
    static uint32_t OrderedToken(const BleService& service) {
        return service.m_connectionSession.StreamToken();
    }
    static ConnectionSession::Result AdmitSharedOrdered(
        BleService& service, const uint8_t* bytes, const size_t size) {
        return service.m_connectionSession.Ingress(
            service.m_bleSettings.CurrentConnectionEpoch(), bytes, size);
    }

    static bool TakeOrderedCheckpoint(BleService& service, uint32_t& sequence, uint16_t& exceptions,
                                      uint32_t* domainRevision = nullptr) {
        namespace wire = connection_session_codec;
        std::array<uint8_t, 20U> bytes{};
        const auto epoch = service.m_bleSettings.CurrentConnectionEpoch();
        for (uint8_t attempt = 0U; attempt < 8U; ++attempt) {
            size_t size = 0U;
            wire::PacketView packet;
            if (!service.m_connectionSession.PeekOutput(epoch, bytes.data(), bytes.size(), size) ||
                wire::Codec::Decode({bytes.data(), size}, packet, 20U) != wire::Error::None) return false;
            service.m_connectionSession.AcceptOutput(epoch);
            if (packet.header.kind == wire::Kind::Control &&
                packet.header.code == uint8_t(wire::Control::Checkpoint) &&
                packet.header.token == service.m_connectionSession.StreamToken() && packet.body.Size() == 10U) {
                sequence = packet.header.sequence;
                exceptions = fxe::ReadU16LE(packet.body.Data() + 8U);
                if (domainRevision != nullptr) *domainRevision = fxe::ReadU32LE(packet.body.Data());
                return true;
            }
        }
        return false;
    }

    static bool TakeOrderedRejection(BleService& service, const uint32_t sequence,
                                     uint16_t* const reason = nullptr) {
        namespace wire = connection_session_codec;
        std::array<uint8_t, 20U> bytes{};
        const auto epoch = service.m_bleSettings.CurrentConnectionEpoch();
        for (uint8_t attempt = 0U; attempt < 8U; ++attempt) {
            size_t size = 0U;
            wire::PacketView packet;
            if (!service.m_connectionSession.PeekOutput(epoch, bytes.data(), bytes.size(), size) ||
                wire::Codec::Decode({bytes.data(), size}, packet, 20U) != wire::Error::None) return false;
            service.m_connectionSession.AcceptOutput(epoch);
            if (packet.header.kind == wire::Kind::Control &&
                packet.header.code == uint8_t(wire::Control::Outcome) &&
                packet.header.token == service.m_connectionSession.StreamToken() &&
                packet.header.sequence == sequence) {
                if (packet.body.Size() != 3U || packet.body[0U] != 1U) return false;
                const uint16_t decodedReason = fxe::ReadU16LE(packet.body.Data() + 1U);
                if (reason != nullptr) *reason = decodedReason;
                return decodedReason != 0U;
            }
        }
        return false;
    }

    static uint32_t ObjectToken(const BleService& service) {
        return service.m_connectionSession.ObjectStreamToken();
    }
    static bool TakeAdmissionProgress(BleService& service, uint32_t& accepted, uint32_t& grant) {
        namespace wire = connection_session_codec;
        std::array<uint8_t, ConnectionSession::REVERSE_OBJECT_PACKET_BYTES> bytes{};
        size_t size = 0U;
        while (service.m_connectionSession.PeekOutput(7U, bytes.data(), bytes.size(), size)) {
            wire::PacketView packet;
            if (wire::Codec::Decode({bytes.data(), size}, packet, 512U) != wire::Error::None) return false;
            const bool wanted = packet.header.kind == wire::Kind::Control &&
                packet.header.code == uint8_t(wire::Control::Progress) &&
                packet.header.token == service.m_connectionSession.ObjectStreamToken();
            if (wanted) { accepted = fxe::ReadU32LE(packet.body.Data()); grant = fxe::ReadU32LE(packet.body.Data() + 4U); }
            service.m_connectionSession.AcceptOutput(7U);
            if (wanted) return true;
        }
        return false;
    }
    static uint32_t PendingObjectSequence(const BleService& service) {
        ConnectionSession::ObjectCommand object;
        return service.m_connectionSession.PeekPendingObject(object) ? object.sequence : 0U;
    }
    static uint8_t RawOrdinaryCount(BleService& service) {
        BleEvent event;
        std::vector<BleEvent> events;
        while (service.m_eventQueue.TryPeekOrdinary(event) &&
               service.m_eventQueue.TryDequeueOrdinaryIfOrdinal(event.ingressOrdinal, event)) events.push_back(event);
        for (const auto& retained : events) service.m_eventQueue.Enqueue(retained);
        return static_cast<uint8_t>(events.size());
    }
    static uint32_t ChunkEpoch(const BleService& service) { return service.m_otaChunkQueue.CurrentEpoch(); }
    static bool DropOrdinary(BleService& service) {
        BleEvent event;
        return service.m_eventQueue.TryPeekOrdinary(event) &&
            service.m_eventQueue.TryDequeueOrdinaryIfOrdinal(event.ingressOrdinal, event);
    }
    static void DiscardOrdinary(BleService& service) { while (DropOrdinary(service)) {} }
    static bool QueueBulk(BleService& service, const uint8_t* bytes, size_t size) {
        return service.m_otaChunkQueue.Enqueue(bytes, size, 1U);
    }
    static bool HasBulkFailure(BleService& service) {
        BleOtaChunkQueueFailure failure;
        return service.m_otaChunkQueue.TryConsumeFailure(service.m_otaChunkQueue.CurrentEpoch(), failure);
    }
    static void DispatchObject(BleService& service, const uint8_t* bytes, size_t size, uint32_t ordinal) {
        ConnectionSession::ObjectCommand object;
        if (service.m_connectionSession.Ingress(7U, bytes, size, &object, ordinal) == ConnectionSession::Result::Ok && object.streamToken != 0U)
            service.DispatchConnectionObject(object, 7U);
    }
    static bool TakeDerived(BleService& service, BleEvent& event) { return service.m_eventQueue.TryDequeue(event); }
    static void Handle(BleService& service, const BleEvent& event) { service.HandleEvent(event); }
    static ConnectionSession& Session(BleService& service) { return service.m_connectionSession; }
    static uint8_t StoreQueueHighWater(const BleService& service) { return service.m_otaChunkQueue.HighWaterMark(); }
    static void FillIngress(BleService& service, const bool priority) {
        BleEvent event;
        event.type = priority ? BleEventType::SettingWrite : BleEventType::OtaControl;
        if (!priority) event.payload.otaControl = BleOtaControlEvent{};
        while (service.m_eventQueue.Enqueue(event)) {}
    }

    static uint32_t LastClientActivityMs(const BleService& service) {
        return service.m_lastClientActivityMs;
    }

    static bool ConfigureRuntimeHealth(
        BleService& service,
        runtime::RuntimeHealthReducer& health,
        NimBLECharacteristic& characteristic,
        RuntimeHealthRecheckProbe& recheckProbe) {
        if (!service.m_eventQueue.Begin() ||
            !service.m_connectionProjection.Begin()) {
            return false;
        }
        const uint32_t epoch = service.m_bleSettings.AdvanceConnectionEpoch();
        if (!service.m_bleSettings.ActivateConnectionEpoch(epoch)) {
            return false;
        }
        service.m_connected.store(true, std::memory_order_release);
        service.m_otaControlCharacteristic = &characteristic;
        service.m_runtimeHealth = &health;
        service.SetRuntimeHealthRecheckCallback(
            &RunRuntimeHealthRecheckProbe, &recheckProbe);
        return true;
    }

    static bool DispatchQueuedHealth(BleService& service,
                                     uint32_t& outConnectionEpoch) {
        outConnectionEpoch = 0U;
        BleEvent event;
        while (service.m_eventQueue.TryDequeue(event)) {
            if (event.type == BleEventType::OtaControl) {
                outConnectionEpoch = event.payload.otaControl.connectionEpoch;
                return service.HandleRuntimeHealthRequest(
                    event.payload.otaControl.input,
                    event.payload.otaControl.len,
                    event.payload.otaControl.connectionEpoch);
            }
        }
        return false;
    }

    static void ResetRuntimeHealthConnectionGeneration(BleService& service) {
        service.OnDisconnected();
        service.OnConnected(7U, "replacement", 23U, 1U);
        service.m_bleSettings.Reset();
    }

    static uint32_t CurrentConnectionEpoch(const BleService& service) {
        return service.m_bleSettings.CurrentConnectionEpoch();
    }

    static uint16_t ConnectionSessionPacketMaximum(const BleService& service) {
        return service.m_connectionSession.PacketMaximum();
    }

    static void ConfigureLitchcraft(BleService& service,
                                    GamePersistenceCoordinator& game,
                                    NimBLECharacteristic& characteristic) {
        (void)service.m_eventQueue.Begin();
        (void)service.m_connectionProjection.Begin();
        (void)service.m_otaChunkQueue.Begin();
        service.m_litchcraftGame = &game;
        service.m_litchcraftCharacteristic = &characteristic;
        service.m_litchcraftEligible = true;
        service.m_connectionHandle = 7U;
        service.m_connected.store(true);
        (void)service.m_bleSettings.ActivateConnectionEpoch(
            service.m_bleSettings.AdvanceConnectionEpoch());
        game.Litchcraft().BeginConnection();
        service.OnLitchcraftSubscription(1U);
    }
    static std::string DeviceInfoPayload(
        BleService& service,
        GamePersistenceCoordinator& game,
        NimBLECharacteristic& status,
        NimBLECharacteristic& craft,
        NimBLECharacteristic& data) {
        service.m_litchcraftGame = &game;
        service.m_litchcraftCharacteristic = &craft;
        service.m_otaDataCharacteristic = &data;
        service.m_otaControlCharacteristic = &status;
        service.m_connected.store(true, std::memory_order_release);
        service.m_mtu.store(23U, std::memory_order_release);
        service.m_hardwareRevision =
            ble_device_capabilities::HardwareRevision::V3;
        service.NotifyDeviceInfo();
        return status.getValue();
    }
    static void FlushLitchcraft(BleService& service) {
        service.UpdateLitchcraftNotifications();
    }

    static void ConfigureNotifyTest(BleService& service,
                                    NimBLECharacteristic& characteristic,
                                    NimBLECharacteristic* status = nullptr,
                                    const uint16_t mtu = 23U) {
        (void)service.m_connectionProjection.Begin();
        service.m_consoleStreamCharacteristic = &characteristic;
        service.m_connectionHandle = 7U;
        service.m_otaControlCharacteristic = status;
        service.m_mtu.store(mtu, std::memory_order_release);
        service.m_connected.store(true, std::memory_order_release);
        (void)service.m_bleSettings.ActivateConnectionEpoch(
            service.m_bleSettings.AdvanceConnectionEpoch());
    }

    static void ConfigureConsoleOutput(BleService& service,
                                       NimBLEServer& server,
                                       NimBLECharacteristic& characteristic,
                                       const std::string& bytes) {
        service.m_server = &server;
        service.m_connectionHandle = 7U;
        service.m_mtu.store(23U, std::memory_order_release);
        service.m_connected.store(true, std::memory_order_release);
        service.m_consoleStreamCharacteristic = &characteristic;
        (void)service.m_eventQueue.Begin();
        (void)service.m_connectionProjection.Begin();
        (void)service.m_bleSettings.ActivateConnectionEpoch(
            service.m_bleSettings.AdvanceConnectionEpoch());
        service.OnConsoleStreamSubscriptionChanged(1U);
        service.m_consoleOutputLen = bytes.size();
        std::memcpy(service.m_consoleOutputBuffer, bytes.data(), bytes.size());
    }

    static size_t FlushConsoleOutput(BleService& service) {
        service.FlushConsoleOutputBuffer();
        return service.m_consoleOutputLen;
    }
    static void WriteConsoleOutput(BleService& service,
                                   const std::string& bytes) {
        service.WriteConsoleOutput(bytes.data(), bytes.size());
    }
    static size_t ConsoleOutputSize(const BleService& service) {
        return service.m_consoleOutputLen;
    }
    static uint32_t IngressDrops(const BleService& service) {
        return service.m_droppedEvents.load(std::memory_order_acquire);
    }
    static uint32_t ConsoleOutputOverflows(const BleService& service) {
        return service.m_consoleOutputOverflows.load(std::memory_order_acquire);
    }

    static bool DigestRequestPending(const BleService& service) {
        return service.m_digestNotifyRequested;
    }

    static bool BeginControlledDigestNotification(BleService& service) {
        // Native tests bypass only ESP32 task startup and image hashing. The
        // provider state machine, producer, and session admission remain real.
        service.m_digestNotifyRequested = true;
        service.m_digestNotifyFragment = 0U;
        service.m_lastDigestNotifyState = FirmwareDigestState::Unavailable;
        return service.m_firmwareDigestProvider.TryBegin();
    }

    static void CompleteControlledDigest(
        BleService& service,
        const std::array<uint8_t, FirmwareDigestProvider::DIGEST_BYTES>& digest) {
        service.m_firmwareDigestProvider.Complete(digest.data());
    }

    static void UpdateDigestNotification(BleService& service) {
        service.UpdateFirmwareDigestNotification();
    }

    static bool TakeConnectionParameters(
        BleService& service,
        BleConnectionParametersEvent& outParameters) {
        BleEvent event;
        if (!service.m_eventQueue.TryDequeue(event) ||
            event.type != BleEventType::ConnectionParameters) {
            return false;
        }
        outParameters = event.payload.connectionParameters;
        return true;
    }

    static bool TakeConnectionChange(
        BleService& service,
        BleConnectionChangeEvent& outConnection) {
        return service.m_connectionProjection.TryTake(outConnection);
    }

    static bool HasSettingsSubscription(const BleService& service) {
        return service.m_bleSettings.HasReadWriteSubscription();
    }

    static void SubscribeSettings(BleService& service, const uint16_t value) {
        service.m_bleSettings.OnReadWriteSubscribe(value);
    }

    static bool HasLitchcraftSubscription(const BleService& service) {
        return service.m_litchcraftSubscribed.load(std::memory_order_acquire);
    }

    static bool HasOtaSubscription(const BleService& service) {
        return service.m_otaControlSubscribed.load(std::memory_order_acquire);
    }

    static void EnableAdvertisingRecovery(BleService& service,
                                          NimBLEAdvertising& advertising,
                                          NimBLEServer& server) {
        service.m_initialized = true;
        service.m_enabled = true;
        service.m_advertising = &advertising;
        service.m_server = &server;
    }

    static void SubscribeSettingsOnly(void* context) {
        auto& service = *static_cast<BleService*>(context);
        service.m_bleSettings.OnReadWriteSubscribe(1U);
    }

    static void SubscribeServiceChannelsOnly(void* context) {
        auto& service = *static_cast<BleService*>(context);
        service.OnLitchcraftSubscription(1U);
        service.OnConsoleStreamSubscriptionChanged(1U);
        service.OnOtaControlSubscriptionChanged(1U);
    }

    static void SubscribeAll(void* context) {
        SubscribeSettingsOnly(context);
        SubscribeServiceChannelsOnly(context);
    }

    static void UnsubscribeSettingsOnly(void* context) {
        auto& service = *static_cast<BleService*>(context);
        service.m_bleSettings.OnReadWriteSubscribe(0U);
    }

    static void UnsubscribeServiceChannelsOnly(void* context) {
        auto& service = *static_cast<BleService*>(context);
        service.OnLitchcraftSubscription(0U);
        service.OnConsoleStreamSubscriptionChanged(0U);
        service.OnOtaControlSubscriptionChanged(0U);
    }

    static void SubscribeDuringActivation(BleService& service) {
        service.m_bleSettings.m_subscriptionActivationTestHook = &SubscribeSettingsOnly;
        service.m_bleSettings.m_subscriptionActivationTestContext = &service;
        service.m_subscriptionActivationTestHook = &SubscribeServiceChannelsOnly;
        service.m_subscriptionActivationTestContext = &service;
    }

    static void UnsubscribeDuringActivation(BleService& service) {
        service.m_bleSettings.m_subscriptionActivationTestHook = &UnsubscribeSettingsOnly;
        service.m_bleSettings.m_subscriptionActivationTestContext = &service;
        service.m_subscriptionActivationTestHook = &UnsubscribeServiceChannelsOnly;
        service.m_subscriptionActivationTestContext = &service;
    }

    static void ReplaceAndWriteDuringDispatch(void* context) {
        auto& service = *static_cast<BleService*>(context);
        service.OnConnected(11U, "dispatch-race", 247U, 1U);
        SubscribeAll(&service);
        service.m_bleSettings.OnReadWritePayload(
            "{\"a\":\"new_game\",\"i\":901}");
    }

    static void ReplaceDuringNextIngressDispatch(BleService& service) {
        service.m_beforeIngressDispatchTestHook = &ReplaceAndWriteDuringDispatch;
        service.m_beforeIngressDispatchTestContext = &service;
    }

    static void DisconnectReconnectAndBeginOta(void* context) {
        auto& service = *static_cast<BleService*>(context);
        service.OnDisconnected(19);
        service.OnConnected(13U, "ota-replacement", 247U, 1U);
        service.OnOtaControlSubscriptionChanged(1U);
        constexpr const char* begin =
            R"({"cmd":"ota_sim_begin","size":200,"chunk":100})";
        service.OnOtaControlCharacteristicWrite(begin, std::strlen(begin));
    }

    static void ReplaceWithOtaDuringNextIngressDispatch(BleService& service) {
        service.m_beforeIngressDispatchTestHook = &DisconnectReconnectAndBeginOta;
        service.m_beforeIngressDispatchTestContext = &service;
    }

    static void AdvanceIngressClock(void*) {
        fxe::ManualMillis::AdvanceMs(100U);
    }

    static void BeforeNextIngressDispatch(BleService& service,
                                          void (*hook)(void*), void* context) {
        service.m_beforeIngressDispatchTestHook = hook;
        service.m_beforeIngressDispatchTestContext = context;
    }

    static void LimitNextIngressDrainToOne(BleService& service) {
        service.m_beforeIngressDispatchTestHook = &AdvanceIngressClock;
        service.m_beforeIngressDispatchTestContext = nullptr;
    }

    static void HandleConsolePacket(BleService& service, const char* payload) {
        service.HandleConsoleInputPacket(payload, std::strlen(payload));
    }

    static void HandleConsolePacket(BleService& service, const char* payload,
                                    const size_t length) {
        service.HandleConsoleInputPacket(payload, length);
    }

    static void HandleOtaPacket(BleService& service, const char* payload,
                                const size_t length) {
        service.HandleOtaControlPacket(
            payload, length, 1U, service.m_bleSettings.CurrentConnectionEpoch());
    }

    static bool HomeSnapshotRequestPending(const BleService& service) {
        return service.m_consoleStreamSnapshotRequested.load(
            std::memory_order_acquire);
    }

    static bool TakeHomeSnapshotRequest(BleService& service) {
        return service.m_consoleStreamSnapshotRequested.exchange(
            false, std::memory_order_acq_rel);
    }

    static bool HandleConnectionGameHistoryFragment(
        BleService& service, const uint8_t* bytes, const size_t size,
        const uint32_t connectionEpoch = 7U) {
        ConnectionSession::ObjectCommand object;
        object.opcode = ConnectionSession::GAME_HISTORY_OBJECT_OPCODE;
        object.body = fxe::ConstByteSpan(bytes, size);
        return service.HandleConnectionGameHistoryObject(
            object, connectionEpoch) == BleService::ObjectAdmission::Accepted;
    }

#if defined(LITCH_BLE_OTA)
    static void SetBenchmarkPhysicalHandle(BleService& service, uint16_t handle) {
        service.m_connectionHandle = handle;
    }
    static void SetBenchmarkNotifyCounters(BleService& service, uint32_t attempts, uint32_t failures) {
        service.m_sessionDiagnostics.notifyAttempts = attempts;
        service.m_sessionDiagnostics.notifyFailures = failures;
    }
    static bool BenchmarkReverseWindowFull(BleService& service) {
        const uint8_t byte = 1U;
        return !service.m_connectionSession.PublishReverseObject(0x7fU, 99U, &byte, 1U);
    }
    static bool QueueBenchmarkPressure(BleService& service, const char* json) {
        return service.QueueConnectionOtaStatus(reinterpret_cast<const uint8_t*>(json), std::strlen(json));
    }
    static bool BenchmarkHasReliablePressure(const BleService& service) {
        return service.m_connectionOtaStatusPendingSize > 0U &&
            !service.m_connectionOtaStatusReplaceable && !service.CanRetainConnectionOtaStatus();
    }
    static std::string BenchmarkPendingStatus(const BleService& service) {
        return std::string(reinterpret_cast<const char*>(service.m_connectionOtaStatusPending.data()),
                           service.m_connectionOtaStatusPendingSize);
    }
    static bool BenchmarkActive(const BleService& service) { return service.m_objectBenchmark.IsActive(); }
    static uint32_t BenchmarkCount(const BleService& service) { return service.m_objectBenchmark.Count(); }
    static uint16_t BenchmarkReverseMaximum(const BleService& service) { return service.m_connectionSession.ReverseObjectMaximumBody(); }
    static void RetireBenchmarkSession(BleService& service) { service.ClearPendingConnectionControlAction(); }
    static bool ActivateMinimumConnectionObjectSession(
        BleService& service, const uint32_t generation = 7U,
        const uint16_t packetMaximum = 20U, const uint8_t objectCapacity = 16U,
        const uint16_t reverseBody = 10U, uint16_t* actualForwardBody = nullptr) {
        namespace wire = connection_session_codec;
        if (!service.m_connectionSession.Begin(
                generation, UINT64_C(0x8899aabbccddeeff), packetMaximum,
                ConnectionSession::LITCHCRAFT_ORDERED_TOPIC)) {
            return false;
        }
        auto encode = [](const wire::PacketView& packet,
                         std::array<uint8_t, 20U>& bytes,
                         size_t& size) {
            return wire::Codec::Encode(
                packet, {bytes.data(), bytes.size()}, size) ==
                wire::Error::None;
        };
        auto take = [&](std::array<uint8_t, 20U>& bytes,
                        wire::PacketView& packet) {
            size_t size = 0U;
            if (!service.m_connectionSession.PeekOutput(
                    generation, bytes.data(), bytes.size(), size) ||
                wire::Codec::Decode({bytes.data(), size}, packet, 20U) !=
                    wire::Error::None) {
                return false;
            }
            service.m_connectionSession.AcceptOutput(generation);
            return true;
        };

        std::array<uint8_t, 10U> helloBody{};
        fxe::WriteU64LE(UINT64_C(0x1122334455667788), helloBody.data());
        fxe::WriteU16LE(packetMaximum, helloBody.data() + 8U);
        std::array<uint8_t, 20U> helloBytes{};
        size_t helloSize = 0U;
        if (!encode({{wire::Kind::Control, uint8_t(wire::Control::Hello),
                      0U, 0U},
                     {helloBody.data(), helloBody.size()}},
                    helloBytes, helloSize) ||
            service.m_connectionSession.Ingress(
                generation, helloBytes.data(), helloSize) !=
                ConnectionSession::Result::Ok) {
            return false;
        }
        std::array<uint8_t, 20U> welcomeBytes{};
        std::array<uint8_t, 20U> identityBytes{};
        wire::PacketView welcome;
        wire::PacketView identity;
        if (!take(welcomeBytes, welcome) || !take(identityBytes, identity)) {
            return false;
        }
        std::array<uint8_t, 60U> transcript{};
        std::copy_n(helloBytes.begin(), helloSize, transcript.begin());
        std::copy(welcomeBytes.begin(), welcomeBytes.end(),
                  transcript.begin() + 20U);
        std::copy(identityBytes.begin(), identityBytes.end(),
                  transcript.begin() + 40U);
        std::array<uint8_t, 4U> crc{};
        fxe::WriteU32LE(fxe::Crc32(transcript.data(), transcript.size()),
                        crc.data());
        std::array<uint8_t, 20U> ingress{};
        size_t ingressSize = 0U;
        if (!encode({{wire::Kind::Control, uint8_t(wire::Control::Ready),
                      welcome.header.token, 0U},
                     {crc.data(), crc.size()}},
                    ingress, ingressSize) ||
            service.m_connectionSession.Ingress(
                generation, ingress.data(), ingressSize) !=
                ConnectionSession::Result::Ok) {
            return false;
        }
        std::array<uint8_t, 20U> output{};
        wire::PacketView packet;
        if (!take(output, packet) ||
            packet.header.code != uint8_t(wire::Control::Activated) ||
            !take(output, packet) ||
            packet.header.code != uint8_t(wire::Control::Open) ||
            packet.header.sequence != 1U) {
            return false;
        }

        constexpr uint32_t stateStreamToken = 0x80000001U;
        std::array<uint8_t, 10U> openedBody{};
        fxe::WriteU32LE(stateStreamToken, openedBody.data());
        fxe::WriteU16LE(ConnectionSession::STATE_BODY_BYTES,
                        openedBody.data() + 4U);
        openedBody[6U] = ConnectionSession::STATE_CAPACITY;
        openedBody[7U] = 1U;
        if (!encode({{wire::Kind::Control, uint8_t(wire::Control::Opened),
                      welcome.header.token, 1U},
                     {openedBody.data(), openedBody.size()}},
                    ingress, ingressSize) ||
            service.m_connectionSession.Ingress(
                generation, ingress.data(), ingressSize) !=
                ConnectionSession::Result::Ok) {
            return false;
        }
        service.m_connectionSession.Update(generation);
        if (!take(output, packet) ||
            packet.header.code != uint8_t(wire::Control::Open) ||
            packet.header.sequence != 2U) {
            return false;
        }
        constexpr uint32_t reverseObjectToken = 0x80000002U;
        openedBody = {};
        fxe::WriteU32LE(reverseObjectToken, openedBody.data());
        fxe::WriteU16LE(reverseBody, openedBody.data() + 4U);
        openedBody[6U] = ConnectionSession::REVERSE_OBJECT_CAPACITY;
        openedBody[7U] = 1U;
        if (!encode({{wire::Kind::Control, uint8_t(wire::Control::Opened),
                      welcome.header.token, 2U},
                     {openedBody.data(), openedBody.size()}},
                    ingress, ingressSize) ||
            service.m_connectionSession.Ingress(
                generation, ingress.data(), ingressSize) !=
                ConnectionSession::Result::Ok) {
            return false;
        }

        std::array<uint8_t, 8U> openBody{};
        fxe::WriteU16LE(ConnectionSession::LITCHCRAFT_OBJECT_TOPIC,
                        openBody.data());
        openBody[2U] = 1U;
        openBody[3U] = objectCapacity;
        fxe::WriteU16LE(packetMaximum - 10U, openBody.data() + 4U);
        if (!encode({{wire::Kind::Control, uint8_t(wire::Control::Open),
                      welcome.header.token, 1U},
                     {openBody.data(), openBody.size()}},
                    ingress, ingressSize) ||
            service.m_connectionSession.Ingress(
                generation, ingress.data(), ingressSize) !=
                ConnectionSession::Result::Ok ||
            !take(output, packet) ||
            packet.header.code != uint8_t(wire::Control::Opened) ||
            service.m_connectionSession.ObjectStreamToken() == 0U) {
            return false;
        }
        if (actualForwardBody != nullptr) *actualForwardBody = fxe::ReadU16LE(packet.body.Data() + 4U);
        service.m_connected.store(true, std::memory_order_release);
        service.m_mtu.store(23U, std::memory_order_release);
        return true;
    }

    static bool ConfirmSharedReverseObject(BleService& service,
                                           const uint32_t sequence) {
        namespace wire = connection_session_codec;
        std::array<uint8_t, 10U> body{};
        fxe::WriteU32LE(sequence, body.data());
        fxe::WriteU32LE(
            sequence + ConnectionSession::REVERSE_OBJECT_CAPACITY,
            body.data() + 4U);
        std::array<uint8_t, 20U> bytes{};
        size_t size = 0U;
        if (wire::Codec::Encode(
                {{wire::Kind::Control, uint8_t(wire::Control::Progress),
                  service.m_connectionSession.ReverseObjectStreamToken(),
                  sequence},
                 {body.data(), body.size()}},
                {bytes.data(), bytes.size()}, size) != wire::Error::None) {
            return false;
        }
        return service.m_connectionSession.Ingress(
                   service.m_connectionSession.Generation(), bytes.data(), size) == ConnectionSession::Result::Ok;
    }

    static bool OneUpdateFillsReverseWindow(BleService& service) {
        service.UpdateConnectionSessionNotifications();
        const std::array<uint8_t, 1U> sentinel{{1U}};
        return !service.m_connectionSession.PublishReverseObject(
            0x7fU, 1U, sentinel.data(), sentinel.size());
    }

    static bool FillSharedReverseWindow(BleService& service) {
        const std::array<uint8_t, 1U> sentinel{{0xa5U}};
        for (uint32_t index = 1U;
             index <= ConnectionSession::REVERSE_OBJECT_CAPACITY; ++index) {
            if (!service.m_connectionSession.PublishReverseObject(
                    0x7fU, index, sentinel.data(), sentinel.size())) {
                return false;
            }
        }
        return !service.m_connectionSession.PublishReverseObject(
            0x7fU, 99U, sentinel.data(), sentinel.size());
    }

    static bool ReleaseSharedReverseWindow(BleService& service) {
        namespace wire = connection_session_codec;
        for (uint8_t index = 0U;
             index < ConnectionSession::REVERSE_OBJECT_CAPACITY; ++index) {
            std::array<uint8_t, 20U> bytes{};
            size_t size = 0U;
            wire::PacketView packet;
            if (!service.m_connectionSession.PeekOutput(
                    7U, bytes.data(), bytes.size(), size) ||
                wire::Codec::Decode({bytes.data(), size}, packet, 20U) !=
                    wire::Error::None || packet.header.code != 0x7fU ||
                packet.body.Size() != 1U || packet.body.Data()[0U] != 0xa5U) {
                return false;
            }
            service.m_connectionSession.AcceptOutput(7U);
            if (!ConfirmSharedReverseObject(service, packet.header.sequence)) {
                return false;
            }
        }
        return true;
    }

    static uint32_t SendSharedReverseWindowWithoutConfirm(BleService& service) {
        namespace wire = connection_session_codec;
        uint32_t lastSequence = 0U;
        for (uint8_t index = 0U;
             index < ConnectionSession::REVERSE_OBJECT_CAPACITY; ++index) {
            std::array<uint8_t, 20U> bytes{};
            size_t size = 0U;
            wire::PacketView packet;
            if (!service.m_connectionSession.PeekOutput(
                    7U, bytes.data(), bytes.size(), size) ||
                wire::Codec::Decode({bytes.data(), size}, packet, 20U) !=
                    wire::Error::None || packet.header.code != 0x7fU) return 0U;
            lastSequence = packet.header.sequence;
            service.m_connectionSession.AcceptOutput(7U);
        }
        return lastSequence;
    }

    static bool QueueSharedReverseProgress(BleService& service,
                                           const uint32_t sequence) {
        namespace wire = connection_session_codec;
        std::array<uint8_t, 10U> body{};
        fxe::WriteU32LE(sequence, body.data());
        fxe::WriteU32LE(sequence + ConnectionSession::REVERSE_OBJECT_CAPACITY,
                        body.data() + 4U);
        std::array<uint8_t, 20U> bytes{};
        size_t size = 0U;
        if (wire::Codec::Encode(
                {{wire::Kind::Control, uint8_t(wire::Control::Progress),
                  service.m_connectionSession.ReverseObjectStreamToken(), sequence},
                 {body.data(), body.size()}},
                {bytes.data(), bytes.size()}, size) != wire::Error::None) return false;
        service.OnConnectionSessionWrite(bytes.data(), size);
        return true;
    }

    static void NotifyOtaJson(BleService& service,
                              NimBLECharacteristic& legacy,
                              const char* json) {
        service.m_otaControlCharacteristic = &legacy;
        service.NotifyOtaJson(json);
    }

    static bool TakeSharedOtaStatusFragment(BleService& service,
                                            std::vector<uint8_t>& body) {
        namespace wire = connection_session_codec;
        service.UpdateConnectionOtaStatusOutput();
        std::array<uint8_t,
                   ConnectionSession::REVERSE_OBJECT_PACKET_BYTES> bytes{};
        size_t size = 0U;
        if (!service.m_connectionSession.PeekOutput(
                7U, bytes.data(), bytes.size(), size)) {
            return false;
        }
        wire::PacketView packet;
        if (wire::Codec::Decode({bytes.data(), size}, packet, 20U) !=
                wire::Error::None ||
            packet.header.code != ConnectionSession::OTA_STATUS_RECEIPT_CODE) {
            return false;
        }
        body.assign(packet.body.Data(),
                    packet.body.Data() + packet.body.Size());
        service.m_connectionSession.AcceptOutput(7U);
        return ConfirmSharedReverseObject(service, packet.header.sequence);
    }

    static bool TakeSharedStatusBesideState(BleService& service,
                                          std::vector<uint8_t>& body) {
        namespace wire = connection_session_codec;
        const uint32_t generation = service.m_bleSettings.CurrentConnectionEpoch();
        service.UpdateConnectionOtaStatusOutput();
        for (unsigned attempt = 0U; attempt < 64U; ++attempt) {
            std::array<uint8_t, ConnectionSession::REVERSE_OBJECT_PACKET_BYTES> bytes{};
            size_t size = 0U;
            wire::PacketView packet;
            if (!service.m_connectionSession.PeekOutput(generation, bytes.data(), bytes.size(), size) ||
                wire::Codec::Decode({bytes.data(), size}, packet, 20U) != wire::Error::None) return false;
            const bool selected = packet.header.kind == wire::Kind::Object &&
                packet.header.code == ConnectionSession::OTA_STATUS_RECEIPT_CODE;
            if (selected) body.assign(packet.body.Data(), packet.body.Data() + packet.body.Size());
            service.m_connectionSession.AcceptOutput(generation);
            if (packet.header.kind == wire::Kind::Object) {
                if (!QueueSharedReverseProgress(service, packet.header.sequence)) return false;
                service.DrainEventQueue();
            }
            if (selected) return true;
        }
        return false;
    }

    static bool HandleConnectionOtaData(
        BleService& service, const uint8_t* bytes, const size_t size,
        const uint32_t ingressOrdinal = 17U) {
        ConnectionSession::ObjectCommand object;
        object.opcode = ConnectionSession::OTA_DATA_OBJECT_OPCODE;
        object.streamToken = service.m_connectionSession.ObjectStreamToken();
        object.body = fxe::ConstByteSpan(bytes, size);
        const auto result = service.HandleConnectionOtaDataObject(object, ingressOrdinal);
        return result == BleService::ObjectAdmission::Accepted || result == BleService::ObjectAdmission::RetainedCredit;
    }

    static bool HandleConnectionOtaControl(
        BleService& service, const uint8_t* bytes, const size_t size,
        const uint32_t ingressOrdinal = 18U,
        const uint32_t connectionEpoch = 7U, const bool useStreamToken = false) {
        ConnectionSession::ObjectCommand object;
        object.opcode = ConnectionSession::OTA_CONTROL_OBJECT_OPCODE;
        if (useStreamToken) object.streamToken = service.m_connectionSession.ObjectStreamToken();
        object.body = fxe::ConstByteSpan(bytes, size);
        const auto result = service.HandleConnectionOtaControlObject(object, ingressOrdinal, connectionEpoch);
        return result == BleService::ObjectAdmission::Accepted || result == BleService::ObjectAdmission::RetainedCredit;
    }

    static bool HandleConnectionLitchcraft(
        BleService& service, const uint8_t* bytes, const size_t size,
        const uint32_t ingressOrdinal = 19U,
        const uint32_t connectionEpoch = 7U) {
        ConnectionSession::ObjectCommand object;
        object.opcode = ConnectionSession::LITCHCRAFT_OBJECT_OPCODE;
        object.streamToken = service.m_connectionSession.ObjectStreamToken();
        object.body = fxe::ConstByteSpan(bytes, size);
        const auto result = service.HandleConnectionLitchcraftObject(object, ingressOrdinal, connectionEpoch);
        return result == BleService::ObjectAdmission::Accepted || result == BleService::ObjectAdmission::RetainedCredit;
    }

    static bool TakeLitchcraftEvent(BleService& service,
                                    std::vector<uint8_t>& out,
                                    uint32_t& outOrdinal,
                                    uint32_t& outConnectionEpoch) {
        BleEvent event;
        if (!service.m_eventQueue.TryDequeue(event) ||
            (event.type != BleEventType::Litchcraft && event.type != BleEventType::ConnectionLitchcraft)) {
            return false;
        }
        const bool derived = event.type == BleEventType::ConnectionLitchcraft;
        if (derived) {
            out.assign(event.payload.connectionLitchcraft.input,
                       event.payload.connectionLitchcraft.input + event.payload.connectionLitchcraft.len);
            outOrdinal = event.ingressOrdinal;
            outConnectionEpoch = event.payload.connectionLitchcraft.objectCreditToken == 0U
                ? 7U : service.m_connectionSession.Generation();
            return true;
        }
        out.assign(event.payload.litchcraft.input,
                   event.payload.litchcraft.input +
                       event.payload.litchcraft.len);
        outOrdinal = event.ingressOrdinal;
        outConnectionEpoch = event.payload.litchcraft.connectionEpoch;
        return true;
    }

    static bool TakeSharedLitchcraftFragment(BleService& service,
                                             std::vector<uint8_t>& body,
                                             const bool frameStore = false) {
        namespace wire = connection_session_codec;
        if (frameStore) service.UpdateConnectionFrameStoreOutput();
        else service.UpdateConnectionLitchcraftOutput();
        std::array<uint8_t,
                   ConnectionSession::REVERSE_OBJECT_PACKET_BYTES> bytes{};
        wire::PacketView packet;
        for (uint8_t skipped = 0U;
             skipped <= ConnectionSession::STATE_CAPACITY; ++skipped) {
            size_t size = 0U;
            if (!service.m_connectionSession.PeekOutput(
                    7U, bytes.data(), bytes.size(), size) ||
                wire::Codec::Decode(
                    {bytes.data(), size}, packet, 20U) != wire::Error::None) {
                return false;
            }
            if (packet.header.kind == wire::Kind::Object &&
                packet.header.code ==
                    ConnectionSession::LITCHCRAFT_RECEIPT_CODE) {
                break;
            }
            if (packet.header.kind != wire::Kind::State) return false;
            service.m_connectionSession.AcceptOutput(7U);
        }
        if (packet.header.kind != wire::Kind::Object ||
            packet.header.code != ConnectionSession::LITCHCRAFT_RECEIPT_CODE) {
            return false;
        }
        body.assign(packet.body.Data(),
                    packet.body.Data() + packet.body.Size());
        service.m_connectionSession.AcceptOutput(7U);
        return ConfirmSharedReverseObject(service, packet.header.sequence);
    }

    static bool TakeOtaControl(BleService& service, std::string& out,
                               uint32_t& outOrdinal,
                               uint32_t& outSessionEpoch,
                               uint32_t& outConnectionEpoch) {
        BleEvent event;
        if (!service.m_eventQueue.TryDequeue(event) ||
            event.type != BleEventType::OtaControl) {
            return false;
        }
        out.assign(event.payload.otaControl.input,
                   event.payload.otaControl.len);
        outOrdinal = event.ingressOrdinal;
        outSessionEpoch = event.payload.otaControl.sessionEpoch;
        outConnectionEpoch = event.payload.otaControl.connectionEpoch;
        return true;
    }

    static bool HasQueuedEvent(BleService& service) {
        BleEvent event;
        return service.m_eventQueue.TryPeek(event);
    }

    static void ConfigureRenewalServer(BleService& service,
                                        const uint16_t nativeHandle = 0xffffU) {
        service.m_server = NimBLEDevice::getServer();
        if (nativeHandle != 0xffffU) {
            service.m_ownerNativeHandle.store(nativeHandle, std::memory_order_release);
        }
    }
    static void PollRenewal(BleService& service) { service.PollConnectionRenewal(); }
    static void SetPendingNativeDisconnect(BleService& service) {
        service.m_staleDisconnectIssued = true;
    }
    static bool RenewalKeepsActivation(const BleService& service) {
        return service.m_connectionSessionActivated.load() && service.m_staleDisconnectIssued;
    }
    static void PublishCapturedRenewal(BleService& service, uint32_t epoch, uint16_t handle) {
        service.m_connectionRenewalHandle.store(handle);
        service.m_connectionRenewalEpoch.store(epoch);
    }
    static void SetConnectionSessionActivated(BleService& service,
                                              const bool activated) {
        service.m_connectionSessionActivated.store(
            activated, std::memory_order_release);
    }

    static bool TakeOtaData(BleService& service,
                            std::vector<uint8_t>& out,
                            uint32_t& outOrdinal) {
        const uint8_t* bytes = nullptr;
        size_t size = 0U;
        uint32_t sessionEpoch = 0U;
        if (!service.m_otaChunkQueue.TryAcquireReadSlot(
                bytes, size, outOrdinal, sessionEpoch)) {
            return false;
        }
        out.assign(bytes, bytes + size);
        service.m_otaChunkQueue.ReleaseReadSlot();
        return true;
    }
#endif
};

} // namespace modules

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

struct Harness {
    modules::SettingsStore settings;
    modules::BleEventQueue eventQueue;
    std::atomic<uint32_t> droppedEvents{0U};
    modules::BleSettings bleSettings;
    modules::ControlMutationService controlMutation;
    NimBLEService service;
    modules::ControlCoordinatorResult undoStart =
        modules::ControlCoordinatorResult::Pending;
    modules::ControlCoordinatorResult newGameStart =
        modules::ControlCoordinatorResult::Applied;
    modules::ControlCoordinatorResult polled =
        modules::ControlCoordinatorResult::Pending;
    modules::ControlCoordinatorResult startGate =
        modules::ControlCoordinatorResult::Applied;
    modules::ControlActionState actionState =
        modules::ControlActionState::Idle;
    modules::ControlActionState actionStateAfterUndoRequest =
        modules::ControlActionState::Idle;
    uint32_t undoStartCount = 0U;
    uint32_t newGameStartCount = 0U;
    uint32_t styleProjectionCount = 0U;
    bool coachingP1 = false;
    bool coachingP2 = false;

    explicit Harness(const bool wireControlMutation = true,
                     const bool allowVolatileUserSettings = false)
        : settings([] {
              modules::SettingsStoreConfig cfg;
              cfg.path = "test_ble_settings_control_contract.json";
              return cfg;
          }()) {
        Check(eventQueue.Begin(), "BLE event queue begins");
        Check(settings.Load(), "settings.Load succeeds when missing");

        modules::ControlMutationCallbacks callbacks;
        callbacks.allowVolatileUserSettings = allowVolatileUserSettings;
        callbacks.context = this;
        callbacks.settingProjected = [](void* context, const char* key) {
            if (std::strcmp(key, "SETTLE_ANIM_STYLE") == 0 ||
                std::strcmp(key, "CHECKMATE_ANIM_STYLE") == 0)
                ++static_cast<Harness*>(context)->styleProjectionCount;
        };
        callbacks.checkStartOnlyGate = [](void* context, const char*) {
            return static_cast<Harness*>(context)->startGate;
        };
        callbacks.requestHint = [](void*, uint16_t) {
            return modules::ControlCoordinatorResult::Applied;
        };
        callbacks.requestUndo = [](void* context, uint16_t, uint8_t) {
            auto& self = *static_cast<Harness*>(context);
            ++self.undoStartCount;
            self.actionState = self.actionStateAfterUndoRequest;
            return self.undoStart;
        };
        callbacks.requestNewGame = [](void* context, uint16_t) {
            auto& self = *static_cast<Harness*>(context);
            ++self.newGameStartCount;
            return self.newGameStart;
        };
        callbacks.pollAction = [](void* context, uint16_t) {
            return static_cast<Harness*>(context)->polled;
        };
        callbacks.getActionState = [](void* context) {
            return static_cast<Harness*>(context)->actionState;
        };
        callbacks.coachingChanged = [](void* context,
                                          const modules::UiOwner owner,
                                          const bool enabled) {
            auto& self = *static_cast<Harness*>(context);
            if (owner == modules::UiOwner::P1) {
                self.coachingP1 = enabled;
            } else if (owner == modules::UiOwner::P2) {
                self.coachingP2 = enabled;
            }
        };
        controlMutation.Configure(settings, callbacks);

        bleSettings.Configure(&settings, &eventQueue, &droppedEvents);
        if (wireControlMutation) {
            bleSettings.SetControlMutationService(&controlMutation);
        }
        Check(bleSettings.CreateCharacteristics(service),
              "BLE settings characteristics create");
        bleSettings.OnReadWriteSubscribe(1U);
    }

    NimBLECharacteristic& ReadWriteCharacteristic() {
        return *service.CharacteristicAt(1U);
    }

    NimBLECharacteristic& EnumerateCharacteristic() {
        return *service.CharacteristicAt(0U);
    }

    bool PumpOneWriteEvent() {
        modules::BleEvent event;
        if (!eventQueue.TryDequeue(event)) {
            return false;
        }
        Check(event.type == modules::BleEventType::SettingWrite,
              "queued BLE event is a setting write");
        bleSettings.HandleSettingWriteEvent(settings, event.payload.setting);
        return true;
    }
};

void TestVolatilePhysicalEndColorsUseSetupBleContract() {
    Harness harness(true, true);
    NimBLEDevice::SetTestMtu(128U);
    harness.settings.DisableForBoot();
    const char* requests[] = {
        "{\"k\":\"P1_END_PIECE_WHEEL\",\"v\":85,\"i\":61}",
        "{\"k\":\"P2_END_PIECE_WHEEL\",\"v\":170,\"i\":62}",
    };
    const char* keys[] = {"P1_END_PIECE_WHEEL", "P2_END_PIECE_WHEEL"};
    for (size_t i = 0U; i < 2U; ++i) {
        harness.ReadWriteCharacteristic().ClearTx();
        harness.bleSettings.OnReadWritePayload(requests[i]);
        Check(harness.PumpOneWriteEvent(), "session-only Setup color reaches production write handling");
        const uint32_t expected = i == 0U ? 85U : 170U;
        Check(harness.settings.ReadValue(keys[i], 999U) == expected,
              "session-only Setup color changes authoritative RAM settings");
        StaticJsonDocument<256> response;
        Check(!deserializeJson(response, harness.ReadWriteCharacteristic().LastTx()),
              "session-only Setup color acknowledgement parses");
        Check((response["ok"] | false) && (response["i"] | 0U) == 61U + i &&
                  (response["v"] | 999U) == expected,
              "session-only Setup color acknowledgement confirms exact request and applied value");
    }
    Check(harness.settings.IsDisabled() && !harness.settings.SaveNow(),
          "successful volatile color never claims the disabled store is durable");
    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"LED_BROWNOUT_LIMIT_LATCH\",\"v\":false,\"i\":63}");
    Check(harness.PumpOneWriteEvent(), "internal-only write reaches guarded production handler");
    Check(harness.ReadWriteCharacteristic().LastTx().find("\"ok\":false") != std::string::npos,
          "session-only palette permission does not expose internal-only settings");
    Check(harness.controlMutation.MutateUsbCcThresholds(100U, 200U, 300U).status ==
              modules::ControlMutationStatus::Rejected,
          "session-only palette permission still rejects hardware calibration");
}

void TestSettingsRejectEmbeddedNulBeforeMutation() {
    Harness harness(true, true);
    const std::string prefix = "{\"k\":\"BRIGHTNESS\",\"v\":20}";
    const std::string nulPayload = prefix + std::string(1U, '\0') + "ignored";
    harness.bleSettings.OnReadWritePayload(nulPayload);
    Check(!harness.PumpOneWriteEvent(), "raw NUL must not enqueue valid JSON prefix");
    harness.bleSettings.OnReadWritePayload("{\"k\":\"BRIGHTNESS\\u0000ignored\",\"v\":20}");
    Check(!harness.PumpOneWriteEvent(), "escaped NUL must not alias a setting key");
    harness.bleSettings.OnReadWritePayload("{\"a\":\"new_game\\u0000ignored\",\"i\":41}");
    Check(!harness.PumpOneWriteEvent(), "escaped NUL must not alias a control action");
    harness.bleSettings.OnReadWritePayload(prefix);
    Check(harness.PumpOneWriteEvent(), "valid request still executes after malformed requests");
}

void TestConsoleAndOtaRejectEmbeddedNulAtProductionIngress() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    modules::BleService console;
    const std::string consolePrefix = R"({"cmd":"home_state"})";
    const std::string consoleRaw =
        consolePrefix + std::string(1U, '\0') + "ignored";
    Peer::HandleConsolePacket(console, consoleRaw.data(), consoleRaw.size());
    Check(!Peer::HomeSnapshotRequestPending(console),
          "console raw NUL cannot execute a valid JSON prefix");
    constexpr char consoleDecoded[] =
        R"({"cmd":"home_state\u0000ignored"})";
    Peer::HandleConsolePacket(
        console, consoleDecoded, sizeof(consoleDecoded) - 1U);
    Check(!Peer::HomeSnapshotRequestPending(console),
          "console decoded NUL cannot alias a command token");
    Peer::HandleConsolePacket(console, consolePrefix.c_str());
    Check(Peer::TakeHomeSnapshotRequest(console),
          "console accepts a valid command after rejected NUL packets");

    modules::BleService ota;
    runtime::RuntimeHealthReducer health;
    NimBLECharacteristic status;
    modules::RuntimeHealthRecheckProbe probe;
    Check(Peer::ConfigureRuntimeHealth(ota, health, status, probe),
          "OTA NUL ingress harness configures an active connection");
    const std::string otaPrefix = R"({"cmd":"fw_version"})";
    const std::string otaRaw = otaPrefix + std::string(1U, '\0') + "ignored";
    Peer::HandleOtaPacket(ota, otaRaw.data(), otaRaw.size());
    Check(status.LastTx().find("invalid_json") != std::string::npos,
          "OTA raw NUL rejects the full packet instead of its valid prefix");
    constexpr char otaDecoded[] = R"({"cmd":"fw_version\u0000ignored"})";
    Peer::HandleOtaPacket(ota, otaDecoded, sizeof(otaDecoded) - 1U);
    Check(status.LastTx().find("invalid_json") != std::string::npos,
          "OTA decoded NUL cannot alias a control command");
    Peer::HandleOtaPacket(ota, otaPrefix.data(), otaPrefix.size());
    Check(status.LastTx().find("{\"v\":") != std::string::npos,
          "OTA accepts a valid command after rejected NUL packets");
}

void TestMaximumValueEnumerationFromEveryOffset() {
    Harness harness;
    size_t descriptorCount = 0U;
    const auto* descriptors = modules::settings_registry::EnumerateAll(descriptorCount);
    std::vector<const fxe::SettingDescriptor*> visible;
    for (size_t index = 0U; index < descriptorCount; ++index) {
        const auto& descriptor = descriptors[index];
        if (!descriptor.IsVisibleToApp() || descriptor.IsInternal()) continue;
        visible.push_back(&descriptor);
        if (descriptor.IsRetired()) continue;
        if (descriptor.type == fxe::SettingType::UInt32) {
            harness.settings.Doc()[descriptor.key] = descriptor.max;
        } else if (descriptor.type == fxe::SettingType::Bool) {
            harness.settings.Doc()[descriptor.key] = false;
        } else if (descriptor.type == fxe::SettingType::Enum) {
            const char* longest = descriptor.defaultStr;
            for (size_t option = 0U; option < descriptor.allowedCount; ++option) {
                if (std::strlen(descriptor.allowedValues[option]) > std::strlen(longest)) {
                    longest = descriptor.allowedValues[option];
                }
            }
            harness.settings.Doc()[descriptor.key] = longest;
        } else {
            Check(false, "new visible string requires a maximum-length enumeration fixture");
        }
    }
    // Every starting offset exercises a different possible oversized trailing
    // descriptor, including its allocation before removal from the packet.
    for (size_t start = 0U; start < visible.size(); ++start) {
        size_t cursor = start;
        bool done = false;
        for (size_t reads = 0U; reads <= visible.size() && !done; ++reads) {
            size_t next = 0U;
            modules::ble_settings_enumerate::BuildResponse(
                harness.settings, harness.EnumerateCharacteristic(), cursor, next, done);
            const std::string payload = harness.EnumerateCharacteristic().getValue();
            StaticJsonDocument<4096> chunk;
            const auto error = deserializeJson(chunk, payload);
            Check(!error && payload.size() <= 511U,
                  "maximum-value enumeration fits the unchanged wire packet");
            const JsonArrayConst items = chunk["it"].as<JsonArrayConst>();
            Check(!items.isNull() && items.size() != 0U,
                  "maximum-value enumeration never returns an overflow fallback");
            if (error || items.isNull() || items.size() == 0U) break;
            for (const JsonObjectConst item : items) {
                while (cursor < visible.size() && visible[cursor]->IsRetired()) ++cursor;
                Check(cursor < visible.size() && item["i"].as<size_t>() == cursor &&
                          std::strcmp(item["k"] | "", visible[cursor]->key) == 0,
                      "maximum-value enumeration visits each descriptor once in order");
                ++cursor;
            }
            while (cursor < visible.size() && visible[cursor]->IsRetired()) ++cursor;
            Check(done ? next == 0U : next == cursor,
                  "enumeration cursor advances without losing the oversized descriptor");
        }
        Check(done && cursor == visible.size(),
              "maximum-value enumeration completes from every registry offset");
    }
}

void TestBoardSetupAssistanceUsesGenericBleContract() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    bool enumerationDone = false;
    bool sawRetiredWakeTiming = false;
    bool sawBoardSetupAssistance = false;
    for (size_t readCount = 0U;
         readCount < 64U && !enumerationDone;
         ++readCount) {
        harness.bleSettings.OnEnumerateRead(
            harness.EnumerateCharacteristic());
        const std::string payload =
            harness.EnumerateCharacteristic().getValue();
        StaticJsonDocument<4096> chunk;
        const DeserializationError error = deserializeJson(chunk, payload);
        Check(!error, "settings enumerate chunk parses");
        if (error) {
            break;
        }

        const JsonArrayConst items = chunk["it"].as<JsonArrayConst>();
        Check(!items.isNull(), "settings enumerate chunk contains items");
        for (const JsonObjectConst item : items) {
            const char* const key = item["k"] | "";
            if (std::strcmp(key, "P1_X_WAKE_HOLD_MS") == 0) {
                sawRetiredWakeTiming = true;
                continue;
            }
            if (std::strcmp(key, "BOARD_SETUP_ASSISTANCE") != 0) {
                continue;
            }

            sawBoardSetupAssistance = true;
            Check(item["i"].as<uint32_t>() == 28U,
                  "board setup assistance appends at visible index 28");
            Check(std::strcmp(item["t"] | "", "bool") == 0,
                  "board setup assistance enumerates as bool");
            Check(std::strcmp(item["c"] | "", "behavior") == 0,
                  "board setup assistance enumerates in behavior category");
            Check(item["v"].is<bool>() && item["v"].as<bool>() &&
                      item["df"].is<bool>() && item["df"].as<bool>(),
                  "board setup assistance enumerate value and default are on");
            Check(std::strcmp(item["d"] | "",
                              "Board setup assistance") == 0,
                  "board setup assistance enumerates the exact app title");
        }

        enumerationDone = chunk["dn"] | false;
    }
    Check(enumerationDone,
          "settings enumeration reaches its bounded terminal chunk");
    Check(!sawRetiredWakeTiming && sawBoardSetupAssistance,
          "settings enumeration omits retired wake timing and preserves appended setting index");

    const uint32_t baselineRevision = harness.settings.GetRevision();
    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"BOARD_SETUP_ASSISTANCE\",\"v\":false,\"i\":91}");
    Check(harness.PumpOneWriteEvent(),
          "board setup assistance correlated write reaches main-loop handling");
    Check(!harness.settings.ReadValue("BOARD_SETUP_ASSISTANCE", true) &&
              harness.settings.GetRevision() == baselineRevision + 1U,
          "board setup assistance write applies once through generic mutation");
    Check(harness.ReadWriteCharacteristic().LastTx().find(
              "\"i\":91") != std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"k\":\"BOARD_SETUP_ASSISTANCE\"") !=
                  std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"v\":false") != std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"ok\":true") != std::string::npos,
          "board setup assistance write ACK carries authoritative false readback");

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(harness.ReadWriteCharacteristic().LastTx() == "[28,false]",
          "board setup assistance change notification uses appended index 28");

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"BOARD_SETUP_ASSISTANCE\",\"i\":92}");
    Check(harness.PumpOneWriteEvent(),
          "board setup assistance correlated read reaches main-loop handling");
    Check(harness.settings.GetRevision() == baselineRevision + 1U &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"i\":92") != std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"k\":\"BOARD_SETUP_ASSISTANCE\"") !=
                  std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"v\":false") != std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"ok\":true") != std::string::npos,
          "board setup assistance correlated read returns persisted false without mutation");
}

void TestAppendedDisplaySettingsUseExtendedSettingsSnapshot() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    bool enumerationDone = false;
    bool sawIndicator = false;
    bool sawV3ButtonEmulation = false;
    bool sawLightSquareMode = false;
    bool sawLightSquareHue = false;
    bool sawLightSquareBrightness = false;
    for (size_t readCount = 0U; readCount < 64U && !enumerationDone;
         ++readCount) {
        harness.bleSettings.OnEnumerateRead(harness.EnumerateCharacteristic());
        StaticJsonDocument<4096> chunk;
        const DeserializationError error = deserializeJson(
            chunk, harness.EnumerateCharacteristic().getValue());
        Check(!error, "extended settings enumeration chunk parses");
        if (error) break;
        for (const JsonObjectConst item : chunk["it"].as<JsonArrayConst>()) {
            if (std::strcmp(item["k"] | "", "BLE_CONNECTION_INDICATOR") == 0) {
                sawIndicator = true;
                Check(item["i"].as<uint32_t>() == 32U &&
                          std::strcmp(item["t"] | "", "bool") == 0 &&
                          item["v"].as<bool>(),
                      "Bluetooth indicator appends at visible index 32");
            } else if (std::strcmp(item["k"] | "",
                                   "V3_BUTTON_LED_EMULATION") == 0) {
                sawV3ButtonEmulation = true;

            } else if (std::strcmp(item["k"] | "",
                                   "LIGHT_SQUARE_MODE") == 0) {
                sawLightSquareMode = true;
                Check(item["i"].as<uint32_t>() == 34U &&
                          std::strcmp(item["t"] | "", "enum") == 0 &&
                          std::strcmp(item["v"] | "", "white") == 0,
                      "light-square mode appends at index 34 and defaults to White");
            } else if (std::strcmp(item["k"] | "",
                                   "LIGHT_SQUARE_HUE") == 0) {
                sawLightSquareHue = true;
                Check(item["i"].as<uint32_t>() == 35U &&
                          item["mn"].as<uint32_t>() == 0U &&
                          item["mx"].as<uint32_t>() == 255U,
                      "light-square hue appends as a continuous wheel at index 35");
            } else if (std::strcmp(item["k"] | "",
                                   "LIGHT_SQUARE_BRIGHTNESS") == 0) {
                sawLightSquareBrightness = true;
                Check(item["i"].as<uint32_t>() == 36U &&
                          item["mn"].as<uint32_t>() == 1U &&
                          item["mx"].as<uint32_t>() == 8U &&
                          item["v"].as<uint32_t>() == 4U,
                      "light-square brightness appends at index 36 without a duplicate Off value");
            }
        }
        enumerationDone = chunk["dn"] | false;
    }
    Check(enumerationDone && !sawIndicator && !sawV3ButtonEmulation &&
              sawLightSquareMode && sawLightSquareHue && sawLightSquareBrightness,
          "enumeration removes both retired light options without shifting remaining indices");

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"BLE_CONNECTION_INDICATOR\",\"v\":false,\"i\":93}");
    Check(harness.PumpOneWriteEvent() &&
              !harness.settings.Doc().containsKey("BLE_CONNECTION_INDICATOR") &&
              harness.ReadWriteCharacteristic().LastTx().find("\"ok\":false") != std::string::npos,
          "removed connection light returns rejection without mutating settings");

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"V3_BUTTON_LED_EMULATION\",\"v\":false,\"i\":94}");
    Check(harness.PumpOneWriteEvent(),
          "V3 button emulation write reaches generic mutation handling");
    Check(!harness.settings.Doc().containsKey("V3_BUTTON_LED_EMULATION") &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"ok\":false") != std::string::npos,
          "retired V3 button emulation rejects a write without changing settings");

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(harness.ReadWriteCharacteristic().LastTx().empty(),
          "retired button-emulation slot does not emit setting changes");

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"LIGHT_SQUARE_MODE\",\"v\":\"color\",\"i\":95}");
    Check(harness.PumpOneWriteEvent() &&
              std::strcmp(harness.settings.ReadValue(
                              "LIGHT_SQUARE_MODE", "white"),
                          "color") == 0 &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"ok\":true") != std::string::npos,
          "light-square Color selection applies through acknowledged settings mutation");

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"LIGHT_SQUARE_HUE\",\"v\":91,\"i\":96}");
    Check(harness.PumpOneWriteEvent() &&
              harness.settings.ReadValue("LIGHT_SQUARE_HUE", 0U) == 91U,
          "light-square hue applies through the generic acknowledged control path");

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"LIGHT_SQUARE_BRIGHTNESS\",\"v\":0,\"i\":97}");
    Check(harness.PumpOneWriteEvent() &&
              harness.settings.ReadValue("LIGHT_SQUARE_BRIGHTNESS", 8U) == 8U &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"e\":\"out_of_range\"") != std::string::npos,
          "light-square brightness rejects a duplicate Off value without mutating the prior setting");
}

void TestSettingWritesUseServiceAndStableReasons() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);
    const uint32_t baselineRevision = harness.settings.GetRevision();

    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"BRIGHTNESS\",\"v\":6,\"i\":51}");
    Check(harness.PumpOneWriteEvent(),
          "correlated setting write reaches main-loop handling");
    Check(harness.settings.ReadValue("BRIGHTNESS", 0U) == 6U &&
              harness.settings.GetRevision() == baselineRevision + 1U,
          "BLE setting write applies once through the mutation service");
    Check(harness.ReadWriteCharacteristic().LastTx().find(
              "\"i\":51") != std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"ok\":true") != std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"v\":6") != std::string::npos,
          "service-applied BLE setting write returns correlated readback");

    harness.startGate = modules::ControlCoordinatorResult::NotAtStart;
    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"BOARD_ROTATE\",\"v\":true,\"i\":52}");
    Check(harness.PumpOneWriteEvent(),
          "start-only setting write reaches canonical validation");
    Check(!harness.settings.ReadValue("BOARD_ROTATE", false) &&
              harness.settings.GetRevision() == baselineRevision + 1U,
          "rejected BLE setting write has no partial setting effect");
    Check(harness.ReadWriteCharacteristic().LastTx().find(
              "\"e\":\"not_at_start\"") != std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"i\":52") != std::string::npos,
          "BLE setting rejection exposes the stable service reason");
}

void TestBoardNameUsesExactKeyWithoutShiftingSnapshotIndices() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"BLE_DEVICE_NAME\",\"v\":\"Club's Litch\",\"i\":95}");
    Check(harness.PumpOneWriteEvent(),
          "board rename reaches the production exact-key handler");
    Check(std::strcmp(harness.settings.ReadValue("BLE_DEVICE_NAME", ""),
                      "Club's Litch") == 0 &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"i\":95") != std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"ok\":true") != std::string::npos,
          "board rename persists and returns a correlated acknowledgement");

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"BLE_DEVICE_NAME\",\"i\":96}");
    Check(harness.PumpOneWriteEvent(),
          "board name exact-key read reaches the production handler");
    Check(harness.ReadWriteCharacteristic().LastTx().find(
              "\"i\":96") != std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"v\":\"Club's Litch\"") != std::string::npos,
          "board name exact-key read returns the board-owned value");
}

void TestDiscoveredBoardNameFitsRealSdkAdvertisingPayload() {
    Harness harness;
    struct NameCase { std::string stored; size_t advertisedBytes; };
    const NameCase names[] = {
        {"Foxie Litch", 11U}, {"Comet 5-3.3", 11U},
        {"12345678901234567890", 20U},
        {std::string(11U, 'a') + u8"日本語星月火", 29U},
        {std::string(12U, 'a') + u8"日本語星月火", 27U},
        {std::string(13U, 'a') + u8"日本語星月火", 28U},
    };
    for (const auto& name : names) {
        harness.settings.Doc()["BLE_DEVICE_NAME"] = name.stored;
        modules::BleService service;
        service.Begin(harness.settings);
        const auto* advertising = NimBLEDevice::getAdvertising();
        const std::string advertised = advertising->Name();
        Check(service.IsAdvertising() && advertised == name.stored.substr(0U, name.advertisedBytes),
              "discovery carries the custom name before connecting at actual SDK packet capacity");
        Check(NimBLEDevice::Name() == name.stored && std::string(service.GetDeviceName()) == name.stored,
              "advertising truncation preserves the full stored and GAP name");
        Check(modules::user_text::IsSafeDisplayName(advertised.data(), advertised.size(), 20U, 31U),
              "advertised name never splits a UTF-8 scalar at the legacy packet boundary");
        const auto scan = advertising->ScanData().getPayload();
        Check(scan.size() <= 31U && scan.size() >= 2U &&
                  scan[1U] == (name.stored.size() == name.advertisedBytes ? 0x09U : 0x08U),
              "scan response uses standard complete or shortened local-name semantics");
        const auto primary = advertising->getAdvertisementData().getPayload();
        const NimBLEUUID base{std::string(modules::ble_internal::BASE_SERVICE_UUID)};
        Check(primary.size() <= 31U && std::search(primary.begin(), primary.end(),
                  base.getValue(), base.getValue() + 16U) != primary.end(),
              "primary advertisement retains the base UUID for name-independent discovery");
        service.Stop();
    }
}

void TestBoardRenamePreservesPendingNativeConnection() {
    Harness harness;
    modules::BleService service;
    service.Begin(harness.settings);
    harness.settings.Doc()["BLE_DEVICE_NAME"] = "Comet 5-3.3";
    service.OnConnected(7U, "native-peer", 185U, 1U);
    Check(!service.IsConnected(), "native callback precedes main-loop connection activation");
    const unsigned before = ota_runtime_test::shutdowns;
    service.Update(harness.settings);
    service.Update(harness.settings);
    Check(ota_runtime_test::shutdowns == before,
          "rename never restarts the stack while a real native connection awaits activation");
    service.DrainEventQueue();
    Check(service.IsConnected(), "pending connection survives rename and becomes authoritative");
    service.OnDisconnected(7U, 0);
    service.DrainEventQueue();
    service.Update(harness.settings);
    Check(ota_runtime_test::shutdowns == before + 1U &&
              std::string(service.GetDeviceName()) == "Comet 5-3.3" &&
              NimBLEDevice::getAdvertising()->Name() == "Comet 5-3.3",
          "rename is published once after the surviving native link disconnects");
    service.Stop();
}

void TestBoardRenameDefersBleRestartUntilDisconnected() {
    Harness harness;
    modules::BleService service;
    Check(std::string(service.GetDeviceName()) == "Foxie Litch",
          "new BLE service uses the spaced product name before settings load");
    Check(modules::BleServiceConnectionEpochTestPeer::Configure(
              service, harness.settings, harness.controlMutation),
          "rename lifecycle service configures");
    service.Begin(harness.settings);
    modules::BleServiceConnectionEpochTestPeer::SetDeviceName(
        service, "Foxie_Litch");
    modules::BleServiceConnectionEpochTestPeer::MarkConnected(service, true);
    harness.settings.Doc()["BLE_DEVICE_NAME"] = "Studio Litch";

    const unsigned before = ota_runtime_test::shutdowns;
    service.Update(harness.settings);
    service.Update(harness.settings);
    Check(ota_runtime_test::shutdowns == before && service.IsConnected(),
          "connected rename preserves the live BLE/game session without restart spam");

    modules::BleServiceConnectionEpochTestPeer::MarkConnected(service, false);
    service.Update(harness.settings);
    Check(ota_runtime_test::shutdowns == before + 1U,
          "deferred board name applies once before the next advertising session");
}

void TestMissingServiceFailsClosed() {
    Harness harness(false);
    NimBLEDevice::SetTestMtu(128U);
    const uint32_t baselineRevision = harness.settings.GetRevision();

    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"BRIGHTNESS\",\"v\":6,\"i\":61}");
    Check(harness.PumpOneWriteEvent(),
          "miswired setting request still drains from the BLE queue");
    Check(harness.settings.ReadValue("BRIGHTNESS", 5U) == 5U &&
              harness.settings.GetRevision() == baselineRevision,
          "missing mutation service cannot raw-edit the settings document");
    Check(harness.ReadWriteCharacteristic().LastTx().find(
              "\"e\":\"unsupported\"") != std::string::npos &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"i\":61") != std::string::npos,
          "missing mutation service returns a correlated stable rejection");

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":62}");
    Check(harness.PumpOneWriteEvent(),
          "miswired action request drains from the BLE queue");
    Check(harness.newGameStartCount == 0U &&
              harness.ReadWriteCharacteristic().LastTx().find(
                  "\"r\":\"unsupported\"") != std::string::npos,
          "missing mutation service cannot execute a BLE action");
}

void TestExplicitHomeStateRequestIsReadOnlyAndIdempotent() {
    modules::BleService service;
    modules::BleServiceConnectionEpochTestPeer::HandleConsolePacket(
        service, "{\"cmd\":\"home_state\"}");
    Check(
        modules::BleServiceConnectionEpochTestPeer::HomeSnapshotRequestPending(
            service),
        "explicit home_state command schedules an authoritative snapshot");
    Check(modules::BleServiceConnectionEpochTestPeer::TakeHomeSnapshotRequest(
              service),
          "scheduled home_state snapshot is consumed once by the publisher");
    Check(!modules::BleServiceConnectionEpochTestPeer::TakeHomeSnapshotRequest(
              service),
          "home_state request has no repeated mutation after consumption");
}

void TestCorrelatedSnapshotRetainsReceiptUntilPublication() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    fxe::ManualMillis::SetNowMs(1000U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    modules::BleService service;
    NimBLEServer server;
    NimBLECharacteristic characteristic;
    server.SetPeerMtu(7U, 23U);
    Peer::ConfigureConsoleOutput(service, server, characteristic, "queued\n");
    const char* request = "{\"h\":4294967295}";
    Check(std::strlen(request) <= 20U,
          "maximum snapshot receipt request fits minimum ATT payload");
    Peer::HandleConsolePacket(service, request);
    bool force = false;
    uint32_t receipt = 99U;
    Check(!service.TryBeginHomeStatePublication(force, &receipt) && receipt == 0U,
          "queued prior output cannot consume the pending receipt");
    Check(Peer::FlushConsoleOutput(service) == 0U, "prior output drains");
    service.OnConsoleStreamSubscriptionChanged(0U);
    Check(!service.TryBeginHomeStatePublication(force, &receipt),
          "unsubscribed publisher retains the pending receipt");
    service.OnConsoleStreamSubscriptionChanged(1U);
    Peer::HandleConsolePacket(service, "{\"cmd\":\"home_state\"}");
    Check(service.TryBeginHomeStatePublication(force, &receipt) && force &&
              receipt == UINT32_MAX,
          "legacy refresh does not erase the correlated receipt");
    Check(service.TryBeginHomeStatePublication(force, &receipt) && !force && receipt == 0U,
          "receipt is consumed once without an automatic retry");
    for (const char* invalid : {"{\"h\":0}", "{\"h\":-1}", "{\"h\":4294967296}",
                                "{\"h\":true}", "{\"h\":\"1\"}"}) {
        Peer::HandleConsolePacket(service, invalid);
        Check(service.TryBeginHomeStatePublication(force, &receipt) && !force && receipt == 0U,
              "invalid receipt does not schedule a snapshot");
    }
    Peer::HandleConsolePacket(service, "{\"cmd\":\"ping\",\"h\":9}");
    Check(service.TryBeginHomeStatePublication(force, &receipt) && !force && receipt == 0U,
          "explicit legacy command is not converted to a correlated read");
    Peer::HandleConsolePacket(service, "{\"cmd\":\"home_state\",\"h\":9}");
    Check(service.TryBeginHomeStatePublication(force, &receipt) && force && receipt == 0U,
          "legacy snapshot keeps its original uncorrelated semantics");
    Peer::HandleConsolePacket(service, "{\"h\":1}");
    Peer::HandleConsolePacket(service, "{\"h\":2}");
    Check(service.TryBeginHomeStatePublication(force, &receipt) && force && receipt == 2U,
          "queued newer request coalesces to its exact receipt");
    Peer::HandleConsolePacket(service, "{\"h\":3}");
    service.OnDisconnected();
    Check(!service.TryBeginHomeStatePublication(force, &receipt),
          "disconnected publication cannot complete an old receipt");
    Peer::ConfigureConsoleOutput(service, server, characteristic, "");
    Check(service.TryBeginHomeStatePublication(force, &receipt) && force && receipt == 0U,
          "fresh subscription cannot inherit a disconnected receipt");
}

void TestMalformedControlActionUsesFullControlEnvelope() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    harness.bleSettings.OnReadWritePayload("{\"a\":1,\"i\":7}");
    Check(!harness.PumpOneWriteEvent(),
          "non-string control action is rejected before queueing");
    Check(harness.ReadWriteCharacteristic().LastTx() ==
              "{\"i\":7,\"a\":\"unknown\",\"s\":\"rejected\",\"r\":\"bad_request\",\"p\":\"idle\",\"ok\":false}",
          "malformed control action uses the roomy action envelope");
}

void TestCoachingRetainsNonIdleActionReceipt() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);
    harness.actionState = modules::ControlActionState::UndoPending;
    for (const uint16_t requestId : {901U, 902U}) {
        char request[96] = {};
        std::snprintf(request, sizeof(request),
            "{\"a\":\"coaching\",\"o\":\"p1\",\"e\":true,\"i\":%u}", requestId);
        harness.bleSettings.OnReadWritePayload(request);
        Check(harness.PumpOneWriteEvent(),
              "non-idle Coaching reaches the production BLE mutation owner");
        StaticJsonDocument<256> receipt;
        Check(!deserializeJson(receipt, harness.ReadWriteCharacteristic().LastTx()) &&
                  std::strcmp(receipt["p"] | "", "undo_pending") == 0 &&
                  std::strcmp(receipt["a"] | "", "coaching") == 0 &&
                  (receipt["ok"] | false) &&
                  receipt["i"].as<uint16_t>() == requestId,
              "Applied and Noop Coaching retain current non-idle action receipt metadata");
    }
}

void TestMalformedOwnerUsesCompactControlEnvelope() {
    Harness harness;
    NimBLEDevice::SetTestMtu(23U);

    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"coaching\",\"o\":\"p3\",\"i\":65535}");
    Check(harness.PumpOneWriteEvent(),
          "malformed owner request reaches BLE write handling");
    Check(harness.ReadWriteCharacteristic().LastTx() ==
              "{\"x\":[65535,2,1]}",
          "minimum-MTU malformed owner reply uses the control compact tuple");
}

void TestUnknownActionUsesControlEnvelopeThroughWriteHandling() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    harness.bleSettings.OnReadWritePayload("{\"a\":\"bogus\",\"i\":17}");
    Check(harness.PumpOneWriteEvent(),
          "unknown action request is queued for control handling");
    Check(harness.ReadWriteCharacteristic().LastTx() ==
              "{\"i\":17,\"a\":\"unknown\",\"s\":\"rejected\",\"r\":\"unknown_action\",\"p\":\"idle\",\"ok\":false}",
          "unknown action reply still uses the action envelope");
}

void TestBleOnlyDrainsTrackedControlCompletions() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    auto local = harness.controlMutation.MutateAction(
        modules::ControlAction::NewGame, 101U);
    Check(local.status == modules::ControlMutationStatus::Accepted,
          "local completion setup action accepted");
    harness.controlMutation.Update();

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload("{\"a\":\"new_game\",\"i\":202}");
    Check(harness.PumpOneWriteEvent(),
          "BLE-owned action request is queued and handled");
    harness.ReadWriteCharacteristic().ClearTx();

    harness.controlMutation.Update();
    harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(harness.ReadWriteCharacteristic().LastTx() ==
              "{\"i\":202,\"a\":\"new_game\",\"s\":\"applied\",\"r\":\"none\",\"p\":\"idle\",\"ok\":true}",
          "BLE drains only its tracked completion");

    modules::ControlMutationOutcome completion;
    Check(harness.controlMutation.TryTakeCompletion(completion) &&
              completion.requestId == 101U,
          "untracked local completion remains queued");
    Check(!harness.controlMutation.TryTakeCompletion(202U, completion),
          "tracked BLE completion was removed from the service queue");
}

void TestSettingsOutboundRetainsLocallyRejectedResults() {
    const uint16_t previousMtu = NimBLEDevice::getMTU();
    NimBLEDevice::SetTestMtu(23U);
    NimBLEDevice::getServer()->SetPeerSlots({{7U, 23U}});
    Harness harness;
    auto& characteristic = harness.ReadWriteCharacteristic();
    characteristic.SetNotifyPayloadLimit(20U);

    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":202}");
    Check(harness.PumpOneWriteEvent(),
          "rejected-outbound action is admitted once");
    characteristic.ClearTx();
    harness.controlMutation.Update();
    characteristic.SetNotifyResult(false);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(characteristic.NotifyCount() == 0U && harness.newGameStartCount == 1U,
          "repeated local enqueue rejection neither loses nor reexecutes the action");
    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":202}");
    Check(harness.PumpOneWriteEvent() && harness.newGameStartCount == 1U,
          "rejected delivery keeps its request ID reserved against action replay");
    characteristic.SetNotifyResult(true);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(characteristic.NotifyCount() == 1U &&
              characteristic.LastTx() == "{\"x\":[202,1,0]}" &&
              harness.newGameStartCount == 1U,
          "minimum-MTU terminal outcome delivers exactly once after local recovery");
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(characteristic.NotifyCount() == 1U,
          "accepted terminal outcome is not sent twice");

    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    characteristic.ClearTx();
    harness.settings.Doc()["BRIGHTNESS"] = 3U;
    harness.settings.NotifyDocChanged();
    characteristic.SetNotifyResult(false);
    Check(harness.bleSettings.UpdateChangeNotifications(harness.settings, true),
          "changed setting is observed before rejected delivery");
    harness.bleSettings.OnReadWriteSubscribe(0U);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    harness.bleSettings.OnReadWriteSubscribe(1U);
    characteristic.SetNotifyResult(true);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(characteristic.LastTx() == "[0,3]",
          "changed setting survives rejection and unsubscribe/resubscribe");

    characteristic.ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":203}");
    Check(harness.PumpOneWriteEvent(),
          "old-epoch terminal fixture is admitted once");
    harness.controlMutation.Update();
    characteristic.SetNotifyResult(false);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    (void)harness.bleSettings.AdvanceConnectionEpoch();
    characteristic.SetNotifyResult(true);
    characteristic.ClearTx();
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(characteristic.LastTx().empty(),
          "callback-side epoch retirement fences retained old-session outcomes");
    Check(harness.bleSettings.ActivateConnectionEpoch(
              harness.bleSettings.CurrentConnectionEpoch()),
          "replacement connection epoch activates");
    harness.bleSettings.OnReadWriteSubscribe(1U);
    characteristic.ClearTx();
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(characteristic.LastTx().empty() && harness.newGameStartCount == 2U,
          "connection retirement drops an undelivered old-epoch outcome without replay");

    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":204}");
    Check(harness.PumpOneWriteEvent(),
          "in-send epoch fixture is admitted once");
    characteristic.ClearTx();
    harness.controlMutation.Update();
    characteristic.SetNotifyHook([](void* context) {
        auto* settings = static_cast<modules::BleSettings*>(context);
        (void)settings->AdvanceConnectionEpoch();
    }, &harness.bleSettings);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    characteristic.SetNotifyHook(nullptr);
    Check(characteristic.NotifyCount() == 1U,
          "SDK callback can retire the epoch during an accepted enqueue");
    Check(harness.bleSettings.ActivateConnectionEpoch(
              harness.bleSettings.CurrentConnectionEpoch()),
          "post-callback replacement epoch activates");
    harness.bleSettings.OnReadWriteSubscribe(1U);
    characteristic.ClearTx();
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(characteristic.LastTx().empty() && harness.newGameStartCount == 3U,
          "outcome crossing epoch retirement is not accepted or replayed");

    for (uint16_t requestId = 210U; requestId < 214U; ++requestId) {
        char request[48] = {};
        const int requestLen = std::snprintf(
            request, sizeof(request), "{\"a\":\"new_game\",\"i\":%u}",
            static_cast<unsigned>(requestId));
        Check(requestLen > 0 &&
                  static_cast<size_t>(requestLen) < sizeof(request),
              "retained completion request fits test buffer");
        harness.bleSettings.OnReadWritePayload(request);
        Check(harness.PumpOneWriteEvent(),
              "retained completion action is admitted");
        characteristic.ClearTx();
        harness.controlMutation.Update();
    }
    characteristic.SetNotifyResult(false);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    characteristic.ClearTx();
    characteristic.SetNotifyResult(true);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(characteristic.NotifyCount() ==
              modules::ControlMutationService::COMPLETION_CAPACITY &&
              harness.newGameStartCount == 7U,
          "all bounded retained completions deliver once without action replay");

    harness.bleSettings.OnReadWriteSubscribe(2U);
    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":220}");
    Check(harness.PumpOneWriteEvent(),
          "indication retry fixture is admitted once");
    characteristic.ClearTx();
    harness.controlMutation.Update();
    characteristic.SetIndicateResult(false);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(characteristic.IndicateCount() == 0U &&
              harness.newGameStartCount == 8U,
          "rejected indications retain the outcome without replaying the action");
    characteristic.SetIndicateResult(true);
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(characteristic.IndicateCount() == 1U &&
              characteristic.LastTx() == "{\"x\":[220,1,0]}",
          "retained outcome delivers once after indication recovery");

    NimBLEDevice::getServer()->SetPeerSlots({});
    NimBLEDevice::SetTestMtu(previousMtu);
}

void TestDisconnectBeforeUpdateExecutesOnceWithoutStaleCompletion() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":301}");
    Check(harness.PumpOneWriteEvent(),
          "disconnect-before-update request is accepted by BLE");
    Check(harness.ReadWriteCharacteristic().LastTx() ==
              "{\"i\":301,\"a\":\"new_game\",\"s\":\"accepted\",\"r\":\"none\",\"p\":\"idle\",\"ok\":true}",
          "connected owner receives the immediate Accepted outcome");
    harness.ReadWriteCharacteristic().ClearTx();

    harness.bleSettings.ResetConnectionState();
    harness.controlMutation.Update();
    harness.controlMutation.Update();
    Check(harness.newGameStartCount == 1U,
          "disconnect relinquishes only the response and executes action once");

    modules::ControlMutationOutcome completion;
    Check(!harness.controlMutation.TryTakeCompletion(301U, completion),
          "future completion is discarded after disconnect");

    // A replacement connection starts with a fresh response ledger.
    harness.bleSettings.ResetConnectionState();
    harness.bleSettings.OnReadWriteSubscribe(1U);
    harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(harness.ReadWriteCharacteristic().LastTx().empty(),
          "replacement connection cannot receive the abandoned completion");
}

void TestDisconnectAfterQueuedCompletionPreservesUnrelatedOwner() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    const auto local = harness.controlMutation.MutateAction(
        modules::ControlAction::NewGame, 401U);
    Check(local.status == modules::ControlMutationStatus::Accepted,
          "unrelated local action is accepted");
    harness.controlMutation.Update();

    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":402}");
    Check(harness.PumpOneWriteEvent(),
          "BLE action is accepted before queued-completion disconnect");
    harness.ReadWriteCharacteristic().ClearTx();
    harness.controlMutation.Update();

    harness.bleSettings.ResetConnectionState();
    modules::ControlMutationOutcome completion;
    Check(!harness.controlMutation.TryTakeCompletion(402U, completion),
          "disconnect removes the already-queued BLE completion");
    Check(harness.controlMutation.TryTakeCompletion(401U, completion) &&
              completion.requestId == 401U,
          "disconnect leaves unrelated local completion available");

    harness.bleSettings.ResetConnectionState();
    harness.bleSettings.OnReadWriteSubscribe(1U);
    harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(harness.ReadWriteCharacteristic().LastTx().empty(),
          "reconnect does not publish the removed BLE completion");
}

void TestRepeatedDisconnectAbandonDoesNotExhaustCompletionCapacity() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    constexpr size_t cycleCount =
        modules::ControlMutationService::COMPLETION_CAPACITY + 3U;
    for (size_t index = 0U; index < cycleCount; ++index) {
        harness.bleSettings.ResetConnectionState();
        harness.bleSettings.OnReadWriteSubscribe(1U);
        harness.ReadWriteCharacteristic().ClearTx();

        char request[48] = {};
        const uint16_t requestId = static_cast<uint16_t>(500U + index);
        const int requestLen = std::snprintf(
            request, sizeof(request),
            "{\"a\":\"new_game\",\"i\":%u}",
            static_cast<unsigned>(requestId));
        Check(requestLen > 0 &&
                  static_cast<size_t>(requestLen) < sizeof(request),
              "abandon-cycle request fits test buffer");
        harness.bleSettings.OnReadWritePayload(request);
        Check(harness.PumpOneWriteEvent(),
              "abandon-cycle action reaches BLE handling");
        harness.ReadWriteCharacteristic().ClearTx();

        harness.bleSettings.ResetConnectionState();
        harness.controlMutation.Update();
        modules::ControlMutationOutcome completion;
        Check(!harness.controlMutation.TryTakeCompletion(requestId, completion),
              "abandon cycle leaves no ownerless completion");
    }
    Check(harness.newGameStartCount == cycleCount,
          "more-than-capacity abandon cycles execute every accepted action");

    harness.bleSettings.ResetConnectionState();
    harness.bleSettings.OnReadWriteSubscribe(1U);
    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":600}");
    Check(harness.PumpOneWriteEvent(),
          "post-abandon action reaches BLE handling");
    harness.ReadWriteCharacteristic().ClearTx();
    harness.controlMutation.Update();
    harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(harness.ReadWriteCharacteristic().LastTx() ==
              "{\"i\":600,\"a\":\"new_game\",\"s\":\"applied\",\"r\":\"none\",\"p\":\"idle\",\"ok\":true}",
          "later connected action completes after repeated abandon cycles");
}

void TestStackResetReleasesQueuedBleCompletion() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":701}");
    Check(harness.PumpOneWriteEvent(),
          "stack-reset action reaches BLE handling");
    harness.controlMutation.Update();

    harness.bleSettings.Reset();
    modules::ControlMutationOutcome completion;
    Check(!harness.controlMutation.TryTakeCompletion(701U, completion),
          "stack reset releases queued BLE completion before clearing IDs");
}

void TestDisconnectDiscardsQueuedIngressBeforeRequestOwnership() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":801}");

    // The old connection disappears before the main loop drains its write.
    harness.bleSettings.ResetConnectionState();
    harness.bleSettings.OnReadWriteSubscribe(1U);
    Check(harness.PumpOneWriteEvent(),
          "old-session ingress remains queued until the main loop drains it");
    harness.controlMutation.Update();
    Check(harness.newGameStartCount == 0U,
          "queued old-session action never reaches semantic execution");
    Check(harness.ReadWriteCharacteristic().LastTx().empty(),
          "queued old-session action sends no reply to the replacement client");

    // The replacement client may reuse the same request ID for different work.
    harness.undoStart = modules::ControlCoordinatorResult::Applied;
    harness.actionStateAfterUndoRequest =
        modules::ControlActionState::UndoPending;
    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"undo\",\"n\":1,\"i\":801}");
    Check(harness.PumpOneWriteEvent(),
          "replacement-session request with reused ID is handled");
    Check(harness.ReadWriteCharacteristic().LastTx() ==
              "{\"i\":801,\"a\":\"undo\",\"s\":\"accepted\",\"r\":\"none\",\"p\":\"idle\",\"ok\":true}",
          "replacement client owns the reused request ID");
    harness.ReadWriteCharacteristic().ClearTx();
    harness.controlMutation.Update();
    harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(harness.undoStartCount == 1U && harness.newGameStartCount == 0U,
          "only the replacement-session action executes");
    Check(harness.ReadWriteCharacteristic().LastTx() ==
              "{\"i\":801,\"a\":\"undo\",\"s\":\"applied\",\"r\":\"none\",\"p\":\"undo_pending\",\"ok\":true}",
          "replacement client receives only its terminal result");
}

void TestDisconnectCallbackInvalidatesQueuedSettingBeforeLifecycleDrain() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);
    const uint32_t baselineRevision = harness.settings.GetRevision();

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"k\":\"BRIGHTNESS\",\"v\":6,\"i\":851}");

    // The transport callback advances the ingress generation immediately,
    // while its lifecycle event remains behind this write in the FIFO.
    (void)harness.bleSettings.AdvanceConnectionEpoch();
    Check(harness.PumpOneWriteEvent(),
          "pre-disconnect setting ingress remains queued for main-loop drain");
    Check(harness.settings.ReadValue("BRIGHTNESS", 5U) == 5U &&
              harness.settings.GetRevision() == baselineRevision,
          "disconnect callback invalidates an older queued setting before lifecycle drain");
    harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    Check(harness.ReadWriteCharacteristic().LastTx().empty() &&
              harness.ReadWriteCharacteristic().NotifyCount() == 0U &&
              harness.ReadWriteCharacteristic().IndicateCount() == 0U,
          "invalidated setting ingress sends no reply to a replacement client");
}

void TestLegacySettingsSetIsOwnedByOneConnectionGeneration() {
    static constexpr char SET_BRIGHTNESS[] =
        "{\"cmd\":\"settings_set\",\"k\":\"BRIGHTNESS\",\"v\":6}";

    {
        Harness harness;
        modules::BleService service;
        Check(modules::BleServiceConnectionEpochTestPeer::Configure(
                  service, harness.settings, harness.controlMutation),
              "legacy command test BLE queue begins");
        const uint32_t baselineRevision = harness.settings.GetRevision();

        service.OnCommandCharacteristicWrite(
            SET_BRIGHTNESS, sizeof(SET_BRIGHTNESS) - 1U);
        service.OnDisconnected();
        Check(modules::BleServiceConnectionEpochTestPeer::DrainConsoleEvent(
                  service),
              "legacy setting command remains ahead of disconnect in FIFO");
        Check(harness.settings.ReadValue("BRIGHTNESS", 5U) == 5U &&
                  harness.settings.GetRevision() == baselineRevision,
              "disconnect callback blocks stale legacy setting mutation");
        Check(modules::BleServiceConnectionEpochTestPeer::LastClientActivityMs(
                  service) == 41U,
              "stale legacy command has no activity side effect");
    }

    {
        Harness harness;
        modules::BleService service;
        Check(modules::BleServiceConnectionEpochTestPeer::Configure(
                  service, harness.settings, harness.controlMutation),
              "replacement legacy command test BLE queue begins");
        const uint32_t baselineRevision = harness.settings.GetRevision();

        service.OnCommandCharacteristicWrite(
            SET_BRIGHTNESS, sizeof(SET_BRIGHTNESS) - 1U);
        service.OnConnected(22U, "replacement", 128U, 1U);
        Check(modules::BleServiceConnectionEpochTestPeer::DrainConsoleEvent(
                  service),
              "legacy setting command remains ahead of replacement connection in FIFO");
        Check(harness.settings.ReadValue("BRIGHTNESS", 5U) == 5U &&
                  harness.settings.GetRevision() == baselineRevision,
              "replacement callback blocks prior-session legacy mutation");
        Check(modules::BleServiceConnectionEpochTestPeer::LastClientActivityMs(
                  service) == 41U,
              "replacement-invalidated command has no activity side effect");
    }

    {
        Harness harness;
        modules::BleService service;
        Check(modules::BleServiceConnectionEpochTestPeer::Configure(
                  service, harness.settings, harness.controlMutation),
              "stack-reset legacy command test BLE queue begins");
        const uint32_t baselineRevision = harness.settings.GetRevision();

        service.OnCommandCharacteristicWrite(
            SET_BRIGHTNESS, sizeof(SET_BRIGHTNESS) - 1U);
        modules::BleServiceConnectionEpochTestPeer::ResetStackGeneration(
            service);
        Check(modules::BleServiceConnectionEpochTestPeer::DrainConsoleEvent(
                  service),
              "legacy setting command remains queued across stack reset");
        Check(harness.settings.ReadValue("BRIGHTNESS", 5U) == 5U &&
                  harness.settings.GetRevision() == baselineRevision,
              "stack reset blocks stale legacy setting mutation");
    }

    {
        Harness harness;
        modules::BleService service;
        Check(modules::BleServiceConnectionEpochTestPeer::Configure(
                  service, harness.settings, harness.controlMutation),
              "current legacy command test BLE queue begins");
        const uint32_t baselineRevision = harness.settings.GetRevision();

        service.OnCommandCharacteristicWrite(
            SET_BRIGHTNESS, sizeof(SET_BRIGHTNESS) - 1U);
        Check(modules::BleServiceConnectionEpochTestPeer::DrainConsoleEvent(
                  service),
              "current-session legacy setting command drains");
        Check(harness.settings.ReadValue("BRIGHTNESS", 5U) == 6U &&
                  harness.settings.GetRevision() == baselineRevision + 1U,
              "current-session legacy setting command still applies once");
        service.OnCommandCharacteristicWrite(
            SET_BRIGHTNESS, sizeof(SET_BRIGHTNESS) - 1U);
        Check(modules::BleServiceConnectionEpochTestPeer::DrainConsoleEvent(
                  service) &&
                  harness.settings.ReadValue("BRIGHTNESS", 5U) == 6U &&
                  harness.settings.GetRevision() == baselineRevision + 1U,
              "repeating the legacy setting is a no-op at the mutation owner");
    }
}

void TestStackResetDiscardsQueuedIngress() {
    Harness harness;
    NimBLEDevice::SetTestMtu(128U);

    harness.ReadWriteCharacteristic().ClearTx();
    harness.bleSettings.OnReadWritePayload(
        "{\"a\":\"new_game\",\"i\":901}");

    harness.bleSettings.Reset();
    harness.bleSettings.Configure(
        &harness.settings, &harness.eventQueue, &harness.droppedEvents);
    harness.bleSettings.SetControlMutationService(&harness.controlMutation);
    NimBLEService restartedService;
    Check(harness.bleSettings.CreateCharacteristics(restartedService),
          "BLE settings characteristics recreate after stack reset");
    harness.bleSettings.OnReadWriteSubscribe(1U);

    modules::BleEvent staleEvent;
    Check(harness.eventQueue.TryDequeue(staleEvent) &&
              staleEvent.type == modules::BleEventType::SettingWrite,
          "stack-reset test retains the previously queued write");
    harness.bleSettings.HandleSettingWriteEvent(
        harness.settings, staleEvent.payload.setting);
    harness.controlMutation.Update();
    Check(harness.newGameStartCount == 0U,
          "stack reset invalidates queued semantic ingress");
    Check(restartedService.CharacteristicAt(1U)->LastTx().empty(),
          "restarted stack does not reply to stale ingress");
}

void TestOtaRuntimeAcceptanceIsReadOnlyAtProductionIngress() {
    modules::BleService service;
    runtime::RuntimeHealthReducer health;
    NimBLECharacteristic characteristic;
    modules::RuntimeHealthRecheckProbe recheckProbe;
    Check(modules::BleServiceConnectionEpochTestPeer::ConfigureRuntimeHealth(
              service, health, characteristic, recheckProbe),
          "OTA runtime-state harness configures an active connection");

    constexpr char REQUEST[] = "{\"cmd\":\"ota_state\"}";
    service.OnOtaControlCharacteristicWrite(REQUEST, sizeof(REQUEST) - 1U);
    service.DrainEventQueue();
    Check(characteristic.LastTx() == "{\"ov\":\"p\"}" &&
              recheckProbe.calls == 0U,
          "OTA state reports pending before runtime acceptance without a health recheck");

    characteristic.ClearTx();
    service.PublishOtaRuntimeAccepted();
    service.OnOtaControlCharacteristicWrite(REQUEST, sizeof(REQUEST) - 1U);
    service.DrainEventQueue();
    Check(characteristic.LastTx() == "{\"ov\":\"v\"}" &&
              recheckProbe.calls == 0U,
          "OTA state reports verified only after runtime publication without a health recheck");
}

void TestRuntimeHealthProtocol() {
    char payload[18] = {};
    runtime::RuntimeHealthSnapshot snapshot;
    Check(modules::ble_runtime_health_protocol::FormatHealthResponse(
              snapshot, payload, sizeof(payload)) &&
              std::strcmp(payload, "{\"h\":\"000000000\"}") == 0,
          "default runtime health formats as fixed-width healthy payload");

    snapshot.revision = 1U;
    snapshot.state = runtime::RuntimeHealthState::InputPaused;
    snapshot.causes = runtime::RuntimeHealthCauseMask(
        runtime::RuntimeHealthCause::BoardScanUncertain);
    Check(modules::ble_runtime_health_protocol::FormatHealthResponse(
              snapshot, payload, sizeof(payload)) &&
              std::strcmp(payload, "{\"h\":\"000120004\"}") == 0,
          "input-paused runtime health preserves revision, state, and causes");

    snapshot.revision = 0xFFFFU;
    snapshot.state = runtime::RuntimeHealthState::ProtectionLatched;
    snapshot.causes =
        runtime::RuntimeHealthCauseMask(runtime::RuntimeHealthCause::BrownoutLatched) |
        runtime::RuntimeHealthCauseMask(
            runtime::RuntimeHealthCause::SevereProtectionLatched);
    Check(modules::ble_runtime_health_protocol::FormatHealthResponse(
              snapshot, payload, sizeof(payload)) &&
              std::strcmp(payload, "{\"h\":\"FFFF30030\"}") == 0,
          "latched runtime health uses uppercase fixed-width hexadecimal");

    char tooSmall[17] = {};
    Check(!modules::ble_runtime_health_protocol::FormatHealthResponse(
              snapshot, tooSmall, sizeof(tooSmall)) &&
              std::strlen(payload) ==
                  modules::ble_runtime_health_protocol::HEALTH_RESPONSE_BYTES,
          "runtime-health response is exactly 17 bytes and minimum-MTU safe");

    modules::BleService service;
    runtime::RuntimeHealthReducer health;
    NimBLECharacteristic characteristic;
    modules::RuntimeHealthRecheckProbe recheckProbe;
    Check(modules::BleServiceConnectionEpochTestPeer::ConfigureRuntimeHealth(
              service, health, characteristic, recheckProbe),
          "runtime-health BLE harness configures an active connection");

    constexpr char HEALTH[] = "{\"cmd\":\"health\"}";
    constexpr char HEALTH_WITH_MEMBER[] = "{\"cmd\":\"health\",\"x\":1}";
    constexpr char HEALTH_RECHECK[] = "{\"cmd\":\"health_chk\"}";
    constexpr char HEALTH_RECHECK_WITH_MEMBER[] =
        "{\"cmd\":\"health_chk\",\"x\":1}";
    Check(modules::ble_runtime_health_protocol::IsHealthRequest(
              HEALTH, sizeof(HEALTH) - 1U) &&
              !modules::ble_runtime_health_protocol::IsHealthRequest(
                  HEALTH_WITH_MEMBER, sizeof(HEALTH_WITH_MEMBER) - 1U),
          "only the exact runtime-health request is recognized");
    Check(modules::ble_runtime_health_protocol::IsHealthRecheckRequest(
              HEALTH_RECHECK, sizeof(HEALTH_RECHECK) - 1U) &&
              !modules::ble_runtime_health_protocol::IsHealthRecheckRequest(
                  HEALTH_RECHECK_WITH_MEMBER,
                  sizeof(HEALTH_RECHECK_WITH_MEMBER) - 1U) &&
              modules::ble_runtime_health_protocol::HEALTH_RECHECK_REQUEST_BYTES ==
                  20U,
          "health recheck is exact and minimum-MTU safe");

    service.OnOtaControlCharacteristicWrite(
        HEALTH_RECHECK, sizeof(HEALTH_RECHECK) - 1U);
    modules::BleServiceConnectionEpochTestPeer::ResetRuntimeHealthConnectionGeneration(
        service);
    uint32_t staleRecheckEpoch = 0U;
    Check(modules::BleServiceConnectionEpochTestPeer::DispatchQueuedHealth(
              service, staleRecheckEpoch) &&
              recheckProbe.calls == 0U && characteristic.NotifyCount() == 0U,
          "stale connection generation cannot invoke the health recheck action");

    recheckProbe.result = true;
    service.OnOtaControlCharacteristicWrite(
        HEALTH_RECHECK, sizeof(HEALTH_RECHECK) - 1U);
    uint32_t successfulRecheckEpoch = 0U;
    Check(modules::BleServiceConnectionEpochTestPeer::DispatchQueuedHealth(
              service, successfulRecheckEpoch) &&
              recheckProbe.calls == 1U && characteristic.NotifyCount() == 1U &&
              characteristic.LastTx() == "{\"c\":\"ok\"}",
          "fresh recheck invokes the injected validated clear exactly once");

    characteristic.ClearTx();
    recheckProbe.result = false;
    service.OnOtaControlCharacteristicWrite(
        HEALTH_RECHECK, sizeof(HEALTH_RECHECK) - 1U);
    uint32_t failedRecheckEpoch = 0U;
    Check(modules::BleServiceConnectionEpochTestPeer::DispatchQueuedHealth(
              service, failedRecheckEpoch) &&
              recheckProbe.calls == 2U && characteristic.NotifyCount() == 1U &&
              characteristic.LastTx() == "{\"c\":\"fail\"}",
          "failed fresh evidence leaves clear failure explicit without retries");

    characteristic.ClearTx();

    service.OnOtaControlCharacteristicWrite(HEALTH, sizeof(HEALTH) - 1U);
    modules::BleServiceConnectionEpochTestPeer::ResetRuntimeHealthConnectionGeneration(
        service);
    uint32_t staleConnectionEpoch = 0U;
    Check(modules::BleServiceConnectionEpochTestPeer::DispatchQueuedHealth(
              service, staleConnectionEpoch) &&
              staleConnectionEpoch !=
                  modules::BleServiceConnectionEpochTestPeer::CurrentConnectionEpoch(
                      service) &&
              characteristic.NotifyCount() == 0U && characteristic.LastTx().empty(),
          "disconnect, reconnect, and reset invalidate queued health replies");

    service.OnOtaControlCharacteristicWrite(HEALTH, sizeof(HEALTH) - 1U);
    uint32_t freshConnectionEpoch = 0U;
    Check(modules::BleServiceConnectionEpochTestPeer::DispatchQueuedHealth(
              service, freshConnectionEpoch) &&
              freshConnectionEpoch ==
                  modules::BleServiceConnectionEpochTestPeer::CurrentConnectionEpoch(
                      service) &&
              characteristic.NotifyCount() == 1U &&
              characteristic.LastTx() == "{\"h\":\"000000000\"}",
          "fresh runtime-health generation returns the authoritative snapshot");
    Check(!modules::BleServiceConnectionEpochTestPeer::DigestRequestPending(service),
          "runtime health does not alter firmware-digest transaction state");
}

void TestConnectionParameterEventOwnership() {
    Check(modules::ble_internal::BleFastConnectionIntervalForRequest(1U) == 6U &&
              modules::ble_internal::BleFastConnectionIntervalForRequest(2U) == 6U &&
              modules::ble_internal::BleFastConnectionIntervalForRequest(3U) == 12U &&
              modules::ble_internal::BleFastConnectionTimeoutForRequest(1U) == 200U &&
              modules::ble_internal::BleFastConnectionTimeoutForRequest(2U) == 200U &&
              modules::ble_internal::BleFastConnectionTimeoutForRequest(3U) == 600U,
          "fast connection retries preserve Android 7.5 ms/2 s then use Apple's 15 ms/6 s request");
    Harness harness;
    modules::BleService service;
    fxe::ManualMillis::SetNowMs(1500U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    Check(modules::BleServiceConnectionEpochTestPeer::Configure(
              service, harness.settings, harness.controlMutation),
          "connection-parameter telemetry queue begins");

    service.OnConnectionParametersChanged(24U, 1U, 200U, true);
    modules::BleConnectionParametersEvent initial;
    Check(modules::BleServiceConnectionEpochTestPeer::TakeConnectionParameters(
              service, initial) &&
              initial.initial && initial.intervalUnits == 24U &&
              initial.latency == 1U &&
              initial.supervisionTimeoutUnits == 200U,
          "initial link timing enters the bounded owner queue without conversion loss");

    service.OnConnectionParametersChanged(12U, 0U, 300U, false);
    fxe::ManualMillis::AdvanceMs(25U);
    service.OnDisconnected(19);
    modules::BleConnectionParametersEvent stale;
    Check(modules::BleServiceConnectionEpochTestPeer::TakeConnectionParameters(
              service, stale) &&
              !stale.initial && stale.intervalUnits == 12U &&
              stale.latency == 0U &&
              stale.supervisionTimeoutUnits == 300U &&
              stale.connectionEpoch !=
                  modules::BleServiceConnectionEpochTestPeer::CurrentConnectionEpoch(
                      service),
          "a replaced connection's parameter sample retains its stale epoch");
    modules::BleConnectionChangeEvent disconnected;
    Check(modules::BleServiceConnectionEpochTestPeer::TakeConnectionChange(
              service, disconnected) &&
              !disconnected.connected && disconnected.reason == 19 &&
              disconnected.callbackAtMs == 1525U,
          "disconnect reason and callback time remain attached to its connection-generation event");
}

void TestEarlyUpdaterAtBootBoundary() {
    Harness harness;
    modules::BleService service;
    hardware::RmtFlashArbiter arbiter;
    g_failures += TestEarlyUpdaterRuntime(harness.settings, service, arbiter);
}

void TestRuntimeBootHealthAtConnectionBoundary() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    Harness harness;
    modules::BleService service;
    NimBLEAdvertising advertising;
    NimBLEServer server;
    hardware::RmtFlashArbiter arbiter;
    Check(modules::BleServiceConnectionEpochTestPeer::Configure(
              service, harness.settings, harness.controlMutation),
          "boot health journey uses the production BLE lifecycle");
    modules::BleServiceConnectionEpochTestPeer::EnableAdvertisingRecovery(
        service, advertising, server);
    modules::BleServiceConnectionEpochTestPeer::SetDeviceName(service, "Foxie Litch");
    service.Update(harness.settings);
    g_failures += TestRuntimeBootHealthRecovery(
        harness.settings, service, arbiter, advertising);
}

void TestAdvertisingRecoversAfterSdkRestartFailure() {
    Harness harness;
    modules::BleService service;
    NimBLEAdvertising advertising;
    NimBLEServer server;
    Check(modules::BleServiceConnectionEpochTestPeer::Configure(
              service, harness.settings, harness.controlMutation),
          "advertising recovery uses the production Update owner");
    service.StartAdvertising();
    Check(!service.IsAdvertising() && advertising.StartCalls() == 0U,
          "boot before BLE initialization cannot claim or start advertising");
    modules::BleServiceConnectionEpochTestPeer::EnableAdvertisingRecovery(
        service, advertising, server);
    service.Update(harness.settings);
    Check(service.IsAdvertising() && advertising.StartCalls() == 1U,
          "normal Update starts the initialized disconnected radio");

    // The pinned SDK onHostSync attempts the forever-advertising restart and
    // ignores its result. Inject only that SDK boundary: no app disconnect or
    // firmware callback clears the previous successful-start projection.
    advertising.SetActive(false);
    advertising.SetStartResult(false);
    Check(!advertising.start(), "controlled SDK auto-restart fails");
    Check(!service.IsAdvertising(),
          "failed SDK restart cannot claim an OTA recovery transport is ready");
    service.Update(harness.settings);
    Check(advertising.StartCalls() == 3U && !service.IsAdvertising(),
          "normal Update retries the stopped SDK radio and retains failure truth");
    advertising.SetStartResult(true);
    service.Update(harness.settings);
    service.Update(harness.settings);
    Check(advertising.StartCalls() == 4U && service.IsAdvertising(),
          "next Update restores discovery without duplicate starts while active");
    std::printf("[BLE_ADVERTISING_RECOVERY] failed_restart=observed update_retry=1 restored=%u starts=%lu\n",
                service.IsAdvertising() ? 1U : 0U,
                static_cast<unsigned long>(advertising.StartCalls()));

    advertising.SetActive(false);
    service.OnConnected(7U, "recovered", 247U, 1U);
    service.Update(harness.settings);
    service.Update(harness.settings);
    Check(!service.IsConnected() && advertising.StartCalls() == 4U &&
              !advertising.isAdvertising(),
          "published connection before lifecycle drain prevents a second advertising window");
    service.DrainEventQueue();
    service.Update(harness.settings);
    Check(service.IsConnected() && advertising.StartCalls() == 4U &&
              !service.IsAdvertising(),
          "advertising peeks retain the callback for activation and the connected session stays quiet");
    service.OnDisconnected(19);
    service.DrainEventQueue();
    Check(!service.IsConnected() && advertising.StartCalls() == 5U &&
              service.IsAdvertising(),
          "ordinary disconnect restores discovery through the same owner");
    advertising.SetActive(false);
    service.OnConnected(8U, "coalesced", 247U, 1U);
    service.OnDisconnected(19);
    service.Update(harness.settings);
    service.DrainEventQueue();
    Check(!service.IsConnected() && advertising.StartCalls() == 6U &&
              service.IsAdvertising(),
          "latest disconnect overwrite permits recovery without consuming lifecycle state");
    advertising.SetActive(false);
    service.OnConnected(9U, "retired-stack", 247U, 1U);
    // Real ShutdownStack retires this epoch before creating the next radio;
    // exercise that owner directly while SDK construction stays stubbed.
    modules::BleServiceConnectionEpochTestPeer::ResetStackGeneration(service);
    service.Update(harness.settings);
    service.DrainEventQueue();
    Check(!service.IsConnected() && advertising.StartCalls() == 7U &&
              service.IsAdvertising(),
          "retired pending connection cannot suppress new-stack advertising or activate an old peer");
    harness.settings.Doc()["BLE_ENABLED"] = 0U;
    service.Update(harness.settings);
    // Stack teardown remains the existing SDK stub. Even if its last object
    // still reports active, an uninitialized service cannot claim readiness.
    service.StartAdvertising();
    Check(!service.IsEnabled() && advertising.StartCalls() == 7U &&
              !service.IsAdvertising(),
          "disabled BLE cannot retry advertising");
}

void TestConnectionLifecycleSurvivesFullIngressQueue() {
    Harness harness;
    modules::BleService service;
    NimBLEAdvertising advertising;
    NimBLEServer server;
    Check(modules::BleServiceConnectionEpochTestPeer::Configure(
              service, harness.settings, harness.controlMutation),
          "connection lifecycle queues begin");

    service.OnConnected(7U, "initial", 247U, 1U);
    service.DrainEventQueue();
    Check(service.IsConnected(), "initial connection becomes active");
    modules::BleServiceConnectionEpochTestPeer::EnableAdvertisingRecovery(
        service, advertising, server);

    for (unsigned i = 0U; i < modules::BleEventQueue::CAPACITY; ++i) {
        service.OnConnectionParametersChanged(24U, 0U, 200U, false);
    }
    service.OnDisconnected(19);
    service.DrainEventQueue();
    Check(!service.IsConnected() &&
              advertising.StartCalls() == 1U && advertising.isAdvertising(),
          "disconnect remains available and restarts advertising when data ingress is full");

    for (unsigned i = 0U; i < modules::BleEventQueue::CAPACITY; ++i) {
        service.OnConnectionParametersChanged(12U, 0U, 200U, false);
    }
    service.OnConnected(8U, "replacement", 247U, 1U);
    modules::BleServiceConnectionEpochTestPeer::SubscribeSettings(service, 1U);
    service.OnLitchcraftSubscription(1U);
    service.OnConsoleStreamSubscriptionChanged(1U);
    service.OnOtaControlSubscriptionChanged(1U);
    service.OnLitchcraftSubscription(0U);
    service.OnLitchcraftSubscription(1U);
    service.DrainEventQueue();
    Check(service.IsConnected() && service.GetConnectionCount() == 1U &&
              std::strcmp(service.GetPeerAddress(), "replacement") == 0,
          "latest replacement connection becomes usable when data ingress is full");
    Check(modules::BleServiceConnectionEpochTestPeer::HasSettingsSubscription(service) &&
              modules::BleServiceConnectionEpochTestPeer::HasLitchcraftSubscription(service) &&
              service.IsConsoleStreamSubscribed() &&
              modules::BleServiceConnectionEpochTestPeer::HasOtaSubscription(service),
          "latest subscriptions received before lifecycle drain survive activation");

    service.OnConnected(9U, "newest", 247U, 1U);
    service.DrainEventQueue();
    Check(!modules::BleServiceConnectionEpochTestPeer::HasSettingsSubscription(service) &&
              !modules::BleServiceConnectionEpochTestPeer::HasLitchcraftSubscription(service) &&
              !service.IsConsoleStreamSubscribed() &&
              !modules::BleServiceConnectionEpochTestPeer::HasOtaSubscription(service),
          "a replacement connection cannot inherit stale subscription projections");

    service.OnConnected(10U, "interleaved", 247U, 1U);
    modules::BleServiceConnectionEpochTestPeer::SubscribeDuringActivation(service);
    service.DrainEventQueue();
    Check(modules::BleServiceConnectionEpochTestPeer::HasSettingsSubscription(service) &&
              modules::BleServiceConnectionEpochTestPeer::HasLitchcraftSubscription(service) &&
              service.IsConsoleStreamSubscribed() &&
              modules::BleServiceConnectionEpochTestPeer::HasOtaSubscription(service),
          "subscription callbacks interleaved with activation cannot be overwritten by an older projection");

    service.OnConnected(12U, "interleaved-unsubscribe", 247U, 1U);
    modules::BleServiceConnectionEpochTestPeer::SubscribeSettings(service, 1U);
    service.OnLitchcraftSubscription(1U);
    service.OnConsoleStreamSubscriptionChanged(1U);
    service.OnOtaControlSubscriptionChanged(1U);
    modules::BleServiceConnectionEpochTestPeer::UnsubscribeDuringActivation(service);
    service.DrainEventQueue();
    Check(!modules::BleServiceConnectionEpochTestPeer::HasSettingsSubscription(service) &&
              !modules::BleServiceConnectionEpochTestPeer::HasLitchcraftSubscription(service) &&
              !service.IsConsoleStreamSubscribed() &&
              !modules::BleServiceConnectionEpochTestPeer::HasOtaSubscription(service),
          "unsubscribe callbacks interleaved with activation cannot be overwritten by an older projection");

    modules::BleServiceConnectionEpochTestPeer::ReplaceDuringNextIngressDispatch(service);
    service.DrainEventQueue();
    harness.controlMutation.Update();
    Check(service.IsConnected() &&
              std::strcmp(service.GetPeerAddress(), "dispatch-race") == 0 &&
              harness.newGameStartCount == 1U,
          "a connection published during drain activates before its first current-generation request");

    NimBLECharacteristic otaStatus;
    modules::BleServiceConnectionEpochTestPeer::ConfigureOtaNotifications(
        service, otaStatus);
    constexpr const char* oldBegin =
        R"({"cmd":"ota_sim_begin","size":200,"chunk":100})";
    service.OnOtaControlCharacteristicWrite(oldBegin, std::strlen(oldBegin));
    service.DrainEventQueue();
    Check(service.IsOtaInProgress(), "old OTA fixture is active before replacement");
    modules::BleServiceConnectionEpochTestPeer::ReplaceWithOtaDuringNextIngressDispatch(service);
    service.DrainEventQueue();
    service.DrainEventQueue();
    Check(service.IsOtaInProgress() &&
              std::strcmp(service.GetPeerAddress(), "ota-replacement") == 0,
          "disconnect cleanup during drain cannot retire the replacement OTA on the next drain");
    uint8_t otaBytes[100] = {};
    service.OnOtaDataCharacteristicWrite(otaBytes, sizeof(otaBytes));
    service.DrainEventQueue();
    Check(service.IsOtaInProgress() &&
              service.GetOtaLedPresentation().percent == 50U,
          "replacement OTA consumes current-generation bytes after mid-drain reconnect");
}

void TestSharedBulkCreditsFollowSlotLifetime() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    namespace store = modules::litchcraft_frame_store;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    Harness harness;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(harness.settings, engine, state);
    modules::BleService service;
    NimBLECharacteristic characteristic;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "hole probe initializes production queues");
    Peer::ConfigureLitchcraft(service, game, characteristic);
    Peer::PrepareProducerGeneration(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, modules::ConnectionSession::OBJECT_CAPACITY), "hole probe negotiates the current ATT244 Object window");
    service.SetLitchcraftFrameIngressCallbacks(
        [](void*, modules::BleOtaChunkQueue&, store::StoreProjection, uint32_t, uint16_t) {},
        [](void*, const uint8_t*, size_t, uint32_t) {},
        [](void*, uint8_t*, size_t, size_t&) { return false; }, [](void*) {}, nullptr, nullptr);
    std::vector<store::Frame> frames(32U);
    std::vector<uint8_t> body;
    for (size_t index = 0U; index < frames.size(); ++index) {
        auto& frame = frames[index]; frame.durationMs = 50U;
        for (size_t emitter = 0U; emitter < frame.emitters.size(); ++emitter)
            frame.emitters[emitter].color = {uint8_t(emitter + index), uint8_t(emitter * 7U + index), uint8_t(emitter * 13U + index)};
        std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{}; size_t size = 0U;
        Check(modules::litchcraft_wire_codec::FrameCodec::Encode(index == 0U ? nullptr : &frames[index - 1U], frame,
            encoded.data(), encoded.size(), size, modules::litchcraft_wire_codec::RgbOnlyMode::Enabled), "hole probe builds valid canonical RGB frame");
        std::array<uint8_t, 6U> header{}; fxe::WriteU16LE(uint16_t(size), header.data());
        fxe::WriteU32LE(fxe::Crc32(encoded.data(), size), header.data() + 2U);
        body.insert(body.end(), header.begin(), header.end()); body.insert(body.end(), encoded.begin(), encoded.begin() + size);
    }
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> begin(53U); begin[0]=0x4dU; begin[1]=2U; begin[2]=18U;
    fxe::WriteU32LE(99U, begin.data()+3U); fxe::WriteU16LE(7U, begin.data()+7U);
    fxe::WriteU32LE(uint32_t(body.size()), begin.data()+13U); fxe::WriteU32LE(fxe::Crc32(body.data(), body.size()), begin.data()+17U);
    fxe::WriteU16LE(32U, begin.data()+21U); begin[23]=3U; begin[24]=9U; fxe::WriteU32LE(1U, begin.data()+25U); packets.push_back(begin);
    for (size_t cursor=0U; cursor<body.size(); cursor+=217U) {
        const size_t count=std::min<size_t>(217U, body.size()-cursor); std::vector<uint8_t> packet(13U+count);
        std::copy_n(begin.begin(),9U,packet.begin());packet[2]=19U;fxe::WriteU32LE(uint32_t(cursor),packet.data()+9U);
        std::copy_n(body.data()+cursor,count,packet.data()+13U);packets.push_back(packet);
    }
    std::vector<uint8_t> commit(13U);std::copy_n(begin.begin(),9U,commit.begin());commit[2]=21U;
    fxe::WriteU32LE(uint32_t(body.size()),commit.data()+9U);packets.push_back(commit);
    Check(packets.size()>2U * modules::ConnectionSession::OBJECT_CAPACITY,"hole probe has more than two full windows of valid Store packets");
    const auto token = Peer::ObjectToken(service);
    uint32_t accepted = 0U, grant = modules::ConnectionSession::OBJECT_CAPACITY, sent = 0U;
    const auto progress = [&] {
        Check(Peer::TakeAdmissionProgress(service, accepted, grant), "bulk credit reads actual session Progress");
    };
    const auto send = [&] {
        const uint32_t sequence = sent + 1U;
        Check(sequence <= grant, "sender submits only a currently granted semantic identity");
        const auto& packet = packets[sequence - 1U];
        std::vector<uint8_t> fragment(packet.size() + 4U);
        fragment[0U] = 1U; fragment[1U] = 3U;
        fxe::WriteU16LE(uint16_t(packet.size()), fragment.data() + 2U);
        std::copy(packet.begin(), packet.end(), fragment.begin() + 4U);
        std::array<uint8_t, 244U> bytes{}; size_t size = 0U;
        Check(wire::Codec::Encode({{wire::Kind::Object, modules::ConnectionSession::LITCHCRAFT_OBJECT_OPCODE,
            token, sequence}, {fragment.data(), fragment.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
            "bulk credit encodes exact production Object envelope");
        service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
        ++sent; progress();
    };
    for (unsigned count = 0U; count < modules::ConnectionSession::OBJECT_CAPACITY; ++count) send();
    Check(accepted == modules::ConnectionSession::OBJECT_CAPACITY && grant == modules::ConnectionSession::OBJECT_CAPACITY && Peer::PendingObjectSequence(service) == 0U,
        "all negotiated grants fit the paused downstream FIFO and close without a lost pending Object");
    if (grant != modules::ConnectionSession::OBJECT_CAPACITY) {
        while (sent < 2U * modules::ConnectionSession::OBJECT_CAPACITY && sent < grant) send();
        std::printf("[BULK_CREDIT_OLD] sent=%u accepted=%u grant=%u pending=%u sender_pending=%u high_water=%u\n",
            sent, accepted, grant, Peer::PendingObjectSequence(service), sent - accepted, Peer::StoreQueueHighWater(service));
        return;
    }
    fxe::ManualMillis::AdvanceMs(3000U); service.DrainEventQueue();
    Check(sent == accepted && sent == grant && Peer::PendingObjectSequence(service) == 0U,
        "closed full downstream window leaves sender pending empty beyond the two-second recovery deadline");
    size_t consumed = 0U; unsigned closedWaits = 1U;
    const auto release = [&] {
        std::vector<uint8_t> packet; uint32_t ordinal = 0U;
        Check(Peer::TakeOtaData(service, packet, ordinal) && consumed < packets.size() && packet == packets[consumed],
            "downstream slot release preserves the exact once ordered mixed Store prefix and tail");
        ++consumed; service.DrainEventQueue(); progress();
    };
    release();
    Check(accepted == modules::ConnectionSession::OBJECT_CAPACITY && grant == (modules::ConnectionSession::OBJECT_CAPACITY + 1U), "one copied slot returns exactly one current-stream grant");
    while (sent < packets.size()) {
        while (sent < packets.size() && sent < grant) send();
        Check(accepted == sent && Peer::PendingObjectSequence(service) == 0U,
            "legitimate sender never creates a capacity-pending Object while waiting for downstream work");
        if (sent < packets.size()) {
            fxe::ManualMillis::AdvanceMs(3000U); ++closedWaits;
            release();
        }
    }
    while (consumed < packets.size()) release();
    Check(accepted == packets.size() && grant == accepted + modules::ConnectionSession::OBJECT_CAPACITY && Peer::RawOrdinaryCount(service) == 0U,
        "all mixed compressed-prefix/body/Commit packets retire with the original window restored");
    std::printf("[BULK_CREDIT] sent=%u accepted=%u grant=%u closed_waits=%u wait_ms=3000 sender_pending=0 exact_packets=%zu queue_bytes=%zu\n",
        sent, accepted, grant, closedWaits, consumed, sizeof(modules::BleOtaChunkQueue));
}

void TestBulkCreditReturnTokenAndEpochFences() {
    modules::BleOtaChunkQueue queue; Check(queue.Begin(), "credit accounting begins actual queue");
    const std::array<uint8_t, 3U> bytes{0x4dU, 2U, 19U};
    const auto release = [&] {
        const uint8_t* input = nullptr; size_t size = 0U; uint32_t ordinal = 0U, epoch = 0U;
        Check(queue.TryAcquireReadSlot(input, size, ordinal, epoch), "credit accounting acquires exact queue slot");
        queue.ReleaseReadSlot();
    };
    for (const uint32_t token : {9U, 5U, 9U}) {
        Check(queue.EnqueueReserved(bytes.data(), bytes.size(), token, token) == modules::BleOtaChunkQueue::Admission::Accepted,
            "credit accounting tags actual reserved slot");
        release();
    }
    uint32_t token = 0U, count = 0U;
    Check(queue.TakeReturnedObjectCredits(token, count) && token == 9U && count == 2U,
        "stale token cannot overwrite a newer unconsumed return batch");
    queue.ReleaseReadSlot();
    Check(!queue.TakeReturnedObjectCredits(token, count), "duplicate release and repeated take cannot mint credit");
    Check(queue.Begin(), "legacy ingress reuses retired descriptor metadata safely");
    Check(queue.Enqueue(bytes.data(), bytes.size(), 10U), "legacy raw ingress remains supported"); release();
    Check(!queue.TakeReturnedObjectCredits(token, count), "legacy token zero never returns a shared Object grant");
    Check(queue.EnqueueReserved(bytes.data(), bytes.size(), 11U, 9U) == modules::BleOtaChunkQueue::Admission::Accepted,
        "current stream can own a packet from an older queue epoch");
    queue.AdvanceEpoch(); release();
    Check(queue.TakeReturnedObjectCredits(token, count) && token == 9U && count == 1U,
        "discarding stale queue epoch returns its current-stream reservation exactly once");
    Check(queue.EnqueueReserved(bytes.data(), bytes.size(), 12U, 11U) == modules::BleOtaChunkQueue::Admission::Accepted,
        "new stream acquires independent reservation"); release();
    Check(queue.EnqueueReserved(bytes.data(), bytes.size(), 13U, 9U) == modules::BleOtaChunkQueue::Admission::Accepted,
        "late retired stream release remains distinguishable"); release();
    Check(queue.TakeReturnedObjectCredits(token, count) && token == 11U && count == 1U,
        "late old stream release cannot replace newer unconsumed current credit");
    Check(queue.Begin(), "capacity proof resets isolated queue");
    std::array<uint8_t, 512U> maximum{};
    for (unsigned index = 0U; index < modules::ConnectionSession::OBJECT_CAPACITY; ++index)
        Check(queue.EnqueueReserved(maximum.data(), maximum.size(), index, 13U) == modules::BleOtaChunkQueue::Admission::Accepted,
            "all negotiated512-byte shared logical packets fit the reserved byte arena");
    Check(queue.Enqueue(maximum.data(), maximum.size(), (modules::ConnectionSession::OBJECT_CAPACITY + 1U)), "maximum raw512 record fits alongside all negotiated maximum-size reservations");
    std::printf("[BULK_CREDIT_QUEUE] sizeof=%zu arena=%zu guaranteed_payload=%zu bookkeeping=%zu stale=ignored zero=none duplicate=none epoch=returned\n",
        sizeof(queue), queue.BYTE_CAPACITY, queue.GUARANTEED_PAYLOAD_BYTES, queue.CREDIT_STORAGE_BYTES);
}

void TestSharedLogicalBulkBoundAndOrdinaryFragments() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    struct Scenario { size_t logicalSize; bool ordinary; };
    for (const auto scenario : {Scenario{244U, false}, Scenario{245U, false},
                                Scenario{512U, false}, Scenario{513U, false},
                                Scenario{244U, true}, Scenario{245U, true}}) {
        Harness harness; modules::BleService service;
        Check(Peer::Configure(service, harness.settings, harness.controlMutation), "logical bound begins real service queues");
        Peer::PrepareProducerGeneration(service);
        Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, 1U), "one-credit stream can assemble a bounded prefix");
        const size_t logicalSize = scenario.logicalSize;
        const bool ordinary = scenario.ordinary;
        const bool valid = logicalSize <= (ordinary ? 244U : modules::ConnectionSession::OBJECT_PACKET_BYTES);
        std::vector<uint8_t> logical(logicalSize, 0x55U);
        logical[0U] = 0x4dU; logical[1U] = 2U; logical[2U] = ordinary ? 10U : 19U;
        uint32_t sequence = 0U, accepted = 0U, grant = 1U;
        for (size_t offset = 0U; offset < logical.size();) {
            const bool first = offset == 0U; const size_t header = first ? 4U : 2U;
            const size_t count = std::min<size_t>(234U - header, logical.size() - offset);
            const bool last = offset + count == logical.size();
            const bool rejected = !valid &&
                (last || logicalSize > modules::ConnectionSession::OBJECT_PACKET_BYTES);
            Check(sequence + 1U <= grant, "partial prefix returns credit before its next fragment even at capacity1");
            std::vector<uint8_t> fragment(header + count); fragment[0U] = 1U;
            fragment[1U] = (first ? 1U : 0U) | (last ? 2U : 0U);
            if (first) fxe::WriteU16LE(uint16_t(logical.size()), fragment.data() + 2U);
            std::copy_n(logical.data() + offset, count, fragment.data() + header);
            std::array<uint8_t, 244U> bytes{}; size_t size = 0U;
            wire::Codec::Encode({{wire::Kind::Object, modules::ConnectionSession::LITCHCRAFT_OBJECT_OPCODE,
                Peer::ObjectToken(service), ++sequence}, {fragment.data(), fragment.size()}}, {bytes.data(), bytes.size()}, size);
            Peer::DispatchObject(service, bytes.data(), size, sequence);
            if (rejected) {
                wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Query), Peer::ObjectToken(service), 0U}, {}},
                    {bytes.data(), bytes.size()}, size);
                Peer::Session(service).Ingress(7U, bytes.data(), size);
            }
            Check(Peer::TakeAdmissionProgress(service, accepted, grant), "fragment path returns actual cumulative session truth");
            Check(accepted == (rejected ? sequence - 1U : sequence) &&
                      grant == (last || rejected ? sequence : sequence + 1U),
                "completed packet holds one reservation or rejects oversized bytes without fabricated credit");
            offset += count;
            if (rejected) break;
        }
        std::vector<uint8_t> actual; uint32_t ordinal = 0U; modules::BleEvent event;
        if (!valid) {
            Check(!Peer::TakeOtaData(service, actual, ordinal) && !Peer::TakeDerived(service, event) &&
                Peer::PendingObjectSequence(service) == 0U, "oversized shared logical packet never reaches either domain FIFO");
        } else if (ordinary) {
            Check(Peer::TakeDerived(service, event) && event.type == modules::BleEventType::ConnectionLitchcraft &&
                event.payload.connectionLitchcraft.len == logical.size() &&
                std::equal(logical.begin(), logical.end(), event.payload.connectionLitchcraft.input),
                "supported244-byte ordinary fragmented packet remains byte exact");
            Peer::Handle(service, event);
            Check(Peer::TakeAdmissionProgress(service, accepted, grant) && grant == sequence + 1U,
                "ordinary derived dequeue returns its one held reservation");
        } else {
            Check(Peer::TakeOtaData(service, actual, ordinal) && actual == logical,
                "supported shared bulk packet up to512 bytes remains byte exact");
            service.DrainEventQueue();
            Check(Peer::TakeAdmissionProgress(service, accepted, grant) && grant == sequence + 1U,
                "cooperative queue-release handoff returns the completed bulk reservation");
        }
    }
    std::puts("[SHARED_LOGICAL_BOUND] bulk244_245_512=exact bulk513=rejected ordinary244=exact ordinary245=rejected capacity1_prefix=live");
}

void TestObjectCapacityAndDerivedEventLifetimes() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    Harness harness;
    modules::BleService service;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "capacity fixture initializes real queues");
    Peer::PrepareProducerGeneration(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, modules::ConnectionSession::OBJECT_CAPACITY), "capacity fixture negotiates current Object session");
    const std::string begin = R"({"cmd":"ota_sim_begin","size":200,"chunk":100})";
    std::vector<uint8_t> fragment(begin.size() + 4U);
    fragment[0U] = 1U; fragment[1U] = 3U;
    fxe::WriteU16LE(uint16_t(begin.size()), fragment.data() + 2U);
    std::copy(begin.begin(), begin.end(), fragment.begin() + 4U);
    Peer::FillIngress(service, false);
    const uint32_t epoch = Peer::ChunkEpoch(service);
    for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
        Check(!Peer::HandleConnectionOtaControl(service, fragment.data(), fragment.size()) && Peer::ChunkEpoch(service) == epoch,
              "full same FIFO defers valid Begin without advancing OTA epoch or latching failure");
    }
    Check(Peer::DropOrdinary(service) && Peer::HandleConnectionOtaControl(service, fragment.data(), fragment.size()) &&
              Peer::ChunkEpoch(service) == epoch + 1U,
          "one returned raw slot copies Begin and advances OTA epoch exactly once");
    Peer::DiscardOrdinary(service);
    const auto token = Peer::ObjectToken(service);
    std::vector<uint8_t> packet(244U); size_t encoded = 0U;
    Check(wire::Codec::Encode({{wire::Kind::Object, modules::ConnectionSession::OTA_CONTROL_OBJECT_OPCODE, token, 1U},
                               {fragment.data(), fragment.size()}}, {packet.data(), packet.size()}, encoded) == wire::Error::None,
          "derived lifecycle encodes actual shared Begin");
    Peer::DispatchObject(service, packet.data(), encoded, 71U);
    uint32_t accepted = 0U, grant = 0U;
    Check(Peer::TakeAdmissionProgress(service, accepted, grant) && accepted == 1U && grant == modules::ConnectionSession::OBJECT_CAPACITY,
          "ordinary derived Begin retains the raw slot instead of granting another packet");
    modules::BleEvent derived;
    Check(Peer::TakeDerived(service, derived) && derived.type == modules::BleEventType::OtaControl &&
              derived.payload.otaControl.objectCreditToken == token && derived.ingressOrdinal == 71U,
          "derived Begin carries explicit stream credit and original ingress identity");
    Peer::Handle(service, derived);
    Check(Peer::TakeAdmissionProgress(service, accepted, grant) && accepted == 1U && grant == (modules::ConnectionSession::OBJECT_CAPACITY + 1U),
          "actual derived dequeue returns one reservation before domain dispatch");
    // Preserve the exact244-byte existing nonbulk V2 ReplaceChunk contract.
    std::vector<uint8_t> craft(244U, 0x55U);
    craft[0U] = 0x4dU; craft[1U] = 2U; craft[2U] = 10U;
    std::vector<uint8_t> first(234U); first[0U] = 1U; first[1U] = 1U;
    fxe::WriteU16LE(uint16_t(craft.size()), first.data() + 2U);
    std::copy_n(craft.begin(), 230U, first.begin() + 4U);
    std::vector<uint8_t> last(16U); last[0U] = 1U; last[1U] = 2U;
    std::copy(craft.begin() + 230U, craft.end(), last.begin() + 2U);
    Check(Peer::HandleConnectionLitchcraft(service, first.data(), first.size()) &&
              Peer::HandleConnectionLitchcraft(service, last.data(), last.size()),
          "full244 nonbulk Craft remains accepted without truncation");
    Check(Peer::TakeDerived(service, derived) && derived.type == modules::BleEventType::ConnectionLitchcraft &&
              derived.payload.connectionLitchcraft.len == 244U &&
              std::equal(craft.begin(), craft.end(), derived.payload.connectionLitchcraft.input),
          "full244 derived Craft shares the520-byte union without truncation");
    const auto oldToken = token;
    Peer::Session(service).End(7U);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, modules::ConnectionSession::OBJECT_CAPACITY) && Peer::ObjectToken(service) > oldToken,
          "replacement session never reuses the retired derived token");
    Peer::Handle(service, derived);
    Peer::Session(service).ReturnObjectCredit(oldToken);
    std::array<uint8_t, 20U> query{}; encoded = 0U;
    wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Query), Peer::ObjectToken(service), 0U}, {}},
                        {query.data(), query.size()}, encoded);
    service.OnConnectionSessionWrite(query.data(), encoded); service.DrainEventQueue();
    Check(Peer::TakeAdmissionProgress(service, accepted, grant) && accepted == 0U && grant == modules::ConnectionSession::OBJECT_CAPACITY,
          "stale derived dequeue cannot enlarge a new session's initial window");
    const std::string info = R"({"cmd":"info"})";
    std::vector<uint8_t> infoFragment(info.size() + 4U);
    infoFragment[0U] = 1U; infoFragment[1U] = 3U;
    fxe::WriteU16LE(uint16_t(info.size()), infoFragment.data() + 2U);
    std::copy(info.begin(), info.end(), infoFragment.begin() + 4U);
    wire::Codec::Encode({{wire::Kind::Object, modules::ConnectionSession::OTA_CONTROL_OBJECT_OPCODE, Peer::ObjectToken(service), 1U},
                        {infoFragment.data(), infoFragment.size()}}, {packet.data(), packet.size()}, encoded);
    Peer::DispatchObject(service, packet.data(), encoded, 81U);
    Check(Peer::TakeAdmissionProgress(service, accepted, grant) && accepted == 1U && grant == (modules::ConnectionSession::OBJECT_CAPACITY + 1U),
          "copying info into independent priority lane returns raw credit immediately");
    derived.payload.otaControl = modules::BleOtaControlEvent{};
    derived.payload.otaControl.objectCreditToken = 0xa5a5a5a5U;
    Check(Peer::TakeDerived(service, derived) && derived.type == modules::BleEventType::OtaControl &&
              derived.payload.otaControl.objectCreditToken == 0U,
          "priority reconstruction clears poisoned old credit metadata");
    modules::BleService bulk;
    Check(Peer::Configure(bulk, harness.settings, harness.controlMutation), "leased bulk fixture initializes real queue");
    const std::array<uint8_t, 3U> bytes{{0x4dU, 2U, 19U}};
    for (unsigned count = 0U; count < modules::BleOtaChunkQueue::CAPACITY; ++count)
        Check(Peer::QueueBulk(bulk, bytes.data(), bytes.size()), "leased fixture fills exact FIFO64");
    Check(!Peer::HandleConnectionOtaData(bulk, bytes.data(), bytes.size()) && !Peer::HasBulkFailure(bulk),
          "leased capacity deferral cannot become a sticky Full abort");
    std::puts("[OBJECT_CAPACITY] begin_epoch=once_after_capacity same_fifo=held full244=exact stale_credit=fenced leased_full=nonfatal");
}

void TestDeferredStoreBeginKeepsCancelBarrierAndRepairLive() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    struct Probe { bool held = true; bool begin = false; unsigned cancels = 0U; bool cancelAfterBegin = false; } probe;
    Harness harness;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(harness.settings, engine, state);
    modules::BleService service;
    NimBLECharacteristic characteristic;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "pending Begin fixture initializes queues");
    Peer::ConfigureLitchcraft(service, game, characteristic); Peer::PrepareProducerGeneration(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, modules::ConnectionSession::OBJECT_CAPACITY), "pending Begin fixture activates real session");
    Peer::MarkConnected(service, true);
    service.SetLitchcraftFrameIngressCallbacks(
        [](void* context, modules::BleOtaChunkQueue& queue, modules::litchcraft_frame_store::StoreProjection, uint32_t, uint16_t) {
            auto& value = *static_cast<Probe*>(context);
            if (value.held) return;
            const uint8_t* bytes = nullptr; size_t size = 0U; uint32_t ordinal = 0U;
            if (queue.TryAcquireReadSlot(bytes, size, ordinal)) {
                if (size >= 3U && bytes[2U] == 18U) value.begin = true;
                queue.ReleaseReadSlot();
            }
        },
        [](void* context, const uint8_t*, size_t, uint32_t) {
            auto& value = *static_cast<Probe*>(context); ++value.cancels; value.cancelAfterBegin = value.begin;
        }, [](void*, uint8_t*, size_t, size_t&) { return false; }, [](void*) {}, nullptr, &probe);
    const std::array<uint8_t, 3U> chunk{{0x4dU, 2U, 19U}};
    for (unsigned count = 0U; count < 64U; ++count) Check(Peer::QueueBulk(service, chunk.data(), chunk.size()), "pending Begin fixture fills FIFO64");
    std::array<uint8_t, 8U> openBody{{1U, 0U, 0U, 8U, 10U, 0U, 0U, 0U}};
    std::array<uint8_t, 244U> bytes{}; size_t size = 0U;
    wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Open), Peer::Session(service).ConnectionToken(), 2U},
                        {openBody.data(), openBody.size()}}, {bytes.data(), bytes.size()}, size);
    Check(Peer::Session(service).Ingress(7U, bytes.data(), size) == modules::ConnectionSession::Result::Ok,
          "priority Cancel opens the existing Ordered stream");
    uint32_t accepted = 0U, grant = 0U; (void)Peer::TakeAdmissionProgress(service, accepted, grant);
    std::array<uint8_t, 57U> begin{}; begin[0U] = 1U; begin[1U] = 3U; begin[2U] = 53U;
    begin[4U] = 0x4dU; begin[5U] = 2U; begin[6U] = 18U;
    wire::Codec::Encode({{wire::Kind::Object, modules::ConnectionSession::LITCHCRAFT_OBJECT_OPCODE, Peer::ObjectToken(service), 1U},
                        {begin.data(), begin.size()}}, {bytes.data(), bytes.size()}, size);
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    Check(Peer::PendingObjectSequence(service) == 1U, "full domain FIFO retains Store Begin before priority Cancel");
    std::array<uint8_t, 9U> cancel{{0x4dU, 2U, 22U}};
    wire::Codec::Encode({{wire::Kind::Ordered, 22U, Peer::Session(service).StreamToken(), 1U},
                        {cancel.data(), cancel.size()}}, {bytes.data(), bytes.size()}, size);
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    Check(probe.cancels == 0U && Peer::PendingObjectSequence(service) == 1U,
          "later priority Cancel cannot pass retained Begin and allow its resurrection");
    wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Query), Peer::ObjectToken(service), 0U}, {}},
                        {bytes.data(), bytes.size()}, size);
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    Check(Peer::TakeAdmissionProgress(service, accepted, grant) && accepted == 0U && grant == modules::ConnectionSession::OBJECT_CAPACITY,
          "Query passes blocked Cancel while pending Begin keeps exact original reservation");
    probe.held = false;
    for (unsigned pass = 0U; pass < 70U && probe.cancels == 0U; ++pass) service.DrainEventQueue();
    Check(probe.begin && probe.cancels == 1U && probe.cancelAfterBegin && Peer::PendingObjectSequence(service) == 0U,
          "existing worker drains, owns Begin, then receives one priority Cancel without late resurrection");
    service.OnDisconnected(19); service.DrainEventQueue();
    Check(!service.IsConnected() && !Peer::Session(service).HasPendingObject(), "disconnect still retires the capacity-blocked lifetime");
    std::puts("[OBJECT_CANCEL] pending_begin=ordered repair=live downstream=drains cancel=once_after_begin disconnect=retires");
}

void TestPendingObjectCannotRecopyAfterMidDrainReplacement() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    struct Probe {
        modules::BleService* service = nullptr;
        bool replacementPublished = false;
        bool hookReleasedSlot = false;
        unsigned beforeReconcileWakes = 0U;
        unsigned staleCopies = 0U;
        uint32_t copiedQueueEpoch = 0U;
        std::array<uint8_t, 14U> pending{{0x4dU, 2U, 19U, 0x9aU, 0U, 0U, 0U, 7U, 0U, 0U, 0U, 0U, 0U, 0x5aU}};
    } probe;
    Harness harness;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(harness.settings, engine, state);
    modules::BleService service;
    NimBLECharacteristic characteristic;
    probe.service = &service;
    service.SetConnectionSessionBootNonce(UINT64_C(0x8899aabbccddeeff));
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "pending generation fence fixture initializes real queues");
    Peer::ConfigureLitchcraft(service, game, characteristic);
    Peer::PrepareProducerGeneration(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, modules::ConnectionSession::OBJECT_CAPACITY), "pending generation fence fixture activates session");
    Peer::MarkConnected(service, true);
    service.SetLitchcraftFrameIngressCallbacks(
        [](void* context, modules::BleOtaChunkQueue& queue, modules::litchcraft_frame_store::StoreProjection, uint32_t, uint16_t) {
            auto& value = *static_cast<Probe*>(context);
            // Observe the destination at the escaped boundary, before a later
            // projection resets the old Session cursor and hides a stale copy.
            if (!value.replacementPublished ||
                Peer::Session(*value.service).Generation() == Peer::CurrentConnectionEpoch(*value.service)) return;
            ++value.beforeReconcileWakes;
            const uint8_t* bytes = nullptr; size_t size = 0U; uint32_t ordinal = 0U, queueEpoch = 0U;
            while (queue.TryAcquireReadSlot(bytes, size, ordinal, queueEpoch)) {
                if (size == value.pending.size() && std::equal(value.pending.begin(), value.pending.end(), bytes)) {
                    ++value.staleCopies;
                    value.copiedQueueEpoch = queueEpoch;
                }
                queue.ReleaseReadSlot();
            }
        }, [](void*, const uint8_t*, size_t, uint32_t) {},
        [](void*, uint8_t*, size_t, size_t&) { return false; }, [](void*) {}, nullptr, &probe);
    const std::array<uint8_t, 3U> filler{{0x4dU, 2U, 19U}};
    for (unsigned count = 0U; count < modules::BleOtaChunkQueue::CAPACITY; ++count)
        Check(Peer::QueueBulk(service, filler.data(), filler.size()), "pending generation fence fills exact destination FIFO64");
    std::array<uint8_t, 18U> fragment{}; fragment[0U] = 1U; fragment[1U] = 3U;
    fxe::WriteU16LE(uint16_t(probe.pending.size()), fragment.data() + 2U);
    std::copy(probe.pending.begin(), probe.pending.end(), fragment.begin() + 4U);
    std::array<uint8_t, 244U> bytes{}; size_t size = 0U;
    Check(wire::Codec::Encode({{wire::Kind::Object, modules::ConnectionSession::LITCHCRAFT_OBJECT_OPCODE, Peer::ObjectToken(service), 1U},
                               {fragment.data(), fragment.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "pending generation fence encodes actual shared Store envelope");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    Check(Peer::PendingObjectSequence(service) == 1U, "full FIFO retains old generation Object before replacement callback");
    Peer::BeforeNextIngressDispatch(service, [](void* context) {
        auto& value = *static_cast<Probe*>(context);
        std::vector<uint8_t> dropped; uint32_t ordinal = 0U;
        value.hookReleasedSlot = Peer::TakeOtaData(*value.service, dropped, ordinal);
        value.service->OnDisconnected(19);
        value.service->OnConnected(19U, "pending-object-replacement", 247U, 1U);
        value.replacementPublished = true;
    }, &probe);
    service.DrainEventQueue();
    Check(probe.hookReleasedSlot && probe.replacementPublished,
          "mid-drain callback releases capacity and publishes a replacement after iteration reconciliation");
    Check(probe.beforeReconcileWakes == 0U && probe.staleCopies == 0U,
          "old pending Object cannot copy or wake destination after callback epoch changes before reconciliation");
    std::printf("[OBJECT_GENERATION_FENCE] before_reconcile_wakes=%u stale_copies=%u copied_queue_epoch=%u current_queue_epoch=%u\n",
                probe.beforeReconcileWakes, probe.staleCopies, probe.copiedQueueEpoch, Peer::ChunkEpoch(service));
    service.DrainEventQueue();
    Check(service.IsConnected() && Peer::Session(service).Generation() == Peer::CurrentConnectionEpoch(service) &&
              Peer::PendingObjectSequence(service) == 0U,
          "next reconciliation retires old pending ownership and preserves replacement connection");
}

void TestGenericDrainRetriesFinalHistoryObjectWithoutNewIngress() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    struct Probe { bool ready = false; unsigned calls = 0U; unsigned admitted = 0U; } probe;
    Harness harness;
    modules::BleService service;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "generic drain fixture initializes existing queues");
    Peer::PrepareProducerGeneration(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, modules::ConnectionSession::OBJECT_CAPACITY), "generic drain fixture opens real history Object session");
    service.SetConnectionGameHistoryIngressCallback([](void* context, const uint8_t*, size_t, uint32_t, bool) {
        auto& value = *static_cast<Probe*>(context); ++value.calls;
        if (!value.ready) return false;
        ++value.admitted; return true;
    }, &probe);
    const std::array<uint8_t, 12U> body{{1U, 3U, 8U, 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}};
    std::array<uint8_t, 244U> bytes{}; size_t size = 0U;
    wire::Codec::Encode({{wire::Kind::Object, modules::ConnectionSession::GAME_HISTORY_OBJECT_OPCODE, Peer::ObjectToken(service), 1U},
                        {body.data(), body.size()}}, {bytes.data(), bytes.size()}, size);
    service.OnConnectionSessionWrite(bytes.data(), size);
    service.DrainEventQueue();
    Check(Peer::PendingObjectSequence(service) == 1U && !Peer::HasQueuedEvent(service) && probe.admitted == 0U,
          "common drain retains the final history Object with no suffix or unrelated ingress");
    probe.ready = true;
    service.DrainEventQueue();
    uint32_t accepted = 0U, grant = 0U;
    Check(Peer::PendingObjectSequence(service) == 0U && probe.admitted == 1U &&
              Peer::TakeAdmissionProgress(service, accepted, grant) && accepted == 1U && grant == (modules::ConnectionSession::OBJECT_CAPACITY + 1U),
          "next empty-queue drain retries the final Object after worker capacity returns without a Query");
    std::puts("[OBJECT_GENERIC_DRAIN] empty_ingress=retry final_history=once grant=returned");
}

void TestConnectionSessionObjectBurstFitsIngressQueue() {
    Harness harness;
    modules::BleService service;
    Check(modules::BleServiceConnectionEpochTestPeer::Configure(
              service, harness.settings, harness.controlMutation),
          "connection-session object burst fixture initializes queues");

    std::array<uint8_t, modules::ConnectionSession::OBJECT_PACKET_BYTES> packet{};
    packet.fill(0xa5U);
    packet[0U] = 0x44U;  // Object kind with the negotiated connection header.
    for (uint8_t sequence = 1U;
         sequence <= modules::ConnectionSession::OBJECT_CAPACITY;
         ++sequence) {
        packet[6U] = sequence;
        service.OnConnectionSessionWrite(packet.data(), packet.size());
    }

    unsigned queued = 0U;
    modules::BleEvent event;
    while (modules::BleServiceConnectionEpochTestPeer::TakeDerived(service, event)) {
        ++queued;
        packet[6U] = static_cast<uint8_t>(queued);
        Check(event.type == modules::BleEventType::ConnectionSession &&
                  event.payload.connectionSession.len == packet.size() &&
                  std::equal(packet.begin(), packet.end(), event.payload.connectionSession.input),
              "every granted callback slot retains all512 bytes and its ordered identity");
    }
    Check(queued == modules::ConnectionSession::OBJECT_CAPACITY,
          "the firmware ingress queue retains one complete negotiated Object burst");
}

void TestSessionLivenessRequiresValidOwnedTrafficAndDisconnectRetires() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(500U);
    Harness harness;
    modules::BleService service;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation),
          "liveness fixture initializes production queues");
    service.SetConnectionSessionBootNonce(UINT64_C(0x8899aabbccddeeff));
    service.OnConnected(7U, "liveness", 23U, 1U);
    service.DrainEventQueue();
    Check(Peer::ActivateMinimumConnectionObjectSession(service, Peer::ProducerEpoch(service)),
          "liveness fixture activates provider session");
    const auto initialActivity = Peer::LastClientActivityMs(service);
    fxe::ManualMillis::AdvanceMs(100U);
    const uint8_t malformed = 0U;
    service.OnConnectionSessionWrite(&malformed, 1U);
    service.DrainEventQueue();
    Check(Peer::LastClientActivityMs(service) == initialActivity,
          "malformed session traffic cannot keep the native link alive");
    Check(service.GetSessionDiagnostics().received == 1U &&
              service.GetSessionDiagnostics().lastIngressResult ==
                  static_cast<uint8_t>(modules::ConnectionSession::Result::Malformed),
          "session diagnostics distinguish malformed dispatch from missing ingress");
    std::array<uint8_t, 20U> heartbeat{};
    size_t heartbeatSize = 0U;
    const auto token = Peer::ConnectionToken(service);
    Check(wire::Codec::Encode(
              {{wire::Kind::Control, uint8_t(wire::Control::Heartbeat), token + 100U, 0U}, {}},
              {heartbeat.data(), heartbeat.size()}, heartbeatSize) == wire::Error::None,
          "obsolete-token heartbeat encodes");
    service.OnConnectionSessionWrite(heartbeat.data(), heartbeatSize);
    service.DrainEventQueue();
    Check(Peer::LastClientActivityMs(service) == initialActivity,
          "wrong session token cannot refresh current connection activity");
    fxe::WriteU32LE(token, heartbeat.data() + 2U);
    service.OnConnectionSessionWrite(heartbeat.data(), heartbeatSize);
    service.DrainEventQueue();
    Check(Peer::LastClientActivityMs(service) == fxe::ManualMillis::NowMs(),
          "valid owned heartbeat refreshes provider liveness without reply");
    Check(service.GetSessionDiagnostics().received == 3U &&
              service.GetSessionDiagnostics().lastIngressResult == 0U &&
              (service.GetSessionDiagnostics().receivedControls &
               (1U << uint8_t(wire::Control::Heartbeat))) != 0U,
          "session diagnostics observe production dispatch without changing liveness");
    const auto validActivity = Peer::LastClientActivityMs(service);
    fxe::ManualMillis::AdvanceMs(100U);
    heartbeat[0U] ^= 0x10U;
    service.OnConnectionSessionWrite(heartbeat.data(), heartbeatSize);
    service.DrainEventQueue();
    Check(Peer::LastClientActivityMs(service) == validActivity,
          "incompatible wire epoch does not extend connection life");
    service.OnDisconnected(19);
    service.DrainEventQueue();
    Check(!Peer::PortableSessionActive(service) && Peer::ObjectToken(service) == 0U,
          "disconnect retires old provider generation despite advanced callback epoch");
    service.OnConnected(8U, "replacement", 23U, 1U);
    service.DrainEventQueue();
    Check(Peer::ObjectToken(service) == 0U && !Peer::PortableSessionActive(service),
          "replacement starts fresh negotiation without old stream authority");
    Check(service.GetSessionDiagnostics().received == 0U &&
              service.GetSessionDiagnostics().receivedControls == 0U,
          "new connection diagnostic masks cannot inherit a retired session");
    Check(Peer::ActivateMinimumConnectionObjectSession(service, Peer::ProducerEpoch(service)),
          "replacement activates before native stack teardown");
    Peer::ShutdownLifetime(service);
    Check(!Peer::PortableSessionActive(service) && Peer::ObjectToken(service) == 0U,
          "native stack teardown retires common session through production cleanup owner");
}

void TestRefusedBeginEndRetriesImmediatelyAfterQueuePressure() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    Harness harness;
    modules::BleService service;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation),
          "fragment refusal fixture initializes real ingress queues");
    const std::array<uint8_t, 12U> inspect{{1U, 3U, 8U, 0U,
        0x4cU, 12U, 0U, 0U, 0U, 0U, 9U, 0U}};
    Peer::FillIngress(service, true);
    Check(!Peer::HandleConnectionLitchcraft(service, inspect.data(), inspect.size()),
          "full priority FIFO refuses unadmitted complete Litchcraft record");
    (void)Peer::DrainQueuedEvents(service);
    Check(Peer::HandleConnectionLitchcraft(service, inspect.data(), inspect.size()),
          "first exact Begin End retry reaches newly available ingress capacity");
    std::vector<uint8_t> received;
    uint32_t ordinal = 0U, epoch = 0U;
    Check(Peer::TakeLitchcraftEvent(service, received, ordinal, epoch) &&
              received == std::vector<uint8_t>(inspect.begin() + 4U, inspect.end()),
          "refused single record is copied exactly once into existing domain owner");
    const std::string command = R"({"cmd":"ota_begin"})";
    std::vector<uint8_t> control(4U + command.size(), 0U);
    control[0U] = 1U; control[1U] = 3U;
    fxe::WriteU16LE(static_cast<uint16_t>(command.size()), control.data() + 2U);
    std::copy(command.begin(), command.end(), control.begin() + 4U);
    Peer::FillIngress(service, false);
    Check(!Peer::HandleConnectionOtaControl(service, control.data(), control.size()),
          "full ordinary FIFO refuses complete OTA control record");
    (void)Peer::DrainQueuedEvents(service);
    Check(Peer::HandleConnectionOtaControl(service, control.data(), control.size()),
          "first exact OTA Begin End retry also remains eligible");
}

void TestAcceptedFragmentPrefixPreventsGracefulObjectClose() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    Harness harness;
    modules::BleService service;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation),
          "fragment Close fixture initializes real queues");
    Peer::PrepareProducerGeneration(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service),
          "fragment Close fixture activates minimum MTU session");
    const auto token = Peer::ObjectToken(service);
    const std::array<uint8_t, 5U> prefix{{1U, 1U, 8U, 0U, 0x4cU}};
    std::array<uint8_t, 20U> bytes{};
    size_t size = 0U;
    Check(wire::Codec::Encode(
              {{wire::Kind::Object, modules::ConnectionSession::LITCHCRAFT_OBJECT_OPCODE,
                token, 1U}, {prefix.data(), prefix.size()}}, {bytes.data(), bytes.size()}, size) ==
              wire::Error::None, "partial Litchcraft prefix encodes");
    service.OnConnectionSessionWrite(bytes.data(), size);
    service.DrainEventQueue();
    Check(wire::Codec::Encode(
              {{wire::Kind::Control, uint8_t(wire::Control::Close), token, 0U}, {}},
              {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "fragment Close encodes");
    service.OnConnectionSessionWrite(bytes.data(), size);
    service.DrainEventQueue();
    Check(Peer::ObjectToken(service) == token,
          "accepted domain prefix blocks Close rather than losing copied bytes");
    service.OnDisconnected(19);
    service.DrainEventQueue();
    Check(Peer::ObjectToken(service) == 0U,
          "failed shared lifetime retires fragment ownership through normal disconnect");
}

void TestOrderedModeDoesNotWaitForObjectCredit() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using Art = modules::LitchcraftSession;
    namespace wire = modules::connection_session_codec;
    Harness harness;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(harness.settings, engine, state);
    Check(game.LoadOrInitialize(), "Ordered mode initializes actual game authority");
    state.ApplyPhysicalOccupancySnapshot(0U);
    Check(game.CanEnterLitchcraft(), "Ordered mode has the production boot Hall baseline and eligible game authority");
    modules::BleService service;
    NimBLECharacteristic characteristic;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "Ordered mode initializes production ingress");
    Peer::ConfigureLitchcraft(service, game, characteristic);
    Peer::PrepareProducerGeneration(service);
    auto& art = game.Litchcraft();
    art.Configure(3U, 123U);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, modules::ConnectionSession::OBJECT_CAPACITY),
          "Ordered mode opens actual shared session with its negotiated Object reservations");
    service.SetLitchcraftFrameIngressCallbacks(
        [](void*, modules::BleOtaChunkQueue&, modules::litchcraft_frame_store::StoreProjection, uint32_t, uint16_t) {},
        [](void*, const uint8_t*, size_t, uint32_t) {},
        [](void*, uint8_t*, size_t, size_t&) { return false; }, [](void*) {}, nullptr, nullptr);
    std::array<uint8_t, 8U> open{};
    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, open.data());
    open[3U] = modules::ConnectionSession::ORDERED_CAPACITY;
    fxe::WriteU16LE(10U, open.data() + 4U);
    std::array<uint8_t, 244U> bytes{}; size_t size = 0U;
    Check(wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Open),
              Peer::ConnectionToken(service), 2U}, {open.data(), open.size()}},
              {bytes.data(), bytes.size()}, size) == wire::Error::None, "Ordered mode opens independent control stream");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    const auto mode = [&](const uint8_t action, const uint32_t session, const uint16_t request,
                          const uint32_t sequence) {
        std::array<uint8_t, 8U> body{{Art::MAGIC, action}};
        fxe::WriteU32LE(session, body.data() + 2U); fxe::WriteU16LE(request, body.data() + 6U);
        Check(wire::Codec::Encode({{wire::Kind::Ordered, 1U, Peer::OrderedToken(service), sequence},
                  {body.data(), body.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None && size == 18U,
              "unchanged compact mode fits minimum ATT20 Ordered delivery");
        service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    };
    struct Admission {
        bool allowed = false;
        uint32_t nonce = 0U;
        uint32_t ended = 0U;
    } admission;
    game.SetLitchcraftPresentationAdmission({&admission,
        [](void* context, Art::PresentationAccess access) -> uint32_t {
            auto& owner = *static_cast<Admission*>(context);
            Check(access == Art::PresentationAccess::Control,
                  "mode admission does not require a first displayed frame");
            return owner.allowed ? ++owner.nonce : 0U;
        },
        [](void* context, uint32_t nonce) {
            auto& owner = *static_cast<Admission*>(context);
            Check(nonce == owner.nonce && nonce == owner.ended + 1U,
                  "each admitted mutation releases its exact nonce once");
            owner.ended = nonce;
        }});
    admission.allowed = true;
    for (unsigned turn = 0U; turn < 3U; ++turn) {
        game.UpdateLitchcraft();
        game.Update();
    }
    Check(admission.nonce == 0U && admission.ended == 0U && !art.IsActive(),
          "idle Play coordinator updates neither acquire presentation admission nor force its release publication");
    admission.allowed = false;
    mode(1U, 0U, 1U, 1U);
    uint32_t deniedThrough = 0U; uint16_t deniedExceptions = 0U;
    Check(!art.IsActive() && !Peer::TakeOrderedCheckpoint(service, deniedThrough, deniedExceptions),
          "denied output admission retains Enter without mutation or terminal receipt");
    admission.allowed = true;
    Peer::UpdateSharedStates(service);
    Check(art.IsActive(), "Ordered Enter admits inactive Litchcraft through canonical mode owner");
    uint32_t through = 0U; uint16_t exceptions = 0U;
    Check(Peer::TakeOrderedCheckpoint(service, through, exceptions) && through == 1U && exceptions == 0U,
          "Ordered Enter reports Applied after domain admission");
    const uint32_t session = art.SessionId(), revision = art.Revision();
    mode(1U, 0U, 1U, 1U);
    Check(art.SessionId() == session && art.Revision() == revision, "Ordered replay cannot repeat mode mutation");
    std::array<uint8_t, 12U> retiredMode{{1U, 3U, 8U, 0U, Art::MAGIC, 2U}};
    fxe::WriteU32LE(session, retiredMode.data() + 6U); fxe::WriteU16LE(2U, retiredMode.data() + 10U);
    Check(!Peer::HandleConnectionLitchcraft(service, retiredMode.data(), retiredMode.size()),
          "activated Object mode routing is retired before event admission");
    service.DrainEventQueue();
    Check(art.IsActive() && art.Revision() == revision, "rejected Object Exit cannot change board mode");
    std::array<uint8_t, 18U> chunk{{1U, 3U, 14U, 0U, 0x4dU, 2U, 19U}};
    fxe::WriteU32LE(session, chunk.data() + 7U); fxe::WriteU16LE(9U, chunk.data() + 11U);
    for (uint32_t sequence = 1U; sequence <= modules::ConnectionSession::OBJECT_CAPACITY; ++sequence) {
        Check(wire::Codec::Encode({{wire::Kind::Object, 0x40U, Peer::ObjectToken(service), sequence},
                  {chunk.data(), chunk.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
              "paused consumer receives real Store Object packets");
        service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    }
    uint32_t accepted = 0U, grant = 0U;
    Check(Peer::TakeAdmissionProgress(service, accepted, grant) && accepted == modules::ConnectionSession::OBJECT_CAPACITY && grant == modules::ConnectionSession::OBJECT_CAPACITY,
          "Object reservations are genuinely held in production bulk FIFO");
    mode(2U, session, 2U, 2U);
    Check(!art.IsActive() && state.IsPlacementModeActive(), "Ordered Exit restores Setup with all Object reservations held");
    Check(Peer::TakeOrderedCheckpoint(service, through, exceptions) && through == 2U && exceptions == 0U,
          "credit-independent Exit reports actual applied mode outcome");
    Check(wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Query),
              Peer::ObjectToken(service), 0U}, {}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "query observes unchanged Object admission truth after mode delivery");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    Check(Peer::TakeAdmissionProgress(service, accepted, grant) && accepted == modules::ConnectionSession::OBJECT_CAPACITY && grant == modules::ConnectionSession::OBJECT_CAPACITY,
          "mode Exit neither consumes bulk data nor fabricates Object credit");
    mode(2U, session + 1U, 3U, 3U);
    uint16_t reason = 0U;
    Check(Peer::TakeOrderedRejection(service, 3U, &reason) && reason == uint16_t(Art::Status::StaleSession),
          "Ordered mode preserves canonical stale-session rejection");
    Peer::RequireOwnerProof(service, true);
    Check(!Peer::ProductAccess(service), "mode fixture lacks native owner proof");
    mode(1U, 0U, 4U, 4U);
    Check(!art.IsActive() && Peer::TakeOrderedRejection(service, 4U, &reason) &&
              reason == uint16_t(Art::Status::Unsupported),
          "Ordered mode cannot bypass current native owner authorization");
    Peer::RequireOwnerProof(service, false);
    std::array<uint8_t, 8U> stale{{Art::MAGIC, 1U, 0U, 0U, 0U, 0U, 5U, 0U}};
    Check(wire::Codec::Encode({{wire::Kind::Ordered, 1U, Peer::OrderedToken(service), 5U},
              {stale.data(), stale.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "mode epoch fixture queues an otherwise valid Enter");
    service.OnConnectionSessionWrite(bytes.data(), size);
    service.OnDisconnected(19);
    service.DrainEventQueue();
    Check(!art.IsActive() && !Peer::PortableSessionActive(service),
          "retired native connection cannot dispatch its queued Ordered mode request");
    Check(admission.nonce != 0U && admission.nonce == admission.ended,
          "retained ingress leaves no admission nonce across dispatch completion");
    std::printf("[ORDERED_ADMISSION] denied=retained retry=no_retransmit mode=applied_once nonce=released\n");
    std::printf("[ORDERED_MODE_CREDIT] accepted=%u grant=%u active=%u Setup=%u minimum_packet=18\n",
                accepted, grant, art.IsActive() ? 1U : 0U, state.IsPlacementModeActive() ? 1U : 0U);
}

void TestUnknownOrderedRetainsUnsupportedReceipt() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    Harness harness;
    modules::BleService service;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation) &&
              Peer::ActivateMinimumConnectionObjectSession(service, Peer::ProducerEpoch(service)),
          "unknown Ordered receipt uses the real activated minimum-MTU service");
    std::array<uint8_t, 8U> open{};
    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, open.data());
    open[3U] = modules::ConnectionSession::ORDERED_CAPACITY;
    fxe::WriteU16LE(10U, open.data() + 4U);
    std::array<uint8_t, 20U> bytes{}; size_t size = 0U;
    Check(wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Open),
              Peer::ConnectionToken(service), 2U}, {open.data(), open.size()}},
              {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "unknown Ordered fixture opens the normal Ordered stream");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    // Identify28 is now registered. Opcode29 exercises the same normal fallback
    // that unregistered Identify28 reaches on a peer without that implementation.
    const std::array<uint8_t, 8U> probe{{1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}};
    Check(wire::Codec::Encode({{wire::Kind::Ordered, 29U, Peer::OrderedToken(service), 1U},
              {probe.data(), probe.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "unknown opcode enters actual production Ordered dispatch");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    uint16_t reason = 0U; uint16_t exceptions = 0U; uint32_t through = 0U;
    Check(Peer::TakeOrderedRejection(service, 1U, &reason) &&
              reason == uint16_t(modules::LitchcraftSession::Status::Unsupported),
          "normal production unknown opcode returns Rejected Unsupported, never silent or Applied");
    Check(Peer::TakeOrderedCheckpoint(service, through, exceptions) && through == 1U && exceptions == 1U,
          "Unsupported checkpoint retains its exceptional bit until Confirm");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    Check(wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Query),
              Peer::OrderedToken(service), 1U}, {}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "terminal recovery explicitly queries the retained exceptional sequence");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    Check(Peer::TakeOrderedRejection(service, 1U, &reason) && reason == 4U &&
              Peer::PortableSessionActive(service),
          "replayed unknown request and Query retain Unsupported without retiring the session");
    std::printf("[UNSUPPORTED_ORDERED] opcode=29 identify28_fallback=equivalent outcome=rejected reason=%u checkpoint=1 exceptions=1 mtu=20 active=1\n", reason);
}

void TestSleepWakeEnumSettingsSharedSessionMutation() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    namespace settingsWire = modules::settings_state_wire;
    Harness harness;
    harness.settings.Doc()["SLEEP_TIMEOUT"] = "disabled";
    harness.settings.Doc()["MOTION_WAKE_MODE"] = "motion";
    harness.settings.Doc()["WHITE_SQUARES_ON"] = false;
    modules::BleService service;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation) &&
              Peer::ActivateMinimumConnectionObjectSession(service, Peer::ProducerEpoch(service)),
          "Sleep/Wake enum journey activates actual minimum-ATT production ingress");
    std::array<uint8_t, 8U> open{};
    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, open.data());
    open[3U] = modules::ConnectionSession::ORDERED_CAPACITY;
    fxe::WriteU16LE(10U, open.data() + 4U);
    std::array<uint8_t, 20U> packet{};
    size_t bytes = 0U;
    Check(wire::Codec::Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Open), Peer::ConnectionToken(service), 2U},
         {open.data(), open.size()}}, {packet.data(), packet.size()}, bytes) == wire::Error::None,
        "Sleep/Wake Ordered stream opens using production framing");
    service.OnConnectionSessionWrite(packet.data(), bytes);
    service.DrainEventQueue();
    struct Mutation { uint8_t index; uint16_t value; const char* key; const char* expected; };
    uint32_t sequence = 0U;
    for (const auto mutation : {Mutation{13U, 0U, "SLEEP_TIMEOUT", "5m"},
          Mutation{21U, 1U, "MOTION_WAKE_MODE", "tap"},
          Mutation{21U, 2U, "MOTION_WAKE_MODE", "motion_and_tap"},
          Mutation{21U, 0U, "MOTION_WAKE_MODE", "motion"},
          Mutation{1U, 1U, "WHITE_SQUARES_ON", nullptr},
          Mutation{26U, 1U, nullptr, nullptr}, Mutation{27U, 1000U, nullptr, nullptr}}) {
        const uint32_t before = harness.settings.GetRevision();
        std::array<uint8_t, 4U> body{{settingsWire::VERSION, mutation.index, 0U, 0U}};
        fxe::WriteU16LE(mutation.value, body.data() + 2U);
        Check(wire::Codec::Encode(
            {{wire::Kind::Ordered, settingsWire::MUTATION_OPCODE, Peer::OrderedToken(service), ++sequence},
             {body.data(), body.size()}}, {packet.data(), packet.size()}, bytes) == wire::Error::None,
            "Sleep/Wake setting uses exact four-byte typed mutation");
        service.OnConnectionSessionWrite(packet.data(), bytes);
        service.DrainEventQueue();
        uint32_t terminal = 0U, revision = 0U;
        uint16_t exceptions = 0U;
        const bool applied = mutation.key != nullptr;
        // The production owner prioritizes a rejected Outcome before Checkpoint.
        const bool rejectedReceipt = applied || Peer::TakeOrderedRejection(service, sequence);
        Check(Peer::TakeOrderedCheckpoint(service, terminal, exceptions, &revision) &&
                  terminal == sequence && ((exceptions & 1U) == 0U) == applied,
              "production Ordered disposition distinguishes applied enums from retired controls");
        if (!applied) {
            Check(rejectedReceipt &&
                      harness.settings.GetRevision() == before,
                  "reserved wake mutations reject before changing settings");
            continue;
        }
        Check(revision == harness.settings.GetRevision() && revision != before,
              "applied setting receipt carries its resulting authoritative revision");
        Check(mutation.expected != nullptr
                  ? std::strcmp(harness.settings.ReadValue(mutation.key, "missing"), mutation.expected) == 0
                  : harness.settings.ReadValue(mutation.key, false),
              "enum strings and bool survive the actual Arduino callback-document lifetime");
        settingsWire::Payload snapshot{};
        Check(settingsWire::Encode(harness.settings, snapshot) &&
                  snapshot.size() == 84U && fxe::ReadU32LE(snapshot.data()) == revision &&
                  fxe::ReadU16LE(snapshot.data() + 4U + mutation.index * 2U) == mutation.value,
              "complete production State confirms the same canonical enum and revision");
        std::printf("[SLEEP_WAKE_SETTING] key=%s normalized=%u disposition=applied revision=%lu snapshot=complete\n",
                    mutation.key, mutation.value, static_cast<unsigned long>(revision));
    }
}

void TestBoardStylePreviewSharedSessionMutation() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    namespace style = modules::settings_state_wire;
    Harness harness;
    modules::BleService service;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation),
          "Board preview shared-session fixture initializes production queues");
    const auto epoch = Peer::ProducerEpoch(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, epoch),
          "Board preview negotiates the real minimum-MTU session");
    std::array<uint8_t, 8U> openBody{};
    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, openBody.data());
    openBody[3U] = modules::ConnectionSession::ORDERED_CAPACITY;
    fxe::WriteU16LE(10U, openBody.data() + 4U);
    std::array<uint8_t, 20U> bytes{};
    size_t size = 0U;
    Check(wire::Codec::Encode(
              {{wire::Kind::Control, uint8_t(wire::Control::Open), Peer::ConnectionToken(service), 2U},
               {openBody.data(), openBody.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "Board preview ordered stream Open encodes");
    service.OnConnectionSessionWrite(bytes.data(), size);
    service.DrainEventQueue();
    Check(Peer::OrderedToken(service) != 0U, "actual service ingress opens the existing ordered stream");
    uint32_t sequence = 0U;
    struct Selection { uint8_t index; uint8_t value; uint32_t projections; };
    for (const Selection selection : {Selection{30U,0U,1U}, Selection{30U,0U,2U},
                                      Selection{5U,0U,3U}, Selection{5U,0U,3U},
                                      Selection{5U,99U,3U}}) {
        const std::array<uint8_t, style::MUTATION_BODY_BYTES> body{{
            style::VERSION, selection.index, selection.value, 0U}};
        Check(wire::Codec::Encode(
                  {{wire::Kind::Ordered, style::MUTATION_OPCODE, Peer::OrderedToken(service), ++sequence},
                   {body.data(), body.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
              "board preview uses existing typed ordered settings mutation");
        service.OnConnectionSessionWrite(bytes.data(), size);
        service.DrainEventQueue();
        Check(harness.styleProjectionCount == selection.projections,
              "fresh placement reselect projects, same mate or rejected value remains quiet");
        service.OnConnectionSessionWrite(bytes.data(), size);
        service.DrainEventQueue();
        Check(harness.styleProjectionCount == selection.projections,
              "actual ordered receipt replay never restarts either board preview");
    }
    harness.bleSettings.OnReadWritePayload("{\"k\":\"SETTLE_ANIM_STYLE\"}");
    Check(harness.PumpOneWriteEvent() && harness.styleProjectionCount == 3U,
          "actual setting read does not start or replay board preview");
    const auto projections = harness.styleProjectionCount;
    service.OnDisconnected(19U);
    service.DrainEventQueue();
    Check(Peer::ActivateMinimumConnectionObjectSession(service, Peer::ProducerEpoch(service)),
          "replacement connection negotiates without a setting write");
    service.OnConnectionSessionWrite(bytes.data(), size);
    service.DrainEventQueue();
    Peer::UpdateSharedStates(service);
    Check(harness.styleProjectionCount == projections,
          "reconnect state publication and stale old token cannot replay a preview");
    std::printf("[BOARD_PREVIEW_SHARED_SESSION] projections=%u fresh_sequences=%u duplicate_read_reconnect=quiet\n",
                projections, sequence);
}

void TestHintStyleSharedSessionMutation() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    namespace style = modules::settings_state_wire;
    Harness harness;
    modules::BleService service;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation),
          "Hint style shared-session fixture initializes production queues");
    const auto epoch = Peer::ProducerEpoch(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, epoch),
          "Hint style negotiates the real minimum-MTU session");
    std::array<uint8_t, 8U> openBody{};
    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, openBody.data());
    openBody[3U] = modules::ConnectionSession::ORDERED_CAPACITY;
    fxe::WriteU16LE(10U, openBody.data() + 4U);
    std::array<uint8_t, 20U> bytes{};
    size_t size = 0U;
    Check(wire::Codec::Encode(
              {{wire::Kind::Control, uint8_t(wire::Control::Open), Peer::ConnectionToken(service), 2U},
               {openBody.data(), openBody.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "Hint style ordered stream Open encodes");
    service.OnConnectionSessionWrite(bytes.data(), size);
    service.DrainEventQueue();
    Check(Peer::OrderedToken(service) != 0U, "actual service ingress opens the existing ordered stream");
    uint32_t sequence = 0U;
    for (const uint16_t value : {0U, 1U, 2U, 3U}) {
        std::array<uint8_t, style::MUTATION_BODY_BYTES> body{{style::VERSION, 39U, 0U, 0U}};
        fxe::WriteU16LE(value, body.data() + 2U);
        Check(wire::Codec::Encode(
                  {{wire::Kind::Ordered, style::MUTATION_OPCODE, Peer::OrderedToken(service), ++sequence},
                   {body.data(), body.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
              "existing index39 typed mutation encodes at minimum MTU");
        service.OnConnectionSessionWrite(bytes.data(), size);
        service.DrainEventQueue();
        Peer::UpdateSharedStates(service);
        const char* const values[]{"foxie_sweep", "player_clockwise", "none", "beachball"};
        const char* expected = values[value];
        Check(std::strcmp(harness.settings.ReadValue("HINT_THINKING_STYLE", "missing"), expected) == 0,
              "actual shared-session ingress applies each retained style0/1/2 and appended Beachball3 through the common mutation owner");
        style::Payload snapshot{};
        Check(style::Encode(harness.settings, snapshot) && snapshot[80U] == value && snapshot[81U] == 0U,
              "shared Settings state retains the index39 two-byte style tail");
        const auto revision = harness.settings.GetRevision();
        service.OnConnectionSessionWrite(bytes.data(), size);
        service.DrainEventQueue();
        Peer::UpdateSharedStates(service);
        Check(harness.settings.GetRevision() == revision,
              "shared-session replay never reapplies or increments the style mutation");
        std::printf("[HINT_STYLE_SHARED_SESSION] metadata_index=37 wire_index=39 value=%u token=%u sequence=%u revision=%u\n",
                    value, Peer::OrderedToken(service), sequence, revision);
    }
    const auto beforeOld = harness.settings.GetRevision();
    const std::array<uint8_t, 4U> oldEpoch{{1U, 39U, 0U, 0U}};
    Check(wire::Codec::Encode(
              {{wire::Kind::Ordered, style::MUTATION_OPCODE, Peer::OrderedToken(service), ++sequence},
               {oldEpoch.data(), oldEpoch.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "old Settings epoch1 packet enters the real session envelope");
    service.OnConnectionSessionWrite(bytes.data(), size);
    service.DrainEventQueue();
    Check(harness.settings.GetRevision() == beforeOld &&
              std::strcmp(harness.settings.ReadValue("HINT_THINKING_STYLE", "missing"), "beachball") == 0,
          "new Settings peer rejects epoch1 before dispatch or revision mutation");
    for (const uint8_t mode : {0U, 1U}) {
        const std::array<uint8_t, 4U> promotion{{style::VERSION, 40U, mode, 0U}};
        Check(wire::Codec::Encode(
                  {{wire::Kind::Ordered, style::MUTATION_OPCODE, Peer::OrderedToken(service), ++sequence},
                   {promotion.data(), promotion.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
              "promotion uses existing Ordered24 and Settings epoch2");
        service.OnConnectionSessionWrite(bytes.data(), size);
        service.DrainEventQueue();
        Check(std::strcmp(harness.settings.ReadValue("PROMOTION_MODE", "queen"),
                          mode == 0U ? "queen" : "choose") == 0,
              "production typed mutation applies the exact promotion mode");
    }
    const uint32_t beforeInvalid = harness.settings.GetRevision();
    std::array<uint8_t, style::MUTATION_BODY_BYTES> invalid{{style::VERSION, 39U, 5U, 0U}};
    Check(wire::Codec::Encode(
              {{wire::Kind::Ordered, style::MUTATION_OPCODE, Peer::OrderedToken(service), ++sequence},
               {invalid.data(), invalid.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "out-of-range style encodes through the same minimum-MTU path");
    service.OnConnectionSessionWrite(bytes.data(), size);
    service.DrainEventQueue();
    Peer::UpdateSharedStates(service);
    Check(harness.settings.GetRevision() == beforeInvalid &&
              std::strcmp(harness.settings.ReadValue("HINT_THINKING_STYLE", ""), "beachball") == 0,
          "unknown style5 cannot replace confirmed Beachball or advance settings revision");

    harness.settings.Doc()["CHECKMATE_ANIM_STYLE"] = "fireworks";
    const auto reset = harness.controlMutation.ResetSetting("CHECKMATE_ANIM_STYLE", 192U);
    style::Payload defaults{};
    Check(reset.status == modules::ControlMutationStatus::Applied &&
              style::Encode(harness.settings, defaults) && defaults[14U] == 1U,
          "ordinary reset owner removes the saved override and confirms new Fire Lob default at stable index5");

    harness.settings.Doc()["HINT_THINKING_STYLE"] = "none";
    const auto resetHint = harness.controlMutation.ResetSetting("HINT_THINKING_STYLE", 193U);
    style::Payload resetDefaults{};
    Check(resetHint.status == modules::ControlMutationStatus::Applied &&
              !harness.settings.Doc().containsKey("HINT_THINKING_STYLE") &&
              style::Encode(harness.settings, resetDefaults) && resetDefaults[80U] == 4U && resetDefaults[81U] == 0U,
          "the existing ResetSetting owner removes explicit None and publishes canonical Cauldron wire value4");
    uint32_t checkpointSequence = 0U;
    uint16_t exceptions = 0U;
    Check(Peer::TakeOrderedCheckpoint(service, checkpointSequence, exceptions) &&
              checkpointSequence == sequence && sequence == modules::ConnectionSession::ORDERED_CAPACITY &&
              exceptions == ((1U << (sequence - 5U)) | 1U),
          "the full-grant checkpoint identifies rejected old epoch5 and invalid style8 before confirmation");
    for (const uint32_t rejectedSequence : {5U, 8U}) {
        Check(wire::Codec::Encode(
                  {{wire::Kind::Control, uint8_t(wire::Control::Query), Peer::OrderedToken(service), rejectedSequence}, {}},
                  {bytes.data(), bytes.size()}, size) == wire::Error::None,
              "the client queries each retained exceptional outcome named by the checkpoint");
        service.OnConnectionSessionWrite(bytes.data(), size);
        service.DrainEventQueue();
        Check(Peer::TakeOrderedRejection(service, rejectedSequence),
              "each exact rejected sequence and nonzero domain reason arrives before Confirm frees its prefix");
    }
    Check(wire::Codec::Encode(
              {{wire::Kind::Control, uint8_t(wire::Control::Confirm), Peer::OrderedToken(service), checkpointSequence}, {}},
              {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "the client confirms the retained terminal prefix before sending past its grant");
    service.OnConnectionSessionWrite(bytes.data(), size);
    service.DrainEventQueue();
    const std::array<uint8_t, 4U> savedNone{{style::VERSION, 39U, 2U, 0U}};
    Check(wire::Codec::Encode(
              {{wire::Kind::Ordered, style::MUTATION_OPCODE, Peer::OrderedToken(service), ++sequence},
               {savedNone.data(), savedNone.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "a saved None override precedes the appended Cauldron selection");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    Check(std::strcmp(harness.settings.ReadValue("HINT_THINKING_STYLE", ""), "none") == 0,
          "the existing ordered grant applies the explicit None override");
    const std::array<uint8_t, 4U> cauldron{{style::VERSION, 39U, 4U, 0U}};
    Check(wire::Codec::Encode(
              {{wire::Kind::Ordered, style::MUTATION_OPCODE, Peer::OrderedToken(service), ++sequence},
               {cauldron.data(), cauldron.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "Cauldron appends style4 through the existing post-Confirm ordered grant");
    service.OnConnectionSessionWrite(bytes.data(), size);
    service.DrainEventQueue();
    Peer::UpdateSharedStates(service);
    style::Payload cauldronState{};
    Check(std::strcmp(harness.settings.ReadValue("HINT_THINKING_STYLE", ""), "cauldron_brewing") == 0 &&
              style::Encode(harness.settings, cauldronState) && cauldronState[80U] == 4U && cauldronState[81U] == 0U,
          "actual shared-session mutation and retained Settings state agree on new Cauldron value4");
    const std::array<uint8_t, 4U> placementNone{{style::VERSION, 30U, 3U, 0U}};
    Check(wire::Codec::Encode(
              {{wire::Kind::Ordered, style::MUTATION_OPCODE, Peer::OrderedToken(service), ++sequence},
               {placementNone.data(), placementNone.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "None placement uses the retained Settings epoch2/opcode24/index30 body");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    style::Payload placementSnapshot{};
    Check(std::strcmp(harness.settings.ReadValue("SETTLE_ANIM_STYLE", ""), "none") == 0 &&
              style::Encode(harness.settings, placementSnapshot) && placementSnapshot[64U] == 3U,
          "real shared-session ingress accepts and publishes None placement3");
    const std::array<uint8_t, 4U> placementRainbow{{style::VERSION, 30U, 4U, 0U}};
    Check(wire::Codec::Encode(
              {{wire::Kind::Ordered, style::MUTATION_OPCODE, Peer::OrderedToken(service), ++sequence},
               {placementRainbow.data(), placementRainbow.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "Rainbow orbit uses the same retained Settings index30/u16 body");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    Check(std::strcmp(harness.settings.ReadValue("SETTLE_ANIM_STYLE", ""), "rainbow_orbit") == 0 &&
              style::Encode(harness.settings, placementSnapshot) && placementSnapshot[64U] == 4U,
          "real typed ingress accepts and publishes Rainbow orbit ordinal4");
    const auto placementRevision = harness.settings.GetRevision();
    const std::array<uint8_t, 4U> placementInvalid{{style::VERSION, 30U, 5U, 0U}};
    Check(wire::Codec::Encode(
              {{wire::Kind::Ordered, style::MUTATION_OPCODE, Peer::OrderedToken(service), ++sequence},
               {placementInvalid.data(), placementInvalid.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
          "unknown placement5 enters the retained packet shape");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    Check(harness.settings.GetRevision() == placementRevision &&
              std::strcmp(harness.settings.ReadValue("SETTLE_ANIM_STYLE", ""), "rainbow_orbit") == 0,
          "unknown placement5 cannot replace confirmed Rainbow orbit or advance its revision");



}

void TestConnectionSessionNegotiatesAfterMtuExchange() {
    Harness harness;
    modules::BleService service;
    Check(modules::BleServiceConnectionEpochTestPeer::Configure(
              service, harness.settings, harness.controlMutation),
          "post-connect session negotiation queue begins");
    service.SetConnectionSessionBootNonce(UINT64_C(0x8899aabbccddeeff));

    // The stack reports ATT's default 23-byte MTU at connection time. The app
    // performs MTU exchange before Hello, so this early sample must not freeze
    // the session at a 20-byte packet for the entire connection.
    service.OnConnected(7U, "mtu-pending", 23U, 1U);
    service.DrainEventQueue();
    Check(modules::BleServiceConnectionEpochTestPeer::ConnectionSessionPacketMaximum(
              service) == modules::ConnectionSession::REVERSE_OBJECT_PACKET_BYTES,
          "connection session retains its negotiation ceiling until Hello");
}

void TestPlayingLitchcraftStateDoesNotPublishEveryFrame() {
    std::array<uint8_t, 10U> previous{};
    previous[0U] = 4U;
    previous[1U] = 32U;
    previous[4U] = 3U;
    previous[5U] = 15U;
    auto current = previous;
    current[0U] = 5U;
    current[2U] = 25U;
    Check(!modules::ble_internal::LitchcraftStateNeedsPublication(
              true, previous, current),
          "playing frame and elapsed anchors do not create a 20 Hz state stream");

    current[4U] = 3U | 16U;
    Check(modules::ble_internal::LitchcraftStateNeedsPublication(true, previous, current),
          "transfer hold publishes immediately while retaining explicit Play intent");
    Check(!modules::ble_internal::LitchcraftStateNeedsPublication(true, current, current, 60000U),
          "held playback does not spend bandwidth on periodic unchanged phase samples");
    current[4U] = 1U;
    Check(modules::ble_internal::LitchcraftStateNeedsPublication(
              true, previous, current),
          "pause remains an immediate semantic publication");
    previous = current;
    current[0U] = 7U;
    Check(modules::ble_internal::LitchcraftStateNeedsPublication(
              true, previous, current),
          "paused frame selection remains an immediate semantic publication");
    Check(modules::ble_internal::LitchcraftStateNeedsPublication(
              false, previous, previous),
          "a fresh connection always receives its initial Litchcraft state");
}


struct PlaybackProducerMedium {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(
        modules::litchcraft_frame_store::ARTWORK_FILE_CAPACITY, 0xffU);
    modules::litchcraft_frame_store::FrameStore::Ops Ops() {
        return {this,
            [](void* c, size_t o, uint8_t* b, size_t n) {
                auto& self = *static_cast<PlaybackProducerMedium*>(c);
                if (o + n > self.bytes.size()) return false;
                std::memcpy(b, self.bytes.data() + o, n); return true;
            },
            [](void* c, size_t o, const uint8_t* b, size_t n) {
                auto& self = *static_cast<PlaybackProducerMedium*>(c);
                if (o + n > self.bytes.size()) return false;
                std::memcpy(self.bytes.data() + o, b, n); return true;
            }, [](void*) { return true; }};
    }
};

void TestAdmittedPaintTailCannotReappearAfterPhysicalUndo() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using Art = modules::LitchcraftSession;
    namespace store = modules::litchcraft_frame_store;
    namespace wire = modules::connection_session_codec;
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStore settings;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(settings, engine, state);
    modules::BleService service;
    NimBLECharacteristic characteristic;
    Peer::ConfigureLitchcraft(service, game, characteristic);
    Peer::PrepareProducerGeneration(service);
    auto& art = game.Litchcraft();
    art.Configure(3U, 123U);
    PlaybackProducerMedium medium;
    store::FrameStore frames(medium.Ops());
    Check(frames.Recover() == store::Status::NoArtwork, "paint/X fixture starts empty");
    store::Manifest manifest{}; manifest.frameCount = 1U; manifest.active = true;
    store::TransactionToken transaction{};
    store::Frame frame{};
    Check(frames.Begin(manifest, 0U, 1U, transaction) == store::Status::Ok &&
              frames.WriteFrame(transaction, 0U, frame) == store::Status::Ok &&
              frames.Commit(transaction) == store::Status::Ok,
          "paint/X fixture seeds one real durable frame");
    modules::litchcraft_frame_stream::FrameStream stream;
    Check(stream.Configure(frames.CurrentManifest(), frames.Generation()), "paint/X configures stream");
    art.AttachFrameStream(&stream);
    for (uint8_t i = 0U; i < 12U; ++i) {
        (void)stream.Service(frames, false); (void)stream.TryPresentRequested();
    }
    art.Update();
    uint8_t enter[] = {Art::MAGIC, 1U, 0U, 0U, 0U, 0U, 1U, 0U};
    art.HandlePacket(enter, sizeof(enter), true, 0U);
    Check(Peer::ActivateMinimumConnectionObjectSession(service), "paint/X activates shared session");
    std::array<uint8_t, 20U> bytes{}; size_t size = 0U;
    std::array<uint8_t, 8U> open{};
    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, open.data());
    open[3U] = modules::ConnectionSession::ORDERED_CAPACITY;
    fxe::WriteU16LE(10U, open.data() + 4U);
    Check(wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Open),
              Peer::ConnectionToken(service), 2U}, {open.data(), open.size()}},
              {bytes.data(), bytes.size()}, size) == wire::Error::None, "paint/X encodes Open");
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    uint32_t sequence = 0U;
    auto encode = [&](const uint8_t opcode, const uint8_t* body, const size_t length) {
        Check(wire::Codec::Encode({{wire::Kind::Ordered, opcode, Peer::OrderedToken(service), ++sequence},
              {body, length}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
              "paint/X encodes real Ordered envelope");
    };
    std::array<uint8_t, 8U> guard{};
    fxe::WriteU32LE(stream.Generation(), guard.data());
    fxe::WriteU32LE(stream.LiveRevision(), guard.data() + 4U);
    encode(8U, guard.data(), guard.size());
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    const std::array<uint8_t, 10U> prefix{{0U, 1U, 0U, 1U, 15U, 52U, 152U, 219U, 0U, 1U}};
    encode(3U, prefix.data(), prefix.size());
    service.OnConnectionSessionWrite(bytes.data(), size); service.DrainEventQueue();
    const uint32_t prefixRevision = stream.LiveRevision();
    Check(prefixRevision == 3U, "paint/X accepts two prefix squares in one open stroke");
    const std::array<uint8_t, 10U> suffix{{0U, 1U, 0U, 2U, 15U, 52U, 152U, 219U, 2U, 3U}};
    encode(3U, suffix.data(), suffix.size());
    const auto admitted = bytes; const size_t admittedSize = size;
    Check(Peer::AdmitSharedOrdered(service, bytes.data(), size) == modules::ConnectionSession::Result::Ok &&
              stream.LiveRevision() == prefixRevision,
          "suffix is domain-admitted in real Ordered window but not executed");
    // App journey covers raw SetButtonDown; this uses the same normalized
    // physical event at the coordinator while the domain window is retained.
    game.ObserveLitchcraftButton({modules::UserButton::P1X,
        fxe::Button::Event::Press, modules::UiOwner::P1});
    game.ObserveLitchcraftButton({modules::UserButton::P1X,
        fxe::Button::Event::Release, modules::UiOwner::P1});
    const uint32_t undoRevision = stream.LiveRevision();
    Check(undoRevision == prefixRevision + 1U && stream.RedoCount() == 1U,
          "physical X undoes accepted prefix before admitted suffix dispatch");
    Peer::UpdateSharedStates(service);
    uint16_t rejectionReason = 0U;
    Check(stream.LiveRevision() == undoRevision &&
              Peer::TakeOrderedRejection(service, sequence, &rejectionReason) &&
              rejectionReason == static_cast<uint16_t>(Art::Status::RevisionConflict),
          "old Guard rejects admitted suffix with exact RevisionConflict outcome");
    service.OnConnectionSessionWrite(admitted.data(), admittedSize); service.DrainEventQueue();
    fxe::ManualMillis::AdvanceMs(6000U);
    for (uint8_t i = 0U; i < 12U; ++i) (void)stream.Service(frames, true);
    Check(frames.ReadFrame(0U, frame) == store::Status::Ok &&
              std::all_of(frame.emitters.begin(), frame.emitters.end(), [](const auto& e) {
                  return e.color.r == 0U && e.color.g == 0U && e.color.b == 0U;
              }) && stream.LiveRevision() == undoRevision,
          "duplicate ingress and background save cannot resurrect any undone prefix or suffix");
    std::printf("[PAINT_X_ADMITTED_TAIL] prefix=%lu undo=%lu admitted=1 rejected=1 persisted_all_emitters=black\n",
                static_cast<unsigned long>(prefixRevision), static_cast<unsigned long>(undoRevision));
}

void TestPlayingLitchcraftProducerPublishesMinutePhaseAndRetriesAdmission() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using Art = modules::LitchcraftSession;
    namespace store = modules::litchcraft_frame_store;
    const int before = g_failures;
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStore settings;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(settings, engine, state);
    modules::BleService service;
    NimBLECharacteristic characteristic;
    Peer::ConfigureLitchcraft(service, game, characteristic);
    Peer::PrepareProducerGeneration(service);
    auto& art = game.Litchcraft();
    art.Configure(3U, 123U);
    PlaybackProducerMedium medium;
    store::FrameStore frames(medium.Ops());
    Check(frames.Recover() == store::Status::NoArtwork, "producer fixture starts empty");
    store::Manifest manifest{}; manifest.frameCount = 2U; manifest.active = true;
    store::TransactionToken token{};
    Check(frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok, "producer begins real Store");
    store::Frame frame{}; frame.durationMs = 501U;
    Check(frames.WriteFrame(token, 0U, frame) == store::Status::Ok, "producer frame zero writes");
    frame.durationMs = 701U;
    Check(frames.WriteFrame(token, 1U, frame) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok, "producer second frame commits");
    modules::litchcraft_frame_stream::FrameStream stream;
    Check(stream.Configure(frames.CurrentManifest(), frames.Generation()), "producer configures real stream");
    art.AttachFrameStream(&stream);
    for (uint8_t i = 0U; i < 12U; ++i) {
        (void)stream.Service(frames, false); (void)stream.TryPresentRequested();
    }
    // The normal runtime samples the attached boot root before app Enter.
    // Establish its document epoch through the real playback owner first.
    art.Update();
    uint8_t enter[] = {Art::MAGIC, 1U, 0U, 0U, 0U, 0U, 1U, 0U};
    art.HandlePacket(enter, sizeof(enter), true, 0U);
    const auto drainArtPackets = [&]() {
        Art::Packet packet{};
        while (art.PeekPacket(packet)) art.AcceptPacket();
    };
    drainArtPackets();
    uint16_t request = 2U;
    auto runtime = [&](const uint8_t action) {
        std::array<uint8_t, 16U> command{};
        command[0U] = 0x4dU; command[1U] = 2U; command[2U] = 23U;
        fxe::WriteU32LE(art.SessionId(), command.data() + 3U);
        fxe::WriteU16LE(request++, command.data() + 7U);
        fxe::WriteU32LE(stream.Generation(), command.data() + 9U);
        command[13U] = action; fxe::WriteU16LE(art.SelectedFrame(), command.data() + 14U);
        art.HandlePacket(command.data(), command.size(), true, 0U);
        drainArtPackets();
    };
    runtime(1U); art.Update();
    Check(art.IsActive() && art.IsAnimationPlaying(), "producer real playback enters playing mode");
    // Exhaust the actual retained State owner before first State1 publication.
    Peer::FillStatePublicationCapacity(service);
    Peer::UpdateSharedStates(service);
    Check(!Peer::LitchcraftPublicationValid(service) && Peer::LitchcraftPublicationRevision(service) == 0U,
          "failed State1 admission cannot advance producer cache or revision");
    Check(Peer::ActivateMinimumConnectionObjectSession(service), "producer opens real State owner after capacity recovery");
    Peer::UpdateSharedStates(service);
    std::array<uint8_t, 10U> body{}; uint32_t publication = 0U;
    Check(Peer::TakeLitchcraftState(service, body, publication) && publication == 1U &&
              (body[4U] & 3U) == 3U, "failed State1 admission retries exact initial current truth");
    const uint32_t firstRevision = Peer::LitchcraftPublicationRevision(service);
    auto tick = [&](const uint32_t now) {
        fxe::ManualMillis::SetNowMs(now);
        art.Update();
        (void)stream.Service(frames, false);
        art.Update();
        Peer::UpdateSharedStates(service);
    };
    bool sawFrameOne = false;
    for (uint32_t elapsed = 20U; elapsed < 60000U; elapsed += 20U) {
        tick(1000U + elapsed);
        sawFrameOne = sawFrameOne || art.SelectedFrame() == 1U;
    }
    Check(sawFrameOne && Peer::LitchcraftPublicationRevision(service) == firstRevision,
          "healthy real frame/elapsed ticks coalesce below one minute");
    tick(60999U);
    Check(Peer::LitchcraftPublicationRevision(service) == firstRevision,
          "minute producer remains quiet at 59999ms");
    tick(61000U);
    Check(Peer::TakeLitchcraftState(service, body, publication) && publication == firstRevision + 1U &&
              body[0U] == art.SelectedFrame() &&
              fxe::ReadU16LE(body.data() + 2U) == art.AnimationFrameElapsedMs(),
          "at 60000ms real producer publishes fresh selected frame and elapsed phase");
    std::printf("[PLAYBACK_PRODUCER] t=%u revision=%u frame=%u elapsed=%u playing=%u\n",
        fxe::Millis(), publication, body[0U], fxe::ReadU16LE(body.data() + 2U), body[4U] & 2U);
    art.SetBulkTransferActive(true); Peer::UpdateSharedStates(service);
    Check(Peer::TakeLitchcraftState(service, body, publication) &&
              (body[4U] & 18U) == 18U,
          "actual shared State projects transfer suspension separately from explicit Play intent");
    const auto heldPhase = fxe::ReadU16LE(body.data() + 2U);
    const auto heldPublication = publication;
    tick(66000U);
    Check(art.AnimationFrameElapsedMs() == heldPhase &&
              Peer::LitchcraftPublicationRevision(service) == heldPublication,
          "held board phase stays truthful without a per-frame State stream");
    art.SetBulkTransferActive(false); Peer::UpdateSharedStates(service);
    Check(Peer::TakeLitchcraftState(service, body, publication) &&
              (body[4U] & 18U) == 2U && fxe::ReadU16LE(body.data() + 2U) == heldPhase,
          "hold retirement immediately republishes the preserved phase and Play intent");
    runtime(0U); Peer::UpdateSharedStates(service);
    Check(Peer::TakeLitchcraftState(service, body, publication) && publication == firstRevision + 4U &&
              (body[4U] & 2U) == 0U, "pause publishes immediate semantic truth despite fresh minute sample");
    const auto pausedRevision = Peer::LitchcraftPublicationRevision(service);
    tick(121000U);
    Check(Peer::LitchcraftPublicationRevision(service) == pausedRevision,
          "paused playback does not publish periodic phase traffic");
    runtime(1U); Peer::UpdateSharedStates(service);
    Check(Peer::TakeLitchcraftState(service, body, publication) && publication == pausedRevision + 1U,
          "resume publishes immediate semantic edge");
    const auto resumedRevision = Peer::LitchcraftPublicationRevision(service);
    tick(180999U);
    Check(Peer::LitchcraftPublicationRevision(service) == resumedRevision,
          "successful semantic publication resets the minute cadence");
    tick(181000U);
    Check(Peer::TakeLitchcraftState(service, body, publication) && publication == resumedRevision + 1U,
          "playing periodic sample resumes one minute after semantic edge");
    // The same fxMillis owner and unsigned elapsed predicate must cross wrap.
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 30000U);
    runtime(0U); Peer::UpdateSharedStates(service); runtime(1U); Peer::UpdateSharedStates(service);
    const auto wrapRevision = Peer::LitchcraftPublicationRevision(service);
    tick(29998U);
    Check(Peer::LitchcraftPublicationRevision(service) == wrapRevision,
          "wrap-aware minute predicate stays quiet below boundary");
    tick(29999U);
    Check(Peer::LitchcraftPublicationRevision(service) == wrapRevision + 1U,
          "wrap-aware minute predicate publishes exact 60000ms boundary");
    Peer::SetPublicationRevision(service, UINT32_MAX);
    runtime(0U); Peer::UpdateSharedStates(service);
    Check(Peer::LitchcraftPublicationRevision(service) == 1U,
          "publication revision rollover skips reserved zero");
    const uint16_t pausedElapsed = art.AnimationFrameElapsedMs();
    const uint8_t pausedFrame = art.SelectedFrame();
    Check(pausedElapsed != 0U, "same-frame seek starts from a nonzero paused anchor");
    runtime(2U); Peer::UpdateSharedStates(service);
    Check(Peer::TakeLitchcraftState(service, body, publication) &&
              publication == 2U && body[0U] == pausedFrame &&
              fxe::ReadU16LE(body.data() + 2U) == 0U,
          "same-frame paused seek publishes its reset anchor immediately");
    const auto durationEdit = [&](const uint16_t duration) {
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(stream.Generation(), guard.data());
        fxe::WriteU32LE(stream.LiveRevision(), guard.data() + 4U);
        Check(art.ApplyConnectionOrdered(8U, guard.data(), guard.size(), 7U) == Art::Status::Ok,
              "producer duration edit acquires existing live revision guard");
        std::array<uint8_t, 5U> edit{3U, art.SelectedFrame(), 0U, 0U, 0U};
        fxe::WriteU16LE(duration, edit.data() + 3U);
        Check(art.ApplyConnectionOrdered(10U, edit.data(), edit.size(), 7U) == Art::Status::Ok,
              "producer duration edit uses real guarded Store owner");
        Peer::UpdateSharedStates(service);
    };
    durationEdit(900U);
    Check(Peer::TakeLitchcraftState(service, body, publication),
          "first duration edit publishes real history and live identity");
    const auto beforeIdentityBody = body;
    const uint32_t beforeIdentityRevision = publication;
    durationEdit(950U);
    Check(Peer::TakeLitchcraftState(service, body, publication) &&
              publication == beforeIdentityRevision + 1U && body == beforeIdentityBody,
          "live-R-only duration semantic publishes even when all ten body bytes are equal");
    std::printf("[PLAYBACK_PRODUCER] identity_revision=%u live_R=%u paused_seek_elapsed=%u equal_body=%u\n",
        publication, stream.LiveRevision(), pausedElapsed, body == beforeIdentityBody);
    uint8_t exit[] = {Art::MAGIC, uint8_t(Art::Op::Exit), 0U, 0U, 0U, 0U, 3U, 0U};
    fxe::WriteU32LE(art.SessionId(), exit + 2U);
    fxe::WriteU16LE(request++, exit + 6U);
    art.HandlePacket(exit, sizeof(exit), true, 0U);
    drainArtPackets();
    Peer::UpdateSharedStates(service);
    Check((Peer::LitchcraftPublicationBody(service)[4U] & 1U) == 0U,
          "Exit immediately cancels active playback publication semantics");
    const auto exitRevision = Peer::LitchcraftPublicationRevision(service);
    const auto exitBody = Peer::LitchcraftPublicationBody(service);
    tick(150000U);
    const auto afterExitBody = Peer::LitchcraftPublicationBody(service);
    for (size_t index = 0U; index < exitBody.size(); ++index) {
        if (exitBody[index] != afterExitBody[index]) {
            std::printf("[PLAYBACK_PRODUCER] exit_body_delta index=%zu before=%u after=%u\n",
                index, exitBody[index], afterExitBody[index]);
        }
    }
    std::printf("[PLAYBACK_PRODUCER] exit_before_R=%u exit_after_R=%u elapsed_before=%u elapsed_after=%u flags_before=%u flags_after=%u source_R=%u\n",
        exitRevision, Peer::LitchcraftPublicationRevision(service),
        fxe::ReadU16LE(exitBody.data() + 2U), fxe::ReadU16LE(afterExitBody.data() + 2U),
        exitBody[4U], afterExitBody[4U], art.ConnectionRevision());
    Check(Peer::LitchcraftPublicationRevision(service) == exitRevision,
          "inactive owner has no minute phase producer");
    service.OnConnected(7U, "producer-reconnect", 23U, 1U); service.DrainEventQueue();
    Check(Peer::LitchcraftPublicationRevision(service) <= 1U,
          "real reconnect lifecycle invalidates old State1 revision and cadence");
    Check(Peer::ActivateMinimumConnectionObjectSession(service, Peer::ProducerEpoch(service)),
          "reconnect opens the actual fresh State owner");
    Peer::UpdateSharedStates(service);
    Check(Peer::TakeLitchcraftState(service, body, publication) && publication == 1U &&
              (body[4U] & 3U) == 0U,
          "reconnect publishes initial current truth with a fresh revision");
    fxe::WriteU16LE(request++, enter + 6U);
    art.HandlePacket(enter, sizeof(enter), true, 0U);
    drainArtPackets();
    runtime(1U); art.Update(); Peer::UpdateSharedStates(service);
    Check(Peer::TakeLitchcraftState(service, body, publication) && (body[4U] & 3U) == 3U,
          "reconnected mode enters and plays through the same producer");
    const uint32_t reconnectedRevision = publication;
    tick(209999U);
    Check(Peer::LitchcraftPublicationRevision(service) == reconnectedRevision,
          "reconnect cadence remains quiet below the fresh minute boundary");
    tick(210000U);
    Check(Peer::TakeLitchcraftState(service, body, publication) && publication == reconnectedRevision + 1U,
          "reconnected playing owner samples at its fresh minute boundary");
    std::printf("[PLAYBACK_PRODUCER] failures=%d capacity_retry=1 minute=1 semantic=1 wrap=1 exit=1 reconnect=1\n",
                g_failures - before);
}

void TestComplianceUsesRealSharedStateAdapter() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    Harness harness; modules::BleService service; NimBLECharacteristic characteristic;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "diagnostic fixture owns real ingress queue");
    Peer::PrepareProducerGeneration(service); Peer::MarkConnected(service, true);
    Peer::SubscribeDiagnosticState(service);
    Peer::ConfigureConnectionSessionNotifications(service, characteristic);
    Check(Peer::ActivateMinimumConnectionObjectSession(service), "diagnostic load activates the real minimum-MTU State session");
    struct PhysicalCapture { NimBLECharacteristic* characteristic; uint16_t buttons=UINT16_MAX; unsigned states=0U; } capture{&characteristic};
    characteristic.SetNotifyHook([](void* context){
        auto& captured=*static_cast<PhysicalCapture*>(context);
        namespace wire=modules::connection_session_codec;
        wire::PacketView packet;
        const auto& bytes=captured.characteristic->LastTx();
        if(wire::Codec::Decode({reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()},packet,20U)==wire::Error::None &&
           packet.header.kind==wire::Kind::State && packet.header.code==modules::physical_input_state_wire::STATE_CODE && packet.body.Size()==10U) {
            captured.buttons=fxe::ReadU16LE(packet.body.Data()+8U); ++captured.states;
        }
    },&capture);
    service.PublishConnectionPhysicalInputState(UINT64_C(0x0000000000000001));
    const uint32_t revision = Peer::PhysicalInputRevision(service);
    Check(service.RefreshDiagnosticPhysicalInputState() && Peer::PhysicalInputRevision(service) == revision + 1U,
          "diagnostic refresh uses the retained physical state with a new production revision");
    characteristic.SetNotifyResult(false);
    Peer::UpdateSharedStates(service);
    const auto failedNotify = service.GetSessionDiagnostics();
    Check(failedNotify.notifyAttempts == 1U && failedNotify.notifyFailures == 1U &&
              failedNotify.sentStateCodes == 0U,
          "native notify rejection records failure without claiming State delivery");
    characteristic.SetNotifyResult(true);
    Peer::UpdateSharedStates(service);
    Check((service.GetSessionDiagnostics().sentStateCodes &
           (UINT64_C(1) << modules::physical_input_state_wire::STATE_CODE)) != 0U &&
              capture.states > 0U && service.GetSessionDiagnostics().notifyFailures == 1U,
          "retained State recovers through production notify and records native acceptance");
    Check(service.SessionNotifyCount() > 0U && service.SessionNotifyCount() == characteristic.NotifyCount(),
          "diagnostic packet is accepted by the actual shared-session egress owner at minimum MTU");
    service.PublishConnectionButtonState({modules::UserButton::Up,fxe::Button::Event::Press,modules::UiOwner::P1},0U);
    Peer::UpdateSharedStates(service);
    Check(capture.buttons==1U,"actual typed physical State carries the held direction before diagnostic exit");
    service.ResetConnectionButtonState(); Peer::UpdateSharedStates(service);
    Check(capture.buttons==0U,"exit reset publishes released typed State even when GPIO suppresses the later release");
    service.PublishConnectionButtonState({modules::UserButton::Up,fxe::Button::Event::Press,modules::UiOwner::P1},1U);
    Peer::UpdateSharedStates(service);
    Check(capture.buttons==1U,"fresh post-exit press resumes physical State publication");
    service.PublishConnectionButtonState({modules::UserButton::Up,fxe::Button::Event::Release,modules::UiOwner::P1},2U);
    Peer::UpdateSharedStates(service);
    Check(capture.buttons==0U && capture.states>=4U,"fresh post-exit release returns the app's typed State to clear");
    const uint8_t malformed = 0U; service.OnConnectionSessionWrite(&malformed, 1U);
    Check(service.SessionIngressCount() == 1U, "raw ingress counter records traffic without claiming valid peer proof");
    Peer::MarkConnected(service, false);
    const auto before = Peer::PhysicalInputRevision(service);
    Check(!service.RefreshDiagnosticPhysicalInputState() && Peer::PhysicalInputRevision(service) == before,
          "disconnected diagnostic refresh cannot mutate retained revision");
    service.SetDiagnosticRadioOverride(true, false); service.Update(harness.settings);
    Check(!Peer::DiagnosticEnabled(service), "volatile BLE off overrides the ordinary enabled setting");
    service.SetDiagnosticRadioOverride(false, false); service.Update(harness.settings);
    Check(Peer::DiagnosticEnabled(service), "clearing override restores the unchanged runtime BLE setting");
}

void TestConnectionSessionIngressDrainImmediatelyNotifiesResponse() {
    namespace wire = modules::connection_session_codec;
    Harness harness;
    modules::BleService service;
    NimBLECharacteristic characteristic;
    Check(modules::BleServiceConnectionEpochTestPeer::Configure(
              service, harness.settings, harness.controlMutation),
          "immediate session response fixture initializes queues");
    modules::BleServiceConnectionEpochTestPeer::
        ConfigureConnectionSessionNotifications(service, characteristic);
    service.SetConnectionSessionBootNonce(UINT64_C(0x8899aabbccddeeff));
    service.OnConnected(7U, "immediate-response", 23U, 1U);
    service.OnConnectionSessionSubscription(1U);
    service.DrainEventQueue();

    std::array<uint8_t, 10U> helloBody{};
    fxe::WriteU64LE(UINT64_C(0x1122334455667788), helloBody.data());
    fxe::WriteU16LE(20U, helloBody.data() + 8U);
    std::array<uint8_t, 20U> hello{};
    size_t helloSize = 0U;
    Check(wire::Codec::Encode(
              {{wire::Kind::Control, uint8_t(wire::Control::Hello), 0U, 0U},
               {helloBody.data(), helloBody.size()}},
              {hello.data(), hello.size()}, helloSize) == wire::Error::None,
          "immediate session response fixture encodes Hello");
    service.OnConnectionSessionWrite(hello.data(), helloSize);
    Check(characteristic.NotifyCount() == 0U,
          "connection callback only queues ingress before the runtime drain");

    service.DrainEventQueue();
    wire::PacketView response;
    Check(characteristic.NotifyCount() == 2U &&
              wire::Codec::Decode(
                  {reinterpret_cast<const uint8_t*>(
                       characteristic.LastTx().data()),
                   characteristic.LastTx().size()},
                  response, 20U) == wire::Error::None &&
              response.header.code == uint8_t(wire::Control::Identity),
          "one ingress drain emits Welcome and Identity without waiting for the 20 ms BLE cadence");
}

struct NotifyTestProbe {
    modules::BleService service;
    NimBLECharacteristic characteristic;
    const uint16_t previousMtu = NimBLEDevice::getMTU();
    uint32_t notified = 0U;
    uint32_t blockingCallbacks = 0U;
    uint32_t millisecondsPerNotify = 0U;
    uint32_t millisecondsBeforeWork = 0U;
    uint32_t pauseCalls = 0U;
    uint32_t largestPauseMs = 0U;
    uint32_t disconnectDuringPauseAt = 0U;
    bool disconnectBeforeWork = false;
    inline static NotifyTestProbe* active = nullptr;
    uint32_t disconnectAt = 0U;
    bool replaceConnection = false;
    std::vector<std::string>* capturedPackets = nullptr;

    NotifyTestProbe() {
        NimBLEDevice::SetTestMtu(23U);
        active = this;
        modules::BleServiceConnectionEpochTestPeer::ConfigureNotifyTest(
            service, characteristic);
        characteristic.SetNotifyHook([](void* context) {
            auto& self = *static_cast<NotifyTestProbe*>(context);
            ++self.notified;
            if (self.capturedPackets != nullptr) {
                self.capturedPackets->push_back(self.characteristic.LastTx());
            }
            fxe::ManualMillis::AdvanceMs(self.millisecondsPerNotify);
            if (self.disconnectAt != 0U &&
                self.notified == self.disconnectAt) {
                self.service.OnDisconnected();
                if (self.replaceConnection) {
                    modules::BleServiceConnectionEpochTestPeer::ConfigureNotifyTest(
                        self.service, self.characteristic);
                }
            }
        }, this);
        service.SetBeforeBlockingDiagnosticCallback([](void* context) {
            auto& self = *static_cast<NotifyTestProbe*>(context);
            ++self.blockingCallbacks;
            fxe::ManualMillis::AdvanceMs(self.millisecondsBeforeWork);
            if (self.disconnectBeforeWork) {
                self.service.OnDisconnected();
            }
        }, this);
    }

    ~NotifyTestProbe() {
        NimBLEDevice::SetTestMtu(previousMtu);
        characteristic.SetNotifyHook(nullptr);
        active = nullptr;
    }

    void Pause(const uint32_t milliseconds) {
        ++pauseCalls;
        if (milliseconds > largestPauseMs) {
            largestPauseMs = milliseconds;
        }
        fxe::ManualMillis::AdvanceMs(milliseconds);
        if (disconnectDuringPauseAt != 0U &&
            pauseCalls == disconnectDuringPauseAt) {
            service.OnDisconnected();
        }
    }
};

void TestBulkInvalidationDoesNotDependOnSettingsSubscription() {
    Harness harness;
    (void)harness.bleSettings.UpdateChangeNotifications(harness.settings, true);
    harness.bleSettings.OnReadWriteSubscribe(0U);
    harness.ReadWriteCharacteristic().ClearTx();
    harness.settings.Doc()["BRIGHTNESS"] = 3U;
    harness.settings.NotifyDocChanged();
    Check(harness.bleSettings.UpdateChangeNotifications(harness.settings, true),
          "visible change invalidates console bootstrap without settings subscription");
    Check(harness.ReadWriteCharacteristic().LastTx().empty(),
          "console-only invalidation does not send an unsubscribed legacy delta");
    Check(!harness.bleSettings.UpdateChangeNotifications(harness.settings, true),
          "unchanged visible state does not schedule repeated bulk records");
    harness.settings.Doc()["GAME_UNDO_HISTORY"] = "changed durable history";
    harness.settings.NotifyDocChanged();
    Check(!harness.bleSettings.UpdateChangeNotifications(harness.settings, true),
          "persistence-only revision does not retransmit all settings");
    harness.bleSettings.OnReadWriteSubscribe(1U);
    Check(!harness.bleSettings.UpdateChangeNotifications(harness.settings, true),
          "legacy resubscription does not need another bulk snapshot");
    Check(harness.ReadWriteCharacteristic().LastTx() == "[0,3]",
          "legacy resubscription delivers the change observed while unsubscribed");
}

void TestSettingsResponsesUseNegotiatedPeerMtu() {
    const uint16_t previous = NimBLEDevice::getMTU();
    NimBLEDevice::SetTestMtu(517U);
    NimBLECharacteristic response;
    const std::string full = "{\"i\":42,\"k\":\"GAME_FIRST_PLAYER\",\"v\":2,\"ok\":true}";
    const std::string compact = "{\"y\":[42,1,0]}";
    for (const uint16_t peerMtu : {23U, 100U, 247U}) {
        NimBLEDevice::getServer()->SetPeerMtu(7U, peerMtu);
        const size_t limit = peerMtu - 3U;
        response.SetNotifyPayloadLimit(limit);
        Check(modules::ble_settings_async::GetNotificationPayloadLimit() == limit,
              "settings response payload follows negotiated peer MTU");
        (void)modules::ble_settings_async::SendAsyncResponse(
            response, 1U, full.data(), full.size(), true,
            compact.data(), compact.size());
        Check(response.LastTx() == (full.size() <= limit ? full : compact),
              "minimum MTU receives complete correlated fallback; larger link receives complete value");
    }
    // A higher-MTU peer survives in slot zero; another disconnect leaves a
    // hole before the minimum-MTU peer. The pinned SDK's indexed lookup can
    // alias that hole to handle zero and choose an oversized full response.
    NimBLEDevice::getServer()->SetPeerSlots({{0U, 247U}, {0xffffU, 0U}, {2U, 23U}});
    response.SetNotifyPayloadLimit(20U);
    Check(modules::ble_settings_async::GetNotificationPayloadLimit() == 20U,
          "sparse mixed-peer slots preserve the smallest negotiated payload");
    (void)modules::ble_settings_async::SendAsyncResponse(
        response, 1U, full.data(), full.size(), true,
        compact.data(), compact.size());
    Check(response.LastTx() == compact,
          "sparse minimum-MTU peer receives the entire correlated fallback");
    NimBLEDevice::getServer()->SetPeerSlots({});
    Check(modules::ble_settings_async::GetNotificationPayloadLimit() == 20U,
          "no connected peer has a conservative notification payload");
    NimBLEDevice::SetTestMtu(previous);
}

void TestConsoleOutputUsesPeerMtuAndRetainsRejectedPacket() {
    fxe::ManualMillis::SetNowMs(1000U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    const uint16_t previousMtu = NimBLEDevice::getMTU();
    // The local preference remains 517 while a phone starts at ATT MTU 23.
    NimBLEDevice::SetTestMtu(517U);
    const std::string prefix = "{\"event\":\"home_state\",\"fen\":\"";
    const std::string bytes = prefix + std::string(280U, 'x') + "\"}\r\n";
    for (const uint16_t upgradedMtu : {23U, 100U, 247U, 517U}) {
        modules::BleService service;
        NimBLEServer server;
        NimBLECharacteristic characteristic;
        server.SetPeerMtu(7U, 23U);
        characteristic.SetNotifyPayloadLimit(20U);
        modules::BleServiceConnectionEpochTestPeer::ConfigureConsoleOutput(
            service, server, characteristic, bytes);
        Check(service.GetMtu() == 23U,
              "console reports the peer MTU rather than the local preference");
        const uint32_t clientActivityBefore =
            modules::BleServiceConnectionEpochTestPeer::LastClientActivityMs(
                service);
        std::string received;
        size_t remaining = modules::BleServiceConnectionEpochTestPeer::FlushConsoleOutput(service);
        received += characteristic.LastTx();
        Check(characteristic.LastTx().size() == 20U && remaining == bytes.size() - 20U,
              "first minimum-MTU packet preserves every unsent Home byte");
        Check(modules::BleServiceConnectionEpochTestPeer::LastClientActivityMs(
                  service) == clientActivityBefore,
              "outbound console notification cannot renew an idle client lease");
        Check(characteristic.LastConnHandle() == 7U,
              "console submits owned bytes to the exact active connection");
        const uint32_t firstCount = characteristic.NotifyCount();
        (void)modules::BleServiceConnectionEpochTestPeer::FlushConsoleOutput(service);
        Check(characteristic.NotifyCount() == firstCount,
              "one frame cannot spin-drain the console queue");
        server.SetPeerMtu(7U, upgradedMtu);
        characteristic.SetNotifyPayloadLimit(upgradedMtu - 3U);
        Check(service.GetMtu() == upgradedMtu,
              "negotiated peer upgrade is observed within the same connection");
        fxe::ManualMillis::AdvanceMs(5U);
        characteristic.SetNotifyResult(false);
        const size_t rejectedRemaining = modules::BleServiceConnectionEpochTestPeer::FlushConsoleOutput(service);
        Check(rejectedRemaining == remaining,
              "failed SDK enqueue preserves the complete unsent tail");
        characteristic.SetNotifyResult(true);
        for (unsigned attempt = 0; remaining != 0U && attempt < 40U; ++attempt) {
            const uint32_t before = characteristic.NotifyCount();
            remaining = modules::BleServiceConnectionEpochTestPeer::FlushConsoleOutput(service);
            if (characteristic.NotifyCount() > before) {
                const size_t limit = upgradedMtu - 3U < 244U ? upgradedMtu - 3U : 244U;
                Check(characteristic.LastTx().size() <= limit,
                      "every upgraded console packet respects peer and bounded drain limits");
                received += characteristic.LastTx();
            }
            fxe::ManualMillis::AdvanceMs(5U);
        }
        Check(received == bytes && remaining == 0U,
              "Home bytes and delimiter reconstruct exactly across MTU change and retry");
        if (upgradedMtu >= 247U) {
            Check(characteristic.NotifyCount() == 3U,
                  "larger peer payload drains this Home record in three packets including initial20");
        }
        std::printf("[CONSOLE_MTU] preferred=517 peer=%u packets=%lu bytes=%zu complete=%u\n",
                    upgradedMtu, static_cast<unsigned long>(characteristic.NotifyCount()),
                    received.size(), received == bytes);
    }
    NimBLEDevice::SetTestMtu(previousMtu);
}

void TestConsoleOutputOverflowIsNotReportedAsIngressLoss() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    fxe::ManualMillis::SetNowMs(1000U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    modules::BleService service;
    NimBLEServer server;
    NimBLECharacteristic characteristic;
    server.SetPeerMtu(7U, 23U);
    characteristic.SetNotifyResult(false);
    const std::string full(4096U, 'x');
    Peer::ConfigureConsoleOutput(service, server, characteristic, full);
    Peer::WriteConsoleOutput(service, "overflow one");
    Peer::WriteConsoleOutput(service, "overflow two");
    Check(Peer::IngressDrops(service) == 0U,
          "actual console buffer overflow cannot claim a lost ingress event");
    Check(Peer::ConsoleOutputOverflows(service) == 2U &&
              Peer::ConsoleOutputSize(service) == full.size(),
          "actual console overflow counts refused writes and preserves the unsent tail");
    const std::string oversized(192U, 'x');
    service.OnCommandCharacteristicWrite(oversized.data(), oversized.size());
    Check(Peer::IngressDrops(service) == 1U &&
              Peer::ConsoleOutputOverflows(service) == 2U,
          "oversized actual ingress is reported separately from console overflow");
}

void TestConsoleOutputRejectsUnsubscribedAndStaleConnection() {
    fxe::ManualMillis::SetNowMs(1000U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    modules::BleService service;
    NimBLEServer server;
    NimBLECharacteristic characteristic;
    const std::string bytes = "queued console tail\r\n";
    server.SetPeerMtu(7U, 23U);
    modules::BleServiceConnectionEpochTestPeer::ConfigureConsoleOutput(
        service, server, characteristic, bytes);
    service.OnConsoleStreamSubscriptionChanged(0U);
    modules::BleServiceConnectionEpochTestPeer::WriteConsoleOutput(
        service, "new unsubscribed log\r\n");
    Check(modules::BleServiceConnectionEpochTestPeer::FlushConsoleOutput(service) == bytes.size() &&
              characteristic.NotifyCount() == 0U &&
              modules::BleServiceConnectionEpochTestPeer::ConsoleOutputSize(service) ==
                  bytes.size(),
          "unsubscribe preserves the queued tail without accumulating new logs");
    service.OnDisconnected();
    service.OnConnected(7U, "replacement", 23U, 1U);
    service.OnConsoleStreamSubscriptionChanged(1U);
    // Callback generation has changed, but the main loop has not processed
    // either queued connection event or cleared the prior session's buffer.
    Check(modules::BleServiceConnectionEpochTestPeer::FlushConsoleOutput(service) == bytes.size() &&
              characteristic.NotifyCount() == 0U,
          "reused handle cannot send old bytes before connection events are drained");
}

void TestNotifyDiagnosticAdmissionAndRecovery() {
    fxe::ManualMillis::SetNowMs(0U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    {
        NotifyTestProbe probe;
        modules::BleLinkTestResult result;
        Check(!probe.service.RunConsoleNotifyTest(20U, UINT16_MAX,
                                                 UINT16_MAX, result),
              "oversized notify diagnostic is rejected before blocking work");
        Check(probe.notified == 0U && probe.blockingCallbacks == 0U,
              "rejected notify diagnostic does not blank or write to the board");
        Check(probe.service.RunConsoleNotifyTest(20U, 200U, 0U, result) &&
                  result.okCount == 200U && result.bytesSent == 4000U,
              "ordinary host benchmark still works after a rejected request");
        Check(probe.characteristic.LastConnHandle() == 7U &&
                  probe.characteristic.LastTx() == std::string(20U, '\x01'),
              "diagnostic submits each exact payload to the original native peer");
        probe.characteristic.SetNotifyResult(false);
        Check(!probe.service.RunConsoleNotifyTest(20U, 3U, 0U, result) &&
                  result.okCount == 0U && result.failCount == 3U &&
                  result.bytesAttempted == 60U && result.bytesSent == 0U,
              "refused native admission cannot become successful diagnostic bytes");
        probe.characteristic.SetNotifyResult(true);
        Check(probe.service.RunConsoleNotifyTest(20U, 2U, 0U, result) &&
                  result.okCount == 2U && result.failCount == 0U,
              "a fresh diagnostic recovers after native queue refusal");
    }
    for (const bool replace : {false, true}) {
        NotifyTestProbe probe;
        probe.disconnectAt = 2U;
        probe.replaceConnection = replace;
        modules::BleLinkTestResult result;
        Check(!probe.service.RunConsoleNotifyTest(20U, 20U, 0U, result),
              "notify diagnostic reports an interrupted connection as failure");
        Check(probe.notified == 2U && result.okCount == 2U,
              "notify diagnostic sends no packet into a disconnected or replacement session");
        probe.disconnectAt = 0U;
        modules::BleServiceConnectionEpochTestPeer::ConfigureNotifyTest(
            probe.service, probe.characteristic);
        Check(probe.service.RunConsoleNotifyTest(20U, 3U, 0U, result) &&
                  result.okCount == 3U,
              "a fresh explicit diagnostic can run after connection recovery");
    }
    {
        NotifyTestProbe probe;
        probe.millisecondsPerNotify = 10001U;
        modules::BleLinkTestResult result;
        Check(!probe.service.RunConsoleNotifyTest(20U, 20U, 0U, result),
              "actual elapsed time bounds a diagnostic whose SDK operation was slow");
        Check(probe.notified == 1U && result.elapsedMs == 10001U,
              "an elapsed diagnostic preserves partial accounting without another notify");
    }
    {
        NotifyTestProbe probe;
        modules::BleLinkTestResult result;
        for (const uint16_t delayMs : {0U, 100U}) {
            const uint16_t atLimit = delayMs == 0U ? 10000U : 100U;
            Check(probe.service.RunConsoleNotifyTest(20U, atLimit, delayMs, result),
                  "the deliberate-pause budget accepts its exact boundary");
            Check(!probe.service.RunConsoleNotifyTest(20U, atLimit + 1U, delayMs, result),
                  "the deliberate-pause budget rejects the next packet");
        }
        Check(!probe.service.RunConsoleNotifyTest(0U, 1U, 0U, result) &&
                  !probe.service.RunConsoleNotifyTest(20U, 0U, 0U, result),
              "empty notification requests remain invalid");
    }
    for (const bool disconnect : {false, true}) {
        NotifyTestProbe probe;
        probe.millisecondsBeforeWork = disconnect ? 0U : 10000U;
        probe.disconnectBeforeWork = disconnect;
        modules::BleLinkTestResult result;
        Check(!probe.service.RunConsoleNotifyTest(20U, 20U, 0U, result) &&
                  probe.notified == 0U,
              "pre-diagnostic safety work cannot consume the deadline or connection then send");
    }
    {
        NotifyTestProbe probe;
        probe.disconnectDuringPauseAt = 1U;
        modules::BleLinkTestResult result;
        Check(!probe.service.RunConsoleNotifyTest(20U, 2U, 1000U, result) &&
                  probe.notified == 1U && probe.pauseCalls == 1U &&
                  probe.largestPauseMs <= 10U && result.elapsedMs <= 10U,
              "disconnect during a deliberate wait returns within one bounded slice");
    }
    {
        fxe::ManualMillis::SetNowMs(UINT32_MAX - 10U);
        NotifyTestProbe probe;
        modules::BleLinkTestResult result;
        Check(probe.service.RunConsoleNotifyTest(100U, 20U, 0U, result) &&
                  result.elapsedMs == 20U && result.packetSize == 20U &&
                  result.bytesAttempted == 400U && result.bytesSent == 400U,
              "ordinary diagnostic survives millis wrap and clamps to the current MTU");
    }
}

void TestTaggedNotifyDiagnosticPayloads() {
    fxe::ManualMillis::SetNowMs(0U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    {
        std::vector<std::string> packets;
        NotifyTestProbe probe;
        probe.capturedPackets = &packets;
        modules::BleLinkTestResult result;
        constexpr uint32_t RUN = 0xfedcba98U;
        Check(probe.service.RunConsoleNotifyTest(100U, 300U, 0U, result, RUN) &&
                  result.packetSize == 20U && packets.size() == 300U,
              "tagged diagnostic retains MTU and complete admitted count");
        bool valid = packets.size() == 300U;
        for (size_t ordinal = 0U; ordinal < packets.size(); ++ordinal) {
            const auto& packet = packets[ordinal];
            if (packet.size() != 20U) { valid = false; continue; }
            valid = valid && packet[0U] == 0x01 && packet[1U] == 0x1e &&
                packet[2U] == 0x1f && packet[3U] == 0x01;
            for (uint8_t index = 0U; index < 8U; ++index)
                valid = valid && uint8_t(packet[4U + index]) ==
                    (0x10U | ((RUN >> (index * 4U)) & 0x0fU));
            for (uint8_t index = 0U; index < 4U; ++index)
                valid = valid && uint8_t(packet[12U + index]) ==
                    (0x10U | ((ordinal >> (index * 4U)) & 0x0fU));
            for (size_t index = 16U; index < packet.size(); ++index)
                valid = valid && packet[index] == 0x01;
        }
        Check(valid,
              "every native packet carries the exact run, full ordinal and payload");
    }
    {
        NotifyTestProbe probe;
        modules::BleLinkTestResult result;
        Check(!probe.service.RunConsoleNotifyTest(15U, 1U, 0U, result, 1U) &&
                  probe.notified == 0U && probe.blockingCallbacks == 0U,
              "tagged packet too small for proof rejects before effects");
        probe.disconnectAt = 2U;
        probe.replaceConnection = true;
        Check(!probe.service.RunConsoleNotifyTest(20U, 200U, 0U, result, 1U) &&
                  result.okCount == 2U && probe.notified == 2U,
              "tagged batch stops before writing to a replacement connection");
    }
    {
        NotifyTestProbe probe;
        NimBLECharacteristic status;
        Peer::ConfigureNotifyTest(probe.service, probe.characteristic, &status, 517U);
        for (const char* value : {"0", "-1", "4294967296", "null", "true", "\"1\""}) {
            const std::string request = std::string(
                "{\"cmd\":\"notify_test\",\"size\":20,\"count\":3,\"run\":") + value + "}";
            Peer::HandleOtaPacket(probe.service, request.data(), request.size());
            Check(probe.notified == 0U && probe.blockingCallbacks == 0U,
                  "malformed tagged run rejects before blanking or native submission");
        }
        constexpr char REQUEST[] =
            "{\"cmd\":\"notify_test\",\"size\":20,\"count\":3,\"run\":4294967295}";
        Peer::HandleOtaPacket(probe.service, REQUEST, sizeof(REQUEST) - 1U);
        const std::string reply = status.LastTx();
        Check(probe.notified == 3U &&
                  reply.find("run=4294967295 count=3") != std::string::npos &&
                  reply.find("size=20 ok=3 fail=0") != std::string::npos,
              "wire diagnostic echoes the full run and requested count with admission totals");
    }
}

void TestTaggedNotifyDiagnosticBackpressure() {
    fxe::ManualMillis::SetNowMs(0U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    {
        std::vector<std::string> packets;
        NotifyTestProbe probe;
        probe.capturedPackets = &packets;
        uint32_t attempts = 0U;
        probe.characteristic.SetNotifyAdmission(
            [](void* context, const uint8_t*, size_t) {
                return ++*static_cast<uint32_t*>(context) > 3U;
            }, &attempts);
        modules::BleLinkTestResult result;
        Check(probe.service.RunConsoleNotifyTest(20U, 2U, 0U, result, 7U) &&
                  result.okCount == 2U && result.failCount == 3U &&
                  result.bytesAttempted == 100U && result.bytesSent == 40U &&
                  attempts == 5U && packets.size() == 2U,
              "tagged producer retains packets through native queue pressure");
        Check(packets.size() == 2U && packets[0U][12U] == 0x10 &&
                  packets[1U][12U] == 0x11,
              "only native admission advances the tagged packet ordinal");
    }
    {
        NotifyTestProbe probe;
        probe.characteristic.SetNotifyResult(false);
        modules::BleLinkTestResult result;
        Check(!probe.service.RunConsoleNotifyTest(20U, 2U, 0U, result, 7U) &&
                  result.okCount == 0U && result.failCount == 10000U &&
                  result.elapsedMs == 10000U && probe.notified == 0U,
              "persistent tagged queue refusal ends at the existing batch deadline");
    }
}

// Hardware flash is replaced only at OtaPlatformAdapter. BLE ingress, queue
// ordering, transaction validation, lifecycle projection and render admission
// below are the production implementations.
class OtaStorageProbe final : public modules::OtaPlatformAdapter {
  public:
    bool Begin(uint32_t size) override { ++begins; expected = size; written = 0U; return true; }
    bool Write(const uint8_t*, size_t length) override { ++writes; written += length; return true; }
    bool End() override { ++ends; return endSucceeds && written == expected; }
    void Abort() override { ++aborts; }
    size_t expected = 0U;
    size_t written = 0U;
    uint32_t begins = 0U;
    uint32_t writes = 0U;
    uint32_t aborts = 0U;
    uint32_t ends = 0U;
    bool endSucceeds = true;
};

void TestOtaAdmissionOrdinarySelectionPreservesPriorityRace() {
    modules::BleEventQueue queue;
    Check(queue.Begin(), "ordinary dependency queue initializes");
    modules::BleEvent ordinary;
    ordinary.type = modules::BleEventType::OtaControl;
    ordinary.ingressOrdinal = UINT32_MAX - 1U;
    Check(queue.Enqueue(ordinary), "ordinary control enters existing FIFO");
    modules::BleEvent observed;
    Check(queue.TryPeek(observed) && observed.ingressOrdinal == ordinary.ingressOrdinal,
          "old consumer initially observes ordinary head");
    modules::BleEvent priority;
    priority.type = modules::BleEventType::ConnectionSession;
    priority.ingressOrdinal = 2U;
    priority.payload.connectionSession.len = 10U;
    priority.payload.connectionSession.input[0U] = 0x40U;
    priority.payload.connectionSession.input[1U] = 13U;
    Check(queue.Enqueue(priority) &&
              !queue.TryDequeueIfOrdinal(ordinary.ingressOrdinal, observed),
          "priority arriving after old peek still defeats original conditional dequeue");
    Check(queue.TryPeekOrdinary(observed) &&
              modules::IsBleOtaOrdinalEarlier(observed.ingressOrdinal, priority.ingressOrdinal) &&
              queue.TryDequeueOrdinaryIfOrdinal(observed.ingressOrdinal, observed) &&
              observed.ingressOrdinal == ordinary.ingressOrdinal &&
              queue.TryPeek(observed) && observed.ingressOrdinal == priority.ingressOrdinal,
          "explicit ordinary dependency selection handles ordinal wrap without consuming priority head");
}

enum class OtaAdmissionCase {
    Immediate, NotifyDenied, Close, OtherOutput, InvalidControl, Reconnect,
    PinnedStatus, BufferedObject,
};

void TestSharedOtaBeginNotifiesAdmissionBeforeBlocking(
    const OtaAdmissionCase scenario = OtaAdmissionCase::Immediate) {
    std::printf("[OTA_ADMISSION] case=%u\n", unsigned(scenario));
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    NimBLECharacteristic egress;
    NimBLECharacteristic legacy;
    OtaStorageProbe storage;
    Check(Peer::Configure(service, settings, control) &&
              Peer::ActivateMinimumConnectionObjectSession(service),
          "OTA admission fixture activates existing minimum-MTU session");
    Peer::PrepareProducerGeneration(service);
    Peer::ConfigureConnectionSessionNotifications(service, egress);
    Peer::SubscribeDiagnosticState(service);
    service.SetOtaPlatformAdapter(&storage);
    struct Capture {
        NimBLECharacteristic* egress;
        uint32_t token;
        uint32_t admitted = 0U;
        uint32_t finalSequence = 0U;
        unsigned blockingCalls = 0U;
        uint32_t reverseSequence = 0U;
    } capture{&egress, Peer::ObjectToken(service)};
    egress.SetNotifyHook([](void* context) {
        auto& value = *static_cast<Capture*>(context);
        const auto& bytes = value.egress->LastTx();
        wire::PacketView packet;
        if (wire::Codec::Decode(
                {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()},
                packet, 20U) != wire::Error::None) return;
        if (packet.header.kind == wire::Kind::Object) {
            value.reverseSequence = packet.header.sequence;
        }
        if (packet.header.kind == wire::Kind::Control &&
            packet.header.code == uint8_t(wire::Control::Progress) &&
            packet.header.token == value.token) {
            value.admitted = packet.header.sequence;
        }
    }, &capture);
    service.SetBeforeBlockingDiagnosticCallback([](void* context) {
        auto& value = *static_cast<Capture*>(context);
        ++value.blockingCalls;
        Check(value.finalSequence != 0U &&
                  value.admitted == value.finalSequence,
              "matching forward Object Progress is notified before blocking OTA Begin");
    }, &capture);
    const bool denied = scenario != OtaAdmissionCase::Immediate;
    egress.SetNotifyResult(!denied);
    if (scenario == OtaAdmissionCase::OtherOutput || scenario == OtaAdmissionCase::PinnedStatus) {
        Check(Peer::FillSharedReverseWindow(service),
              "admission pressure retains another real output before Begin");
        Peer::UpdateSharedStates(service);
        if (scenario == OtaAdmissionCase::PinnedStatus) {
            Peer::NotifyOtaJson(service, legacy, R"({"v":"retained-before-begin"})");
        }
    }
    auto queueControl = [&](const wire::Control code, const uint32_t token,
                            const fxe::ConstByteSpan body = {}) {
        std::array<uint8_t, 20U> bytes{};
        size_t size = 0U;
        Check(wire::Codec::Encode(
                  {{wire::Kind::Control, uint8_t(code), token, 0U}, body},
                  {bytes.data(), bytes.size()}, size) == wire::Error::None,
              "admission pressure encodes actual session control");
        service.OnConnectionSessionWrite(bytes.data(), size);
    };
    std::vector<uint8_t> finalEnvelope;
    const std::string command = R"({"cmd":"ota_begin","size":10,"chunk":20})";
    size_t offset = 0U;
    uint32_t sequence = 0U;
    auto queueBegin = [&] {
        offset = 0U;
        sequence = 0U;
        while (offset < command.size()) {
            const bool first = offset == 0U;
            const size_t header = first ? 4U : 2U;
            const size_t count = std::min<size_t>(10U - header, command.size() - offset);
            std::vector<uint8_t> body(header + count, 0U);
            body[0U] = 1U;
            body[1U] = (first ? 1U : 0U) |
                (offset + count == command.size() ? 2U : 0U);
            if (first) fxe::WriteU16LE(command.size(), body.data() + 2U);
            std::copy_n(command.data() + offset, count, body.data() + header);
            std::array<uint8_t, 20U> bytes{};
            size_t size = 0U;
            Check(wire::Codec::Encode(
                      {{wire::Kind::Object, 0x32U, capture.token, ++sequence},
                       {body.data(), body.size()}},
                      {bytes.data(), bytes.size()}, size) == wire::Error::None,
                  "OTA admission fixture encodes exact Object fragment");
            service.OnConnectionSessionWrite(bytes.data(), size);
            finalEnvelope.assign(bytes.begin(), bytes.begin() + size);
            offset += count;
        }
        capture.finalSequence = sequence;
    };
    queueBegin();
    if (scenario == OtaAdmissionCase::BufferedObject) {
        std::array<uint8_t, 20U> bytes{};
        const uint8_t body = 0U;
        size_t size = 0U;
        Check(wire::Codec::Encode(
                  {{wire::Kind::Object, 0x7fU, capture.token, sequence + 1U}, {&body, 1U}},
                  {bytes.data(), bytes.size()}, size) == wire::Error::None,
              "later unrelated Object enters ordinary FIFO behind final Begin fragment");
        service.OnConnectionSessionWrite(bytes.data(), size);
        for (uint32_t item = 0U; item < sequence; ++item) {
            Peer::LimitNextIngressDrainToOne(service);
            service.DrainEventQueue();
        }
    } else {
        service.DrainEventQueue();
    }
    if (denied) {
        Check(capture.blockingCalls == 0U && storage.begins == 0U &&
                  storage.writes == 0U && storage.ends == 0U &&
                  Peer::HasQueuedEvent(service),
              "notify refusal retains original queued Begin before all backend work");
        if (scenario == OtaAdmissionCase::Close || scenario == OtaAdmissionCase::PinnedStatus ||
            scenario == OtaAdmissionCase::BufferedObject) {
            queueControl(wire::Control::Close, capture.token);
        }
        if (scenario == OtaAdmissionCase::InvalidControl) {
            std::array<uint8_t, 10U> hello{};
            fxe::WriteU64LE(123U, hello.data());
            fxe::WriteU16LE(20U, hello.data() + 8U);
            queueControl(wire::Control::Hello, 0U, {hello.data(), hello.size()});
            std::array<uint8_t, 10U> invalid{};
            invalid[0U] = 0x40U;
            invalid[1U] = 255U;
            service.OnConnectionSessionWrite(invalid.data(), invalid.size());
        }
        queueControl(wire::Control::Query, capture.token);
        service.DrainEventQueue();
        Check((service.GetSessionDiagnostics().receivedControls &
                   (1U << uint8_t(wire::Control::Query))) != 0U &&
                  Peer::ObjectToken(service) == capture.token &&
                  capture.blockingCalls == 0U && storage.begins == 0U &&
                  Peer::HasQueuedEvent(service),
              "Query0 crosses backpressure while Close Hello and invalid controls keep their order");
        if (scenario == OtaAdmissionCase::Reconnect) {
            service.OnDisconnected();
            service.OnConnected(8U, "replacement-admission", 23U, 1U);
            service.DrainEventQueue();
            egress.SetNotifyResult(true);
            service.DrainEventQueue();
            Check(storage.begins == 0U && capture.blockingCalls == 0U &&
                      !service.IsOtaInProgress(),
                  "retired queued Begin cannot execute or reply in replacement connection");
            const uint32_t generation = Peer::ProducerEpoch(service);
            Check(Peer::ActivateMinimumConnectionObjectSession(service, generation),
                  "replacement connection activates fresh session after retiring old queued control");
            Peer::SubscribeDiagnosticState(service);
            capture.token = Peer::ObjectToken(service);
            capture.admitted = 0U;
            queueBegin();
            service.DrainEventQueue();
            Check(storage.begins == 1U && capture.blockingCalls == 1U &&
                      capture.admitted == capture.finalSequence && service.IsOtaInProgress(),
                  "only fresh replacement Begin executes after its own admission");
            return;
        }
        egress.SetNotifyResult(true);
        for (unsigned attempt = 0U; attempt < 16U && storage.begins == 0U; ++attempt) {
            Peer::UpdateSharedStates(service);
            if (capture.reverseSequence != 0U) {
                Check(Peer::QueueSharedReverseProgress(service, capture.reverseSequence),
                      "actual notified reverse output receives cumulative credit repair");
                capture.reverseSequence = 0U;
            }
            service.DrainEventQueue();
        }
    }
    Check(capture.admitted == capture.finalSequence,
          "forward Progress eventually reaches the actual subscribed notify seam");
    Check(capture.blockingCalls == 1U && storage.begins == 1U &&
              storage.writes == 0U && storage.ends == 0U &&
              service.IsOtaInProgress(),
          "admitted Begin executes once before any image bytes or End");
    service.DrainEventQueue();
    if (scenario == OtaAdmissionCase::Close || scenario == OtaAdmissionCase::PinnedStatus ||
        scenario == OtaAdmissionCase::BufferedObject) {
        Check(Peer::ObjectToken(service) == 0U &&
                  (service.GetSessionDiagnostics().receivedControls &
                   (1U << uint8_t(wire::Control::Close))) != 0U,
              "original Close retires its stream only after Begin admission and execution");
    }
    service.OnConnectionSessionWrite(finalEnvelope.data(), finalEnvelope.size());
    service.DrainEventQueue();
    Check(capture.blockingCalls == 1U && storage.begins == 1U &&
              storage.writes == 0U && storage.ends == 0U,
          "exact envelope replay never repeats admitted Begin or creates image bytes");
}

void TestOtaBeginWaitsForStoreClear() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    for (const bool simulated : {false, true}) {
        for (const bool running : {false, true}) {
            fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
            fxe::ManualMillis::SetNowMs(1000U);
            modules::SettingsStore settings;
            modules::ControlMutationService control;
            modules::BleService service;
            OtaStorageProbe image;
            NimBLECharacteristic status;
            Check(Peer::Configure(service, settings, control),
                  "OTA clear exclusion fixture initializes production ingress");
            Peer::ConfigureOtaNotifications(service, status);
            service.SetOtaPlatformAdapter(&image);
            service.OnConnected(7U, "clear-ota", 247U, 1U);
            service.DrainEventQueue();
            struct StoreProbe {
                bool pending = false;
                bool running = false;
                bool savePending = true;
                unsigned beginChecks = 0U;
                unsigned saveRequests = 0U;
                unsigned beforeBlocking = 0U;
            } store{!running, running};
            service.SetOtaStorePersistenceCallbacks(
                [](void* context) { ++static_cast<StoreProbe*>(context)->saveRequests; },
                [](void* context) { return static_cast<StoreProbe*>(context)->savePending; },
                &store, [](void* context) {
                    auto& probe = *static_cast<StoreProbe*>(context);
                    ++probe.beginChecks;
                    return probe.pending || probe.running;
                });
            service.SetBeforeBlockingDiagnosticCallback(
                [](void* context) { ++static_cast<StoreProbe*>(context)->beforeBlocking; },
                &store);
            const char* const begin = simulated
                ? R"({"cmd":"ota_sim_begin","size":200,"chunk":100})"
                : R"({"cmd":"ota_begin","size":200,"chunk":100})";
            std::array<uint8_t, 100U> stale{};
            stale.fill(0x7bU);
            service.OnOtaControlCharacteristicWrite(begin, std::strlen(begin));
            Check(service.OnOtaDataCharacteristicWrite(stale.data(), stale.size()),
                  "rejected Begin has queued production data to invalidate");
            service.DrainEventQueue();
            service.DrainEventQueue();
            Check(status.LastTx().find("busy_store_clear") != std::string::npos &&
                      !service.IsOtaInProgress() && !service.GetOtaLedPresentation().active &&
                      image.begins == 0U && image.writes == 0U && image.ends == 0U &&
                      store.beforeBlocking == 0U && store.beginChecks == 1U &&
                      store.saveRequests == 0U,
                  "pending or running clear rejects real and simulated Begin before OTA work");
            Check(service.OnOtaDataCharacteristicWrite(stale.data(), stale.size()),
                  "late bytes from the rejected update enter the production queue");
            store.pending = false;
            store.running = false;
            service.OnOtaControlCharacteristicWrite(begin, std::strlen(begin));
            service.DrainEventQueue();
            service.DrainEventQueue();
            Check(service.IsOtaInProgress() &&
                      status.LastTx().find("begin") != std::string::npos &&
                      service.GetOtaLedPresentation().percent == 0U &&
                      image.begins == (simulated ? 0U : 1U) && image.writes == 0U &&
                      store.beforeBlocking == 1U && store.beginChecks == 2U &&
                      store.savePending && store.saveRequests == 0U,
                  "terminal clear admits fresh Begin despite ordinary pending saves and no stale bytes");
            const std::array<uint8_t, 100U> fresh{};
            Check(service.OnOtaDataCharacteristicWrite(fresh.data(), fresh.size()) &&
                      service.OnOtaDataCharacteristicWrite(fresh.data(), fresh.size()),
                  "fresh update admits its own image bytes");
            service.DrainEventQueue();
            constexpr const char* end = R"({"cmd":"ota_end","size":200,"crc32":"c971a876"})";
            service.OnOtaControlCharacteristicWrite(end, std::strlen(end));
            service.DrainEventQueue();
            Check(!service.IsOtaInProgress() &&
                      status.LastTx().find("done") != std::string::npos &&
                      image.writes == (simulated ? 0U : 1U) &&
                      image.written == (simulated ? 0U : fresh.size() * 2U) &&
                      image.ends == (simulated ? 0U : 1U),
                  "only fresh bytes reach the backend and CRC-valid completion after clear");
            std::printf("[OTA_CLEAR_BEGIN] simulated=%u clear=%s rejected=busy_store_clear "
                        "fresh=done pending_save_admitted=1 backend_bytes=%zu\n",
                        simulated, running ? "running" : "pending", image.written);
        }
    }
}

void TestOtaRejectsNewStoreBulkUntilAbort() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    Check(Peer::Configure(service, settings, control),
          "OTA Store ingress fixture starts bounded queues");
    service.OnConnected(7U, "ota-store", 247U, 1U);
    service.DrainEventQueue();
    constexpr const char* beginOta =
        R"({"cmd":"ota_sim_begin","size":53,"chunk":53})";
    service.OnOtaControlCharacteristicWrite(beginOta, std::strlen(beginOta));
    service.DrainEventQueue();
    Check(service.IsOtaInProgress(),
          "OTA Store ingress fixture has an active update");

    std::array<uint8_t, 53U> storeBegin{};
    storeBegin[0U] = 0x4dU;
    storeBegin[1U] = 2U;
    storeBegin[2U] = 18U;
    std::array<uint8_t, 57U> object{};
    object[0U] = 1U;
    object[1U] = 3U;
    object[2U] = static_cast<uint8_t>(storeBegin.size());
    std::copy(storeBegin.begin(), storeBegin.end(), object.begin() + 4U);
    std::vector<uint8_t> received;
    uint32_t ordinal = 0U;
    Check(!Peer::HandleConnectionLitchcraft(
              service, object.data(), object.size()) &&
              !Peer::TakeOtaData(service, received, ordinal),
          "active OTA rejects a new Store replacement before worker ingress");

    constexpr const char* abortOta = R"({"cmd":"ota_abort"})";
    service.OnOtaControlCharacteristicWrite(abortOta, std::strlen(abortOta));
    service.DrainEventQueue();
    Check(!service.IsOtaInProgress() &&
              Peer::HandleConnectionLitchcraft(
                  service, object.data(), object.size()) &&
              Peer::TakeOtaData(service, received, ordinal) &&
              received == std::vector<uint8_t>(
                  storeBegin.begin(), storeBegin.end()),
          "Store replacement ingress resumes after OTA abort");
}

void TestOtaEndWaitsForDurableArtwork() {
    using Persistence = modules::LitchcraftPersistence;
    using Art = modules::LitchcraftSession;
    enum class Ending { Saved, Failed, Timeout, PreparationBusy, PreparationTimeout, Disconnect, Abort, Replacement, PriorFlush, InvalidCrc, Simulation, NearTimeout, ImageFailure, SleepCancel, GraceDisable, GraceRename, StoreDirty, StoreModePending, StoreCommitPending, StoreTimeout };
    constexpr const char* ENDING_NAMES[] = {"saved", "storage-failed-retry", "timeout", "disconnect",
        "preparation-busy", "preparation-timeout", "abort", "replacement", "prior-explicit-flush", "invalid-crc", "simulation", "near-inactivity", "image-failed", "sleep-cancel", "grace-disable", "grace-rename", "store-dirty", "store-mode-pending", "store-commit-pending", "store-timeout"};
    for (const auto ending : {Ending::Saved, Ending::Failed, Ending::Timeout,
                              Ending::PreparationBusy, Ending::PreparationTimeout, Ending::Disconnect, Ending::Abort, Ending::Replacement, Ending::PriorFlush,
                              Ending::InvalidCrc, Ending::Simulation, Ending::NearTimeout, Ending::ImageFailure, Ending::SleepCancel, Ending::GraceDisable, Ending::GraceRename,
                              Ending::StoreDirty, Ending::StoreModePending, Ending::StoreCommitPending, Ending::StoreTimeout}) {
        fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(1000U);
        modules::SettingsStore settings;
        settings.Doc()["GAME_FEN"] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
        modules::LitchMaxEngine engine;
        runtime::GameStateMachine state;
        modules::GamePersistenceCoordinator game(settings, engine, state);
        Check(game.LoadOrInitialize(), "OTA persistence fixture initializes real game snapshot owner");
        modules::ControlMutationService control;
        modules::BleService service;
        Check(modules::BleServiceConnectionEpochTestPeer::Configure(service, settings, control), "OTA durable queues initialize");
        service.SetLitchcraftCoordinator(&game);
        OtaStorageProbe image;
        service.SetOtaPlatformAdapter(&image);
        NimBLECharacteristic status;
        modules::BleServiceConnectionEpochTestPeer::ConfigureOtaNotifications(service, status);
        service.OnConnected(7U, "durable-ota", 247U, 1U); service.DrainEventQueue();
        struct DurableProbe {
            Persistence::SaveOutcome outcome = Persistence::SaveOutcome::Pending;
            unsigned admitted = 0U;
            bool retained = false;
            unsigned lateSetting = 0U;
        } durable;
        struct StoreProbe {
            bool dirty = false;
            bool modePending = false;
            bool commitPending = false;
            unsigned requests = 0U;
        } store;
        if (ending == Ending::StoreDirty || ending == Ending::StoreTimeout ||
            ending == Ending::StoreModePending ||
            ending == Ending::StoreCommitPending) {
            store.dirty = ending != Ending::StoreModePending;
            store.modePending = ending == Ending::StoreModePending;
            store.commitPending = ending == Ending::StoreCommitPending;
            if (store.commitPending) store.dirty = false;
            service.SetOtaStorePersistenceCallbacks(
                [](void* context) {
                    ++static_cast<StoreProbe*>(context)->requests;
                },
                [](void* context) {
                    const auto& state = *static_cast<StoreProbe*>(context);
                    return state.dirty || state.modePending ||
                           state.commitPending;
                }, &store);
        }
        game.ArtPersistence().Configure({&durable,
            [](void* context, const JsonDocument& document, uint32_t& receipt) {
                auto& probe = *static_cast<DurableProbe*>(context);
                if (probe.retained) return fxe::SettingsStore::AsyncSaveAdmission::Deferred;
                probe.retained = true; probe.outcome = Persistence::SaveOutcome::Pending;
                receipt = ++probe.admitted;
                probe.lateSetting = document["OTA_LATE_SETTING"] | 0U;
                return fxe::SettingsStore::AsyncSaveAdmission::Accepted;
            },
            [](void* context, uint32_t) { return static_cast<DurableProbe*>(context)->outcome; },
            [](void* context, uint32_t) { static_cast<DurableProbe*>(context)->retained = false; }});
        auto& art = game.Litchcraft();
        art.Configure(3U, 123U);
        uint8_t enter[] = {Art::MAGIC, uint8_t(Art::Op::Enter), 0, 0, 0, 0, 1, 0};
        art.HandlePacket(enter, sizeof(enter), true, 0U);
        uint8_t paint[] = {Art::MAGIC, uint8_t(Art::Op::Paint), 0, 0, 0, 0, 2, 0, 1, 0, 3, 0};
        for (unsigned i = 0U; i < 4U; ++i) paint[2U + i] = art.SessionId() >> (8U * i);
        service.OnLitchcraftWrite(paint, sizeof(paint)); service.DrainEventQueue();
        if (ending == Ending::SleepCancel) {
            art.ConsumeActivity();
            firmware::SleepController sleep;
            Check(sleep.RequestSleep(firmware::SleepRequestOrigin::Idle, fxe::Millis(), true),
                  "durable sleep fixture enters the production countdown");
            fxe::ManualMillis::AdvanceMs(3500U);
            sleep.Update(settings, fxe::Millis(), firmware::SleepSuppression::Allow);
            firmware::SleepPreparationHooks hooks{&game,
                [](void* context) {
                    auto& coordinator = *static_cast<modules::GamePersistenceCoordinator*>(context);
                    if (!coordinator.PrepareSettingsSnapshotForLifecycle()) return false;
                    coordinator.ArtPersistence().RequestFlush();
                    coordinator.ArtPersistence().Update();
                    return true;
                },
                [](void* context) {
                    auto& persistence = static_cast<modules::GamePersistenceCoordinator*>(context)->ArtPersistence();
                    persistence.Update();
                    if (persistence.FlushState() == Persistence::FlushStatus::Saved) return firmware::SleepPersistenceStatus::Succeeded;
                    if (persistence.FlushState() == Persistence::FlushStatus::Failed) return firmware::SleepPersistenceStatus::Failed;
                    return firmware::SleepPersistenceStatus::Pending;
                }, [](void*) { return true; }, [](void*) { return true; }};
            Check(sleep.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Pending &&
                      sleep.GetState() == firmware::SleepState::Preparing && durable.admitted == 1U,
                  "sleep remains Preparing while production art receipt is pending");
            const auto priorGeneration = art.CanvasGeneration();
            paint[6] = 3U; paint[11] = 2U;
            service.OnLitchcraftWrite(paint, sizeof(paint)); service.DrainEventQueue();
            game.UpdateLitchcraft();
            if (art.ConsumeActivity()) sleep.NoteHallSensorActivity();
            Check(art.CanvasGeneration() != priorGeneration && sleep.GetState() == firmware::SleepState::Awake,
                  "accepted BLE painting cancels sleep through the production activity ordering");
            durable.outcome = Persistence::SaveOutcome::Saved;
            game.UpdateLitchcraft();
            Check(sleep.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::NotPreparing &&
                      !sleep.ConsumeSleepAuthorization(),
                  "an older Saved receipt cannot authorize teardown after accepted painting woke the board");
            std::printf("[LITCHCRAFT_SLEEP_CANCEL] callback=Paint#3 mode=Litchcraft sleep=Awake old_saved_teardown=0\n");
            continue;
        }
        const auto send = [&](const char* data) {
            service.OnOtaControlCharacteristicWrite(data, std::strlen(data)); service.DrainEventQueue();
        };
        const auto begin = [&]() {
            send(ending == Ending::Simulation ? R"({"cmd":"ota_sim_begin","size":200,"chunk":100})" :
                                               R"({"cmd":"ota_begin","size":200,"chunk":100})");
            uint8_t bytes[100] = {};
            service.OnOtaDataCharacteristicWrite(bytes, sizeof(bytes));
            service.OnOtaDataCharacteristicWrite(bytes, sizeof(bytes));
            service.DrainEventQueue();
        };
        constexpr const char* END = R"({"cmd":"ota_end","size":200,"crc32":"c971a876"})";
        if (ending == Ending::PriorFlush) {
            game.ArtPersistence().RequestFlush(); game.ArtPersistence().Update();
            settings.Doc()["OTA_LATE_SETTING"] = 9U;
        }
        begin();
        if (ending == Ending::PreparationBusy || ending == Ending::PreparationTimeout) {
            // This direct-modal fixture bypasses Hall; observe its stable board before O.
            state.DebugSetPhysicalOccupancyMask(state.GetExpectedOccupancyMask());
            Check(state.StartPromotion(), "OTA persistence fixture enters a transient promotion interaction");
        }
        if (ending == Ending::NearTimeout) fxe::ManualMillis::AdvanceMs(29999U);
        if (ending == Ending::InvalidCrc) {
            send(R"({"cmd":"ota_end","size":200,"crc32":"00000000"})");
            Check(durable.admitted == 0U && image.ends == 0U && !service.IsOtaInProgress(),
                  "invalid OTA completion never prepares or persists game/art state");
            continue;
        }
        send(END); send(END);
        if (ending == Ending::Simulation) {
            Check(durable.admitted == 0U && status.LastTx().find("done") != std::string::npos,
                  "simulation has no reboot lifecycle and never requests durable artwork");
            continue;
        }
        if (ending == Ending::PreparationBusy || ending == Ending::PreparationTimeout) {
            Check(image.ends == 0U && image.aborts == 0U && durable.admitted == 0U &&
                      service.IsOtaInProgress() &&
                      status.LastTx().find("persist_failed") == std::string::npos,
                  "snapshot-not-ready is retained for retry instead of misreported as storage failure");
            if (ending == Ending::PreparationTimeout) {
                fxe::ManualMillis::AdvanceMs(10000U); service.DrainEventQueue();
                Check(image.ends == 0U && !service.IsOtaInProgress() &&
                          status.LastTx().find("persist_not_ready") != std::string::npos,
                      "snapshot preparation timeout remains distinct from a storage failure");
                continue;
            }
            state.HandleButtonEvent(modules::UserButton::P1O, fxe::Button::Event::Press);
            modules::PromotionChoice choice;
            Check(state.ConsumePromotionChoice(choice), "transient promotion interaction resolves");
            fxe::ManualMillis::AdvanceMs(100U); service.DrainEventQueue();
            Check(durable.admitted == 1U && service.IsOtaInProgress(),
                  "OTA retries and admits persistence once the game snapshot becomes ready");
            durable.outcome = Persistence::SaveOutcome::Saved; service.DrainEventQueue();
            Check(image.ends == 1U && status.LastTx().find("done") != std::string::npos,
                  "retried snapshot reaches durable success and finalizes the validated image");
            continue;
        }
        Check(image.ends == 0U && service.IsOtaInProgress() && durable.admitted == 1U &&
                  status.LastTx().find("done") == std::string::npos,
              "duplicate OTA end waits on one durable receipt before image finalization or done");
        if (ending == Ending::StoreDirty || ending == Ending::StoreModePending ||
            ending == Ending::StoreCommitPending || ending == Ending::StoreTimeout) {
            const auto paintBeforePendingEnd = art.CanvasGeneration();
            paint[6] = 3U; paint[11] = 2U;
            service.OnLitchcraftWrite(paint, sizeof(paint));
            service.DrainEventQueue();
            Check(art.CanvasGeneration() == paintBeforePendingEnd,
                  "accepted OTA end freezes artwork while Store save is pending");
            durable.outcome = Persistence::SaveOutcome::Saved;
            service.DrainEventQueue();
            Check(store.requests == 1U && image.ends == 0U &&
                      service.IsOtaInProgress() &&
                      status.LastTx().find("done") == std::string::npos,
                  "OTA end cannot finalize while Store edit or mode root is unsaved");
            if (ending == Ending::StoreTimeout) {
                fxe::ManualMillis::AdvanceMs(10000U);
                service.DrainEventQueue();
                Check(image.ends == 0U && !service.IsOtaInProgress() &&
                          status.LastTx().find("persist_timeout") != std::string::npos,
                      "dirty Store timeout rejects OTA without false completion");
                paint[6] = 4U;
                service.OnLitchcraftWrite(paint, sizeof(paint));
                service.DrainEventQueue();
                Check(art.CanvasGeneration() != paintBeforePendingEnd,
                      "Store save timeout releases temporary artwork pause");
                store.dirty = false;
                begin(); send(END);
                durable.outcome = Persistence::SaveOutcome::Saved;
                service.DrainEventQueue();
                Check(store.requests == 2U && image.ends == 1U &&
                          status.LastTx().find("done") != std::string::npos,
                      "fresh OTA retry finalizes after Store save recovers");
            } else {
                store.dirty = false;
                store.modePending = false;
                store.commitPending = false;
                service.DrainEventQueue();
                Check(image.ends == 1U &&
                          status.LastTx().find("done") != std::string::npos,
                      "OTA finalizes after Store and settings are both durable");
            }
            continue;
        } else if (ending == Ending::GraceDisable || ending == Ending::GraceRename) {
            modules::BleServiceConnectionEpochTestPeer::MarkInitialized(service);
            if (ending == Ending::GraceDisable) settings.Doc()["BLE_ENABLED"] = 0U;
            else settings.Doc()["BLE_DEVICE_NAME"] = "New Name";
            durable.outcome = Persistence::SaveOutcome::Saved; service.DrainEventQueue();
            const unsigned priorShutdowns = ota_runtime_test::shutdowns;
            const unsigned priorRestarts = ota_runtime_test::restarts;
            service.Update(settings);
            const auto priorGeneration = art.CanvasGeneration();
            paint[6] = 3U; paint[11] = 2U;
            service.OnLitchcraftWrite(paint, sizeof(paint)); service.DrainEventQueue();
            Check(ota_runtime_test::shutdowns == priorShutdowns &&
                      modules::BleServiceConnectionEpochTestPeer::PendingReboot(service) &&
                      art.CanvasGeneration() == priorGeneration,
                  "queued BLE settings changes cannot tear down finalization grace or unpause artwork");
            std::array<uint8_t, 57U> storeBeginObject{};
            storeBeginObject[0U] = 1U;
            storeBeginObject[1U] = 3U;
            storeBeginObject[2U] = 53U;
            storeBeginObject[4U] = 0x4dU;
            storeBeginObject[5U] = 2U;
            storeBeginObject[6U] = 18U;
            Check(!modules::BleServiceConnectionEpochTestPeer::
                      HandleConnectionLitchcraft(
                          service, storeBeginObject.data(),
                          storeBeginObject.size()),
                  "OTA reboot grace rejects fresh Store bulk ingress");
            fxe::ManualMillis::AdvanceMs(499U); service.Update(settings);
            Check(ota_runtime_test::restarts == priorRestarts, "finalized OTA retains its full 500ms reboot grace");
            fxe::ManualMillis::AdvanceMs(1U);
            bool restarted = false;
            try { service.Update(settings); } catch (const ota_runtime_test::RestartRequested&) { restarted = true; }
            Check(restarted && ota_runtime_test::restarts == priorRestarts + 1U,
                  "disabled or renamed BLE still reaches the production OTA restart exactly once at the deadline");
        } else if (ending == Ending::ImageFailure) {
            image.endSucceeds = false;
            durable.outcome = Persistence::SaveOutcome::Saved; service.DrainEventQueue();
            Check(image.ends == 1U && !service.IsOtaInProgress() &&
                      status.LastTx().find("end_failed") != std::string::npos,
                  "failed image finalization never reports done");
            const auto priorGeneration = art.CanvasGeneration();
            paint[6] = 3U; paint[11] = 2U;
            service.OnLitchcraftWrite(paint, sizeof(paint)); service.DrainEventQueue();
            Check(art.CanvasGeneration() != priorGeneration && art.IsActive(),
                  "failed image finalization releases the temporary artwork mutation pause");
        } else if (ending == Ending::Saved) {
            paint[6] = 3U; paint[11] = 1U;
            service.OnLitchcraftWrite(paint, sizeof(paint)); service.DrainEventQueue();
            // The V3.3's tracked save took 5086 ms during the reported failure.
            fxe::ManualMillis::AdvanceMs(5086U); service.DrainEventQueue();
            Check(image.ends == 0U && image.aborts == 0U && service.IsOtaInProgress(),
                  "OTA waits through the observed slow save without activating or aborting");
            durable.outcome = Persistence::SaveOutcome::Saved; service.DrainEventQueue();
            Check(image.ends == 0U && durable.admitted == 2U && service.IsOtaInProgress(),
                  "artwork mutated during a pending save requires another durable snapshot before OTA End");
            durable.outcome = Persistence::SaveOutcome::Saved; service.DrainEventQueue();
            Check(image.ends == 1U && status.LastTx().find("done") != std::string::npos,
                  "OTA finalizes once only after the newest artwork is durable");
            send(END);
            Check(image.ends == 1U && durable.admitted == 2U, "completed OTA end duplicate never rewrites art or re-finalizes image");
            const auto savedGeneration = art.CanvasGeneration();
            art.ObserveOccupancy(UINT64_C(1) << 9U);
            art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Press, modules::UiOwner::P1});
            paint[6] = 4U; paint[11] = 2U;
            service.OnLitchcraftWrite(paint, sizeof(paint)); service.DrainEventQueue();
            uint8_t exit[] = {Art::MAGIC, uint8_t(Art::Op::Exit), 0, 0, 0, 0, 5, 0};
            for (unsigned i = 0U; i < 4U; ++i) exit[2U + i] = art.SessionId() >> (8U * i);
            service.OnLitchcraftWrite(exit, sizeof(exit)); service.DrainEventQueue();
            for (uint16_t offset = 0U; offset < Art::ELEMENT_COUNT;) {
                const uint8_t count = std::min<uint16_t>(77U, Art::ELEMENT_COUNT - offset);
                std::vector<uint8_t> bulk{Art::MAGIC, Art::BULK_REPLACE};
                for (unsigned i = 0U; i < 4U; ++i) bulk.push_back(art.SessionId() >> (8U * i));
                bulk.insert(bulk.end(), {6U, 0U, uint8_t(offset), uint8_t(offset >> 8U),
                                        uint8_t(offset + count == Art::ELEMENT_COUNT), count});
                bulk.insert(bulk.end(), size_t(count) * 3U, 42U);
                service.OnOtaDataCharacteristicWrite(bulk.data(), bulk.size());
                service.DrainEventQueue();
                offset += count;
            }
            Check(art.CanvasGeneration() == savedGeneration && art.IsActive(),
                  "post-finalization reboot grace cannot accept physical, BLE paint, bulk or mode mutations");
        } else if (ending == Ending::PriorFlush) {
            durable.outcome = Persistence::SaveOutcome::Saved; service.DrainEventQueue();
            Check(image.ends == 0U && durable.admitted == 2U && durable.lateSetting == 9U,
                  "OTA cannot inherit an older explicit lifecycle receipt as its newly prepared settings save");
            durable.outcome = Persistence::SaveOutcome::Saved; service.DrainEventQueue();
            Check(image.ends == 1U, "OTA finalizes after its own latest whole-document flush");
        } else if (ending == Ending::NearTimeout) {
            fxe::ManualMillis::AdvanceMs(2U);
            modules::BleServiceConnectionEpochTestPeer::PollOtaInactivity(service);
            Check(service.IsOtaInProgress() && image.ends == 0U,
                  "accepted OTA end retains its persistence window across the old chunk inactivity deadline");
            fxe::ManualMillis::AdvanceMs(9998U);
            durable.outcome = Persistence::SaveOutcome::Saved; service.DrainEventQueue();
            Check(image.ends == 1U, "a durable success observed at the deadline takes precedence over timing out");
        } else if (ending == Ending::Failed) {
            durable.outcome = Persistence::SaveOutcome::Failed; service.DrainEventQueue();
            Check(image.ends == 0U && image.aborts != 0U && !service.IsOtaInProgress() &&
                      status.LastTx().find("persist_failed") != std::string::npos && art.IsActive(),
                  "terminal art persistence failure aborts OTA without false done and preserves painting owner");
            begin(); send(END);
            durable.outcome = Persistence::SaveOutcome::Saved; service.DrainEventQueue();
            Check(image.ends == 1U && status.LastTx().find("done") != std::string::npos,
                  "a fresh OTA retry can complete after persistence recovers");
        } else if (ending == Ending::Timeout) {
            fxe::ManualMillis::AdvanceMs(9999U); service.DrainEventQueue();
            Check(image.ends == 0U && service.IsOtaInProgress(), "durable OTA wait remains pending before its bounded deadline");
            fxe::ManualMillis::AdvanceMs(1U); service.DrainEventQueue();
            Check(image.ends == 0U && !service.IsOtaInProgress() && durable.retained &&
                      status.LastTx().find("persist_timeout") != std::string::npos,
                  "OTA persistence timeout is distinguished from storage failure and does not pretend an accepted receipt was canceled");
        } else {
            durable.outcome = Persistence::SaveOutcome::Saved;
            if (ending == Ending::Abort) send(R"({"cmd":"ota_abort"})");
            else {
                if (ending == Ending::Disconnect) service.OnDisconnected();
                service.OnConnected(8U, "replacement", 247U, 1U);
                service.DrainEventQueue();
            }
            game.ArtPersistence().Update(); service.DrainEventQueue();
            Check(image.ends == 0U && !service.IsOtaInProgress() &&
                      status.LastTx().find("done") == std::string::npos,
                  "queued abort or replacement connection defeats a ready old-generation OTA finalization");
        }
        std::printf("[OTA_DURABLE_END] case=%s finalize_calls=%u admissions=%u active=%u\n",
                    ENDING_NAMES[unsigned(ending)], image.ends, durable.admitted, service.IsOtaInProgress());
    }
}

void TestDeviceInfoAdvertisesAnimationWithStoreIngress() {
    modules::SettingsStore settings;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(settings, engine, state);
    modules::BleService service;
    NimBLECharacteristic status;
    NimBLECharacteristic craft;
    NimBLECharacteristic data;

    const std::string unavailable =
        modules::BleServiceConnectionEpochTestPeer::DeviceInfoPayload(
            service, game, status, craft, data);
    Check(unavailable == "{\"i\":[3,3,15423]}",
          "device info omits animation when Store ingress is unavailable");

    service.SetLitchcraftFrameIngressCallbacks(
        [](void*, modules::BleOtaChunkQueue&,
           modules::litchcraft_frame_store::StoreProjection, uint32_t,
           uint16_t) {},
        [](void*, const uint8_t*, size_t, uint32_t) {},
        [](void*, uint8_t*, size_t, size_t& outSize) {
            outSize = 0U;
            return false;
        },
        [](void*) {},
        [](void*, uint16_t, uint32_t) { return false; }, nullptr);
    const std::string available =
        modules::BleServiceConnectionEpochTestPeer::DeviceInfoPayload(
            service, game, status, craft, data);
    Check(available == "{\"i\":[3,3,31807]}",
          "Store-only runtime advertises animation without retired schema-3 storage");
}

// Models only the physical latch/SDK boundary. Uses the production arbiter;
// real bus timing, RGBW electrical current and brightness still require HIL.
class OtaLedLatchProbe final : public modules::ILedRenderer {
  public:
    explicit OtaLedLatchProbe(uint16_t count) : ledCount(count) {}
    uint16_t GetLedCount() const override { return ledCount; }
    void Clear() override { pending.fill(modules::RGB_BLACK); }
    void SetLedRgb(uint16_t i, uint8_t r, uint8_t g, uint8_t b) override {
        if (i < pending.size()) pending[i] = {r, g, b};
    }
    void Show() override {
        if (!arbiter.TryBeginRender()) return;
        latched = pending;
        ++shows;
        arbiter.EndRender();
    }
    hardware::RmtFlashArbiter arbiter;
    std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT> pending{}, latched{};
    uint16_t ledCount;
    uint32_t shows = 0U;
};

void TestOtaRuntimeProgressJourney(const hardware::BoardRevision revision,
                                 const bool rotate) {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillis::NowMs);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    OtaStorageProbe storage;
    Check(modules::BleServiceConnectionEpochTestPeer::Configure(
              service, settings, control), "OTA journey queues initialize");
    service.SetOtaPlatformAdapter(&storage);
    NimBLECharacteristic otaStatus;
    modules::BleServiceConnectionEpochTestPeer::ConfigureOtaNotifications(service, otaStatus);
    service.OnConnected(7U, "virtual-ota", 247U, 1U);
    service.DrainEventQueue();
    firmware::LedRuntimeAdmission admission;
    hardware::PhysicalLedFrame frame{};
    const auto topology = hardware::BoardLedTopologyFor(revision);
    OtaLedLatchProbe leds(topology.physicalBoardLedCount + topology.physicalStatusLedCount);
    modules::PowerBudgetPolicy power(
        modules::led_render_policy::MakeConservativePowerBudgetPolicyConfig(
            {true, false}));
    const bool rgbw = revision != hardware::BoardRevision::V3_3S3;
    auto pipelineConfig = modules::led_render_policy::MakePipelineConfig(rgbw, 32U);
    if (!rgbw) {
        pipelineConfig.channelMaxCurrentMa = 20U;
        pipelineConfig.outputBrightnessScale256 = 128U;
    }
    modules::BudgetAwareLedPipeline pipeline(leds, power, pipelineConfig);
    pipeline.SetBrightnessScale01(modules::OtaLedBrightnessScale(8U) / 256.0f);
    auto render = [&](bool due) {
        const auto presentation = service.GetOtaLedPresentation();
        if (!admission.Update(presentation.active, due, fxe::Millis())) return false;
        modules::BoardLedFrame semanticBoard{};
        modules::ComposeOtaLedPresentation(presentation, semanticBoard);
        frame = hardware::MapSemanticLedFrame(
            {revision, rotate, rotate},
            semanticBoard,
            std::array<modules::Rgb, modules::EDGE_LED_COUNT>{});
        modules::ApplyOtaPhysicalStatusPresentation(
            presentation, revision, frame);
        pipeline.RenderFrame(frame.data(), leds.GetLedCount());
        Check(pipeline.GetLastStats().appliedLedCurrentMa <= power.GetAllowedLedCurrentMa(),
              "OTA frame stays within conservative current allowance");
        return true;
    };
    auto send = [&](const char* command) {
        service.OnOtaControlCharacteristicWrite(command, std::strlen(command));
        service.DrainEventQueue();
    };
    auto visible = [&]() {
        for (const auto pixel : leds.latched) {
            if (pixel.r != 0U || pixel.g != 0U || pixel.b != 0U) return true;
        }
        return false;
    };
    // Independent PCB-chain oracle: V3.3 inserts six button LEDs after
    // perimeter positions 3 and 19. USB-side positions remain 27 and 28.
    const auto perimeterPixel = [&](uint8_t position) {
        const uint8_t chain = revision != hardware::BoardRevision::V3_3S3
            ? position : position < 4U ? position
            : static_cast<uint8_t>(position < 20U ? position + 6U : position + 12U);
        return leds.latched[topology.physicalBoardLedCount + chain];
    };
    auto checkPerimeter = [&](const char* checkpoint, uint8_t expectedLit) {
        for (uint8_t position = 0U; position < 32U; ++position) {
            const auto pixel = perimeterPixel(position);
            Check((pixel.b != 0U) == (position < expectedLit),
                  "OTA byte progress is contiguous across every physical perimeter position, including USB");
        }
        if (!rgbw) {
            for (const uint8_t first : {4U, 26U}) {
                for (uint8_t role = 0U; role < 6U; ++role) {
                    const auto pixel = leds.latched[256U + first + role];
                    Check(pixel.r == 0U && pixel.g == 0U && pixel.b == 0U,
                          "OTA progress leaves inserted V3.3 button LEDs dark");
                }
            }
        }
        std::printf("[OTA_PERIMETER_JOURNEY] revision=%u rotate=%u step=%s t=%u "
                    "active=%u received=%u perimeter=%u/32 usb=%u,%u next=%s\n",
                    unsigned(revision), rotate, checkpoint, fxe::Millis(),
                    service.GetOtaLedPresentation().active,
                    service.GetOtaLedPresentation().percent, expectedLit,
                    perimeterPixel(27U).b, perimeterPixel(28U).b,
                    service.IsOtaInProgress() ? "wait-for-update-verification"
                        : service.GetOtaLedPresentation().active
                            ? "wait-for-reboot" : "continue-normal-use");
    };
    const char* begin = R"({"cmd":"ota_begin","size":200,"chunk":100})";
    uint8_t bytes[100] = {};
    service.OnOtaControlCharacteristicWrite(begin, std::strlen(begin));
    service.OnOtaDataCharacteristicWrite(bytes, sizeof(bytes));
    service.DrainEventQueue();
    Check(service.GetOtaLedPresentation().percent == 0U,
          "accepted begin yields before queued writes so initial progress can latch");
    Check(service.IsOtaInProgress(), "OTA journey accepts real BLE begin");
    Check(otaStatus.LastTx().find("begin") != std::string::npos,
          "accepted begin emits the client acknowledgement");
    Check(render(false) && visible(),
          "OTA entry renders visible zero percent before ordinary cadence");
    checkPerimeter("begin-ack", 0U);
    const auto initial = frame;
    fxe::ManualMillis::AdvanceMs(200U);
    Check(render(true) && std::memcmp(frame.data(), initial.data(), sizeof(frame)) != 0,
          "central activity pulse moves even while received bytes stay at zero");
    for (uint8_t square = 0U; square < 64U; ++square) {
        if (square / 8U >= 2U && square / 8U <= 5U &&
            square % 8U >= 2U && square % 8U <= 5U) continue;
        const auto targets = hardware::MapSemanticSquareLedTargets(
            revision, square, hardware::SquareLedSlot::Square0, rotate);
        for (uint8_t i = 0U; i < targets.count; ++i) {
            const auto pixel = frame[targets.indices[i]];
            Check(pixel.r == 0U && pixel.g == 0U && pixel.b == 0U,
                  "update activity stays in central four by four, without digits");
        }
    }
    service.DrainEventQueue();
    fxe::ManualMillis::AdvanceMs(200U);
    Check(service.GetOtaLedPresentation().percent == 50U,
          "OTA midpoint derives from authoritative received bytes");
    Check(render(true) && visible() && std::memcmp(frame.data(), initial.data(), sizeof(frame)) != 0,
          "runtime admits changing visible progress during a real transfer");
    checkPerimeter("midpoint", 16U);
    const auto midpoint = leds.latched;
    const uint32_t shownAtMidpoint = leds.shows;
    Check(leds.arbiter.TryBeginFlash(leds.arbiter.CompletedRenderGeneration()),
          "OTA journey holds the flash lease");
    service.OnOtaDataCharacteristicWrite(bytes, 98U);
    service.DrainEventQueue();
    Check(service.GetOtaLedPresentation().percent == 99U,
          "99 percent comes from actual received bytes");
    Check(render(true) && leds.shows == shownAtMidpoint &&
              std::memcmp(leds.latched.data(), midpoint.data(), sizeof(midpoint)) == 0,
          "flash exclusion retains the previous lit frame rather than blanking");
    leds.arbiter.EndFlash();
    Check(render(true) && leds.shows == shownAtMidpoint + 1U && visible(),
          "progress resumes on next admitted frame after flash release");
    checkPerimeter("99-percent", 31U);
    Check(perimeterPixel(27U).b != 0U && perimeterPixel(28U).b != 0U,
          "both USB-side LEDs participate before the final byte arrives");
    service.OnOtaDataCharacteristicWrite(bytes, 2U);
    service.DrainEventQueue();
    Check(render(true),
          "all bytes received fills the final perimeter segment");
    checkPerimeter("all-bytes", 32U);
    send(R"({"cmd":"ota_abort"})");
    Check(!service.GetOtaLedPresentation().active && render(false),
          "OTA abort immediately returns render admission to ordinary work");
    Check(storage.aborts > 0U, "OTA abort reaches the storage boundary");
    checkPerimeter("abort-return", 0U);

    send(begin);
    service.OnOtaDataCharacteristicWrite(bytes, sizeof(bytes));
    service.OnOtaDataCharacteristicWrite(bytes, sizeof(bytes));
    service.DrainEventQueue();
    Check(render(true), "failed-image journey renders its received bytes");
    checkPerimeter("before-invalid-crc", 32U);
    send(R"({"cmd":"ota_end","size":200,"crc32":"00000000"})");
    Check(otaStatus.LastTx().find("checksum_mismatch") != std::string::npos &&
              !service.GetOtaLedPresentation().active,
          "rejected CRC acknowledges failure and releases update LED ownership");
    Check(render(false), "CRC rejection immediately resumes ordinary render admission");
    checkPerimeter("error-return", 0U);

    send(R"({"cmd":"ota_sim_begin","size":200,"chunk":100})");
    Check(service.IsOtaInProgress() && !service.GetOtaLedPresentation().active,
          "benchmark transfer does not own visible update progress");
    send(R"({"cmd":"ota_abort"})");
    send(R"({"cmd":"ota_probe","mode":"flash","size":200})");
    send(R"({"cmd":"ota_begin","size":200,"chunk":100})");
    service.OnOtaDataCharacteristicWrite(bytes, sizeof(bytes));
    service.OnOtaDataCharacteristicWrite(bytes, sizeof(bytes));
    service.DrainEventQueue();
    // CRC computed independently with Python zlib over 200 zero bytes.
    send(R"({"cmd":"ota_end","size":200,"crc32":"c971a876"})");
    Check(otaStatus.LastTx().find("done") != std::string::npos,
          "strict size/CRC completion emits the done acknowledgement");
    Check(!service.IsOtaInProgress() && service.GetOtaLedPresentation().active &&
              service.GetOtaLedPresentation().percent == 100U,
          "completed upload retains progress throughout reboot grace");
    service.OnDisconnected();
    service.DrainEventQueue();
    Check(service.GetOtaLedPresentation().active && render(true) && visible(),
          "post-completion disconnect leaves received progress visible");
    checkPerimeter("done-ack-disconnect-grace", 32U);
    std::puts("[OTA_JOURNEY] abort=ordinary simulation=no cue complete=activity/full-edge disconnect=retained");
}

class OtaRefillingStorage final : public modules::OtaPlatformAdapter {
  public:
    explicit OtaRefillingStorage(modules::BleService& owner) : service(owner) {}
    bool Begin(uint32_t size) override { return size == 24400U; }
    bool Write(const uint8_t* data, size_t length) override {
        for (size_t i = 0U; i < length; ++i) {
            if (data[i] != static_cast<uint8_t>((written + i) / 244U)) ordered = false;
        }
        written += length;
        // A completed SDK operation can overrun the cooperative 20ms budget.
        fxe::ManualMillis::AdvanceMs(75U);
        Refill(16U);
        return true;
    }
    bool End() override { return written == 24400U && ordered; }
    void Abort() override { aborted = true; }
    void Refill(uint32_t count) {
        while (count-- > 0U && sent < 100U) {
            uint8_t bytes[244];
            std::memset(bytes, static_cast<int>(sent), sizeof(bytes));
            service.OnOtaDataCharacteristicWrite(bytes, sizeof(bytes));
            ++sent;
        }
        if (sent == 100U && !endQueued) {
            endQueued = true;
            const char* end = R"({"cmd":"ota_end","size":24400,"crc32":"b9f66f54"})";
            service.OnOtaControlCharacteristicWrite(end, std::strlen(end));
        }
    }
    modules::BleService& service;
    uint32_t sent = 0U;
    size_t written = 0U;
    bool ordered = true, aborted = false, endQueued = false;
};

void TestOtaRefilledIngressYieldsToRendering() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillis::NowMs);
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 100U);
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    OtaRefillingStorage storage(service);
    NimBLECharacteristic status;
    Check(modules::BleServiceConnectionEpochTestPeer::Configure(service, settings, control),
          "refilled OTA queues initialize");
    modules::BleServiceConnectionEpochTestPeer::ConfigureOtaNotifications(service, status);
    service.SetOtaPlatformAdapter(&storage);
    service.OnConnected(7U, "virtual-refill", 247U, 1U);
    service.DrainEventQueue();
    auto command = [&](const char* text) {
        service.OnOtaControlCharacteristicWrite(text, std::strlen(text));
        service.DrainEventQueue();
    };
    command(R"({"cmd":"ota_probe","mode":"flash","size":24400})");
    command(R"({"cmd":"ota_begin","size":24400,"chunk":244,"burst":48})");
    firmware::LedRuntimeAdmission admission;
    Check(admission.Update(service.GetOtaLedPresentation().active, false,
                           fxe::Millis()),
          "refilled upload admits its initial render");
    uint32_t lastFrame = fxe::Millis();
    uint32_t frames = 1U, slices = 0U, maxFrameGap = 0U;
    storage.Refill(32U);
    service.DrainEventQueue();
    Check(service.IsOtaInProgress() && storage.written == 4096U && storage.sent == 48U,
          "one over-budget write yields with accepted refilled tail still queued");
    while (service.IsOtaInProgress() && slices++ < 50U) {
        const uint32_t gap = fxe::Millis() - lastFrame;
        if (admission.Update(service.GetOtaLedPresentation().active,
                             gap >= modules::OTA_LED_REFRESH_MS,
                             fxe::Millis())) {
            if (gap > maxFrameGap) maxFrameGap = gap;
            lastFrame = fxe::Millis();
            ++frames;
        }
        service.DrainEventQueue();
        fxe::ManualMillis::AdvanceMs(5U);
    }
    Check(!service.IsOtaInProgress() && service.GetOtaLedPresentation().active &&
              storage.written == 24400U && storage.ordered && !storage.aborted,
          "bounded slices preserve exact bytes, CRC and queued end ordering");
    Check(status.LastTx().find("done") != std::string::npos,
          "refilled strict transfer completes without queue-full failure");
    Check(frames >= 2U && maxFrameGap <= modules::OTA_LED_REFRESH_MS + 80U,
          "render opportunities survive repeated slow writes and clock wrap");
    std::printf("[OTA_REFILL] bytes=%zu slices=%u render_opportunities=%u max_gap_ms=%u ordered=%u\n",
                storage.written, slices, frames, maxFrameGap, storage.ordered ? 1U : 0U);
}

void TestLitchcraftReplacementConnectionIsolation() {
    using Art = modules::LitchcraftSession;
    enum class Transfer { LegacyStaged, BulkQueued, BulkPartial };
    enum class Reconnect { Replacement, Coalesced, DrainedDisconnect, Unactivated };
    const char* transferNames[] = {"legacy-staged", "bulk-queued", "bulk-partial"};
    const char* reconnectNames[] = {"replacement", "coalesced", "drained-disconnect", "unactivated"};
    for (const auto transfer : {Transfer::LegacyStaged, Transfer::BulkQueued, Transfer::BulkPartial}) {
        for (const auto reconnect : {Reconnect::Replacement, Reconnect::Coalesced, Reconnect::DrainedDisconnect, Reconnect::Unactivated}) {
            fxe::ManualMillis::SetNowMs(100U);
            fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
            modules::SettingsStore settings;
            modules::LitchMaxEngine engine;
            runtime::GameStateMachine state;
            modules::GamePersistenceCoordinator game(settings, engine, state);
            modules::BleService service;
            NimBLECharacteristic characteristic;
            modules::BleServiceConnectionEpochTestPeer::ConfigureLitchcraft(service, game, characteristic);
            auto& art = game.Litchcraft();
            art.Configure(3U, 123U);
            uint8_t enter[] = {Art::MAGIC, uint8_t(Art::Op::Enter), 0, 0, 0, 0, 1, 0};
            art.HandlePacket(enter, sizeof(enter), true, 0U);
            const auto session = art.SessionId();
            const auto revision = art.Revision();
            const auto canvas = art.Artwork();
            const auto undoCount = art.UndoCount();
            auto originalCanvas = [&]() {
                return std::equal(art.Artwork().begin(), art.Artwork().end(), canvas.begin(),
                                  [](const auto& left, const auto& right) {
                                      return left.r == right.r && left.g == right.g && left.b == right.b;
                                  });
            };
            uint16_t request = 2U;
            auto drainReplies = [&]() {
                Art::Packet packet;
                while (art.PeekPacket(packet)) {
                    art.AcceptPacket();
                }
            };
            auto send = [&](const Art::Op op, const std::vector<uint8_t>& payload) {
                std::vector<uint8_t> bytes{Art::MAGIC, uint8_t(op)};
                for (unsigned i = 0U; i < 4U; ++i) {
                    bytes.push_back(session >> (8U * i));
                }
                bytes.push_back(request);
                bytes.push_back(request++ >> 8U);
                bytes.insert(bytes.end(), payload.begin(), payload.end());
                service.OnLitchcraftWrite(bytes.data(), bytes.size());
                service.DrainEventQueue();
            };
            auto enqueueBulk = [&](uint16_t offset, const uint16_t end) {
                for (; offset < end;) {
                    const uint8_t count = std::min<uint16_t>(77U, end - offset);
                    std::vector<uint8_t> bytes{Art::MAGIC, Art::BULK_REPLACE};
                    for (unsigned i = 0U; i < 4U; ++i) {
                        bytes.push_back(session >> (8U * i));
                    }
                    bytes.insert(bytes.end(), {uint8_t(request), uint8_t(request >> 8U),
                                               uint8_t(offset), uint8_t(offset >> 8U),
                                               uint8_t(offset + count == Art::ELEMENT_COUNT), count});
                    bytes.insert(bytes.end(), size_t(count) * 3U, 42U);
                    service.OnOtaDataCharacteristicWrite(bytes.data(), bytes.size());
                    offset += count;
                }
            };
            drainReplies();
            if (transfer == Transfer::BulkQueued) {
                // Exercise the production callback and shared OTA data queue.
                // All four old-generation chunks are pending at replacement.
                enqueueBulk(0U, Art::ELEMENT_COUNT);
            } else if (transfer == Transfer::BulkPartial) {
                enqueueBulk(0U, 77U);
                service.DrainEventQueue();
            } else {
                send(Art::Op::ReplaceBegin, {});
                drainReplies();
                for (uint16_t offset = 0U; offset < Art::ELEMENT_COUNT; offset += 3U) {
                    const uint16_t count = std::min<uint16_t>(3U, Art::ELEMENT_COUNT - offset);
                    std::vector<uint8_t> payload{uint8_t(offset), uint8_t(offset >> 8U)};
                    payload.insert(payload.end(), size_t(count) * 3U, 42U);
                    send(Art::Op::ReplaceChunk, payload);
                    drainReplies();
                }
            }
            if (reconnect != Reconnect::Replacement) {
                service.OnDisconnected();
            }
            if (reconnect == Reconnect::DrainedDisconnect) {
                service.DrainEventQueue();
            }
            if (reconnect == Reconnect::Unactivated) {
                for (unsigned i = 0U; i < modules::BleEventQueue::CAPACITY; ++i) {
                    service.OnConnectionParametersChanged(24U, 0U, 200U, false);
                }
            }
            service.OnConnected(8U, "replacement", 247U, 1U);
            if (reconnect == Reconnect::Unactivated) {
                // The lifecycle projection activates independently of the full
                // command queue, so current-generation bulk ingress remains usable.
                enqueueBulk(0U, Art::ELEMENT_COUNT);
            }
            service.DrainEventQueue();
            if (transfer == Transfer::BulkPartial) {
                enqueueBulk(77U, Art::ELEMENT_COUNT);
                service.DrainEventQueue();
                Art::Packet reply;
                Check(reconnect == Reconnect::Unactivated ||
                          (art.PeekPacket(reply) && reply.bytes[12] == uint8_t(Art::Status::Incomplete)),
                      "new-generation tail cannot finish an old partially drained bulk replacement");
            }
            request = 1U;
            if (transfer == Transfer::LegacyStaged) {
                send(Art::Op::ReplaceCommit, {});
                Art::Packet reply;
                Check(reconnect == Reconnect::Unactivated ||
                          (art.PeekPacket(reply) && reply.bytes[12] == uint8_t(Art::Status::Incomplete)),
                      "replacement generation cannot commit the previous connection's legacy staging");
            }
            if (reconnect == Reconnect::Unactivated) {
                Check(art.IsActive() && art.SessionId() == session &&
                          art.Artwork()[0].r == 42U && art.Artwork()[255].b == 42U &&
                          art.Revision() == revision + 1U && art.UndoCount() == undoCount + 1U,
                      "queue pressure activates the replacement and accepts its current-generation transfer");
            } else {
                Check(art.IsActive() && art.SessionId() == session && art.Revision() == revision &&
                          originalCanvas() && art.UndoCount() == undoCount,
                      "old-connection replacement leaves live session, artwork, revision and history intact");
            }
            drainReplies();
            if (reconnect != Reconnect::Unactivated) {
                enqueueBulk(0U, Art::ELEMENT_COUNT);
                service.DrainEventQueue();
            }
            Check(art.Artwork()[0].r == 42U && art.Artwork()[255].b == 42U &&
                      art.Revision() == revision + 1U && art.UndoCount() == undoCount + 1U,
                  "new connection can replace artwork once after stale ingress or staging cancellation");
            drainReplies();
            if (reconnect == Reconnect::Unactivated) {
                request = 100U;
            }
            ++request;
            send(Art::Op::Undo, {});
            Check(originalCanvas(), "replacement recovery Undo restores the original artwork");
            std::printf("[LITCHCRAFT_REPLACEMENT] transfer=%s reconnect=%s session=%lu revision=%lu\n",
                        transferNames[unsigned(transfer)], reconnectNames[unsigned(reconnect)], static_cast<unsigned long>(art.SessionId()),
                        static_cast<unsigned long>(art.Revision()));
        }
    }
}

void TestLitchcraftReplacementConnectionFinishesAppStroke() {
    using Art = modules::LitchcraftSession;
    modules::SettingsStore settings;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(settings, engine, state);
    modules::BleService service;
    NimBLECharacteristic characteristic;
    modules::BleServiceConnectionEpochTestPeer::ConfigureLitchcraft(service, game, characteristic);
    auto& art = game.Litchcraft();
    art.Configure(3U, 123U);
    uint8_t enter[] = {Art::MAGIC, uint8_t(Art::Op::Enter), 0, 0, 0, 0, 1, 0};
    art.HandlePacket(enter, sizeof(enter), true, 0U);
    uint8_t paint[] = {Art::MAGIC, uint8_t(Art::Op::Paint), 0, 0, 0, 0, 2, 0, 1, 0, 1, 0};
    for (unsigned i = 0U; i < 4U; ++i) {
        paint[2U + i] = art.SessionId() >> (8U * i);
    }
    service.OnLitchcraftWrite(paint, sizeof(paint));
    service.DrainEventQueue();
    Check(art.UndoCount() == 1U && art.Artwork()[0].r != 0U,
          "old connection has a live painted app stroke available to Undo");
    const auto revision = art.Revision();
    service.OnDisconnected();
    service.OnConnected(8U, "replacement", 247U, 1U);
    service.DrainEventQueue();
    Check(art.UndoCount() == 1U && art.Artwork()[0].r != 0U && art.Revision() == revision,
          "coalesced reconnect retains interrupted app painting as one completed Undo operation");
    paint[6] = 1U;
    paint[10] = 2U;
    paint[11] = 1U;
    service.OnLitchcraftWrite(paint, sizeof(paint));
    service.DrainEventQueue();
    Check(art.UndoCount() == 2U,
          "reused app stroke ID on the replacement connection cannot merge with the retired stroke");
    paint[1] = uint8_t(Art::Op::Undo);
    paint[6] = 2U;
    service.OnLitchcraftWrite(paint, 8U);
    service.DrainEventQueue();
    Check(art.Artwork()[0].r != 0U && art.Artwork()[4].r == 0U,
          "first Undo removes only painting from the replacement connection");
    paint[6] = 3U;
    service.OnLitchcraftWrite(paint, 8U);
    service.DrainEventQueue();
    Check(art.Artwork()[0].r == 0U && art.UndoCount() == 0U,
          "second Undo removes the retained interrupted stroke");
}

void TestLitchcraftV2ProductionHighMtuIngressAndEpochs() {
    using Art = modules::LitchcraftSession;
    using Persistence = modules::LitchcraftPersistence;
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    modules::SettingsStore settings;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(settings, engine, state);
    modules::BleService service;
    NimBLECharacteristic characteristic;
    modules::BleServiceConnectionEpochTestPeer::ConfigureLitchcraft(service, game, characteristic);
    auto& art = game.Litchcraft();
    art.Configure(3U, 123U);
    uint8_t enter[] = {Art::MAGIC, 1U, 0U, 0U, 0U, 0U, 1U, 0U};
    art.HandlePacket(enter, sizeof(enter), true, 0U);
    Art::BulkPacket ignored;
    while (art.PeekPacket(ignored, 244U)) art.AcceptPacket();
    auto& persistence = game.ArtPersistence();
    Persistence::AdvancedStorage storage;
    Persistence::Ops ops;
    ops.readAdvanced = [](void*, Persistence::AdvancedSlot, uint8_t*, size_t, size_t&) {
        return Persistence::AdvancedReadOutcome::Missing;
    };
    ops.enqueueAdvanced = [](void*, const uint8_t*, size_t, uint32_t&) {
        return fxe::SettingsStore::AsyncSaveAdmission::Deferred;
    };
    ops.poll = [](void*, uint32_t) { return Persistence::SaveOutcome::Pending; };
    ops.release = [](void*, uint32_t) {};
    persistence.Configure(ops);
    persistence.ConfigureAdvancedStorage(storage);
    auto& document = storage.staging;
    for (uint8_t i = 0U; i < 8U; ++i) document.favorites[i] = {i, 10U, 20U};
    Check(modules::litchcraft_animation::ValidatedDocument::TryAdoptBySwap(document, storage.document),
          "v2 production fixture admits bounded authority");
    modules::litchcraft_animation::EncodedDocument encoded;
    Check(modules::litchcraft_animation::EncodeDocument(storage.document, encoded), "v2 production canonical fixture");
    auto put = [](std::vector<uint8_t>& bytes, uint32_t value, size_t count) {
        for (size_t i = 0U; i < count; ++i) bytes.push_back(value >> (i * 8U));
    };
    auto request = [&](uint8_t op, uint16_t id) {
        std::vector<uint8_t> bytes{0x4dU, 2U, op};
        put(bytes, art.SessionId(), 4U); put(bytes, id, 2U);
        return bytes;
    };
    auto begin = request(9U, 2U);
    put(begin, 0U, 4U); put(begin, encoded.size, 2U); put(begin, encoded.crc32, 4U);
    auto send = [&](const std::vector<uint8_t>& bytes) {
        service.OnLitchcraftWrite(bytes.data(), bytes.size());
        service.DrainEventQueue();
    };
    send(begin);
    Check(persistence.AdvancedTransportStorage() != nullptr, "char0106 callback-to-coordinator admits v2 Begin");
    modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);
    auto chunk = request(10U, 3U); put(chunk, 2U, 2U); put(chunk, 0U, 2U);
    chunk.insert(chunk.end(), encoded.bytes.begin(), encoded.bytes.begin() + 231U);
    Check(chunk.size() == 244U, "production ingress probes maximum negotiated payload");
    send(chunk);
    Check(std::equal(encoded.bytes.begin(), encoded.bytes.begin() + 231U, storage.envelope.bytes.begin()),
          "all 231 data bytes survive real callback union queue dispatcher and session");
    Art::BulkPacket frozenAck;
    Check(art.PeekPacket(frozenAck, 244U) && frozenAck.bytes[13] == 0U, "high MTU chunk accepted");
    characteristic.SetNotifyResult(false);
    modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);
    Art::BulkPacket retryAck;
    Check(art.PeekPacket(retryAck, 244U) && retryAck.bytes == frozenAck.bytes,
          "v2 notify failure retains exact ACK at production seam");
    characteristic.SetNotifyResult(true);
    modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);
    // A queued old chunk and disconnect must not mutate or disable the new
    // connection, even when callbacks reuse the same physical connection handle.
    auto oldChunk = request(10U, 4U); put(oldChunk, 2U, 2U); put(oldChunk, 231U, 2U);
    oldChunk.push_back(99U);
    service.OnLitchcraftWrite(oldChunk.data(), oldChunk.size());
    service.OnDisconnected();
    service.OnConnected(7U, "replacement", 247U, 1U);
    service.OnLitchcraftSubscription(1U);
    service.DrainEventQueue();
    Check(persistence.AdvancedTransportStorage() == nullptr && storage.envelope.bytes[231U] != 99U,
          "stale v2 queued bytes rejected and replacement connection retires old lease");
    begin[7] = 1U;
    send(begin);
    Check(persistence.AdvancedTransportStorage() != nullptr, "fresh request space opens new lease after old cleanup");
    modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);
    chunk[7] = 2U; chunk[9] = 1U;
    send(chunk);
    Check(art.PeekPacket(retryAck, 244U) && retryAck.bytes[13] == 0U,
          "replacement connection accepts high MTU chunks after stale lifecycle drain");
    modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);
    auto cancel = request(12U, 3U); put(cancel, 1U, 2U); send(cancel);
    Check(persistence.AdvancedTransportStorage() == nullptr, "production Cancel returns shared body to persistence");
    modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);

    auto streamBegin = request(14U, 4U);
    put(streamBegin, persistence.AdvancedGeneration(), 4U);
    put(streamBegin, encoded.size, 2U);
    put(streamBegin, encoded.crc32, 4U);
    send(streamBegin);
    modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);
    Check(persistence.AdvancedTransportStorage() != nullptr,
          "production streamed Begin acquires the transport body");

    auto streamChunk = request(15U, 5U);
    put(streamChunk, 4U, 2U);
    put(streamChunk, 0U, 2U);
    streamChunk.insert(
        streamChunk.end(), encoded.bytes.begin(), encoded.bytes.begin() + 32U);
    auto checkpoint = request(17U, 5U);
    put(checkpoint, 4U, 2U);
    put(checkpoint, 32U, 2U);
    auto erase = std::vector<uint8_t>{Art::MAGIC, uint8_t(Art::Op::Tool)};
    put(erase, art.SessionId(), 4U);
    put(erase, 6U, 2U);
    erase.insert(erase.end(), {1U, 15U, 9U, 8U, 7U});
    service.OnOtaDataCharacteristicWrite(streamChunk.data(), streamChunk.size());
    service.OnOtaDataCharacteristicWrite(checkpoint.data(), checkpoint.size());
    service.OnLitchcraftWrite(erase.data(), erase.size());

    modules::BleServiceConnectionEpochTestPeer::LimitNextIngressDrainToOne(service);
    service.DrainEventQueue();
    Check(!art.IsErasing(),
          "real OTA-data chunk remains ahead of its accepted checkpoint barrier");
    modules::BleServiceConnectionEpochTestPeer::LimitNextIngressDrainToOne(service);
    service.DrainEventQueue();
    Art::BulkPacket checkpointAck;
    Check(!art.IsErasing() && art.PeekPacket(checkpointAck, 244U) &&
              checkpointAck.bytes[0] == 0x4dU && checkpointAck.bytes[2] == 0x81U &&
              checkpointAck.bytes[11] == 5U && checkpointAck.bytes[12] == 0U &&
              checkpointAck.bytes[13] == uint8_t(Art::Status::Ok),
          "real OTA-data checkpoint is authoritative before later compact Tool");
    art.AcceptPacket();
    modules::BleServiceConnectionEpochTestPeer::LimitNextIngressDrainToOne(service);
    service.DrainEventQueue();
    Check(art.IsErasing(),
          "later compact Tool runs immediately after the accepted barrier");

    auto unrelatedChunk = request(15U, 7U);
    put(unrelatedChunk, 4U, 2U);
    put(unrelatedChunk, 32U, 2U);
    unrelatedChunk.push_back(encoded.bytes[32U]);
    auto draw = erase;
    draw[6] = 7U;
    draw[8] = 0U;
    service.OnOtaDataCharacteristicWrite(
        unrelatedChunk.data(), unrelatedChunk.size());
    service.OnLitchcraftWrite(draw.data(), draw.size());
    modules::BleServiceConnectionEpochTestPeer::LimitNextIngressDrainToOne(service);
    service.DrainEventQueue();
    Check(!art.IsErasing(),
          "compact Tool still bypasses unrelated accepted OTA-data bulk");
    service.DrainEventQueue();
    modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);

    auto finalChunk = request(15U, 8U);
    put(finalChunk, 4U, 2U);
    put(finalChunk, 33U, 2U);
    finalChunk.push_back(encoded.bytes[33U]);
    auto commit = request(16U, 8U);
    put(commit, 4U, 2U);
    auto eraseAfterCommit = erase;
    eraseAfterCommit[6] = 9U;
    service.OnOtaDataCharacteristicWrite(finalChunk.data(), finalChunk.size());
    service.OnOtaDataCharacteristicWrite(commit.data(), commit.size());
    service.OnLitchcraftWrite(eraseAfterCommit.data(), eraseAfterCommit.size());
    modules::BleServiceConnectionEpochTestPeer::LimitNextIngressDrainToOne(service);
    service.DrainEventQueue();
    Check(!art.IsErasing(),
          "real OTA-data chunk remains ahead of its accepted commit barrier");
    modules::BleServiceConnectionEpochTestPeer::LimitNextIngressDrainToOne(service);
    service.DrainEventQueue();
    Art::BulkPacket commitAck;
    Check(!art.IsErasing() && art.PeekPacket(commitAck, 244U) &&
              commitAck.bytes[0] == 0x4dU && commitAck.bytes[2] == 0x81U &&
              commitAck.bytes[11] == 8U && commitAck.bytes[12] == 0U &&
              commitAck.bytes[13] == uint8_t(Art::Status::Incomplete),
          "real OTA-data commit reaches authority before later compact Tool");
    art.AcceptPacket();
    modules::BleServiceConnectionEpochTestPeer::LimitNextIngressDrainToOne(service);
    service.DrainEventQueue();
    Check(art.IsErasing(),
          "later compact Tool runs immediately after the accepted commit");
    std::puts("[LITCHCRAFT_V2_BLE] payload=244 exact_ack=1 stale_epoch=1 reused_handle=1 lease_cleanup=1 checkpoint_barrier=1 commit_barrier=1 compact_bypass=1");
}

void TestLitchcraftExactNotificationsAndEpochs() {
    using Art = modules::LitchcraftSession;
    modules::SettingsStore settings;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(settings, engine, state);
    modules::BleService service;
    NimBLECharacteristic characteristic;
    modules::BleServiceConnectionEpochTestPeer::ConfigureLitchcraft(service, game, characteristic);
    auto& art = game.Litchcraft();
    art.Configure(3U, 123U);
    uint8_t enter[] = {Art::MAGIC, 1, 0, 0, 0, 0, 1, 0};
    art.HandlePacket(enter, sizeof(enter), true, 0U);
    Art::Packet ignored;
    while (art.PeekPacket(ignored))
        art.AcceptPacket();
    uint8_t tool[] = {Art::MAGIC, 3, 0, 0, 0, 0, 2, 0, 0, 15, 8, 12, 19};
    for (uint8_t i = 0; i < 4U; ++i)
        tool[2U + i] = art.SessionId() >> (8U * i);
    service.OnLitchcraftWrite(tool, sizeof(tool));
    service.DrainEventQueue();
    const auto revision = art.Revision();
    Art::Packet retained;
    Check(art.PeekPacket(retained), "live tool mutation creates a typed notification");
    characteristic.SetNotifyResult(false);
    modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);
    Art::Packet retry;
    Check(art.PeekPacket(retry) && retained.bytes == retry.bytes &&
              characteristic.NotifyCount() == 0U,
          "Litchcraft exact-payload rejection retains unsent head without losing stroke events");
    characteristic.SetNotifyResult(true);
    modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);
    Check(characteristic.LastConnHandle() == 7U, "Litchcraft notifications target the owned connection handle");
    service.OnLitchcraftWrite(tool, sizeof(tool));
    service.DrainEventQueue();
    Check(art.Revision() == revision, "queued duplicate Litchcraft mutation applies once");
    tool[6] = 3U;
    tool[10] = 44U;
    service.OnLitchcraftWrite(tool, sizeof(tool));
    service.OnDisconnected();
    service.OnConnected(7U, "replacement", 23U, 1U);
    service.OnLitchcraftSubscription(1U); // CCCD can arrive before lifecycle drain.
    service.DrainEventQueue();
    Check(art.Revision() == revision && art.IsActive(), "old-generation queued paint is dropped while entered mode survives reconnect");
    tool[6] = 1U;
    service.OnLitchcraftWrite(tool, sizeof(tool));
    service.DrainEventQueue();
    Check(art.Color().r == 44U, "new BLE generation restarts operation IDs without replacing artwork");
    while (art.PeekPacket(ignored))
        art.AcceptPacket();
    uint8_t snapshot[] = {Art::MAGIC, 8, 0, 0, 0, 0, 2, 0};
    service.OnLitchcraftWrite(snapshot, sizeof(snapshot));
    service.DrainEventQueue();
    struct Capture {
        NimBLECharacteristic* characteristic;
        std::vector<std::string> frames;
    } capture{&characteristic, {}};
    characteristic.SetNotifyHook([](void* value) {
        auto& capture = *static_cast<Capture*>(value);
        capture.frames.push_back(capture.characteristic->LastTx());
    },
                                 &capture);
    unsigned loops = 0U;
    while (art.PeekPacket(ignored) && loops++ < 30U)
        modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);
    characteristic.SetNotifyHook(nullptr);
    Check(capture.frames.size() == 133U,
          "minimum-MTU BLE snapshot sends each exact ACK/begin/tool/chunk/end plus gesture packet");
    bool ordered = capture.frames.size() == 133U;
    for (size_t i = 3U; i < 131U && ordered; ++i) {
        const auto& frame = capture.frames[i];
        const auto offset = uint8_t(frame[12]) | (uint16_t(uint8_t(frame[13])) << 8U);
        ordered = frame.size() == 20U && uint8_t(frame[1]) == uint8_t(Art::Event::SnapshotChunk) && offset == (i - 3U) * 2U;
    }
    ordered = ordered &&
              uint8_t(capture.frames[132U][1]) ==
                  uint8_t(Art::Event::ColorPick) &&
              uint8_t(capture.frames[132U][12]) == 0U;
    Check(ordered, "consecutive BLE snapshot chunks retain independent payloads and ascending offsets");
    std::printf("[LITCHCRAFT_BLE] packets=%zu mtu=23 exact_payload=1 rejection_retained=1 epoch_isolated=1\n", capture.frames.size());
}

void TestSchema4RequiresConnectionSessionIngress() {
    struct Probe final {
        uint32_t ingressCalls = 0U;
        uint32_t acquiredPackets = 0U;
        uint32_t priorityCalls = 0U;
        uint32_t acceptedResponses = 0U;
        uint32_t ownerEpoch = 0U;
        modules::litchcraft_frame_store::StoreProjection projection{};
        bool responseReady = false;
        std::array<uint8_t, 19U> response{};
    } probe;
    probe.response[0] = 0x4dU;
    probe.response[1] = 2U;
    probe.response[2] = 0x8dU;

    modules::SettingsStore settings;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(settings, engine, state);
    modules::BleService service;
    NimBLECharacteristic characteristic;
    modules::BleServiceConnectionEpochTestPeer::ConfigureLitchcraft(
        service, game, characteristic);
    game.Litchcraft().Configure(3U, 123U);
    uint8_t enter[] = {modules::LitchcraftSession::MAGIC, 1U, 0U, 0U,
                       0U, 0U, 1U, 0U};
    game.Litchcraft().HandlePacket(enter, sizeof(enter), true, 0U);
    modules::LitchcraftSession::Packet queued;
    while (game.Litchcraft().PeekPacket(queued)) {
        game.Litchcraft().AcceptPacket();
    }
    service.SetLitchcraftFrameIngressCallbacks(
        [](void* context, modules::BleOtaChunkQueue& queue,
           const modules::litchcraft_frame_store::StoreProjection projection,
           const uint32_t ownerEpoch, const uint16_t) {
            auto& value = *static_cast<Probe*>(context);
            ++value.ingressCalls;
            value.projection = projection;
            value.ownerEpoch = ownerEpoch;
            const uint8_t* bytes = nullptr;
            size_t size = 0U;
            uint32_t ordinal = 0U;
            uint32_t epoch = 0U;
            if (queue.TryAcquireReadSlot(
                    bytes, size, ordinal, epoch)) {
                ++value.acquiredPackets;
                queue.ReleaseReadSlot();
            }
        },
        [](void* context, const uint8_t*, const size_t size,
           const uint32_t ownerEpoch) {
            auto& value = *static_cast<Probe*>(context);
            if (size == 9U && ownerEpoch != 0U) ++value.priorityCalls;
        },
        [](void* context, uint8_t* out, const size_t capacity,
           size_t& outSize) {
            auto& value = *static_cast<Probe*>(context);
            if (!value.responseReady || capacity < value.response.size()) {
                outSize = 0U;
                return false;
            }
            std::copy(value.response.begin(), value.response.end(), out);
            outSize = value.response.size();
            return true;
        },
        [](void* context) {
            auto& value = *static_cast<Probe*>(context);
            value.responseReady = false;
            ++value.acceptedResponses;
        },
        [](void*, uint16_t, uint32_t) { return false; },
        &probe);

    std::array<uint8_t, 10U> inspect{{0x4dU, 2U, 1U, 0U, 0U,
                                      0U, 0U, 7U, 0U, 4U}};
    const uint32_t session = game.Litchcraft().SessionId();
    for (uint8_t index = 0U; index < 4U; ++index) {
        inspect[3U + index] = static_cast<uint8_t>(session >> (index * 8U));
    }
    service.OnLitchcraftWrite(inspect.data(), inspect.size());
    service.DrainEventQueue();
    Check(probe.acquiredPackets == 0U,
          "legacy Litchcraft Inspect cannot bypass the shared Object stream");

    std::array<uint8_t, 53U> begin{};
    begin[0U] = 0x4dU;
    begin[1U] = 2U;
    begin[2U] = 18U;
    std::copy(inspect.begin() + 3U, inspect.begin() + 7U,
              begin.begin() + 3U);
    Check(!service.OnOtaDataCharacteristicWrite(begin.data(), begin.size()),
          "legacy OTA-data Store ingress is rejected before the shared queue");
    service.DrainEventQueue();
    Check(probe.acquiredPackets == 0U,
          "rejected direct Store ingress cannot wake the Store worker");

    std::array<uint8_t, 9U> cancel{{0x4dU, 2U, 22U, 0U, 0U,
                                    0U, 0U, 7U, 0U}};
    std::copy(inspect.begin() + 3U, inspect.begin() + 7U,
              cancel.begin() + 3U);
    service.OnLitchcraftWrite(cancel.data(), cancel.size());
    service.DrainEventQueue();
    Check(probe.priorityCalls == 0U && probe.acquiredPackets == 0U,
          "legacy Litchcraft Cancel cannot bypass connection Ordered ingress");

    modules::BleServiceConnectionEpochTestPeer::FlushLitchcraft(service);
    Check(probe.acceptedResponses == 0U,
          "Store receipts remain owned by an activated connection Object stream");

    constexpr const char* simBegin =
        R"({"cmd":"ota_sim_begin","size":53,"chunk":53})";
    service.OnOtaControlCharacteristicWrite(simBegin, std::strlen(simBegin));
    service.DrainEventQueue();
    Check(service.IsOtaInProgress() &&
              service.OnOtaDataCharacteristicWrite(begin.data(), begin.size()),
          "active firmware update keeps coincidentally Store-shaped image bytes opaque");
}

void TestFrameStoreResidentResponseFitsMinimumObjectPackets() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    struct Probe final {
        std::array<uint8_t, 106U> response{};
        bool ready = true;
        uint8_t accepted = 0U;
    } probe;
    probe.response[0U] = 0x4dU;
    probe.response[1U] = 2U;
    probe.response[2U] = 0x92U;
    fxe::WriteU16LE(7U, probe.response.data() + 11U);
    probe.response[15U] = 2U;
    for (size_t index = 16U; index < probe.response.size(); ++index) {
        probe.response[index] = static_cast<uint8_t>(index);
    }
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    Check(Peer::Configure(service, settings, control) &&
              Peer::ActivateMinimumConnectionObjectSession(service),
          "minimum-payload Store fixture activates one shared session");
    service.SetLitchcraftFrameIngressCallbacks(
        [](void*, modules::BleOtaChunkQueue&,
           modules::litchcraft_frame_store::StoreProjection, uint32_t,
           uint16_t) {},
        [](void*, const uint8_t*, size_t, uint32_t) {},
        [](void* context, uint8_t* out, size_t capacity, size_t& outSize) {
            auto& value = *static_cast<Probe*>(context);
            if (!value.ready || capacity < value.response.size()) return false;
            std::copy(value.response.begin(), value.response.end(), out);
            outSize = value.response.size();
            return true;
        },
        [](void* context) {
            auto& value = *static_cast<Probe*>(context);
            value.ready = false;
            ++value.accepted;
        },
        [](void*, uint16_t, uint32_t) { return false; }, &probe);
    std::vector<uint8_t> reconstructed;
    uint16_t fragments = 0U;
    while (probe.accepted == 0U && fragments < 20U) {
        std::vector<uint8_t> fragment;
        if (!Peer::TakeSharedLitchcraftFragment(service, fragment, true)) {
            break;
        }
        const bool begins = (fragment[1U] & 1U) != 0U;
        const size_t headerSize = begins ? 4U : 2U;
        Check(fragment.size() <= 10U && fragment.size() > headerSize,
              "each Store receipt fragment fits the 20-byte ATT payload");
        if (begins) {
            Check(fxe::ReadU16LE(fragment.data() + 2U) ==
                      probe.response.size(),
                  "first Store fragment declares the exact response length");
        }
        reconstructed.insert(reconstructed.end(),
                             fragment.begin() + headerSize, fragment.end());
        ++fragments;
    }
    Check(probe.accepted == 1U && fragments > 4U &&
              reconstructed == std::vector<uint8_t>(
                  probe.response.begin(), probe.response.end()),
          "two-resident Store response completes exactly once across minimum packets");
#endif
}

void TestFrameStoreSnapshotFillsReverseCreditsInOneUpdate() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    struct Probe final {
        std::array<uint8_t, 106U> response{};
    } probe;
    probe.response[0U] = 0x4dU;
    probe.response[1U] = 2U;
    probe.response[2U] = 0x93U;
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    Check(Peer::Configure(service, settings, control) &&
              Peer::ActivateMinimumConnectionObjectSession(service),
          "reverse burst fixture activates shared session");
    service.SetLitchcraftFrameIngressCallbacks(
        [](void*, modules::BleOtaChunkQueue&,
           modules::litchcraft_frame_store::StoreProjection, uint32_t,
           uint16_t) {},
        [](void*, const uint8_t*, size_t, uint32_t) {},
        [](void* context, uint8_t* out, size_t capacity, size_t& outSize) {
            auto& value = *static_cast<Probe*>(context);
            if (capacity < value.response.size()) return false;
            std::copy(value.response.begin(), value.response.end(), out);
            outSize = value.response.size();
            return true;
        },
        [](void*) {},
        [](void*, uint16_t, uint32_t) { return false; }, &probe);
    Check(Peer::OneUpdateFillsReverseWindow(service),
          "one board service pass fills every available reverse Object credit");
#endif
}

void TestSharedV2InspectRoutesByCapability() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    auto sendInspect = [](modules::BleService& service, uint8_t capability) {
        const std::array<uint8_t, 10U> inspect{{
            0x4dU, 2U, 1U, 0U, 0U, 0U, 0U, 7U, 0U, capability}};
        const std::array<uint8_t, 10U> first{{
            1U, 1U, 10U, 0U, inspect[0U], inspect[1U], inspect[2U],
            inspect[3U], inspect[4U], inspect[5U]}};
        const std::array<uint8_t, 6U> last{{
            1U, 2U, inspect[6U], inspect[7U], inspect[8U], inspect[9U]}};
        return Peer::HandleConnectionLitchcraft(
                   service, first.data(), first.size(), 71U) &&
            Peer::HandleConnectionLitchcraft(
                service, last.data(), last.size(), 72U);
    };

    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService v2;
    Check(Peer::Configure(v2, settings, control) && sendInspect(v2, 1U),
          "effect-clock Inspect is admitted through shared Object");
    std::vector<uint8_t> received;
    uint32_t ordinal = 0U;
    uint32_t epoch = 0U;
    Check(Peer::TakeLitchcraftEvent(v2, received, ordinal, epoch) &&
              received == std::vector<uint8_t>({
                  0x4dU, 2U, 1U, 0U, 0U, 0U, 0U, 7U, 0U, 1U}) &&
              ordinal == 72U && epoch == 7U,
          "effect-clock Inspect reaches the V2 document owner, not Store ingress");

    modules::BleService store;
    Check(Peer::Configure(store, settings, control) && sendInspect(store, 5U),
          "frame-store capability Inspect is admitted through shared Object");
    received.clear();
    Check(Peer::TakeOtaData(store, received, ordinal) &&
              received == std::vector<uint8_t>({
                  0x4dU, 2U, 1U, 0U, 0U, 0U, 0U, 7U, 0U, 5U}) &&
              ordinal == 72U,
          "frame-store capability Inspect reaches the Store worker queue");
    Check(sendInspect(store, 7U),
          "resident-listing capability Inspect is admitted through shared Object");
    received.clear();
    Check(Peer::TakeOtaData(store, received, ordinal) &&
              received == std::vector<uint8_t>({
                  0x4dU, 2U, 1U, 0U, 0U, 0U, 0U, 7U, 0U, 7U}) &&
              ordinal == 72U,
          "resident-listing Inspect reaches the Store worker queue");
#endif
}

void TestMinimumObjectFragmentsReachStoreWorkerOnlyWhenComplete() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    Check(Peer::Configure(service, settings, control) &&
              Peer::ActivateMinimumConnectionObjectSession(service),
          "minimum-payload Store ingress fixture activates shared Object");
    const std::array<uint8_t, 18U> select{{
        0x4dU, 2U, 26U, 1U, 0U, 0U, 0U, 7U, 0U,
        1U, 2U, 0U, 0U, 0U, 3U, 0U, 0U, 0U}};
    const std::array<uint8_t, 10U> first{{
        1U, 1U, 18U, 0U, select[0U], select[1U], select[2U],
        select[3U], select[4U], select[5U]}};
    const std::array<uint8_t, 10U> middle{{
        1U, 0U, select[6U], select[7U], select[8U], select[9U],
        select[10U], select[11U], select[12U], select[13U]}};
    const std::array<uint8_t, 6U> last{{
        1U, 2U, select[14U], select[15U], select[16U], select[17U]}};
    Check(Peer::HandleConnectionLitchcraft(
              service, first.data(), first.size(), 71U) &&
              Peer::HandleConnectionLitchcraft(
                  service, middle.data(), middle.size(), 72U),
          "Store Select fragments stage without exposing a partial request");
    std::vector<uint8_t> queued;
    uint32_t ordinal = 0U;
    Check(!Peer::TakeOtaData(service, queued, ordinal),
          "Store worker receives nothing before final fragment");
    Check(Peer::HandleConnectionLitchcraft(
              service, last.data(), last.size(), 73U) &&
              Peer::TakeOtaData(service, queued, ordinal) &&
              queued == std::vector<uint8_t>(select.begin(), select.end()) &&
              ordinal == 73U,
          "complete minimum-packet Select reaches Store worker exactly once");
#endif
}

void TestSharedGameHistoryFragmentsCommitOneExactFrame() {
    struct Probe final {
        std::array<uint8_t, 20U> bytes{};
        size_t size = 0U;
        uint32_t epoch = 0U;
        uint32_t calls = 0U;
        bool admit = false;
    } probe;
    modules::BleService service;
    service.SetConnectionGameHistoryIngressCallback(
        [](void* context, const uint8_t* bytes, const size_t size,
           const uint32_t epoch, const bool) {
            auto& value = *static_cast<Probe*>(context);
            ++value.calls;
            value.size = size;
            value.epoch = epoch;
            std::copy(bytes, bytes + size, value.bytes.begin());
            return value.admit;
        },
        &probe);
    std::array<uint8_t, 20U> frame{};
    for (uint8_t index = 0U; index < frame.size(); ++index) {
        frame[index] = static_cast<uint8_t>(index + 1U);
    }
    const std::array<uint8_t, 10U> first{{
        1U, 1U, 20U, 0U, frame[0U], frame[1U], frame[2U], frame[3U],
        frame[4U], frame[5U]}};
    const std::array<uint8_t, 10U> middle{{
        1U, 0U, frame[6U], frame[7U], frame[8U], frame[9U], frame[10U],
        frame[11U], frame[12U], frame[13U]}};
    const std::array<uint8_t, 8U> last{{
        1U, 2U, frame[14U], frame[15U], frame[16U], frame[17U],
        frame[18U], frame[19U]}};
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    Check(Peer::HandleConnectionGameHistoryFragment(
              service, first.data(), first.size()) &&
              Peer::HandleConnectionGameHistoryFragment(
                  service, middle.data(), middle.size()) &&
              probe.calls == 0U,
          "partial shared history fragments stay invisible to the owner");
    Check(!Peer::HandleConnectionGameHistoryFragment(
              service, last.data(), last.size()) &&
              probe.calls == 1U && probe.size == frame.size() &&
              probe.epoch == 7U && probe.bytes == frame,
          "unadmitted complete history frame remains retryable");
    probe.admit = true;
    Check(Peer::HandleConnectionGameHistoryFragment(
              service, last.data(), last.size()) &&
              probe.calls == 2U && probe.bytes == frame,
          "exact final fragment commits once the history owner admits it");

    std::array<uint8_t, 24U> single{};
    single[0U] = 1U;
    single[1U] = 3U;
    single[2U] = static_cast<uint8_t>(frame.size());
    std::copy(frame.begin(), frame.end(), single.begin() + 4U);
    probe.admit = false;
    Check(!Peer::HandleConnectionGameHistoryFragment(service, single.data(), single.size()),
          "unadmitted high-MTU Begin+End remains retryable");
    probe.admit = true;
    const unsigned callsBeforeRetry = probe.calls;
    Check(Peer::HandleConnectionGameHistoryFragment(service, single.data(), single.size()) &&
              probe.calls == callsBeforeRetry + 1U && probe.bytes == frame,
          "exact single-fragment history retry reaches its production owner without an extra failed attempt");

    const std::array<uint8_t, 5U> malformed{{1U, 3U, 8U, 0U, 1U}};
    Check(!Peer::HandleConnectionGameHistoryFragment(
              service, malformed.data(), malformed.size()),
          "early End discards malformed shared history staging");
    Check(Peer::HandleConnectionGameHistoryFragment(
              service, first.data(), first.size()),
          "new Begin recovers after malformed history staging");
}

void TestSharedOtaDataAdmitsOneExactChunk() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    Check(Peer::Configure(service, settings, control),
          "shared OTA data fixture initializes bounded ingress");
    const std::array<uint8_t, 10U> chunk{{
        0xe9U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U}};
    Check(Peer::HandleConnectionOtaData(
              service, chunk.data(), chunk.size(), 37U),
          "shared OTA Object admits one exact raw chunk");
    std::vector<uint8_t> received;
    uint32_t ordinal = 0U;
    Check(Peer::TakeOtaData(service, received, ordinal) &&
              received == std::vector<uint8_t>(chunk.begin(), chunk.end()) &&
              ordinal == 37U,
          "shared OTA Object preserves exact bytes and ingress order");
    Check(!Peer::HandleConnectionOtaData(service, chunk.data(), 0U),
          "shared OTA Object rejects an empty chunk");
#endif
}

void TestSharedOtaControlCommitsOneOrderedExistingEvent() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    Check(Peer::Configure(service, settings, control),
          "shared OTA control fixture initializes bounded ingress");
    const std::string command = R"({"cmd":"ota_begin"})";
    std::vector<std::vector<uint8_t>> fragments;
    size_t offset = 0U;
    bool first = true;
    while (offset < command.size()) {
        const size_t header = first ? 4U : 2U;
        const size_t count = std::min<size_t>(10U - header,
                                             command.size() - offset);
        const bool last = offset + count == command.size();
        std::vector<uint8_t> fragment(header + count, 0U);
        fragment[0U] = 1U;
        fragment[1U] = (first ? 1U : 0U) | (last ? 2U : 0U);
        if (first) {
            fragment[2U] = static_cast<uint8_t>(command.size() & 0xffU);
            fragment[3U] = static_cast<uint8_t>(command.size() >> 8U);
        }
        std::copy_n(command.data() + offset, count,
                    fragment.data() + header);
        fragments.push_back(fragment);
        offset += count;
        first = false;
    }
    for (size_t index = 0U; index + 1U < fragments.size(); ++index) {
        Check(Peer::HandleConnectionOtaControl(
                  service, fragments[index].data(), fragments[index].size(),
                  static_cast<uint32_t>(40U + index), 9U),
              "partial shared OTA control fragment is admitted");
    }
    Check(!Peer::HasQueuedEvent(service),
          "partial shared OTA control remains invisible");
    const uint32_t finalOrdinal =
        static_cast<uint32_t>(40U + fragments.size() - 1U);
    const auto& last = fragments.back();
    Check(Peer::HandleConnectionOtaControl(
              service, last.data(), last.size(), finalOrdinal, 9U),
          "final shared OTA control fragment commits the existing event");
    std::string received;
    uint32_t ordinal = 0U;
    uint32_t sessionEpoch = 0U;
    uint32_t connectionEpoch = 0U;
    Check(Peer::TakeOtaControl(service, received, ordinal, sessionEpoch,
                              connectionEpoch) &&
              received == command && ordinal == finalOrdinal &&
              sessionEpoch == 2U && connectionEpoch == 9U,
          "shared OTA control preserves bytes, Object order, and OTA epoch");
#endif
}

void TestSharedOtaStatusFragmentsAtMinimumMtu() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    modules::BleService service;
    Check(Peer::ActivateMinimumConnectionObjectSession(service),
          "shared OTA status fixture activates a minimum-MTU Object stream");
    NimBLECharacteristic legacy;
    constexpr const char* json =
        R"({"status":"progress","percent":42})";
    Peer::NotifyOtaJson(service, legacy, json);
    Check(legacy.NotifyCount() == 0U,
          "activated session does not duplicate OTA status on legacy GATT");

    std::string received;
    std::vector<uint8_t> fragment;
    bool first = true;
    while (Peer::TakeSharedOtaStatusFragment(service, fragment)) {
        const bool begins = (fragment[1U] & 1U) != 0U;
        const bool ends = (fragment[1U] & 2U) != 0U;
        const size_t header = begins ? 4U : 2U;
        Check(fragment.size() <= 10U && fragment[0U] == 1U &&
                  begins == first,
              "shared OTA status respects negotiated Object body bounds");
        if (begins) {
            Check(fxe::ReadU16LE(fragment.data() + 2U) ==
                      std::strlen(json),
                  "first shared OTA status fragment declares exact JSON size");
        }
        received.append(
            reinterpret_cast<const char*>(fragment.data() + header),
            fragment.size() - header);
        first = false;
        if (ends) break;
    }
    Check(received == json,
          "minimum-MTU shared OTA status reconstructs exact existing JSON");
#endif
}

bool CollectSharedDigestJson(modules::BleService& service,
                            std::vector<std::string>& records) {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    std::string assembling;
    size_t declaredSize = 0U;
    bool active = false;
    bool intact = true;
    for (unsigned attempt = 0U; attempt < 128U; ++attempt) {
        Peer::UpdateDigestNotification(service);
        std::vector<uint8_t> fragment;
        if (!Peer::TakeSharedOtaStatusFragment(service, fragment)) continue;
        if (fragment.size() < 2U || fragment.size() > 10U ||
            fragment[0U] != 1U) {
            intact = false;
            continue;
        }
        const bool begins = (fragment[1U] & 1U) != 0U;
        const bool ends = (fragment[1U] & 2U) != 0U;
        const size_t header = begins ? 4U : 2U;
        if (fragment.size() < header) {
            intact = false;
            continue;
        }
        if (begins) {
            if (active) {
                std::printf("[DIGEST_ADMISSION] interrupted=%s\n",
                            assembling.c_str());
                intact = false;
            }
            assembling.clear();
            declaredSize = fxe::ReadU16LE(fragment.data() + 2U);
            active = true;
        } else if (!active) {
            intact = false;
        }
        assembling.append(
            reinterpret_cast<const char*>(fragment.data() + header),
            fragment.size() - header);
        if (ends) {
            intact = intact && active && assembling.size() == declaredSize;
            records.push_back(assembling);
            std::printf("[DIGEST_ADMISSION] complete=%s\n", assembling.c_str());
            assembling.clear();
            active = false;
        }
    }
    return intact && !active;
}

void TestFirmwareDigestRetainsEveryJsonAcrossReverseAdmission() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    const std::array<std::string, 9U> expected{{
        R"({"h0":"00010203"})", R"({"h1":"04050607"})",
        R"({"h2":"08090a0b"})", R"({"h3":"0c0d0e0f"})",
        R"({"h4":"10111213"})", R"({"h5":"14151617"})",
        R"({"h6":"18191a1b"})", R"({"h7":"1c1d1e1f"})",
        R"({"h":"r"})"}};
    std::array<uint8_t, modules::FirmwareDigestProvider::DIGEST_BYTES> digest{};
    for (size_t index = 0U; index < digest.size(); ++index) {
        digest[index] = static_cast<uint8_t>(index);
    }

    for (const bool denied : {false, true}) {
        std::printf("[DIGEST_ADMISSION] reverse_window_denied=%u\n", denied);
        modules::BleService service;
        NimBLECharacteristic legacy;
        Peer::ConfigureOtaNotifications(service, legacy);
        Check(Peer::ActivateMinimumConnectionObjectSession(service) &&
                  Peer::BeginControlledDigestNotification(service),
              "digest fixture activates actual minimum-MTU session and Pending provider");
        std::vector<std::string> pending;
        Check(CollectSharedDigestJson(service, pending) &&
                  pending == std::vector<std::string>{R"({"h":"p"})"},
              "actual digest producer emits one intact Pending JSON");

        if (denied) {
            Check(Peer::FillSharedReverseWindow(service),
                  "actual reverse Object window refuses admission when grants and slots are full");
        }
        Peer::CompleteControlledDigest(service, digest);
        if (denied) {
            for (unsigned attempt = 0U; attempt < 12U; ++attempt) {
                Peer::UpdateDigestNotification(service);
            }
            Check(Peer::ReleaseSharedReverseWindow(service),
                  "real reverse Object Progress releases the denied admission window");
        }

        std::vector<std::string> received;
        Check(CollectSharedDigestJson(service, received),
              "digest JSON Begin/End boundaries and lengths remain intact while producer updates");
        Check(received.size() == expected.size(),
              "shared digest emits exactly eight SHA fragments and one Ready JSON");
        for (size_t index = 0U; index < expected.size(); ++index) {
            std::printf("[DIGEST_ADMISSION] expected[%zu]=%s actual=%s\n",
                        index, expected[index].c_str(),
                        index < received.size() ? received[index].c_str() : "<missing>");
            Check(index < received.size() && received[index] == expected[index],
                  "shared digest preserves every exact h0..h7 and h:r JSON in order");
        }
        Check(legacy.NotifyCount() == 0U && !Peer::DigestRequestPending(service),
              "shared digest finishes without duplicate legacy characteristic notifications");
    }

    modules::BleService direct;
    NimBLECharacteristic legacy;
    Peer::ConfigureOtaNotifications(direct, legacy);
    Peer::MarkConnected(direct, true);
    Check(Peer::BeginControlledDigestNotification(direct),
          "pre-session updater fixture begins controlled digest provider");
    Peer::UpdateDigestNotification(direct);
    Check(legacy.LastTx() == R"({"h":"p"})",
          "pre-session updater receives exact Pending JSON");
    Peer::CompleteControlledDigest(direct, digest);
    for (size_t index = 0U; index < expected.size(); ++index) {
        Peer::UpdateDigestNotification(direct);
        Check(legacy.LastTx() == expected[index] &&
                  legacy.NotifyCount() == index + 2U,
              "pre-session updater preserves every exact digest notification in order");
    }
    Check(!Peer::DigestRequestPending(direct),
          "pre-session updater completes its digest request");
#endif
}

struct SharedStatusRecords {
    std::vector<std::string> records;
    std::string assembling;
    size_t declared = 0U;
    bool active = false;
    bool intact = true;

    void Consume(const std::vector<uint8_t>& fragment) {
        if (fragment.size() < 2U || fragment.size() > 10U || fragment[0U] != 1U) {
            intact = false;
            return;
        }
        const bool begins = (fragment[1U] & 1U) != 0U;
        const bool ends = (fragment[1U] & 2U) != 0U;
        const size_t header = begins ? 4U : 2U;
        if (fragment.size() < header) { intact = false; return; }
        if (begins) {
            intact = intact && !active;
            assembling.clear();
            declared = fxe::ReadU16LE(fragment.data() + 2U);
            active = true;
        } else intact = intact && active;
        assembling.append(reinterpret_cast<const char*>(fragment.data() + header),
                          fragment.size() - header);
        if (ends) {
            intact = intact && active && assembling.size() == declared;
            records.push_back(assembling);
            std::printf("[STATUS_RETENTION] complete=%s\n", assembling.c_str());
            active = false;
            assembling.clear();
        }
    }

    void Take(modules::BleService& service) {
        std::vector<uint8_t> fragment;
        if (modules::BleServiceConnectionEpochTestPeer::TakeSharedStatusBesideState(service, fragment)) {
            Consume(fragment);
        }
    }
};

bool QueueSharedStatusCommand(modules::BleService& service, const std::string& command,
                              const uint32_t generation = 7U, const bool useStreamToken = false) {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    size_t offset = 0U;
    while (offset < command.size()) {
        const bool begins = offset == 0U;
        const size_t header = begins ? 4U : 2U;
        const size_t count = std::min<size_t>(10U - header, command.size() - offset);
        std::vector<uint8_t> fragment(header + count, 0U);
        fragment[0U] = 1U;
        fragment[1U] = (begins ? 1U : 0U) | (offset + count == command.size() ? 2U : 0U);
        if (begins) fxe::WriteU16LE(static_cast<uint16_t>(command.size()), fragment.data() + 2U);
        std::copy_n(command.data() + offset, count, fragment.data() + header);
        if (!Peer::HandleConnectionOtaControl(service, fragment.data(), fragment.size(), 0U, generation, useStreamToken)) return false;
        offset += count;
    }
    return true;
}

#include "ota_attempt_proof_cases.hpp"

void TestRequiredGattFailureCleansPartialStack() {
    using namespace modules::ble_internal;
    Harness harness;
    OtaStorageProbe storage;
    for (const bool recovery : {true, false}) {
        for (const uint32_t failedService : {1U, 2U}) {
            if (recovery && failedService == 2U) continue;
            modules::BleService service;
            service.SetOtaPlatformAdapter(&storage);
            const unsigned shutdowns = ota_runtime_test::shutdowns;
            NimBLEService::FailStartOnCall(failedService);
            if (recovery) service.BeginRecovery(); else service.Begin(harness.settings);
            Check(!service.IsAdvertising() &&
                      NimBLEDevice::getServer()->FindService(BASE_SERVICE_UUID) == nullptr &&
                      ota_runtime_test::shutdowns == shutdowns + 1U,
                  "required GATT registration failure cleans SDK tree without advertising");
            NimBLEService::FailStartOnCall(0U);
            service.Update(harness.settings);
            Check(service.IsAdvertising() &&
                      NimBLEDevice::getServer()->FindService(BASE_SERVICE_UUID) != nullptr,
                  "existing Update reconstructs a clean complete GATT tree after failure");
            service.Stop();
        }
    }

}

void TestFailedHostStopRetainsNativeBindingUntilCleanupRetry() {
    using namespace modules::ble_internal;
    Harness harness;
    modules::BleService service;
    service.Begin(harness.settings);
    const unsigned before = ota_runtime_test::shutdowns;
    Check(g_bleService == &service &&
              NimBLEDevice::getServer()->FindService(BASE_SERVICE_UUID) != nullptr,
          "native stop fixture owns a complete bound GATT tree");
    NimBLEDevice::FailNextHostStop();
    Check(!service.Stop(), "failed native shutdown is reported to the sleep caller");
    Check(g_bleService == &service && !service.IsEnabled() &&
              ota_runtime_test::shutdowns == before &&
              NimBLEDevice::getServer()->FindService(BASE_SERVICE_UUID) != nullptr,
          "failed host stop retains the native NVS binding and SDK tree without false shutdown");
    service.Update(harness.settings);
    Check(g_bleService == &service && service.IsAdvertising() &&
              ota_runtime_test::shutdowns == before + 1U &&
              NimBLEDevice::getServer()->FindService(BASE_SERVICE_UUID) != nullptr,
          "existing Update stops and deletes the retained tree once before rebuilding after failure");
    Check(service.Stop(), "completed native shutdown authorizes the sleep caller");
}

void TestRecoveryPublicSnapshotWithoutOwnerOrSettingsLoad() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using namespace modules::ble_internal;
    namespace wire = modules::connection_session_codec;
    Harness harness;
    modules::BleService service;
    OtaStorageProbe storage;
    service.SetOtaPlatformAdapter(&storage);
    unsigned captures = 0U;
    service.SetSupportCapture([](void* context, bool publicOnly, uint8_t, uint32_t request,
                                uint8_t* out, size_t capacity) -> size_t {
        if (!publicOnly) { ++*static_cast<unsigned*>(context); return 0U; }
        modules::support_snapshot::PublicHealth h;
        h.validity = 4U; h.uptimeMs = UINT64_C(0x100001234);
        h.resetReason = 4U; h.releaseLabel = "0.9.5-test";
        h.recoveryMode = 1U; h.recoveryCause = 1U; h.recoveryState = 2U;
        h.boots = 4U; h.earlyCrashes = 3U; h.storageAccess = 1U; h.updater = 2U;
        return modules::support_snapshot::EncodePublic(out, capacity, request, h);
    }, &captures);
    service.BeginRecovery();
    auto* server = NimBLEDevice::getServer();
    NimBLEConnInfo conn;
    server->Callbacks()->onConnect(server, conn);
    service.DrainEventQueue();
    const auto generation = Peer::ProducerEpoch(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, generation),
          "public recovery capsule uses actual existing session at minimum MTU");
    auto* egress = server->FindService(BASE_SERVICE_UUID)->FindCharacteristic(CONNECTION_SESSION_EGRESS_CHARACTERISTIC_UUID);
    egress->Callbacks()->onSubscribe(egress, conn, 1U);
    struct Captured {
        NimBLECharacteristic* source;
        modules::BleService* service;
        std::vector<std::vector<uint8_t>> packets;
        SharedStatusRecords statuses;
    } captured{egress, &service, {}, {}};
    egress->SetNotifyHook([](void* context) {
        auto& c = *static_cast<Captured*>(context);
        const auto bytes = c.source->LastTx();
        c.packets.emplace_back(bytes.begin(), bytes.end());
        wire::PacketView packet;
        if (wire::Codec::Decode({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}, packet, 20U) == wire::Error::None &&
            packet.header.kind == wire::Kind::Object &&
            (packet.header.code == 0x45U || packet.header.code == modules::ConnectionSession::OTA_STATUS_RECEIPT_CODE)) {
            if (packet.header.code == modules::ConnectionSession::OTA_STATUS_RECEIPT_CODE) {
                std::vector<uint8_t> fragment(packet.body.Data(), packet.body.Data() + packet.body.Size());
                c.statuses.Consume(fragment);
            }
            std::array<uint8_t, 10U> body{};
            fxe::WriteU32LE(packet.header.sequence, body.data());
            fxe::WriteU32LE(packet.header.sequence + modules::ConnectionSession::REVERSE_OBJECT_CAPACITY, body.data() + 4U);
            std::array<uint8_t, 20U> encoded{};
            size_t size = 0U;
            if (wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Progress), packet.header.token, packet.header.sequence},
                {body.data(), body.size()}}, {encoded.data(), encoded.size()}, size) == wire::Error::None)
                c.service->OnConnectionSessionWrite(encoded.data(), size);
        }
    }, &captured);
    std::array<uint8_t, 20U> encoded{};
    size_t size = 0U;
    const std::array<uint8_t, 6U> request{1U, 0U, 77U, 0U, 0U, 0U};
    Check(wire::Codec::Encode({{wire::Kind::Object, 0x45U, Peer::ObjectToken(service), 1U},
          {request.data(), request.size()}}, {encoded.data(), encoded.size()}, size) == wire::Error::None,
          "public request fits minimum ATT payload");
    service.OnConnectionSessionWrite(encoded.data(), size);
    service.DrainEventQueue();
    for (unsigned i = 0U; i < 32U; ++i) {
        service.DrainEventQueue();
        service.Update(harness.settings);
    }
    std::vector<uint8_t> capsule;
    for (const auto& bytes : captured.packets) {
        wire::PacketView packet;
        if (wire::Codec::Decode({bytes.data(), bytes.size()}, packet, 20U) != wire::Error::None ||
            packet.header.kind != wire::Kind::Object || packet.header.code != 0x45U) continue;
        const auto skip = (packet.body[1U] & 1U) ? 4U : 2U;
        for (size_t i = skip; i < packet.body.Size(); ++i) capsule.push_back(packet.body[i]);
    }
    Check(capsule.size() == 106U && capsule[0U] == 2U && capsule[86U] == 1U &&
              capsule[87U] == 1U && capsule[99U] == 1U &&
              fxe::ReadU32LE(capsule.data() + 82U) == 77U &&
              fxe::ReadU32LE(capsule.data() + 94U) == 3U,
          "public early-crash/reset/uptime/storage-not-loaded facts reach wire without owner proof or settings load");
    // Exercise the real final notify filter, not TakeSharedStatusBesideState's
    // direct retained-output inspection. OTA receipts are reverse Objects32.
    const auto driveNotifications = [&]() {
        for (unsigned i = 0U; i < 32U; ++i) {
            service.DrainEventQueue();
            service.Update(harness.settings);
        }
    };
    const auto received = [&](const char* exact) {
        return std::find(captured.statuses.records.begin(), captured.statuses.records.end(), exact) != captured.statuses.records.end();
    };
    Check(QueueSharedStatusCommand(service, R"({"cmd":"ota_begin","size":10,"chunk":20})", generation),
          "recovery OTA Begin reaches existing producer");
    driveNotifications();
    Check(received(R"({"b":1})"),
          "RecoveryOtaFinalEgressJourney: actual notify emits reverse Object32 Begin receipt");
    const std::array<uint8_t, 10U> image{};
    Check(Peer::HandleConnectionOtaData(service, image.data(), image.size()),
          "recovery final-egress fixture writes through the production OTA data owner");
    driveNotifications();
    const auto receivedState = [&](const char* expected) {
        return std::any_of(captured.statuses.records.begin(), captured.statuses.records.end(),
            [&](const std::string& record) {
                StaticJsonDocument<256U> doc;
                return !deserializeJson(doc, record) && doc["st"] == expected && doc["rx"] == 10U;
            });
    };
    Check(receivedState("progress"),
          "actual recovery notify emits reverse Object32 progress after image data");
    char endCommand[80]{};
    std::snprintf(endCommand, sizeof(endCommand), "{\"cmd\":\"ota_end\",\"size\":10,\"crc32\":\"%08lx\"}",
        static_cast<unsigned long>(fxe::Crc32(image.data(), image.size())));
    Check(QueueSharedStatusCommand(service, endCommand, generation),
          "recovery final-egress fixture submits exact terminal request");
    driveNotifications();
    Check(receivedState("done") && captured.statuses.intact && storage.ends == 1U,
          "actual recovery notify emits an intact reverse Object32 terminal receipt");
    captured.packets.clear();
    const std::array<uint8_t, 6U> nextRequest{1U, 0U, 78U, 0U, 0U, 0U};
    wire::Codec::Encode({{wire::Kind::Object, 0x45U, Peer::ObjectToken(service), 2U},
          {nextRequest.data(), nextRequest.size()}}, {encoded.data(), encoded.size()}, size);
    service.OnConnectionSessionWrite(encoded.data(), size);
    driveNotifications();
    size_t publicBytes = 0U;
    for (const auto& bytes : captured.packets) {
        wire::PacketView packet;
        if (wire::Codec::Decode({bytes.data(), bytes.size()}, packet, 20U) == wire::Error::None &&
            packet.header.kind == wire::Kind::Object && packet.header.code == 0x45U)
            publicBytes += packet.body.Size() - ((packet.body[1U] & 1U) ? 4U : 2U);
    }
    Check(publicBytes == 106U,
          "public support remains deliverable after actual OTA receipts; no blocked Object32 head-of-line");
    captured.packets.clear();
    wire::Codec::Encode({{wire::Kind::Object, 0x44U, Peer::ObjectToken(service), 3U},
          {request.data(), request.size()}}, {encoded.data(), encoded.size()}, size);
    service.OnConnectionSessionWrite(encoded.data(), size);
    service.DrainEventQueue();
    Check(captures == 0U, "recovery private44 cannot invoke the capture owner");
    Check(Peer::InjectPrivateSupport(service), "negative egress test retains private bytes before policy guard");
    for (unsigned i = 0U; i < 4U; ++i) service.Update(harness.settings);
    bool privateOutput = false;
    for (const auto& bytes : captured.packets) {
        wire::PacketView packet;
        if (wire::Codec::Decode({bytes.data(), bytes.size()}, packet, 20U) == wire::Error::None &&
            packet.header.kind == wire::Kind::Object && packet.header.code == 0x44U) privateOutput = true;
    }
    Check(!privateOutput && Peer::RecoveryHasNoDomainBindings(service),
          "recovery egress blocks retained private44 even though updater needs no owner proof");
    service.Stop();
}

void TestRecoveryUsesUpdaterWithoutDomainStartup() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using namespace modules::ble_internal;
    fxe::ManualMillis::SetNowMs(1000U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    Harness harness;
    modules::BleService service;
    OtaStorageProbe storage;
    harness.settings.Doc()["BLE_ENABLED"] = false;
    harness.settings.Doc()["BLE_DEVICE_NAME"] = "User name";
    const uint32_t revision = harness.settings.GetRevision();
    service.SetControlMutationService(&harness.controlMutation);
    service.SetOtaPlatformAdapter(&storage);
    service.SetHardwareRevision(modules::ble_device_capabilities::HardwareRevision::V3_3);
    // Capture the actual normal production registration order before entering
    // recovery, as a bonded phone's cached handles refer to this profile.
    service.Begin(harness.settings);
    harness.settings.Doc()["BLE_ENABLED"] = true;
    service.Begin(harness.settings);
    auto* normalBase = NimBLEDevice::getServer()->FindService(BASE_SERVICE_UUID);
    std::vector<std::string> cachedNormalAttributes;
    if (normalBase != nullptr) {
        for (size_t i = 0U; normalBase->CharacteristicAt(i) != nullptr; ++i)
            cachedNormalAttributes.push_back(normalBase->CharacteristicAt(i)->Uuid());
    }
    service.Stop();
    service.BeginRecovery();
    auto* const server = NimBLEDevice::getServer();
    auto* const base = server->FindService(BASE_SERVICE_UUID);
    Check(base != nullptr && server->FindService(SETTINGS_SERVICE_UUID) == nullptr &&
              Peer::RecoveryHasNoDomainBindings(service),
          "actual recovery stack creates updater/session only without a settings store");
    if (base == nullptr) return;
    bool cachedProfileMatches = !cachedNormalAttributes.empty();
    for (size_t i = 0U; i < cachedNormalAttributes.size(); ++i) {
        const auto* current = base->CharacteristicAt(i);
        cachedProfileMatches = cachedProfileMatches && current != nullptr &&
            current->Uuid() == cachedNormalAttributes[i];
    }
    Check(cachedProfileMatches,
          "cached normal base attribute positions survive actual recovery registration; public bootstrap cannot move");

    auto* const control = base->FindCharacteristic(OTA_CONTROL_CHARACTERISTIC_UUID);
    Check(control != nullptr && base->FindCharacteristic(CONNECTION_SESSION_INGRESS_CHARACTERISTIC_UUID) &&
              base->FindCharacteristic(CONNECTION_SESSION_EGRESS_CHARACTERISTIC_UUID) &&
              base->FindCharacteristic(OTA_DATA_CHARACTERISTIC_UUID) &&
              base->FindCharacteristic(COMMAND_CHARACTERISTIC_UUID),
          "recovery retains stable base GATT skeleton with updater-only capabilities");
    if (control == nullptr) return;
    service.SetDiagnosticRadioOverride(true, false);
    service.Update(harness.settings);
    service.Begin(harness.settings);
    Check(service.IsEnabled() && std::string(service.GetDeviceName()) == "Litch Recovery" &&
              Peer::RecoveryHasNoDomainBindings(service),
          "late loaded preferences/normal Begin/diagnostic off cannot disable recovery");
    NimBLEConnInfo conn;
    NimBLEDevice::getAdvertising()->SetActive(false);
    server->Callbacks()->onConnect(server, conn);
    service.DrainEventQueue();
    control->Callbacks()->onSubscribe(control, conn, 1U);
    const auto send = [&](const char* text) {
        control->setValue(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
        control->Callbacks()->onWrite(control, conn);
        service.DrainEventQueue();
    };
    send(R"({"cmd":"info"})");
    Check(control->LastTx() == R"({"i":[3,4,24,1]})",
          "recovery mode and exact truthful mask arrive through existing pre-session info");
    const uint8_t recoveryAddress[6] = {0x81U, 0x3fU, 0x52U, 0xa6U, 0x10U, 0x24U};
    NimBLEDevice::SetTestAddress(recoveryAddress, true);
    send(R"({"cmd":"info","bs":1})");
    Check(control->LastTx() == R"({"i":[3,4,24,1]})" && NimBLEDevice::AddressReads() == 0U,
          "recovery selector returns unchanged public info without reading local identity");
    constexpr char domainCommand[] = R"({"cmd":"console","line":"newgame"})";
    service.OnCommandCharacteristicWrite(domainCommand, sizeof(domainCommand) - 1U);
    char input = 0;
    Check(!service.TryReadConsoleInputChar(input) &&
              harness.settings.GetRevision() == revision && harness.newGameStartCount == 0U,
          "legacy domain ingress cannot reach Console/settings/game in recovery");
    auto* const console = base->FindCharacteristic(CONSOLE_STREAM_CHARACTERISTIC_UUID);
    auto* const command = base->FindCharacteristic(COMMAND_CHARACTERISTIC_UUID);
    auto* const craft = base->FindCharacteristic(LITCHCRAFT_CHARACTERISTIC_UUID);
    const auto ordinaryBefore = Peer::RawOrdinaryCount(service);
    if (command && console && craft) {
        command->setValue(reinterpret_cast<const uint8_t*>("newgame"), 7U);
        command->Callbacks()->onWrite(command, conn);
        console->Callbacks()->onSubscribe(console, conn, 1U);
        const uint8_t enter[] = {1U, 1U, 0U, 0U};
        craft->setValue(enter, sizeof(enter));
        craft->Callbacks()->onWrite(craft, conn);
        craft->Callbacks()->onSubscribe(craft, conn, 1U);
        service.WriteConsoleOutput("private console", 15U);
        service.Update(harness.settings);
        Check(Peer::RawOrdinaryCount(service) == ordinaryBefore &&
                  !service.IsConsoleStreamSubscribed() && console->NotifyCount() == 0U &&
                  craft->NotifyCount() == 0U && Peer::RecoveryHasNoDomainBindings(service),
              "skeletal recovery Command/Console/Craft callbacks admit no domain work or notifications");
    }
    const uint32_t generation = Peer::ProducerEpoch(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, generation),
          "recovery negotiates the actual existing minimum-MTU shared session");
    namespace wire = modules::connection_session_codec;
    std::array<uint8_t, 20U> packet{};
    size_t packetSize = 0U;
    std::array<uint8_t, 8U> open{};
    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, open.data());
    open[3U] = modules::ConnectionSession::ORDERED_CAPACITY;
    fxe::WriteU16LE(10U, open.data() + 4U);
    Check(wire::Codec::Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Open), Peer::ConnectionToken(service), 2U},
         {open.data(), open.size()}}, {packet.data(), packet.size()}, packetSize) == wire::Error::None,
         "recovery peer can open existing ordered stream");
    service.OnConnectionSessionWrite(packet.data(), packetSize);
    service.DrainEventQueue();
    const std::array<uint8_t, 4U> mutation{{modules::settings_state_wire::VERSION, 39U, 1U, 0U}};
    Check(wire::Codec::Encode(
        {{wire::Kind::Ordered, modules::settings_state_wire::MUTATION_OPCODE,
          Peer::OrderedToken(service), 1U}, {mutation.data(), mutation.size()}},
        {packet.data(), packet.size()}, packetSize) == wire::Error::None,
        "recovery adversary submits a valid typed settings mutation");
    service.OnConnectionSessionWrite(packet.data(), packetSize);
    service.DrainEventQueue();
    Check(harness.settings.GetRevision() == revision &&
              std::string(harness.settings.ReadValue("HINT_THINKING_STYLE", "foxie_sweep")) == "foxie_sweep",
          "actual recovery Ordered dispatch rejects domain mutation without applying it");
    Check(QueueSharedStatusCommand(service, R"({"cmd":"ota_begin","size":10,"chunk":20})", generation),
          "recovery shared updater control reaches production ingress");
    service.DrainEventQueue();
    SharedStatusRecords capture;
    for (unsigned i = 0U; i < 32U; ++i) capture.Take(service);
    Check(service.IsOtaInProgress() && storage.begins == 1U,
          "recovery admits exactly one actual OTA backend Begin");
    const std::array<uint8_t, 10U> bytes{};
    Check(Peer::HandleConnectionOtaData(service, bytes.data(), bytes.size()),
          "recovery uses the actual shared OTA data adapter after activation");
    service.DrainEventQueue();
    for (unsigned i = 0U; i < 32U; ++i) capture.Take(service);
    char end[80] = {};
    std::snprintf(end, sizeof(end), "{\"cmd\":\"ota_end\",\"size\":10,\"crc32\":\"%08lx\"}",
                  static_cast<unsigned long>(fxe::Crc32(bytes.data(), bytes.size())));
    Check(QueueSharedStatusCommand(service, end, generation), "recovery queues exact OTA End");
    service.DrainEventQueue();
    Check(storage.written == 10U && storage.ends == 1U && Peer::PendingReboot(service) &&
              harness.settings.GetRevision() == revision && Peer::RecoveryHasNoDomainBindings(service),
          "recovery completes through existing image owner with no domain persistence");
    service.Stop();
    // A fresh recovery lifetime repairs native connection retirement normally.
    service.BeginRecovery();
    server->Callbacks()->onConnect(server, conn);
    service.DrainEventQueue();
    server->Callbacks()->onDisconnect(server, conn, 19);
    service.DrainEventQueue();
    server->Callbacks()->onConnect(server, conn);
    service.DrainEventQueue();
    Check(service.IsConnected() && Peer::RecoveryHasNoDomainBindings(service),
          "recovery disconnect/reconnect retains updater-only connection ownership");
    service.Stop();
    service.SetOtaPlatformAdapter(nullptr);
    service.BeginRecovery();
    server->Callbacks()->onConnect(server, conn);
    service.DrainEventQueue();
    auto* const unavailable = server->FindService(BASE_SERVICE_UUID)->FindCharacteristic(OTA_CONTROL_CHARACTERISTIC_UUID);
    unavailable->Callbacks()->onSubscribe(unavailable, conn, 1U);
    const auto sendUnavailable = [&](const char* text) {
        unavailable->setValue(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
        unavailable->Callbacks()->onWrite(unavailable, conn);
        service.DrainEventQueue();
    };
    sendUnavailable(R"({"cmd":"info"})");
    Check(unavailable->LastTx() == R"({"i":[3,4,8,1]})",
          "failed SDK adapter initialization cannot advertise firmware update capability");
    const auto begins = storage.begins;
    sendUnavailable(R"({"cmd":"ota_begin","size":10})");
    Check(!service.IsOtaInProgress() && storage.begins == begins &&
              unavailable->LastTx() == R"({"e":"unavailable"})",
          "old pre-session sender cannot bypass unavailable strict SDK adapter");
    service.Stop();
    // A real reboot constructs a fresh normal owner; Begin never promotes an
    // existing recovery owner in place. Check the reverse cached transition.
    modules::BleService normalAfterRecovery;
    harness.settings.Doc()["BLE_ENABLED"] = true;
    normalAfterRecovery.Begin(harness.settings);
    const auto* restoredBase = NimBLEDevice::getServer()->FindService(BASE_SERVICE_UUID);
    bool reverseProfileMatches = restoredBase != nullptr;
    for (size_t i = 0U; reverseProfileMatches && i < cachedNormalAttributes.size(); ++i) {
        const auto* attribute = restoredBase->CharacteristicAt(i);
        reverseProfileMatches = attribute && attribute->Uuid() == cachedNormalAttributes[i];
    }
    Check(reverseProfileMatches,
          "cached recovery base positions return to the identical normal profile after fresh boot");
    normalAfterRecovery.Stop();
}

void TestPendingDigestAndMixedStatusPressure() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    NimBLECharacteristic legacy;
    runtime::RuntimeHealthReducer health;
    modules::RuntimeHealthRecheckProbe recheck;
    recheck.result = true;
    Check(Peer::Configure(service, settings, control) &&
              Peer::ActivateMinimumConnectionObjectSession(service),
          "mixed status fixture owns actual ingress and minimum-MTU session");
    Peer::PrepareProducerGeneration(service);
    Peer::ConfigureOtaNotifications(service, legacy);
    Peer::AttachRuntimeHealth(service, health, recheck);
    Check(Peer::BeginControlledDigestNotification(service), "mixed digest provider starts Pending");
    Peer::UpdateDigestNotification(service); // Only the Pending Begin fragment is retained.
    std::array<uint8_t, modules::FirmwareDigestProvider::DIGEST_BYTES> digest{};
    for (size_t index = 0U; index < digest.size(); ++index) digest[index] = static_cast<uint8_t>(index);
    Peer::CompleteControlledDigest(service, digest);
    Check(QueueSharedStatusCommand(service, R"({"cmd":"fw_version"})") &&
              QueueSharedStatusCommand(service, R"({"cmd":"info"})") &&
              QueueSharedStatusCommand(service, R"({"cmd":"health"})") &&
              QueueSharedStatusCommand(service, R"({"cmd":"health_chk"})"),
          "mixed reliable queries enter the actual shared-control reassembly and event queues");
    SharedStatusRecords capture;
    for (unsigned attempt = 0U; attempt < 256U; ++attempt) {
        service.DrainEventQueue();
        Peer::UpdateDigestNotification(service);
        capture.Take(service);
    }
    const std::vector<std::string> expectedDigest{
        R"({"h":"p"})", R"({"h0":"00010203"})", R"({"h1":"04050607"})",
        R"({"h2":"08090a0b"})", R"({"h3":"0c0d0e0f"})", R"({"h4":"10111213"})",
        R"({"h5":"14151617"})", R"({"h6":"18191a1b"})", R"({"h7":"1c1d1e1f"})", R"({"h":"r"})"};
    std::vector<std::string> receivedDigest;
    for (const auto& record : capture.records) {
        if (std::find(expectedDigest.begin(), expectedDigest.end(), record) != expectedDigest.end()) {
            receivedDigest.push_back(record);
        }
    }
    const std::string version = std::string(R"({"v":")") + firmware::FIRMWARE_VERSION + R"("})";
    Check(capture.intact && !capture.active && receivedDigest == expectedDigest,
          "Pending-to-Ready preserves every exact digest JSON beside other reliable responses");
    for (const auto& response : {version, std::string(R"({"i":[3,0,127]})"),
                                 std::string(R"({"h":"000000000"})"), std::string(R"({"c":"ok"})")}) {
        Check(std::count(capture.records.begin(), capture.records.end(), response) == 1,
              "fw_version info health and recheck each return one exact shared JSON under pressure");
    }
    Check(recheck.calls == 1U && legacy.NotifyCount() == 0U,
          "queued shared health recheck executes once without retired direct notifications");
}

void TestBlockedInfoAllowsQueuedProgressAndTimeoutRecovery() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    fxe::ManualMillis::SetNowMs(1000U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    NimBLECharacteristic legacy;
    OtaStorageProbe storage;
    Check(Peer::Configure(service, settings, control) &&
              Peer::ActivateMinimumConnectionObjectSession(service), "timeout pressure fixture activates actual session");
    Peer::PrepareProducerGeneration(service);
    Peer::ConfigureOtaNotifications(service, legacy);
    service.SetOtaPlatformAdapter(&storage);
    Check(QueueSharedStatusCommand(service, R"({"cmd":"ota_begin","size":10,"chunk":20})"),
          "timeout fixture queues a real OTA begin");
    service.DrainEventQueue();
    SharedStatusRecords capture;
    for (unsigned attempt = 0U; attempt < 32U; ++attempt) capture.Take(service);
    Check(service.IsOtaInProgress() && storage.begins == 1U, "timeout fixture enters authoritative OTA transaction");
    capture.records.clear();
    Check(Peer::FillSharedReverseWindow(service), "reliable output pressure consumes actual reverse grants");
    const uint32_t lastSent = Peer::SendSharedReverseWindowWithoutConfirm(service);
    constexpr const char* pinned = R"({"v":"pinned-before-timeout"})";
    Peer::NotifyOtaJson(service, legacy, pinned);
    Peer::NotifyProgress(service);
    Check(QueueSharedStatusCommand(service, R"({"cmd":"info"})"), "priority info queues behind retained reliable output");
    service.DrainEventQueue();
    fxe::ManualMillis::AdvanceMs(modules::BLE_OTA_INACTIVITY_TIMEOUT_MS + 1U);
    Peer::PollOtaInactivity(service);
    Check(!service.IsOtaInProgress() && storage.aborts == 1U,
          "actual OTA timeout promptly aborts backend while reliable status admission is denied");
    Check(lastSent != 0U && Peer::QueueSharedReverseProgress(service, lastSent),
          "real cumulative Progress enters reserved session control behind blocked priority info");
    for (unsigned attempt = 0U; attempt < 128U; ++attempt) {
        service.DrainEventQueue();
        capture.Take(service);
    }
    Check(capture.intact && !capture.active && capture.records.size() >= 3U && capture.records.front() == pinned,
          "progress and timeout error cannot interrupt the pinned reliable JSON");
    Check(std::count(capture.records.begin(), capture.records.end(), R"({"e":"timeout"})") == 1 &&
              std::count(capture.records.begin(), capture.records.end(), R"({"i":[3,0,63]})") == 1,
          "queued Progress releases denial and both deferred timeout error and priority info arrive intact");
    Check(legacy.NotifyCount() == 0U, "denied shared replies never escape onto legacy GATT");
}

void TestSharedDoneWaitsForActualFinalNotifyAcceptance(
    const bool repeatedFinalizedControl = false) {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    std::printf("[DONE_RETENTION] repeated_finalized_control=%u\n", repeatedFinalizedControl);
    fxe::ManualMillis::SetNowMs(1000U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    NimBLECharacteristic legacy;
    NimBLECharacteristic egress;
    OtaStorageProbe storage;
    Check(Peer::Configure(service, settings, control) &&
              Peer::ActivateMinimumConnectionObjectSession(service), "done pressure fixture activates actual session");
    Peer::PrepareProducerGeneration(service);
    Peer::ConfigureRenewalServer(service, 7U);
    Peer::ConfigureOtaNotifications(service, legacy);
    service.SetOtaPlatformAdapter(&storage);
    runtime::BootGuard bootGuard;
    ota_runtime_test::SetBootResetReason(1U); // power-on
    bootGuard.OnBootStart();
    ota_runtime_test::SetBootResetReason(3U); // two real incomplete SW resets
    bootGuard.OnBootStart();
    bootGuard.OnBootStart();
    Check(bootGuard.GetSnapshot().earlyCrashCount == 2U &&
              !bootGuard.IsSafeModeActive(),
          "two genuine early crashes retain evidence without latching recovery");
    service.SetBootGuard(&bootGuard);
    Check(QueueSharedStatusCommand(service, R"({"cmd":"ota_begin","size":10,"chunk":20})"),
          "done fixture queues real OTA begin");
    service.DrainEventQueue();
    SharedStatusRecords setup;
    for (unsigned attempt = 0U; attempt < 32U; ++attempt) setup.Take(service);
    const std::array<uint8_t, 10U> image{{0xe9U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U}};
    Check(Peer::HandleConnectionOtaData(service, image.data(), image.size(), 0U), "done fixture admits exact OTA bytes");
    service.DrainEventQueue();
    for (unsigned attempt = 0U; attempt < 32U; ++attempt) setup.Take(service);
    const uint32_t firstExpectedSequence = Peer::LastPublishedReverseSequence(service) + 1U;
    Check(QueueSharedStatusCommand(service, R"({"cmd":"ota_end"})"), "done fixture queues real OTA end");
    service.DrainEventQueue();
    Check(storage.ends == 1U && Peer::PendingReboot(service), "actual image finalization owns pending reboot");
    Peer::ConfigureConnectionSessionNotifications(service, egress);
    Peer::SubscribeDiagnosticState(service);
    struct Capture {
        NimBLECharacteristic* egress;
        SharedStatusRecords json;
        uint32_t sequence = 0U;
        bool blockedFinal = false;
        uint32_t firstSequence;
        uint32_t nextSequence;
        std::vector<std::string> admitted;
        bool stopAfterFirstDone;
    } capture{&egress, {}, 0U, false, firstExpectedSequence, firstExpectedSequence, {}, repeatedFinalizedControl};
    egress.SetNotifyHook([](void* context) {
        auto& capture = *static_cast<Capture*>(context);
        namespace wire = modules::connection_session_codec;
        wire::PacketView packet;
        const auto& bytes = capture.egress->LastTx();
        if (wire::Codec::Decode({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}, packet, 20U) != wire::Error::None ||
            packet.header.kind != wire::Kind::Object) return;
        // Observe the reverse Object receiver boundary: retained exact retries
        // do not become a second domain fragment. Conflicts and holes fail.
        if (packet.header.sequence < capture.nextSequence) {
            const uint32_t index = packet.header.sequence - capture.firstSequence;
            capture.json.intact = capture.json.intact &&
                packet.header.sequence >= capture.firstSequence &&
                index < capture.admitted.size() && capture.admitted[index] == bytes;
            return;
        }
        if (packet.header.sequence != capture.nextSequence) {
            capture.json.intact = false;
            return;
        }
        capture.admitted.push_back(bytes);
        ++capture.nextSequence;
        capture.sequence = packet.header.sequence;
        if (packet.header.code != modules::ConnectionSession::OTA_STATUS_RECEIPT_CODE) return;
        capture.json.Consume(std::vector<uint8_t>(packet.body.Data(), packet.body.Data() + packet.body.Size()));
        if (!capture.blockedFinal && capture.json.active &&
            capture.json.assembling.size() + 8U >= capture.json.declared) {
            capture.blockedFinal = true;
            capture.egress->SetNotifyResult(false);
        }
        if (capture.stopAfterFirstDone && !capture.json.records.empty()) {
            capture.egress->SetNotifyResult(false);
        }
    }, &capture);
    for (unsigned attempt = 0U; attempt < 64U && !capture.blockedFinal; ++attempt) {
        service.Update(settings);
        if (capture.sequence != 0U) {
            Check(Peer::QueueSharedReverseProgress(service, capture.sequence), "actual notified prefix receives cumulative Progress");
            capture.sequence = 0U;
        }
        service.DrainEventQueue();
    }
    Check(capture.blockedFinal && capture.json.records.empty() && !Peer::RebootGraceArmed(service),
          "NimBLE rejection of final done fragment cannot arm delivery grace");
    if (repeatedFinalizedControl) {
        const uint32_t firstFinalSequence = Peer::LastPublishedReverseSequence(service);
        for (unsigned repetition = 0U; repetition < 3U; ++repetition) {
            Check(QueueSharedStatusCommand(service, R"({"cmd":"ota_end"})") &&
                      QueueSharedStatusCommand(service, R"({"cmd":"ota_abort"})"),
                  "repeated finalized end and abort enter actual shared control queues under notify denial");
        }
        for (unsigned attempt = 0U; attempt < 32U; ++attempt) {
            service.DrainEventQueue();
            service.Update(settings);
        }
        Check(storage.ends == 1U && storage.aborts == 0U &&
                  Peer::LastPublishedReverseSequence(service) >= firstFinalSequence + 6U &&
                  !Peer::RebootGraceArmed(service),
              "later duplicate done is fully retained without re-finalizing aborting or starting grace");
    }
    fxe::ManualMillis::AdvanceMs(1000U);
    bool earlyRestart = false;
    try { service.Update(settings); } catch (const ota_runtime_test::RestartRequested&) { earlyRestart = true; }
    Check(!earlyRestart && !Peer::RebootGraceArmed(service), "pending done keeps draining and cannot reboot while final notify is rejected");
    egress.SetNotifyResult(true);
    service.Update(settings);
    const uint32_t deliveredAt = fxe::Millis();
    Check(capture.json.intact && !capture.json.active &&
              capture.json.records == std::vector<std::string>{R"({"st":"done","pct":100,"rx":10,"total":10,"wb":10,"wm":0})"} &&
              Peer::RebootGraceArmed(service) && Peer::RebootArmedAtMs(service) == deliveredAt,
          "exact final done JSON starts grace only when its final fragment is actually notified");
    fxe::ManualMillis::AdvanceMs(499U);
    auto* const server = NimBLEDevice::getServer();
    const auto disconnects = server->DisconnectCount();
    server->SetDisconnectAccepted(!repeatedFinalizedControl);
    bool restarted = false;
    try { service.Update(settings); } catch (const ota_runtime_test::RestartRequested&) { restarted = true; }
    Check(!restarted && server->DisconnectCount() == disconnects,
          "actual delivered done retains full 499 ms grace before native disconnect");
    fxe::ManualMillis::AdvanceMs(1U);
    try { service.Update(settings); } catch (const ota_runtime_test::RestartRequested&) { restarted = true; }
    Check(!restarted && server->DisconnectCount() == disconnects + 1U &&
              server->LastDisconnect() == 7U,
          "actual delivered done requests one native disconnect after full 500 ms grace");
    if (!repeatedFinalizedControl) {
        fxe::ManualMillis::AdvanceMs(1U);
        service.OnDisconnected(7U, 19);
    } else {
        // A rejected SDK request or missing callback cannot strand finalized firmware.
        fxe::ManualMillis::AdvanceMs(249U);
        try { service.Update(settings); } catch (const ota_runtime_test::RestartRequested&) { restarted = true; }
        Check(!restarted && server->DisconnectCount() == disconnects + 1U,
              "native disconnect is not retried while its bounded grace remains");
        fxe::ManualMillis::AdvanceMs(1U);
    }
    try { service.Update(settings); } catch (const ota_runtime_test::RestartRequested&) { restarted = true; }
    Check(restarted && server->DisconnectCount() == disconnects + 1U,
          "finalized OTA reboots after native disconnect or its bounded 250 ms fallback");
    server->SetDisconnectAccepted(true);
    ota_runtime_test::SetBootResetReason(3U); // actual OTA's software restart
    bootGuard.OnBootStart();
    Check(bootGuard.GetSnapshot().earlyCrashCount == 2U &&
              !bootGuard.IsSafeModeActive() && !bootGuard.IsLastResetCrashy(),
          "successful OTA reboot preserves prior crashes without adding a false one");
    std::printf("[OTA_BOOT_GUARD] after_update early_crashes=%lu safe_mode=%u\n",
                static_cast<unsigned long>(bootGuard.GetSnapshot().earlyCrashCount),
                static_cast<unsigned>(bootGuard.IsSafeModeActive()));
    // The successful update's intent is consumed: another unmarked software
    // reset before health must still reach the ordinary crash threshold.
    bootGuard.OnBootStart();
    Check(bootGuard.GetSnapshot().earlyCrashCount == 3U &&
              bootGuard.IsSafeModeActive() && bootGuard.IsLastResetCrashy(),
          "OTA intent is consumed before the new boot can crash again");
    ota_runtime_test::SetBootResetReason(1U);
    bootGuard.OnBootStart();
    ota_runtime_test::SetBootResetReason(3U);
    bootGuard.OnBootStart();
    bootGuard.OnBootStart();
    // Even an armed intentional restart cannot hide a different reset cause.
    bootGuard.PrepareForIntentionalReboot();
    ota_runtime_test::SetBootResetReason(6U); // next boot's genuine task watchdog
    bootGuard.OnBootStart();
    Check(bootGuard.GetSnapshot().earlyCrashCount == 3U &&
              bootGuard.IsSafeModeActive() && bootGuard.IsLastResetCrashy(),
          "one-shot OTA intent cannot suppress the next genuine early crash");
    std::printf("[OTA_BOOT_GUARD] armed_watchdog early_crashes=%lu safe_mode=%u\n",
                static_cast<unsigned long>(bootGuard.GetSnapshot().earlyCrashCount),
                static_cast<unsigned>(bootGuard.IsSafeModeActive()));
    ota_runtime_test::SetBootResetReason(1U);
    bootGuard.OnBootStart();
    bootGuard.MarkBootHealthy();
    bootGuard.PrepareForIntentionalReboot();
    ota_runtime_test::SetBootResetReason(6U);
    bootGuard.OnBootStart();
    Check(bootGuard.GetSnapshot().earlyCrashCount == 0U &&
              !bootGuard.IsSafeModeActive(),
          "intent preparation cannot rearm early-crash tracking after healthy runtime");
    std::printf("[OTA_BOOT_GUARD] healthy_teardown early_crashes=%lu safe_mode=%u\n",
                static_cast<unsigned long>(bootGuard.GetSnapshot().earlyCrashCount),
                static_cast<unsigned>(bootGuard.IsSafeModeActive()));
    egress.SetNotifyHook(nullptr);
    Check(legacy.NotifyCount() == 0U, "done delivery never duplicates onto retired legacy characteristic");
}

void TestConnectionReplacementDiscardsOldReliableProgressAndError() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    NimBLECharacteristic legacy;
    Check(Peer::Configure(service, settings, control) &&
              Peer::ActivateMinimumConnectionObjectSession(service), "generation fixture owns actual shared status adapter");
    Peer::PrepareProducerGeneration(service);
    Peer::ConfigureOtaNotifications(service, legacy);
    Check(Peer::FillSharedReverseWindow(service), "generation fixture denies actual reverse admission");
    Peer::NotifyOtaJson(service, legacy, R"({"v":"old-generation"})");
    Peer::NotifyProgress(service);
    Peer::NotifyTimeoutError(service);
    service.OnDisconnected(19);
    service.DrainEventQueue();
    service.OnConnected(7U, "new-status-generation", 23U, 1U);
    service.DrainEventQueue();
    const uint32_t generation = Peer::CurrentConnectionEpoch(service);
    Check(generation != 7U && Peer::ActivateMinimumConnectionObjectSession(service, generation),
          "replacement reconnect negotiates a distinct real connection session");
    Check(QueueSharedStatusCommand(service, R"({"cmd":"info"})", generation),
          "fresh shared info query reaches production reassembly in new generation");
    SharedStatusRecords capture;
    for (unsigned attempt = 0U; attempt < 64U; ++attempt) {
        service.DrainEventQueue();
        capture.Take(service);
    }
    Check(capture.intact && !capture.active &&
              capture.records == std::vector<std::string>{R"({"i":[3,0,63]})"} && legacy.NotifyCount() == 0U,
          "new generation receives only fresh info without old reliable progress or terminal error leakage");
}

void TestSharedLitchcraftLifecycleAtMinimumMtu() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    auto fragments = [](const uint8_t* bytes, const size_t size) {
        std::vector<std::vector<uint8_t>> out;
        size_t offset = 0U;
        bool first = true;
        while (offset < size) {
            const size_t header = first ? 4U : 2U;
            const size_t count = std::min<size_t>(10U - header, size - offset);
            const bool last = offset + count == size;
            std::vector<uint8_t> fragment(header + count, 0U);
            fragment[0U] = 1U;
            fragment[1U] = (first ? 1U : 0U) | (last ? 2U : 0U);
            if (first) {
                fragment[2U] = static_cast<uint8_t>(size & 0xffU);
                fragment[3U] = static_cast<uint8_t>(size >> 8U);
            }
            std::copy_n(bytes + offset, count, fragment.data() + header);
            out.push_back(fragment);
            offset += count;
            first = false;
        }
        return out;
    };

    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService ingress;
    Check(Peer::Configure(ingress, settings, control),
          "shared Litchcraft ingress fixture initializes bounded queues");
    const std::array<uint8_t, 8U> inspect{{0x4cU, 12U, 0U, 0U,
                                           0U, 0U, 9U, 0U}};
    const auto inspectFragments = fragments(inspect.data(), inspect.size());
    for (size_t index = 0U; index < inspectFragments.size(); ++index) {
        const auto& fragment = inspectFragments[index];
        Check(Peer::HandleConnectionLitchcraft(
                  ingress, fragment.data(), fragment.size(),
                  static_cast<uint32_t>(60U + index), 7U),
              "shared Litchcraft command fragment is admitted");
    }
    std::vector<uint8_t> command;
    uint32_t ordinal = 0U;
    uint32_t epoch = 0U;
    Check(Peer::TakeLitchcraftEvent(ingress, command, ordinal, epoch) &&
              command == std::vector<uint8_t>(inspect.begin(), inspect.end()) &&
              ordinal == 60U + inspectFragments.size() - 1U && epoch == 7U,
          "shared Litchcraft command commits one exact existing event");

    const std::array<uint8_t, 15U> artwork{{
        0x4cU, 0x40U, 1U, 0U, 0U, 0U, 1U, 0U,
        0U, 0U, 1U, 1U, 3U, 2U, 1U}};
    const auto artworkFragments = fragments(artwork.data(), artwork.size());
    for (size_t index = 0U; index < artworkFragments.size(); ++index) {
        const auto& fragment = artworkFragments[index];
        Check(Peer::HandleConnectionLitchcraft(
                  ingress, fragment.data(), fragment.size(),
                  static_cast<uint32_t>(70U + index), 7U),
              "shared artwork fragment is admitted");
    }
    std::vector<uint8_t> queuedArtwork;
    Check(Peer::TakeOtaData(ingress, queuedArtwork, ordinal) &&
              queuedArtwork ==
                  std::vector<uint8_t>(artwork.begin(), artwork.end()) &&
              ordinal == 70U + artworkFragments.size() - 1U,
          "shared artwork data reaches the existing bulk owner exactly once");

    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::GamePersistenceCoordinator game(settings, engine, state);
    modules::BleService egress;
    NimBLECharacteristic legacy;
    Peer::ConfigureLitchcraft(egress, game, legacy);
    Check(Peer::ActivateMinimumConnectionObjectSession(egress),
          "shared Litchcraft egress activates a minimum-MTU Object stream");
    const std::array<uint8_t, 8U> enter{{0x4cU, 1U, 0U, 0U,
                                        0U, 0U, 1U, 0U}};
    game.Litchcraft().HandlePacket(enter.data(), enter.size(), true, 0U);
    modules::LitchcraftSession::BulkPacket expected;
    Check(game.Litchcraft().PeekPacket(
              expected, modules::ConnectionSession::OBJECT_PACKET_BYTES),
          "Litchcraft owner exposes the exact pending receipt");
    std::vector<uint8_t> reconstructed;
    std::vector<uint8_t> fragment;
    bool first = true;
    while (Peer::TakeSharedLitchcraftFragment(egress, fragment)) {
        const bool begins = (fragment[1U] & 1U) != 0U;
        const bool ends = (fragment[1U] & 2U) != 0U;
        const size_t header = begins ? 4U : 2U;
        Check(fragment.size() <= 10U && begins == first,
              "shared Litchcraft receipt respects minimum Object bounds");
        reconstructed.insert(reconstructed.end(), fragment.begin() + header,
                             fragment.end());
        first = false;
        if (ends) break;
    }
    Check(reconstructed == std::vector<uint8_t>(
              expected.bytes.begin(), expected.bytes.begin() + expected.size) &&
              legacy.NotifyCount() == 0U,
          "shared Litchcraft receipt preserves exact bytes without direct GATT");
#endif
}

void TestBootstrapRestartRenewsOnlyItsNativeConnection() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    Check(Peer::Configure(service, settings, control), "renewal initializes production ingress");
    Peer::ConfigureRenewalServer(service);
    auto* server = NimBLEDevice::getServer();
    auto before = server->DisconnectCount();
    service.OnConnected(7U, "renewal", 247U, 1U);
    service.DrainEventQueue();
    // Ownership-only cases control the activation latch; the independent
    // VirtualLitch journey negotiates actual Hello/Ready and Object traffic.
    Peer::SetConnectionSessionActivated(service, true);
    constexpr char probe[] = "{\"cmd\":\"info\"}";
    for (const char* invalid : {"{\"cmd\":\"ping\"}", "{\"cmd\":\"ota_abort\"}",
             "{\"cmd\":\"info\",\"x\":1}", "{\"cmd\":\"info\"}x", "{\"cmd\":\"device_info\"}", "{}"}) {
        service.OnOtaControlCharacteristicWrite(invalid, std::strlen(invalid));
        Peer::PollRenewal(service);
        Check(server->DisconnectCount() == before, "other/malformed direct operations never request renewal");
    }
    service.OnOtaControlCharacteristicWrite(probe, sizeof(probe)); // trailing NUL is not exact
    Peer::PollRenewal(service);
    Check(server->DisconnectCount() == before, "non-exact probe length never renews");
    server->SetDisconnectAccepted(false);
    service.OnOtaControlCharacteristicWrite(probe, sizeof(probe) - 1U);
    Peer::PollRenewal(service);
    Check(server->DisconnectCount() == before + 1U && !Peer::RenewalKeepsActivation(service),
          "rejected SDK request leaves the existing idle disconnect owner available");
    server->SetDisconnectAccepted(true);
    before = server->DisconnectCount();
    for (unsigned repeat = 0U; repeat < 3U; ++repeat)
        service.OnOtaControlCharacteristicWrite(probe, sizeof(probe) - 1U);
    Check(server->DisconnectCount() == before, "characteristic callback never dispatches native teardown");
    Peer::PollRenewal(service);
    Check(server->DisconnectCount() == before + 1U && server->LastDisconnect() == 7U &&
              Peer::RenewalKeepsActivation(service) && !Peer::HasQueuedEvent(service),
          "loop requests one real disconnect while preserving activation and rejecting command admission");
    service.OnOtaControlCharacteristicWrite(probe, sizeof(probe) - 1U);
    Peer::PollRenewal(service);
    Check(server->DisconnectCount() == before + 1U, "repeated probes coalesce after dispatch");

    service.OnDisconnected(7U, 19);
    service.OnConnected(7U, "next", 247U, 1U);
    service.DrainEventQueue();
    Peer::SetConnectionSessionActivated(service, true);
    const auto capturedEpoch = Peer::CurrentConnectionEpoch(service);
    service.OnOtaControlCharacteristicWrite(probe, sizeof(probe) - 1U);
    service.OnDisconnected(7U, 19);
    service.OnConnected(7U, "successor", 247U, 1U);
    service.DrainEventQueue();
    Peer::SetConnectionSessionActivated(service, true);
    Peer::PollRenewal(service);
    Check(server->DisconnectCount() == before + 1U &&
              Peer::CurrentConnectionEpoch(service) != capturedEpoch,
          "pending old epoch cannot disconnect successor even when its native handle is reused");
    const auto currentEpoch = Peer::CurrentConnectionEpoch(service);
    Peer::PublishCapturedRenewal(service, currentEpoch, 8U);
    Peer::PollRenewal(service);
    Check(server->DisconnectCount() == before + 1U, "request handle must match its current native epoch");
    service.OnOtaControlCharacteristicWrite(probe, sizeof(probe) - 1U);
    Peer::PollRenewal(service);
    Check(server->DisconnectCount() == before + 2U && server->LastDisconnect() == 7U,
          "successor can request its own independently fenced renewal");
    service.OnDisconnected(7U, 19);
    service.OnConnected(7U, "idle-disconnect-pending", 247U, 1U);
    service.DrainEventQueue();
    Peer::SetConnectionSessionActivated(service, true);
    // Hold the SDK's disconnect callback after the existing idle owner has
    // issued its request. This is the same pending flag production Update owns.
    Check(server->disconnect(7U), "idle owner native disconnect request is accepted");
    Peer::SetPendingNativeDisconnect(service);
    const auto pendingDisconnects = server->DisconnectCount();
    service.OnOtaControlCharacteristicWrite(probe, sizeof(probe) - 1U);
    Peer::PollRenewal(service);
    Check(server->DisconnectCount() == pendingDisconnects && Peer::RenewalKeepsActivation(service),
          "bootstrap renewal joins an already pending idle disconnect without issuing a second SDK request");
    std::puts("[NATIVE_SESSION_RENEWAL] exact_probe=1 callback_dispatch=0 coalesced=1 stale_epoch_rejected=1 wrong_handle_rejected=1");
#endif
}

void TestActivatedSessionRejectsRetiredDirectOtaIngress() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    Check(Peer::Configure(service, settings, control),
          "retired direct OTA fixture initializes bounded ingress");
    Peer::SetConnectionSessionActivated(service, true);
    constexpr const char* begin = R"({"cmd":"ota_begin"})";
    service.OnOtaControlCharacteristicWrite(begin, std::strlen(begin));
    const std::array<uint8_t, 3U> data{{1U, 2U, 3U}};
    Check(!service.OnOtaDataCharacteristicWrite(data.data(), data.size()) &&
              !Peer::HasQueuedEvent(service),
          "activated shared session retires direct OTA control and data ingress");
    const std::array<uint8_t, 15U> artwork{{
        0x4cU, 0x40U, 1U, 0U, 0U, 0U, 1U, 0U,
        0U, 0U, 1U, 1U, 0U, 0U, 0U}};
    Check(!service.OnOtaDataCharacteristicWrite(
              artwork.data(), artwork.size()),
          "activated session retires transitional artwork ingress too");
    const std::array<uint8_t, 9U> store{{
        0x4dU, 2U, 18U, 1U, 0U, 0U, 0U, 1U, 0U}};
    Check(!service.OnOtaDataCharacteristicWrite(store.data(), store.size()),
          "activated session does not reopen retired Store ingress");
#endif
}

void TestReconnectRetiresOldSessionBeforeCapabilityProbe() {
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    Check(Peer::Configure(service, settings, control),
          "reconnect capability fixture initializes bounded ingress");
    Peer::SetConnectionSessionActivated(service, true);

    // A long save may delay the connection projection's main-loop drain.
    // The first info request on the new BLE link must not inherit the old
    // link's retired direct-ingress flag while that projection is pending.
    service.OnDisconnected(19);
    service.OnConnected(8U, "replacement", 247U, 1U);
    constexpr const char* info = R"({"cmd":"info"})";
    service.OnOtaControlCharacteristicWrite(info, std::strlen(info));

    std::string received;
    uint32_t ordinal = 0U;
    uint32_t otaEpoch = 0U;
    uint32_t connectionEpoch = 0U;
    Check(Peer::TakeOtaControl(service, received, ordinal, otaEpoch,
                               connectionEpoch) &&
              received == info &&
              connectionEpoch == Peer::CurrentConnectionEpoch(service),
          "fresh-link capability probe is admitted before lifecycle drain");
#endif
}
} // namespace

namespace runtime {
void DelayNotifyTestSlice(const uint32_t milliseconds) {
    if (NotifyTestProbe::active != nullptr) {
        NotifyTestProbe::active->Pause(milliseconds);
    }
}
} // namespace runtime

#include "owner_pairing_cases.hpp"

namespace {
using Peer = modules::BleServiceConnectionEpochTestPeer;
namespace session_wire = modules::connection_session_codec;

struct ReverseProgressFixture {
    fxe::ScopedMillisProviderOverride clock{&fxe::ManualMillisProvider};
    Harness harness;
    modules::BleService service;
    NimBLECharacteristic egress;
    std::vector<uint32_t> sent;
    std::vector<std::string> bytes;

    explicit ReverseProgressFixture(const unsigned packets) {
        Check(Peer::Configure(service, harness.settings, harness.controlMutation),
              "proof initializes existing production peer queues");
        Peer::PrepareProducerGeneration(service);
        Peer::ConfigureConnectionSessionNotifications(service, egress);
        Peer::SubscribeDiagnosticState(service);
        Check(Peer::ActivateMinimumConnectionObjectSession(service),
              "proof activates existing eight-credit minimum-MTU session");
        egress.SetNotifyHook([](void* context) {
            auto& self = *static_cast<ReverseProgressFixture*>(context);
            session_wire::PacketView packet;
            const auto& bytes = self.egress.LastTx();
            if (session_wire::Codec::Decode(
                    {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()},
                    packet, 20U) == session_wire::Error::None &&
                packet.header.kind == session_wire::Kind::Object &&
                packet.header.code == 0x7fU) {
                self.sent.push_back(packet.header.sequence);
                self.bytes.push_back(bytes);
            }
        }, this);
        for (unsigned index = 1U; index <= packets; ++index) {
            const std::array<uint8_t, 1U> body{{static_cast<uint8_t>(index)}};
            Check(Peer::Session(service).PublishReverseObject(
                      0x7fU, index, body.data(), body.size()),
                  "proof publishes immutable reverse Object through existing owner");
        }
    }

    void Update() { Peer::UpdateSharedStates(service); }

    void Progress(const uint32_t confirmed, const uint16_t ahead) {
        std::array<uint8_t, 10U> body{};
        fxe::WriteU32LE(confirmed, body.data());
        fxe::WriteU32LE(confirmed + 8U, body.data() + 4U);
        fxe::WriteU16LE(ahead, body.data() + 8U);
        std::array<uint8_t, 20U> packet{};
        size_t size = 0U;
        Check(session_wire::Codec::Encode(
                  {{session_wire::Kind::Control,
                    uint8_t(session_wire::Control::Progress),
                    Peer::Session(service).ReverseObjectStreamToken(), confirmed},
                   {body.data(), body.size()}},
                  {packet.data(), packet.size()}, size) == session_wire::Error::None,
              "phone-shaped cumulative Progress uses production codec");
        const size_t before = sent.size();
        service.OnConnectionSessionWrite(packet.data(), size);
        Check(sent.size() == before,
              "actual SDK write callback queues without notifying inline");
        service.DrainEventQueue();
    }

    void Query() {
        std::array<uint8_t, 20U> packet{};
        size_t size = 0U;
        Check(session_wire::Codec::Encode(
                  {{session_wire::Kind::Control,
                    uint8_t(session_wire::Control::Query),
                    Peer::Session(service).ReverseObjectStreamToken(), 0U}, {}},
                  {packet.data(), packet.size()}, size) == session_wire::Error::None,
              "reverse Query uses existing production codec");
        service.OnConnectionSessionWrite(packet.data(), size);
        service.DrainEventQueue();
    }
};

void TestReverseObjectRefillAfterPartialReplayKeepsSequenceOrder() {
    fxe::ManualMillis::SetNowMs(8000U);
    ReverseProgressFixture fixture(8U);
    fixture.Update(); fixture.Update();
    Check(fixture.sent == std::vector<uint32_t>({1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}),
          "ordering witness begins with all eight accepted native first sends");
    if (fixture.sent.size() != 8U) return;
    fxe::ManualMillis::AdvanceMs(121U); fixture.Update();
    Check(fixture.sent.size() == 12U &&
          std::equal(fixture.sent.begin() + 8U, fixture.sent.end(), fixture.sent.begin()) &&
          Peer::Session(fixture.service).SuccessfulReverseObjectReplays() == 4U,
          "one bounded update accepts exactly four timer repairs before the delayed cumulative receipt");
    if (fixture.sent.size() != 12U) return;
    fixture.Progress(8U, 0U);
    Check(fixture.sent.size() == 12U, "cumulative8 retires old retained slots without another native replay");
    for (uint32_t sequence = 9U; sequence <= 16U; ++sequence) {
        const uint8_t body = static_cast<uint8_t>(sequence);
        Check(Peer::Session(fixture.service).PublishReverseObject(0x7fU, sequence, &body, 1U),
              "cumulative credit admits the exact next eight immutable Objects");
    }
    std::array<uint8_t, 20U> selectedBefore{}, selectedAfter{};
    size_t beforeSize = 0U, afterSize = 0U;
    Check(Peer::Session(fixture.service).PeekOutput(7U, selectedBefore.data(), selectedBefore.size(), beforeSize),
          "fresh native refusal begins with an actual selected retained packet");
    fixture.egress.SetNotifyResult(false); fixture.Update();
    Check(Peer::Session(fixture.service).PeekOutput(7U, selectedAfter.data(), selectedAfter.size(), afterSize) &&
          beforeSize == afterSize && std::equal(selectedBefore.begin(), selectedBefore.begin() + beforeSize, selectedAfter.begin()) &&
          fixture.sent.size() == 12U && fixture.service.GetSessionDiagnostics().notifyFailures == 1U,
          "native refusal produces no accepted packet and preserves the fresh selected output");
    fixture.egress.SetNotifyResult(true); fixture.Update(); fixture.Update();
    Check(fixture.bytes.size() > 12U && fixture.bytes[12U] ==
          std::string(reinterpret_cast<const char*>(selectedBefore.data()), beforeSize),
          "first actual accepted native packet is byte-identical to the refused selection");
    const std::vector<uint32_t> fresh(fixture.sent.begin() + 12U, fixture.sent.end());
    Check(fresh == std::vector<uint32_t>({9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U}),
          "reused physical slots cannot make newer first sends overtake the oldest pending transport sequence");
    bool exact = true;
    for (size_t index = 12U; index < fixture.bytes.size(); ++index) {
        const auto& encoded = fixture.bytes[index]; session_wire::PacketView packet;
        exact = exact && session_wire::Codec::Decode(
            {reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()}, packet, 20U) == session_wire::Error::None &&
            packet.body.Size() == 1U && packet.body[0U] == packet.header.sequence;
    }
    Check(exact && Peer::Session(fixture.service).SuccessfulReverseObjectReplays() == 4U,
          "fresh sends preserve exact bytes and never count as accepted retransmissions");
    std::printf("[REVERSE_RECYCLED_ORDER] initial=1..8 partial_repairs=4 confirmed=8 native_false=1 fresh=");
    for (size_t index = 0U; index < fresh.size(); ++index)
        std::printf("%s%lu", index == 0U ? "" : ",", static_cast<unsigned long>(fresh[index]));
    std::printf(" exact=%u\n", exact);
}

void TestReverseObjectPartialProgressKeepsInFlightTail() {
    fxe::ManualMillis::SetNowMs(1000U);
    ReverseProgressFixture fixture(4U);
    fixture.Update();
    Check(fixture.sent == std::vector<uint32_t>{1U, 2U, 3U, 4U},
          "one actual notification update sends exactly four original packets");
    const auto original = fixture.bytes;
    fixture.Progress(1U, 0U);
    unsigned repeated = 0U;
    bool exact = true;
    auto unique = fixture.sent;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    for (size_t index = 4U; index < fixture.sent.size(); ++index) {
        const uint32_t sequence = fixture.sent[index];
        if (sequence >= 2U && sequence <= 4U) {
            ++repeated;
            exact = exact && fixture.bytes[index] == original[sequence - 1U];
        }
    }
    std::printf("[REVERSE_PARTIAL_PROGRESS] now_ms=%lu confirmed=1 terminal=1 grant=9 ahead=0 first=1,2,3,4 after=",
                static_cast<unsigned long>(fxe::Millis()));
    for (size_t index = 4U; index < fixture.sent.size(); ++index)
        std::printf("%s%lu", index == 4U ? "" : ",",
                    static_cast<unsigned long>(fixture.sent[index]));
    std::printf(" unique=%zu repeated=%u exact=%u elapsed_ms=0 retry_due_ms=121\n",
                unique.size(), repeated, exact);
    Check(exact, "premature duplicate packets retain exact immutable bytes");
    Check(repeated == 0U,
          "partial advancing Progress must not immediately resend an in-flight tail before121ms");
}

void TestReverseObjectKnownHoleStillRepairsSelectively() {
    fxe::ManualMillis::SetNowMs(2000U);
    ReverseProgressFixture fixture(8U);
    fixture.Update(); fixture.Update();
    Check(fixture.sent.size() == 8U, "known-hole control sends complete eight-slot window");
    fixture.Progress(1U, 0x007eU);
    Check(fixture.sent.size() == 9U && fixture.sent.back() == 2U &&
              fixture.bytes.back() == fixture.bytes[1U],
          "actual Progress with later set bits repairs only exact missing sequence2");
    std::puts("[REVERSE_KNOWN_HOLE] confirmed=1 ahead=0x007e immediate_repair=2 exact=1");
}

void TestReverseObjectLostTailRetainsTimedAndQueriedRecovery() {
    fxe::ManualMillis::SetNowMs(3000U);
    ReverseProgressFixture fixture(4U);
    fixture.Update();
    const auto original = fixture.bytes;
    fxe::ManualMillis::AdvanceMs(120U); fixture.Update();
    Check(fixture.sent.size() == 4U, "unacknowledged reverse tail is quiet before121ms");
    fxe::ManualMillis::AdvanceMs(1U); fixture.Update();
    Check(fixture.sent.size() == 8U &&
              std::equal(original.begin(), original.end(), fixture.bytes.begin() + 4U),
          "normal121ms timer recovers every exact unacknowledged packet including lost tail4");
    fixture.Query();
    Check(fixture.sent.size() == 12U &&
              std::equal(original.begin(), original.end(), fixture.bytes.begin() + 8U),
          "actual Query independently recovers the same retained reverse tail");
    std::puts("[REVERSE_TAIL_RECOVERY] before120_ms=quiet timer121_ms=exact1..4 query=exact1..4");
}

void TestReverseObjectNotifyFalseKeepsEpochAndNextTransferSafe() {
    fxe::ManualMillis::SetNowMs(4000U);
    ReverseProgressFixture fixture(4U);
    std::array<uint8_t, 20U> before{}, after{};
    size_t beforeSize = 0U, afterSize = 0U;
    Check(Peer::Session(fixture.service).PeekOutput(7U, before.data(), before.size(), beforeSize),
          "notify-false control selects current retained packet");
    fixture.egress.SetNotifyResult(false); fixture.Update();
    Check(fixture.sent.empty() && fixture.service.GetSessionDiagnostics().notifyFailures == 1U &&
              Peer::Session(fixture.service).PeekOutput(7U, after.data(), after.size(), afterSize) &&
              beforeSize == afterSize && std::equal(before.begin(), before.begin() + beforeSize, after.begin()) &&
              !Peer::Session(fixture.service).PeekOutput(8U, after.data(), after.size(), afterSize),
          "actual notify(false) retains exact current-epoch output and rejects another epoch");
    fixture.egress.SetNotifyResult(true); fixture.Update();
    Check(fixture.sent == std::vector<uint32_t>{1U, 2U, 3U, 4U},
          "successful notification resumes exact denied first output");
    fixture.Progress(4U, 0U);
    const std::array<uint8_t, 1U> body{{0xa5U}};
    Check(Peer::Session(fixture.service).PublishReverseObject(0x7fU, 99U, body.data(), body.size()),
          "confirmation frees credit for subsequent transfer output");
    fixture.Update();
    Check(fixture.sent.back() == 5U, "next transfer cannot inherit stale selected sequence");
    std::puts("[REVERSE_NOTIFY_FALSE] retained_exact=1 current_epoch=7 wrong_epoch8=rejected next_sequence=5");
}
}

namespace {
void TestReverseObjectPerPacketProgressTransfersExactBody(const bool afterPartialReplay = false) {
    fxe::ManualMillis::SetNowMs(5000U);
    ReverseProgressFixture fixture(0U);
    constexpr uint32_t packets = 32U;
    constexpr size_t bodySize = 8U;
    std::array<uint8_t, packets * bodySize> expected{};
    for (size_t index = 0U; index < expected.size(); ++index)
        expected[index] = static_cast<uint8_t>(index * 37U + 11U);
    uint32_t published = 0U;
    const auto fill = [&] {
        while (published < packets &&
               Peer::Session(fixture.service).PublishReverseObject(
                   0x7fU, published + 1U, expected.data() + published * bodySize, bodySize))
            ++published;
    };
    fill();
    Check(published == 8U, "per-packet phone schedule fills the actual eight-slot reverse window");
    std::vector<uint8_t> assembled;
    uint32_t confirmed = 0U;
    size_t cursor = 0U;
    if (afterPartialReplay) {
        fixture.Update(); fixture.Update();
        Check(fixture.sent.size() == 8U, "mixed schedule accepts its initial complete native window");
        if (fixture.sent.size() != 8U) return;
        for (size_t index = 0U; index < 8U; ++index) {
            const auto& encoded = fixture.bytes[index]; session_wire::PacketView packet;
            const bool decoded = session_wire::Codec::Decode(
                {reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()}, packet, 20U) == session_wire::Error::None;
            Check(decoded && packet.header.sequence == index + 1U && packet.body.Size() == bodySize,
                  "delayed cumulative prefix is justified by actual accepted immutable native bytes");
            if (!decoded || packet.body.Size() != bodySize) return;
            assembled.insert(assembled.end(), packet.body.Data(), packet.body.Data() + packet.body.Size());
        }
        fxe::ManualMillis::AdvanceMs(121U); fixture.Update();
        Check(fixture.sent.size() == 12U, "mixed schedule accepts only four legitimate timed repairs");
        if (fixture.sent.size() != 12U) return;
        fixture.Progress(8U, 0U);
        Check(fixture.sent.size() == 12U, "mixed schedule retires the delayed prefix before publishing the next window");
        if (fixture.sent.size() != 12U) return;
        confirmed = 8U; cursor = fixture.bytes.size();
        fill();
        fixture.egress.SetNotifyResult(false); fixture.Update();
        Check(fixture.bytes.size() == cursor, "mixed schedule native refusal never becomes a received packet");
        fixture.egress.SetNotifyResult(true);
    }
    fixture.Update();
    Check(fixture.sent.size() == (afterPartialReplay ? 16U : 4U),
          "phone callbacks begin behind the actual four-notify update");
    std::array<std::string, packets + 1U> received{};
    for (unsigned turn = 0U; turn < 2048U &&
         (confirmed < packets || cursor < fixture.bytes.size()); ++turn) {
        if (cursor == fixture.bytes.size()) { fixture.Update(); continue; }
        session_wire::PacketView packet;
        const auto& encoded = fixture.bytes[cursor++];
        const bool decoded = session_wire::Codec::Decode(
            {reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()}, packet, 20U) == session_wire::Error::None;
        Check(decoded && packet.header.sequence >= 1U && packet.header.sequence <= packets,
              "phone schedule receives only current reverse transfer sequences");
        if (!decoded || packet.header.sequence == 0U || packet.header.sequence > packets) break;
        auto& retained = received[packet.header.sequence];
        Check(retained.empty() || retained == encoded, "phone schedule rejects changed duplicate bytes");
        retained = encoded;
        while (confirmed < packets && !received[confirmed + 1U].empty()) {
            const auto& next = received[++confirmed];
            session_wire::PacketView delivered;
            Check(session_wire::Codec::Decode(
                      {reinterpret_cast<const uint8_t*>(next.data()), next.size()}, delivered, 20U) == session_wire::Error::None,
                  "phone schedule decodes the newly contiguous immutable packet");
            assembled.insert(assembled.end(), delivered.body.Data(), delivered.body.Data() + delivered.body.Size());
        }
        uint16_t ahead = 0U;
        for (uint32_t sequence = confirmed + 1U; sequence <= packets && sequence - confirmed <= 16U; ++sequence)
            if (!received[sequence].empty()) ahead |= static_cast<uint16_t>(1U << (sequence - confirmed - 1U));
        // Dart's same cumulative cursor/bitmap, with feedback writes completing
        // between incoming SDK callbacks, including duplicate callbacks.
        fixture.Progress(confirmed, ahead);
        fill();
        if (afterPartialReplay && turn % 7U == 3U) fixture.egress.SetNotifyResult(false);
        fixture.Update();
        fixture.egress.SetNotifyResult(true);
    }
    Check(confirmed == packets && assembled == std::vector<uint8_t>(expected.begin(), expected.end()),
          "per-packet phone feedback completes the exact full body");
    Check(fixture.sent.size() == packets + (afterPartialReplay ? 4U : 0U),
          "per-packet feedback never amplifies fresh windows after the four explicitly required timed repairs");
    std::printf("[REVERSE_PER_PACKET_BODY] confirmed=%lu published=%lu notifications=%zu bytes=%zu expected_bytes=%zu exact=%u partial_replay=%u\n",
                static_cast<unsigned long>(confirmed), static_cast<unsigned long>(published),
                fixture.sent.size(), assembled.size(), expected.size(),
                assembled == std::vector<uint8_t>(expected.begin(), expected.end()) ? 1U : 0U,
                static_cast<unsigned>(afterPartialReplay));
}

void TestReverseObjectDelayedFeedbackAndBitmapBoundary() {
    fxe::ManualMillis::SetNowMs(6000U);
    ReverseProgressFixture fixture(8U);
    fixture.Update(); fixture.Update();
    fxe::ManualMillis::AdvanceMs(120U);
    fixture.Update();
    Check(fixture.sent.size() == 8U, "held reverse feedback remains quiet before121ms");
    fixture.Progress(4U, 0U);
    Check(fixture.sent.size() == 8U, "delayed cumulative prefix does not invent loss in the in-flight tail");
    std::array<uint8_t, 10U> body{};
    fxe::WriteU32LE(4U, body.data());
    fxe::WriteU32LE(12U, body.data() + 4U);
    fxe::WriteU16LE(0x8000U, body.data() + 8U);
    std::array<uint8_t, 20U> packet{};
    size_t size = 0U;
    Check(session_wire::Codec::Encode(
              {{session_wire::Kind::Control, uint8_t(session_wire::Control::Progress),
                Peer::Session(fixture.service).ReverseObjectStreamToken(), 4U},
               {body.data(), body.size()}}, {packet.data(), packet.size()}, size) == session_wire::Error::Control,
          "bitmap bit15 outside the eight-credit grant is rejected by production codec");
    fxe::WriteU32LE(20U, body.data() + 4U);
    Check(session_wire::Codec::Encode(
              {{session_wire::Kind::Control, uint8_t(session_wire::Control::Progress),
                Peer::Session(fixture.service).ReverseObjectStreamToken(), 4U},
               {body.data(), body.size()}}, {packet.data(), packet.size()}, size) == session_wire::Error::None,
          "generic sixteen-bit bitmap remains codec-valid at bit15");
    fixture.service.OnConnectionSessionWrite(packet.data(), size);
    fixture.service.DrainEventQueue();
    Check(fixture.service.GetSessionDiagnostics().lastIngressResult ==
              static_cast<uint8_t>(modules::ConnectionSession::Result::Conflict),
          "actual ingress rejects sixteen-credit Progress against negotiated eight-slot reverse window");
    fixture.Progress(8U, 0U);
    std::puts("[REVERSE_DELAYED_PROGRESS] held120_ms=quiet cumulative4=quiet bit15=bounded_rejection cumulative8=retired");
}

void TestReverseObjectHighestNegotiatedHoleBitKeepsCreditHeld() {
    fxe::ManualMillis::SetNowMs(7000U);
    ReverseProgressFixture fixture(8U);
    fixture.Update(); fixture.Update();
    fixture.Progress(0U, 0x0080U);
    fixture.Update();
    bool exact = fixture.sent.size() == 15U;
    for (size_t index = 0U; exact && index < 7U; ++index)
        exact = fixture.sent[8U + index] == index + 1U &&
            fixture.bytes[8U + index] == fixture.bytes[index];
    Check(exact, "highest negotiated positive bit repairs only its seven preceding holes");
    const std::array<uint8_t, 1U> body{{0xa5U}};
    Check(!Peer::Session(fixture.service).PublishReverseObject(0x7fU, 9U, body.data(), body.size()),
          "received-ahead evidence alone cannot release reverse slots or extend credit");
    fixture.Progress(8U, 0U);
    Check(Peer::Session(fixture.service).PublishReverseObject(0x7fU, 9U, body.data(), body.size()),
          "only cumulative confirmation releases the full reverse window");
    fixture.Update();
    Check(fixture.sent.back() == 9U, "confirmed window resumes with its next immutable sequence");
    std::puts("[REVERSE_HIGH_BIT_CREDIT] bit7=selective1..7 received_only=held confirmed8=sequence9");
}
}

struct ObjectBenchmarkCapture {
    NimBLECharacteristic characteristic;
    std::vector<std::string> packets;
    ObjectBenchmarkCapture() {
        characteristic.SetNotifyHook([](void* context) {
            auto& self = *static_cast<ObjectBenchmarkCapture*>(context);
            self.packets.push_back(self.characteristic.LastTx());
        }, this);
    }
};

// Consume the real native notification hook, not Session::PeekOutput: native
// admission must be live before a derived shared control can reach its owner.
bool CollectBenchmarkNativeStatus(modules::BleService& service,
                                 ObjectBenchmarkCapture& capture,
                                 std::vector<std::string>& records) {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    std::string assembling;
    size_t declared = 0U;
    bool active = false, intact = true;
    for (unsigned attempt = 0U; attempt < 128U; ++attempt) {
        Peer::UpdateSharedStates(service);
        std::vector<std::string> packets;
        packets.swap(capture.packets);
        if (packets.empty()) break;
        for (const auto& encoded : packets) {
            wire::PacketView packet;
            if (wire::Codec::Decode({reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()},
                                   packet, 512U) != wire::Error::None) {
                intact = false; continue;
            }
            if (packet.header.kind != wire::Kind::Object) continue;
            if (packet.header.code != 0x32U || packet.body.Size() < 3U) {
                intact = false; continue;
            }
            const auto* body = packet.body.Data();
            const bool begins = (body[1U] & 1U) != 0U;
            const bool ends = (body[1U] & 2U) != 0U;
            const size_t header = begins ? 4U : 2U;
            if (body[0U] != 1U || (body[1U] & ~3U) != 0U ||
                packet.body.Size() <= header ||
                packet.body.Size() > Peer::BenchmarkReverseMaximum(service)) {
                intact = false; continue;
            }
            if (begins) {
                if (active) intact = false;
                assembling.clear(); active = true;
                declared = fxe::ReadU16LE(body + 2U);
            } else if (!active) intact = false;
            assembling.append(reinterpret_cast<const char*>(body + header), packet.body.Size() - header);
            if (ends) {
                intact = intact && active && assembling.size() == declared;
                records.push_back(assembling);
                std::printf("[OBJECT_BENCHMARK_STATUS] %s\n", assembling.c_str());
                assembling.clear(); active = false;
            }
            intact = Peer::ConfirmSharedReverseObject(service, packet.header.sequence) && intact;
        }
    }
    return intact && !active;
}

bool SendBenchmarkSharedObject(modules::BleService& service, uint32_t& sequence,
                              const uint8_t opcode, const uint8_t* body, const size_t size) {
    namespace wire = modules::connection_session_codec;
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    std::array<uint8_t, modules::ConnectionSession::OBJECT_PACKET_BYTES> bytes{};
    size_t encoded = 0U;
    if (wire::Codec::Encode({{wire::Kind::Object, opcode, Peer::ObjectToken(service),
                              sequence++}, {body, size}},
                           {bytes.data(), bytes.size()}, encoded) != wire::Error::None) return false;
    service.OnConnectionSessionWrite(bytes.data(), encoded);
    service.DrainEventQueue();
    return true;
}

bool SendBenchmarkSharedControl(modules::BleService& service, uint32_t& sequence,
                               const char* json, const size_t maximumBody) {
    const size_t total = std::strlen(json);
    size_t offset = 0U;
    while (offset < total) {
        std::array<uint8_t, modules::ConnectionSession::OBJECT_PACKET_BYTES -
                                modules::connection_session_codec::HEADER_BYTES> body{};
        const size_t header = offset == 0U ? 4U : 2U;
        const size_t count = std::min(maximumBody - header, total - offset);
        body[0U] = 1U;
        body[1U] = (offset == 0U ? 1U : 0U) | (offset + count == total ? 2U : 0U);
        if (offset == 0U) fxe::WriteU16LE(total, body.data() + 2U);
        std::copy_n(reinterpret_cast<const uint8_t*>(json) + offset, count, body.data() + header);
        if (!SendBenchmarkSharedObject(service, sequence,
                modules::ConnectionSession::OTA_CONTROL_OBJECT_OPCODE,
                body.data(), header + count)) return false;
        offset += count;
    }
    return true;
}

void TestObjectBenchmarkActualSharedReverseAndLinkObservations() {
    std::puts("[OBJECT_BENCHMARK_CASE] TestObjectBenchmarkActualSharedReverseAndLinkObservations");
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    using Bench = modules::ConnectionObjectBenchmark;
    fxe::ManualMillis::SetNowMs(1000U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    Harness harness; modules::BleService service; ObjectBenchmarkCapture capture;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "RAM benchmark owns actual ingress queues");
    Peer::PrepareProducerGeneration(service); Peer::MarkConnected(service, true);
    Peer::SubscribeDiagnosticState(service);
    Peer::ConfigureConnectionSessionNotifications(service, capture.characteristic);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, modules::ConnectionSession::OBJECT_CAPACITY, 37U),
          "real handshake negotiates a reverse body smaller than packet maximum");
    Peer::SetBenchmarkPhysicalHandle(service, 7U);
    service.OnConnectionParametersChanged(6U, 0U, 200U, false, 7U);
    service.OnLinkObservation(7U, 247U, 2U, 2U);
    service.DrainEventQueue();
    const auto link = service.GetLinkObservation();
    Check(link.mtu == 247U && link.intervalUnits == 6U && link.phyKnown && link.txPhy == 2U,
          "actual callback event seam captures negotiated MTU/interval/PHY without HCI calls");
    service.OnLinkObservation(9U, 517U, 1U, 1U); service.DrainEventQueue();
    Check(service.GetLinkObservation().mtu == 247U && service.GetLinkObservation().txPhy == 2U,
          "late foreign handle cannot rewrite current link observations");
    uint32_t sequence = 1U;
    Check(SendBenchmarkSharedControl(service, sequence,
        R"({"cmd":"object_bench_begin","q":1,"run":2,"size":300000,"seed":23063})", 234U),
        "reverse Begin traverses actual shared OTA-control adapter");
    Check(service.IsBulkTransferActive(), "admitted lifetime-owned benchmark is bulk activity");
    Check(Peer::BenchmarkReverseMaximum(service) == 37U, "benchmark uses actual Opened body selection");
    uint32_t received = 0U, crc = 0U; bool ended = false;
    for (unsigned turn = 0U; turn < 10000U && !ended; ++turn) {
        const auto before = capture.characteristic.NotifyCount();
        Peer::UpdateSharedStates(service);
        Check(capture.characteristic.NotifyCount() - before <= 4U, "actual service remains bounded to four notifications");
        std::vector<std::string> packets;
        packets.swap(capture.packets);
        for (const auto& encoded : packets) {
            wire::PacketView packet;
            if (wire::Codec::Decode({reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()}, packet, 244U) != wire::Error::None ||
                packet.header.kind != wire::Kind::Object) continue;
            if (packet.header.code == Bench::REVERSE_CODE) {
                Check(packet.body.Size() <= 37U && fxe::ReadU16LE(packet.body.Data() + 1U) == 2U,
                      "each real retained packet respects selected body and exact run");
                const auto* body = packet.body.Data();
                if (body[0U] == Bench::DATA_TAG) {
                    const size_t count = packet.body.Size() - 3U;
                    for (size_t i = 0U; i < count; ++i)
                        Check(body[i + 3U] == static_cast<uint8_t>((received + i) * 33U + 0x5a17U + ((received + i) >> 8U)),
                              "actual reverse producer carries every expected pattern byte");
                    crc = fxe::Crc32(body + 3U, count, ~crc); received += count;
                } else {
                    Check(body[0U] == Bench::END_TAG && packet.body.Size() == 10U &&
                          received == 300000U && crc == 0x8c064b0bU &&
                          (uint32_t(body[3U]) | uint32_t(body[4U]) << 8U | uint32_t(body[5U]) << 16U) == 300000U &&
                          fxe::ReadU32LE(body + 6U) == crc,
                          "only exact full payload and retained final CRC establish completion");
                    Check(!Peer::BenchmarkActive(service) && service.IsBulkTransferActive(),
                          "prepared End retains bulk activity through the actual Session delivery tail");
                    ended = true;
                }
            }
            Check(Peer::ConfirmSharedReverseObject(service, packet.header.sequence),
                  "only real cumulative reverse Progress returns retained credits");
        }
    }
    Check(ended && !service.IsBulkTransferActive(),
          "actual shared producer completes all300000 bytes and final ACK restores ordinary activity");
    std::printf("[OBJECT_BENCHMARK_REVERSE] bytes=%lu crc=%08lx selected_body=37 completed=%u\n",
        static_cast<unsigned long>(received), static_cast<unsigned long>(crc), unsigned(ended));
    Peer::RetireBenchmarkSession(service);
    Check(service.GetLinkObservation().phyKnown && !Peer::BenchmarkActive(service),
          "shared-session retirement retains physical PHY until actual disconnect");
    Check(!service.IsBulkTransferActive(), "retired benchmark lifetime cannot throttle ordinary rendering");
}

void TestObjectBenchmarkActualForwardPatternAndMinimumBegin() {
    std::puts("[OBJECT_BENCHMARK_CASE] TestObjectBenchmarkActualForwardPatternAndMinimumBegin");
#if defined(LITCH_BLE_OTA)
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    for (const bool corrupt : {false, true}) {
        Harness harness; modules::BleService service; NimBLECharacteristic legacy;
        Check(Peer::Configure(service, harness.settings, harness.controlMutation), "forward diagnostic fixture begins actual queues");
        Peer::PrepareProducerGeneration(service); Peer::MarkConnected(service, true);
        Peer::ConfigureOtaNotifications(service, legacy);
        Peer::SubscribeDiagnosticState(service);
        ObjectBenchmarkCapture capture;
        Peer::ConfigureConnectionSessionNotifications(service, capture.characteristic);
        Check(Peer::ActivateMinimumConnectionObjectSession(service), "forward fixture uses actual minimum-MTU session");
        uint32_t sequence = 1U;
        Check(SendBenchmarkSharedControl(service, sequence,
            R"({"cmd":"object_bench_probe","q":41})", 10U), "minimum-body capability probe uses actual shared control");
        std::vector<std::string> probeRecords;
        Check(CollectBenchmarkNativeStatus(service, capture, probeRecords), "minimum-body fragmented probe reassembles completely");
        bool schema = false;
        for (const auto& json : probeRecords) {
            StaticJsonDocument<768> probe;
            if (!deserializeJson(probe, json) && probe["q"] == 41 && probe["schema"] == 1 &&
                probe["max_bytes"] == 300000 && probe["code"] == 51 && probe["pattern"] == 1) schema = true;
        }
        Check(schema, "actual mandatory probe proves schema/q/code/max/pattern before Begin");
        Check(SendBenchmarkSharedControl(service, sequence,
            R"({"cmd":"ota_sim_begin","size":3,"chunk":20,"bench_schema":1,"run":1,"seed":2})", 10U),
              "pattern simulation Begin uses real shared fragmentation");
        std::vector<std::string> status;
        Check(CollectBenchmarkNativeStatus(service, capture, status) &&
              std::find(status.begin(), status.end(), R"({"b":1})") != status.end(),
              "actual ATT20 Begin semantics are compact b:1");
        uint8_t body[3U] = {2U, 35U, 68U};
        if (corrupt) body[1U] ^= 1U;
        Check(SendBenchmarkSharedObject(service, sequence, modules::ConnectionSession::OTA_DATA_OBJECT_OPCODE,
                                        body, sizeof(body)), "data traverses actual shared0x31 and simulated backend");
        if (corrupt) {
            Check(!service.IsOtaInProgress(), "one actual pattern byte mismatch aborts without reporting completion");
        } else {
            const auto crc = fxe::Crc32(body, sizeof(body));
            char end[96U] = {};
            std::snprintf(end, sizeof(end), "{\"cmd\":\"ota_end\",\"size\":3,\"crc32\":\"%08lx\"}", static_cast<unsigned long>(crc));
            Check(SendBenchmarkSharedControl(service, sequence, end, 10U), "End preserves existing exact size/CRC owner");
            status.clear(); Check(CollectBenchmarkNativeStatus(service, capture, status), "forward proof remains complete across min-MTU status fragments");
            bool proven = false;
            for (const auto& json : status) {
                StaticJsonDocument<768> value;
                if (!deserializeJson(value, json) && value["bench"] == 1 && value["pattern"] == 1 &&
                    value["rx"] == 3 && value["run"] == 1) proven = true;
            }
            Check(proven && !service.IsOtaInProgress() && !Peer::PendingReboot(service),
                  "only verified simulation done claims count+pattern+CRC and never requests reboot");
        }
    }
#endif
}

void TestObjectBenchmarkRefusalCancelAndDirectIngress() {
    std::puts("[OBJECT_BENCHMARK_CASE] TestObjectBenchmarkRefusalCancelAndDirectIngress");
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    fxe::ManualMillis::SetNowMs(1000U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    Harness harness; modules::BleService service; ObjectBenchmarkCapture capture;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "cancel fixture owns real queues");
    Peer::PrepareProducerGeneration(service); Peer::MarkConnected(service, true);
    Peer::SubscribeDiagnosticState(service); Peer::ConfigureConnectionSessionNotifications(service, capture.characteristic);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, modules::ConnectionSession::OBJECT_CAPACITY, 234U), "cancel fixture handshake");
    uint32_t sequence = 1U;
    Check(SendBenchmarkSharedControl(service, sequence,
        R"({"cmd":"object_bench_begin","q":1,"run":4,"size":300000,"seed":2})", 234U), "actual shared Begin is applied with notifications available");
    const bool activeBefore = Peer::BenchmarkActive(service);
    Check(Peer::BenchmarkActive(service) && Peer::BenchmarkCount(service) > 0U,
          "nonvacuous setup: active producer copied positive payload before pressure");
    Check(Peer::BenchmarkReverseWindowFull(service), "all eight actual retained reverse positions are full");
    // Begin status is a single packet at this selected body. Confirm only that
    // actual record; leave data unconfirmed and refill its one freed position.
    bool acknowledgedBegin = false;
    for (const auto& encoded : capture.packets) {
        wire::PacketView packet;
        if (wire::Codec::Decode({reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()}, packet, 244U) == wire::Error::None &&
            packet.header.kind == wire::Kind::Object && packet.header.code == 0x32U) {
            acknowledgedBegin = Peer::ConfirmSharedReverseObject(service, packet.header.sequence);
            break;
        }
    }
    Check(acknowledgedBegin, "real native notification plus real Progress completes Begin status");
    capture.packets.clear(); Peer::UpdateSharedStates(service);
    Check(Peer::BenchmarkReverseWindowFull(service), "data refill restores all eight retained positions");
    constexpr const char* pressure = R"({"st":"error","detail":"retained-test-terminal"})";
    Check(Peer::QueueBenchmarkPressure(service, pressure) && Peer::BenchmarkHasReliablePressure(service),
          "nonreplaceable pending status genuinely prevents ordinary control dequeue");
    const std::string originalStatus = Peer::BenchmarkPendingStatus(service);
    const auto count = Peer::BenchmarkCount(service);
    const auto lastSequence = Peer::LastPublishedReverseSequence(service);
    capture.characteristic.SetNotifyAdmission([](void*, const uint8_t* bytes, size_t size) {
        return size >= 10U && (bytes[0U] & 0x0fU) == 0U; // Real admission Progress still succeeds.
    }, nullptr);
    Check(SendBenchmarkSharedControl(service, sequence,
        R"({"cmd":"object_bench_cancel","q":2,"run":4})", 234U), "matching Cancel traverses actual shared ingress under status pressure");
    Check(!Peer::BenchmarkActive(service), "OLD dequeue gate leaves active; current classifier immediately fences matching run");
    Check(Peer::BenchmarkCount(service) == count && Peer::LastPublishedReverseSequence(service) == lastSequence &&
          Peer::BenchmarkPendingStatus(service) == originalStatus && Peer::BenchmarkReverseWindowFull(service),
          "cancel preserves copied count, all retained packets and exact nonreplaceable terminal");
    std::printf("[OBJECT_BENCHMARK_CANCEL] active_before=%u copied=%lu full8=%u status_pending=%u active_after=%u preserved=%u\n",
        unsigned(activeBefore), static_cast<unsigned long>(count), unsigned(Peer::BenchmarkReverseWindowFull(service)),
        unsigned(Peer::BenchmarkHasReliablePressure(service)), unsigned(Peer::BenchmarkActive(service)),
        unsigned(Peer::BenchmarkPendingStatus(service) == originalStatus));
}

void TestObjectBenchmarkFullForwardAndPendingAdmission() {
    std::puts("[OBJECT_BENCHMARK_CASE] TestObjectBenchmarkFullForwardAndPendingAdmission");
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    for (const bool corruptLate : {false, true}) {
        fxe::ManualMillis::SetNowMs(1000U);
        fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
        Harness harness; modules::BleService service; NimBLECharacteristic legacy; OtaStorageProbe storage;
        Check(Peer::Configure(service, harness.settings, harness.controlMutation), "full forward fixture actual queues");
        Peer::PrepareProducerGeneration(service); Peer::MarkConnected(service, true);
        Peer::ConfigureOtaNotifications(service, legacy);
        Peer::SubscribeDiagnosticState(service);
        ObjectBenchmarkCapture capture;
        Peer::ConfigureConnectionSessionNotifications(service, capture.characteristic); service.SetOtaPlatformAdapter(&storage);
        Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, modules::ConnectionSession::OBJECT_CAPACITY), "actual high-body forward handshake");
        Peer::SetBenchmarkPhysicalHandle(service, 7U);
        service.OnLinkObservation(7U, 247U, 0U, 0U);
        service.DrainEventQueue();
        Check(service.GetMtu() == 247U, "high-body fixture observes actual matching negotiated MTU247");
        bool pending = true;
        service.SetOtaStorePersistenceCallbacks(nullptr, [](void* value) { return *static_cast<bool*>(value); }, &pending);
        uint32_t sequence = 1U;
        const char* begin = R"({"cmd":"ota_sim_begin","size":300000,"chunk":234,"bench_schema":1,"run":1,"seed":23063})";
        Check(SendBenchmarkSharedControl(service, sequence, begin, 234U), "pending admission goes through actual shared adapter");
        std::vector<std::string> status;
        Check(CollectBenchmarkNativeStatus(service, capture, status) && !service.IsOtaInProgress() && !Peer::BenchmarkActive(service),
              "existing lifecycle pending-save observation refuses isolated RAM Begin without flushing");
        pending = false;
        Check(SendBenchmarkSharedControl(service, sequence,
            R"({"cmd":"object_bench_probe","q":42})", 234U), "idle capability probe actual shared adapter");
        status.clear(); Check(CollectBenchmarkNativeStatus(service, capture, status), "idle probe complete");
        bool schema = false;
        for (const auto& json : status) {
            StaticJsonDocument<768> probe;
            if (!deserializeJson(probe, json) && probe["q"] == 42 && probe["schema"] == 1 &&
                probe["code"] == 51 && probe["max_bytes"] == 300000 && probe["pattern"] == 1) schema = true;
        }
        Check(schema, "actual capability boundary precedes full transfer");
        Check(SendBenchmarkSharedControl(service, sequence, begin, 234U), "same refused run may start after pending transitions idle");
        status.clear(); Check(CollectBenchmarkNativeStatus(service, capture, status) && service.IsOtaInProgress() && Peer::BenchmarkActive(service),
              "nonvacuous full forward simulation is active");
        std::array<uint8_t, 234U> body{};
        for (uint32_t offset = 0U; offset < 300000U;) {
            const size_t count = std::min<size_t>(body.size(), 300000U - offset);
            for (size_t i = 0U; i < count; ++i)
                body[i] = static_cast<uint8_t>((offset + i) * 33U + 0x5a17U + ((offset + i) >> 8U));
            if (corruptLate && offset + count == 300000U) body[count - 1U] ^= 1U;
            const uint32_t dataSequence = sequence;
            const size_t beforeAdmission = capture.packets.size();
            if (offset == 0U) Peer::LimitNextIngressDrainToOne(service);
            Check(SendBenchmarkSharedObject(service, sequence, modules::ConnectionSession::OTA_DATA_OBJECT_OPCODE,
                                            body.data(), count), "every data chunk uses actual Object0x31 admission/backend");
            if (offset == 0U) {
                const auto readNativeGrant = [&](const size_t first, uint32_t& grant) {
                    namespace wire = modules::connection_session_codec;
                    bool found = false;
                    for (size_t index = first; index < capture.packets.size(); ++index) {
                        const auto& bytes = capture.packets[index];
                        wire::PacketView packet;
                        if (wire::Codec::Decode(
                                {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()},
                                packet, 512U) == wire::Error::None &&
                            packet.header.kind == wire::Kind::Control &&
                            packet.header.code == uint8_t(wire::Control::Progress) &&
                            packet.header.token == Peer::ObjectToken(service) &&
                            packet.body.Size() == 10U &&
                            fxe::ReadU32LE(packet.body.Data()) == dataSequence) {
                            grant = fxe::ReadU32LE(packet.body.Data() + 4U);
                            found = true;
                        }
                    }
                    return found;
                };
                uint32_t admittedGrant = 0U, releasedGrant = 0U;
                Check(Peer::BenchmarkCount(service) == 0U &&
                          readNativeGrant(beforeAdmission, admittedGrant),
                      "single ingress pass admits the copied chunk without releasing its reservation");
                const size_t beforeConsumption = capture.packets.size();
                service.DrainEventQueue();
                Check(Peer::BenchmarkCount(service) == count &&
                          readNativeGrant(beforeConsumption, releasedGrant) &&
                          releasedGrant == admittedGrant + 1U,
                      "one chunks-only drain returns exactly one grant through native notify without Update or another drain");
            }
            offset += count;
        }
        if (corruptLate) {
            status.clear(); Check(CollectBenchmarkNativeStatus(service, capture, status), "late corrupt error remains reassemblable");
            Check(!service.IsOtaInProgress() && Peer::BenchmarkCount(service) < 300000U,
                  "last-byte corruption after many chunks cannot report full completion");
            for (const auto& json : status) {
                StaticJsonDocument<768> error;
                Check(!deserializeJson(error, json) && error["bench"] != 1,
                      "late corruption produces no verified done proof");
            }
        } else {
            Check(Peer::BenchmarkCount(service) == 300000U, "actual backend consumed full300000 bytes");
            Check(SendBenchmarkSharedControl(service, sequence,
                R"({"cmd":"ota_end","size":300000,"crc32":"8c064b0b"})", 234U), "End uses independent complete pattern CRC");
            status.clear(); Check(CollectBenchmarkNativeStatus(service, capture, status), "full forward terminal reassembled");
            bool proven = false;
            for (const auto& json : status) {
                StaticJsonDocument<768> proof;
                if (!deserializeJson(proof, json) && proof["bench"] == 1 && proof["pattern"] == 1 &&
                    proof["run"] == 1 && proof["rx"] == 300000 && proof["total"] == 300000 &&
                    std::strcmp(proof["crc32"] | "", "8c064b0b") == 0) proven = true;
            }
            Check(proven && !service.IsOtaInProgress() && !Peer::PendingReboot(service),
                  "only actual full count/pattern/CRC terminal proves forward completion");
        }
        Check(storage.begins == 0U && storage.writes == 0U && storage.ends == 0U && storage.aborts == 0U,
              "both verified and rejected RAM runs make zero platform storage calls");
        std::printf("[OBJECT_BENCHMARK_FORWARD] bytes=%lu corrupt_late=%u active=%u storage_calls=%lu\n",
            static_cast<unsigned long>(Peer::BenchmarkCount(service)), unsigned(corruptLate),
            unsigned(service.IsOtaInProgress()),
            static_cast<unsigned long>(storage.begins + storage.writes + storage.ends + storage.aborts));
    }
}

void TestObjectBenchmarkCancelCannotAbortUnrelatedOta() {
    std::puts("[OBJECT_BENCHMARK_CASE] TestObjectBenchmarkCancelCannotAbortUnrelatedOta");
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    Harness harness; modules::BleService service; NimBLECharacteristic legacy; OtaStorageProbe storage;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "unrelated OTA fixture configures actual owner");
    Peer::PrepareProducerGeneration(service); Peer::MarkConnected(service, true);
    Peer::ConfigureOtaNotifications(service, legacy);
        Peer::SubscribeDiagnosticState(service);
        ObjectBenchmarkCapture capture;
        Peer::ConfigureConnectionSessionNotifications(service, capture.characteristic); service.SetOtaPlatformAdapter(&storage);
    Check(Peer::ActivateMinimumConnectionObjectSession(service), "unrelated OTA handshake");
    uint32_t sequence = 1U;
    Check(SendBenchmarkSharedControl(service, sequence,
        R"({"cmd":"ota_begin","size":3,"chunk":20})", 10U), "ordinary OTA begins through unchanged typed owner");
    std::vector<std::string> status; Check(CollectBenchmarkNativeStatus(service, capture, status) && service.IsOtaInProgress(), "real OTA is active before invalid benchmark controls");
    const auto aborts = storage.aborts;
    Check(SendBenchmarkSharedControl(service, sequence,
        R"({"cmd":"object_bench_cancel","q":2,"run":99})", 10U), "wrong-run shared Cancel is parsed");
    status.clear(); Check(CollectBenchmarkNativeStatus(service, capture, status), "wrong-run refusal remains bounded");
    const char* cancel = R"({"cmd":"object_bench_cancel","q":3,"run":99})";
    service.OnOtaControlCharacteristicWrite(cancel, std::strlen(cancel)); service.DrainEventQueue();
    Check(service.IsOtaInProgress() && storage.aborts == aborts,
          "wrong-run and nonshared diagnostic Cancel cannot abort ordinary OTA");
}

void TestObjectBenchmarkActualNegotiatedPacketCapacity() {
    std::puts("[OBJECT_BENCHMARK_CASE] TestObjectBenchmarkActualNegotiatedPacketCapacity");
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using namespace modules::ble_internal;
    namespace wire = modules::connection_session_codec;
    const uint16_t originalMtu = NimBLEDevice::getMTU();
    Harness harness; modules::BleService service; OtaStorageProbe storage;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "negotiation fixture configures actual owner");
    service.Begin(harness.settings);
    auto* server = NimBLEDevice::getServer();
    auto* base = server == nullptr ? nullptr : server->FindService(BASE_SERVICE_UUID);
    auto* ingress = base == nullptr ? nullptr : base->FindCharacteristic(CONNECTION_SESSION_INGRESS_CHARACTERISTIC_UUID);
    Check(server != nullptr && server->Callbacks() != nullptr && ingress != nullptr && ingress->Callbacks() != nullptr,
          "high-MTU fixture uses registered native link and characteristic callbacks");
    if (server == nullptr || server->Callbacks() == nullptr || ingress == nullptr || ingress->Callbacks() == nullptr) {
        service.Stop();
        NimBLEDevice::SetTestMtu(originalMtu);
        return;
    }
    NimBLEConnInfo info(517U, 7U);
    server->SetPeerMtu(7U, 517U);
    server->Callbacks()->onConnect(server, info);
    service.DrainEventQueue();
    Peer::PrepareProducerGeneration(service);
    Peer::SubscribeDiagnosticState(service);
    ObjectBenchmarkCapture capture;
    Peer::ConfigureConnectionSessionNotifications(service, capture.characteristic);
    service.SetOtaPlatformAdapter(&storage);
    uint16_t forwardBody = 0U;
    Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 512U, modules::ConnectionSession::OBJECT_CAPACITY, 502U, &forwardBody),
          "actual production Hello/Open handshake offers common512");
    Check(Peer::Session(service).PacketMaximum() == 512U && forwardBody == 502U &&
          Peer::Session(service).ReverseObjectMaximumBody() == 502U,
          "production selects common512 and complete502-byte bodies in both directions");
    server->Callbacks()->onMTUChange(517U, info);
    service.DrainEventQueue();
    Check(service.GetMtu() == 517U, "actual MTU callback enables the negotiated512-byte packet");
    uint32_t sequence = 1U;
    Check(SendBenchmarkSharedControl(service, sequence,
        R"({"cmd":"ota_sim_begin","size":502,"chunk":502,"bench_schema":1,"run":1,"seed":23063})", forwardBody),
          "high-MTU RAM Begin uses the existing shared control adapter");
    std::vector<std::string> status;
    Check(CollectBenchmarkNativeStatus(service, capture, status) && service.IsOtaInProgress() && Peer::BenchmarkActive(service),
          "high-MTU transfer is active before the maximum packet reaches its callback");
    std::array<uint8_t, 502U> body{};
    for (size_t offset = 0U; offset < body.size(); ++offset)
        body[offset] = static_cast<uint8_t>(offset * 33U + 0x5a17U + (offset >> 8U));
    std::array<uint8_t, 512U> bytes{}; size_t size = 0U;
    Check(wire::Codec::Encode({{wire::Kind::Object, modules::ConnectionSession::OTA_DATA_OBJECT_OPCODE,
              Peer::ObjectToken(service), sequence++}, {body.data(), body.size()}},
              {bytes.data(), bytes.size()}, size) == wire::Error::None && size == 512U,
          "one complete negotiated502-byte body encodes as512 wire bytes");
    ingress->setValue(bytes.data(), size);
    ingress->Callbacks()->onWrite(ingress, info);
    Check(Peer::RawOrdinaryCount(service) == 1U && Peer::BenchmarkCount(service) == 0U,
          "native callback retains the full packet in the ordinary FIFO before domain work");
    service.DrainEventQueue();
    Check(Peer::BenchmarkCount(service) == body.size() && service.IsOtaInProgress(),
          "actual queue and OTA adapter deliver all502 pattern bytes without truncation");
    Check(SendBenchmarkSharedControl(service, sequence,
        R"({"cmd":"ota_end","size":502,"crc32":"d87b1ce4"})", forwardBody),
          "high-MTU End checks the independent known502-byte pattern CRC");
    status.clear();
    Check(CollectBenchmarkNativeStatus(service, capture, status), "high-MTU terminal survives native notification admission");
    bool proven = false;
    for (const auto& json : status) {
        StaticJsonDocument<768> proof;
        if (!deserializeJson(proof, json) && proof["bench"] == 1 && proof["pattern"] == 1 &&
            proof["run"] == 1 && proof["rx"] == 502 && proof["total"] == 502 &&
            std::strcmp(proof["crc32"] | "", "d87b1ce4") == 0) proven = true;
    }
    Check(proven && !service.IsOtaInProgress() && !Peer::PendingReboot(service) &&
              storage.begins == 0U && storage.writes == 0U && storage.ends == 0U && storage.aborts == 0U,
          "full512-byte callback delivery proves count/pattern/CRC with no flash or reboot");
    service.Stop();
    NimBLEDevice::SetTestMtu(originalMtu);
    server->SetPeerSlots({});
}

void TestObjectBenchmarkDleDetailObservations() {
    std::puts("[OBJECT_BENCHMARK_CASE] TestObjectBenchmarkDleDetailObservations");
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    {
        Harness harness; modules::BleService service;
        Check(Peer::Configure(service, harness.settings, harness.controlMutation), "stale DLE owns actual queue");
        Peer::PrepareProducerGeneration(service); Peer::MarkConnected(service, true);
        Peer::SetBenchmarkPhysicalHandle(service, 7U);
        service.OnNativeDataLengthChange(7U, {251U, 2120U, 251U, 2120U});
        Peer::ResetStackGeneration(service);
        service.DrainEventQueue();
        Check(!service.GetLinkObservation().dataLengthKnown,
              "queued DLE from a retired native epoch cannot establish current facts");
    }
    Harness harness; modules::BleService service; ObjectBenchmarkCapture capture;
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "DLE detail uses actual shared session queues");
    Peer::PrepareProducerGeneration(service); Peer::MarkConnected(service, true);
    Peer::SetBenchmarkPhysicalHandle(service, 7U);
    Peer::SubscribeDiagnosticState(service);
    Peer::ConfigureConnectionSessionNotifications(service, capture.characteristic);
    Check(Peer::ActivateMinimumConnectionObjectSession(service), "DLE detail negotiates actual ATT20 framing");
    uint32_t sequence = 1U;
    auto detail = [&](const uint16_t q, StaticJsonDocument<768>& json) {
        const std::string request = "{\"cmd\":\"object_bench_probe\",\"q\":" + std::to_string(q) + ",\"detail\":1}";
        Check(SendBenchmarkSharedControl(service, sequence, request.c_str(), 10U), "detail query uses typed control fragmentation");
        std::vector<std::string> records;
        Check(CollectBenchmarkNativeStatus(service, capture, records), "detail status uses accepted native notifications and cumulative Progress");
        for (const auto& record : records) {
            StaticJsonDocument<768> parsed;
            if (!deserializeJson(parsed, record) && parsed["s"] == "object_bench_detail" && parsed["q"] == q) {
                Check(record.size() < 224U, "complete detail fits unchanged status budget including all legal integer widths");
                json.set(parsed);
                return true;
            }
        }
        return false;
    };
    StaticJsonDocument<768> unknown;
    Check(detail(31U, unknown) && unknown["schema"] == 1 && unknown["dk"] == 0 &&
          unknown["txo"] == 0 && unknown["txt"] == 0 && unknown["rxo"] == 0 && unknown["rxt"] == 0,
          "absence of a native DLE event stays unknown rather than claiming27 or251");
    for (const char* invalid : {"null", "2", "\"1\""}) {
        const std::string request = std::string("{\"cmd\":\"object_bench_probe\",\"q\":33,\"detail\":") + invalid + "}";
        Check(SendBenchmarkSharedControl(service, sequence, request.c_str(), 10U),
              "invalid-detail witness reaches the real shared control adapter");
        std::vector<std::string> records;
        Check(CollectBenchmarkNativeStatus(service, capture, records), "invalid-detail error is retained and fragmented normally");
        bool rejected = false, success = false;
        for (const auto& record : records) {
            StaticJsonDocument<768> parsed;
            if (deserializeJson(parsed, record)) continue;
            rejected = rejected || parsed["e"] == "invalid_benchmark";
            success = success || parsed["s"] == "object_bench_probe" || parsed["s"] == "object_bench_detail";
        }
        Check(rejected && !success, "present null/unsupported/wrong-type detail rejects without a default success reply");
    }
    service.OnNativeDataLengthChange(7U, {251U, 2120U, 237U, 17040U});
    service.OnLinkObservation(7U, 247U, 2U, 2U);
    service.DrainEventQueue();
    service.OnNativeDataLengthChange(9U, {27U, 328U, 27U, 328U});
    service.DrainEventQueue();
    StaticJsonDocument<768> actual;
    Check(detail(32U, actual) && actual["dk"] == 1 && actual["txo"] == 251 &&
          actual["txt"] == 2120 && actual["rxo"] == 237 && actual["rxt"] == 17040 &&
          actual["pk"] == 1 && actual["tp"] == 2 && actual["rp"] == 2 &&
          actual["na"].is<uint32_t>() && actual["nf"].is<uint32_t>() && actual["rr"].is<uint32_t>(),
          "exact native DLE/PHY facts and existing counters survive foreign-handle input");
    // Width coverage is not evidence that these synthetic maxima are real link values.
    service.OnNativeDataLengthChange(7U, {65535U, 65535U, 65535U, 65535U});
    service.DrainEventQueue();
    Peer::SetBenchmarkNotifyCounters(service, UINT32_MAX - 1024U, UINT32_MAX);
    StaticJsonDocument<768> maximum;
    Check(detail(UINT16_MAX, maximum) && maximum["q"] == UINT16_MAX && maximum["txo"] == UINT16_MAX &&
          maximum["txt"] == UINT16_MAX && maximum["rxo"] == UINT16_MAX && maximum["rxt"] == UINT16_MAX &&
          maximum["na"].as<uint32_t>() >= UINT32_MAX - 1024U && maximum["nf"] == UINT32_MAX,
          "five-digit DLE/query and ten-digit native counters serialize without truncation at ATT20");
    Peer::RetireBenchmarkSession(service);
    Check(service.GetLinkObservation().dataLengthKnown,
          "shared-session retirement retains physical DLE facts");
}


void TestPreferredPhyExplicitBenchmarkOptIn() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using Record = modules::BoardOwnerRecord;
    enum class Fence { None, Disconnect, CancelThenDefault, OwnerLoss, WrongCancel };
    struct Case { const char* json; bool begin; bool prefer; bool sdkAccepted; uint16_t mtu; Fence fence; };
    constexpr const char* enabled = R"({"cmd":"ota_sim_begin","size":3,"chunk":20,"bench_schema":1,"run":1,"seed":2,"prefer_phy_2m":true})";
    const Case cases[] = {
        {enabled, true, true, false, 23U, Fence::None},
        {enabled, true, true, false, 517U, Fence::None},
        {enabled, true, true, true, 23U, Fence::None},
        {enabled, true, true, true, 517U, Fence::None},
        {R"({"cmd":"ota_sim_begin","size":3,"chunk":20,"bench_schema":1,"run":1,"seed":2})", true, false, true, 23U, Fence::None},
        {R"({"cmd":"ota_sim_begin","size":3,"chunk":20,"bench_schema":1,"run":1,"seed":2,"prefer_phy_2m":false})", true, false, true, 517U, Fence::None},
        {R"({"cmd":"ota_sim_begin","size":3,"chunk":20,"bench_schema":1,"run":1,"seed":2,"prefer_phy_2m":null})", false, false, true, 23U, Fence::None},
        {R"({"cmd":"ota_sim_begin","size":3,"chunk":20,"bench_schema":1,"run":1,"seed":2,"prefer_phy_2m":1})", false, false, true, 23U, Fence::None},
        {R"({"cmd":"ota_sim_begin","size":3,"chunk":20,"bench_schema":1,"run":1,"seed":2,"prefer_phy_2m":"true"})", false, false, true, 23U, Fence::None},
        {R"({"cmd":"ota_sim_begin","size":3,"chunk":20,"prefer_phy_2m":true})", false, false, true, 23U, Fence::None},
        {R"({"cmd":"ota_begin","size":3,"chunk":20,"prefer_phy_2m":false})", false, false, true, 23U, Fence::None},
        {R"({"cmd":"object_bench_probe","q":31,"prefer_phy_2m":true})", false, false, true, 23U, Fence::None},
        {R"({"cmd":"info","prefer_phy_2m":true})", false, false, true, 23U, Fence::None},
        {R"({"cmd":"device_info","prefer_phy_2m":false})", false, false, true, 23U, Fence::None},
        {enabled, true, true, true, 23U, Fence::Disconnect},
        {enabled, true, true, true, 517U, Fence::CancelThenDefault},
        {enabled, true, true, true, 23U, Fence::OwnerLoss},
        {enabled, true, true, true, 23U, Fence::WrongCancel},
    };
    const uint16_t originalMtu = NimBLEDevice::getMTU();
    for (const auto& c : cases) {
        owner_native_fixture::Reset(); fxe::ManualMillis::SetNowMs(1000U);
        fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
        Harness harness; harness.settings.Doc()["BLE_ENABLED"] = true;
        OwnerPreviewPersistence persistence;
        modules::BoardOwnerPairingRuntime owner(persistence);
        modules::BoardIdentityPersistence identity;
        StaticJsonDocument<256> document;
        Check(identity.Prepare(document, [](void*, uint8_t* bytes, size_t size) {
            std::fill_n(bytes, size, uint8_t{0x47U}); return true;
        }, nullptr), "preferred PHY fixture prepares actual durable owner identity");
        identity.OnDurableSettingsSnapshot(document); owner.Boot(Record::Active, {true, 1U}, true);
        auto* server = NimBLEDevice::getServer(); server->SetPeerSlots({});
        server->ResetPhyRequestProbe(c.sdkAccepted); NimBLEDevice::SetTestMtu(c.mtu);
        modules::BleService service; service.SetControlMutationService(&harness.controlMutation);
        service.Begin(harness.settings); Peer::ConfigureOwnerPreview(service, owner, identity);
        auto* callbacks = server->Callbacks();
        Check(callbacks != nullptr, "PHY diagnostic uses registered production native callbacks");
        if (callbacks == nullptr) { service.Stop(); server->ResetPhyRequestProbe(); continue; }
        NimBLEConnInfo info(c.mtu, 7U); server->SetPeerMtu(7U, c.mtu);
        callbacks->onConnect(server, info); service.DrainEventQueue(); service.Update(harness.settings);
        Check(server->PhyRequestCount() == 0U && server->ConnParamRequestCount() == 0U,
              "PHY and interval preferences wait for current owner admission");
        const uint32_t epoch = Peer::ProducerEpoch(service);
        Check(Peer::ActivateMinimumConnectionObjectSession(service, epoch),
              "explicit PHY diagnostic uses canonical minimum-MTU shared bootstrap");
        service.UpdateOwnerPairing(); Check(!Peer::ProductAccess(service), "connection bootstrap retains native-owner gate");
        const auto connection = Peer::OwnerConnection(service);
        modules::BleOwnerSecurityEvent security{};
        security.connHandle = 7U; security.connectionEpoch = epoch; security.sessionToken = connection.sessionToken;
        security.nativeStatus = 0; security.inventoryAvailable = true; security.peerCount = 1U;
        security.encrypted = true; security.bonded = true; security.peerMatches = true;
        service.OnOwnerSecurity(security); service.DrainEventQueue(); service.UpdateOwnerPairing();
        Check(Peer::ProductAccess(service), "valid native security reaches actual durable-owner readiness before diagnostic");
        ObjectBenchmarkCapture capture; Peer::ConfigureConnectionSessionNotifications(service, capture.characteristic);
        Peer::SubscribeDiagnosticState(service);
        uint32_t sequence = 1U;
        Check(SendBenchmarkSharedControl(service, sequence, c.json, 10U), "exact helper JSON including chunk crosses actual typed0x32 adapter");
        std::vector<std::string> status;
        Check(CollectBenchmarkNativeStatus(service, capture, status), "real accepted native status fragments are cumulatively confirmed");
        Check(server->PhyRequestCount() == 0U, "control Drain only arms intent; it never calls the PHY SDK");
        if (!c.begin) {
            bool rejected = false;
            for (const auto& json : status) { StaticJsonDocument<128> value;
                rejected = rejected || (!deserializeJson(value, json) && value["e"] == "invalid_benchmark"); }
            Check(rejected && !Peer::BenchmarkActive(service) && !service.IsOtaInProgress(),
                  "invalid types and non-benchmark options reject before OTA side effects");
        } else {
            Check(Peer::BenchmarkActive(service) && service.IsOtaInProgress(),
                  "positive witness has an admitted active pattern benchmark before testing SDK request");
        }
        if (c.fence == Fence::Disconnect) {
            server->SetPeerSlots({}); callbacks->onDisconnect(server, info, 0);
        } else if (c.fence == Fence::OwnerLoss) {
            auto failure = security; failure.nativeStatus = 5; failure.encrypted = false;
            service.OnOwnerSecurity(failure);
        } else if (c.fence == Fence::WrongCancel) {
            Check(SendBenchmarkSharedControl(service, sequence,
                R"({"cmd":"object_bench_cancel","q":2,"run":2})", 10U), "wrong-run cancel enters actual typed owner");
            status.clear(); Check(CollectBenchmarkNativeStatus(service, capture, status), "wrong-run refusal remains retained");
            Check(Peer::BenchmarkActive(service), "wrong-run cancel cannot cancel a current pending opt-in");
        } else if (c.fence == Fence::CancelThenDefault) {
            Check(SendBenchmarkSharedControl(service, sequence,
                R"({"cmd":"object_bench_cancel","q":2,"run":1})", 10U), "matching cancel precedes pending SDK consumption");
            status.clear(); Check(CollectBenchmarkNativeStatus(service, capture, status), "cancel native status retires exactly");
            Check(SendBenchmarkSharedControl(service, sequence,
                R"({"cmd":"ota_sim_begin","size":3,"chunk":20,"bench_schema":1,"run":2,"seed":2,"prefer_phy_2m":false})", 10U),
                "new explicitfalse Begin cannot inherit older pending opt-in");
            status.clear(); Check(CollectBenchmarkNativeStatus(service, capture, status), "replacement Begin remains admitted");
        }
        service.Update(harness.settings);
        const bool shouldRequest = c.fence != Fence::Disconnect && c.fence != Fence::OwnerLoss;
        Check(server->PhyRequestCount() == (shouldRequest ? 1U : 0U),
              "normal current owner requests once independently of benchmark flags");
        if (shouldRequest) {
            Check(server->LastPhyHandle() == 7U && server->LastPhyTxMask() == 0x02U &&
                      server->LastPhyRxMask() == 0x02U && server->LastPhyOptions() == 0U && !server->PhyAfterConnParams(),
                  "SDK boundary receives currenthandle two-way2M preference before interval requests");
        }
        Check(!service.GetLinkObservation().phyKnown, "neither SDKtrue nor SDKfalse manufactures PHY selection");
        service.Update(harness.settings);
        Check(server->PhyRequestCount() == (shouldRequest ? 1U : 0U), "subsequent owner turns never retry PHY");
        Check(server->ConnParamRequestCount() == (shouldRequest && !c.sdkAccepted ? 1U : 0U),
              "SDK rejection releases the interval fallback while accepted PHY remains pending");
        if (c.fence == Fence::Disconnect) {
            service.DrainEventQueue(); server->SetPeerMtu(7U, c.mtu);
            callbacks->onConnect(server, info); service.DrainEventQueue(); service.Update(harness.settings);
            Check(server->PhyRequestCount() == 0U && service.IsConnected(),
                  "reconnected native owner remains gated until security admission");
        }
        if (c.fence == Fence::None || c.fence == Fence::WrongCancel) {
            Check(Peer::ProductAccess(service) && service.IsConnected() && service.GetMtu() == c.mtu,
                  "SDKunsupported/error or invalid option adds no native-owner/MTU readiness gate");
            if (c.begin) {
                const uint8_t pattern[3U] = {2U,35U,68U};
                Check(SendBenchmarkSharedObject(service, sequence, modules::ConnectionSession::OTA_DATA_OBJECT_OPCODE,
                                                pattern, sizeof(pattern)), "optional preference preserves actual verified pattern backend");
                Check(SendBenchmarkSharedControl(service, sequence,
                    R"({"cmd":"ota_end","size":3,"crc32":"33ddff94"})",10U), "optional preference preserves exact End/CRC owner");
                status.clear(); Check(CollectBenchmarkNativeStatus(service,capture,status), "verified terminal status remains consumable");
                bool verified = false;
                for (const auto& json : status) { StaticJsonDocument<384> value;
                    verified = verified || (!deserializeJson(value,json) && value["bench"] == 1 && value["rx"] == 3 &&
                        value["pattern"] == 1 && std::strcmp(value["crc32"] | "", "33ddff94") == 0); }
                Check(verified && !service.IsOtaInProgress(), "optional preference preserves exact independent count/pattern/CRC completion");
            }
        }
        service.Stop(); server->SetPeerSlots({}); server->ResetPhyRequestProbe();
    }
    NimBLEDevice::SetTestMtu(originalMtu); NimBLEDevice::getServer()->SetPeerSlots({});
}

void TestNormalConnectionPhyPreferenceAndSnapshot() {
    std::puts("[PHY_DEFAULT_CASE] normal callback -> Drain -> Update -> session retirement");
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    const uint16_t originalMtu = NimBLEDevice::getMTU();
    for (const bool accepted : {false, true}) {
        const uint32_t expectedPhyRequests = accepted ? 0U : 1U;
        Harness harness; harness.settings.Doc()["BLE_ENABLED"] = true;
        modules::BleService service; service.Begin(harness.settings);
        auto* server = NimBLEDevice::getServer();
        server->SetPeerSlots({}); server->ResetPhyRequestProbe(accepted);
        server->ResetPhyReadProbe(true, accepted ? 2U : 1U, accepted ? 2U : 1U);
        auto* callbacks = server->Callbacks();
        NimBLEConnInfo info(247U, 7U); server->SetPeerMtu(7U, 247U);
        callbacks->onConnect(server, info); service.DrainEventQueue();
        Check(server->PhyRequestCount() == 0U && server->PhyReadCount() == 0U,
              "callbacks never perform PHY HCI work");
        service.Update(harness.settings);
        Check(server->PhyRequestCount() == expectedPhyRequests && server->PhyReadCount() == 1U &&
              server->LastPhyReadHandle() == 7U &&
              (accepted || (server->LastPhyHandle() == 7U &&
                  server->LastPhyTxMask() == 2U && server->LastPhyRxMask() == 2U)),
              "ordinary connection observes actual PHY and requests2M only when it is still needed");
        Check(server->ConnParamRequestCount() == 0U,
              "the first owner turn settles PHY before any interval request");
        const auto value = service.GetLinkObservation();
        Check(value.phyKnown && value.txPhy == (accepted ? 2U : 1U) &&
              value.rxPhy == value.txPhy && service.IsConnected(),
              "actualSDKread reports2M or1M independently of preference acceptance without disconnect");
        Peer::RetireBenchmarkSession(service);
        service.Update(harness.settings);
        Check(service.GetLinkObservation().phyKnown && server->PhyRequestCount() == expectedPhyRequests &&
              server->PhyReadCount() == 1U && server->ConnParamRequestCount() == 1U,
              "known2M or rejected PHY releases interval preference without retrying the physical epoch");
        server->SetPeerSlots({}); callbacks->onDisconnect(server, info, 0); service.DrainEventQueue();
        Check(!service.GetLinkObservation().phyKnown, "physical disconnect clears observations");
        server->SetPeerMtu(7U, 247U); callbacks->onConnect(server, info); service.DrainEventQueue();
        service.Update(harness.settings);
        Check(server->PhyRequestCount() == 2U * expectedPhyRequests && server->PhyReadCount() == 2U &&
                  server->ConnParamRequestCount() == 1U,
              "replacement connection reusing a native handle settles its own PHY before interval preference");
        service.Update(harness.settings);
        Check(server->ConnParamRequestCount() == 2U,
              "replacement physical epoch releases one interval request after its PHY decision");
        service.Stop(); server->SetPeerSlots({}); server->ResetPhyRequestProbe(); server->ResetPhyReadProbe();
    }
    NimBLEDevice::SetTestMtu(originalMtu); NimBLEDevice::getServer()->SetPeerSlots({});
}

void TestObjectBenchmarkRealStackLinkCallbacks() {
    std::puts("[OBJECT_BENCHMARK_CASE] TestObjectBenchmarkRealStackLinkCallbacks");
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    const uint16_t originalMtu = NimBLEDevice::getMTU();
    fxe::ManualMillis::SetNowMs(1000U);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    Harness harness; modules::BleService service;
    service.Begin(harness.settings);
    auto* server = NimBLEDevice::getServer();
    auto* callbacks = server == nullptr ? nullptr : server->Callbacks();
    Check(callbacks != nullptr, "real firmware stack registers native link callbacks");
    if (callbacks == nullptr) return;
    server->ResetPhyRequestProbe();
    NimBLEConnInfo info(247U, 7U);
    server->SetPeerMtu(7U, 247U);
    callbacks->onConnect(server, info);
    service.DrainEventQueue();
    Check(server->ConnParamRequestCount() == 0U,
          "native callbacks only publish connection intent before the main-loop owner runs");
    callbacks->onMTUChange(247U, info);
    callbacks->onPhyUpdate(info, 2U, 2U);
    service.OnNativeDataLengthChange(7U, {251U, 2120U, 251U, 2120U});
    callbacks->onConnParamsUpdate(info);
    service.DrainEventQueue();
    const auto observed = service.GetLinkObservation();
    Check(observed.mtu == 247U && observed.intervalUnits == info.getConnInterval() &&
          observed.phyKnown && observed.txPhy == 2U && observed.rxPhy == 2U && observed.dataLengthKnown,
          "actual SDK callbacks carry cached negotiated observations to Support owner");
    NimBLEConnInfo old(517U, 9U);
    callbacks->onMTUChange(517U, old);
    callbacks->onPhyUpdate(old, 1U, 1U);
    service.DrainEventQueue();
    Check(service.GetLinkObservation().mtu == 247U && service.GetLinkObservation().txPhy == 2U,
          "foreign-handle SDK callback cannot replace current observations");
    callbacks->onPhyUpdate(info, 1U, 1U); service.DrainEventQueue();
    // The SDK fixture reports interval12. Feed the existing typed observation
    // seam with the witnessed interval24; request order still uses real Update.
    service.OnConnectionParametersChanged(24U, 0U, 72U, false, 7U);
    service.DrainEventQueue(); service.Update(harness.settings);
    Check(server->ConnParamRequestCount() == 0U && server->PhyRequestCount() == 1U && !server->PhyAfterConnParams(),
          "cooperative owner starts PHY without issuing an overlapping interval request");
    for (unsigned event = 0U; event < 3U; ++event) {
        service.OnConnectionParametersChanged(24U, 0U, 72U, false, 7U);
        service.DrainEventQueue(); service.Update(harness.settings);
    }
    Check(server->ConnParamRequestCount() == 0U,
          "startup parameter callbacks cannot issue any interval request while PHY remains pending");
    const uint32_t physicalEpoch = Peer::ProducerEpoch(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, physicalEpoch),
          "same physical link reaches actual shared-session activation");
    service.Update(harness.settings);
    Check(server->ConnParamRequestCount() == 0U,
          "known PHY alone does not complete the outstanding native PHY procedure");
    callbacks->onPhyUpdate(old, 0U, 0U); service.DrainEventQueue(); service.Update(harness.settings);
    Check(server->ConnParamRequestCount() == 0U,
          "foreign PHY completion cannot release the current connection request");
    callbacks->onPhyUpdate(info, 1U, 1U); service.DrainEventQueue(); service.Update(harness.settings);
    Check(server->ConnParamRequestCount() == 1U && service.GetLinkObservation().txPhy == 1U,
          "current valid1M completion releases the initial interval fallback without requiring2M");
    for (unsigned event = 0U; event < 2U; ++event) {
        service.OnConnectionParametersChanged(24U, 0U, 72U, false, 7U);
        service.DrainEventQueue(); service.Update(harness.settings);
    }
    Check(server->ConnParamRequestCount() == 3U,
          "settled PHY permits only the existing three-request physical budget");
    Check(Peer::ActivateMinimumConnectionObjectSession(service, physicalEpoch),
          "shared-session replacement retains the physical request budget");
    service.Update(harness.settings); service.Update(harness.settings);
    Check(server->ConnParamRequestCount() == 3U,
          "session replacement and repeated Update cannot create a fourth physical request");
    Peer::PrepareProducerGeneration(service);
    Peer::SubscribeDiagnosticState(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, 7U, 244U, modules::ConnectionSession::OBJECT_CAPACITY, 37U),
          "native callback fixture opens actual diagnostic session");
    uint32_t sequence = 1U;
    Check(SendBenchmarkSharedControl(service, sequence,
        R"({"cmd":"object_bench_begin","q":1,"run":1,"size":300000,"seed":2})", 234U) &&
        Peer::BenchmarkActive(service), "real callback disconnect starts with an active producer");
    // The SDK removes its peer before delivering the native disconnect callback.
    server->SetPeerSlots({});
    callbacks->onDisconnect(server, info, 0);
    service.DrainEventQueue();
    Check(!service.GetLinkObservation().phyKnown && !service.GetLinkObservation().dataLengthKnown &&
          !Peer::BenchmarkActive(service),
          "actual disconnect retires active producer and known PHY");
    server->ResetPhyReadProbe(true, 1U, 1U);
    server->SetPeerMtu(7U, 247U); callbacks->onConnect(server, info);
    service.DrainEventQueue(); service.Update(harness.settings);
    Check(server->ConnParamRequestCount() == 3U,
          "replacement physical epoch cannot inherit settled PHY from its reused handle");
    callbacks->onPhyUpdate(info, 0U, 0U); service.DrainEventQueue(); service.Update(harness.settings);
    Check(server->ConnParamRequestCount() == 4U,
          "failed current PHY completion releases the replacement interval fallback");
    Check(service.GetLinkObservation().phyKnown && service.GetLinkObservation().txPhy == 1U,
          "failed completion does not publish invalid PHY bytes over the observed1M link");
    service.Stop();
    server->ResetPhyReadProbe();
    NimBLEDevice::SetTestMtu(originalMtu);
    server->SetPeerSlots({});
}

int main() {
    if (std::getenv("LITCH_OWNER_EVIDENCE_ONLY") != nullptr) {
        fxe::ScopedMillisProviderOverride ownerClock(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(0U);
        TestOwnerNativeEvidenceRecoversAfterConnectLookupFailure();
        if (std::getenv("LITCH_OWNER_EVIDENCE_NEIGHBORS") != nullptr) {
            TestRejectedOwnerEpochCannotStartQuietSecurity();
            TestOwnerNativeSecurityRequestsAreIntentBounded();
            TestOwnerDuplicateNativeConnectPreservesAttempt();
            TestForeignOwnerRefusalReachesSharedSessionBeforeDisconnect();
            TestNativePeerIdentityAdmissionAndRecovery();
            TestOwnerPreviewRetirementQueueAndQuietReconnect();
        }
        return g_failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_ORDERED_MODE_ONLY") != nullptr) {
        TestOrderedModeDoesNotWaitForObjectCredit();
        return g_failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_BULK_CREDIT_PROBE") != nullptr) {
        TestSharedBulkCreditsFollowSlotLifetime();
        TestBulkCreditReturnTokenAndEpochFences();
        TestSharedLogicalBulkBoundAndOrdinaryFragments();
        return g_failures == 0 ? 0 : 1;
    }
    TestBootstrapRestartRenewsOnlyItsNativeConnection();
    if (std::getenv("LITCH_NATIVE_RENEWAL_ONLY") != nullptr) return g_failures == 0 ? 0 : 1;
    const uint32_t originalTimeMs = fxe::Millis();
    TestObjectBenchmarkFullForwardAndPendingAdmission();
    TestObjectBenchmarkCancelCannotAbortUnrelatedOta();
    TestObjectBenchmarkActualNegotiatedPacketCapacity();
    TestObjectBenchmarkDleDetailObservations();
    TestObjectBenchmarkRealStackLinkCallbacks();
    TestPreferredPhyExplicitBenchmarkOptIn();
    TestNormalConnectionPhyPreferenceAndSnapshot();
    TestObjectBenchmarkActualSharedReverseAndLinkObservations();
    TestObjectBenchmarkActualForwardPatternAndMinimumBegin();
    TestObjectBenchmarkRefusalCancelAndDirectIngress();
    fxe::ManualMillis::SetNowMs(originalTimeMs);
    {
        // Owner button fixtures use event time zero. Keep their actual ingress
        // clock explicit even after a timed VirtualLitch journey has run.
        const uint32_t restoreMs = fxe::ManualMillis::NowMs();
        fxe::ScopedMillisProviderOverride ownerClock(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(0U);
        TestTypedIdentifyVirtualLitchTimedJourney();
        TestIdentifyRatePolicyAndUnpairedOrderedPath();
        TestPairedBluetoothIdentitySuffix();
        TestRejectedOwnerEpochCannotStartQuietSecurity();
        TestOwnerNativeSecurityRequestsAreIntentBounded();
        TestNativeSavedPhonesThroughSharedSession();
        TestOwnerStoreDiagnosticCountsAreReadOnlyAndChecked();
        TestOwnerDuplicateNativeConnectPreservesAttempt();
        TestForeignOwnerRefusalReachesSharedSessionBeforeDisconnect();
        TestNativePeerIdentityAdmissionAndRecovery();
        TestOwnerNativeEvidenceRecoversAfterConnectLookupFailure();
        TestOwnerSettingsClearExecutionAdmission();
        TestOwnerPreviewSharedConsentAndNativeLease();
        TestOwnerPreviewSharedConsentAndNativeLease(true);
        TestOwnerPreviewRetirementQueueAndQuietReconnect();
        fxe::ManualMillis::SetNowMs(restoreMs);
    }
    TestSettingsRejectEmbeddedNulBeforeMutation();
    TestConsoleAndOtaRejectEmbeddedNulAtProductionIngress();
    std::puts("[BLE_SETTINGS_CONTROL_CONTRACT] start");
    TestLitchcraftExactNotificationsAndEpochs();
    TestSchema4RequiresConnectionSessionIngress();
    TestFrameStoreResidentResponseFitsMinimumObjectPackets();
    TestFrameStoreSnapshotFillsReverseCreditsInOneUpdate();
    TestSharedV2InspectRoutesByCapability();
    TestMinimumObjectFragmentsReachStoreWorkerOnlyWhenComplete();
    TestSharedGameHistoryFragmentsCommitOneExactFrame();
    TestSharedOtaDataAdmitsOneExactChunk();
    TestSharedOtaControlCommitsOneOrderedExistingEvent();
    TestSharedOtaStatusFragmentsAtMinimumMtu();
    TestCorrelatedOtaAttemptProofAtProductionBoundary();
    TestFirmwareDigestRetainsEveryJsonAcrossReverseAdmission();
    TestRequiredGattFailureCleansPartialStack();
    TestFailedHostStopRetainsNativeBindingUntilCleanupRetry();
    TestRecoveryUsesUpdaterWithoutDomainStartup();
    TestRecoveryPublicSnapshotWithoutOwnerOrSettingsLoad();
    TestPendingDigestAndMixedStatusPressure();
    TestBlockedInfoAllowsQueuedProgressAndTimeoutRecovery();
    TestSharedDoneWaitsForActualFinalNotifyAcceptance();
    TestSharedDoneWaitsForActualFinalNotifyAcceptance(true);
    TestConnectionReplacementDiscardsOldReliableProgressAndError();
    TestSharedLitchcraftLifecycleAtMinimumMtu();
    TestActivatedSessionRejectsRetiredDirectOtaIngress();
    TestReconnectRetiresOldSessionBeforeCapabilityProbe();
    TestLitchcraftV2ProductionHighMtuIngressAndEpochs();
    TestDeviceInfoAdvertisesAnimationWithStoreIngress();
    TestLitchcraftReplacementConnectionIsolation();
    TestLitchcraftReplacementConnectionFinishesAppStroke();
    for (const auto revision : {hardware::BoardRevision::V2S3,
                               hardware::BoardRevision::V3S3,
                               hardware::BoardRevision::V3_3S3}) {
        for (const bool rotate : {false, true}) {
            TestOtaRuntimeProgressJourney(revision, rotate);
        }
    }
    TestOtaAdmissionOrdinarySelectionPreservesPriorityRace();
    for (const auto scenario : {OtaAdmissionCase::Immediate, OtaAdmissionCase::NotifyDenied,
                                OtaAdmissionCase::Close, OtaAdmissionCase::OtherOutput,
                                OtaAdmissionCase::InvalidControl, OtaAdmissionCase::Reconnect,
                                OtaAdmissionCase::PinnedStatus, OtaAdmissionCase::BufferedObject}) {
        TestSharedOtaBeginNotifiesAdmissionBeforeBlocking(scenario);
    }
    TestOtaBeginWaitsForStoreClear();
    TestOtaRejectsNewStoreBulkUntilAbort();
    TestOtaEndWaitsForDurableArtwork();
    TestOtaRefilledIngressYieldsToRendering();
    TestBoardSetupAssistanceUsesGenericBleContract();
    TestMaximumValueEnumerationFromEveryOffset();
    TestAppendedDisplaySettingsUseExtendedSettingsSnapshot();
    TestSettingWritesUseServiceAndStableReasons();
    TestBoardNameUsesExactKeyWithoutShiftingSnapshotIndices();
    TestDiscoveredBoardNameFitsRealSdkAdvertisingPayload();
    TestBoardRenamePreservesPendingNativeConnection();
    TestBoardRenameDefersBleRestartUntilDisconnected();
    TestVolatilePhysicalEndColorsUseSetupBleContract();
    TestMissingServiceFailsClosed();
    TestExplicitHomeStateRequestIsReadOnlyAndIdempotent();
    TestCorrelatedSnapshotRetainsReceiptUntilPublication();
    TestMalformedControlActionUsesFullControlEnvelope();
    TestCoachingRetainsNonIdleActionReceipt();
    TestMalformedOwnerUsesCompactControlEnvelope();
    TestUnknownActionUsesControlEnvelopeThroughWriteHandling();
    TestBleOnlyDrainsTrackedControlCompletions();
    TestSettingsOutboundRetainsLocallyRejectedResults();
    TestDisconnectBeforeUpdateExecutesOnceWithoutStaleCompletion();
    TestDisconnectAfterQueuedCompletionPreservesUnrelatedOwner();
    TestRepeatedDisconnectAbandonDoesNotExhaustCompletionCapacity();
    TestStackResetReleasesQueuedBleCompletion();
    TestDisconnectDiscardsQueuedIngressBeforeRequestOwnership();
    TestDisconnectCallbackInvalidatesQueuedSettingBeforeLifecycleDrain();
    TestLegacySettingsSetIsOwnedByOneConnectionGeneration();
    TestStackResetDiscardsQueuedIngress();
    TestOtaRuntimeAcceptanceIsReadOnlyAtProductionIngress();
    TestRuntimeHealthProtocol();
    TestConnectionParameterEventOwnership();
    TestConnectionSessionNegotiatesAfterMtuExchange();
    TestOrderedModeDoesNotWaitForObjectCredit();
    TestUnknownOrderedRetainsUnsupportedReceipt();
    TestBoardStylePreviewSharedSessionMutation();
    TestSleepWakeEnumSettingsSharedSessionMutation();
    TestHintStyleSharedSessionMutation();
    TestSessionLivenessRequiresValidOwnedTrafficAndDisconnectRetires();
    TestRefusedBeginEndRetriesImmediatelyAfterQueuePressure();
    TestAcceptedFragmentPrefixPreventsGracefulObjectClose();
    TestPlayingLitchcraftStateDoesNotPublishEveryFrame();
    TestAdmittedPaintTailCannotReappearAfterPhysicalUndo();
    TestPlayingLitchcraftProducerPublishesMinutePhaseAndRetriesAdmission();
    TestComplianceUsesRealSharedStateAdapter();
    TestConnectionSessionIngressDrainImmediatelyNotifiesResponse();
    TestReverseObjectPartialProgressKeepsInFlightTail();
    TestReverseObjectKnownHoleStillRepairsSelectively();
    TestReverseObjectLostTailRetainsTimedAndQueriedRecovery();
    TestReverseObjectNotifyFalseKeepsEpochAndNextTransferSafe();
    TestReverseObjectPerPacketProgressTransfersExactBody();
    TestReverseObjectRefillAfterPartialReplayKeepsSequenceOrder();
    TestReverseObjectPerPacketProgressTransfersExactBody(true);
    TestReverseObjectDelayedFeedbackAndBitmapBoundary();
    TestReverseObjectHighestNegotiatedHoleBitKeepsCreditHeld();
    TestConnectionSessionObjectBurstFitsIngressQueue();
    TestSharedBulkCreditsFollowSlotLifetime();
    TestBulkCreditReturnTokenAndEpochFences();
    TestSharedLogicalBulkBoundAndOrdinaryFragments();
    TestObjectCapacityAndDerivedEventLifetimes();
    TestDeferredStoreBeginKeepsCancelBarrierAndRepairLive();
    TestGenericDrainRetriesFinalHistoryObjectWithoutNewIngress();
    TestPendingObjectCannotRecopyAfterMidDrainReplacement();
    g_failures += TestArduinoBleConnectionProjectionPeek();
    TestEarlyUpdaterAtBootBoundary();
    TestRuntimeBootHealthAtConnectionBoundary();
    TestAdvertisingRecoversAfterSdkRestartFailure();
    TestConnectionLifecycleSurvivesFullIngressQueue();
    TestNotifyDiagnosticAdmissionAndRecovery();
    TestTaggedNotifyDiagnosticPayloads();
    TestTaggedNotifyDiagnosticBackpressure();
    TestConsoleOutputUsesPeerMtuAndRetainsRejectedPacket();
    TestConsoleOutputOverflowIsNotReportedAsIngressLoss();
    TestConsoleOutputRejectsUnsubscribedAndStaleConnection();
    TestSettingsResponsesUseNegotiatedPeerMtu();
    TestBulkInvalidationDoesNotDependOnSettingsSubscription();

    if (g_failures == 0) {
        std::puts("[BLE_SETTINGS_CONTROL_CONTRACT] ok");
        return 0;
    }
    std::printf("[BLE_SETTINGS_CONTROL_CONTRACT] failures=%d\n", g_failures);
    return 1;
}
