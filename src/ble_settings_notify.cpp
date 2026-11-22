#include "modules/ble_settings.hpp"

#if defined(LITCH_BLE)

#include <ArduinoJson.h>

#include <cstddef>
#include <cstring>

#include <NimBLEDevice.h>

#include "logger.hpp"
#include "modules/ble_settings_async.hpp"
#include "modules/settings_registry.hpp"
#include "modules/ble_settings_value_codec.hpp"
#include "modules/control_mutation_service.hpp"

namespace modules {

namespace {

constexpr size_t SETTINGS_NOTIFY_JSON_CAPACITY = 128;
constexpr size_t SETTINGS_NOTIFY_BUFFER_CAPACITY = 128;

bool EncodeJson(const JsonDocument& doc, char* outBuffer, const size_t outCapacity, size_t& outLen) {
    outLen = serializeJson(doc, outBuffer, outCapacity);
    return outLen > 0U && outLen < outCapacity;
}

} // namespace

void BleSettings::BuildInitialSnapshot(const SettingsStore& settings) {
    size_t descriptorCount = 0;
    const fxe::SettingDescriptor* descriptors = modules::settings_registry::EnumerateAll(descriptorCount);
    size_t snapshotIndex = 0;

    for (size_t i = 0; descriptors != nullptr && i < descriptorCount; ++i) {
        const fxe::SettingDescriptor& descriptor = descriptors[i];
        if (descriptor.IsRetired() && descriptor.IsVisibleToApp() &&
            !descriptor.IsInternal()) {
            ++snapshotIndex;
            continue;
        }
        if (!modules::ble_settings_value_codec::IsAppVisible(descriptor)) {
            continue;
        }

        if (snapshotIndex >= SNAPSHOT_CAPACITY) {
            logger::Warn("ble: settings snapshot capacity reached");
            break;
        }

        SnapshotEntry& entry = m_snapshot[snapshotIndex];
        if (!modules::ble_settings_value_codec::SerializeCurrentValueForSnapshot(
                settings, descriptor, entry.value, sizeof(entry.value))) {
            entry.value[0] = '\0';
        }
        entry.used = true;
        ++snapshotIndex;
    }
}

bool BleSettings::NotifySettingChanged(const SettingsStore& settings, const fxe::SettingDescriptor& descriptor) {
    const uint32_t connectionEpoch = CurrentConnectionEpoch();
    if (m_readWriteCharacteristic == nullptr ||
        !OwnsConnectionEpoch(connectionEpoch)) {
        return false;
    }

    const size_t maxAsyncPayloadLen = ble_settings_async::GetNotificationPayloadLimit(m_responseConnectionHandle.load(std::memory_order_acquire));
    StaticJsonDocument<SETTINGS_NOTIFY_JSON_CAPACITY> doc;
    uint16_t settingIndex = 0;
    if (!modules::ble_settings_value_codec::TryResolveVisibleSettingIndex(descriptor.key, settingIndex)) {
        return false;
    }

    JsonArray compact = doc.to<JsonArray>();
    compact.add(settingIndex);
    switch (descriptor.type) {
        case fxe::SettingType::UInt32:
            compact.add(modules::settings_registry::ReadCurrentUInt32(
                settings, descriptor));
            break;
        case fxe::SettingType::Bool: {
            const bool defaultBool = descriptor.defaultUint != 0U;
            compact.add(settings.ReadValue(descriptor.key, defaultBool));
            break;
        }
        case fxe::SettingType::String:
        case fxe::SettingType::Enum: {
            const char* defaultStr = (descriptor.defaultStr != nullptr) ? descriptor.defaultStr : "";
            const char* value = settings.ReadValue(descriptor.key, defaultStr);
            compact.add((value != nullptr) ? value : "");
            break;
        }
    }

    char payload[SETTINGS_NOTIFY_BUFFER_CAPACITY] = {};
    size_t payloadLen = 0;
    if (!EncodeJson(doc, payload, sizeof(payload), payloadLen)) {
        logger::Warn("ble: notify payload too large for key=%s", descriptor.key);
        return false;
    }

    if (payloadLen > maxAsyncPayloadLen) {
        logger::Trace("ble: settings notify refresh fallback key=%s payload_len=%lu max_async_len=%lu",
                      descriptor.key,
                      static_cast<unsigned long>(payloadLen),
                      static_cast<unsigned long>(maxAsyncPayloadLen));
        doc.clear();
        JsonObject refreshOnly = doc.to<JsonObject>();
        refreshOnly["i"] = settingIndex;
        refreshOnly["r"] = 1;
        if (!EncodeJson(doc, payload, sizeof(payload), payloadLen) || payloadLen > maxAsyncPayloadLen) {
            logger::Warn("ble: notify refresh payload too large for key=%s", descriptor.key);
            return false;
        }
    }

    const uint16_t subscriptionValue =
        m_subscriptionValue.load(std::memory_order_acquire);
    if ((subscriptionValue & 0x03U) == 0U) {
        return false;
    }

    if (!m_productAccess.load(std::memory_order_acquire)) return false;
    const bool accepted = SendAdmittedResponse(
        subscriptionValue, payload, payloadLen,
        true);
    return accepted && OwnsConnectionEpoch(connectionEpoch);
}

bool BleSettings::UpdateChangeNotifications(const SettingsStore& settings, const bool connected) {
    if (!m_productAccess.load(std::memory_order_acquire)) return false;
    const uint32_t connectionEpoch = CurrentConnectionEpoch();
    if (!OwnsConnectionEpoch(connectionEpoch)) {
        return false;
    }
    const bool notifyEnabled =
        (m_subscriptionValue.load(std::memory_order_acquire) & 0x03U) != 0U;
    const uint32_t revision = settings.GetRevision();

    if (connected && notifyEnabled && m_readWriteCharacteristic != nullptr &&
        m_controlMutationService != nullptr) {
        for (size_t index = 0U; index < m_trackedControlRequestCount;) {
            ControlMutationOutcome completion;
            if (!m_controlMutationService->TryPeekCompletion(
                    m_trackedControlRequestIds[index], completion)) {
                ++index;
                continue;
            }
            if (!SendControlOutcome(completion)) {
                return false;
            }
            (void)m_controlMutationService->AcceptCompletion(
                completion.requestId);
            RemoveTrackedControlRequestAt(index);
        }
    }

    if (!connected || (revision == m_lastNotifiedRevision &&
        (!notifyEnabled || m_pendingChangedIndices == 0U))) {
        return false;
    }

    bool changed = false;
    size_t descriptorCount = 0;
    const fxe::SettingDescriptor* descriptors = modules::settings_registry::EnumerateAll(descriptorCount);
    size_t snapshotIndex = 0U;

    for (size_t i = 0; descriptors != nullptr && i < descriptorCount; ++i) {
        const fxe::SettingDescriptor& descriptor = descriptors[i];
        if (descriptor.IsRetired() && descriptor.IsVisibleToApp() &&
            !descriptor.IsInternal()) {
            ++snapshotIndex;
            continue;
        }
        if (!modules::ble_settings_value_codec::IsAppVisible(descriptor)) {
            continue;
        }

        if (snapshotIndex >= SNAPSHOT_CAPACITY) {
            logger::Warn("ble: settings notify snapshot full");
            break;
        }
        SnapshotEntry& entry = m_snapshot[snapshotIndex++];

        char currentValue[sizeof(entry.value)] = {};
        if (!modules::ble_settings_value_codec::SerializeCurrentValueForSnapshot(
                settings, descriptor, currentValue, sizeof(currentValue))) {
            continue;
        }

        const uint64_t bit = uint64_t{1} << (snapshotIndex - 1U);
        if (!entry.used || std::strcmp(entry.value, currentValue) != 0) {
            changed = true;
            m_pendingChangedIndices |= bit;
            std::memcpy(entry.value, currentValue, sizeof(entry.value));
            entry.value[sizeof(entry.value) - 1U] = '\0';
            entry.used = true;
        }
        if ((m_pendingChangedIndices & bit) != 0U && notifyEnabled &&
            m_readWriteCharacteristic != nullptr) {
            if (NotifySettingChanged(settings, descriptor)) {
                m_pendingChangedIndices &= ~bit;
            }
        }
    }

    m_lastNotifiedRevision = revision;
    return changed;
}

} // namespace modules

#endif
