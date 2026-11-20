#include "modules/ble_settings_enumerate.hpp"

#if defined(LITCH_BLE)

#include <ArduinoJson.h>

#include <cstddef>
#include <cstring>

#include <NimBLEDevice.h>

#include "logger.hpp"
#include "modules/settings_registry.hpp"

namespace modules::ble_settings_enumerate {

namespace {

constexpr size_t SETTINGS_ENUMERATE_JSON_CAPACITY = 2048;
constexpr size_t BLE_ATTRIBUTE_MAX_VALUE_LEN = 512;
constexpr size_t SETTINGS_ENUMERATE_BUFFER_CAPACITY =
    BLE_ATTRIBUTE_MAX_VALUE_LEN;
constexpr size_t SETTINGS_ENUMERATE_MAX_PAYLOAD_LEN = BLE_ATTRIBUTE_MAX_VALUE_LEN - 1U;
constexpr char EMPTY_CHUNK_JSON[] = "{\"it\":[],\"nx\":0,\"dn\":true}";

// NimBLE callbacks run on the nimble_host task with a small stack. The only
// retained scratch is the complete bounded response, behind this adapter.
char g_enumeratePayload[SETTINGS_ENUMERATE_BUFFER_CAPACITY] = {};

class PayloadWriter final {
  public:
    size_t write(const uint8_t value) {
        if (m_length < sizeof(g_enumeratePayload)) {
            g_enumeratePayload[m_length] = static_cast<char>(value);
        }
        ++m_length;
        return 1U;
    }

    size_t write(const uint8_t* data, const size_t length) {
        for (size_t index = 0U; index < length; ++index) {
            write(data[index]);
        }
        return length;
    }

    void AppendRaw(const char* text) {
        write(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
    }

    template <typename T>
    void AppendScalar(const T value) {
        // Scalars need no JSON pool. Reuse ArduinoJson's public serializer so
        // string escaping and number/bool/null types match the former tree.
        StaticJsonDocument<0> scalar;
        scalar.set(value);
        serializeJson(scalar, *this);
    }

    template <typename T>
    void AppendField(const char* key, const T value) {
        AppendRaw(key);
        AppendScalar(value);
    }

    size_t Length() const { return m_length; }
    void Rollback(const size_t length) { m_length = length; }

  private:
    // Count even after the output fills, so an oversized descriptor can be
    // rejected as a whole without publishing any of its partial bytes.
    size_t m_length = 0U;
};

const char* SettingTypeToShortToken(const fxe::SettingType type) {
    switch (type) {
        case fxe::SettingType::UInt32:
            return "u32";
        case fxe::SettingType::Bool:
            return "bool";
        case fxe::SettingType::String:
            return "str";
        case fxe::SettingType::Enum:
            return "enum";
    }

    return "u32";
}

const char* SettingCategoryToShortToken(const fxe::SettingCategory category) {
    switch (category) {
        case fxe::SettingCategory::Display:
            return "display";
        case fxe::SettingCategory::Animation:
            return "animation";
        case fxe::SettingCategory::Behavior:
            return "behavior";
        case fxe::SettingCategory::Clock:
            return "clock";
        case fxe::SettingCategory::Hardware:
            return "hardware";
        case fxe::SettingCategory::Internal:
            return "internal";
    }

    return "internal";
}

size_t AppendDescriptor(PayloadWriter& writer,
                      const SettingsStore& settings,
                      const fxe::SettingDescriptor& descriptor,
                      const size_t visibleIndex) {
    size_t fieldCount = 4U;
    size_t optionCount = 0U;
    writer.AppendField("{\"i\":", visibleIndex);
    writer.AppendField(",\"k\":", descriptor.key);
    writer.AppendField(",\"t\":", SettingTypeToShortToken(descriptor.type));
    writer.AppendField(",\"c\":", SettingCategoryToShortToken(descriptor.category));

    switch (descriptor.type) {
        case fxe::SettingType::UInt32:
            fieldCount += 4U;
            writer.AppendField(",\"v\":", modules::settings_registry::ReadCurrentUInt32(
                settings, descriptor));
            writer.AppendField(",\"df\":", descriptor.defaultUint);
            writer.AppendField(",\"mn\":", descriptor.min);
            writer.AppendField(",\"mx\":", descriptor.max);
            break;
        case fxe::SettingType::Bool: {
            fieldCount += 2U;
            const bool defaultBool = descriptor.defaultUint != 0U;
            writer.AppendField(",\"v\":", settings.ReadValue(descriptor.key, defaultBool));
            writer.AppendField(",\"df\":", defaultBool);
            break;
        }
        case fxe::SettingType::String:
        case fxe::SettingType::Enum: {
            fieldCount += 2U;
            const char* defaultStr = descriptor.defaultStr != nullptr ? descriptor.defaultStr : "";
            writer.AppendField(",\"v\":", settings.ReadValue(descriptor.key, defaultStr));
            writer.AppendField(",\"df\":", defaultStr);
            if (descriptor.type == fxe::SettingType::Enum &&
                descriptor.allowedValues != nullptr && descriptor.allowedCount > 0U) {
                ++fieldCount;
                writer.AppendRaw(",\"opts\":[");
                bool firstOption = true;
                for (size_t index = 0U; index < descriptor.allowedCount; ++index) {
                    const char* option = descriptor.allowedValues[index];
                    if (option == nullptr) continue;
                    ++optionCount;
                    if (!firstOption) writer.AppendRaw(",");
                    writer.AppendScalar(option);
                    firstOption = false;
                }
                writer.AppendRaw("]");
            }
            break;
        }
    }

    if (descriptor.description != nullptr && descriptor.description[0] != '\0') {
        ++fieldCount;
        writer.AppendField(",\"d\":", descriptor.description);
    }
    writer.AppendRaw("}");
    // All former tree strings were const pointers, so only variant slots used
    // its pool. Preserve its sticky overflow fallback without retaining it.
    return JSON_ARRAY_SIZE(1U) + JSON_OBJECT_SIZE(fieldCount) + JSON_ARRAY_SIZE(optionCount);
}

void AppendChunkTail(PayloadWriter& writer, const size_t nextIndex, const bool done) {
    writer.AppendField("],\"nx\":", nextIndex);
    writer.AppendField(",\"dn\":", done);
    writer.AppendRaw("}");
}

} // namespace

void BuildResponse(const SettingsStore& settings,
                   NimBLECharacteristic& characteristic,
                   const size_t startIndex,
                   size_t& outNextIndex,
                   bool& outDone) {
    PayloadWriter writer;
    writer.AppendRaw("{\"it\":[");
    bool payloadTruncated = false;
    bool firstDescriptor = true;
    size_t visibleIndex = 0U;
    size_t jsonPoolUsage = JSON_OBJECT_SIZE(3U);

    size_t descriptorCount = 0U;
    const fxe::SettingDescriptor* descriptors = modules::settings_registry::EnumerateAll(descriptorCount);
    for (size_t index = 0U; descriptors != nullptr && index < descriptorCount; ++index) {
        const fxe::SettingDescriptor& descriptor = descriptors[index];
        if (!descriptor.IsVisibleToApp() || descriptor.IsInternal()) continue;
        if (descriptor.IsRetired() || visibleIndex < startIndex) {
            ++visibleIndex;
            continue;
        }

        const size_t descriptorStart = writer.Length();
        if (!firstDescriptor) writer.AppendRaw(",");
        jsonPoolUsage += AppendDescriptor(writer, settings, descriptor, visibleIndex);
        if (jsonPoolUsage > SETTINGS_ENUMERATE_JSON_CAPACITY) break;
        ++visibleIndex;

        const size_t descriptorEnd = writer.Length();
        // Match the former preview with dn=false, even for the final item.
        AppendChunkTail(writer, visibleIndex, false);
        const size_t previewLength = writer.Length();
        writer.Rollback(descriptorEnd);
        if (previewLength > SETTINGS_ENUMERATE_MAX_PAYLOAD_LEN) {
            writer.Rollback(descriptorStart);
            --visibleIndex;
            payloadTruncated = true;
            break;
        }
        firstDescriptor = false;
    }

    if (jsonPoolUsage > SETTINGS_ENUMERATE_JSON_CAPACITY) {
        logger::Warn("ble: settings enumerate json overflow");
        characteristic.setValue(reinterpret_cast<const uint8_t*>(EMPTY_CHUNK_JSON), std::strlen(EMPTY_CHUNK_JSON));
        outNextIndex = 0U;
        outDone = true;
        return;
    }

    outNextIndex = payloadTruncated ? visibleIndex : 0U;
    outDone = !payloadTruncated;
    AppendChunkTail(writer, outNextIndex, outDone);
    const size_t payloadLen = writer.Length();
    if (payloadLen == 0U || payloadLen >= sizeof(g_enumeratePayload)) {
        logger::Warn("ble: settings enumerate serialization overflow");
        characteristic.setValue(reinterpret_cast<const uint8_t*>(EMPTY_CHUNK_JSON), std::strlen(EMPTY_CHUNK_JSON));
        outNextIndex = 0U;
        outDone = true;
        return;
    }
    g_enumeratePayload[payloadLen] = '\0';

    characteristic.setValue(reinterpret_cast<const uint8_t*>(g_enumeratePayload), payloadLen);
    const size_t storedLength = characteristic.getLength();
    if (storedLength != payloadLen) {
        logger::Warn("ble: settings enumerate setValue length mismatch (want=%lu got=%lu)",
                     static_cast<unsigned long>(payloadLen),
                     static_cast<unsigned long>(storedLength));
        characteristic.setValue(reinterpret_cast<const uint8_t*>(EMPTY_CHUNK_JSON), std::strlen(EMPTY_CHUNK_JSON));
        outNextIndex = 0;
        outDone = true;
    }
}

} // namespace modules::ble_settings_enumerate

#endif
