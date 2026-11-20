#include "modules/ble_service.hpp"
#include "modules/ble_json_text.hpp"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

#include "firmware/firmware_version.hpp"
#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/ble_service_internal.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/ota_attempt_proof.hpp"

namespace modules {
#if defined(LITCH_BLE)
void BleService::HandleOtaControlPacket(const char* const data,
                                        const size_t len,
                                        const uint32_t sessionEpoch,
                                        const uint32_t connectionEpoch,
                                        const bool sharedIngress) {
    if (!m_recoveryOnly && OwnerProductAccess() && HandleRuntimeHealthRequest(data, len, connectionEpoch)) {
        return;
    }
    if (!m_bleSettings.OwnsConnectionEpoch(connectionEpoch)) {
        return;
    }
#if defined(LITCH_BLE_OTA)
    const bool beginCandidate =
        IsBleOtaBeginControlCandidate(data, len);
#else
    (void)sessionEpoch;
#endif
    if (m_bleLinkTestInProgress) {
#if defined(LITCH_BLE_OTA)
        if (ResolveBleOtaBeginCandidate(beginCandidate, false, false) ==
            BleOtaBeginCandidateResolution::RejectAndInvalidate) {
            RejectOtaBeginCandidate(sessionEpoch);
        }
#endif
        NotifyOtaStatus("error", "busy_test");
        return;
    }
    if (data == nullptr || len == 0U) {
        return;
    }

#if defined(LITCH_NATIVE)
    // Native64 JSON slots are wider; retain the unchanged S3 256-byte bound.
    StaticJsonDocument<384> doc;
#else
    StaticJsonDocument<256> doc;
#endif
    const DeserializationError err = deserializeJson(doc, data, len);
    if (err || std::memchr(data, '\0', len) != nullptr ||
        !HasSafeBleJsonText(doc.as<JsonVariantConst>())) {
#if defined(LITCH_BLE_OTA)
        if (ResolveBleOtaBeginCandidate(beginCandidate, false, false) ==
            BleOtaBeginCandidateResolution::RejectAndInvalidate) {
            RejectOtaBeginCandidate(sessionEpoch);
        }
#endif
        NotifyOtaStatus("error", "invalid_json");
        return;
    }

    const char* const cmd = doc["cmd"] | "";
#if defined(LITCH_BLE_OTA)
    const bool parsedBegin = std::strcmp(cmd, "ota_begin") == 0 ||
                             std::strcmp(cmd, "ota_sim_begin") == 0;
    if (beginCandidate != parsedBegin &&
        ResolveBleOtaBeginCandidate(beginCandidate, parsedBegin, false) ==
            BleOtaBeginCandidateResolution::RejectAndInvalidate) {
        RejectOtaBeginCandidate(sessionEpoch);
        NotifyOtaStatus("error", "invalid_begin");
        return;
    }
#endif
#if defined(LITCH_BLE_OTA)
    const bool phyOption = doc.containsKey("prefer_phy_2m");
    if (phyOption && (!doc["prefer_phy_2m"].is<bool>() ||
        std::strcmp(cmd, "ota_sim_begin") != 0 || !doc.containsKey("bench_schema"))) {
        if (parsedBegin) RejectOtaBeginCandidate(sessionEpoch);
        NotifyOtaStatus("error", "invalid_benchmark");
        return;
    }
#if !defined(LITCH_HW_VARIANT_S3)
    if (phyOption && doc["prefer_phy_2m"].as<bool>()) {
        if (parsedBegin) RejectOtaBeginCandidate(sessionEpoch);
        NotifyOtaStatus("error", "invalid_benchmark");
        return;
    }
#endif
#endif
    if (std::strcmp(cmd, "info") == 0 ||
        std::strcmp(cmd, "device_info") == 0) {
        NotifyDeviceInfo();
        return;
    }

    if (!m_recoveryOnly && !OwnerProductAccess()) return;

#if !defined(LITCH_BLE_OTA)
    // Object 0x32 still carries the current profile's read-only diagnostics.
    // Updater/link-test mutations have no backend on non-OTA profiles.
    if (sharedIngress && std::strcmp(cmd, "fw_version") != 0 &&
        std::strcmp(cmd, "fw_digest") != 0 &&
        std::strcmp(cmd, "ota_state") != 0) {
        NotifyOtaStatus("error", "no_ota");
        return;
    }
#endif

#if defined(LITCH_BLE_OTA)
    if (HandleObjectBenchmarkControl(cmd, doc.as<JsonObjectConst>(), sharedIngress)) return;
#else
    (void)sharedIngress;
#endif

    if (std::strcmp(cmd, "fw_version") == 0) {
        logger::Trace("BLE OTA version request");
        NotifyOtaStatus("version", firmware::FIRMWARE_VERSION);
        return;
    }

    if (std::strcmp(cmd, "fw_digest") == 0) {
        if (IsOtaInProgress()) {
            NotifyOtaStatus("error", "busy_ota");
            return;
        }
        RequestFirmwareDigestNotification();
        return;
    }

    if (std::strcmp(cmd, "ota_state") == 0) {
        if (doc.containsKey("proof")) {
#if defined(LITCH_BLE_OTA)
            if (!m_connectionSession.IsActivated() ||
                !doc["proof"].is<uint32_t>() || doc["proof"].as<uint32_t>() != 1U ||
                !doc["q"].is<uint32_t>() || doc["q"].as<uint32_t>() == 0U) {
                NotifyOtaStatus("error", "invalid_proof");
                return;
            }
            char payload[224] = {};
            static_assert(sizeof(payload) == sizeof(m_connectionOtaStatusPending));
            if (FormatOtaAttemptProof(m_otaPlatformAdapter,
                    {doc["q"].as<uint32_t>(), m_connectionSessionBootNonce,
                     m_otaRuntimeAccepted}, payload, sizeof(payload))) {
                NotifyOtaJson(payload);
            }
#else
            NotifyOtaStatus("error", "unavailable");
#endif
            return;
        }
        NotifyOtaJson(m_otaRuntimeAccepted ? "{\"ov\":\"v\"}" : "{\"ov\":\"p\"}");
        return;
    }

    if (m_recoveryOnly && std::strncmp(cmd, "ota_", 4U) != 0) {
        NotifyOtaStatus("error", "recovery_only");
        return;
    }

    if (std::strcmp(cmd, "notify_test") == 0) {
        if (IsOtaInProgress()) {
            NotifyOtaStatus("error", "busy_ota");
            return;
        }

        if (!doc["size"].is<uint16_t>() || !doc["count"].is<uint16_t>() ||
            (!doc["delay_ms"].isNull() && !doc["delay_ms"].is<uint16_t>()) ||
            (doc.containsKey("run") &&
             (!doc["run"].is<uint32_t>() || doc["run"].as<uint32_t>() == 0U))) {
            NotifyOtaStatus("error", "invalid_test_args");
            return;
        }
        const uint16_t packetSize = doc["size"] | 0U;
        const uint16_t packetCount = doc["count"] | 0U;
        const uint16_t interPacketDelayMs = doc["delay_ms"] | 0U;
        const uint32_t run = doc["run"] | 0U;
        if (!AreNotifyTestArgumentsValid(packetSize, packetCount,
                                         interPacketDelayMs) ||
            (run != 0U && packetSize < ble_internal::BLE_LINK_TEST_TAG_BYTES)) {
            NotifyOtaStatus("error", "invalid_test_args");
            return;
        }

        struct LinkTestGuard {
            explicit LinkTestGuard(BleService& bleService) : m_bleService(bleService) {
                m_bleService.SetBleLinkTestInProgress(true);
            }
            ~LinkTestGuard() { m_bleService.SetBleLinkTestInProgress(false); }

          private:
            BleService& m_bleService;
        } guard(*this);

        BleLinkTestResult result;
        const bool ran = RunConsoleNotifyTest(packetSize, packetCount, interPacketDelayMs, result, run);
        if (!m_bleSettings.OwnsConnectionEpoch(connectionEpoch) || !IsConnected()) {
            return;
        }
        const uint32_t elapsedMs = (result.elapsedMs == 0U) ? 1U : result.elapsedMs;
        const uint32_t bytesPerSecond = static_cast<uint32_t>(
            (static_cast<uint64_t>(result.bytesSent) * 1000U) / elapsedMs);
        if (!ran) {
            NotifyOtaStatus("error", "notify_test_failed");
            return;
        }

        char detail[96] = {};
        const int detailLength = std::snprintf(detail,
                      sizeof(detail),
                      "size=%u ok=%u fail=%u elapsed_ms=%lu rate_Bps=%lu",
                      static_cast<unsigned>(result.packetSize),
                      static_cast<unsigned>(result.okCount),
                      static_cast<unsigned>(result.failCount),
                      static_cast<unsigned long>(elapsedMs),
                      static_cast<unsigned long>(bytesPerSecond));
        if (run != 0U && detailLength > 0 && size_t(detailLength) < sizeof(detail)) {
            std::snprintf(detail + detailLength, sizeof(detail) - size_t(detailLength),
                          " run=%lu count=%u", static_cast<unsigned long>(run),
                          static_cast<unsigned>(packetCount));
        }
        NotifyOtaStatus("bench_notify", detail);
        return;
    }

#if defined(LITCH_BLE_OTA)
    if (std::strcmp(cmd, "ota_probe") == 0) {
        const char* const mode = doc["mode"] | "flash";
        const uint32_t size = doc["size"] | 0U;
        m_otaStrictProfileArmed = true;
        NotifyOtaProbe(mode, size);
        return;
    }

    const bool simulatedBegin = std::strcmp(cmd, "ota_sim_begin") == 0;
    if (simulatedBegin || std::strcmp(cmd, "ota_begin") == 0) {
        if (m_objectBenchmark.IsActive()) {
            RejectOtaBeginCandidate(sessionEpoch);
            NotifyOtaStatus("error", "busy_benchmark");
            return;
        }
        const bool benchmarkRequested = doc.containsKey("bench_schema");
        if (benchmarkRequested && (!simulatedBegin || !sharedIngress ||
            !doc["bench_schema"].is<uint8_t>() || doc["bench_schema"].as<uint8_t>() != 1U ||
            !doc["run"].is<uint16_t>() || !doc["seed"].is<uint16_t>() ||
            !doc["size"].is<uint32_t>() || !BenchmarkAvailable())) {
            RejectOtaBeginCandidate(sessionEpoch);
            NotifyOtaStatus("error", "invalid_benchmark");
            return;
        }
        if (!simulatedBegin && m_otaPlatformAdapter == nullptr) {
            RejectOtaBeginCandidate(sessionEpoch);
            NotifyOtaStatus("error", "unavailable");
            return;
        }
        if (!m_recoveryOnly && m_otaStoreBeginBusy != nullptr &&
            m_otaStoreBeginBusy(m_otaStorePersistenceContext)) {
            RejectOtaBeginCandidate(sessionEpoch);
            NotifyOtaStatus("error", "busy_store_clear");
            return;
        }
        if (m_otaPendingReboot) {
            RejectOtaBeginCandidate(sessionEpoch);
            NotifyOtaStatus("error", "reboot_pending");
            return;
        }
        if (m_firmwareDigestProvider.State() == FirmwareDigestState::Pending) {
            RejectOtaBeginCandidate(sessionEpoch);
            NotifyOtaStatus("error", "busy_digest");
            return;
        }
        const uint32_t size = doc["size"] | 0U;
        if (size == 0U) {
            logger::Warn("BLE OTA begin rejected: missing size");
            RejectOtaBeginCandidate(sessionEpoch);
            NotifyOtaStatus("error", "missing_size");
            return;
        }

        InvokeBeforeBlockingWorkCallback();
        logger::Trace("BLE OTA %sbegin requested: size=%lu",
                     simulatedBegin ? "sim " : "",
                     static_cast<unsigned long>(size));
        if (m_otaTransaction.IsActive()) {
            logger::Warn("BLE OTA begin while transfer active; aborting prior session");
            AbortOtaTransfer(true, false);
        }

        m_otaSimulationMode = simulatedBegin;
        m_otaWriteStageLen = 0U;
        m_otaWriteCallCount = 0U;
        m_otaWriteBytesFlushed = 0U;
        m_otaWriteTotalMs = 0U;
        m_otaWriteMaxMs = 0U;
        m_otaWriteSlowCount = 0U;

        const uint16_t maxChunkBytes = GetOtaMaxChunkBytes();
        const uint16_t requestedChunkBytes =
            doc["chunk"] | static_cast<uint16_t>(GetOtaRecommendedChunkBytes());
        m_otaNegotiatedChunkBytes =
            (requestedChunkBytes > maxChunkBytes) ? maxChunkBytes : requestedChunkBytes;
        if (m_otaNegotiatedChunkBytes < 20U) {
            m_otaNegotiatedChunkBytes = 20U;
        }
        static_assert(BleOtaChunkQueue::CAPACITY >= BLE_OTA_MAX_BURST_COUNT,
                      "OTA burst bound must fit the shared chunk queue");
        const uint32_t requestedBurstCount =
            doc["burst"] | static_cast<uint32_t>(GetOtaRecommendedBurstCount());
        m_otaNegotiatedBurstCount =
            ClampBleOtaBurstCount(requestedBurstCount);
        const uint32_t requestedWindowBytes = doc["window"] | GetOtaRecommendedWindowBytes();
        m_otaNegotiatedWindowBytes =
            ClampBleOtaWindowBytes(
                requestedWindowBytes, m_otaNegotiatedChunkBytes);

        const BleOtaTransactionResult beginResult = m_otaTransaction.Begin(
            BleOtaBeginRequest{
                size,
                m_otaNegotiatedChunkBytes,
                sessionEpoch,
                fxe::Millis(),
                m_otaStrictProfileArmed,
            },
            GetOtaTransactionOps());
        if (beginResult != BleOtaTransactionResult::Accepted) {
            const char* detail = "begin_failed";
            if (beginResult == BleOtaTransactionResult::ImageTooLarge) {
                detail = "image_too_large";
            } else if (beginResult == BleOtaTransactionResult::InvalidSize) {
                detail = "missing_size";
            }
            logger::Error("BLE OTA begin rejected: size=%lu reason=%s",
                          static_cast<unsigned long>(size), detail);
            AbortOtaTransfer(true, true, sessionEpoch);
            NotifyOtaStatus("error", detail);
            return;
        }

        if (benchmarkRequested && !BeginForwardBenchmark(doc.as<JsonObjectConst>(), sharedIngress)) {
            AbortOtaTransfer(true, true, sessionEpoch);
            NotifyOtaStatus("error", "invalid_benchmark");
            return;
        }
        m_otaExpectedSize = m_otaTransaction.ExpectedSize();
        m_otaBytesReceived = 0U;
        m_otaProgressPercent = 0U;
        m_otaLastDisplayedPercent = 255U;
        m_otaLastLoggedStep = 255U;
        m_otaInProgress = true;
        m_lastOtaStatusMs = 0U;

        logger::Info("BLE OTA %sbegin accepted", m_otaSimulationMode ? "sim " : "");
        NotifyOtaBeginAccepted();
        (void)HandleOtaQueueFailure();
        return;
    }

    if (std::strcmp(cmd, "ota_end") == 0) {
        if (m_otaTransaction.IsComplete()) {
            logger::Trace("BLE OTA duplicate end acknowledged");
            NotifyOtaStatus("done");
            return;
        }
        if (!m_otaTransaction.IsActive()) {
            logger::Warn("BLE OTA end rejected: not in progress");
            NotifyOtaStatus("error", "not_in_progress");
            return;
        }

        logger::Trace("BLE OTA end requested: rx=%lu total=%lu",
                     static_cast<unsigned long>(m_otaBytesReceived),
                     static_cast<unsigned long>(m_otaExpectedSize));
        const bool hasSize = doc.containsKey("size");
        const uint32_t claimedSize = doc["size"] | 0U;
        const char* claimedCrc = nullptr;
        if (doc["crc32"].is<const char*>()) {
            claimedCrc = doc["crc32"].as<const char*>();
        }
        const BleOtaEndRequest endRequest{hasSize, claimedSize, claimedCrc};
        auto endResult = m_otaTransaction.ValidateEnd(endRequest);
        if (endResult == BleOtaTransactionResult::Accepted) {
            if (!m_recoveryOnly &&
                DeferOtaEndForPersistence(data, len, sessionEpoch, connectionEpoch)) return;
            if (!m_recoveryOnly && !m_otaSimulationMode && m_litchcraftGame != nullptr) {
                m_litchcraftGame->Litchcraft().SetMutationsPaused(true);
            }
            endResult = m_otaTransaction.End(endRequest, GetOtaTransactionOps());
        }
        if (endResult != BleOtaTransactionResult::Accepted) {
            const char* detail = "end_failed";
            if (endResult == BleOtaTransactionResult::MissingEndSize) {
                detail = "missing_size";
            } else if (endResult == BleOtaTransactionResult::EndSizeMismatch ||
                       endResult == BleOtaTransactionResult::Incomplete) {
                detail = "size_mismatch";
            } else if (endResult == BleOtaTransactionResult::MissingCrc) {
                detail = "missing_crc";
            } else if (endResult == BleOtaTransactionResult::MalformedCrc) {
                detail = "invalid_crc";
            } else if (endResult == BleOtaTransactionResult::ChecksumMismatch) {
                detail = "checksum_mismatch";
            } else if (endResult == BleOtaTransactionResult::FlushFailed) {
                detail = "write_failed";
            }
            logger::Warn("BLE OTA verification failed: %s; update aborted", detail);
            LogOtaWriteMetrics(detail);
            AbortOtaTransfer(true, true, sessionEpoch);
            NotifyOtaStatus("error", detail);
            return;
        }

        const bool benchmarkDone = m_objectBenchmark.IsForward();
        if (benchmarkDone && !m_objectBenchmark.FinishForward(
                BenchmarkLifetime(), m_otaTransaction.ComputedCrc32())) {
            AbortOtaTransfer(true, true, sessionEpoch);
            NotifyOtaStatus("error", "benchmark_failed");
            return;
        }
        const bool simulatedTransfer = m_otaSimulationMode;
        m_otaInProgress = false;
        m_otaSimulationMode = false;
        m_otaPendingReboot = !simulatedTransfer;
        m_otaProgressPercent = 100U;
        if (!m_otaPendingReboot) {
            logger::Info("BLE OTA simulation done: rx=%lu total=%lu",
                         static_cast<unsigned long>(m_otaBytesReceived),
                         static_cast<unsigned long>(m_otaExpectedSize));
        } else {
            LogOtaWriteMetrics("done");
            logger::Info("BLE OTA done: rx=%lu total=%lu",
                         static_cast<unsigned long>(m_otaBytesReceived),
                         static_cast<unsigned long>(m_otaExpectedSize));
        }
        m_otaRebootGraceArmed = false;
        m_otaRebootDisconnectAtMs = 0U;
        m_otaRebootDisconnectIssued = false;
        if (benchmarkDone) NotifyObjectBenchmarkDone();
        else NotifyOtaStatus("done");
        if (m_otaPendingReboot && !m_connectionSession.IsActivated()) {
            m_otaRebootArmedAtMs = fxe::Millis();
            m_otaRebootGraceArmed = true;
        }
        return;
    }

    if (std::strcmp(cmd, "ota_abort") == 0) {
        if (m_otaPendingReboot) {
            logger::Trace("BLE OTA abort ignored after completed finalization");
            NotifyOtaStatus("done");
            return;
        }
        logger::Warn("BLE OTA abort requested: rx=%lu total=%lu in_progress=%u",
                     static_cast<unsigned long>(m_otaBytesReceived),
                     static_cast<unsigned long>(m_otaExpectedSize),
                     m_otaInProgress ? 1U : 0U);
        AbortOtaTransfer(true, true, sessionEpoch);
        NotifyOtaStatus("aborted");
        return;
    }
#else
    if (std::strncmp(cmd, "ota_", 4U) == 0) {
        logger::Warn("BLE update command rejected: hardware has no OTA partition");
        NotifyOtaStatus("error", "no_ota");
        return;
    }
#endif

    logger::Warn("BLE OTA unknown control command: %s", cmd);
    NotifyOtaStatus("error", "unknown_cmd");
}
#endif

} // namespace modules
