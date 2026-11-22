#pragma once

#include "ble_service_test_config.hpp"
#include <NimBLEDevice.h>
#include <string>
#include <vector>

#include "modules/ble_service.hpp"
#include "firmware/hybrid_console_transport.hpp"
#include "foxie_embedded/console.hpp"
#include "modules/board_state_notification.hpp"
#include "modules/litchcraft_frame_store_receiver.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "virtual_litch_session.hpp"

namespace modules {

struct VirtualLitchPacket {
    std::string channel;
    std::string bytes;
    uint32_t connectionEpoch;
};

// Only the native SDK/characteristic boundary. All ingress, queue dispatch,
// mutations, response formatting and Home decisions use production owners.
class VirtualLitchBleAdapter final {
  public:
    explicit VirtualLitchBleAdapter(runtime::VirtualLitchSession& session, uint8_t hardwareRevision = 2U,
                                  bool corruptStore = false);
    ~VirtualLitchBleAdapter();
    void Connect(bool connected, uint16_t mtu = 247U);
    void Subscribe(const std::string& channel, bool enabled);
    void Write(const std::string& channel, const std::string& bytes);
    std::string Read(const std::string& channel);
    void Pump();
    uint32_t DisconnectRequests() const { return m_ble.m_server->DisconnectCount(); }
    uint16_t DisconnectHandle() const { return m_ble.m_server->LastDisconnect(); }
    uint32_t NativeEpoch() const { return m_ble.m_ownerNativeEpoch.load(); }
    bool SessionActivated() const { return m_ble.m_connectionSessionActivated.load(); }
    void SetStoreWorkerEnabled(bool enabled) { m_storeWorkerEnabled = enabled; }
    void SetStoreIngressEnabled(bool enabled) { m_storeIngressEnabled = enabled; }
    bool StoreStructuralPending() const { return m_frameStream.HasPendingStructuralEdit(); }
    std::vector<VirtualLitchPacket> TakePackets();
    uint32_t HomeSequence() const { return m_homePublisher.Sequence(); }
    bool HasStoredArtwork() const { return m_frameStore.HasArtwork(); }
    bool FrameReadActive() const { return m_frameReceiver.ReadActive(); }
    uint32_t StoredGeneration() const { return m_frameStore.Generation(); }
    const litchcraft_frame_store::Manifest& StoredManifest() const {
        return m_frameStore.CurrentManifest();
    }
    litchcraft_frame_stream::FrameStream::LiveAuthority StoreLiveAuthority() const {
        return m_frameStream.ReadLiveAuthority();
    }
    bool ReadCleanStoredFrame(uint16_t index,
                              litchcraft_frame_store::Frame& out);

  private:
    static void Configure(void*, SettingsStore&, ControlMutationService&, GamePersistenceCoordinator&);
    static void Drain(void*, SettingsStore&, ControlMutationService&, GamePersistenceCoordinator&);
    static void CaptureSettings(void*);
    static void CaptureConsole(void*);
    static void CaptureLitchcraft(void*);
    static void CaptureConnectionSession(void*);
    static void CaptureOtaControl(void*);
    static void ServiceFrameIngress(
        void*, BleOtaChunkQueue&, litchcraft_frame_store::StoreProjection,
        uint32_t, uint16_t);
    static void ServiceFramePriority(void*, const uint8_t*, size_t, uint32_t);
    static bool PeekFrameResponse(void*, uint8_t*, size_t, size_t&);
    static void AcceptFrameResponse(void*);
    static bool AdmitGameHistoryFrame(
        void*, const uint8_t*, size_t, uint32_t, bool);
    static bool PeekGameHistoryFrame(
        void*, uint8_t*, size_t, size_t&, uint32_t&, uint32_t);
    static void AcceptGameHistoryFrame(void*, uint32_t, uint32_t);
    void Capture(const char* channel, const NimBLECharacteristic& source);
    void PublishFrameResponse(
        const litchcraft_frame_store::StoreIngressResult& result);
    void AdoptFrameOwner(uint32_t ownerEpoch);
    void SynchronizeFrameStream();

    struct MemoryFrameFile final {
        std::vector<uint8_t> bytes = std::vector<uint8_t>(
            litchcraft_frame_store::ARTWORK_FILE_CAPACITY, 0U);
        static bool Read(void*, size_t, uint8_t*, size_t);
        static bool Write(void*, size_t, const uint8_t*, size_t);
        static bool Sync(void*) { return true; }
        litchcraft_frame_store::FrameStore::Ops Ops() {
            return {this, &Read, &Write, &Sync};
        }
    };

    runtime::VirtualLitchSession& m_session;
    uint8_t m_hardwareRevision;
    bool m_corruptStore;
    struct DisconnectedSerial {
        int available() const { return 0; }
        int read() const { return -1; }
        int availableForWrite() const { return 0; }
        size_t write(const uint8_t*, size_t) { return 0U; }
        explicit operator bool() const { return false; }
    };
    BleService m_ble;
    DisconnectedSerial m_serial;
    firmware_internal::HybridConsoleTransport<DisconnectedSerial, BleService> m_transport;
    fxe::Console m_consoleWriter;
    BoardHealthLine m_lastHealth;
    NimBLEService m_settingsService;
    NimBLECharacteristic m_console;
    NimBLECharacteristic m_litchcraft, m_data, m_otaControl;
    NimBLECharacteristic m_connectionSession;
    LitchcraftPersistence::AdvancedStorage m_artStorage;
    MemoryFrameFile m_frameFile;
    litchcraft_frame_store::FrameStore m_frameStore;
    litchcraft_frame_store::StoreReceiver m_frameReceiver;
    litchcraft_frame_stream::FrameStream m_frameStream;
    std::array<uint8_t, litchcraft_frame_store::STORE_RESPONSE_MAX_BYTES> m_frameResponse{};
    uint16_t m_frameResponseSize = 0U;
    uint32_t m_frameOwnerEpoch = 0U;
    bool m_storeWorkerEnabled = true;
    bool m_storeIngressEnabled = true;
    std::array<uint8_t, 20U> m_gameHistoryFrame{};
    uint8_t m_gameHistoryFrameSize = 0U;
    uint32_t m_gameHistoryFrameToken = 0U;
    bool m_gameHistoryFrameReady = false;
    uint32_t m_storageReceipt = 0U;
    NimBLECharacteristic* m_readWrite = nullptr;
    HomeStateNotificationPublisher m_homePublisher;
    std::vector<VirtualLitchPacket> m_packets;
};

} // namespace modules
