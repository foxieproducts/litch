#ifdef ARDUINO

#include <cctype>

#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"

#include "modules/settings_registry.hpp"
#include "modules/control_mutation_service.hpp"

namespace firmware_internal {
namespace {

fxe::Console::SettingsSetResult SettingsSetResultFromMutationOutcome(
    const modules::ControlMutationOutcome& outcome) {
    if (outcome.status == modules::ControlMutationStatus::Applied) {
        return fxe::Console::SettingsSetResult::Applied;
    }
    if (outcome.status == modules::ControlMutationStatus::Noop) {
        return fxe::Console::SettingsSetResult::Noop;
    }

    switch (outcome.reason) {
        case modules::ControlMutationReason::UnknownKey:
            return fxe::Console::SettingsSetResult::UnknownKey;
        case modules::ControlMutationReason::TypeMismatch:
            return fxe::Console::SettingsSetResult::TypeMismatch;
        case modules::ControlMutationReason::OutOfRange:
            return fxe::Console::SettingsSetResult::OutOfRange;
        case modules::ControlMutationReason::InvalidEnumValue:
            return fxe::Console::SettingsSetResult::InvalidEnumValue;
        case modules::ControlMutationReason::NotAtStart:
            return fxe::Console::SettingsSetResult::NotAtStart;
        case modules::ControlMutationReason::Busy:
            return fxe::Console::SettingsSetResult::Busy;
        case modules::ControlMutationReason::StorageUnavailable:
            return fxe::Console::SettingsSetResult::StorageUnavailable;
        default:
            return fxe::Console::SettingsSetResult::TypeMismatch;
    }
}

bool CopySettingKey(const std::string_view key, char (&outKey)[64]) {
    if (key.empty() || key.size() >= sizeof(outKey)) {
        return false;
    }

    for (size_t i = 0; i < key.size(); ++i) {
        outKey[i] = key[i];
    }
    outKey[key.size()] = '\0';
    return true;
}

} // namespace

size_t SettingsKeyEnumCallback(const char* prefix,
                               const char** outMatches,
                               const size_t outMatchesCap) {
    size_t matchCount = 0;

    size_t descriptorCount = 0;
    const fxe::SettingDescriptor* descriptors = modules::settings_registry::EnumerateAll(descriptorCount);
    for (size_t i = 0; i < descriptorCount; ++i) {
        const fxe::SettingDescriptor& descriptor = descriptors[i];
        if (!descriptor.key || descriptor.key[0] == '\0') {
            continue;
        }

        bool hasLowercase = false;
        for (size_t j = 0; descriptor.key[j] != '\0'; ++j) {
            if (std::islower(static_cast<unsigned char>(descriptor.key[j])) != 0) {
                hasLowercase = true;
                break;
            }
        }
        if (hasLowercase) {
            continue;
        }

        matchCount = console_commands::AddMatchIfPrefix(descriptor.key,
                                                        prefix,
                                                        outMatches,
                                                        matchCount,
                                                        outMatchesCap);
    }

    return matchCount;
}

bool SettingsWritePolicyCallback(const std::string_view key, bool* outForceSaveNow) {
    if (!outForceSaveNow) {
        return false;
    }

    if (key == LED_BROWNOUT_LIMIT_LATCH_SETTING_KEY) {
        *outForceSaveNow = true;
        return true;
    }

    const fxe::SettingDescriptor* descriptor = SettingsDescriptorLookupCallback(key);
    if (!descriptor) {
        return false;
    }

    *outForceSaveNow = descriptor->IsForceSave();
    return true;
}

void BeforeSettingsMutationCallback(const std::string_view key) {
    auto& sys = GetSystem();
    CancelBootPresentation(sys);
    if (key.empty() || key == LED_BROWNOUT_LIMIT_LATCH_SETTING_KEY) {
        // The internal recovery latch is deliberately absent from the public
        // settings registry, but raw paste removals can still target it.
        sys.settings.ArmPersistenceGuard();
        ResetUsbCcSourceAuthorization(sys);
        return;
    }

    const fxe::SettingDescriptor* descriptor = SettingsDescriptorLookupCallback(key);
    if (!descriptor || !descriptor->key) {
        return;
    }

    const std::string_view canonicalKey(descriptor->key);
    if (canonicalKey == "USB_CC_THRESH_500" ||
        canonicalKey == "USB_CC_THRESH_1500" ||
        canonicalKey == "USB_CC_THRESH_3000") {
        sys.settings.ArmPersistenceGuard();
        ResetUsbCcSourceAuthorization(sys);
    }
}

const fxe::SettingDescriptor* SettingsDescriptorLookupCallback(const std::string_view key) {
    if (key.empty() || key.size() >= 64) {
        return nullptr;
    }

    char keyBuffer[64] = {};
    for (size_t i = 0; i < key.size(); ++i) {
        keyBuffer[i] = key[i];
    }
    keyBuffer[key.size()] = '\0';

    const fxe::SettingDescriptor* descriptor = modules::settings_registry::LookupByKey(keyBuffer);
    if (descriptor != nullptr) {
        return descriptor;
    }

    for (size_t i = 0; i < key.size(); ++i) {
        const char c = keyBuffer[i];
        if (c >= 'a' && c <= 'z') {
            keyBuffer[i] = static_cast<char>(c - ('a' - 'A'));
        }
    }

    return modules::settings_registry::LookupByKey(keyBuffer);
}

fxe::Console::SettingsSetResult SettingsValidateAndSetCallback(fxe::SettingsStore& settings,
                                                               const std::string_view key,
                                                               const JsonVariantConst value) {
    char keyBuffer[64] = {};
    if (!CopySettingKey(key, keyBuffer)) {
        return fxe::Console::SettingsSetResult::UnknownKey;
    }

    auto& sys = GetSystem();
    if (&settings != &sys.settings || !sys.controlMutation.IsConfigured()) {
        // The console is intentionally available during boot recovery, before
        // gameplay mutation policy is configured. Preserve that diagnostic and
        // read-only surface, but never let a typed setting write bypass the
        // canonical service in a partially initialized or miswired process.
        return fxe::Console::SettingsSetResult::StorageUnavailable;
    }

    const modules::ControlMutationOutcome outcome =
        sys.controlMutation.MutateSetting(keyBuffer, value);
    return SettingsSetResultFromMutationOutcome(outcome);
}

fxe::Console::SettingsSetResult SettingsResetCallback(
    fxe::SettingsStore& settings,
    const std::string_view key) {
    char keyBuffer[64] = {};
    if (!CopySettingKey(key, keyBuffer)) {
        return fxe::Console::SettingsSetResult::UnknownKey;
    }

    auto& sys = GetSystem();
    if (&settings != &sys.settings || !sys.controlMutation.IsConfigured()) {
        return fxe::Console::SettingsSetResult::StorageUnavailable;
    }

    return SettingsSetResultFromMutationOutcome(
        sys.controlMutation.ResetSetting(keyBuffer));
}

} // namespace firmware_internal

#endif
