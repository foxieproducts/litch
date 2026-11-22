#include <ArduinoJson.h>

#include <cstdint>
#include <cstring>
#include <string_view>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/setting_descriptor.hpp"
#include "foxie_embedded/settings_store.hpp"

#include "console_builtins_helpers.hpp"
#include "settings_store_internal.hpp"

namespace fxe {

namespace {

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
        matchCount = fxe::AddUniqueMatchIfPrefix(kv.key().c_str(), effectivePrefix, outMatches, matchCount, outMatchesCap);
    }

    return matchCount;
}

} // namespace

void Console::InstallBuiltinSettings() {
    auto settingsCmd = [this](const Args& args) -> bool {
        if (!m_settings) {
            PrintLn("No settings store configured");
            return false;
        }

        auto* settings = m_settings;

        const auto saveOrDefer = [&](const std::string_view key) -> bool {
            if (ShouldForceSaveNowForSettingKey(key)) {
                settings->MarkMustSaveDirty();
            } else {
                settings->MarkDirty();
            }
            PrintLn("OK");
            return true;
        };

        const auto sub = (args.argc >= 2) ? args.Get(1) : std::string_view("status");
        const bool formatConfirmed = sub == "format" && args.argc == 3 &&
            args.Get(2) == "ERASE_BOARD_STORAGE";
        const bool replacesStorage = sub == "load" || sub == "clear" ||
            sub == "erase" || sub == "corrupt" || formatConfirmed;
        if (replacesStorage && m_settingsStorageAdmissionCb) {
            const auto operation = sub == "load" ? SettingsStorageOperation::Load :
                (sub == "corrupt" ? SettingsStorageOperation::Corrupt :
                 (formatConfirmed ? SettingsStorageOperation::Format :
                  SettingsStorageOperation::Erase));
            if (!m_settingsStorageAdmissionCb(operation)) {
                PrintLn("Unavailable here; use physical board recovery.");
                return false;
            }
        }
        if (sub == "status") {
            PrintLn("path=%s", settings->GetPath());
            PrintLn("disabled=%u errors=%lu last_error=%s",
                    static_cast<unsigned>(settings->IsDisabled() ? 1U : 0U),
                    static_cast<unsigned long>(settings->GetErrorCount()),
                    fxe::SettingsStoreErrorToString(settings->GetLastError()));
            PrintLn("saves_since_boot=%lu last_save_bytes=%lu",
                    static_cast<unsigned long>(settings->GetSaveCount()),
                    static_cast<unsigned long>(settings->GetLastSaveSizeBytes()));

            PrintLn("saves_total=%lu",
                    static_cast<unsigned long>(
                        settings->GetPersistedSaveTotal()));
            return true;
        }

        if (args.argc < 2) {
            PrintLn("Usage: settings <status|path|load|save|erase|clear|corrupt|format|keys|get|set|dump|paste> ...");
            return false;
        }

        if (sub == "path") {
            PrintLn("%s", settings->GetPath());
            return true;
        }

        if (sub == "load") {
            NotifyBeforeSettingsMutation({});
            const bool ok = m_settingsLoadCb ? m_settingsLoadCb() : settings->Load();
            PrintLn(ok ? "OK" : "FAIL");
            return ok;
        }

        if (sub == "save") {
            NotifyBeforeSettingsMutation({});
            const bool ok = settings->SaveNow();
            PrintLn(ok ? "OK" : "FAIL");
            return ok;
        }

        if (sub == "erase" || sub == "clear") {
            if (args.argc != 2U) {
                PrintLn("Usage: settings clear|erase");
                return false;
            }
            if (m_settingsClearCb) {
                const bool accepted = m_settingsClearCb();
                PrintLn(accepted ? "settings clear: accepted" : "settings clear: rejected");
                return accepted;
            }
            NotifyBeforeSettingsMutation({});
            const bool ok = settings->DeleteFile();
            if (ok) {
                // Reset the in-memory view immediately (no reboot required).
                (void)(m_settingsLoadCb ? m_settingsLoadCb() : settings->Load());
            }
            PrintLn(ok ? "OK" : "FAIL");
            return ok;
        }

        if (sub == "corrupt") {
            NotifyBeforeSettingsMutation({});
            const bool ok = settings->CorruptFileForTest();
            PrintLn(ok ? "OK" : "FAIL");
            return ok;
        }

        if (sub == "format") {
            if (args.argc > 3 || (args.argc == 3 &&
                args.Get(2) != "status" && args.Get(2) != "ERASE_BOARD_STORAGE")) {
                PrintLn("Usage: settings format <status|ERASE_BOARD_STORAGE>");
                return false;
            }
            const bool confirm = args.argc == 3 &&
                args.Get(2) == "ERASE_BOARD_STORAGE";
            if (args.argc == 2) {
                PrintLn("Formatting erases board settings, games and artwork. Phone libraries are unchanged.");
                PrintLn("To confirm: settings format ERASE_BOARD_STORAGE");
            }
            const auto status = m_storageReformatCb
                ? m_storageReformatCb(confirm ? StorageReformatAction::Confirm
                                              : StorageReformatAction::Status)
                : StorageReformatStatus::Unavailable;
            const char* state = "unavailable";
            switch (status) {
                case StorageReformatStatus::Unavailable: break;
                case StorageReformatStatus::Idle: state = "idle"; break;
                case StorageReformatStatus::Busy: state = "busy"; break;
                case StorageReformatStatus::Pending: state = "pending"; break;
                case StorageReformatStatus::Running: state = "running"; break;
                case StorageReformatStatus::Succeeded: state = "succeeded"; break;
                case StorageReformatStatus::Failed: state = "failed"; break;
            }
            PrintLn("storage_format=%s reboot_required=%u", state,
                    status == StorageReformatStatus::Succeeded ? 1U : 0U);
            return status != StorageReformatStatus::Unavailable &&
                status != StorageReformatStatus::Busy &&
                status != StorageReformatStatus::Failed;
        }

        auto& doc = settings->Doc();

        if (sub == "keys") {
            BeginBulkOutput();
            for (const auto kv : doc.as<JsonObject>()) {
                if (CanAccessDiagnosticSetting(kv.key().c_str())) PrintLn("%s", kv.key().c_str());
            }
            EndBulkOutput();
            return true;
        }

        if (sub == "get") {
            if (args.argc < 3) {
                PrintLn("Usage: settings get <key>");
                return false;
            }
            const char* key = args.CStr(2);
            const char* lookupKey = FindTopLevelObjectKeyCaseInsensitive(doc, key);
            if (!CanAccessDiagnosticSetting(lookupKey != nullptr ? lookupKey : key)) {
                PrintLn("(unavailable)");
                return false;
            }
            JsonVariant v = (lookupKey != nullptr) ? doc[lookupKey] : doc[key];
            if (v.isNull()) {
                PrintLn("(null)");
                return true;
            }

            if (HasSettingsDescriptorLookupCallback()) {
                const fxe::SettingDescriptor* descriptor = LookupSettingDescriptor(std::string_view(key));
                if (descriptor != nullptr && descriptor->type == fxe::SettingType::Bool) {
                    bool boolValue = false;
                    if (TryReadBoolLikeValue(v.as<JsonVariantConst>(), boolValue)) {
                        PrintLn("%s", boolValue ? "true" : "false");
                        return true;
                    }
                }
            }

            char out[256];
            const size_t n = serializeJson(v, out, sizeof(out));
            if (n == 0) {
                PrintLn("(too large)");
                return false;
            }
            out[sizeof(out) - 1] = '\0';
            PrintLn("%s", out);
            return true;
        }

        if (sub == "set") {
            if (args.argc < 4) {
                PrintLn("Usage: settings set <key> <value>");
                return false;
            }
            const char* key = args.CStr(2);
            const char* value = args.CStr(3);

            char canonicalKey[64] = {};
            if (!CopyUppercaseSettingKey(key, canonicalKey, sizeof(canonicalKey))) {
                PrintLn("Usage: settings set <key> <value>");
                return false;
            }

            const std::string_view keyView(canonicalKey);
            StaticJsonDocument<128> parsedDoc;
            StaticJsonDocument<192> rawDoc;
            JsonVariantConst setValue;

            const bool parsed = (deserializeJson(parsedDoc, value) == DeserializationError::Ok);
            if (parsed) {
                setValue = parsedDoc.as<JsonVariantConst>();
            } else {
                // ArduinoJson may link `const char*` instead of copying.
                // `value` points into the mutable console input buffer, so force a copy.
                rawDoc.set(const_cast<char*>(value));
                setValue = rawDoc.as<JsonVariantConst>();
            }

            if (HasSettingsValidateAndSetCallback()) {
                const SettingsSetResult validation =
                    ValidateAndSetSettingValue(*settings, keyView, setValue);
                if (validation == SettingsSetResult::Applied ||
                    validation == SettingsSetResult::Noop) {
                    PrintLn("OK");
                    return true;
                }
                if (validation != SettingsSetResult::Ok) {
                    PrintLn("settings set: %s", SettingsSetResultToString(validation));
                    return false;
                }
            }

            NotifyBeforeSettingsMutation(keyView);
            if (const char* existing = FindTopLevelObjectKeyCaseInsensitive(doc, canonicalKey)) {
                if (std::strcmp(existing, canonicalKey) != 0) {
                    (void)doc.remove(existing);
                }
            }
            if (!HasSettingsValidateAndSetCallback() && parsed) {
                doc[canonicalKey] = parsedDoc.as<JsonVariant>();
            } else if (!HasSettingsValidateAndSetCallback()) {
                doc[canonicalKey].set(const_cast<char*>(value));
            }

            return saveOrDefer(keyView);
        }

        if (sub == "dump") {
            if (m_settingsDiagnosticAccessCb) {
                // Stream permitted values without duplicating the settings document.
                struct Writer {
                    Console& console;
                    char buffer[128]{};
                    size_t used = 0;
                    void Flush() { if (used) { buffer[used] = '\0'; console.Print("%s", buffer); used = 0; } }
                    size_t write(uint8_t byte) {
                        buffer[used++] = static_cast<char>(byte);
                        if (used == sizeof(buffer) - 1U) Flush();
                        return 1U;
                    }
                    size_t write(const uint8_t* bytes, size_t count) {
                        for (size_t i = 0; i < count; ++i) write(bytes[i]);
                        return count;
                    }
                } writer{*this};
                BeginBulkOutput();
                writer.write('{');
                bool first = true;
                for (const auto kv : doc.as<JsonObjectConst>()) {
                    if (!CanAccessDiagnosticSetting(kv.key().c_str())) continue;
                    if (!first) writer.write(',');
                    first = false;
                    StaticJsonDocument<0> keyDocument;
                    keyDocument.set(kv.key().c_str());
                    serializeJson(keyDocument, writer);
                    writer.write(':');
                    serializeJson(kv.value(), writer);
                }
                writer.write('}'); writer.Flush(); PrintLn("");
                EndBulkOutput();
                return true;
            }
            char out[1024];
            const size_t n = serializeJsonPretty(doc, out, sizeof(out));
            if (n == 0) {
                PrintLn("(too large)");
                return false;
            }
            out[sizeof(out) - 1] = '\0';

            BeginBulkOutput();
            PrintLn("%s", out);
            EndBulkOutput();
            return true;
        }
        if (sub == "paste") {
            return StartSettingsPaste(*settings);
        }

        PrintLn("Unknown subcommand: %s", fxe::SafeStr(args.CStr(1)));
        return false;
    };

    auto settingsComplete = [this](const Args& args,
                                   const size_t argIndex,
                                   const char* prefix,
                                   const char** outMatches,
                                   const size_t outMatchesCap) -> size_t {
        if (argIndex == 1) {
            size_t matchCount = 0;
            static constexpr const char* SUBS[] = {"status",
                                                   "path",
                                                   "load",
                                                   "save",
                                                   "erase",
                                                   "clear",
                                                   "corrupt",
                                                   "format",
                                                   "keys",
                                                   "get",
                                                   "set",
                                                   "dump",
                                                   "paste"};
            for (const char* s : SUBS) {
                matchCount = fxe::AddUniqueMatchIfPrefix(
                    s, prefix, outMatches, matchCount, outMatchesCap);
            }
            return matchCount;
        }

        // settings get <key>
        // settings set <key> <value>
        if (argIndex == 2 && args.Has(1) && (args.Equals(1, "get") || args.Equals(1, "set"))) {
            return CollectSettingKeyMatches(this, prefix, outMatches, outMatchesCap);
        }

        return 0;
    };

    RegisterCommand("settings",
                    "Settings: status|path|load|save|erase|clear|corrupt|format|keys|get|set|dump|paste",
                    settingsCmd,
                    settingsComplete,
                    "Usage: settings <status|path|load|save|erase|clear|corrupt|keys|dump|paste> | "
                    "settings format <status|ERASE_BOARD_STORAGE> | settings get <key> | settings set <key> <value>");
}

} // namespace fxe
