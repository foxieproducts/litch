#include "modules/ble_settings.hpp"

#if defined(LITCH_BLE)

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "logger.hpp"
#include "modules/ble_json_text.hpp"
#include "modules/ble_control_protocol.hpp"
#include "modules/ble_settings_async.hpp"
#include "modules/ble_settings_value_codec.hpp"
#include "modules/settings_registry.hpp"

namespace modules {
namespace {

constexpr size_t SETTINGS_RW_PARSE_JSON_CAPACITY = 256;
constexpr size_t SETTINGS_RW_RESPONSE_JSON_CAPACITY = 256;
constexpr size_t SETTINGS_RW_RESPONSE_BUFFER_CAPACITY = 256;
constexpr char CONTROL_EVENT_KEY[] = "@control";

bool EncodeJson(const JsonDocument& doc,
                char* outBuffer,
                const size_t outCapacity,
                size_t& outLen) {
    outLen = serializeJson(doc, outBuffer, outCapacity);
    return outLen > 0U && outLen < outCapacity;
}

bool TryReadRequestId(const JsonVariantConst root, uint16_t& outRequestId) {
    outRequestId = 0U;
    if (!root.containsKey("i")) {
        return true;
    }
    if (!root["i"].is<uint32_t>()) {
        return false;
    }
    const uint32_t value = root["i"].as<uint32_t>();
    if (value == 0U || value > UINT16_MAX) {
        return false;
    }
    outRequestId = static_cast<uint16_t>(value);
    return true;
}

uint16_t ResolveControlReplyRequestId(ControlMutationService* const service,
                                      const uint16_t requestId) {
    if (requestId != 0U || service == nullptr) {
        return requestId;
    }
    return service->AllocateRequestId();
}

ControlAction ParseControlReplyAction(const JsonVariantConst root) {
    const JsonVariantConst actionValue = root["a"];
    if (!actionValue.is<const char*>()) {
        return ControlAction::Unknown;
    }
    const char* const actionToken = actionValue.as<const char*>();
    if (actionToken == nullptr || actionToken[0] == '\0') {
        return ControlAction::Unknown;
    }
    return ParseBleControlActionToken(actionToken);
}

ControlMutationOutcome MakeRejectedControlOutcome(
    ControlMutationService* const service,
    const uint16_t requestId,
    const ControlAction action,
    const ControlMutationReason reason) {
    ControlMutationOutcome outcome;
    outcome.requestId = ResolveControlReplyRequestId(service, requestId);
    outcome.action = action;
    outcome.status = ControlMutationStatus::Rejected;
    outcome.reason = reason;
    return outcome;
}

} // namespace

bool BleSettings::SendAdmittedResponse(uint16_t subscriptionValue, const char* payload,
    size_t payloadLen, bool ok, const char* compactPayload, size_t compactPayloadLen) {
    const auto epoch = CurrentConnectionEpoch();
    const auto handle = m_responseConnectionHandle.load(std::memory_order_acquire);
    if (m_readWriteCharacteristic == nullptr || !OwnsConnectionEpoch(epoch) ||
        !AllowsConnection(handle)) return false;
    return modules::ble_settings_async::SendAsyncResponse(*m_readWriteCharacteristic,
        subscriptionValue, payload, payloadLen, ok, compactPayload, compactPayloadLen, handle);
}

void BleSettings::OnReadWritePayload(const std::string_view payload) {
    if (!m_productAccess.load(std::memory_order_acquire)) return;
    if (payload.empty() || payload.size() > 512U ||
        payload.find('\0') != std::string_view::npos) {
        SendReadWriteFailure("", "bad_request");
        return;
    }

    StaticJsonDocument<SETTINGS_RW_PARSE_JSON_CAPACITY> doc;
    const DeserializationError error = deserializeJson(doc, payload.data(), payload.size());
    if (error) {
        SendReadWriteFailure("", "bad_request");
        return;
    }

    const JsonVariantConst root = doc.as<JsonVariantConst>();
    if (!HasSafeBleJsonText(root)) {
        SendReadWriteFailure("", "bad_request");
        return;
    }
    if (root.containsKey("a")) {
        uint16_t requestId = 0U;
        if (!TryReadRequestId(root, requestId)) {
            SendControlOutcome(MakeRejectedControlOutcome(
                m_controlMutationService, 0U, ParseControlReplyAction(root),
                ControlMutationReason::BadRequest));
            return;
        }
        const JsonVariantConst actionValue = root["a"];
        if (!actionValue.is<const char*>()) {
            SendControlOutcome(MakeRejectedControlOutcome(
                m_controlMutationService, requestId, ControlAction::Unknown,
                ControlMutationReason::BadRequest));
            return;
        }
        const char* const action = actionValue.as<const char*>();
        if (action == nullptr || action[0] == '\0') {
            SendControlOutcome(MakeRejectedControlOutcome(
                m_controlMutationService, requestId, ControlAction::Unknown,
                ControlMutationReason::BadRequest));
            return;
        }
        char actionJson[64] = {};
        const size_t actionLen =
            serializeJson(root, actionJson, sizeof(actionJson));
        if (actionLen == 0U || actionLen >= sizeof(actionJson) ||
            !EnqueueSettingRequest(CONTROL_EVENT_KEY, actionJson, true)) {
            SendControlOutcome(MakeRejectedControlOutcome(
                m_controlMutationService, requestId,
                ParseBleControlActionToken(action),
                actionLen >= sizeof(actionJson)
                    ? ControlMutationReason::BadRequest
                    : ControlMutationReason::QueueFull));
        }
        return;
    }

    uint16_t requestId = 0U;
    if (!TryReadRequestId(root, requestId)) {
        SendReadWriteFailure("", "bad_request");
        return;
    }

    const char* key = root["k"].as<const char*>();
    if (key == nullptr || key[0] == '\0') {
        SendReadWriteFailure("", "bad_request");
        return;
    }

    if (root.containsKey("v")) {
        char valueJson[64] = {};
        const size_t valueLen = requestId == 0U
                                    ? serializeJson(root["v"], valueJson,
                                                    sizeof(valueJson))
                                    : serializeJson(root, valueJson,
                                                    sizeof(valueJson));
        if (valueLen == 0U || valueLen >= sizeof(valueJson)) {
            SendReadWriteFailure(key, "value_too_large", requestId);
            return;
        }

        if (!EnqueueSettingRequest(key, valueJson, true)) {
            SendReadWriteFailure(key, "queue_full", requestId);
        }
        return;
    }

    char readMetadata[24] = {};
    if (requestId != 0U) {
        const int len = std::snprintf(readMetadata, sizeof(readMetadata),
                                      "{\"i\":%u}",
                                      static_cast<unsigned>(requestId));
        if (len <= 0 || static_cast<size_t>(len) >= sizeof(readMetadata)) {
            SendReadWriteFailure(key, "bad_request", requestId);
            return;
        }
    }
    if (!EnqueueSettingRequest(key, readMetadata, false)) {
        SendReadWriteFailure(key, "queue_full", requestId);
    }
}

void BleSettings::OnReadWriteSubscribe(const uint16_t subValue) {
    const uint32_t connectionEpoch = CurrentConnectionEpoch();
    m_ingressSubscriptionValue.store(subValue, std::memory_order_relaxed);
    m_ingressSubscriptionEpoch.store(connectionEpoch, std::memory_order_release);
    m_ingressSubscriptionRevision.fetch_add(1U, std::memory_order_acq_rel);
    if (OwnsConnectionEpoch(connectionEpoch)) {
        m_subscriptionValue.store(subValue, std::memory_order_release);
    }
}

bool BleSettings::HasReadWriteSubscription() const {
    return (m_subscriptionValue.load(std::memory_order_acquire) & 0x03U) != 0U;
}

bool BleSettings::EnqueueSettingRequest(const char* key,
                                        const char* valueJson,
                                        const bool hasValue) const {
    if (m_eventQueue == nullptr || key == nullptr) {
        return false;
    }

    BleEvent event;
    event.type = BleEventType::SettingWrite;
    event.payload.setting.hasValue = hasValue;
    event.payload.setting.connectionEpoch =
        m_ingressConnectionEpoch.load(std::memory_order_acquire);

    const size_t keyLen = std::strlen(key);
    if (keyLen == 0U || keyLen >= sizeof(event.payload.setting.key)) {
        return false;
    }
    std::memcpy(event.payload.setting.key, key, keyLen + 1U);

    const char* json = (valueJson != nullptr) ? valueJson : "";
    const size_t valueLen = std::strlen(json);
    if (valueLen >= sizeof(event.payload.setting.value)) {
        return false;
    }
    std::memcpy(event.payload.setting.value, json, valueLen + 1U);

    if (m_eventQueue->Enqueue(event)) {
        return true;
    }

    if (m_droppedEvents != nullptr) {
        m_droppedEvents->fetch_add(1, std::memory_order_acq_rel);
    }
    return false;
}

void BleSettings::HandleSettingWriteEvent(
    SettingsStore& settings,
    const BleSettingWriteEvent& event,
    const BleSettingsMutationCallback mutationCallback,
    void* const mutationContext) {
    // Kept in the adapter signature for source compatibility. Canonical
    // mutation callbacks now belong to ControlMutationService.
    (void)mutationCallback;
    (void)mutationContext;
    if (!OwnsConnectionEpoch(event.connectionEpoch)) {
        // The originating BLE connection no longer owns replies or semantic
        // work. The callback-side ingress epoch changes before the queued
        // lifecycle event drains, so this also closes the FIFO window where
        // an older write sits ahead of disconnect. Drop it silently so a
        // replacement client cannot inherit its request ID or action.
        return;
    }
    if (std::strcmp(event.key, CONTROL_EVENT_KEY) == 0) {
        StaticJsonDocument<SETTINGS_RW_PARSE_JSON_CAPACITY> actionDoc;
        const DeserializationError parseError =
            deserializeJson(actionDoc, event.value);
        const JsonVariantConst root = actionDoc.as<JsonVariantConst>();
        uint16_t requestId = 0U;
        if (parseError) {
            SendControlOutcome(MakeRejectedControlOutcome(
                m_controlMutationService, 0U, ControlAction::Unknown,
                ControlMutationReason::BadRequest));
            return;
        }
        if (!TryReadRequestId(root, requestId)) {
            SendControlOutcome(MakeRejectedControlOutcome(
                m_controlMutationService, 0U, ParseControlReplyAction(root),
                ControlMutationReason::BadRequest));
            return;
        }
        if (m_controlMutationService == nullptr) {
            SendControlOutcome(MakeRejectedControlOutcome(
                m_controlMutationService, requestId,
                ParseControlReplyAction(root),
                ControlMutationReason::Unsupported));
            return;
        }

        ParsedBleControlActionRequest request;
        if (!TryParseBleControlActionRequest(root, request)) {
            SendControlOutcome(MakeRejectedControlOutcome(
                m_controlMutationService, requestId,
                ParseControlReplyAction(root),
                ControlMutationReason::BadRequest));
            return;
        }
        const ControlMutationOutcome outcome =
            m_controlMutationService->MutateAction(
                request.action, requestId, request.owner,
                request.coachingMutation, request.undoMoveCount);
        if (outcome.status == ControlMutationStatus::Accepted) {
            TrackControlRequest(outcome.requestId);
        }
        SendControlOutcome(outcome);
        return;
    }

    uint16_t requestId = 0U;
    StaticJsonDocument<SETTINGS_RW_PARSE_JSON_CAPACITY> valueDoc;
    JsonVariantConst settingValue;
    if (event.value[0] != '\0') {
        const DeserializationError parseError =
            deserializeJson(valueDoc, event.value);
        if (parseError) {
            SendReadWriteFailure(event.key, "bad_request");
            return;
        }
        const JsonVariantConst root = valueDoc.as<JsonVariantConst>();
        if (root.containsKey("i")) {
            if (!TryReadRequestId(root, requestId)) {
                SendReadWriteFailure(event.key, "bad_request");
                return;
            }
            settingValue = root["v"];
        } else {
            settingValue = root;
        }
    }

    const fxe::SettingDescriptor* descriptor =
        modules::settings_registry::LookupByKey(event.key);
    if (descriptor == nullptr ||
        !modules::settings_registry::IsDirectAppAccessible(*descriptor)) {
        SendReadWriteFailure(
            event.key,
            modules::settings_registry::ValidateAndSetResultToString(
                modules::settings_registry::ValidateAndSetResult::UnknownKey),
            requestId);
        return;
    }

    if (event.hasValue) {
        if (m_controlMutationService == nullptr) {
            // A writable BLE characteristic is never a compatibility reason to
            // bypass the canonical mutation policy. Production wires the
            // service before the BLE stack starts; fail closed if that wiring
            // contract is broken.
            SendReadWriteFailure(event.key, "unsupported", requestId);
            return;
        }

        const ControlMutationOutcome outcome =
            m_controlMutationService->MutateSetting(
                descriptor->key, settingValue, requestId);
        if (outcome.status == ControlMutationStatus::Rejected) {
            SendReadWriteFailure(event.key,
                                 ControlMutationReasonToString(
                                     outcome.reason),
                                 requestId);
            return;
        }
    }

    SendReadWriteSuccess(settings, *descriptor, requestId);
}

void BleSettings::SendReadWriteSuccess(const SettingsStore& settings,
                                       const fxe::SettingDescriptor& descriptor,
                                       const uint16_t requestId) {
    if (m_readWriteCharacteristic == nullptr) {
        return;
    }

    StaticJsonDocument<SETTINGS_RW_RESPONSE_JSON_CAPACITY> doc;
    doc["k"] = descriptor.key;
    doc["ok"] = true;
    if (requestId != 0U) {
        doc["i"] = requestId;
    }
    JsonVariant valueVariant = doc["v"].to<JsonVariant>();
    if (!modules::ble_settings_value_codec::WriteCurrentValue(
            settings, descriptor, valueVariant)) {
        SendReadWriteFailure(descriptor.key, "type_mismatch");
        return;
    }

    char payload[SETTINGS_RW_RESPONSE_BUFFER_CAPACITY] = {};
    size_t payloadLen = 0;
    if (!EncodeJson(doc, payload, sizeof(payload), payloadLen)) {
        SendReadWriteFailure(descriptor.key, "response_too_large");
        return;
    }
    const uint16_t subscriptionValue =
        m_subscriptionValue.load(std::memory_order_acquire);
    if ((subscriptionValue & 0x03U) == 0U) {
        return;
    }
    char compact[24] = {};
    const size_t compactLen = requestId == 0U
                                  ? 0U
                                  : FormatBleSettingCompactResponse(
                                        requestId, true,
                                        ControlMutationReason::None, compact,
                                        sizeof(compact));
    (void)SendAdmittedResponse(
        subscriptionValue, payload, payloadLen,
        true, compactLen == 0U ? nullptr : compact, compactLen);
}

void BleSettings::SendReadWriteFailure(const char* key,
                                       const char* error,
                                       const uint16_t requestId) {
    if (m_readWriteCharacteristic == nullptr) {
        return;
    }

    StaticJsonDocument<SETTINGS_RW_RESPONSE_JSON_CAPACITY> doc;
    doc["k"] = (key != nullptr) ? key : "";
    doc["e"] = (error != nullptr) ? error : "unknown_error";
    doc["ok"] = false;
    if (requestId != 0U) {
        doc["i"] = requestId;
    }

    char payload[SETTINGS_RW_RESPONSE_BUFFER_CAPACITY] = {};
    size_t payloadLen = 0;
    if (!EncodeJson(doc, payload, sizeof(payload), payloadLen)) {
        return;
    }
    const uint16_t subscriptionValue =
        m_subscriptionValue.load(std::memory_order_acquire);
    if ((subscriptionValue & 0x03U) == 0U) {
        return;
    }
    char compact[24] = {};
    const size_t compactLen = requestId == 0U
                                  ? 0U
                                  : FormatBleSettingCompactResponse(
                                        requestId, false,
                                        BleSettingReasonFromErrorToken(error),
                                        compact, sizeof(compact));
    (void)SendAdmittedResponse(
        subscriptionValue, payload, payloadLen,
        false, compactLen == 0U ? nullptr : compact, compactLen);
}

bool BleSettings::SendControlOutcome(const ControlMutationOutcome& outcome) {
    const uint32_t connectionEpoch = CurrentConnectionEpoch();
    if (m_readWriteCharacteristic == nullptr ||
        !OwnsConnectionEpoch(connectionEpoch)) {
        return false;
    }

    StaticJsonDocument<SETTINGS_RW_RESPONSE_JSON_CAPACITY> doc;
    doc["i"] = outcome.requestId;
    doc["a"] = BleControlActionToString(outcome.action);
    doc["s"] = ControlMutationStatusToString(outcome.status);
    doc["r"] = ControlMutationReasonToString(outcome.reason);
    doc["p"] = ControlActionStateToString(outcome.actionState);
    doc["ok"] = outcome.status != ControlMutationStatus::Rejected;
    char payload[SETTINGS_RW_RESPONSE_BUFFER_CAPACITY] = {};
    size_t payloadLen = 0U;
    if (!EncodeJson(doc, payload, sizeof(payload), payloadLen)) {
        return false;
    }

    char compact[24] = {};
    const size_t compactLen = FormatControlMutationCompactResponse(
        outcome, compact, sizeof(compact));
    const uint16_t subscriptionValue =
        m_subscriptionValue.load(std::memory_order_acquire);
    if (!m_productAccess.load(std::memory_order_acquire)) return false;
    const bool accepted = SendAdmittedResponse(
        subscriptionValue, payload, payloadLen,
        outcome.status != ControlMutationStatus::Rejected,
        compactLen > 0U ? compact : nullptr, compactLen);
    return accepted && OwnsConnectionEpoch(connectionEpoch);
}

void BleSettings::TrackControlRequest(const uint16_t requestId) {
    if (requestId == 0U) {
        return;
    }
    for (size_t index = 0U; index < m_trackedControlRequestCount; ++index) {
        if (m_trackedControlRequestIds[index] == requestId) {
            return;
        }
    }
    if (m_trackedControlRequestCount >= TRACKED_CONTROL_REQUEST_CAPACITY) {
        logger::Warn("ble: tracked control completion capacity reached");
        return;
    }
    m_trackedControlRequestIds[m_trackedControlRequestCount++] = requestId;
}

void BleSettings::RemoveTrackedControlRequestAt(const size_t index) {
    if (index >= m_trackedControlRequestCount) {
        return;
    }
    for (size_t next = index + 1U; next < m_trackedControlRequestCount;
         ++next) {
        m_trackedControlRequestIds[next - 1U] =
            m_trackedControlRequestIds[next];
    }
    --m_trackedControlRequestCount;
    m_trackedControlRequestIds[m_trackedControlRequestCount] = 0U;
}

} // namespace modules

#endif
