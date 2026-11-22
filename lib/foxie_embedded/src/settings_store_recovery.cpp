#include "settings_store_internal.hpp"

#include <cstring>

#include "foxie_embedded/logger.hpp"
#include "foxie_embedded/settings_store.hpp"

namespace fxe {

bool ProcessSettingsDocument(JsonDocument& recovery,
                             const SettingsStoreConfig& config,
                             const char* path, SettingsRemovedKeys* removed) {
    SettingsScrubStats stats;
    const bool scrubbed = ScrubBadPrintableStrings(recovery, &stats, removed);
    if (scrubbed && (stats.removedObjectKeys != 0 || stats.nulledArrayElements != 0)) {
        fxe::Warn("settings: scrubbed document (%s): removed_keys=%u nulled_array=%u",
                  path,
                  static_cast<unsigned>(stats.removedObjectKeys),
                  static_cast<unsigned>(stats.nulledArrayElements));
    }
    bool mutated = scrubbed;
    if (config.migrateFunc) mutated = config.migrateFunc(recovery) || mutated;
    // Migration must still see original legacy spellings. Validators use
    // canonical keys, so normalize before scrub rather than letting a
    // lowercase corrupt record block its validated alternate replacement.
    mutated = NormalizeTopLevelObjectKeysUppercase(recovery) || mutated;
    if (config.scrubFunc) mutated = config.scrubFunc(recovery, removed) || mutated;
    return NormalizeTopLevelObjectKeysUppercase(recovery) || mutated;
}

namespace {

static constexpr uint8_t MAX_COPY_DEPTH = 10;

bool CopyJsonObjectOwned(JsonVariant destination,
                         const JsonObjectConst source,
                         const uint8_t depth) {
    JsonObject output = destination.to<JsonObject>();
    if (output.isNull()) {
        return false;
    }
    for (const JsonPairConst pair : source) {
        const JsonString key = pair.key();
        if (key.isNull()) {
            return false;
        }
        JsonVariant child =
            output[const_cast<char*>(key.c_str())].to<JsonVariant>();
        if (!CopyJsonValueOwned(child, pair.value(), static_cast<uint8_t>(depth + 1U))) {
            return false;
        }
    }
    return true;
}

bool CopyJsonArrayOwned(JsonVariant destination,
                        const JsonArrayConst source,
                        const uint8_t depth) {
    JsonArray output = destination.to<JsonArray>();
    if (output.isNull()) {
        return false;
    }
    for (const JsonVariantConst value : source) {
        if (!CopyJsonValueOwned(
                output.add(), value, static_cast<uint8_t>(depth + 1U))) {
            return false;
        }
    }
    return true;
}

bool CopyJsonPrimitiveOwned(JsonVariant destination,
                            const JsonVariantConst source) {
    if (source.is<JsonString>()) {
        const JsonString value = source.as<JsonString>();
        return !value.isNull() && destination.set(const_cast<char*>(value.c_str()));
    }
    if (source.is<bool>()) {
        return destination.set(source.as<bool>());
    }
    if (source.is<uint64_t>()) {
        return destination.set(source.as<uint64_t>());
    }
    if (source.is<int64_t>()) {
        return destination.set(source.as<int64_t>());
    }
    if (source.is<double>()) {
        return destination.set(source.as<double>());
    }
    return source.isNull() && destination.set(nullptr);
}

enum class CopyMissingResult : uint8_t {
    Skipped,
    Added,
    Failed,
};

CopyMissingResult CopyMissingSetting(JsonObject primary, JsonObjectConst eligibleKeys, const JsonPairConst pair) {
    const char* key = pair.key().c_str();
    if (key == nullptr || key[0] == '\0' ||
        FindTopLevelObjectKeyCaseInsensitive(primary, key) != nullptr) {
        return CopyMissingResult::Skipped;
    }

    char targetKey[64] = {};
    if (!CopyUppercaseSettingKey(key, targetKey, sizeof(targetKey))) {
        const size_t keyLen = std::strlen(key);
        if (keyLen == 0 || keyLen >= sizeof(targetKey)) {
            return CopyMissingResult::Skipped;
        }
        std::memcpy(targetKey, key, keyLen + 1U);
    }

    if (!eligibleKeys.containsKey(targetKey)) return CopyMissingResult::Skipped;
    JsonVariant target = primary[targetKey].to<JsonVariant>();
    if (CopyJsonValueOwned(target, pair.value(), 0U)) {
        return CopyMissingResult::Added;
    }

    // Remove the partially constructed value before the caller aborts recovery.
    (void)primary.remove(targetKey);
    return CopyMissingResult::Failed;
}

} // namespace

bool CopyJsonValueOwned(JsonVariant destination,
                        const JsonVariantConst source,
                        const uint8_t depth) {
    if (depth > MAX_COPY_DEPTH) {
        return false;
    }

    if (source.is<JsonObjectConst>()) {
        return CopyJsonObjectOwned(destination, source.as<JsonObjectConst>(), depth);
    }

    if (source.is<JsonArrayConst>()) {
        return CopyJsonArrayOwned(destination, source.as<JsonArrayConst>(), depth);
    }
    return CopyJsonPrimitiveOwned(destination, source);
}

bool SettingsRemovedKeys::Record(const JsonString& key) {
    if (!m_enabled || m_failed) return !m_failed;
    const char* bytes = key.c_str();
    const size_t size = key.size();
    // An invalid name cannot safely identify any alternate setting.
    if (bytes == nullptr || size == 0U) return true;
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c < 32U || c > 126U || c == '\\') return true;
    }
    if (size >= 64U) return true;
    if (!m_scratchInitialized) {
#if !defined(ARDUINO)
        // Native-only callers may retain the standalone library convenience.
        if (m_scratch == nullptr) {
            m_nativeScratch.emplace(SETTINGS_STORE_JSON_CAPACITY);
            m_scratch = &*m_nativeScratch;
        }
#endif
        if (m_scratch == nullptr || m_scratch->capacity() < SETTINGS_STORE_JSON_CAPACITY) {
            m_failed = true; return false;
        }
        m_scratch->clear();
        auto root = m_scratch->to<JsonArray>();
        root.add(nullptr);
        root.createNestedObject();
        m_scratchInitialized = true;
    }
    JsonObject keys = (*m_scratch)[1U].as<JsonObject>();
    if (FindTopLevelObjectKeyCaseInsensitive(keys, bytes) != nullptr) return true;
    // Mutable input forces an owned key copy. No selected payload is copied
    // until recording finishes, so canonicalization cannot alter user values.
    const bool copied = keys[const_cast<char*>(bytes)].set(true);
    const char* owned = FindTopLevelObjectKeyCaseInsensitive(keys, bytes);
    if (!copied || owned == nullptr || m_scratch->overflowed()) {
        m_failed = true; return false;
    }
    char* canonical = const_cast<char*>(owned);
    for (size_t i = 0; i < size; ++i)
        if (canonical[i] >= 'a' && canonical[i] <= 'z') canonical[i] -= 'a' - 'A';
    return true;
}

bool MergeMissingSettingsFromPath(const char* path,
                                  JsonObject primary, JsonObjectConst eligibleKeys,
                                  const SettingsStoreConfig& config,
                                  JsonDocument& recovery,
                                  JsonLoadResult& outFailure,
                                  const SettingsReadAccess access) {
    outFailure = JsonLoadResult::Ok;
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    recovery.clear();
    const JsonLoadResult result = LoadJsonFromPath(path, recovery, access);
    if (result != JsonLoadResult::Ok) {
        if (result == JsonLoadResult::OpenError || result == JsonLoadResult::ReadError ||
            result == JsonLoadResult::NoMemory || result == JsonLoadResult::TooLarge) {
            outFailure = result;
        }
        return false;
    }

    (void)ProcessSettingsDocument(recovery, config, path);
    if (recovery.overflowed()) {
        outFailure = JsonLoadResult::NoMemory;
        return false;
    }
    if (primary.isNull() || !recovery.is<JsonObjectConst>()) {
        return false;
    }

    bool mergedAny = false;
    for (const JsonPairConst pair : recovery.as<JsonObjectConst>()) {
        const CopyMissingResult result = CopyMissingSetting(primary, eligibleKeys, pair);
        if (result == CopyMissingResult::Failed) {
            outFailure = JsonLoadResult::NoMemory;
            return false;
        }
        mergedAny = (result == CopyMissingResult::Added) || mergedAny;
    }
    return mergedAny;
}

bool CopyJsonDocumentOwned(JsonDocument& destination,
                           const JsonDocument& source) {
    destination.clear();
    JsonVariant output = destination.to<JsonVariant>();
    const bool copied = CopyJsonValueOwned(output, source.as<JsonVariantConst>(), 0U);
    return copied && !destination.overflowed();
}

} // namespace fxe
