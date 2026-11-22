#include <ArduinoJson.h>

#include <cstdint>
#include <cstring>
#include <string_view>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/fixed_string.hpp"
#include "foxie_embedded/setting_descriptor.hpp"
#include "foxie_embedded/settings_store.hpp"

#include "console_builtins_helpers.hpp"
#include "settings_store_internal.hpp"

namespace fxe {

namespace {

struct ConsoleRawWriter {
    Console& console;

    size_t write(const uint8_t b) {
        const char c = static_cast<char>(b);
        console.PrintRaw(&c, 1);
        return 1;
    }

    size_t write(const uint8_t* buffer, const size_t size) {
        if (!buffer || size == 0) {
            return 0;
        }
        console.PrintRaw(reinterpret_cast<const char*>(buffer), size);
        return size;
    }
};

const char* SettingTypeToString(const fxe::SettingType type) {
    switch (type) {
        case fxe::SettingType::UInt32:
            return "uint32";
        case fxe::SettingType::Bool:
            return "bool";
        case fxe::SettingType::String:
            return "string";
        case fxe::SettingType::Enum:
            return "enum";
    }

    return "unknown";
}

const fxe::SettingDescriptor* LookupSettingDescriptorForKey(Console& console,
                                                            const std::string_view key) {
    if (key.empty()) {
        return nullptr;
    }

    if (const fxe::SettingDescriptor* descriptor = console.LookupSettingDescriptor(key)) {
        return descriptor;
    }

    char keyBuffer[64] = {};
    char canonicalKey[64] = {};
    if (key.size() >= sizeof(keyBuffer)) {
        return nullptr;
    }

    for (size_t i = 0; i < key.size(); ++i) {
        keyBuffer[i] = key[i];
    }
    keyBuffer[key.size()] = '\0';

    if (!CopyUppercaseSettingKey(keyBuffer, canonicalKey, sizeof(canonicalKey))) {
        return nullptr;
    }

    return console.LookupSettingDescriptor(canonicalKey);
}

bool TryReadBoolLikeValue(const JsonVariantConst value, bool& outValue) {
    if (value.is<bool>()) {
        outValue = value.as<bool>();
        return true;
    }

    if (value.is<uint64_t>()) {
        const uint64_t numeric = value.as<uint64_t>();
        if (numeric > 1U) {
            return false;
        }
        outValue = (numeric != 0U);
        return true;
    }

    if (value.is<int64_t>()) {
        const int64_t numeric = value.as<int64_t>();
        if (numeric < 0 || numeric > 1) {
            return false;
        }
        outValue = (numeric != 0);
        return true;
    }

    return false;
}

void PrintCurrentSettingValue(Console& console,
                              fxe::SettingsStore& settings,
                              const fxe::SettingDescriptor& descriptor) {
    const char* existing = FindTopLevelObjectKeyCaseInsensitive(settings.Doc(), descriptor.key);
    if (!existing) {
        return;
    }

    ConsoleRawWriter writer{console};
    JsonVariantConst currentValue = settings.Doc()[existing];

    if (descriptor.type == fxe::SettingType::Bool) {
        bool boolValue = false;
        if (TryReadBoolLikeValue(currentValue, boolValue)) {
            console.Print("%s", boolValue ? "true" : "false");
            return;
        }
    }

    const size_t n = serializeJson(currentValue, writer);
    if (n == 0) {
        console.PrintRaw("(too large)", std::strlen("(too large)"));
    }
}

void PrintDefaultSettingValue(Console& console, const fxe::SettingDescriptor& descriptor) {
    if (descriptor.type == fxe::SettingType::Bool) {
        console.Print("%s", descriptor.defaultUint != 0U ? "true" : "false");
        return;
    }

    if (descriptor.type == fxe::SettingType::Enum || descriptor.type == fxe::SettingType::String) {
        const char* defaultValue = (descriptor.defaultStr != nullptr) ? descriptor.defaultStr : "";
        if (defaultValue[0] == '\0') {
            console.Print("\"\"");
            return;
        }

        console.Print("%s", defaultValue);
        return;
    }

    console.Print("%lu", static_cast<unsigned long>(descriptor.defaultUint));
}

void PrintSettingDescriptorSummary(Console& console,
                                   fxe::SettingsStore& settings,
                                   const fxe::SettingDescriptor& descriptor) {
    console.BeginBulkOutput();
    console.Print("%s: %s", descriptor.key, SettingTypeToString(descriptor.type));

    if (descriptor.type == fxe::SettingType::UInt32) {
        console.Print(", range %lu-%lu", static_cast<unsigned long>(descriptor.min), static_cast<unsigned long>(descriptor.max));
    }

    if (descriptor.type == fxe::SettingType::Bool) {
        console.Print(", range true/false");
    }

    console.Print(", default ");
    PrintDefaultSettingValue(console, descriptor);

    const char* existing = FindTopLevelObjectKeyCaseInsensitive(settings.Doc(), descriptor.key);
    if (existing != nullptr) {
        console.Print(", current ");
        PrintCurrentSettingValue(console, settings, descriptor);
    } else {
        console.Print(", not set, using default");
    }

    console.PrintLn("");
    console.EndBulkOutput();
}

void PrintSettingEnumValueHelp(Console& console, const fxe::SettingDescriptor& descriptor) {
    if (descriptor.type != fxe::SettingType::Enum || descriptor.allowedValues == nullptr || descriptor.allowedCount == 0U) {
        return;
    }

    const char* defaultValue = (descriptor.defaultStr != nullptr) ? descriptor.defaultStr : "";

    console.BeginBulkOutput();
    console.PrintLn("%s values:", descriptor.key);
    for (size_t i = 0; i < descriptor.allowedCount; ++i) {
        const char* allowed = descriptor.allowedValues[i];
        if (!allowed || allowed[0] == '\0') {
            continue;
        }

        if (std::strcmp(allowed, defaultValue) == 0) {
            console.PrintLn("  %s (default)", allowed);
        } else {
            console.PrintLn("  %s", allowed);
        }
    }
    console.EndBulkOutput();
}

size_t CollectEnumValueMatches(const char* prefix,
                               const fxe::SettingDescriptor& descriptor,
                               const char** outMatches,
                               const size_t outMatchesCap) {
    if (descriptor.type != fxe::SettingType::Enum || descriptor.allowedValues == nullptr || descriptor.allowedCount == 0U) {
        return 0;
    }

    size_t matchCount = 0;
    for (size_t i = 0; i < descriptor.allowedCount; ++i) {
        const char* allowed = descriptor.allowedValues[i];
        if (!allowed || allowed[0] == '\0') {
            continue;
        }
        matchCount = fxe::AddUniqueMatchIfPrefix(allowed, prefix, outMatches, matchCount, outMatchesCap);
    }
    return matchCount;
}

size_t CollectBoolValueMatches(const char* prefix,
                               const char** outMatches,
                               const size_t outMatchesCap) {
    size_t matchCount = 0;
    matchCount = fxe::AddUniqueMatchIfPrefix("true", prefix, outMatches, matchCount, outMatchesCap);
    matchCount = fxe::AddUniqueMatchIfPrefix("false", prefix, outMatches, matchCount, outMatchesCap);
    return matchCount;
}

bool IsValidSettingKey(const std::string_view key) {
    static constexpr size_t MAX_KEY_LEN = 63;
    if (key.empty() || key.size() > MAX_KEY_LEN) {
        return false;
    }

    for (const char ch : key) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == 0) {
            return false;
        }
        if (c < 32 || c > 126) {
            return false;
        }
        if (c == '\\') {
            return false;
        }
    }

    return true;
}

size_t CollectSettingKeyMatches(Console* self,
                                const char* prefix,
                                const char** outMatches,
                                const size_t outMatchesCap) {
    size_t matchCount = 0;

    char normalizedPrefix[64] = {};
    const char* effectivePrefix = prefix;
    if (prefix != nullptr && prefix[0] != '\0' &&
        CopyUppercaseSettingKey(prefix, normalizedPrefix, sizeof(normalizedPrefix))) {
        effectivePrefix = normalizedPrefix;
    }

    if (!self) {
        return matchCount;
    }

    const size_t extraCount = self->EnumerateExtraSettingKeys(effectivePrefix, outMatches, outMatchesCap);
    for (size_t i = 0; i < extraCount; ++i) {
        if (self->CanAccessDiagnosticSetting(outMatches[i])) outMatches[matchCount++] = outMatches[i];
    }

    fxe::SettingsStore* settings = self->GetSettingsStore();
    if (!settings) {
        return matchCount;
    }

    auto& doc = settings->Doc();
    for (const auto kv : doc.as<JsonObject>()) {
        if (!self->CanAccessDiagnosticSetting(kv.key().c_str())) continue;
        matchCount = fxe::AddUniqueMatchIfPrefix(
            kv.key().c_str(), effectivePrefix, outMatches, matchCount, outMatchesCap);
    }

    return matchCount;
}

} // namespace

void Console::InstallBuiltinSet() {
    auto setCmd = [this](const Args& args) -> bool {
        if (!m_settings) {
            PrintLn("No settings store configured");
            return false;
        }

        auto* settings = m_settings;
        auto& doc = settings->Doc();

        const auto saveOrDefer = [&](const std::string_view key) -> bool {
            if (ShouldForceSaveNowForSettingKey(key)) {
                settings->MarkMustSaveDirty();
            } else {
                settings->MarkDirty();
            }
            PrintLn("OK");
            return true;
        };

        // set
        if (args.argc < 2) {
            BeginBulkOutput();
            ConsoleRawWriter writer{*this};
            for (const auto kv : doc.as<JsonObject>()) {
                const char* key = kv.key().c_str();
                if (!CanAccessDiagnosticSetting(key)) continue;
                PrintRaw(key, std::strlen(key));
                PrintRaw("=", 1);
                const size_t n = serializeJson(kv.value(), writer);
                if (n == 0) {
                    PrintRaw("(too large)", std::strlen("(too large)"));
                }
                PrintRaw("\r\n", 2);
            }
            EndBulkOutput();
            return true;
        }

        const std::string_view keyView = args.Get(1);
        const char* key = args.CStr(1);
        if (!key || key[0] == '\0' || !IsValidSettingKey(keyView)) {
            PrintLn("usage: set [key] [value...]");
            return false;
        }

        char canonicalKey[64] = {};
        if (!CopyUppercaseSettingKey(key, canonicalKey, sizeof(canonicalKey))) {
            PrintLn("usage: set [key] [value...]");
            return false;
        }

        const std::string_view canonicalKeyView(canonicalKey);

        // set <key>
        if (args.argc == 2) {
            if (!CanAccessDiagnosticSetting(canonicalKeyView)) {
                PrintLn("(unavailable)");
                return false;
            }
            if (HasSettingsDescriptorLookupCallback()) {
                const fxe::SettingDescriptor* descriptor =
                    LookupSettingDescriptorForKey(*this, canonicalKeyView);
                if (!descriptor) {
                    PrintLn("set: %s", SettingsSetResultToString(SettingsSetResult::UnknownKey));
                    return false;
                }

                PrintSettingDescriptorSummary(*this, *settings, *descriptor);
                return true;
            }

            NotifyBeforeSettingsMutation(canonicalKeyView);
            if (const char* existing = FindTopLevelObjectKeyCaseInsensitive(doc, canonicalKey)) {
                (void)doc.remove(existing);
            } else {
                (void)doc.remove(canonicalKey);
            }

            return saveOrDefer(canonicalKeyView);
        }

        // set <key> <value...>
        fxe::FixedString<192> value;
        (void)value.Assign(args.Get(2));
        for (size_t i = 3; i < args.argc; ++i) {
            (void)value.PushBack(' ');
            (void)value.Append(args.Get(i));
        }

        StaticJsonDocument<128> parsedDoc;
        StaticJsonDocument<192> rawDoc;
        JsonVariantConst setValue;

        const bool parsed = (deserializeJson(parsedDoc, value.CStr()) == DeserializationError::Ok);
        if (parsed) {
            setValue = parsedDoc.as<JsonVariantConst>();
        } else {
            rawDoc.set(value.CStr());
            setValue = rawDoc.as<JsonVariantConst>();
        }

        if (HasSettingsValidateAndSetCallback()) {
            const SettingsSetResult validation = ValidateAndSetSettingValue(*settings, canonicalKeyView, setValue);
            if (validation == SettingsSetResult::Applied ||
                validation == SettingsSetResult::Noop) {
                PrintLn("OK");
                return true;
            }
            if (validation != SettingsSetResult::Ok) {
                PrintLn("set: %s", SettingsSetResultToString(validation));
                return false;
            }
        }

        NotifyBeforeSettingsMutation(canonicalKeyView);
        if (const char* existing = FindTopLevelObjectKeyCaseInsensitive(doc, canonicalKey)) {
            if (std::strcmp(existing, canonicalKey) != 0) {
                (void)doc.remove(existing);
            }
        }
        if (!HasSettingsValidateAndSetCallback() && parsed) {
            doc[canonicalKey] = parsedDoc.as<JsonVariant>();
        } else if (!HasSettingsValidateAndSetCallback()) {
            doc[canonicalKey].set(const_cast<char*>(value.CStr()));
        }

        return saveOrDefer(canonicalKeyView);
    };

    auto setComplete = [this](const Args& args,
                              const size_t argIndex,
                              const char* prefix,
                              const char** outMatches,
                              const size_t outMatchesCap) -> size_t {
        // Key completion.
        if (argIndex == 1) {
            if (args.argc == 2 && args.Has(1) && HasSettingsDescriptorLookupCallback()) {
                const fxe::SettingDescriptor* descriptor =
                    LookupSettingDescriptorForKey(*this, args.Get(1));
                fxe::SettingsStore* settings = GetSettingsStore();
                if (descriptor && settings) {
                    PrintSettingDescriptorSummary(*this, *settings, *descriptor);
                    if (descriptor->type == fxe::SettingType::Enum) {
                        PrintSettingEnumValueHelp(*this, *descriptor);
                    }
                    return 0;
                }
            }

            return CollectSettingKeyMatches(this, prefix, outMatches, outMatchesCap);
        }

        // Value completion / help from setting descriptors.
        if (argIndex == 2 && args.Has(1) && HasSettingsDescriptorLookupCallback()) {
            const fxe::SettingDescriptor* descriptor =
                LookupSettingDescriptorForKey(*this, args.Get(1));
            if (!descriptor) {
                return 0;
            }

            if (descriptor->type == fxe::SettingType::Bool) {
                return CollectBoolValueMatches(prefix, outMatches, outMatchesCap);
            }

            if (descriptor->type != fxe::SettingType::Enum) {
                return 0;
            }

            if (prefix == nullptr || prefix[0] == '\0') {
                PrintSettingEnumValueHelp(*this, *descriptor);
                return 0;
            }

            return CollectEnumValueMatches(prefix, *descriptor, outMatches, outMatchesCap);
        }

        (void)args;
        (void)prefix;
        return 0;
    };

    RegisterCommand("set", "Settings: set [key] [value...] (no args prints all)", setCmd, setComplete);
}

} // namespace fxe
