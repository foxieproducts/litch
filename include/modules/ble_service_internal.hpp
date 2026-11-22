#pragma once

#include <array>
#include <cstring>

#include "modules/ble_service.hpp"

namespace modules::ble_internal {

#if defined(LITCH_BLE)
inline constexpr char BLE_ENABLED_KEY[] = "BLE_ENABLED";
inline constexpr char BLE_DEVICE_NAME_KEY[] = "BLE_DEVICE_NAME";
inline constexpr char BASE_SERVICE_UUID[] = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0100";
inline constexpr char COMMAND_CHARACTERISTIC_UUID[] = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0101";
inline constexpr char CONSOLE_STREAM_CHARACTERISTIC_UUID[] = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0102";
inline constexpr char OTA_CONTROL_CHARACTERISTIC_UUID[] = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0104";
inline constexpr char LITCHCRAFT_CHARACTERISTIC_UUID[] = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0106";
inline constexpr char CONNECTION_SESSION_INGRESS_CHARACTERISTIC_UUID[] =
    "6f2a20d1-89df-4ac6-a9f2-53f6f7db0108";
inline constexpr char CONNECTION_SESSION_EGRESS_CHARACTERISTIC_UUID[] =
    "6f2a20d1-89df-4ac6-a9f2-53f6f7db0109";
inline constexpr char OTA_DATA_CHARACTERISTIC_UUID[] = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0105";
inline constexpr char SETTINGS_SERVICE_UUID[] = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0101";
inline constexpr char FACTORY_RADIO_SERVICE_UUID[] = "f17c";
inline constexpr char FACTORY_RADIO_CHARACTERISTIC_UUID[] = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0201";

inline constexpr size_t MAX_CONSOLE_EVENT_BYTES = sizeof(BleConsoleInputEvent::input) - 1U;
inline constexpr size_t MAX_OTA_CONTROL_EVENT_BYTES = sizeof(BleOtaControlEvent::input) - 1U;
inline constexpr size_t MAX_OTA_CHUNK_BYTES = 512U;
inline constexpr size_t MAX_CONSOLE_NOTIFY_CHUNK = 244;
inline constexpr uint32_t MIN_CONSOLE_NOTIFY_INTERVAL_MS = 5;
inline constexpr uint32_t MIN_OTA_STATUS_INTERVAL_MS = 100;
inline constexpr uint32_t OTA_INGRESS_DRAIN_SLICE_MS = 20U;
// Drain one complete callback burst and leave room for the first bounded
// storage slice in the same cooperative pass. The time budget remains the
// primary guard when a slow domain operation refills the queue.
inline constexpr uint32_t OTA_INGRESS_DRAIN_MAX_ITEMS =
    BleEventQueue::CAPACITY + 32U;
inline constexpr uint32_t OTA_STATUS_HEARTBEAT_INTERVAL_MS = 250;
inline constexpr uint16_t BLE_PREFERRED_MTU = 517;
inline constexpr int8_t BLE_TX_POWER_DBM = 9;
inline constexpr uint16_t BLE_FAST_CONN_INTERVAL_MIN_UNITS = 6;
inline constexpr uint16_t BLE_FAST_CONN_INTERVAL_MAX_UNITS = 6;
inline constexpr uint16_t BLE_APPLE_FAST_CONN_INTERVAL_UNITS = 12;
inline constexpr uint8_t BLE_FAST_CONN_REQUEST_LIMIT = 3U;
inline constexpr uint16_t BLE_FAST_CONN_LATENCY = 0;
inline constexpr uint16_t BLE_FAST_CONN_TIMEOUT_UNITS = 200;
inline constexpr uint16_t BLE_APPLE_FAST_CONN_TIMEOUT_UNITS = 600;
inline constexpr uint16_t BLE_FAST_DATA_LEN_OCTETS = 251;
inline constexpr uint16_t BLE_MAX_ATT_PAYLOAD_BYTES = 512;

// Android accepts the 7.5 ms low-latency request. Apple requires at least
// 15 ms for a non-HID accessory, so the last bounded retry must be compliant
// instead of repeating a request that iOS will reject and leave at 30 ms.
inline constexpr uint16_t BleFastConnectionIntervalForRequest(
    const uint8_t requestOrdinal) {
    return requestOrdinal >= BLE_FAST_CONN_REQUEST_LIMIT
        ? BLE_APPLE_FAST_CONN_INTERVAL_UNITS
        : BLE_FAST_CONN_INTERVAL_MAX_UNITS;
}
inline constexpr uint16_t BleFastConnectionTimeoutForRequest(
    const uint8_t requestOrdinal) {
    return requestOrdinal >= BLE_FAST_CONN_REQUEST_LIMIT
        ? BLE_APPLE_FAST_CONN_TIMEOUT_UNITS
        : BLE_FAST_CONN_TIMEOUT_UNITS;
}
inline constexpr size_t OTA_CONTROL_CHARACTERISTIC_MAX_LEN = 256;
inline constexpr uint16_t INVALID_CONN_HANDLE = 0xFFFF;
inline constexpr uint32_t STALE_SESSION_DISCONNECT_GRACE_MS = 12000;
inline constexpr uint32_t STALE_SESSION_IDLE_GRACE_MS = 5000;
inline constexpr uint32_t IDENTIFY_DURATION_MS_DEFAULT = 3000;
inline constexpr uint32_t IDENTIFY_DURATION_MS_MIN = 250;
inline constexpr uint32_t IDENTIFY_DURATION_MS_MAX = 60000;
inline constexpr uint16_t BLE_LINK_TEST_MAX_PACKET_BYTES = BLE_MAX_ATT_PAYLOAD_BYTES;
inline constexpr uint16_t BLE_LINK_TEST_TAG_BYTES = 16U;
inline constexpr uint32_t OTA_CRC32_INITIAL = 0xFFFFFFFFU;
inline constexpr uint32_t OTA_CRC32_FINAL_XOR = 0xFFFFFFFFU;

inline constexpr uint32_t LITCHCRAFT_PLAYBACK_STATE_INTERVAL_MS = 60000U;

inline bool LitchcraftStateNeedsPublication(
    const bool previousValid,
    const std::array<uint8_t, 10U>& previous,
    const std::array<uint8_t, 10U>& current,
    const uint32_t elapsedSincePublicationMs = 0U) {
    if (!previousValid) return true;
    for (uint8_t index = 1U; index < current.size(); ++index) {
        // Playing frame time advances naturally; paused time changes only
        // when a semantic seek/reset replaces its anchor.
        if (index != 2U && index != 3U && current[index] != previous[index]) {
            return true;
        }
    }
    // While paused, selecting a frame is itself semantic state. During
    // playback it advances naturally and must not become notification traffic.
    if ((current[4U] & 2U) == 0U || (current[4U] & 16U) != 0U) {
        return current[0U] != previous[0U] || current[2U] != previous[2U] ||
            current[3U] != previous[3U];
    }
    return (current[4U] & 1U) != 0U &&
        elapsedSincePublicationMs >= LITCHCRAFT_PLAYBACK_STATE_INTERVAL_MS;
}

extern BleService* g_bleService;

inline uint8_t ClampIdentifyNumber(const int num) {
    if (num < 0) {
        return 0;
    }
    if (num > 99) {
        return 99;
    }
    return static_cast<uint8_t>(num);
}

inline uint32_t ClampIdentifyDurationMs(const uint32_t durationMs) {
    if (durationMs < IDENTIFY_DURATION_MS_MIN) {
        return IDENTIFY_DURATION_MS_MIN;
    }
    if (durationMs > IDENTIFY_DURATION_MS_MAX) {
        return IDENTIFY_DURATION_MS_MAX;
    }
    return durationMs;
}

inline void ParseIdentifyColor(const char* colorName,
                               uint8_t& outRed,
                               uint8_t& outGreen,
                               uint8_t& outBlue) {
    if (colorName == nullptr || colorName[0] == '\0') {
        outRed = 255;
        outGreen = 255;
        outBlue = 255;
        return;
    }

    if (std::strcmp(colorName, "blue") == 0) {
        outRed = 0;
        outGreen = 128;
        outBlue = 255;
        return;
    }
    if (std::strcmp(colorName, "red") == 0) {
        outRed = 255;
        outGreen = 0;
        outBlue = 0;
        return;
    }
    if (std::strcmp(colorName, "green") == 0) {
        outRed = 0;
        outGreen = 255;
        outBlue = 0;
        return;
    }
    if (std::strcmp(colorName, "orange") == 0) {
        outRed = 255;
        outGreen = 96;
        outBlue = 0;
        return;
    }
    if (std::strcmp(colorName, "cyan") == 0) {
        outRed = 0;
        outGreen = 255;
        outBlue = 255;
        return;
    }
    if (std::strcmp(colorName, "magenta") == 0) {
        outRed = 255;
        outGreen = 0;
        outBlue = 255;
        return;
    }
    if (std::strcmp(colorName, "yellow") == 0) {
        outRed = 255;
        outGreen = 255;
        outBlue = 0;
        return;
    }

    outRed = 255;
    outGreen = 255;
    outBlue = 255;
}
#endif

} // namespace modules::ble_internal
