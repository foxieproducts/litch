#include "modules/ble_service.hpp"

#include <ArduinoJson.h>

#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#endif

#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "hardware/hardware.hpp"
#include "modules/ble_device_capabilities.hpp"
#include "modules/ble_service_internal.hpp"
#include "modules/game_persistence_coordinator.hpp"

namespace modules {

#if defined(LITCH_BLE)
namespace {

#if defined(LITCH_BLE_OTA)
constexpr uint16_t kRecommendedFastOtaChunkBytes =
    static_cast<uint16_t>(ble_internal::BLE_FAST_DATA_LEN_OCTETS - 7U);
#endif

size_t GetOtaNotifyPayloadLimit(const uint16_t mtu) {
    uint16_t effectiveMtu = mtu;
    if (effectiveMtu < 23U) {
        effectiveMtu = 23U;
    }

    size_t maxNotifyPayload = static_cast<size_t>(effectiveMtu - 3U);
    if (maxNotifyPayload < 20U) {
        maxNotifyPayload = 20U;
    }
    if (maxNotifyPayload > ble_internal::BLE_MAX_ATT_PAYLOAD_BYTES) {
        maxNotifyPayload = ble_internal::BLE_MAX_ATT_PAYLOAD_BYTES;
    }
    return maxNotifyPayload;
}

} // namespace

#if defined(LITCH_BLE_OTA)
uint16_t BleService::GetOtaMaxChunkBytes() const {
    uint16_t mtu = GetMtu();
    if (mtu < 23U) {
        mtu = 23U;
    }

    uint16_t payload = static_cast<uint16_t>(mtu - 3U);
    if (payload < 20U) {
        payload = 20U;
    }
    if (payload > ble_internal::BLE_MAX_ATT_PAYLOAD_BYTES) {
        payload = ble_internal::BLE_MAX_ATT_PAYLOAD_BYTES;
    }
    return payload;
}

uint16_t BleService::GetOtaRecommendedChunkBytes() const {
    const uint16_t maxChunkBytes = GetOtaMaxChunkBytes();
    if (kRecommendedFastOtaChunkBytes < 20U) {
        return maxChunkBytes;
    }
    return (kRecommendedFastOtaChunkBytes < maxChunkBytes) ? kRecommendedFastOtaChunkBytes : maxChunkBytes;
}

uint8_t BleService::GetOtaRecommendedBurstCount() const {
    return 48U;
}

uint32_t BleService::GetOtaRecommendedWindowBytes() const {
    return static_cast<uint32_t>(GetOtaRecommendedChunkBytes()) *
           static_cast<uint32_t>(GetOtaRecommendedBurstCount());
}
#endif

bool BleService::NotifyOtaJson(const char* jsonPayload) {
    if (!m_recoveryOnly && !OwnerProductAccess() &&
        (jsonPayload == nullptr || std::strncmp(jsonPayload, "{\"i\":[", 6U) != 0)) return false;
    if (!IsConnected() || jsonPayload == nullptr || jsonPayload[0] == '\0') {
        return false;
    }

    const size_t payloadLen = std::strlen(jsonPayload);
    if (m_connectionSession.IsActivated()) {
        return QueueConnectionOtaStatus(
            reinterpret_cast<const uint8_t*>(jsonPayload), payloadLen);
    }
    if (m_otaControlCharacteristic == nullptr) return false;

    const size_t maxNotifyPayload = GetOtaNotifyPayloadLimit(GetMtu());

    const size_t notifyLen =
        payloadLen > maxNotifyPayload ? maxNotifyPayload : payloadLen;

    m_otaControlCharacteristic->setValue(
        reinterpret_cast<const uint8_t*>(jsonPayload), notifyLen);
    return (m_recoveryOnly || OwnerProductAccess() ||
            std::strncmp(jsonPayload, "{\"i\":[", 6U) == 0) &&
        m_otaControlCharacteristic->notify(reinterpret_cast<const uint8_t*>(jsonPayload),
                                           notifyLen, m_connectionHandle);
}

#if defined(LITCH_BLE_OTA)
void BleService::NotifyOtaProbe(const char* mode, const uint32_t requestedSize) {
    const size_t maxNotifyPayload = GetOtaNotifyPayloadLimit(GetMtu());
    if (maxNotifyPayload <= 20U) {
        NotifyOtaJson("{\"proto\":2}");
        return;
    }

    char payload[224] = {};
    std::snprintf(payload,
                  sizeof(payload),
                  "{\"st\":\"probe\",\"proto\":2,\"mode\":\"%s\",\"size\":%lu,\"max_chunk\":%u,\"chunk\":%u,\"burst\":%u,\"window\":%lu,\"status_ms\":%lu,\"supports_sim\":1,\"supports_notify_test\":1}",
                  (mode != nullptr && mode[0] != '\0') ? mode : "flash",
                  static_cast<unsigned long>(requestedSize),
                  static_cast<unsigned>(GetOtaMaxChunkBytes()),
                  static_cast<unsigned>(GetOtaRecommendedChunkBytes()),
                  static_cast<unsigned>(GetOtaRecommendedBurstCount()),
                  static_cast<unsigned long>(GetOtaRecommendedWindowBytes()),
                  static_cast<unsigned long>(ble_internal::MIN_OTA_STATUS_INTERVAL_MS));
    NotifyOtaJson(payload);
}
#endif

void BleService::NotifyDeviceInfo() {
    if (m_recoveryOnly) {
        char payload[21] = {};
        if (ble_device_capabilities::FormatRecoveryInfo(
                m_hardwareRevision,
                hardware::HW_SUPPORTS_BLE_OTA && m_otaPlatformAdapter != nullptr,
                payload, sizeof(payload))) NotifyOtaJson(payload);
        return;
    }
#if defined(LITCH_BLE_OTA)
    static_assert(hardware::HW_SUPPORTS_BLE_OTA,
                  "OTA-enabled build must advertise OTA support");
#else
    static_assert(!hardware::HW_SUPPORTS_BLE_OTA,
                  "non-OTA build must not advertise OTA support");
#endif

    const bool supportsLitchcraft =
        m_litchcraftGame != nullptr && m_litchcraftCharacteristic != nullptr;
#if defined(LITCH_BLE_OTA)
    const bool supportsLitchcraftBulk =
        supportsLitchcraft && m_otaDataCharacteristic != nullptr;
#else
    const bool supportsLitchcraftBulk = false;
#endif
#if defined(LITCH_BLE_OTA)
    const bool supportsLitchcraftAnimation =
        supportsLitchcraft &&
        m_litchcraftFrameIngressCallback != nullptr &&
        m_litchcraftFrameResponsePeekCallback != nullptr &&
        m_litchcraftFrameResponseAcceptCallback != nullptr;
#else
    const bool supportsLitchcraftAnimation = false;
#endif
    const bool supportsGameHistory =
        m_connectionGameHistoryIngressCallback != nullptr &&
        m_connectionGameHistoryPeekCallback != nullptr &&
        m_connectionGameHistoryAcceptCallback != nullptr;
    char payload[21] = {};
    const bool formatted = ble_device_capabilities::FormatCompactInfo(
        m_hardwareRevision,
        hardware::HW_SUPPORTS_BLE_OTA,
        m_runtimeHealthRecheckCallback != nullptr,
        supportsGameHistory, false,
        payload,
        sizeof(payload),
        supportsLitchcraft,
        supportsLitchcraftBulk,
        supportsLitchcraft,
        supportsLitchcraft,
        supportsLitchcraftAnimation,
        supportsLitchcraft &&
            m_litchcraftGame->Litchcraft().IsShakeEraseSupported(), OwnerPairingEnabled());
    if (formatted) {
        NotifyOtaJson(payload);
    }
}

#if defined(LITCH_BLE_OTA)
void BleService::NotifyOtaBeginAccepted() {
    const size_t maxNotifyPayload = GetOtaNotifyPayloadLimit(GetMtu());
    if (maxNotifyPayload <= 20U) {
        NotifyOtaJson("{\"b\":1}");
        return;
    }

    char payload[208] = {};
    std::snprintf(payload,
                  sizeof(payload),
                  "{\"st\":\"begin\",\"proto\":2,\"sim\":%u,\"rx\":%lu,\"total\":%lu,\"chunk\":%u,\"burst\":%u,\"window\":%lu}",
                  m_otaSimulationMode ? 1U : 0U,
                  static_cast<unsigned long>(m_otaBytesReceived),
                  static_cast<unsigned long>(m_otaExpectedSize),
                  static_cast<unsigned>(m_otaNegotiatedChunkBytes),
                  static_cast<unsigned>(m_otaNegotiatedBurstCount),
                  static_cast<unsigned long>(m_otaNegotiatedWindowBytes));
    NotifyOtaJson(payload);
}

void BleService::StartOtaIdentify(const uint8_t number, const uint32_t durationMs) {
    m_identifyNumber = number;
    m_identifyRed = 0U;
    m_identifyGreen = 96U;
    m_identifyBlue = 255U;
    m_identifyPadTwoDigits = true;
    m_identifyDurationMs = durationMs;
    m_identifyStopPending = false;
    m_identifyStartPending = true;
}

void BleService::StopOtaIdentify() {
    m_identifyStartPending = false;
    m_identifyStopPending = true;
}

void BleService::ResetOtaTransferState(const bool stopIdentify) {
    if (m_litchcraftGame != nullptr) m_litchcraftGame->Litchcraft().SetMutationsPaused(false);
    m_pendingOtaEnd.len = 0U;
    m_nextOtaPersistencePrepareAtMs = 0U;
    m_otaEndFlushRequested = false;
    m_otaEndPersistenceReady = false;
    m_otaInProgress = false;
    m_otaSimulationMode = false;
    m_otaPendingReboot = false;
    m_otaRebootArmedAtMs = 0U;
    m_otaRebootGraceArmed = false;
    m_otaRebootDisconnectAtMs = 0U;
    m_otaRebootDisconnectIssued = false;
    m_connectionOtaRebootSequence = 0U;
    m_connectionOtaProgressDirty = false;
    m_otaExpectedSize = 0;
    m_otaBytesReceived = 0;
    m_lastOtaStatusMs = 0;
    m_otaStartedAtMs = 0;
    m_otaProgressPercent = 0;
    m_otaCrc32 = 0U;
    m_otaNegotiatedChunkBytes = 0U;
    m_otaNegotiatedBurstCount = 0U;
    m_otaNegotiatedWindowBytes = 0U;
    m_otaLastDisplayedPercent = 255;
    m_otaLastLoggedStep = 255;
    m_otaWriteStageLen = 0;
    m_otaWriteCallCount = 0;
    m_otaWriteBytesFlushed = 0;
    m_otaWriteTotalMs = 0;
    m_otaWriteMaxMs = 0;
    m_otaWriteSlowCount = 0;
    m_otaChunkEnqueueCount = 0;
    m_otaChunkEnqueueDrops = 0;
    m_otaChunkDrainCycles = 0;
    m_otaChunkDrainCount = 0;
    m_otaChunkDrainMaxPerCycle = 0;
    m_otaNotifyCount = 0;
    m_otaNotifyTotalMs = 0;
    m_otaNotifyMaxMs = 0;
    if (stopIdentify) {
        StopOtaIdentify();
    }
}
#endif
#endif

bool BleService::IsOtaInProgress() const {
#if defined(LITCH_BLE_OTA)
    return m_otaTransaction.IsActive();
#else
    return false;
#endif
}

uint8_t BleService::GetOtaProgressPercent() const {
#if defined(LITCH_BLE_OTA)
    return m_otaProgressPercent;
#else
    return 0;
#endif
}

OtaLedPresentation BleService::GetOtaLedPresentation() const {
#if defined(LITCH_BLE_OTA)
    return ProjectOtaLedPresentation(m_otaTransaction.IsActive(),
                                     m_otaSimulationMode,
                                     m_otaPendingReboot,
                                     m_otaProgressPercent);
#else
    return {};
#endif
}

} // namespace modules
