#pragma once

#include <cstddef>
#include <cstdint>

#include <ArduinoJson.h>

#include "modules/control_mutation_service.hpp"
#include "modules/ui_types.hpp"

namespace modules {

namespace ble_control_wire {

constexpr uint8_t VERSION = 1U;
constexpr uint8_t ACTION_OPCODE = 25U;
constexpr size_t ACTION_BODY_BYTES = 5U;
constexpr uint8_t CONSOLE_OPCODE = 26U;
constexpr uint8_t CONSOLE_BEGIN = 1U;
constexpr uint8_t CONSOLE_END = 2U;
constexpr size_t CONSOLE_HEADER_BYTES = 2U;
constexpr size_t CONSOLE_BEGIN_HEADER_BYTES = 3U;

} // namespace ble_control_wire

struct ParsedBleControlActionRequest {
    ControlAction action = ControlAction::Unknown;
    UiOwner owner = UiOwner::None;
    CoachingMutation coachingMutation = CoachingMutation::Toggle;
    uint8_t undoMoveCount = 1U;
};

[[nodiscard]] ControlAction ParseBleControlActionToken(const char* token);
[[nodiscard]] const char* BleControlActionToString(ControlAction action);
[[nodiscard]] bool TryParseBleControlActionRequest(
    JsonVariantConst root,
    ParsedBleControlActionRequest& outRequest);
[[nodiscard]] bool TryDecodeConnectionControlAction(
    const uint8_t* body,
    size_t size,
    ParsedBleControlActionRequest& outRequest);
[[nodiscard]] ControlMutationReason
BleSettingReasonFromErrorToken(const char* errorToken);
[[nodiscard]] size_t FormatBleSettingCompactResponse(
    uint16_t requestId,
    bool ok,
    ControlMutationReason reason,
    char* outBuffer,
    size_t outCapacity);

} // namespace modules
