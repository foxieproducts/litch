#include "modules/ble_service.hpp"

#include <ArduinoJson.h>

#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#endif

#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/ble_json_text.hpp"
#include "modules/ble_service_internal.hpp"
#include "modules/control_mutation_service.hpp"

namespace modules {

#if defined(LITCH_BLE)
void BleService::HandleConsoleInputEvent(
    const BleConsoleInputEvent& event) {
    if (!OwnerProductAccess() || !m_bleSettings.OwnsConnectionEpoch(event.connectionEpoch)) {
        return;
    }

    m_lastClientActivityMs = fxe::Millis();
    HandleConsoleInputPacket(event.input, event.len);
}

void BleService::HandleConsoleInputPacket(const char* data, const size_t len) {
    if (!OwnerProductAccess() || m_bleLinkTestInProgress || data == nullptr || len == 0 ||
        std::memchr(data, '\0', len) != nullptr) {
        return;
    }

    if (IsJsonEnvelope(data, len)) {
        StaticJsonDocument<256> doc;
        const DeserializationError err = deserializeJson(doc, data, len);
        if (!err && !HasSafeBleJsonText(doc.as<JsonVariantConst>())) {
            return;
        }
        if (!err) {
            const char* cmd = doc["cmd"] | "";
            if (std::strcmp(cmd, "ping") == 0) {
                return;
            }
            if (cmd[0] == '\0' && doc["h"].is<uint32_t>() && doc["h"].as<uint32_t>() != 0U) {
                // Compact correlated read fits the minimum ATT payload. This
                // main-loop event already passed the connection-epoch guard.
                m_consoleSnapshotReceipt.store(doc["h"].as<uint32_t>(), std::memory_order_release);
                m_consoleStreamSnapshotRequested.store(true, std::memory_order_release);
                return;
            }
            if (std::strcmp(cmd, "home_state") == 0) {
                // Idempotent read request. The normal firmware loop owns the
                // snapshot and publishes it only through the subscribed Home
                // stream; this command changes no game or setting state.
                m_consoleStreamSnapshotRequested.store(
                    true, std::memory_order_release);
                return;
            }
            if (std::strcmp(cmd, "identify_stop") == 0) {
                // Diagnostic compatibility exception: identify changes only
                // bounded BLE-owned transient presentation state. It is not a
                // registered setting or a board-equivalent control action.
                m_identifyStopPending = true;
                m_identifyStartPending = false;
                return;
            }
            if (std::strcmp(cmd, "identify") == 0) {
                const int num = doc["num"] | 1;
                const uint32_t durationMs = doc["duration_ms"] | ble_internal::IDENTIFY_DURATION_MS_DEFAULT;
                const char* colorName = doc["color"] | "";

                m_identifyNumber = ble_internal::ClampIdentifyNumber(num);
                m_identifyDurationMs = ble_internal::ClampIdentifyDurationMs(durationMs);
                ble_internal::ParseIdentifyColor(colorName, m_identifyRed, m_identifyGreen, m_identifyBlue);
                m_identifyStopPending = false;
                m_identifyStartPending = true;
                return;
            }
            if (std::strcmp(cmd, "console") == 0) {
                const char* line = doc["line"] | "";
                if (line[0] != '\0') {
                    EnqueueConsoleInputBytes(line, std::strlen(line));
                    static constexpr char NL = '\n';
                    EnqueueConsoleInputBytes(&NL, 1);
                }
                return;
            }
            if (std::strcmp(cmd, "settings_set") == 0 && m_settings != nullptr) {
                const char* key = doc["k"] | "";
                JsonVariantConst value = doc["v"];
                if (key[0] == '\0' || value.isNull()) {
                    return;
                }

                if (m_controlMutationService == nullptr) {
                    // This deployed envelope has no acknowledgement channel.
                    // Production wires the service before BLE begins, so a
                    // missing service must fail closed instead of silently
                    // editing the settings document through the legacy path.
                    return;
                }

                (void)m_controlMutationService->MutateSetting(key, value);
                return;
            }
        }

        return;
    }

    EnqueueConsoleInputBytes(data, len);
}

void BleService::EnqueueConsoleInputBytes(const char* data, const size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }

    (void)m_consoleInput.Enqueue(data, len);
}

void BleService::FlushConsoleOutputBuffer() {
    if (!OwnerProductAccess() || m_consoleOutputLen == 0 || m_consoleStreamCharacteristic == nullptr ||
        !IsConnected() ||
        !m_consoleStreamSubscribed.load(std::memory_order_acquire) ||
        !m_bleSettings.OwnsConnectionEpoch(m_bleSettings.CurrentConnectionEpoch())) {
        return;
    }

    const uint32_t nowMs = fxe::Millis();
    if ((nowMs - m_lastConsoleNotifyMs) < ble_internal::MIN_CONSOLE_NOTIFY_INTERVAL_MS) {
        return;
    }

    // The local preferred MTU is not the peer's negotiated payload. Early
    // notifications must also survive the minimum ATT MTU of 23 bytes.
    const uint16_t mtu = GetMtu();
    const size_t payload = mtu >= 23U ? mtu - 3U : 20U;
    const size_t limit = payload < ble_internal::MAX_CONSOLE_NOTIFY_CHUNK
                             ? payload : ble_internal::MAX_CONSOLE_NOTIFY_CHUNK;
    const size_t chunk = m_consoleOutputLen < limit ? m_consoleOutputLen : limit;
    // Supplying the bytes and current connection asks NimBLE to enqueue this
    // exact packet. Its no-argument notify only schedules an attribute update
    // and cannot report a failed enqueue or protect successive value changes.
    if (!OwnerProductAccess()) return;
    const bool submitted = m_consoleStreamCharacteristic->notify(
        reinterpret_cast<const uint8_t*>(m_consoleOutputBuffer), chunk, m_connectionHandle);
    // Pace the next attempt from completion, including a rejected submission.
    m_lastConsoleNotifyMs = fxe::Millis();
    if (!submitted) return;

    if (chunk < m_consoleOutputLen) {
        const size_t remaining = m_consoleOutputLen - chunk;
        std::memmove(m_consoleOutputBuffer, m_consoleOutputBuffer + chunk, remaining);
        m_consoleOutputLen = remaining;
    } else {
        m_consoleOutputLen = 0;
    }
}


#endif

} // namespace modules
