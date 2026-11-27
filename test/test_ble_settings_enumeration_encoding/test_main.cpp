#define LITCH_BLE 1
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include "modules/ble_settings_enumerate.hpp"
#include "modules/settings_registry.hpp"

namespace modules::settings_enumeration_fixture_registry {
const fxe::SettingDescriptor* g_descriptors = nullptr;
size_t g_descriptorCount = 0U;
bool g_useFixture = false;

const fxe::SettingDescriptor* EnumerateAll(size_t& count) {
    if (!g_useFixture) return settings_registry::EnumerateAll(count);
    count = g_descriptorCount;
    return g_descriptors;
}

uint32_t ReadCurrentUInt32(const SettingsStore& settings,
                          const fxe::SettingDescriptor& descriptor) {
    return settings_registry::ReadCurrentUInt32(settings, descriptor);
}
} // namespace modules::settings_enumeration_fixture_registry

class EnumerationCharacteristic : public NimBLECharacteristic {
  public:
    bool rejectNextValue = false;
    bool setValue(const uint8_t* data, size_t length) {
        if (rejectNextValue) {
            rejectNextValue = false;
            return NimBLECharacteristic::setValue(data, 0U);
        }
        return NimBLECharacteristic::setValue(data, length);
    }
};

// Run the production serializer with a supplied immutable registry and an SDK
// value-length fault. Namespace aliases avoid duplicate aggregate symbols.
#define settings_registry settings_enumeration_fixture_registry
#define ble_settings_enumerate ble_settings_enumeration_fixture
#define NimBLECharacteristic EnumerationCharacteristic
#include "../../src/ble_settings_enumerate.cpp"
#undef NimBLECharacteristic
#undef ble_settings_enumerate
#undef settings_registry

namespace {
int g_failures = 0;
using modules::SettingsStore;

void Check(bool condition, const char* message) {
    if (condition) return;
    ++g_failures;
    std::printf("[FAIL] %s\n", message);
}

void UseFixture(const fxe::SettingDescriptor* descriptors, size_t count) {
    namespace fixture = modules::settings_enumeration_fixture_registry;
    fixture::g_useFixture = true;
    fixture::g_descriptors = descriptors;
    fixture::g_descriptorCount = count;
}

fxe::SettingDescriptor Descriptor(const char* key, fxe::SettingType type) {
    return {key, type, fxe::SettingCategory::Behavior, 0U, nullptr,
            0U, UINT32_MAX, nullptr, 0U, nullptr, fxe::VISIBLE_TO_APP};
}

std::string Read(const SettingsStore& settings, size_t start,
                 size_t& next, bool& done, bool reject = false) {
    EnumerationCharacteristic characteristic;
    characteristic.rejectNextValue = reject;
    modules::ble_settings_enumeration_fixture::BuildResponse(
        settings, characteristic, start, next, done);
    return characteristic.getValue();
}

void TestExactTypesEscapingAndRetiredIndices() {
    SettingsStore settings;
    std::array<fxe::SettingDescriptor, 6U> descriptors = {
        Descriptor("hidden", fxe::SettingType::Bool),
        Descriptor("retired", fxe::SettingType::Bool),
        Descriptor("count", fxe::SettingType::UInt32),
        Descriptor("enabled", fxe::SettingType::Bool),
        Descriptor("text", fxe::SettingType::String),
        Descriptor("choice", fxe::SettingType::Enum),
    };
    descriptors[0].flags |= fxe::INTERNAL;
    descriptors[1].flags |= fxe::RETIRED;
    descriptors[2].defaultUint = UINT32_MAX;
    descriptors[3].defaultUint = 1U;
    descriptors[4].defaultStr = "";
    descriptors[4].description = "Quote \" and slash \\";
    const char* options[] = {"", nullptr, "caf\xc3\xa9", "line\n\t\r\b\f\"\\"};
    descriptors[5].allowedValues = options;
    descriptors[5].allowedCount = 4U;
    descriptors[5].defaultStr = "caf\xc3\xa9";
    settings.Doc()["enabled"] = false;
    settings.Doc()["text"] = "\xe6\xa3\x8b\xf0\x9f\x99\x82\n\t\r\b\f\"\\";
    UseFixture(descriptors.data(), descriptors.size());

    size_t next = 99U;
    bool done = false;
    const std::string expected =
        "{\"it\":[{\"i\":1,\"k\":\"count\",\"t\":\"u32\",\"c\":\"behavior\",\"v\":4294967295,\"df\":4294967295,\"mn\":0,\"mx\":4294967295},"
        "{\"i\":2,\"k\":\"enabled\",\"t\":\"bool\",\"c\":\"behavior\",\"v\":false,\"df\":true},"
        "{\"i\":3,\"k\":\"text\",\"t\":\"str\",\"c\":\"behavior\",\"v\":\"\xe6\xa3\x8b\xf0\x9f\x99\x82\\n\\t\\r\\b\\f\\\"\\\\\",\"df\":\"\",\"d\":\"Quote \\\" and slash \\\\\"}"
        ",{\"i\":4,\"k\":\"choice\",\"t\":\"enum\",\"c\":\"behavior\",\"v\":\"caf\xc3\xa9\",\"df\":\"caf\xc3\xa9\",\"opts\":[\"\",\"caf\xc3\xa9\",\"line\\n\\t\\r\\b\\f\\\"\\\\\"]}],\"nx\":0,\"dn\":true}";
    const std::string actual = Read(settings, 0U, next, done);
    Check(actual == expected && next == 0U && done,
          "exact JSON types, key order, UTF-8 and escapes preserve pagination and retired indices");
    const std::string tail = Read(settings, 4U, next, done);
    Check(tail == "{\"it\":[{\"i\":4,\"k\":\"choice\",\"t\":\"enum\",\"c\":\"behavior\",\"v\":\"caf\xc3\xa9\",\"df\":\"caf\xc3\xa9\",\"opts\":[\"\",\"caf\xc3\xa9\",\"line\\n\\t\\r\\b\\f\\\"\\\\\"]}],\"nx\":0,\"dn\":true}" &&
              next == 0U && done,
          "enum tail preserves empty/null options, all escaping and terminal cursor");
}

void TestPayloadBoundaryAndTransactionalRollback() {
    SettingsStore settings;
    auto descriptor = Descriptor("x", fxe::SettingType::String);
    UseFixture(&descriptor, 1U);
    size_t next = 0U;
    bool done = false;
    std::string value(440U, 'x');
    bool sawAcceptedBoundary = false;
    bool sawRejectedBoundary = false;
    for (size_t length = 400U; length < 470U; ++length) {
        value.resize(length, 'x');
        settings.Doc()["x"] = value.c_str();
        const std::string payload = Read(settings, 0U, next, done);
        if (payload.size() == 510U) {
            sawAcceptedBoundary = done && next == 0U;
            value.push_back('x');
            settings.Doc()["x"] = value.c_str();
            const std::string rejected = Read(settings, 0U, next, done);
            sawRejectedBoundary = rejected == "{\"it\":[],\"nx\":0,\"dn\":false}" &&
                                  !done && next == 0U;
            break;
        }
    }
    Check(sawAcceptedBoundary && sawRejectedBoundary,
          "511-byte false-tail preview accepts its exact boundary and rolls back the next byte");

    const auto small = Descriptor("small", fxe::SettingType::Bool);
    std::array<fxe::SettingDescriptor, 2U> descriptors = {small, descriptor};
    UseFixture(descriptors.data(), descriptors.size());
    value.assign(400U, 'x');
    settings.Doc()["x"] = value.c_str();
    const std::string first = Read(settings, 0U, next, done);
    Check(first == "{\"it\":[{\"i\":0,\"k\":\"small\",\"t\":\"bool\",\"c\":\"behavior\",\"v\":false,\"df\":false}],\"nx\":1,\"dn\":false}" &&
              next == 1U && !done,
          "oversized following descriptor rolls back its comma and bytes as one transaction");
    const std::string second = Read(settings, next, next, done);
    Check(second.find("\"i\":1,\"k\":\"x\"") != std::string::npos && done && next == 0U,
          "rolled-back descriptor remains available at the next cursor");
}

void TestProductionHintStyleMetadata() {
    modules::settings_enumeration_fixture_registry::g_useFixture = false;
    SettingsStore settings;
    settings.Doc()["HINT_THINKING_STYLE"] = "player_clockwise";
    size_t cursor = 0U;
    bool done = false;
    bool found = false;
    bool foundPlacement = false;
    for (unsigned page = 0U; page < 64U && !done; ++page) {
        size_t next = 0U;
        const auto response = Read(settings, cursor, next, done);
        StaticJsonDocument<2048> document;
        Check(!deserializeJson(document, response), "production metadata page parses");
        for (const auto item : document["it"].as<JsonArrayConst>()) {
            if (std::strcmp(item["k"] | "", "SETTLE_ANIM_STYLE") == 0) {
                foundPlacement = true;
                const auto placement = item["opts"].as<JsonArrayConst>();
                Check(item["i"].as<unsigned>() == 30U && placement.size() == 5U &&
                          std::strcmp(placement[0U] | "", "landing_flash") == 0 &&
                          std::strcmp(placement[1U] | "", "mini_orbit") == 0 &&
                          std::strcmp(placement[2U] | "", "ripple") == 0 &&
                          std::strcmp(placement[3U] | "", "none") == 0 &&
                          std::strcmp(placement[4U] | "", "rainbow_orbit") == 0 &&
                          std::strcmp(item["v"] | "", "ripple") == 0 &&
                          std::strcmp(item["df"] | "", "ripple") == 0,
                      "placement metadata retains stable ordinals0..4 and advertises the fresh Ripple default");
            }
            if (std::strcmp(item["k"] | "", "HINT_THINKING_STYLE") != 0) continue;
            found = true;
            Check(item["i"].as<unsigned>() == 37U &&
                      std::strcmp(item["t"] | "", "enum") == 0 &&
                      std::strcmp(item["c"] | "", "display") == 0,
                  "Hint style remains at append-only visible metadata index37");
            const auto options = item["opts"].as<JsonArrayConst>();
            Check(options.size() == 5U &&
                      std::strcmp(options[0U] | "", "foxie_sweep") == 0 &&
                      std::strcmp(options[1U] | "", "player_clockwise") == 0 &&
                      std::strcmp(options[2U] | "", "none") == 0 &&
                      std::strcmp(options[3U] | "", "beachball") == 0 &&
                      std::strcmp(options[4U] | "", "cauldron_brewing") == 0 &&
                      std::strcmp(item["v"] | "", "player_clockwise") == 0 &&
                      std::strcmp(item["df"] | "", "cauldron_brewing") == 0,
                  "actual app metadata preserves all four stable choices, appends Cauldron and retains explicit Clockwise while advertising the contracted Cauldron default");
            std::printf("[HINT_STYLE_METADATA] visible_index=37 wire_index=39 page=%s\n", response.c_str());
        }
        Check(done || next > cursor, "production metadata pagination makes progress");
        if (!done && next <= cursor) break;
        cursor = next;
    }
    Check(found && foundPlacement && done, "fresh app metadata contains both named style choices");
}

void TestEmptyFailureAndRestart() {
    SettingsStore settings;
    size_t next = 99U;
    bool done = false;
    UseFixture(nullptr, 0U);
    constexpr char empty[] = "{\"it\":[],\"nx\":0,\"dn\":true}";
    Check(Read(settings, 123U, next, done) == empty && next == 0U && done,
          "empty registry terminates from an arbitrary cursor");

    auto descriptor = Descriptor("choice", fxe::SettingType::Enum);
    std::array<const char*, 255U> options;
    options.fill("");
    descriptor.allowedValues = options.data();
    descriptor.allowedCount = 255U;
    UseFixture(&descriptor, 1U);
    Check(Read(settings, 0U, next, done) == empty && next == 0U && done,
          "former JSON slot overflow remains a terminal fallback after descriptor rollback");

    descriptor = Descriptor("enabled", fxe::SettingType::Bool);
    UseFixture(&descriptor, 1U);
    Check(Read(settings, 0U, next, done, true) == empty && next == 0U && done,
          "SDK value-length failure retains the empty terminal fallback");
    const std::string first = Read(settings, 0U, next, done);
    Check(first.find("\"k\":\"enabled\"") != std::string::npos && done && next == 0U,
          "enumeration restarts normally after an SDK length failure");
    Check(Read(settings, 100U, next, done) == empty && next == 0U && done &&
              Read(settings, 0U, next, done) == first,
          "terminal exhaustion and a fresh cursor reproduce the same first response");
}
} // namespace

int main() {
    TestExactTypesEscapingAndRetiredIndices();
    TestPayloadBoundaryAndTransactionalRollback();
    TestEmptyFailureAndRestart();
    TestProductionHintStyleMetadata();
    std::printf("[SETTINGS_ENUMERATION_ENCODING] %s\n", g_failures == 0 ? "ok" : "failed");
    return g_failures == 0 ? 0 : 1;
}
