#include "virtual_litch_ble_adapter.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/home_state_wire_codec.hpp"
#include "modules/settings_state_wire_codec.hpp"

namespace modules {

VirtualLitchBleAdapter::VirtualLitchBleAdapter(
    runtime::VirtualLitchSession& session, const uint8_t hardwareRevision,
    const bool corruptStore) : m_session(session),
    m_hardwareRevision(hardwareRevision), m_corruptStore(corruptStore),
    m_transport(m_serial, m_ble), m_consoleWriter(m_transport, {.installBuiltins = false}),
    m_frameStore(m_frameFile.Ops()), m_frameReceiver(m_frameStore) {
    NimBLEDevice::SetTestMtu(247U);
    if (corruptStore) m_frameFile.bytes[0U] = 0xa5U;
    m_session.RunControlIngress(&Configure, this);
}

VirtualLitchBleAdapter::~VirtualLitchBleAdapter() {
    m_ble.m_bleSettings.Reset();
}

bool VirtualLitchBleAdapter::ReadCleanStoredFrame(
    const uint16_t index, litchcraft_frame_store::Frame& out) {
    const auto authority = m_frameStream.ReadLiveAuthority();
    if (!m_frameStore.HasArtwork() || authority.dirty ||
        authority.generation != m_frameStore.Generation() ||
        index >= m_frameStore.CurrentManifest().frameCount) {
        return false;
    }
    return m_frameStore.ReadFrame(index, out) ==
        litchcraft_frame_store::Status::Ok;
}

void VirtualLitchBleAdapter::Configure(
    void* context, SettingsStore& settings, ControlMutationService& controls, GamePersistenceCoordinator& game) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    auto& ble = self.m_ble;
    if (!ble.m_eventQueue.Begin() || !ble.m_connectionProjection.Begin() || !ble.m_otaChunkQueue.Begin()) {
        throw std::runtime_error("BLE ingress unavailable");
    }
    ble.m_settings = &settings;
    ble.m_server = NimBLEDevice::getServer();
    ble.m_bleSettings.Configure(&settings, &ble.m_eventQueue,
                                 &ble.m_droppedEvents);
    ble.SetControlMutationService(&controls);
    ble.SetLitchcraftCoordinator(&game);
    ble.SetLitchcraftRuntimeEligible(true);
    ble.m_connectionSessionBootNonce = 0x4c4300014c430002ULL;
    game.Litchcraft().Configure(self.m_hardwareRevision, 0x4c430001U);
    // The V3 simulation supplies an available motion sensor.
    game.Litchcraft().ConfigureShakeErase(self.m_hardwareRevision == 3U);
    game.Litchcraft().AttachFrameStream(&self.m_frameStream);
    self.m_frameReceiver.BindLiveStream(self.m_frameStream);
    const auto recovered = self.m_frameReceiver.Recover({});
    if (self.m_corruptStore && recovered != litchcraft_frame_store::Status::Corrupt) {
        throw std::runtime_error("corrupt Store fixture must fail closed through production recovery");
    }
    ble.SetLitchcraftFrameIngressCallbacks(
        &ServiceFrameIngress, &ServiceFramePriority, &PeekFrameResponse,
        &AcceptFrameResponse,
        [](void*, uint16_t, uint32_t) { return false; }, &self);
    ble.SetConnectionGameHistoryIngressCallback(
        &AdmitGameHistoryFrame, &self);
    ble.SetConnectionGameHistoryOutputCallbacks(
        &PeekGameHistoryFrame, &AcceptGameHistoryFrame, &self);
    // Persistence worker boundary: immediately successful receipts isolate
    // transport latency. This fixture does not claim physical flash timing.
    const LitchcraftPersistence::Ops persistenceOps{
        &self,
        [](void* context, const JsonDocument&, uint32_t& receipt) {
            receipt = ++static_cast<VirtualLitchBleAdapter*>(context)->m_storageReceipt;
            return fxe::SettingsStore::AsyncSaveAdmission::Accepted;
        },
        [](void*, uint32_t) { return LitchcraftPersistence::SaveOutcome::Saved; },
        [](void*, uint32_t) {},
        [](void*, LitchcraftPersistence::AdvancedSlot, uint8_t*, size_t, size_t&) {
            return LitchcraftPersistence::AdvancedReadOutcome::Missing;
        },
        [](void* context, const uint8_t*, size_t, uint32_t& receipt) {
            receipt = ++static_cast<VirtualLitchBleAdapter*>(context)->m_storageReceipt;
            return fxe::SettingsStore::AsyncSaveAdmission::Accepted;
        }};
    if (self.m_corruptStore) {
        game.ArtPersistence().ConfigureStoreOnly(persistenceOps);
    } else {
        game.ArtPersistence().Configure(persistenceOps);
        game.ArtPersistence().ConfigureAdvancedStorage(self.m_artStorage);
    }
    self.m_litchcraft.SetNotifyHook(&CaptureLitchcraft, &self);
    self.m_connectionSession.SetNotifyHook(&CaptureConnectionSession, &self);
    self.m_otaControl.SetNotifyHook(&CaptureOtaControl, &self);
    ble.m_litchcraftCharacteristic = &self.m_litchcraft;
    ble.m_connectionSessionEgressCharacteristic = &self.m_connectionSession;
    ble.m_otaControlCharacteristic = &self.m_otaControl;
    ble.m_otaDataCharacteristic = &self.m_data;
    if (!ble.m_bleSettings.CreateCharacteristics(self.m_settingsService)) {
        throw std::runtime_error("BLE characteristics unavailable");
    }
    self.m_readWrite = self.m_settingsService.CharacteristicAt(1U);
    self.m_readWrite->SetNotifyHook(&CaptureSettings, &self);
    self.m_console.SetNotifyHook(&CaptureConsole, &self);
    ble.m_consoleStreamCharacteristic = &self.m_console;
}

bool VirtualLitchBleAdapter::MemoryFrameFile::Read(
    void* context, const size_t offset, uint8_t* out, const size_t size) {
    auto& self = *static_cast<MemoryFrameFile*>(context);
    if (out == nullptr || offset > self.bytes.size() ||
        size > self.bytes.size() - offset) {
        return false;
    }
    std::memcpy(out, self.bytes.data() + offset, size);
    return true;
}

bool VirtualLitchBleAdapter::AdmitGameHistoryFrame(
    void* const context, const uint8_t* const bytes, const size_t size,
    const uint32_t, const bool) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    if (bytes == nullptr || size < 8U ||
        size > self.m_gameHistoryFrame.size()) {
        return false;
    }
    std::copy(bytes, bytes + size, self.m_gameHistoryFrame.begin());
    // This fixture exercises the production transport boundary while the
    // storage-backed owner remains deliberately separate. Reply with the
    // protocol's exact CurrentState envelope for the admitted transaction.
    self.m_gameHistoryFrame[2U] = 0x21U;
    self.m_gameHistoryFrame[3U] = 0U;
    self.m_gameHistoryFrame[6U] = 1U;
    self.m_gameHistoryFrame[7U] = 0U;
    std::fill(self.m_gameHistoryFrame.begin() + 8U,
              self.m_gameHistoryFrame.begin() + size, 0U);
    self.m_gameHistoryFrameSize = static_cast<uint8_t>(size);
    ++self.m_gameHistoryFrameToken;
    if (self.m_gameHistoryFrameToken == 0U) {
        self.m_gameHistoryFrameToken = 1U;
    }
    self.m_gameHistoryFrameReady = true;
    return true;
}

bool VirtualLitchBleAdapter::PeekGameHistoryFrame(
    void* const context, uint8_t* const out, const size_t capacity,
    size_t& size, uint32_t& token, const uint32_t) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    size = 0U;
    token = 0U;
    if (!self.m_gameHistoryFrameReady || out == nullptr ||
        capacity < self.m_gameHistoryFrameSize) {
        return false;
    }
    std::copy_n(self.m_gameHistoryFrame.begin(), self.m_gameHistoryFrameSize,
                out);
    size = self.m_gameHistoryFrameSize;
    token = self.m_gameHistoryFrameToken;
    return true;
}

void VirtualLitchBleAdapter::AcceptGameHistoryFrame(
    void* const context, const uint32_t token, const uint32_t) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    if (self.m_gameHistoryFrameReady &&
        token == self.m_gameHistoryFrameToken) {
        self.m_gameHistoryFrameReady = false;
    }
}

bool VirtualLitchBleAdapter::MemoryFrameFile::Write(
    void* context, const size_t offset, const uint8_t* bytes,
    const size_t size) {
    auto& self = *static_cast<MemoryFrameFile*>(context);
    if (bytes == nullptr || offset > self.bytes.size() ||
        size > self.bytes.size() - offset) {
        return false;
    }
    std::memcpy(self.bytes.data() + offset, bytes, size);
    return true;
}

void VirtualLitchBleAdapter::PublishFrameResponse(
    const litchcraft_frame_store::StoreIngressResult& result) {
    if (!result.responseReady || result.responseSize == 0U) return;
    std::copy_n(result.response.data(), result.responseSize,
                m_frameResponse.begin());
    m_frameResponseSize = result.responseSize;
}

void VirtualLitchBleAdapter::AdoptFrameOwner(const uint32_t ownerEpoch) {
    if (m_frameOwnerEpoch == ownerEpoch) return;
    m_frameReceiver.EndOwner(m_frameOwnerEpoch);
    m_frameResponseSize = 0U;
    m_frameOwnerEpoch = ownerEpoch;
}

void VirtualLitchBleAdapter::SynchronizeFrameStream() {
    if (!m_frameStore.HasArtwork() || m_frameStore.Generation() == 0U) {
        m_frameStream.Clear();
        return;
    }
    if (m_frameStream.Generation() != m_frameStore.Generation()) {
        (void)m_frameStream.Configure(
            m_frameStore.CurrentManifest(), m_frameStore.Generation());
    }
    // Committed replacement authority starts paused. Pump follows the live
    // session's playing state after adoption; this initial warmup stays Next.
    for (uint8_t count = 0U; count < 8U; ++count) {
        const auto result = m_frameStream.Service(m_frameStore, false,
            litchcraft_frame_stream::FrameStream::ServiceMode::PagingAndEdits,
            litchcraft_frame_stream::FrameStream::PrefetchPolicy::Next);
        (void)m_frameStream.TryPresentRequested();
        if (result == litchcraft_frame_stream::FrameStream::ServiceResult::Idle ||
            result == litchcraft_frame_stream::FrameStream::ServiceResult::Corrupt ||
            result == litchcraft_frame_stream::FrameStream::ServiceResult::IoFailure) {
            break;
        }
    }
}

void VirtualLitchBleAdapter::ServiceFrameIngress(
    void* context, BleOtaChunkQueue& queue,
    const litchcraft_frame_store::StoreProjection projection,
    const uint32_t ownerEpoch, const uint16_t packetMaximum) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    // Hold the real bulk FIFO at its existing worker callback boundary.
    // Admission, retained credits, priority Cancel and reverse output still run.
    if (!self.m_storeIngressEnabled) return;
    self.AdoptFrameOwner(ownerEpoch);
    if (self.m_frameResponseSize != 0U) return;
    self.m_frameReceiver.SetSnapshotPacketMaximum(packetMaximum);
    self.m_frameReceiver.SetProjection(projection);
    while (queue.TryPeekLitchcraftFrameStore()) {
        const uint8_t* bytes = nullptr;
        size_t size = 0U;
        uint32_t ordinal = 0U;
        uint32_t packetEpoch = 0U;
        if (!queue.TryAcquireReadSlot(
                bytes, size, ordinal, packetEpoch)) {
            break;
        }
        (void)ordinal;
        litchcraft_frame_store::StoreIngressResult result{};
        if (packetEpoch == queue.CurrentEpoch()) {
            const bool selecting = size >= 3U &&
                bytes[2U] == litchcraft_frame_store::STORE_SELECT;
            litchcraft_frame_store::FrameStore::ResidentDescriptor previous{};
            const bool hadPrevious = selecting &&
                self.m_frameStore.CurrentResident(previous);
            const litchcraft_frame_store::StoreProjection oldProjection{
                self.m_session.Litchcraft().SessionId(),
                self.m_session.Litchcraft().SelectedFrame(),
                self.m_frameStore.HasArtwork()
                    ? self.m_frameStore.CurrentManifest().topology : uint8_t{3U},
                self.m_session.Litchcraft().IsActive(),
                self.m_session.Litchcraft().IsAnimationPlaying()};
            self.m_frameReceiver.Handle(bytes, size, ownerEpoch, result);
            if (selecting && result.playbackReady &&
                !self.m_frameStream.StageFirstFrame(
                    self.m_frameStore.CurrentManifest(),
                    self.m_frameStore.Generation(),
                    self.m_frameStore.DecodedFrame())) {
                const bool editPending = self.m_frameStream.HasUnpersistedEdit();
                if (hadPrevious &&
                    self.m_frameStore.Generation() != previous.generation) {
                    (void)self.m_frameStore.UndoResidentSelection(previous);
                }
                self.m_frameReceiver.RejectResidentPresentation(
                    oldProjection, fxe::ReadU16LE(bytes + 7U),
                    editPending ? litchcraft_frame_store::WireStatus::Busy
                                : litchcraft_frame_store::WireStatus::ResourceLimit,
                    result);
            }
            if (result.playbackReady &&
                self.m_frameReceiver.IncomingPreviewReady()) {
                (void)self.m_frameStream.StageFirstFrame(
                    self.m_frameReceiver.IncomingManifest(),
                    self.m_frameReceiver.IncomingGeneration(),
                    self.m_frameReceiver.IncomingFirstFrame());
            }
        }
        const bool committed = size >= 3U &&
            bytes[2U] == litchcraft_frame_store::STORE_COMMIT &&
            result.responseReady && result.response[13U] == 0U;
        queue.ReleaseReadSlot();
        if (committed) self.SynchronizeFrameStream();
        self.PublishFrameResponse(result);
        if (result.responseReady) break;
    }
}

void VirtualLitchBleAdapter::ServiceFramePriority(
    void* context, const uint8_t* bytes, const size_t size,
    const uint32_t ownerEpoch) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    self.AdoptFrameOwner(ownerEpoch);
    litchcraft_frame_store::StoreIngressResult result{};
    self.m_frameReceiver.Handle(bytes, size, ownerEpoch, result);
    self.PublishFrameResponse(result);
}

bool VirtualLitchBleAdapter::PeekFrameResponse(
    void* context, uint8_t* out, const size_t capacity, size_t& outSize) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    outSize = 0U;
    if (out == nullptr || self.m_frameResponseSize == 0U ||
        capacity < self.m_frameResponseSize) {
        return false;
    }
    std::copy_n(self.m_frameResponse.begin(), self.m_frameResponseSize, out);
    outSize = self.m_frameResponseSize;
    return true;
}

void VirtualLitchBleAdapter::AcceptFrameResponse(void* context) {
    static_cast<VirtualLitchBleAdapter*>(context)->m_frameResponseSize = 0U;
}

void VirtualLitchBleAdapter::Connect(const bool connected, const uint16_t mtu) {
    if (connected) {
        if (mtu < 23U || mtu > 517U) {
            throw std::invalid_argument("MTU must be between 23 and 517");
        }
        m_ble.m_server->SetPeerMtu(1U, mtu);
        m_console.SetNotifyPayloadLimit(mtu - 3U);
        m_litchcraft.SetNotifyPayloadLimit(mtu - 3U);
        m_connectionSession.SetNotifyPayloadLimit(mtu - 3U);
        m_otaControl.SetNotifyPayloadLimit(mtu - 3U);
        m_readWrite->SetNotifyPayloadLimit(mtu - 3U);
        m_ble.OnConnected(1U, "VirtualLitch", mtu, 1U);
    } else {
        AdoptFrameOwner(0U);
        m_ble.m_server->SetPeerSlots({});
        m_ble.OnDisconnected();
    }
    Pump();
}

void VirtualLitchBleAdapter::Subscribe(const std::string& channel,
                                       const bool enabled) {
    if (channel == "litchcraft") {
        m_ble.OnLitchcraftSubscription(enabled ? 1U : 0U);
    } else if (channel == "connection_session") {
        m_ble.OnConnectionSessionSubscription(enabled ? 1U : 0U);
    } else if (channel == "settings") {
        m_ble.m_bleSettings.OnReadWriteSubscribe(enabled ? 1U : 0U);
    } else if (channel == "console") {
        m_ble.OnConsoleStreamSubscriptionChanged(enabled ? 1U : 0U);
    } else if (channel == "ota_control") {
        m_ble.OnOtaControlSubscriptionChanged(enabled ? 1U : 0U);
    } else {
        throw std::invalid_argument("unsupported subscription channel");
    }
    Pump();
}

void VirtualLitchBleAdapter::Write(const std::string& channel,
                                   const std::string& bytes) {
    if (!m_ble.IsConnected()) {
        throw std::logic_error("write while disconnected");
    }
    if (channel == "litchcraft") {
        m_ble.OnLitchcraftWrite(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    } else if (channel == "connection_session") {
        m_ble.OnConnectionSessionWrite(
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    } else if (channel == "artwork_data") {
        if (bytes.size() < 3U || uint8_t(bytes[0]) != 0x4dU ||
            uint8_t(bytes[1]) != 2U || uint8_t(bytes[2]) < 15U || uint8_t(bytes[2]) > 17U) {
            throw std::invalid_argument("only Litchcraft bulk is supported");
        }
        m_ble.OnOtaDataCharacteristicWrite(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    } else if (channel == "settings") {
        m_ble.m_bleSettings.OnReadWritePayload(bytes.c_str());
    } else if (channel == "command") {
        m_ble.OnCommandCharacteristicWrite(bytes.data(), bytes.size());
    } else if (channel == "ota_control") {
        m_ble.OnOtaControlCharacteristicWrite(bytes.data(), bytes.size());
    } else {
        throw std::invalid_argument("unsupported write channel");
    }
    Pump();
}

std::string VirtualLitchBleAdapter::Read(const std::string& channel) {
    if (channel == "enumerate") {
        auto* characteristic = m_settingsService.CharacteristicAt(0U);
        m_ble.m_bleSettings.OnEnumerateRead(*characteristic);
        return characteristic->getValue();
    }
    if (channel == "settings") {
        return m_readWrite->getValue();
    }
    throw std::invalid_argument("unsupported read channel");
}

void VirtualLitchBleAdapter::Drain(
    void* context, SettingsStore& settings, ControlMutationService&, GamePersistenceCoordinator&) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    self.m_ble.DrainEventQueue();
    if (self.m_ble.m_bleSettings.UpdateChangeNotifications(settings, self.m_ble.IsConnected())) {
        self.m_ble.m_consoleStreamSnapshotRequested.store(true, std::memory_order_release);
    }
}

void VirtualLitchBleAdapter::Pump() {
    // Session and transport share one explicit clock; wall time never drives
    // publication, response completion or console notification pacing.
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(m_session.State().now_ms);
    // Execute the production loop-owned disconnect effect at this SDK seam.
    m_ble.PollConnectionRenewal();
    m_session.RunControlIngress(&Drain, this);
    // The production storage worker continues one Snapshot request after its
    // first response. The single-threaded bridge must service that same owner
    // while reverse Object delivery and Progress provide the backpressure.
    if (m_frameOwnerEpoch != 0U && m_frameResponseSize == 0U &&
        m_frameReceiver.ReadActive() &&
        !m_ble.m_otaChunkQueue.TryPeekLitchcraftFrameStore()) {
        litchcraft_frame_store::StoreIngressResult result{};
        m_frameReceiver.ContinueRead(m_frameOwnerEpoch, result);
        PublishFrameResponse(result);
    }
    // The production storage worker services dirty streamed frames on the
    // same monotonic clock. Keep that owner running in the bridge so a clean
    // root is meaningful after its normal save-quiet interval.
    if (m_storeWorkerEnabled && m_frameStream.Generation() != 0U &&
        (!m_frameStream.HasUnpersistedEdit() ||
         m_frameStream.EditSaveReady(fxe::Millis()))) {
        const auto result = m_frameStream.Service(m_frameStore, false,
            litchcraft_frame_stream::FrameStream::ServiceMode::PagingAndEdits,
            m_session.Litchcraft().IsAnimationPlaying()
                ? litchcraft_frame_stream::FrameStream::PrefetchPolicy::TwoAhead
                : litchcraft_frame_stream::FrameStream::PrefetchPolicy::Next);
        if (result == litchcraft_frame_stream::FrameStream::ServiceResult::Corrupt ||
            result == litchcraft_frame_stream::FrameStream::ServiceResult::IoFailure) {
            throw std::runtime_error("VirtualLitch Store worker failed: stream=" +
                std::to_string(m_frameStream.Generation()) + " store=" +
                std::to_string(m_frameStore.Generation()) + " result=" +
                std::to_string(static_cast<int>(result)));
        }
        (void)m_frameStream.TryPresentRequested();
    }
    const auto healthState = m_session.BoardHealthConnectionState();
    const auto healthRevision = static_cast<uint32_t>(
        fxe::ReadU16LE(healthState.runtime.data() + 1U)) + 1U;
    (void)m_ble.PublishConnectionState(
        ConnectionSession::RUNTIME_HEALTH_STATE_CODE, healthRevision,
        healthState.runtime.data(), healthState.runtime.size());
    uint32_t contextIdentity = fxe::Crc32(
        healthState.context.data(), healthState.context.size());
    if (contextIdentity == 0U) contextIdentity = 1U;
    (void)m_ble.PublishConnectionState(
        ConnectionSession::BOARD_HEALTH_CONTEXT_STATE_CODE, contextIdentity,
        healthState.context.data(), healthState.context.size());
    settings_state_wire::Payload settingsPayload;
    if (settings_state_wire::Encode(
            m_session.SettingsForPersistenceTest(), settingsPayload)) {
        uint32_t identity =
            fxe::Crc32(settingsPayload.data(), settingsPayload.size());
        if (identity == 0U) identity = 1U;
        for (uint8_t fragment = 0U;
             fragment < settings_state_wire::FRAGMENT_COUNT; ++fragment) {
            (void)m_ble.PublishConnectionState(
                settings_state_wire::FragmentCode(fragment),
                identity,
                settingsPayload.data() + static_cast<size_t>(fragment) *
                    settings_state_wire::FRAGMENT_BYTES,
                settings_state_wire::FragmentLength(fragment));
        }
    }
    if (m_session.CanPublishHomeState()) {
        home_state_wire::Payload payload;
        if (home_state_wire::Encode(m_session.HomeStateSnapshot(), payload)) {
            uint32_t identity = fxe::Crc32(payload.data(), payload.size());
            if (identity == 0U) identity = 1U;
            for (uint8_t fragment = 0U;
                 fragment < home_state_wire::FRAGMENT_COUNT; ++fragment) {
                (void)m_ble.PublishConnectionState(
                    static_cast<uint8_t>(
                        home_state_wire::FIRST_STATE_CODE + fragment),
                    identity,
                    payload.data() + static_cast<size_t>(fragment) *
                        home_state_wire::FRAGMENT_BYTES,
                    home_state_wire::FRAGMENT_BYTES);
            }
        }
    }
    bool force = false;
    uint32_t receipt = 0U;
    HomeStateNotificationLine line;
    if (m_session.CanPublishHomeState() && m_ble.TryBeginHomeStatePublication(force, &receipt)) {
        (void)m_session.TryPrepareHomePublication(m_homePublisher, force, line);
        const auto health = m_session.BoardHealthStateLine();
        WriteBoardStatePublication(m_consoleWriter, m_lastHealth,
            {line, health, m_session.SettingsForPersistenceTest(), force, receipt});
    }
    m_ble.UpdateLitchcraftNotifications();
    m_ble.UpdateConnectionSessionNotifications();
    m_ble.FlushConsoleOutputBuffer();
}

void VirtualLitchBleAdapter::Capture(
    const char* channel, const NimBLECharacteristic& source) {
    if (m_packets.size() >= 128U) {
        throw std::runtime_error("undrained virtual BLE packet capacity");
    }
    m_packets.push_back({channel, source.LastTx(),
                        m_ble.m_bleSettings.CurrentConnectionEpoch()});
}

void VirtualLitchBleAdapter::CaptureSettings(void* context) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    self.Capture("settings", *self.m_readWrite);
}

void VirtualLitchBleAdapter::CaptureConsole(void* context) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    self.Capture("console", self.m_console);
}

void VirtualLitchBleAdapter::CaptureLitchcraft(void* context) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    self.Capture("litchcraft", self.m_litchcraft);
}

void VirtualLitchBleAdapter::CaptureConnectionSession(void* context) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    self.Capture("connection_session", self.m_connectionSession);
}

void VirtualLitchBleAdapter::CaptureOtaControl(void* context) {
    auto& self = *static_cast<VirtualLitchBleAdapter*>(context);
    self.Capture("ota_control", self.m_otaControl);
}

std::vector<VirtualLitchPacket> VirtualLitchBleAdapter::TakePackets() {
    auto packets = std::move(m_packets);
    m_packets.clear();
    return packets;
}

// These are the excluded radio/OTA boundaries. A reconnect may ask the radio
// to advertise; the SDK-facing fixture supplies the next connection event.
void BleService::StartAdvertising() {}
bool BleService::StopAdvertising() { return true; }

} // namespace modules
