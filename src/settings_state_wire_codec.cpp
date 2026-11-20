#include "modules/settings_state_wire_codec.hpp"
#include "modules/assistance_preferences.hpp"

#include <cstring>

#include "foxie_embedded/byte_order.hpp"
#include "modules/ble_settings_value_codec.hpp"
#include "modules/player_end_palette.hpp"
#include "modules/settings_registry.hpp"
#include "modules/ui_color_keys.hpp"

namespace modules::settings_state_wire {
namespace {

constexpr std::array<const char*, VISIBLE_COUNT> VISIBLE_KEYS{{
    "BRIGHTNESS",
    "WHITE_SQUARES_ON",
    "P1_PIECE_WHEEL",
    "P2_PIECE_WHEEL",
    "TURN_ANIM_STYLE",
    "CHECKMATE_ANIM_STYLE",
    "MOVE_CONFIRM",
    "VS_CPU_LEVEL",
    "HINT_LEVEL",
    "VS_CPU_PLAYER",
    "BOARD_ROTATE",
    "GAME_FIRST_PLAYER",
    "TIMER",
    "SLEEP_TIMEOUT",
    "RGBW_COOL_TO_WARM_THRESHOLD",
    "USB_CC_THRESH_500",
    "USB_CC_THRESH_1500",
    "USB_CC_THRESH_3000",
    "BLE_ENABLED",
    "TAKEBACK_GRACE_SECONDS",
    "MOTION_WAKE_ENABLED",
    "MOTION_WAKE_MODE",
    "MOTION_WAKE_THRESHOLD_MG",
    "MOTION_WAKE_DURATION_MS",
    "TAP_WAKE_THRESHOLD_MG",
    "TAP_WAKE_SHOCK_MS",
    "P1_X_DEEP_WAKE_ENABLED",
    "P1_X_WAKE_HOLD_MS",
    "BOARD_SETUP_ASSISTANCE",
    "MINIMAL_ANIMATIONS",
    "SETTLE_ANIM_STYLE",
    "AUTO_BRIGHTNESS",
    "BLE_CONNECTION_INDICATOR",
    "V3_BUTTON_LED_EMULATION",
    "LIGHT_SQUARE_MODE",
    "LIGHT_SQUARE_HUE",
    "LIGHT_SQUARE_BRIGHTNESS",
}};

const fxe::SettingDescriptor* VisibleDescriptor(const uint8_t wanted) {
    if (wanted >= VISIBLE_KEYS.size()) return nullptr;
    const auto* descriptor = settings_registry::LookupByKey(
        VISIBLE_KEYS[wanted]);
    return descriptor != nullptr &&
            ble_settings_value_codec::IsAppVisible(*descriptor)
        ? descriptor : nullptr;
}

bool Normalize(const SettingsStore& settings,
               const fxe::SettingDescriptor& descriptor, uint16_t& out) {
    switch (descriptor.type) {
        case fxe::SettingType::UInt32: {
            const uint32_t value =
                settings_registry::ReadCurrentUInt32(settings, descriptor);
            if (value > UINT16_MAX) return false;
            out = static_cast<uint16_t>(value);
            return true;
        }
        case fxe::SettingType::Bool:
            out = settings.ReadValue(descriptor.key,
                                     descriptor.defaultUint != 0U)
                ? 1U : 0U;
            return true;
        case fxe::SettingType::Enum: {
            const char* value = settings.ReadValue(
                descriptor.key,
                descriptor.defaultStr != nullptr ? descriptor.defaultStr : "");
            for (uint8_t index = 0U; index < descriptor.allowedCount; ++index) {
                if (std::strcmp(value, descriptor.allowedValues[index]) == 0) {
                    out = index;
                    return true;
                }
            }
            return false;
        }
        case fxe::SettingType::String:
            return false;
    }
    return false;
}

} // namespace

bool Encode(const SettingsStore& settings, Payload& out) {
    Payload encoded{};
    // Code range and exact payload length identify this retained-state schema;
    // mutation packets keep their independent explicit VERSION byte.
    fxe::WriteU32LE(settings.GetRevision(), encoded.data());
    for (uint8_t index = 0U; index < VISIBLE_COUNT; ++index) {
        // Reserved retired fields; the remaining wire indices are unchanged.
        if (index == 26U || index == 27U || index == 32U || index == 33U) continue;
        const auto* descriptor = VisibleDescriptor(index);
        uint16_t value = 0U;
        if (descriptor == nullptr || !Normalize(settings, *descriptor, value)) {
            return false;
        }
        fxe::WriteU16LE(value, encoded.data() + 4U + index * 2U);
    }
    encoded[78U] = static_cast<uint8_t>(player_end_palette::ReadSettingUInt32(
        settings, ui_color_keys::P1_END_PIECE_WHEEL, 170U));
    encoded[79U] = static_cast<uint8_t>(player_end_palette::ReadSettingUInt32(
        settings, ui_color_keys::P2_END_PIECE_WHEEL, 0U));
    const auto* style = settings_registry::LookupByKey("HINT_THINKING_STYLE");
    uint16_t styleValue = 0U;
    if (style == nullptr || !Normalize(settings, *style, styleValue)) return false;
    fxe::WriteU16LE(styleValue, encoded.data() + 80U);
    const auto* promotion = settings_registry::LookupByKey("PROMOTION_MODE");
    uint16_t promotionValue = 0U;
    if (promotion == nullptr || !Normalize(settings, *promotion, promotionValue)) return false;
    fxe::WriteU16LE(promotionValue, encoded.data() + 82U);
    out = encoded;
    return true;
}

bool DecodeMutation(const uint8_t visibleIndex,
                    const uint16_t normalizedValue, const char*& outKey,
                    JsonDocument& doc, JsonVariantConst& outValue) {
    const auto* descriptor = visibleIndex == BOOT_DISPLAY_MUTATION_INDEX
        ? settings_registry::LookupByKey("BOOT_DISPLAY")
        : visibleIndex >= ASSISTANCE_FIRST_MUTATION_INDEX &&
            visibleIndex < ASSISTANCE_FIRST_MUTATION_INDEX + 6U
        ? settings_registry::LookupByKey(assistance_preferences::KEYS[
              visibleIndex - ASSISTANCE_FIRST_MUTATION_INDEX])
        : visibleIndex == P1_END_MUTATION_INDEX
        ? settings_registry::LookupByKey(ui_color_keys::P1_END_PIECE_WHEEL)
        : visibleIndex == P2_END_MUTATION_INDEX
            ? settings_registry::LookupByKey(ui_color_keys::P2_END_PIECE_WHEEL)
            : visibleIndex == STYLE_MUTATION_INDEX
                ? settings_registry::LookupByKey("HINT_THINKING_STYLE")
                : visibleIndex == PROMOTION_MUTATION_INDEX
                    ? settings_registry::LookupByKey("PROMOTION_MODE")
                    : VisibleDescriptor(visibleIndex);
    if (descriptor == nullptr) return false;
    doc.clear();
    switch (descriptor->type) {
        case fxe::SettingType::UInt32:
            doc["v"] = normalizedValue;
            break;
        case fxe::SettingType::Bool:
            if (normalizedValue > 1U) return false;
            doc["v"] = normalizedValue != 0U;
            break;
        case fxe::SettingType::Enum:
            if (normalizedValue >= descriptor->allowedCount) return false;
            doc["v"] = descriptor->allowedValues[normalizedValue];
            break;
        case fxe::SettingType::String:
            return false;
    }
    outKey = descriptor->key;
    outValue = doc["v"].as<JsonVariantConst>();
    return true;
}

LivePalettePayload EncodeLivePalette(const SettingsStore& settings) {
    LivePalettePayload payload{};
    payload[0U] = 1U;
    fxe::WriteU32LE(settings.GetRevision(), payload.data() + 1U);
    payload[5U] = static_cast<uint8_t>(player_end_palette::ReadSettingUInt32(
        settings, ui_color_keys::P1_END_PIECE_WHEEL, 170U));
    payload[6U] = static_cast<uint8_t>(player_end_palette::ReadSettingUInt32(
        settings, ui_color_keys::P2_END_PIECE_WHEEL, 0U));
    payload[7U] = settings.ReadValue("GAME_FIRST_PLAYER", uint32_t{1U}) == 2U ? 2U : 1U;
    return payload;
}

AssistancePayload EncodeAssistance(const SettingsStore& settings) {
    AssistancePayload payload{};
    payload[0U] = 1U;
    fxe::WriteU32LE(settings.GetRevision(), payload.data() + 1U);
    for (uint8_t bit = 0U; bit < 6U; ++bit) {
        if (settings.ReadValue(assistance_preferences::KEYS[bit], bit < 4U)) {
            payload[5U] |= static_cast<uint8_t>(1U << bit);
        }
    }
    return payload;
}

BootDisplayPayload EncodeBootDisplay(const SettingsStore& settings) {
    BootDisplayPayload payload{}; payload[0U] = 1U;
    fxe::WriteU32LE(settings.GetRevision(), payload.data() + 1U);
    const auto* descriptor = settings_registry::LookupByKey("BOOT_DISPLAY");
    uint16_t value = 0U;
    if (descriptor != nullptr) (void)Normalize(settings, *descriptor, value);
    payload[5U] = static_cast<uint8_t>(value);
    return payload;
}
} // namespace modules::settings_state_wire
