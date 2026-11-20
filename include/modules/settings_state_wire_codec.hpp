#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <ArduinoJson.h>

#include "modules/settings_store.hpp"

namespace modules::settings_state_wire {

inline constexpr uint8_t VERSION = 2U;
inline constexpr uint8_t VISIBLE_COUNT = 37U;
inline constexpr size_t PAYLOAD_BYTES = 84U;
inline constexpr size_t FRAGMENT_BYTES = 10U;
inline constexpr uint8_t FIRST_STATE_CODE = 14U;
inline constexpr uint8_t FRAGMENT_COUNT = 9U;
inline constexpr uint8_t LAST_STATE_CODE =
    FIRST_STATE_CODE + 7U;
inline constexpr uint8_t STYLE_STATE_CODE = 39U;
inline constexpr uint8_t STYLE_MUTATION_INDEX = 39U;
inline constexpr uint8_t PROMOTION_MUTATION_INDEX = 40U;
inline constexpr uint8_t BOOT_DISPLAY_MUTATION_INDEX = 47U;
inline constexpr uint8_t BOOT_DISPLAY_STATE_CODE = 44U;
using BootDisplayPayload = std::array<uint8_t, 6U>;
BootDisplayPayload EncodeBootDisplay(const SettingsStore& settings);
inline constexpr uint8_t ASSISTANCE_STATE_CODE = 43U;
inline constexpr uint8_t ASSISTANCE_FIRST_MUTATION_INDEX = 41U;
inline constexpr size_t ASSISTANCE_BYTES = 6U;
using AssistancePayload = std::array<uint8_t, ASSISTANCE_BYTES>;
AssistancePayload EncodeAssistance(const SettingsStore& settings);
inline constexpr uint8_t LIVE_PALETTE_STATE_CODE = 46U;
using LivePalettePayload = std::array<uint8_t, 8U>;
LivePalettePayload EncodeLivePalette(const SettingsStore& settings);
constexpr uint8_t FragmentCode(uint8_t index) {
    return index < 8U ? FIRST_STATE_CODE + index : STYLE_STATE_CODE;
}
constexpr size_t FragmentLength(uint8_t index) {
    return index < 8U ? FRAGMENT_BYTES : 4U;
}
inline constexpr uint8_t MUTATION_OPCODE = 24U;
inline constexpr size_t MUTATION_BODY_BYTES = 4U;
inline constexpr uint8_t P1_END_MUTATION_INDEX = VISIBLE_COUNT;
inline constexpr uint8_t P2_END_MUTATION_INDEX = VISIBLE_COUNT + 1U;

using Payload = std::array<uint8_t, PAYLOAD_BYTES>;

bool Encode(const SettingsStore& settings, Payload& out);

// Resolves one stable visible index and normalized uint16 value into the
// descriptor's actual JSON value. The returned variant borrows storage from doc.
bool DecodeMutation(uint8_t visibleIndex, uint16_t normalizedValue,
                    const char*& outKey, JsonDocument& doc,
                    JsonVariantConst& outValue);

} // namespace modules::settings_state_wire
