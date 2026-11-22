#include "settings_store_internal.hpp"

#include <cctype>
#include <cstring>

#include "foxie_embedded/logger.hpp"
#include "foxie_embedded/settings_store.hpp"

namespace fxe {

PartitionErasure ClassifyPartitionErasure(
    const size_t partitionSize, const PartitionRead read,
    const void* const context,
    uint8_t* const scratch, const size_t scratchSize) {
    if (partitionSize == 0U || read == nullptr || scratch == nullptr ||
        scratchSize == 0U) {
        return PartitionErasure::ReadError;
    }

    size_t offset = 0U;
    while (offset < partitionSize) {
        const size_t remaining = partitionSize - offset;
        const size_t count = remaining < scratchSize ? remaining : scratchSize;
        if (!read(context, offset, scratch, count)) {
            return PartitionErasure::ReadError;
        }
        for (size_t index = 0U; index < count; ++index) {
            if (scratch[index] != 0xFFU) {
                return PartitionErasure::ContainsData;
            }
        }
        offset += count;
    }
    return PartitionErasure::Erased;
}

bool MakeDerivedPath(char* out,
                     const size_t outCapacity,
                     const char* basePath,
                     const char* suffix) {
    if (out == nullptr || outCapacity == 0 || basePath == nullptr || suffix == nullptr) {
        return false;
    }

    const size_t baseLen = std::strlen(basePath);
    const size_t suffixLen = std::strlen(suffix);
    if (baseLen == 0 || (baseLen + suffixLen) >= outCapacity) {
        return false;
    }

    std::memcpy(out, basePath, baseLen);
    std::memcpy(out + baseLen, suffix, suffixLen);
    out[baseLen + suffixLen] = '\0';
    return true;
}

bool DeepCopyJsonDoc(const JsonDocument& src, JsonDocument& dst) {
    dst.clear();
    return dst.set(src);
}

bool CopyUppercaseSettingKey(const char* key, char* out, const size_t outCapacity) {
    if (key == nullptr || out == nullptr || outCapacity == 0) {
        return false;
    }

    const size_t n = std::strlen(key);
    static constexpr size_t MAX_KEY_LEN = 63;
    if (n == 0 || n > MAX_KEY_LEN || (n + 1U) > outCapacity) {
        return false;
    }

    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(key[i]);
        if (c == 0 || c < 32 || c > 126 || c == '\\') {
            return false;
        }

        if (c >= 'a' && c <= 'z') {
            out[i] = static_cast<char>(c - ('a' - 'A'));
        } else {
            out[i] = static_cast<char>(c);
        }
    }

    out[n] = '\0';
    return true;
}

namespace {

bool EqualsNoCaseAscii(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }

    while (*a != '\0' && *b != '\0') {
        const unsigned char ca = static_cast<unsigned char>(*a);
        const unsigned char cb = static_cast<unsigned char>(*b);
        const unsigned char ua = static_cast<unsigned char>(std::toupper(ca));
        const unsigned char ub = static_cast<unsigned char>(std::toupper(cb));
        if (ua != ub) {
            return false;
        }
        ++a;
        ++b;
    }

    return (*a == '\0') && (*b == '\0');
}

class StringPointerUseCounter final : public ArduinoJson::detail::Visitor<uint8_t> {
  public:
    explicit StringPointerUseCounter(const char* target)
        : m_target(target) {}

    uint8_t visitArray(const ArduinoJson::detail::CollectionData& array) {
        return VisitValues(array, false);
    }

    uint8_t visitObject(const ArduinoJson::detail::CollectionData& object) {
        return VisitValues(object, true);
    }

    uint8_t visitRawJson(const char* data, size_t) {
        Add(data == m_target);
        return m_count;
    }

    uint8_t visitString(const char* data, size_t) {
        Add(data == m_target);
        return m_count;
    }

  private:
    void Add(const bool matches) {
        if (matches && m_count < 2U) {
            ++m_count;
        }
    }

    uint8_t VisitValues(const ArduinoJson::detail::CollectionData& collection,
                        const bool countKeys) {
        for (const ArduinoJson::detail::VariantSlot* slot = collection.head(); slot != nullptr;
             slot = slot->next()) {
            Add(countKeys && slot->key() == m_target);
            (void)slot->data()->accept(*this);
            if (m_count > 1U) {
                break;
            }
        }
        return m_count;
    }

    const char* m_target = nullptr;
    uint8_t m_count = 0;
};

uint8_t CountStringPointerUses(JsonDocument& doc, const char* target) {
    StringPointerUseCounter counter(target);
    return ArduinoJson::detail::VariantAttorney::getData(doc)->accept(counter);
}

ArduinoJson::detail::VariantSlot* FindExactObjectSlot(
    ArduinoJson::detail::CollectionData& object,
    const char* key) {
    for (ArduinoJson::detail::VariantSlot* slot = object.head(); slot != nullptr;
         slot = slot->next()) {
        if (std::strcmp(slot->key(), key) == 0) {
            return slot;
        }
    }
    return nullptr;
}

bool RenameObjectSlotUppercase(JsonDocument& doc,
                               ArduinoJson::detail::VariantSlot& slot,
                               char* upperKey) {
    auto* pool = ArduinoJson::detail::VariantAttorney::getPool(doc);
    char* rawKey = const_cast<char*>(slot.key());
    const bool keyIsUnique = CountStringPointerUses(doc, rawKey) == 1U;
    if (slot.ownsKey() && pool->owns(rawKey) && keyIsUnique) {
        std::memcpy(rawKey, upperKey, std::strlen(upperKey) + 1U);
        return true;
    }

    return ArduinoJson::detail::slotSetKey(
        &slot, ArduinoJson::detail::adaptString(upperKey), pool);
}

} // namespace

const char* FindTopLevelObjectKeyCaseInsensitive(const JsonObjectConst object, const char* key) {
    if (key == nullptr || key[0] == '\0') return nullptr;
    for (const JsonPairConst pair : object) {
        if (EqualsNoCaseAscii(pair.key().c_str(), key)) return pair.key().c_str();
    }
    return nullptr;
}

const char* FindTopLevelObjectKeyCaseInsensitive(const JsonDocument& doc, const char* key) {
    return FindTopLevelObjectKeyCaseInsensitive(doc.as<JsonObjectConst>(), key);
}

bool NormalizeTopLevelObjectKeysUppercase(JsonDocument& doc) {
    if (!doc.is<JsonObject>()) {
        return false;
    }

    auto* root = ArduinoJson::detail::VariantAttorney::getData(doc);
    ArduinoJson::detail::CollectionData* object = root->asObject();
    bool mutated = false;
    for (ArduinoJson::detail::VariantSlot* slot = object->head(); slot != nullptr;) {
        ArduinoJson::detail::VariantSlot* next = slot->next();
        const char* rawKey = slot->key();
        if (rawKey == nullptr || rawKey[0] == '\0') {
            slot = next;
            continue;
        }

        char upperKey[64] = {};
        if (!CopyUppercaseSettingKey(rawKey, upperKey, sizeof(upperKey))) {
            slot = next;
            continue;
        }

        const bool isCanonical = (std::strcmp(rawKey, upperKey) == 0);
        if (isCanonical) {
            slot = next;
            continue;
        }

        // A canonical entry anywhere in the object wins. A winning alias is renamed
        // in place, so later aliases see it as canonical and are removed.
        if (FindExactObjectSlot(*object, upperKey) != nullptr) {
            object->removeSlot(slot);
            mutated = true;
            slot = next;
            continue;
        }

        if (RenameObjectSlotUppercase(doc, *slot, upperKey)) {
            mutated = true;
        } else {
            fxe::Warn("settings: insufficient JSON capacity to canonicalize key: %s", rawKey);
        }
        slot = next;
    }

    return mutated;
}

namespace {

bool IsSafePersistedStringValue(const JsonVariantConst v) {
    const JsonString s = v.as<JsonString>();
    if (s.isNull()) {
        return false;
    }

    const size_t n = s.size();
    if (n == 0) {
        return true;
    }

    const char* p = s.c_str();
    if (p == nullptr) {
        return false;
    }

    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if (c == 0) {
            return false;
        }

        // Validate decoded bytes, not printable escape-like spelling. JSON's
        // actual \u0000 escape has already become a NUL and is rejected above;
        // a literal backslash followed by "u0000" is ordinary user text.
        // Reject control characters that can break logging/parsers or hide corruption.
        // Allow bytes >= 128 so UTF-8 text doesn't get pruned.
        if ((c < 32) || (c == 127)) {
            return false;
        }
    }

    return true;
}

bool IsPrintableAsciiNoNulKey(const JsonString key) {
    if (key.isNull()) {
        return false;
    }

    const size_t n = key.size();
    static constexpr size_t MAX_KEY_LEN = 63;
    if (n == 0) {
        return false;
    }
    if (n > MAX_KEY_LEN) {
        return false;
    }

    const char* p = key.c_str();
    if (p == nullptr) {
        return false;
    }

    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if (c == 0) {
            // Reject keys containing embedded NUL (e.g., parsed from "\u0000").
            return false;
        }
        if (c < 32 || c > 126) {
            return false;
        }
        if (c == '\\') {
            // Reject escape sequences in keys (schema keys are plain ASCII).
            return false;
        }
    }

    return true;
}

} // namespace

bool ScrubBadPrintableStrings(JsonDocument& doc, SettingsScrubStats* outStats,
                             SettingsRemovedKeys* removed) {
    static constexpr uint8_t MAX_DEPTH = 6;

    SettingsScrubStats scratchStats = {};
    SettingsScrubStats& stats = (outStats != nullptr) ? *outStats : scratchStats;
    stats = {};

    auto scrubObject = [&](auto&& self, JsonObject obj, const uint8_t depth) -> bool {
        if (depth > MAX_DEPTH) {
            return false;
        }

        bool scrubbed = false;

        // Collect iterators first; mutating while iterating can invalidate the iterator.
        // We use iterators (not C-string keys) so we can remove keys containing embedded NUL.
        static constexpr size_t MAX_REMOVES = 32;
        JsonObject::iterator itersToRemove[MAX_REMOVES] = {};
        size_t removeCount = 0;

        for (JsonObject::iterator it = obj.begin(); it != obj.end(); ++it) {
            const JsonPair kv = *it;
            const JsonString keyStr = kv.key();
            if (!IsPrintableAsciiNoNulKey(keyStr)) {
                if (removeCount < MAX_REMOVES) {
                    itersToRemove[removeCount] = it;
                    ++removeCount;
                }
                scrubbed = true;
                continue;
            }

            JsonVariant v = kv.value();
            if (v.is<JsonString>() && !IsSafePersistedStringValue(v)) {
                if (removeCount < MAX_REMOVES) {
                    itersToRemove[removeCount] = it;
                    ++removeCount;
                }
                scrubbed = true;
                continue;
            }

            if (v.is<JsonObject>()) {
                scrubbed = self(self, v.as<JsonObject>(), static_cast<uint8_t>(depth + 1U)) || scrubbed;
                continue;
            }

            if (v.is<JsonArray>()) {
                JsonArray arr = v.as<JsonArray>();
                for (size_t i = 0; i < arr.size(); ++i) {
                    JsonVariant e = arr[i];
                    if (e.is<JsonString>() && !IsSafePersistedStringValue(e)) {
                        arr[i] = nullptr;
                        scrubbed = true;
                        if (stats.nulledArrayElements != UINT16_MAX) {
                            ++stats.nulledArrayElements;
                        }
                        continue;
                    }
                    if (e.is<JsonObject>()) {
                        scrubbed = self(self, e.as<JsonObject>(), static_cast<uint8_t>(depth + 1U)) || scrubbed;
                        continue;
                    }
                    if (e.is<JsonArray>()) {
                        JsonArray nestedArr = e.as<JsonArray>();
                        for (size_t j = 0; j < nestedArr.size(); ++j) {
                            JsonVariant e2 = nestedArr[j];
                            if (e2.is<JsonString>() && !IsSafePersistedStringValue(e2)) {
                                nestedArr[j] = nullptr;
                                scrubbed = true;
                                if (stats.nulledArrayElements != UINT16_MAX) {
                                    ++stats.nulledArrayElements;
                                }
                            } else if (e2.is<JsonObject>()) {
                                scrubbed = self(self, e2.as<JsonObject>(), static_cast<uint8_t>(depth + 1U)) || scrubbed;
                            }
                        }
                    }
                }
            }
        }

        for (size_t i = removeCount; i-- > 0;) {
            if (depth == 0U && removed != nullptr &&
                !removed->Record((*itersToRemove[i]).key())) return true;
            obj.remove(itersToRemove[i]);
            scrubbed = true;
            if (stats.removedObjectKeys != UINT16_MAX) {
                ++stats.removedObjectKeys;
            }
        }

        return scrubbed;
    };

    if (doc.is<JsonObject>()) {
        return scrubObject(scrubObject, doc.as<JsonObject>(), 0);
    }

    if (doc.is<JsonArray>()) {
        JsonArray arr = doc.as<JsonArray>();
        bool scrubbed = false;
        for (size_t i = 0; i < arr.size(); ++i) {
            const JsonVariantConst ce = arr[i];
            if (ce.is<JsonString>() && !IsSafePersistedStringValue(ce)) {
                arr[i] = nullptr;
                scrubbed = true;
                if (stats.nulledArrayElements != UINT16_MAX) {
                    ++stats.nulledArrayElements;
                }
            } else if (ce.is<JsonObject>()) {
                scrubbed = scrubObject(scrubObject, arr[i].as<JsonObject>(), 0) || scrubbed;
            }
        }
        return scrubbed;
    }

    return false;
}

} // namespace fxe
