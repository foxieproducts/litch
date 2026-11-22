#include "modules/control_mutation_service.hpp"
#include "modules/assistance_preferences.hpp"

#include <cstring>

#include "hardware/usb_cc_sense.hpp"
#include "modules/player_end_palette.hpp"
#include "modules/settings_registry.hpp"

namespace modules {
namespace {

constexpr size_t MAX_SETTING_BATCH_SIZE = 3U;
constexpr const char* USB_CC_KEYS[MAX_SETTING_BATCH_SIZE] = {
    "USB_CC_THRESH_500",
    "USB_CC_THRESH_1500",
    "USB_CC_THRESH_3000",
};
enum class SettingBatchMode : uint8_t {
    Write = 0,
    Reset,
};
ControlMutationReason RegistryReason(
    const settings_registry::ValidateAndSetResult result) {
    switch (result) {
        case settings_registry::ValidateAndSetResult::Ok:
            return ControlMutationReason::None;
        case settings_registry::ValidateAndSetResult::UnknownKey:
            return ControlMutationReason::UnknownKey;
        case settings_registry::ValidateAndSetResult::TypeMismatch:
            return ControlMutationReason::TypeMismatch;
        case settings_registry::ValidateAndSetResult::OutOfRange:
            return ControlMutationReason::OutOfRange;
        case settings_registry::ValidateAndSetResult::InvalidEnumValue:
            return ControlMutationReason::InvalidEnumValue;
    }
    return ControlMutationReason::BadRequest;
}
bool EqualsNoCase(const char* lhs, const char* rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    for (size_t index = 0U;; ++index) {
        char left = lhs[index];
        char right = rhs[index];
        if (left >= 'a' && left <= 'z') {
            left = static_cast<char>(left - ('a' - 'A'));
        }
        if (right >= 'a' && right <= 'z') {
            right = static_cast<char>(right - ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
        if (left == '\0') {
            return true;
        }
    }
}
const char* FindStoredKey(const JsonDocument& doc,
                          const char* canonicalKey,
                          const bool aliasOnly) {
    for (const JsonPairConst pair : doc.as<JsonObjectConst>()) {
        const char* const storedKey = pair.key().c_str();
        if (EqualsNoCase(storedKey, canonicalKey) &&
            (!aliasOnly || std::strcmp(storedKey, canonicalKey) != 0)) {
            return storedKey;
        }
    }
    return nullptr;
}
bool RemoveStoredKeys(JsonDocument& doc,
                      const char* canonicalKey,
                      const bool aliasOnly) {
    bool removed = false;
    while (true) {
        const char* const storedKey =
            FindStoredKey(doc, canonicalKey, aliasOnly);
        if (storedKey == nullptr) {
            return removed;
        }
        char keyCopy[64] = {};
        const size_t length = std::strlen(storedKey);
        if (length >= sizeof(keyCopy)) {
            return removed;
        }
        std::memcpy(keyCopy, storedKey, length + 1U);
        doc.remove(keyCopy);
        removed = true;
    }
}
bool HasLegacyFirstPlayerKeys(const JsonDocument& doc) {
    return doc.containsKey("GAME_P1_PLAYS_BLACK") ||
           doc.containsKey("p1_plays_black");
}
void RemoveLegacyFirstPlayerKeys(JsonDocument& doc) {
    (void)doc.remove("GAME_P1_PLAYS_BLACK");
    (void)doc.remove("p1_plays_black");
}
bool IsStartOnlySetting(const char* key) {
    return std::strcmp(key, "BOARD_ROTATE") == 0 ||
           std::strcmp(key, "GAME_FIRST_PLAYER") == 0 ||
           std::strcmp(key, "TIMER") == 0;
}
bool IsCpuSetting(const char* key) {
    return std::strcmp(key, "VS_CPU_LEVEL") == 0 ||
           std::strcmp(key, "VS_CPU_PLAYER") == 0;
}
bool IsImmediateProjectionSetting(const char* key) {
    return std::strcmp(key, "VS_CPU_LEVEL") == 0 ||
           std::strcmp(key, "BRIGHTNESS") == 0 ||
           std::strcmp(key, "WHITE_SQUARES_ON") == 0 ||
           std::strcmp(key, "CHECKMATE_ANIM_STYLE") == 0 ||
           std::strcmp(key, "SETTLE_ANIM_STYLE") == 0 ||
           std::strcmp(key, "MINIMAL_ANIMATIONS") == 0 ||
           std::strcmp(key, "BOARD_ROTATE") == 0 ||
           std::strcmp(key, "GAME_FIRST_PLAYER") == 0;
}
bool IsUsbCcSetting(const char* key) {
    return std::strcmp(key, USB_CC_KEYS[0]) == 0 ||
           std::strcmp(key, USB_CC_KEYS[1]) == 0 ||
           std::strcmp(key, USB_CC_KEYS[2]) == 0;
}

ControlMutationPersistenceTier PersistenceTierFor(
    const fxe::SettingDescriptor& descriptor) {
    if (!descriptor.IsPersisted()) {
        return ControlMutationPersistenceTier::None;
    }
    return descriptor.IsForceSave()
               ? ControlMutationPersistenceTier::MustSave
               : ControlMutationPersistenceTier::Ordinary;
}

ControlMutationPersistenceTier HighestTier(
    const ControlMutationPersistenceTier left,
    const ControlMutationPersistenceTier right) {
    return static_cast<uint8_t>(left) >= static_cast<uint8_t>(right)
               ? left
               : right;
}

bool HasWriteCapacity(
    const SettingsStore& settings,
    const settings_registry::PreparedSettingMutation* prepared,
    const size_t count,
    const SettingBatchMode mode) {
    const JsonDocument& doc = settings.Doc();
    if (doc.overflowed() || doc.memoryUsage() > doc.capacity()) {
        return false;
    }
    size_t missingKeys = 0U;
    size_t stringBytes = 0U;
    for (size_t index = 0U; index < count; ++index) {
        const fxe::SettingDescriptor& descriptor = *prepared[index].descriptor;
        const char* const storageKey = prepared[index].storageKey != nullptr
                                           ? prepared[index].storageKey
                                           : descriptor.key;
        const bool paletteSetting =
            modules::player_end_palette::IsLegacyLogicalKey(descriptor.key) ||
            modules::player_end_palette::IsPhysicalEndKey(descriptor.key);
        if ((mode == SettingBatchMode::Write || paletteSetting) &&
            !doc.containsKey(storageKey)) {
            ++missingKeys;
        }
        if (std::strcmp(descriptor.key, "BRIGHTNESS") == 0 &&
            prepared[index].uintValue == 0U && !doc.containsKey("WHITE_SQUARES_ON")) {
            ++missingKeys;
        }
        if (paletteSetting ||
            std::strcmp(descriptor.key, "GAME_FIRST_PLAYER") == 0) {
            missingKeys += doc.containsKey("P1_PIECE_WHEEL") ? 0U : 1U;
            missingKeys += doc.containsKey("P2_PIECE_WHEEL") ? 0U : 1U;
        }
        if (mode == SettingBatchMode::Write && prepared[index].changed &&
            (descriptor.type == fxe::SettingType::String ||
             descriptor.type == fxe::SettingType::Enum) &&
            prepared[index].stringValue != nullptr) {
            stringBytes += std::strlen(prepared[index].stringValue) + 1U;
        }
    }
    const size_t objectBytes =
        missingKeys == 0U ? 0U : JSON_OBJECT_SIZE(missingKeys);
    return objectBytes + stringBytes <= doc.capacity() - doc.memoryUsage();
}

ControlMutationOutcome ApplySettingBatch(
    SettingsStore* const settings,
    const ControlMutationCallbacks& callbacks,
    settings_registry::PreparedSettingMutation* const prepared,
    const size_t count,
    const SettingBatchMode mode,
    const uint16_t requestId) {
    ControlMutationOutcome outcome;
    outcome.requestId = requestId;
    if (settings == nullptr || prepared == nullptr || count == 0U ||
        count > MAX_SETTING_BATCH_SIZE) {
        return outcome;
    }

    bool documentChanged = false;
    bool cancelCpu = false;
    bool projectSetting[MAX_SETTING_BATCH_SIZE] = {};
    for (size_t index = 0U; index < count; ++index) {
        const fxe::SettingDescriptor* const descriptor =
            prepared[index].descriptor;
        if (descriptor == nullptr) {
            return outcome;
        }
        const UiOwner automaticOwner =
            assistance_preferences::AutomaticHintsOwner(descriptor->key);
        if (automaticOwner != UiOwner::None &&
            assistance_preferences::CpuOwns(*settings, automaticOwner)) {
            outcome.reason = ControlMutationReason::EngineRejected;
            return outcome;
        }
        const char* const storageKey = prepared[index].storageKey != nullptr
                                           ? prepared[index].storageKey
                                           : descriptor->key;
        const bool hasStoredKey =
            FindStoredKey(settings->Doc(), storageKey, false) != nullptr;
        const bool hasAlias =
            FindStoredKey(settings->Doc(), descriptor->key, true) != nullptr;
        const bool legacyCleanup =
            std::strcmp(descriptor->key, "GAME_FIRST_PLAYER") == 0 &&
            HasLegacyFirstPlayerKeys(settings->Doc());
        documentChanged = documentChanged || prepared[index].changed ||
                          legacyCleanup ||
                          (mode == SettingBatchMode::Reset ? hasStoredKey
                                                           : hasAlias);
        cancelCpu = cancelCpu ||
                    (prepared[index].changed && IsCpuSetting(descriptor->key));
        projectSetting[index] =
            (prepared[index].changed && IsImmediateProjectionSetting(descriptor->key)) ||
            (mode == SettingBatchMode::Write &&
             std::strcmp(descriptor->key, "SETTLE_ANIM_STYLE") == 0);
        outcome.persistenceTier = HighestTier(
            outcome.persistenceTier, PersistenceTierFor(*descriptor));
    }
    outcome.settingsRevision = settings->GetRevision();
    if (!documentChanged) {
        // An intentional fresh placement-style write is also a preview action.
        // Reads, reset cleanup, failed validation and ordered receipt replay do
        // not enter this accepted-write projection seam.
        if (callbacks.settingProjected != nullptr) {
            for (size_t index = 0U; index < count; ++index) {
                if (projectSetting[index]) callbacks.settingProjected(
                    callbacks.context, prepared[index].descriptor->key);
            }
        }
        outcome.status = ControlMutationStatus::Noop;
        outcome.reason = ControlMutationReason::None;
        return outcome;
    }
    if (outcome.persistenceTier != ControlMutationPersistenceTier::None &&
        settings->IsDisabled()) {
        for (size_t index = 0U; index < count; ++index) {
            const auto& descriptor = *prepared[index].descriptor;
            if (!callbacks.allowVolatileUserSettings ||
                !settings_registry::IsDirectAppAccessible(descriptor) ||
                IsUsbCcSetting(descriptor.key) ||
                std::strcmp(descriptor.key, "RGBW_COOL_TO_WARM_THRESHOLD") == 0) {
                outcome.reason = ControlMutationReason::StorageUnavailable;
                return outcome;
            }
        }
        outcome.persistenceTier = ControlMutationPersistenceTier::None;
    }
    if (!HasWriteCapacity(*settings, prepared, count, mode)) {
        outcome.reason = ControlMutationReason::StorageUnavailable;
        return outcome;
    }
    for (size_t index = 0U; index < count; ++index) {
        const char* const canonicalKey = prepared[index].descriptor->key;
        if (IsStartOnlySetting(canonicalKey)) {
            if (callbacks.checkStartOnlyGate == nullptr) {
                outcome.reason = ControlMutationReason::Unsupported;
                return outcome;
            }
            const ControlCoordinatorResult gate =
                callbacks.checkStartOnlyGate(callbacks.context,
                                             canonicalKey);
            if (gate != ControlCoordinatorResult::Applied) {
                outcome.reason =
                    ControlMutationReasonFromCoordinatorResult(gate);
                return outcome;
            }
        }
    }

    if (callbacks.beforeSettingMutation != nullptr) {
        callbacks.beforeSettingMutation(
            callbacks.context, prepared[0].descriptor->key);
    }
    for (size_t index = 0U; index < count; ++index) {
        const char* const canonicalKey = prepared[index].descriptor->key;
        if (mode == SettingBatchMode::Reset) {
            if (modules::player_end_palette::IsLegacyLogicalKey(canonicalKey) ||
                modules::player_end_palette::IsPhysicalEndKey(canonicalKey)) {
                settings_registry::ApplyPreparedMutation(*settings,
                                                         prepared[index]);
            } else {
                const char* const storageKey =
                    prepared[index].storageKey != nullptr
                        ? prepared[index].storageKey
                        : canonicalKey;
                (void)RemoveStoredKeys(settings->Doc(), storageKey, false);
                if (std::strcmp(canonicalKey, "GAME_FIRST_PLAYER") == 0) {
                    modules::player_end_palette::SynchronizeLegacyAliases(
                        settings->Doc());
                }
            }
        } else {
            (void)RemoveStoredKeys(settings->Doc(), canonicalKey, true);
            settings_registry::ApplyPreparedMutation(*settings,
                                                     prepared[index]);
        }
        if (std::strcmp(canonicalKey, "GAME_FIRST_PLAYER") == 0) {
            RemoveLegacyFirstPlayerKeys(settings->Doc());
        }
    }
    if (cancelCpu && callbacks.cancelCpuWork != nullptr) {
        callbacks.cancelCpuWork(callbacks.context);
    }
    if (callbacks.settingProjected != nullptr) {
        for (size_t index = 0U; index < count; ++index) {
            if (projectSetting[index]) {
                callbacks.settingProjected(
                    callbacks.context, prepared[index].descriptor->key);
            }
        }
    }

    switch (outcome.persistenceTier) {
        case ControlMutationPersistenceTier::MustSave:
            settings->MarkMustSaveDirty();
            break;
        case ControlMutationPersistenceTier::Ordinary:
            settings->MarkDirty();
            break;
        case ControlMutationPersistenceTier::None:
            settings->NotifyDocChanged();
            break;
    }
    outcome.status = ControlMutationStatus::Applied;
    outcome.reason = ControlMutationReason::None;
    outcome.settingsRevision = settings->GetRevision();
    if (callbacks.coachingChanged != nullptr) {
        for (size_t index = 0U; index < count; ++index) {
            const char* key = prepared[index].descriptor->key;
            const UiOwner owner = assistance_preferences::AutomaticHintsOwner(key);
            if (owner != UiOwner::None && prepared[index].changed) {
                callbacks.coachingChanged(callbacks.context, owner,
                    settings->ReadValue(key, false));
            }
        }
    }
    return outcome;
}

const fxe::SettingDescriptor* LookupDescriptorNoCase(const char* key) {
    size_t count = 0U;
    const fxe::SettingDescriptor* const descriptors =
        settings_registry::EnumerateAll(count);
    for (size_t index = 0U; descriptors != nullptr && index < count;
         ++index) {
        if (EqualsNoCase(key, descriptors[index].key)) {
            return &descriptors[index];
        }
    }
    return nullptr;
}

settings_registry::ValidateAndSetResult PrepareDefaultMutation(
    const SettingsStore& settings,
    const fxe::SettingDescriptor& descriptor,
    settings_registry::PreparedSettingMutation& outPrepared) {
    outPrepared = settings_registry::PreparedSettingMutation{};
    outPrepared.descriptor = &descriptor;
    outPrepared.storageKey =
        modules::player_end_palette::ResolvePhysicalStorageKey(
            settings, descriptor.key);
    switch (descriptor.type) {
        case fxe::SettingType::UInt32: {
            StaticJsonDocument<16> defaultValue;
            defaultValue.set(descriptor.defaultUint);
            return settings_registry::PrepareMutation(
                settings,
                descriptor.key,
                defaultValue.as<JsonVariantConst>(),
                outPrepared);
        }
        case fxe::SettingType::Bool:
            outPrepared.boolValue = descriptor.defaultUint != 0U;
            outPrepared.changed = settings.ReadValue(
                                      descriptor.key,
                                      outPrepared.boolValue) !=
                                  outPrepared.boolValue;
            return settings_registry::ValidateAndSetResult::Ok;
        case fxe::SettingType::String:
        case fxe::SettingType::Enum: {
            const char* const defaultValue =
                descriptor.defaultStr != nullptr ? descriptor.defaultStr : "";
            const char* const currentValue =
                settings.ReadValue(descriptor.key, defaultValue);
            outPrepared.stringValue = defaultValue;
            outPrepared.changed = currentValue == nullptr ||
                                  std::strcmp(currentValue, defaultValue) != 0;
            return settings_registry::ValidateAndSetResult::Ok;
        }
    }
    return settings_registry::ValidateAndSetResult::TypeMismatch;
}

bool ValidateUsbCcCandidate(
    const SettingsStore& settings,
    const settings_registry::PreparedSettingMutation* prepared,
    const size_t count) {
    const hardware::UsbCcThresholds defaults =
        hardware::GetDefaultUsbCcThresholds();
    uint32_t candidate[MAX_SETTING_BATCH_SIZE] = {
        settings.ReadValue(
            USB_CC_KEYS[0], static_cast<uint32_t>(defaults.level500)),
        settings.ReadValue(
            USB_CC_KEYS[1], static_cast<uint32_t>(defaults.level1500)),
        settings.ReadValue(
            USB_CC_KEYS[2], static_cast<uint32_t>(defaults.level3000)),
    };
    for (size_t index = 0U; index < count; ++index) {
        const char* const key = prepared[index].descriptor->key;
        if (std::strcmp(key, USB_CC_KEYS[0]) == 0) {
            candidate[0] = prepared[index].uintValue;
        } else if (std::strcmp(key, USB_CC_KEYS[1]) == 0) {
            candidate[1] = prepared[index].uintValue;
        } else if (std::strcmp(key, USB_CC_KEYS[2]) == 0) {
            candidate[2] = prepared[index].uintValue;
        }
    }
    if (candidate[0] > UINT16_MAX || candidate[1] > UINT16_MAX ||
        candidate[2] > UINT16_MAX) {
        return false;
    }
    return hardware::AreUsbCcThresholdsValid({
        static_cast<uint16_t>(candidate[0]),
        static_cast<uint16_t>(candidate[1]),
        static_cast<uint16_t>(candidate[2]),
    });
}

ControlMutationOutcome PrepareFailure(
    SettingsStore* const settings,
    const uint16_t requestId,
    const settings_registry::ValidateAndSetResult result) {
    ControlMutationOutcome outcome;
    outcome.requestId = requestId;
    outcome.reason = RegistryReason(result);
    outcome.settingsRevision = settings != nullptr
                                   ? settings->GetRevision()
                                   : 0U;
    return outcome;
}

} // namespace

ControlMutationOutcome ControlMutationService::MutateSetting(
    const char* key,
    const JsonVariantConst value,
    const uint16_t requestId) {
    const auto action = m_supportActions != nullptr && !m_recordingAction
        ? m_supportActions->Begin(trace::SupportActionKind::SettingChange) : 0U;
    const auto outcome = MutateSettingInternal(key, value, requestId);
    RecordSupportOutcome(action, outcome);
    return outcome;
}

ControlMutationOutcome ControlMutationService::MutateSettingInternal(
    const char* key, const JsonVariantConst value, const uint16_t requestId) {
    if (m_settings == nullptr) {
        ControlMutationOutcome outcome{requestId};
        outcome.reason = ControlMutationReason::StorageUnavailable;
        return outcome;
    }
    if (key == nullptr || key[0] == '\0') {
        return ControlMutationOutcome{requestId};
    }
    settings_registry::PreparedSettingMutation prepared;
    const settings_registry::ValidateAndSetResult result =
        settings_registry::PrepareMutation(*m_settings, key, value, prepared);
    if (result != settings_registry::ValidateAndSetResult::Ok ||
        prepared.descriptor == nullptr) {
        return PrepareFailure(m_settings, requestId, result);
    }
    if (IsUsbCcSetting(prepared.descriptor->key) &&
        !ValidateUsbCcCandidate(*m_settings, &prepared, 1U)) {
        return PrepareFailure(
            m_settings, requestId,
            settings_registry::ValidateAndSetResult::OutOfRange);
    }
    return ApplySettingBatch(m_settings, m_callbacks, &prepared, 1U,
                             SettingBatchMode::Write, requestId);
}

ControlMutationOutcome ControlMutationService::ResetSetting(
    const char* key,
    const uint16_t requestId) {
    if (m_settings == nullptr) {
        ControlMutationOutcome outcome{requestId};
        outcome.reason = ControlMutationReason::StorageUnavailable;
        return outcome;
    }
    if (key == nullptr || key[0] == '\0') {
        return ControlMutationOutcome{requestId};
    }
    const fxe::SettingDescriptor* const descriptor =
        LookupDescriptorNoCase(key);
    if (descriptor == nullptr) {
        return PrepareFailure(
            m_settings, requestId,
            settings_registry::ValidateAndSetResult::UnknownKey);
    }
    settings_registry::PreparedSettingMutation prepared;
    const settings_registry::ValidateAndSetResult result =
        PrepareDefaultMutation(*m_settings, *descriptor, prepared);
    if (result != settings_registry::ValidateAndSetResult::Ok) {
        return PrepareFailure(m_settings, requestId, result);
    }
    if (IsUsbCcSetting(descriptor->key) &&
        !ValidateUsbCcCandidate(*m_settings, &prepared, 1U)) {
        return PrepareFailure(
            m_settings, requestId,
            settings_registry::ValidateAndSetResult::OutOfRange);
    }
    return ApplySettingBatch(m_settings, m_callbacks, &prepared, 1U,
                             SettingBatchMode::Reset, requestId);
}

ControlMutationOutcome ControlMutationService::MutateUsbCcThresholds(
    const uint16_t threshold500,
    const uint16_t threshold1500,
    const uint16_t threshold3000,
    const uint16_t requestId) {
    if (m_settings == nullptr) {
        ControlMutationOutcome outcome{requestId};
        outcome.reason = ControlMutationReason::StorageUnavailable;
        return outcome;
    }
    const uint16_t values[MAX_SETTING_BATCH_SIZE] = {
        threshold500, threshold1500, threshold3000};
    StaticJsonDocument<32> documents[MAX_SETTING_BATCH_SIZE];
    settings_registry::PreparedSettingMutation prepared[MAX_SETTING_BATCH_SIZE];
    for (size_t index = 0U; index < MAX_SETTING_BATCH_SIZE; ++index) {
        documents[index].set(values[index]);
        const settings_registry::ValidateAndSetResult result =
            settings_registry::PrepareMutation(
                *m_settings, USB_CC_KEYS[index],
                documents[index].as<JsonVariantConst>(), prepared[index]);
        if (result != settings_registry::ValidateAndSetResult::Ok) {
            return PrepareFailure(m_settings, requestId, result);
        }
    }
    if (!ValidateUsbCcCandidate(
            *m_settings, prepared, MAX_SETTING_BATCH_SIZE)) {
        return PrepareFailure(
            m_settings, requestId,
            settings_registry::ValidateAndSetResult::OutOfRange);
    }
    return ApplySettingBatch(
        m_settings, m_callbacks, prepared, MAX_SETTING_BATCH_SIZE,
        SettingBatchMode::Write, requestId);
}

ControlMutationOutcome ControlMutationService::ResetUsbCcThresholds(
    const uint16_t requestId) {
    if (m_settings == nullptr) {
        ControlMutationOutcome outcome{requestId};
        outcome.reason = ControlMutationReason::StorageUnavailable;
        return outcome;
    }
    settings_registry::PreparedSettingMutation prepared[MAX_SETTING_BATCH_SIZE];
    for (size_t index = 0U; index < MAX_SETTING_BATCH_SIZE; ++index) {
        const fxe::SettingDescriptor* const descriptor =
            settings_registry::LookupByKey(USB_CC_KEYS[index]);
        if (descriptor == nullptr) {
            return PrepareFailure(
                m_settings, requestId,
                settings_registry::ValidateAndSetResult::UnknownKey);
        }
        const settings_registry::ValidateAndSetResult result =
            PrepareDefaultMutation(*m_settings, *descriptor, prepared[index]);
        if (result != settings_registry::ValidateAndSetResult::Ok) {
            return PrepareFailure(m_settings, requestId, result);
        }
    }
    if (!ValidateUsbCcCandidate(
            *m_settings, prepared, MAX_SETTING_BATCH_SIZE)) {
        return PrepareFailure(
            m_settings, requestId,
            settings_registry::ValidateAndSetResult::OutOfRange);
    }
    return ApplySettingBatch(
        m_settings, m_callbacks, prepared, MAX_SETTING_BATCH_SIZE,
        SettingBatchMode::Reset, requestId);
}

} // namespace modules
