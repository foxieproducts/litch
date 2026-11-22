#include <unity.h>

#include <fstream>
#include <string>

#include "modules/settings_state_wire_codec.hpp"
#include "modules/owner_pairing_session_wire.hpp"
#include "modules/settings_registry.hpp"

namespace {

void TestAssistanceIsAdditiveAndKeepsOldSnapshotExact() {
    using namespace modules::settings_state_wire;
    static_assert(ASSISTANCE_STATE_CODE > modules::owner_pairing_session_wire::IDENTITY_HIGH_CODE);
    modules::SettingsStore settings;
    Payload before{}, after{};
    TEST_ASSERT_TRUE(Encode(settings, before));
    const auto defaults = EncodeAssistance(settings);
    TEST_ASSERT_EQUAL_UINT8(43U, ASSISTANCE_STATE_CODE);
    TEST_ASSERT_EQUAL_UINT8(1U, defaults[0U]);
    TEST_ASSERT_EQUAL_HEX8(0x0fU, defaults[5U]);
    for (uint8_t index = 0U; index < 6U; ++index) {
        const char* key = nullptr;
        JsonVariantConst value;
        StaticJsonDocument<32> doc;
        TEST_ASSERT_TRUE(DecodeMutation(41U + index, 1U, key, doc, value));
        TEST_ASSERT_NOT_NULL(key);
        TEST_ASSERT_TRUE(value.as<bool>());
        settings.Doc()[key] = true;
        TEST_ASSERT_FALSE(DecodeMutation(41U + index, 2U, key, doc, value));
    }
    TEST_ASSERT_TRUE(Encode(settings, after));
    TEST_ASSERT_EQUAL_UINT32(84U, after.size());
    TEST_ASSERT_EQUAL_MEMORY(before.data(), after.data(), before.size());
    TEST_ASSERT_EQUAL_HEX8(0x3fU, EncodeAssistance(settings)[5U]);
}

void TestDefaultSnapshotMatchesSharedFixture() {
    modules::SettingsStore settings;
    modules::settings_state_wire::Payload payload;
    settings.Doc()["V3_BUTTON_LED_EMULATION"] = true;
    settings.Doc()["P1_X_DEEP_WAKE_ENABLED"] = false;
    settings.Doc()["P1_X_WAKE_HOLD_MS"] = 4000U;
    TEST_ASSERT_TRUE(modules::settings_state_wire::Encode(settings, payload));
    const char* const dormantKeys[]{
        "PIECE_ANIMATION_STYLE", "P1_PIECE_ANIMATION_STYLE", "P2_PIECE_ANIMATION_STYLE"};
    for (const char* key : dormantKeys) {
        settings.Doc()[key] = "rainbow";
        TEST_ASSERT_NULL(modules::settings_registry::LookupByKey(key));
    }
    (void)modules::settings_registry::CanonicalizeLoadedSettings(settings.Doc());
    for (const char* key : dormantKeys) {
        TEST_ASSERT_EQUAL_STRING("rainbow", settings.ReadValue(key, ""));
    }
    modules::settings_state_wire::Payload withDormantPreferences{};
    TEST_ASSERT_TRUE(modules::settings_state_wire::Encode(settings, withDormantPreferences));
    TEST_ASSERT_EQUAL_MEMORY(payload.data(), withDormantPreferences.data(), payload.size());
    for (const uint8_t index : {26U, 27U}) {
        TEST_ASSERT_EQUAL_UINT8(0U, payload[4U + index * 2U]);
        TEST_ASSERT_EQUAL_UINT8(0U, payload[5U + index * 2U]);
        StaticJsonDocument<32> retired;
        const char* key = nullptr;
        JsonVariantConst value;
        TEST_ASSERT_FALSE(modules::settings_state_wire::DecodeMutation(
            index, 1U, key, retired, value));
    }
    TEST_ASSERT_EQUAL_UINT8(1U, payload[14U]); // Absent checkmate style defaults to Fire Lob.
    TEST_ASSERT_EQUAL_UINT8(0U, payload[70U]);
    TEST_ASSERT_EQUAL_UINT8(0U, payload[71U]);
    std::ifstream fixture("test/fixtures/settings_state_wire_v3.csv");
    TEST_ASSERT_TRUE(fixture.good());
    std::string line;
    std::getline(fixture, line);
    std::getline(fixture, line);
    const size_t comma = line.find(',');
    TEST_ASSERT_NOT_EQUAL(std::string::npos, comma);
    const std::string hex = line.substr(comma + 1U);
    TEST_ASSERT_EQUAL_UINT32(payload.size() * 2U, hex.size());
    for (size_t index = 0U; index < payload.size(); ++index) {
        TEST_ASSERT_EQUAL_HEX8(
            static_cast<uint8_t>(
                std::stoul(hex.substr(index * 2U, 2U), nullptr, 16)),
            payload[index]);
    }
}

void TestStyleTailPreservesOriginalPrefix() {
    modules::SettingsStore settings;
    modules::settings_state_wire::Payload before{}, after{};
    settings.Doc()["HINT_THINKING_STYLE"] = "foxie_sweep";
    TEST_ASSERT_TRUE(modules::settings_state_wire::Encode(settings, before));
    TEST_ASSERT_EQUAL_UINT8(0U, before[80U]);
    settings.Doc()["HINT_THINKING_STYLE"] = "player_clockwise";
    TEST_ASSERT_TRUE(modules::settings_state_wire::Encode(settings, after));
    TEST_ASSERT_EQUAL_MEMORY(before.data(), after.data(), 80U);
    TEST_ASSERT_EQUAL_UINT8(1U, after[80U]);
    TEST_ASSERT_EQUAL_UINT8(0U, after[81U]);
    settings.Doc()["HINT_THINKING_STYLE"] = "none";
    modules::settings_state_wire::Payload none{};
    TEST_ASSERT_TRUE(modules::settings_state_wire::Encode(settings, none));
    TEST_ASSERT_EQUAL_MEMORY(before.data(), none.data(), 80U);
    TEST_ASSERT_EQUAL_UINT8(2U, none[80U]);
    TEST_ASSERT_EQUAL_UINT8(0U, none[81U]);
    settings.Doc()["HINT_THINKING_STYLE"] = "beachball";
    modules::settings_state_wire::Payload beachball{};
    TEST_ASSERT_TRUE(modules::settings_state_wire::Encode(settings, beachball));
    TEST_ASSERT_EQUAL_MEMORY(before.data(), beachball.data(), 80U);
    TEST_ASSERT_EQUAL_UINT8(3U, beachball[80U]);
    TEST_ASSERT_EQUAL_UINT8(0U, beachball[81U]);
    settings.Doc()["PROMOTION_MODE"] = "choose";
    TEST_ASSERT_TRUE(modules::settings_state_wire::Encode(settings, none));
    TEST_ASSERT_EQUAL_UINT8(1U, none[82U]);
    TEST_ASSERT_EQUAL_UINT8(0U, none[83U]);
    TEST_ASSERT_EQUAL_MEMORY(before.data(), none.data(), 80U);
    settings.Doc()["CHECKMATE_ANIM_STYLE"] = "fireworks";
    TEST_ASSERT_TRUE(modules::settings_state_wire::Encode(settings, none));
    TEST_ASSERT_EQUAL_UINT8(0U, none[14U]); // Explicit saved Fireworks remains index0.
    TEST_ASSERT_EQUAL_UINT8(39U, modules::settings_state_wire::FragmentCode(8U));
    TEST_ASSERT_EQUAL_UINT8(4U, modules::settings_state_wire::FragmentLength(8U));
    for (uint8_t i = 0U; i < 8U; ++i) {
        TEST_ASSERT_EQUAL_UINT8(14U + i, modules::settings_state_wire::FragmentCode(i));
        TEST_ASSERT_EQUAL_UINT8(10U, modules::settings_state_wire::FragmentLength(i));
    }
}

void TestPlacementUsesRippleDefaultAndKeepsHistoricalValues() {
    modules::SettingsStore settings;
    modules::settings_state_wire::Payload encoded{};
    TEST_ASSERT_TRUE(modules::settings_state_wire::Encode(settings, encoded));
    TEST_ASSERT_EQUAL_UINT8(2U, encoded[64U]);
    const char* styles[]{"landing_flash", "mini_orbit", "ripple", "none", "rainbow_orbit"};
    for (uint8_t value = 0U; value < 5U; ++value) {
        settings.Doc()["SETTLE_ANIM_STYLE"] = styles[value];
        TEST_ASSERT_TRUE(modules::settings_state_wire::Encode(settings, encoded));
        TEST_ASSERT_EQUAL_UINT8(value, encoded[64U]);
        TEST_ASSERT_EQUAL_UINT8(0U, encoded[65U]);
    }
}

void TestMutationDecodesTypedVisibleValues() {
    StaticJsonDocument<32> document;
    const char* key = nullptr;
    JsonVariantConst value;
    for (const uint8_t index : {48U, 49U}) {
        TEST_ASSERT_FALSE(modules::settings_state_wire::DecodeMutation(
            index, 1U, key, document, value));
    }
    TEST_ASSERT_TRUE(modules::settings_state_wire::DecodeMutation(
        30U, 3U, key, document, value));
    TEST_ASSERT_EQUAL_STRING("SETTLE_ANIM_STYLE", key);
    TEST_ASSERT_EQUAL_STRING("none", value.as<const char*>());
    TEST_ASSERT_FALSE(modules::settings_state_wire::DecodeMutation(
        30U, 5U, key, document, value));
    TEST_ASSERT_TRUE(modules::settings_state_wire::DecodeMutation(
        10U, 1U, key, document, value));
    TEST_ASSERT_EQUAL_STRING("BOARD_ROTATE", key);
    TEST_ASSERT_TRUE(value.as<bool>());

    TEST_ASSERT_TRUE(modules::settings_state_wire::DecodeMutation(
        34U, 1U, key, document, value));
    TEST_ASSERT_EQUAL_STRING("LIGHT_SQUARE_MODE", key);
    TEST_ASSERT_EQUAL_STRING("color", value.as<const char*>());

    TEST_ASSERT_TRUE(modules::settings_state_wire::DecodeMutation(
        36U, 7U, key, document, value));
    TEST_ASSERT_EQUAL_STRING("LIGHT_SQUARE_BRIGHTNESS", key);
    TEST_ASSERT_EQUAL_UINT16(7U, value.as<uint16_t>());

    TEST_ASSERT_TRUE(modules::settings_state_wire::DecodeMutation(
        modules::settings_state_wire::P1_END_MUTATION_INDEX, 42U,
        key, document, value));
    TEST_ASSERT_EQUAL_STRING("P1_END_PIECE_WHEEL", key);
    TEST_ASSERT_EQUAL_UINT16(42U, value.as<uint16_t>());

    TEST_ASSERT_FALSE(modules::settings_state_wire::DecodeMutation(
        32U, 0U, key, document, value));
    TEST_ASSERT_FALSE(modules::settings_state_wire::DecodeMutation(
        33U, 0U, key, document, value));
    TEST_ASSERT_FALSE(modules::settings_state_wire::DecodeMutation(
        33U, 1U, key, document, value));
    TEST_ASSERT_FALSE(modules::settings_state_wire::DecodeMutation(
        34U, 2U, key, document, value));
    TEST_ASSERT_FALSE(modules::settings_state_wire::DecodeMutation(
        39U, 5U, key, document, value));
    TEST_ASSERT_TRUE(modules::settings_state_wire::DecodeMutation(
        39U, 1U, key, document, value));
    TEST_ASSERT_EQUAL_STRING("HINT_THINKING_STYLE", key);
    TEST_ASSERT_EQUAL_STRING("player_clockwise", value.as<const char*>());
    TEST_ASSERT_TRUE(modules::settings_state_wire::DecodeMutation(
        39U, 2U, key, document, value));
    TEST_ASSERT_EQUAL_STRING("HINT_THINKING_STYLE", key);
    TEST_ASSERT_EQUAL_STRING("none", value.as<const char*>());
    TEST_ASSERT_TRUE(modules::settings_state_wire::DecodeMutation(
        39U, 3U, key, document, value));
    TEST_ASSERT_EQUAL_STRING("HINT_THINKING_STYLE", key);
    TEST_ASSERT_EQUAL_STRING("beachball", value.as<const char*>());
    TEST_ASSERT_TRUE(modules::settings_state_wire::DecodeMutation(
        39U, 4U, key, document, value));
    TEST_ASSERT_EQUAL_STRING("HINT_THINKING_STYLE", key);
    TEST_ASSERT_EQUAL_STRING("cauldron_brewing", value.as<const char*>());
    TEST_ASSERT_TRUE(modules::settings_state_wire::DecodeMutation(
        40U, 1U, key, document, value));
    TEST_ASSERT_EQUAL_STRING("PROMOTION_MODE", key);
    TEST_ASSERT_EQUAL_STRING("choose", value.as<const char*>());
    TEST_ASSERT_FALSE(modules::settings_state_wire::DecodeMutation(
        40U, 2U, key, document, value));
    TEST_ASSERT_TRUE(modules::settings_state_wire::DecodeMutation(
        5U, 1U, key, document, value));
    TEST_ASSERT_EQUAL_STRING("CHECKMATE_ANIM_STYLE", key);
    TEST_ASSERT_EQUAL_STRING("fire-lob", value.as<const char*>());

}

} // namespace

int main() {
    UNITY_BEGIN();
    TEST_ASSERT_EQUAL_UINT8(2U, modules::settings_state_wire::VERSION);
    RUN_TEST(TestDefaultSnapshotMatchesSharedFixture);
    RUN_TEST(TestAssistanceIsAdditiveAndKeepsOldSnapshotExact);
    RUN_TEST(TestPlacementUsesRippleDefaultAndKeepsHistoricalValues);
    RUN_TEST(TestMutationDecodesTypedVisibleValues);
    RUN_TEST(TestStyleTailPreservesOriginalPrefix);
    return UNITY_END();
}
