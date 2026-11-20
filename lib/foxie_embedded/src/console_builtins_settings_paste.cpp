#include <ArduinoJson.h>

#include <cstdint>
#include <cstring>
#include <string_view>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/settings_store.hpp"

#include "settings_store_internal.hpp"

namespace fxe {

namespace {

enum class ApplyLineResult : uint8_t {
    Ignored = 0,
    Applied,
    ServiceApplied,
    Failed,
};

bool IsValidSettingKey(const std::string_view key) {
    static constexpr size_t MAX_KEY_LEN = 63;
    if (key.empty() || key.size() > MAX_KEY_LEN) {
        return false;
    }

    for (const char ch : key) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == 0 || c < 32 || c > 126 || c == '\\') {
            return false;
        }
    }

    return true;
}

void TrimWhitespace(char*& start, char*& end) {
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')) {
        ++start;
    }

    while (end > start) {
        const char c = *(end - 1);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        --end;
    }

    *end = '\0';
}

char* SkipPrefix(char* text, const char* prefix) {
    if (!text || !prefix) {
        return text;
    }

    const size_t len = std::strlen(prefix);
    return std::strncmp(text, prefix, len) == 0 ? text + len : text;
}

bool HasTrailingWhitespace(const char* start, const char* end) {
    if (!start || !end || end <= start) {
        return false;
    }

    const char* cursor = end;
    while (cursor > start) {
        const char c = *(cursor - 1);
        if (c == ' ' || c == '\t') {
            return true;
        }
        if (c != '\r' && c != '\n') {
            break;
        }
        --cursor;
    }
    return false;
}

void NormalizeMissingTrailingQuote(char* valueStart, char* valueEnd) {
    if (!valueStart || !valueEnd || valueEnd <= valueStart || valueStart[0] != '"') {
        return;
    }

    char* lastNonSpace = valueEnd;
    while (lastNonSpace > valueStart) {
        const char c = *(lastNonSpace - 1);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        --lastNonSpace;
    }

    if (lastNonSpace <= valueStart || *(lastNonSpace - 1) == '"' || lastNonSpace >= valueEnd) {
        return;
    }

    *lastNonSpace = '"';
    *(lastNonSpace + 1) = '\0';
}

void NormalizePasteValue(char*& valueStart) {
    char* valueEnd = valueStart + std::strlen(valueStart);
    if (HasTrailingWhitespace(valueStart, valueEnd)) {
        NormalizeMissingTrailingQuote(valueStart, valueEnd);
        valueEnd = valueStart + std::strlen(valueStart);
    }
    TrimWhitespace(valueStart, valueEnd);
}

bool ParseSettingsLine(char* line, char*& outKey, char*& outValue) {
    outKey = nullptr;
    outValue = nullptr;
    if (!line || line[0] == '\0') {
        return false;
    }

    char* start = line;
    char* end = line + std::strlen(line);
    TrimWhitespace(start, end);
    if (start[0] == '\0' || start[0] == '#' || (start[0] == '/' && start[1] == '/')) {
        return false;
    }

    start = SkipPrefix(start, "set ");
    start = SkipPrefix(start, "settings set ");
    char* equals = std::strchr(start, '=');
    if (equals) {
        *equals = '\0';
        char* keyEnd = equals;
        TrimWhitespace(start, keyEnd);
        char* valueStart = equals + 1;
        NormalizePasteValue(valueStart);
        if (start[0] == '\0') {
            return false;
        }
        outKey = start;
        outValue = valueStart;
        return true;
    }

    char* keyEnd = start;
    while (*keyEnd != '\0' && *keyEnd != ' ' && *keyEnd != '\t') {
        ++keyEnd;
    }
    if (start == keyEnd) {
        return false;
    }

    char* valueStart = keyEnd;
    while (*valueStart == ' ' || *valueStart == '\t') {
        ++valueStart;
    }
    NormalizePasteValue(valueStart);

    *keyEnd = '\0';
    outKey = start;
    outValue = valueStart;
    return true;
}

bool UnquoteJsonStringInPlace(char* value) {
    if (!value) {
        return false;
    }

    const size_t len = std::strlen(value);
    if (len < 2 || value[0] != '"' || value[len - 1] != '"') {
        return false;
    }

    char* write = value;
    const char* read = value + 1;
    const char* end = value + len - 1;
    while (read < end) {
        char c = *read++;
        if (c == '\\' && read < end) {
            c = *read++;
        }
        *write++ = c;
    }
    *write = '\0';
    return true;
}

bool RemoveExistingKey(JsonDocument& doc, const char* canonicalKey) {
    const char* existing = FindTopLevelObjectKeyCaseInsensitive(doc, canonicalKey);
    if (existing == nullptr) {
        return false;
    }
    doc.remove(existing);
    return true;
}

ApplyLineResult ApplySettingsPasteLine(Console& console,
                                       SettingsStore& settings,
                                       char* line,
                                       bool& outMustSave) {
    outMustSave = false;
    char* key = nullptr;
    char* value = nullptr;
    if (!ParseSettingsLine(line, key, value)) {
        return ApplyLineResult::Ignored;
    }

    if (!IsValidSettingKey(key ? std::string_view(key) : std::string_view())) {
        return ApplyLineResult::Failed;
    }

    char canonicalKey[64] = {};
    if (!CopyUppercaseSettingKey(key, canonicalKey, sizeof(canonicalKey))) {
        return ApplyLineResult::Failed;
    }
    outMustSave = console.ShouldForceSaveNowForSettingKey(canonicalKey);

    auto& doc = settings.Doc();
    if (value == nullptr || value[0] == '\0') {
        if (!console.CanAccessDiagnosticSetting(canonicalKey)) return ApplyLineResult::Failed;
        const bool isRegistered =
            console.HasSettingsDescriptorLookupCallback() &&
            console.LookupSettingDescriptor(canonicalKey) != nullptr;
        if (isRegistered) {
            if (!console.HasSettingsResetCallback()) {
                return ApplyLineResult::Failed;
            }

            const Console::SettingsSetResult reset =
                console.ResetSettingValue(settings, canonicalKey);
            return reset == Console::SettingsSetResult::Applied ||
                           reset == Console::SettingsSetResult::Noop
                       ? ApplyLineResult::ServiceApplied
                       : ApplyLineResult::Failed;
        }

        if (FindTopLevelObjectKeyCaseInsensitive(doc, canonicalKey) == nullptr) {
            return ApplyLineResult::Failed;
        }

        console.NotifyBeforeSettingsMutation(canonicalKey);
        return RemoveExistingKey(doc, canonicalKey)
                   ? ApplyLineResult::Applied
                   : ApplyLineResult::Failed;
    }

    const bool wasQuoted = UnquoteJsonStringInPlace(value);
    StaticJsonDocument<256> parsedDoc;
    StaticJsonDocument<256> rawDoc;
    const bool parsed = !wasQuoted && deserializeJson(parsedDoc, value) == DeserializationError::Ok;
    if (!parsed) {
        rawDoc.set(const_cast<char*>(value));
    }
    const JsonVariantConst setValue =
        parsed ? parsedDoc.as<JsonVariantConst>() : rawDoc.as<JsonVariantConst>();

    if (console.HasSettingsValidateAndSetCallback()) {
        const Console::SettingsSetResult validation =
            console.ValidateAndSetSettingValue(settings, canonicalKey, setValue);
        if (validation == Console::SettingsSetResult::Applied ||
            validation == Console::SettingsSetResult::Noop) {
            return ApplyLineResult::ServiceApplied;
        }
        if (validation != Console::SettingsSetResult::Ok) {
            return ApplyLineResult::Failed;
        }
    }

    console.NotifyBeforeSettingsMutation(canonicalKey);
    const char* existing =
        FindTopLevelObjectKeyCaseInsensitive(doc, canonicalKey);
    if (existing && std::strcmp(existing, canonicalKey) != 0) {
        (void)doc.remove(existing);
    }
    if (!console.HasSettingsValidateAndSetCallback() && parsed) {
        doc[canonicalKey] = parsedDoc.as<JsonVariant>();
    } else if (!console.HasSettingsValidateAndSetCallback()) {
        doc[canonicalKey].set(const_cast<char*>(value));
    }
    return ApplyLineResult::Applied;
}

} // namespace

bool Console::StartSettingsPaste(SettingsStore& settings) {
    auto* settingsPtr = &settings;
    const bool started = StartPasteMode(
        [this,
         settingsPtr,
         applied = size_t{0},
         failed = size_t{0},
         mustSave = false,
         needsBatchSave = false](const PasteEvent event, char* line, const size_t) mutable -> bool {
            if (event == PasteEvent::LineOverflow) {
                ++failed;
                return true;
            }

            if (event == PasteEvent::Line) {
                bool lineMustSave = false;
                const ApplyLineResult result = ApplySettingsPasteLine(
                    *this, *settingsPtr, line, lineMustSave);
                applied += result == ApplyLineResult::Applied ||
                                   result == ApplyLineResult::ServiceApplied
                               ? 1U
                               : 0U;
                failed += result == ApplyLineResult::Failed ? 1U : 0U;
                needsBatchSave = needsBatchSave ||
                                 result == ApplyLineResult::Applied;
                mustSave = mustSave ||
                           (result == ApplyLineResult::Applied && lineMustSave);
                return true;
            }

            PrintLn("paste: applied=%lu failed=%lu",
                    static_cast<unsigned long>(applied),
                    static_cast<unsigned long>(failed));
            if (applied == 0) {
                return false;
            }

            if (needsBatchSave) {
                NotifyBeforeSettingsMutation({});
            }
            if (needsBatchSave && mustSave) {
                settingsPtr->MarkMustSaveDirty();
                m_settingsPasteRebootDelayMs =
                    settingsPtr->GetMustSaveDelayMs();
            } else if (needsBatchSave) {
                settingsPtr->MarkDirty();
                m_settingsPasteRebootDelayMs =
                    settingsPtr->GetDeferredSaveDelayMs();
            } else {
                m_settingsPasteRebootDelayMs =
                    settingsPtr->GetDeferredSaveDelayMs();
            }
            m_settingsPasteRebootTime.Reset();
            m_settingsPasteRebootPending = true;
            PrintLn("OK (save queued; reboot deferred)");
            return true;
        });

    if (!started) {
        PrintLn("paste: already active");
        return false;
    }

    // A newly started batch supersedes any prior batch's delayed reboot. The
    // new Finish event will schedule one deadline for the combined live state.
    m_settingsPasteRebootPending = false;
    PrintLn("paste: enter settings lines, then press Ctrl+D");
    PrintLn("paste: formats supported: KEY=VALUE or set KEY=VALUE");
    return true;
}

} // namespace fxe
