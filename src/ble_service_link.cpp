#include <algorithm>

#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#endif

#include "foxie_embedded/elapsed_time.hpp"
#include "logger.hpp"
#include "modules/ble_service.hpp"
#include "modules/ble_service_internal.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "runtime/progress_watchdog.hpp"

namespace modules {
namespace {

constexpr uint32_t MAX_NOTIFY_TEST_MS = 10000U;
constexpr uint32_t NOTIFY_PAUSE_SLICE_MS = 10U;

} // namespace

bool BleService::IsConnected() const {
    return m_connected.load(std::memory_order_acquire);
}

bool BleService::IsBulkTransferActive() const {
#if defined(LITCH_BLE)
    return IsConnected() && OwnerProductAccess() && !m_recoveryOnly &&
        m_connectionSession.IsActivated() &&
        (m_connectionSession.HasBulkReverseObject() ||
#if defined(LITCH_BLE_OTA)
         (m_objectBenchmark.Owns(BenchmarkLifetime()) && m_objectBenchmark.IsActive()) ||
#endif
         (m_litchcraftGame != nullptr && m_litchcraftGame->Litchcraft().BulkTransferActive()));
#else
    return false;
#endif
}

bool BleService::IsAdvertising() const {
#if defined(LITCH_BLE)
    return m_initialized && m_advertising != nullptr &&
           m_advertising->isAdvertising();
#else
    return false;
#endif
}

uint16_t BleService::GetMtu() const {
#if defined(LITCH_BLE)
    if (m_connected.load(std::memory_order_acquire) && m_server != nullptr &&
        m_connectionHandle != ble_internal::INVALID_CONN_HANDLE) {
        const uint16_t negotiated = m_server->getPeerMTU(m_connectionHandle);
        if (negotiated >= 23U) {
            return negotiated;
        }
    }
#endif
    return m_mtu.load(std::memory_order_acquire);
}

#if defined(LITCH_BLE)
void BleService::UpdateFastConnectionPreference() {
    using namespace ble_internal;
    const uint32_t epoch = m_bleSettings.CurrentConnectionEpoch();
    const uint16_t handle = m_connectionHandle;
    if (m_server == nullptr || !IsConnected() || handle == INVALID_CONN_HANDLE ||
        m_ownerNativeEpoch.load(std::memory_order_acquire) != epoch ||
        m_ownerNativeHandle.load(std::memory_order_acquire) != handle ||
        m_ownerQuiescing.load(std::memory_order_acquire)) return;
    if (m_fastConnectionEpoch != epoch) {
        m_fastConnectionEpoch = epoch;
        m_fastConnectionRequestCount = 0U;
        m_fastConnectionUpdatePending = true;
        m_fastConnectionBootstrapHandled = false;
    }
    if (m_fastConnectionRequestCount >= BLE_FAST_CONN_REQUEST_LIMIT) return;
#if defined(LITCH_HW_VARIANT_S3)
    if (m_preferredPhyEpoch != epoch || m_preferredPhyPending) return;
#endif
    const bool bootstrapReady = m_connectionSession.IsActivated() && OwnerProductAccess();
    if (bootstrapReady && !m_fastConnectionBootstrapHandled) {
        m_fastConnectionBootstrapHandled = true;
        m_fastConnectionUpdatePending = true;
    }
    if (!m_fastConnectionUpdatePending ||
        (m_fastConnectionRequestCount == BLE_FAST_CONN_REQUEST_LIMIT - 1U &&
         !bootstrapReady)) return;
    m_fastConnectionUpdatePending = false;
    const uint8_t ordinal = m_fastConnectionRequestCount + 1U;
    const uint16_t interval = BleFastConnectionIntervalForRequest(ordinal);
    if (m_linkObservation.intervalUnits != 0U &&
        m_linkObservation.intervalUnits <= interval) return;
    m_fastConnectionRequestCount = ordinal;
    const uint16_t timeout = BleFastConnectionTimeoutForRequest(ordinal);
    logger::Trace("ble: conn preference request=%u epoch=%lu interval_units=%u timeout_units=%u",
                  static_cast<unsigned>(ordinal), static_cast<unsigned long>(epoch),
                  static_cast<unsigned>(interval), static_cast<unsigned>(timeout));
    m_server->updateConnParams(handle, interval, interval, BLE_FAST_CONN_LATENCY, timeout);
}
#endif

uint8_t BleService::GetConnectionCount() const {
    return m_connectionCount.load(std::memory_order_acquire);
}

const char* BleService::GetPeerAddress() const {
    return m_peerAddress.CStr();
}

void BleService::SetBeforeBlockingDiagnosticCallback(
    const BeforeBlockingDiagnosticCallback callback,
    void* const context) {
    m_beforeBlockingDiagnosticCallback = callback;
    m_beforeBlockingDiagnosticContext = context;
}

void BleService::InvokeBeforeBlockingWorkCallback() {
    if (m_beforeBlockingDiagnosticCallback != nullptr) {
        m_beforeBlockingDiagnosticCallback(
            m_beforeBlockingDiagnosticContext);
    }
}

void BleService::SetBleLinkTestInProgress(const bool enabled) {
#if defined(LITCH_BLE)
    m_bleLinkTestInProgress = enabled;
#else
    (void)enabled;
#endif
}

bool BleService::AreNotifyTestArgumentsValid(const uint16_t packetSize,
                                            const uint16_t packetCount,
                                            const uint16_t delayMs) {
    const uint32_t pauseMs = delayMs == 0U ? 1U : delayMs;
    return packetSize != 0U && packetCount != 0U &&
           packetCount <= MAX_NOTIFY_TEST_MS / pauseMs;
}

bool BleService::RunConsoleNotifyTest(const uint16_t packetSize,
                                      const uint16_t packetCount,
                                      const uint16_t interPacketDelayMs,
                                      BleLinkTestResult& outResult,
                                      const uint32_t run) {
    outResult = BleLinkTestResult();
    outResult.packetSize = packetSize;
    outResult.packetCount = packetCount;
#if defined(LITCH_BLE)
    if (!AreNotifyTestArgumentsValid(packetSize, packetCount,
                                     interPacketDelayMs) ||
        (run != 0U && packetSize < ble_internal::BLE_LINK_TEST_TAG_BYTES) ||
        !IsConnected() || m_consoleStreamCharacteristic == nullptr ||
        m_connectionHandle == ble_internal::INVALID_CONN_HANDLE) {
        return false;
    }

    const uint32_t connectionEpoch = m_bleSettings.CurrentConnectionEpoch();
    const uint16_t connectionHandle = m_connectionHandle;
    const auto elapsed = fxe::ElapsedTime::StartNow();
    const auto connectionIsCurrent = [&]() {
        return IsConnected() &&
               m_connectionHandle == connectionHandle &&
               m_bleSettings.OwnsConnectionEpoch(connectionEpoch);
    };
    const auto finish = [&](const bool completed) {
        outResult.elapsedMs = elapsed.ElapsedMs();
        return completed && outResult.okCount > 0U;
    };
    if (!connectionIsCurrent()) {
        return finish(false);
    }
    InvokeBeforeBlockingWorkCallback();

    uint16_t maxPayload = 20U;
    const uint16_t mtu = GetMtu();
    if (mtu > 3U) {
        maxPayload = static_cast<uint16_t>(mtu - 3U);
    }
    maxPayload = std::min(maxPayload,
                         ble_internal::BLE_LINK_TEST_MAX_PACKET_BYTES);
    const uint16_t effectivePacketSize = std::min(packetSize, maxPayload);
    outResult.packetSize = effectivePacketSize;

    uint8_t payload[ble_internal::BLE_LINK_TEST_MAX_PACKET_BYTES] = {};
    std::fill_n(payload, effectivePacketSize, 0x01U);
    if (run != 0U) {
        // Control-only bytes remain invisible to the ordinary Console parser.
        payload[1U] = 0x1eU;
        payload[2U] = 0x1fU;
        for (uint8_t index = 0U; index < 8U; ++index)
            payload[4U + index] = 0x10U | ((run >> (index * 4U)) & 0x0fU);
    }

    for (uint16_t i = 0U; i < packetCount;) {
        if (!connectionIsCurrent() || elapsed.HasElapsed(MAX_NOTIFY_TEST_MS)) {
            return finish(false);
        }
        if (run != 0U) {
            for (uint8_t index = 0U; index < 4U; ++index)
                payload[12U + index] = 0x10U | ((i >> (index * 4U)) & 0x0fU);
        }
        const bool ok = m_consoleStreamCharacteristic->notify(
            payload, effectivePacketSize, connectionHandle);
        outResult.bytesAttempted += effectivePacketSize;
        if (ok) {
            ++outResult.okCount;
            outResult.bytesSent += effectivePacketSize;
        } else {
            ++outResult.failCount;
        }
        // Tagged runs measure delivered packets; queue pressure retains this
        // ordinal until admission. The existing pause/deadline bounds retries.
        if (ok || run == 0U) ++i;
        // Only a returned SDK operation establishes progress. A stuck notify
        // remains supervised by the existing watchdog.
        runtime::FeedProgressWatchdog();

        uint32_t pauseMs = interPacketDelayMs == 0U ? 1U : interPacketDelayMs;
        while (pauseMs > 0U) {
            const uint32_t elapsedMs = elapsed.ElapsedMs();
            if (!connectionIsCurrent() || elapsedMs >= MAX_NOTIFY_TEST_MS) {
                return finish(false);
            }
            const uint32_t sliceMs = std::min(
                std::min(pauseMs, NOTIFY_PAUSE_SLICE_MS),
                MAX_NOTIFY_TEST_MS - elapsedMs);
            runtime::DelayDiagnosticSlice(sliceMs);
            runtime::FeedProgressWatchdog();
            pauseMs -= sliceMs;
        }
    }
    return finish(connectionIsCurrent() &&
                  elapsed.ElapsedMs() <= MAX_NOTIFY_TEST_MS);
#else
    (void)interPacketDelayMs;
    (void)run;
    return false;
#endif
}

} // namespace modules

#include "modules/ble_service.hpp"

#if defined(LITCH_BLE) && defined(LITCH_BLE_OTA)
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "modules/game_persistence_coordinator.hpp"
#include "modules/ble_json_text.hpp"

namespace modules {

ConnectionObjectBenchmark::Lifetime BleService::BenchmarkLifetime() const {
    return {m_connectionSession.Generation(), m_connectionSession.ConnectionToken(),
            m_connectionSession.ObjectStreamToken(),
            m_connectionSession.ReverseObjectStreamToken()};
}

bool BleService::IsCurrentSharedBenchmarkCancel(const BleEvent& event) const {
    if (event.type != BleEventType::OtaControl || m_recoveryOnly || !OwnerProductAccess() ||
        !IsConnected() || !m_connectionSession.IsActivated() || !m_objectBenchmark.IsActive() ||
        !m_objectBenchmark.Owns(BenchmarkLifetime())) return false;
    const auto& control = event.payload.otaControl;
    if (!m_bleSettings.OwnsConnectionEpoch(control.connectionEpoch) ||
        control.objectCreditToken == 0U ||
        control.objectCreditToken != m_connectionSession.ObjectStreamToken() ||
        control.len == 0U || control.len >= sizeof(control.input) ||
        std::memchr(control.input, '\0', control.len) != nullptr) return false;
    StaticJsonDocument<256U> request;
    if (deserializeJson(request, control.input, control.len) ||
        !HasSafeBleJsonText(request.as<JsonVariantConst>())) return false;
    const char* const cmd = request["cmd"] | "";
    return std::strcmp(cmd, "object_bench_cancel") == 0 &&
        request["q"].is<uint16_t>() && request["q"].as<uint16_t>() != 0U &&
        request["run"].is<uint16_t>() &&
        request["run"].as<uint16_t>() == m_objectBenchmark.Run();
}

bool BleService::BenchmarkAvailable() const {
    return !m_recoveryOnly && OwnerProductAccess() && IsConnected() &&
        m_connectionSession.IsActivated() && !IsOtaInProgress() &&
        !m_objectBenchmark.IsActive() && !m_pendingConnectionControlAction.IsActive() &&
        !m_otaChunkQueue.TryPeekLitchcraftFrameStore() &&
        m_connectionLitchcraftExpected == 0U &&
        m_connectionLitchcraftOutputOffset == 0U &&
        m_connectionFrameStoreOutputOffset == 0U &&
        (m_litchcraftGame == nullptr || !m_litchcraftGame->Litchcraft().IsActive()) &&
        (m_otaStoreBeginBusy == nullptr || !m_otaStoreBeginBusy(m_otaStorePersistenceContext)) &&
        (m_otaStoreSavePending == nullptr || !m_otaStoreSavePending(m_otaStorePersistenceContext));
}

bool BleService::HandleObjectBenchmarkControl(const char* const cmd,
    const JsonObjectConst request, const bool sharedIngress) {
    const bool probe = std::strcmp(cmd, "object_bench_probe") == 0;
    const bool begin = std::strcmp(cmd, "object_bench_begin") == 0;
    const bool cancel = std::strcmp(cmd, "object_bench_cancel") == 0;
    if (!probe && !begin && !cancel) return false;
    if (!sharedIngress || m_recoveryOnly || !OwnerProductAccess() ||
        !m_connectionSession.IsActivated() ||
        !request["q"].is<uint16_t>() || request["q"].as<uint16_t>() == 0U) {
        NotifyOtaStatus("error", "invalid_benchmark");
        return true;
    }
    const auto query = request["q"].as<uint16_t>();
    if (!cancel && !CanRetainConnectionOtaStatus()) return true; // Cancel never waits for output room.
    char response[224U] = {};
    int length = 0;
    if (probe) {
        const auto link = GetLinkObservation();
        if (request.containsKey("detail") &&
            (!request["detail"].is<uint8_t>() || request["detail"].as<uint8_t>() != 1U)) {
            NotifyOtaStatus("error", "invalid_benchmark");
            return true;
        }
        if (request["detail"].as<uint8_t>() == 1U) {
            const auto stats = GetSessionDiagnostics();
            const auto& dl = link.dataLength;
            // Maximum uint16/uint32 widths fit this unchanged 224-byte status owner.
            length = std::snprintf(response, sizeof(response),
                "{\"s\":\"object_bench_detail\",\"q\":%u,\"schema\":1,\"dk\":%u,\"txo\":%u,\"txt\":%u,\"rxo\":%u,\"rxt\":%u,\"pk\":%u,\"tp\":%u,\"rp\":%u,\"na\":%lu,\"nf\":%lu,\"rr\":%lu}",
                query, static_cast<unsigned>(link.dataLengthKnown),
                dl.txOctets, dl.txTimeUs, dl.rxOctets, dl.rxTimeUs,
                static_cast<unsigned>(link.phyKnown), link.txPhy, link.rxPhy,
                static_cast<unsigned long>(stats.notifyAttempts),
                static_cast<unsigned long>(stats.notifyFailures),
                static_cast<unsigned long>(m_connectionSession.SuccessfulReverseObjectReplays()));
        } else {
            length = std::snprintf(response, sizeof(response),
                "{\"s\":\"object_bench_probe\",\"q\":%u,\"schema\":1,\"max_bytes\":300000,\"code\":51,\"pattern\":1,\"mtu\":%u,\"interval\":%u,\"latency\":%u,\"timeout\":%u,\"tx_phy\":%u,\"rx_phy\":%u,\"phy_known\":%u,\"dle_known\":%u}",
                query, link.mtu, link.intervalUnits, link.latency, link.timeoutUnits,
                link.txPhy, link.rxPhy, static_cast<unsigned>(link.phyKnown),
                static_cast<unsigned>(link.dataLengthKnown));
        }
    } else if (!request["run"].is<uint16_t>() || request["run"].as<uint16_t>() == 0U) {
        NotifyOtaStatus("error", "invalid_benchmark");
        return true;
    } else {
        const auto run = request["run"].as<uint16_t>();
        if (cancel) {
            if (run != m_objectBenchmark.Run()) {
                NotifyOtaStatus("error", "benchmark_run");
                return true;
            }
            const bool forward = m_objectBenchmark.IsForward();
            m_objectBenchmark.Cancel();
            if (forward && m_otaSimulationMode) AbortOtaTransfer(true, true);
        } else {
            if (!request["size"].is<uint32_t>() || !request["seed"].is<uint16_t>() ||
                !BenchmarkAvailable() || !m_objectBenchmark.Begin(BenchmarkLifetime(),
                    ConnectionObjectBenchmark::Direction::Reverse, run,
                    request["size"].as<uint32_t>(), request["seed"].as<uint16_t>())) {
                NotifyOtaStatus("error", "busy_or_invalid_benchmark");
                return true;
            }
        }
        length = std::snprintf(response, sizeof(response),
            "{\"s\":\"object_bench_%s\",\"q\":%u,\"run\":%u}",
            cancel ? "cancel" : "begin", query, run);
    }
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(response)) {
        NotifyOtaStatus("error", "benchmark_status");
    } else {
        (void)NotifyOtaJson(response);
    }
    return true;
}

bool BleService::BeginForwardBenchmark(const JsonObjectConst request,
                                       const bool sharedIngress) {
    return sharedIngress && request["bench_schema"].is<uint8_t>() &&
        request["bench_schema"].as<uint8_t>() == 1U &&
        request["run"].is<uint16_t>() && request["seed"].is<uint16_t>() &&
        request["size"].is<uint32_t>() &&
        m_objectBenchmark.Begin(BenchmarkLifetime(),
            ConnectionObjectBenchmark::Direction::Forward,
            request["run"].as<uint16_t>(), request["size"].as<uint32_t>(),
            request["seed"].as<uint16_t>());
}

void BleService::UpdateObjectBenchmarkOutput(uint8_t* const scratch, const size_t capacity) {
    if (!m_objectBenchmark.IsActive()) return;
    if (!m_objectBenchmark.Owns(BenchmarkLifetime()) || !OwnerProductAccess() ||
        !m_connectionSession.IsActivated()) {
        const bool forward = m_objectBenchmark.IsForward();
        m_objectBenchmark.Cancel();
        if (forward && m_otaSimulationMode) AbortOtaTransfer(true, true);
        return;
    }
    if (!m_objectBenchmark.IsReverse()) return;
    const size_t maximum = std::min<size_t>(
        m_connectionSession.ReverseObjectMaximumBody(),
        std::min<size_t>(ConnectionSession::REVERSE_OBJECT_PACKET_BYTES,
            m_connectionSession.PacketMaximum()) - connection_session_codec::HEADER_BYTES);
    if (scratch == nullptr || capacity < maximum) return;
    for (uint8_t count = 0U; count < ConnectionSession::REVERSE_OBJECT_CAPACITY; ++count) {
        size_t size = 0U;
        if (!m_objectBenchmark.PeekReverse(BenchmarkLifetime(), scratch, maximum, size) ||
            !m_connectionSession.PublishReverseObject(ConnectionObjectBenchmark::REVERSE_CODE,
                m_objectBenchmark.Run(), scratch, size, true)) break;
        (void)m_objectBenchmark.AcceptReverse(BenchmarkLifetime(), scratch, size);
    }
}

void BleService::NotifyObjectBenchmarkDone() {
    char response[192U] = {};
    const int length = std::snprintf(response, sizeof(response),
        "{\"st\":\"done\",\"bench\":1,\"run\":%u,\"rx\":%lu,\"total\":%lu,\"crc32\":\"%08lx\",\"pattern\":1}",
        m_objectBenchmark.Run(), static_cast<unsigned long>(m_objectBenchmark.Count()),
        static_cast<unsigned long>(m_objectBenchmark.Count()),
        static_cast<unsigned long>(m_objectBenchmark.Crc()));
    if (length > 0 && static_cast<size_t>(length) < sizeof(response)) (void)NotifyOtaJson(response);
}
} // namespace modules
#endif
