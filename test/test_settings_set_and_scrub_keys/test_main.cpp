#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

#include "console.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/settings_store.hpp"

namespace fxe {
struct SettingsScrubStats;
bool ScrubBadPrintableStrings(JsonDocument& doc, SettingsScrubStats* outStats = nullptr,
                             SettingsRemovedKeys* removed = nullptr);
} // namespace fxe

namespace {

bool IsValidKeyCStr(const char* key) {
    if (key == nullptr) {
        return false;
    }

    const size_t n = std::strlen(key);
    if (n == 0 || n > 63) {
        return false;
    }

    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(key[i]);
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

bool AreAllKeysValid(const JsonDocument& doc) {
    if (!doc.is<JsonObject>()) {
        return true;
    }

    for (const JsonPairConst kv : doc.as<JsonObjectConst>()) {
        if (!IsValidKeyCStr(kv.key().c_str())) {
            return false;
        }
    }

    return true;
}

bool HasAnyInvalidPrintableStringValue(const JsonDocument& doc) {
    if (!doc.is<JsonObject>()) {
        return false;
    }

    for (const JsonPairConst kv : doc.as<JsonObjectConst>()) {
        const JsonVariantConst v = kv.value();
        if (!v.is<JsonString>()) {
            continue;
        }

        const JsonString s = v.as<JsonString>();
        if (s.isNull()) {
            continue;
        }

        const char* p = s.c_str();
        if (p == nullptr) {
            return true;
        }

        const size_t n = s.size();
        if (n > 512) {
            return true;
        }

        for (size_t i = 0; i < n; ++i) {
            const unsigned char c = static_cast<unsigned char>(p[i]);
            if (c == 0) {
                return true;
            }
            if (c < 32 || c > 126) {
                return true;
            }
            if (c == '\\') {
                return true;
            }
        }
    }

    return false;
}

} // namespace

int main() {
    fxe::test_support::TestResults results("SETTINGS_SET_AND_SCRUB_KEYS");
    results.Start();

    // Scrub: must be safe and preserve valid documents.
    {
        StaticJsonDocument<512> doc;

        const char* json = R"({"P2_PIECE_WHEEL":70,"OK":"hi"})";
        const auto err = deserializeJson(doc, json);
        results.Check(err == DeserializationError::Ok, "test json parses");

        results.Check(AreAllKeysValid(doc), "doc keys are valid before scrub");

        const bool scrubbed = fxe::ScrubBadPrintableStrings(doc, nullptr);
        results.Check(!scrubbed, "scrub does not mutate valid docs");
        results.Check(doc.is<JsonObject>(), "scrub preserves object root");
        results.Check(doc.containsKey("P2_PIECE_WHEEL"), "scrub preserves valid keys");
        results.Check((doc["P2_PIECE_WHEEL"] | 0) == 70, "valid key value preserved");
    }

    // Persistence sees decoded bytes, so literal escape spelling is valid text.
    {
        StaticJsonDocument<512> doc;
        const char* json = R"({"LITERAL":"name\\u0000suffix","ACTUAL":"name\u0000suffix","SAFE":"keep"})";
        results.Check(!deserializeJson(doc, json), "literal and actual NUL JSON fixture parses");
        results.Check(doc["ACTUAL"].as<JsonString>().size() == 11U,
                      "actual JSON NUL remains part of the decoded length");
        results.Check(fxe::ScrubBadPrintableStrings(doc), "actual NUL is scrubbed");
        results.Check(!doc.containsKey("ACTUAL"), "actual NUL field removed");
        results.Check(std::strcmp(doc["LITERAL"] | "", "name\\u0000suffix") == 0,
                      "six printable escape-like bytes survive scrubbing");
        results.Check(std::strcmp(doc["SAFE"] | "", "keep") == 0,
                      "unrelated valid field remains intact");
        results.Check(!fxe::ScrubBadPrintableStrings(doc), "valid survivor scrub is idempotent");

        char encoded[512] = {};
        serializeJson(doc, encoded, sizeof(encoded));
        StaticJsonDocument<512> reloaded;
        results.Check(!deserializeJson(reloaded, encoded), "serialized printable spelling reloads");
        results.Check(!fxe::ScrubBadPrintableStrings(reloaded), "reload never mistakes double escaping for corruption");
        results.Check(std::strcmp(reloaded["LITERAL"] | "", "name\\u0000suffix") == 0,
                      "JSON round trip preserves literal spelling exactly");
    }

    // Console: setting a key must not create hidden/empty keys.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_settings_set_and_scrub_keys.json";
        cfg.deferredSaveDelayMs = 100000;
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_settings_set_and_scrub_keys.json.bak");
        (void)std::remove("build/test_settings_set_and_scrub_keys.json.tmp");

        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "settings load");

        MemoryConsoleTransport transport;
        Console console(transport);
        console.SetSettingsStore(&settings);

        fxe::test_support::RunCommand(console, transport, "set P2_PIECE_WHEEL 60");
        results.Check(settings.Doc().containsKey("P2_PIECE_WHEEL"), "doc contains P2_PIECE_WHEEL");
        results.Check(settings.ReadValue("P2_PIECE_WHEEL", static_cast<int32_t>(0)) == 60,
                      "doc value is 60");

        // Overwrite console's internal exec buffer to catch any accidental key aliasing.
        fxe::test_support::RunCommand(console, transport, "help");

        results.Check(settings.Doc().containsKey("P2_PIECE_WHEEL"), "doc still contains P2_PIECE_WHEEL after another command");
        results.Check(AreAllKeysValid(settings.Doc()), "doc keys remain valid");
    }

    return results.Finish();
}
